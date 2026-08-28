# Wymagania i macierz śledzenia implementacji

Stan analizy: **2026-08-24**. Dokument odwzorowuje zakres pracy na elementy
firmware’u, AWS IoT i aplikacji. Status „zaimplementowane” oznacza obecność
mechanizmu w kodzie; nie zastępuje kwalifikacji produkcyjnej na docelowym
sprzęcie.

## Klasy statusu

- **DONE** — kompletna ścieżka funkcjonalna istnieje w projekcie;
- **PARTIAL** — rdzeń istnieje, ale wynik zależy od wariantu sprzętu albo
  zewnętrznego procesu;
- **OPS** — wymaganie jest realizowane przez AWS, backend, CI lub produkcję, a
  nie wyłącznie przez firmware;
- **FUTURE** — świadomie poza minimalnym zakresem pracy.

## Wymagania funkcjonalne

| ID | Wymaganie | Status | Implementacja i dowód |
| --- | --- | --- | --- |
| FR-01 | Jednoznaczna identyfikacja urządzenia | DONE | UID STM32 → runtime Thing/client ID; `device_identity.*`; provisioning sprawdza 24-znakowy UID |
| FR-02 | Automatyczny first boot i rejestracja | DONE/OPS | Fleet Provisioning by claim, allowlista hooka, `CreateKeysAndCertificate`, `RegisterThing`; [AUTOMATIC_PROVISIONING.md](AUTOMATIC_PROVISIONING.md) |
| FR-03 | Bezpieczne połączenie z AWS | DONE | TLS 1.2, SNI/hostname, CA, mTLS, RNG i ważny czas; `tls_client.c`, `system_time.c` |
| FR-04 | Kanały statusu, telemetry, alarmów i diagnostyki | DONE | wersjonowany kontrakt topic v1; `mqtt_topics.*`; [DURABLE_OUTBOX_AND_TOPIC_CONTRACT.md](DURABLE_OUTBOX_AND_TOPIC_CONTRACT.md) |
| FR-05 | Telemetria cykliczna i zdarzeniowa | DONE/PARTIAL | scheduler, dynamiczna lista pomiarów, temperatura MCU i VDDA; prąd/wilgotność/sygnał wymagają sterowników wariantu; [SENSOR_REGISTRY.md](SENSOR_REGISTRY.md) |
| FR-06 | Alarmy, ostrzeżenia i zdarzenia krytyczne | DONE | `iot_events.*`, priorytety i trwały outbox |
| FR-07 | Ograniczone logi diagnostyczne | DONE | poziom logowania, limit payloadu, throttling, nieblokujący UART/logger |
| FR-08 | Sterowanie częstotliwością i kosztami | DONE | telemetry 5–3600 s, tryb interakcji max 30 min, backoff offline, kontrola diagnostyki |
| FR-09 | Odbiór i walidacja konfiguracji | DONE | named Shadow `config`, schema 1–3, zakresy, typy, UID i konflikt wersji; [DEVICE_SHADOW_CONFIGURATION.md](DEVICE_SHADOW_CONFIGURATION.md) |
| FR-10 | Trwała konfiguracja i restart bez utraty spójności | DONE | dwa sektory/generacje/CRC/commit-last w `device_config.c` |
| FR-11 | AWS IoT Jobs | DONE | discovery, notify-next, statusy, retry, cancellation; `aws_jobs.*`; [AWS_IOT_JOBS.md](AWS_IOT_JOBS.md) |
| FR-12 | Pobranie artefaktu do Flash | DONE | HTTPS, rozmiar, SHA-256, retry, resume od nowa i staging; `https_downloader.*`, `flash_staging.*` |
| FR-13 | Pełne bezpieczne OTA | DONE | OTA3, ECDSA P-256, dual-slot, bootloader, trial health, rollback i autoryzowany downgrade; [FULL_OTA.md](FULL_OTA.md) |
| FR-14 | Tryb pełnej interakcji | DONE | start/status/stop, timeout 30–1800 s, telemetry 5 s, diagnostics 10 s; [INTERACTION_MODE.md](INTERACTION_MODE.md) |
| FR-15 | Praca offline i reconnect | DONE | supervisor linku, DHCP/static fallback, MQTT reconnect, trwałe eventy i 16 próbek telemetry |
| FR-16 | Zdalne komendy i idempotencja | DONE | `commandId`, trwała historia 8 wyników, jawny `cmd/result`; [MQTT_COMMAND_REFERENCE.md](MQTT_COMMAND_REFERENCE.md) |
| FR-17 | Zarządzanie cyklem życia | DONE/OPS | factory reset, decommission, provisioning cancel, credential rotation; unieważnienie principal/Thing pozostaje operacją AWS |
| FR-18 | Capability discovery i rozszerzalne sensory | DONE/PARTIAL | rejestr 32 slotów, capability schema v2, 8 wpisów discovery; dowolny sensor nadal wymaga adaptera C i sterownika magistrali |

