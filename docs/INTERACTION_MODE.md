# Tryb pełnej interakcji

Stan implementacji: **aktywny runtime, komendy MQTT i timeout,
zweryfikowano 2026-08-24**.

## Cel

Tryb pełnej interakcji jest ograniczonym czasowo trybem serwisowym. Zwiększa
częstotliwość telemetrii i diagnostyki podczas aktywnej pracy operatora, ale nie
zmienia trwałej konfiguracji, nie omija autoryzacji komend i nie wyłącza
watchdoga.

## Komendy

Komendy trafiają na `iot/v1/{environment}/devices/{thingName}/cmd` i wymagają
unikalnego `commandId`.

Start:

```json
{
  "commandId": "interaction-20260824-001",
  "type": "interaction.start",
  "durationSeconds": 300
}
```

- domyślny czas: 300 s;
- minimum: 30 s;
- maksimum: 1800 s;
- ponowny poprawny start zastępuje deadline nowym czasem.

Stop i odczyt stanu:

```json
{ "commandId": "interaction-stop-001", "type": "interaction.stop" }
```

```json
{ "commandId": "interaction-status-001", "type": "interaction.status" }
```

Wynik jest publikowany na `cmd/result`. Powtórzony `commandId` korzysta z
trwałej historii ośmiu ostatnich wyników i nie wykonuje ponownie handlera.

## Zachowanie runtime

- telemetria jest wysyłana nie rzadziej niż co 5 s, jeżeli trwa sesja MQTT;
- `MODE_ACTIVE` jest generowane co 10 s jako niedurable INFO;
- `MODE_STARTED`, `MODE_STOPPED` i `MODE_EXPIRED` są trwałymi zdarzeniami
  serwisowymi;
- status urządzenia zawiera `interaction.active` oraz pozostały czas;
- telemetria zawiera `interactionMode=true`;
- keep-alive, Jobs, Shadow, alarmy, offline outbox i watchdog nadal działają.

Stan interakcji jest celowo tylko w RAM. Restart, factory reset, utrata zasilania
lub uruchomienie nowego firmware kończy tryb. Po utracie samego Ethernetu timer
dalej biegnie według uptime; trwałe zdarzenie zakończenia zostanie wysłane po
powrocie łączności.

## Granice bezpieczeństwa i kosztu

Tryb nie jest powłoką zdalną i nie pozwala uruchamiać dowolnego kodu. Obsługuje
wyłącznie skompilowane komendy. Limit 30 minut zapobiega pozostawieniu urządzenia
w kosztownym trybie na stałe. Telemetria co 5 s zwiększa liczbę wiadomości około
12 razy względem konfiguracji 60 s, dlatego aplikacja powinna pokazywać aktywny
timer i wymagać jawnej akcji operatora.

Zdarzenia `MODE_ACTIVE` nie są zapisywane w Flash, aby sesja serwisowa nie
zużywała outboxu. Start/stop/expiry są trwałe dla audytu, ale podlegają ogólnemu
ograniczeniu pojedynczego sektora opisanemu w
`DURABLE_OUTBOX_AND_TOPIC_CONTRACT.md`.

## Test akceptacyjny

1. Uruchomić tryb na 30 s i potwierdzić ACK, status aktywny oraz telemetrię co
   około 5 s.
2. Odczytać status bez zmiany deadline.
3. Poczekać na automatyczne wygaśnięcie i potwierdzić `MODE_EXPIRED`.
4. Uruchomić ponownie, zatrzymać komendą i potwierdzić `MODE_STOPPED`.
5. Powtórzyć ten sam `commandId`; efekt nie może zostać wykonany drugi raz.
6. Odłączyć Ethernet w trakcie, podłączyć po deadline i sprawdzić trwałe
   zdarzenie zakończenia.
7. Zrestartować STM podczas trybu; po starcie `interaction.active` musi być
   `false`.

