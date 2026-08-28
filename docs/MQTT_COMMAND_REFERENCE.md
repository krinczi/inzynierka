# Katalog komend MQTT urządzenia

Stan kontraktu: **topic v1, wynik schema v2, zweryfikowano 2026-08-24**.

Dokument opisuje komendy obsługiwane przez aktualny firmware. Nie jest to
zdalna powłoka: urządzenie wykonuje wyłącznie jawnie skompilowane handlery,
waliduje pola oraz publikuje wynik aplikacyjny.

## Kanały i wspólna koperta

```text
iot/v1/{environment}/devices/{thingName}/cmd
iot/v1/{environment}/devices/{thingName}/cmd/result
```

Każda komenda musi zawierać:

```json
{
  "commandId": "unikalny-identyfikator-operacji",
  "type": "nazwa.komendy"
}
```

- `commandId` ma maksymalnie 95 znaków i identyfikuje logiczne wykonanie;
- `type` ma maksymalnie 47 znaków;
- nieznane typy i błędne typy pól są odrzucane;
- przy QoS 1 STM potwierdza odbiór pakietu MQTT przed uruchomieniem handlera;
- transportowy PUBACK nie oznacza wykonania — potwierdzeniem jest
  `cmd/result`;
- osiem ostatnich wyników jest przechowywanych w trwałym dzienniku. Ponowienie
  tego samego `commandId` odtwarza wynik bez ponownego wykonania operacji;
- cache jest skończony, dlatego backend również musi utrzymywać idempotencję i
  nie może bezterminowo ponawiać starego identyfikatora.

Wynik ma postać:

```json
{
  "deviceId": "stm32h753zi-dev-01",
  "schemaVersion": 2,
  "commandId": "unikalny-identyfikator-operacji",
  "status": "ACKED",
  "message": "opis wyniku",
  "uptimeMs": 123456,
  "executedAt": 1787587200
}
```

`executedAt` występuje tylko przy wiarygodnym czasie RTC/SNTP. Aktualna
publikacja `cmd/result` używa QoS 0, natomiast zapis wyniku przed publikacją
chroni przed ponownym wykonaniem. Gdy odpowiedź zaginie, backend powinien
ponowić **ten sam** `commandId`, aby STM odtworzył zapamiętany rezultat.

## Zwykłe komendy operacyjne

### Sterowanie diodą

```json
{
  "commandId": "led-001",
  "type": "set_led",
  "value": true
}
```

`value` musi być wartością boolean. Komenda steruje wyłącznie `LD2` na płytce
referencyjnej i stanowi prosty test ścieżki cloud-to-device.

### Tryb pełnej interakcji

```json
{
  "commandId": "interaction-001",
  "type": "interaction.start",
  "durationSeconds": 300
}
```

`durationSeconds` jest opcjonalne; zakres wynosi 30–1800 s, a domyślna wartość
to 300 s.

```json
{ "commandId": "interaction-002", "type": "interaction.status" }
```

```json
{ "commandId": "interaction-003", "type": "interaction.stop" }
```

Szczegóły częstotliwości i wygaśnięcia opisuje
[INTERACTION_MODE.md](INTERACTION_MODE.md).

### Anulowanie aktywnego AWS Job

```json
{
  "commandId": "job-cancel-001",
  "type": "jobs.cancel",
  "jobId": "stm32-download-20260824153000"
}
```

Handler anuluje tylko pasującą, aktualnie wykonywaną operację urządzenia,
przerywa downloader HTTPS i publikuje końcowy status Job. Brak aktywnego Job o
podanym ID jest wynikiem idempotentnym `ACKED`, a nie błędem transportu.

## Komendy poświadczeń i provisioning

### Rotacja unikalnego certyfikatu

```json
{
  "commandId": "credentials-rotate-001",
  "type": "credentials.rotate",
  "confirm": true,
  "hardwareUid": "001F00423438511136353232"
}
```

Komenda wymaga stanu stabilnego `PROVISIONED`. UID musi odpowiadać UID
odczytanemu z MCU. Poświadczenia są tworzone przez Fleet Provisioning i
zapisywane w magazynie generacyjnym; stare pozostają awaryjnym slotem do czasu
potwierdzenia stabilnej sesji nowym certyfikatem.

### Półautomatyczne zatwierdzenie aktywacji

```json
{
  "commandId": "provisioning-apply-001",
  "type": "provisioning.apply",
  "activationId": "activation-id-from-backend",
  "hardwareUid": "001F00423438511136353232"
}
```