## Wymagania niefunkcjonalne

| ID | Wymaganie | Realizacja | Ograniczenie / kryterium odbioru |
| --- | --- | --- | --- |
| NFR-01 | Poufność i autentyczność transmisji | TLS 1.2 mTLS, weryfikacja CA/hostname | negatywny test złego CA/certyfikatu i braku czasu |
| NFR-02 | Ochrona kluczy | dual-slot credential store, zeroizacja buforów | RDP1/WRP lub secure element muszą być wdrożone per produkt |
| NFR-03 | Integralność aktualizacji | SHA-256 transportu + ECDSA podpisu + read-back | klucz podpisujący wyłącznie HSM/chronione CI w produkcji |
| NFR-04 | Dostępność | timeouty, retry/backoff, IWDG, reconnect, recovery OTA | kampania długotrwała i fault injection na sprzęcie |
| NFR-05 | Spójność trwałych danych | generacje, CRC, commit-last, dual-slot dla konfiguracji/credentials/OTA control | outbox jest jednosektorowy i nie ma atomowej kompakcji |
| NFR-06 | Ograniczone zasoby | statyczne bufory, brak malloc w krytycznych parserach, limity JSON | kontrola mapy linkera, heap minimum i stack high-water marks |
| NFR-07 | Skalowalność kontraktu | Thing-derived topics, environment i topic version | polityki AWS/backend/STM muszą zmieniać wersję w sposób skoordynowany |
| NFR-08 | Obserwowalność | status retained, alarmy, diagnostics, reset reason, Jobs status | retencja, dashboard i alerty należą do operacji backendu |
| NFR-09 | Kontrola kosztów | konfigurowalna częstotliwość, limity logów, bounded interaction | okresowy przegląd metryk billingowych; [OPERATIONAL_COST_MODEL.md](OPERATIONAL_COST_MODEL.md) |
| NFR-10 | Utrzymywalność | wersjonowane formaty i kontrakty, dokumentacja modułów | aktualizacja dokumentacji razem z markerem `TLS BUILD` |
| NFR-11 | Bezpieczeństwo destrukcyjnych komend | confirm, UID, świeży timestamp, audyt | polityka AWS least privilege i autoryzacja operatora w API |
| NFR-12 | Przenośność sensorów | rejestr i weak platform hooks | nie oznacza automatycznej obsługi dowolnego fizycznego modułu |

## Traceability do zadań pracy

| Zadanie z opisu | Dokument główny | Oczekiwany artefakt dowodowy |
| --- | --- | --- |
| zdalne zarządzanie i integracja AWS IoT | ten dokument, README, katalog komend | diagram architektury, topic map, log CONNECT/SUBACK |
| TLS, uwierzytelnianie i certyfikaty | provisioning, hardening | `verify_flags=0`, unikalny principal, negatywny test |
| first boot → provisioning → telemetry | provisioning | log stanów, Thing attributes i połączenie unikalnym certyfikatem |
| telemetry/alarmy/diagnostyka | outbox, sensory | payloady AWS/backend, event IDs, test offline |
| konfiguracja i trwały restart | Device Shadow | desired/reported, wersje, restart z tym samym snapshotem |
| Jobs i pobranie pliku | Jobs, staging | status `SUCCEEDED`, size i SHA-256 verified |
| pełna interakcja | interaction | status, zmieniona częstotliwość, expiry |
| analiza kosztów i optymalizacja | model kosztowy | arkusz/scenariusze dla N urządzeń |
| testy i zebranie wyników | plan testów | raport przebiegu, logi, zrzuty chmury i powtórzenia |
| architektura/interfejsy STM | runtime, katalog komend | mapa tasków, pamięci, API modułów i granice odpowiedzialności |

## Jawne elementy poza samym firmware

Pełne wdrożenie wymaga jeszcze konfiguracji least-privilege w AWS, allowlisty
urządzeń, sekretów backendu poza repozytorium, HSM/CI dla OTA, produkcyjnych
option bytes, monitoringu, backupu bazy, retencji i alertów kosztowych. Są to
kontrole **OPS**, nie brak handlera w firmware.
