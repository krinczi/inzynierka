# Konfiguracja urządzenia przez AWS IoT Device Shadow

Stan implementacji: **named shadow `config`, schema v3, dual-slot Flash,
zweryfikowano 2026-08-24**.

## Odpowiedzialność warstw

- Aplikacja przechowuje stan `desired`, `reported`, historię wersji i autora
  zmiany.
- AWS IoT Device Shadow zapewnia trwały kanał synchronizacji dla urządzenia,
  również gdy STM jest chwilowo offline.
- STM waliduje każdą wartość niezależnie i zapisuje konfigurację dopiero przed
  raportem `APPLIED`.
- Device Shadow nie przechowuje certyfikatów, kluczy prywatnych ani binarnego
  firmware.

Konfiguracja używa tematów zarezerwowanych:

```text
$aws/things/{thingName}/shadow/name/config/get
$aws/things/{thingName}/shadow/name/config/get/accepted
$aws/things/{thingName}/shadow/name/config/get/rejected
$aws/things/{thingName}/shadow/name/config/update
$aws/things/{thingName}/shadow/name/config/update/delta
$aws/things/{thingName}/shadow/name/config/update/accepted
$aws/things/{thingName}/shadow/name/config/update/rejected
```

## Dokument desired schema v3

API publikuje pod `state.desired` dokument logiczny:

```json
{
  "deviceId": "stm32h753zi-dev-01",
  "schemaVersion": 3,
  "configVersion": 8,
  "configuration": {
    "telemetry": {
      "frequencySeconds": 60,
      "enabledMetrics": ["temperature", "voltage"]
    },
    "diagnostics": {
      "logLevel": "INFO",
      "maxMessageBytes": 512
    },
    "network": {
      "mqttKeepAliveSeconds": 60,
      "addressingMode": "DHCP",
      "dhcpFallbackEnabled": true,
      "ipv4Address": "0.0.0.0",
      "subnetMask": "0.0.0.0",
      "gateway": "0.0.0.0",
      "dnsPrimary": "0.0.0.0",
      "dnsSecondary": "0.0.0.0"
    },
    "firmware": {
      "allowRollback": false
    }
  }
}
```

AWS opakowuje ten obiekt w dokument Shadow. Handler STM wyszukuje pola w
otrzymanym `desired`/`delta`; nie ufa nazwie topicu jako dowodowi poprawności
payloadu.

## Walidacja

| Pole | Ograniczenie |
| --- | --- |
| `schemaVersion` | 1–3; aktualny zapis i raport używa 3 |
| `configVersion` | liczba rosnąca; niższa jest `STALE`, ta sama z innymi danymi jest `CONFLICT` |
| `deviceId` | musi być równy aktywnej nazwie Thing |
| `frequencySeconds` | 5–3600 s |
| `enabledMetrics` | 1–32 unikalne ID; schema v3: `[a-z][a-z0-9_.-]{0,30}` |
| `logLevel` | `ERROR`, `WARN`, `INFO`, `DEBUG`, `TRACE` |
| `maxMessageBytes` | 64–4096 B |
| `mqttKeepAliveSeconds` | 15–900 s |
| `addressingMode` | `DHCP` albo `STATIC` |
| IPv4 static | kanoniczny host unicast, maska ciągła, gateway w tej samej podsieci, niezerowy DNS primary |
| `allowRollback` | boolean; tylko dodatkowa lokalna bramka, nie samodzielna autoryzacja downgrade |

Schema 1 jest czytana ze zgodnymi domyślnymi wartościami DHCP. Schema 2 dodaje
sieć. Schema 3 dopuszcza stabilne dynamiczne ID rejestru sensorów. Raport STM
zawsze używa aktualnej schema v3.

## Wersjonowanie i konflikt

API wymaga `baseVersion` równego najnowszej wersji znanej z desired, request i
reported. Jeżeli inny operator zapisał konfigurację wcześniej, API zwraca
konflikt i wymusza ponowne pobranie. Następnie tworzy `configVersion + 1`.

STM stosuje drugą, niezależną ochronę:

- niższa wersja: `REJECTED_STALE`;
- ta sama wersja i te same dane: `UNCHANGED`, bez kolejnego zapisu Flash;
- ta sama wersja i inne dane: `REJECTED_CONFLICT`;
- wyższa poprawna wersja: zapis i `APPLIED`.

Historia w aplikacji pozwala przywrócić dowolny wcześniejszy zestaw wartości,
ale przywrócenie tworzy **nową**, wyższą wersję. STM nie obniża licznika
`configVersion`.

## Trwały zapis

Konfiguracja zajmuje Bank 2 sektory 6 i 7:

- slot A: `0x081C0000-0x081DFFFF`;
- slot B: `0x081E0000-0x081FFFFF`.

Rekord ma generację, format storage 3, dane, CRC i commit-last. Rekordy są
dopisywane do sektora; po zapełnieniu zapis przechodzi do drugiego sektora, a
ostatnia poprawna generacja pozostaje dostępna podczas przełączenia. Po restarcie
wygrywa najnowszy kompletny rekord. Factory reset przywraca bezpieczne wartości
domyślne i nie zachowuje starego desired lokalnie.

## Zmiana sieci

Po poprawnym zapisie STM najpierw publikuje reported. Dopiero potem przerywa
bieżącą sesję i rekonfiguruje interfejs. Domyślny tryb to DHCP. Dla statycznego
IPv4 opcjonalny fallback DHCP uruchamia się po powtarzających się błędach bramki
cloud-health; desired pozostaje statyczny, aby operator widział przyczynę i mógł
ją poprawić.

## Odpowiedź reported

STM raportuje `deviceId`, `schemaVersion`, `requestVersion`, aktywny
`configVersion`, `status`, komunikat, uptime oraz pełny snapshot konfiguracji.
`APPLIED` jest ważne tylko wtedy, gdy `requestVersion == configVersion`.
Backend nie powinien uznawać samego `update/accepted` AWS za potwierdzenie
zastosowania — jest nim dopiero reported urządzenia.

## Testy akceptacyjne

1. Wysłać zmianę z prawidłowym `baseVersion` i potwierdzić `PENDING → APPLIED`.
2. Wysłać dwa zapisy z tym samym `baseVersion`; drugi musi otrzymać konflikt API.
3. Wysłać starszy `configVersion` bezpośrednio przez Shadow; STM ma raportować
   odrzucenie bez zapisu Flash.
4. Wysłać niepoprawny zakres i błędną sieć statyczną; desired nie może zmienić
   aktywnego runtime.
5. Zrestartować urządzenie i porównać reported z ostatnią zastosowaną wersją.
6. Przywrócić wersję z historii i potwierdzić nowe, wyższe `configVersion` oraz
   `restoredFromVersion` w audycie aplikacji.
7. Zmienić DHCP na static, przerwać łączność i potwierdzić kontrolowany fallback
   bez utraty konfiguracji.

