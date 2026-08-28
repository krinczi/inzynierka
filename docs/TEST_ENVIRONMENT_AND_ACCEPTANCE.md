# Środowisko testowe i plan akceptacji

Wersja planu: **2026-08-24**. Plan obejmuje STM32H753, Ethernet, AWS IoT Core i
lokalną aplikację. Nie zawiera sekretów ani danych konkretnego konta.

## Elementy środowiska

### Urządzenie

- STM32H753ZI/Nucleo-H753ZI z debug probe i zasilaniem USB;
- Ethernet do sieci z DHCP albo zwalidowaną konfiguracją static;
- UART3 do rejestracji logów;
- firmware aplikacyjny oraz oddzielny bootloader z tym samym trust anchor OTA;
- aktualny marker `TLS BUILD` zgodny z [README.md](README.md).

### AWS

- IoT endpoint w tym samym regionie co polityki, Thing i Jobs;
- Fleet Provisioning template, claim policy i pre-provisioning hook;
- DynamoDB allowlista UID;
- unikalny certyfikat urządzenia z polityką ograniczoną do jego client/topiców;
- backend certificate z osobną polityką dla prefixu `iot/v1/...` i named Shadow;
- bucket S3 dla artefaktów oraz CloudFormation jako źródło konfiguracji;
- opcjonalne CloudWatch, Budgets i Cost Anomaly Detection.

### Aplikacja lokalna

Konfiguracja środowiska wskazuje wartości, a nie zawartość sekretów:

```text
AWS_IOT_REGION=<region>
AWS_IOT_ENDPOINT=<ats-endpoint>
AWS_IOT_CLIENT_ID=<unikalny-client-id-backendu>
AWS_IOT_TOPIC_PREFIX=iot/v1/dev/devices
AWS_IOT_CERT_PATH=<backend-certificate.pem.crt>
AWS_IOT_KEY_PATH=<ścieżka-do-prywatnego-klucza-backendu>
AWS_IOT_CA_PATH=<AmazonRootCA1.pem>
IOT_INGEST_URL=http://localhost:3000/api/iot-ingest/messages
```

Prywatnego klucza nie należy otwierać, drukować ani commitować. Test weryfikuje
jedynie istnienie pliku, uprawnienia procesu i poprawne zestawienie mTLS.

## Kolejność uruchomienia

1. Zbudować bootloader i aplikację w konfiguracji Release oraz zachować pliki
   `.map`, `.elf` i identyfikator commita.
2. Zweryfikować pakiet firmware narzędziem release przed uploadem.
3. Wgrać bootloader i aplikację zgodnie z mapą Flash.
4. Uruchomić bazę i zależności aplikacji, zsynchronizować schema Prisma i
   uruchomić API, worker oraz frontend.
5. Otworzyć UART przed resetem STM, aby nie utracić początku boot logu.
6. Podłączyć Ethernet i czekać na wiarygodny czas, TLS, MQTT CONNECT oraz pełny
   komplet SUBACK.
7. Dopiero po stabilnym statusie wykonywać testy destrukcyjne, Jobs i OTA.

## Kryteria podstawowego startu

Test jest pozytywny, gdy występują kolejno:

- poprawna przyczyna resetu i inicjalizacja lokalnych magazynów;
- poprawny adres DHCP albo zaakceptowane static;
- RTC/SNTP ma stan pozwalający na TLS;
- `verify_flags=0x00000000` i `HANDSHAKE OK`;
- MQTT `CONNACK` z kodem sukcesu;
- SUBACK bazowych topiców, named Shadow i AWS Jobs;
- status, capabilities, reported config i telemetry;
- brak HardFault, resetu IWDG i niekontrolowanej pętli reconnect.

## Macierz testów end-to-end