Ta komenda zachowuje zgodność z przepływem półautomatycznym. Błędny UID
zwiększa licznik prób; po pięciu błędach żądania są blokowane, a pomiędzy
próbami działa wykładniczy backoff ograniczony do 60 s. Produkcyjny first boot
korzysta z automatycznego Fleet Provisioning opisanego w
[AUTOMATIC_PROVISIONING.md](AUTOMATIC_PROVISIONING.md).

### Awaryjne unieważnienie lokalnego provisioning

```json
{
  "commandId": "provisioning-cancel-001",
  "type": "provisioning.cancel",
  "activationId": "activation-id-from-backend",
  "hardwareUid": "001F00423438511136353232",
  "confirm": true,
  "requestedAt": 1787587200
}
```

`requestedAt` jest czasem Unix i musi mieścić się w oknie 0–300 s względem
wiarygodnego czasu urządzenia. Po utrwaleniu żądania STM odsyła wynik, planuje
restart i usuwa lokalne poświadczenia w ścieżce recovery. Operacja lokalna nie
zastępuje unieważnienia certyfikatu i rekordu urządzenia po stronie AWS.

## Uprzywilejowane komendy cyklu życia

Komendy w tej sekcji wymagają jednocześnie:

- `confirm: true`;
- dokładnego `hardwareUid`;
- `requestedAt` nie starszego niż 300 s i niepochodzącego z przyszłości;
- poprawnego, wiarygodnego czasu systemowego.

### Factory reset

```json
{
  "commandId": "factory-reset-001",
  "type": "lifecycle.factory_reset",
  "hardwareUid": "001F00423438511136353232",
  "confirm": true,
  "requestedAt": 1787587200
}
```

Żądanie jest najpierw utrwalane. Po odpowiedzi urządzenie planuje reset i przy
następnym starcie czyści konfigurację oraz poświadczenia, wracając do
kontrolowanego first boot.

### Decommission

```json
{
  "commandId": "decommission-001",
  "type": "lifecycle.decommission",
  "hardwareUid": "001F00423438511136353232",
  "confirm": true,
  "requestedAt": 1787587200
}
```

Decommission wymaga stabilnych unikalnych poświadczeń. Po recovery lokalny stan
`DECOMMISSIONED` blokuje ponowne wejście do chmury. Backend musi osobno
unieważnić certyfikat, odłączyć principal od Thing oraz zamknąć rekord floty.

### Natychmiastowy rollback do poprzedniego slotu

```json
{
  "commandId": "firmware-rollback-001",
  "type": "firmware.rollback",
  "hardwareUid": "001F00423438511136353232",
  "confirm": true,
  "requestedAt": 1787587200
}
```

Wymaga `allowRollback=true` w konfiguracji i uwierzytelnionego poprzedniego
obrazu już obecnego w drugim slocie. Nie pobiera dowolnej starszej wersji. Taki
downgrade realizuje podpisany, jawnie autoryzowany AWS Job opisany w
[FULL_OTA.md](FULL_OTA.md).

## Semantyka błędów i retry

- `ACKED` oznacza przyjęcie i wykonanie albo bezpieczne zaplanowanie operacji;
- `FAILED` oznacza odrzucenie na poziomie aplikacyjnym;
- timeout HTTP/API bez `cmd/result` nie dowodzi, że urządzenie nie wykonało
  operacji;
- retry musi zachować `commandId`; nowy identyfikator oznacza nową operację;
- backend nie powinien wysyłać kolejnej operacji destrukcyjnej, dopóki nie zna
  wyniku poprzedniej;
- przy operacjach prowadzących do restartu ACK jest publikowany przed
  opóźnionym resetem (około 2 s), ale potwierdzenie należy również korelować z
  późniejszym statusem urządzenia.

## Minimalna macierz testowa

1. Dla każdej komendy sprawdzić poprawny payload, brak wymaganego pola i błędny
   typ pola.
2. Powtórzyć ten sam `commandId` przed i po restarcie; efekt ma wystąpić raz.
3. Sprawdzić starszy oraz przyszły `requestedAt` dla komend uprzywilejowanych.
4. Sprawdzić niezgodny UID i brak `confirm`.
5. Zerwać połączenie po odbiorze komendy, ale przed odebraniem wyniku przez
   backend; retry z tym samym ID ma odtworzyć rezultat.
6. Zweryfikować, że status urządzenia po factory reset, decommission i rollback
   odpowiada utrwalonemu wynikowi komendy.