| ID | Scenariusz | Akcja | Wynik oczekiwany |
| --- | --- | --- | --- |
| T-01 | TLS/mTLS | prawidłowy certyfikat i czas | handshake oraz MQTT OK |
| T-02 | zły trust/certyfikat | build testowy lub unieważniony principal | brak sesji, brak telemetry, jawny błąd |
| T-03 | telemetry | obserwacja przez ≥3 okresy | rosnące `uptimeMs`, stabilne IDs/metadane, poprawne pomiary |
| T-04 | alarm/diagnostics | wywołanie kontrolowanego zdarzenia | osobne kanały, severity, limit payloadu |
| T-05 | Shadow apply | nowy valid `configVersion` | `PENDING → APPLIED`, zgodny reported |
| T-06 | Shadow invalid/conflict | zły zakres i ta sama wersja z innymi danymi | odrzucenie bez zmiany runtime/Flash |
| T-07 | trwałość konfiguracji | reset po `APPLIED` | ten sam snapshot i wersja |
| T-08 | reconnect | odłączyć i podłączyć Ethernet | link DOWN/UP, nowa sesja, brak restartu MCU |
| T-09 | offline outbox | wywołać eventy i telemetry bez sieci | bounded zapis, flush po reconnect, deduplikacja backendu |
| T-10 | interaction | start 30 s, status, stop/expiry | telemetry około 5 s i trwałe zdarzenie zakończenia |
| T-11 | service-file Job | mały plik i poprawny SHA-256 | HTTPS, verified size/hash, `SUCCEEDED` |
| T-12 | transfer failure | zły hash/URL lub zerwanie sieci | bounded retry, `FAILED`, brak fałszywego READY |
| T-13 | cancel Job | `jobs.cancel` dla aktywnego ID | transfer przerwany i końcowy status |
| T-14 | signed OTA | nowszy poprawny OTA3 | staging, swap, trial, health confirm |
| T-15 | zły podpis/target | zmodyfikowany package | fail closed, poprzedni obraz startuje |
| T-16 | trial failure | kontrolowany brak health confirm | automatyczny rollback |
| T-17 | manual rollback | poprzedni slot + uprzywilejowana komenda | jawny ACK, reset, poprzednia wersja |
| T-18 | authorized downgrade | podpisany starszy obraz + obie bramki | trial, confirm i nowy baseline |
| T-19 | command idempotency | ten sam ID przed/po resecie | jedno wykonanie, replay wyniku |
| T-20 | Fleet first boot | pusty credential store + UID na liście dozwolonych | unikalny certyfikat, RegisterThing, reconnect |
| T-21 | provisioning reject | UID spoza allowlisty | brak unikalnych poświadczeń i telemetrii produkcyjnej |
| T-22 | credential rotation | rotacja i stabilna sesja | nowy certyfikat aktywny, stary możliwy do unieważnienia |
| T-23 | factory reset/decommission | uprzywilejowana komenda | trwałe żądanie, reset/recovery, właściwy lifecycle |
| T-24 | watchdog | kontrolowane zatrzymanie heartbeat w buildzie testowym | reset IWDG i raport przy kolejnym starcie |
| T-25 | sensory | raw/range/stale/błąd adaptera | valid/quality/error bez sztucznej wartości |

## Testy przerwania zasilania

Power-cut wykonuje się na kontrolowanym stanowisku, nigdy przez przypadkowe
odpinanie debuggera bez rejestracji fazy. Macierz musi objąć:

1. zapis nowej konfiguracji do pustego i częściowo zapełnionego slotu;
2. zapis oraz rotację poświadczeń;
3. erase/program/finalize stagingu;
4. każdą fazę swapu OTA i OTA control journal;
5. trial przed i po health confirm;
6. append oraz kompakcję outboxu.

Po każdym przerwaniu należy zapisać fazę, numer próby, aktywny obraz, stan
journalu i rezultat recovery. Dla outboxu oczekuje się poprawnego odrzucenia
niepełnego rekordu, lecz aktualna jednosektorowa kompakcja nie gwarantuje
zachowania wszystkich wiadomości.

## Test długotrwały

Minimalny soak test powinien trwać 24 h, a przed wydaniem produkcyjnym 7–30 dni.
Zbierane metryki:

- liczba reconnectów, błędów DNS/TCP/TLS/MQTT i utraconych PUBACK;
- minimum FreeRTOS heap i high-water mark tasków;
- RX allocation failures, UART/logger drops i Flash write failures;
- resety według przyczyny, HardFault registers i watchdog stale mask;
- liczba wiadomości na kanał i rozmiar payloadu;
- zużycie outboxu i liczba kompakcji;
- liczba Jobs, pobrane bajty, czas transferu i retry.

## Pakiet dowodowy do pracy

Dla każdego testu przechowywać: ID testu, datę, commit firmware/backendu,
wersję obrazu, konfigurację, kroki, oczekiwany i rzeczywisty wynik, log UART,
potwierdzenie backend/AWS oraz ocenę PASS/FAIL. Sekrety i presigned URL muszą
zostać zanonimizowane. Zrzut ekranu bez identyfikatora testu nie jest sam w
sobie pełnym dowodem powtarzalności.
