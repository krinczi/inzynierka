# Dokumentacja firmware STM32H753 IoT Manager

Stan zgodności z kodem: **2026-08-24**.

Ten katalog opisuje aktualny firmware aplikacyjny, bootloader, kontrakty AWS IoT
oraz granicę pomiędzy mechanizmami zaimplementowanymi a czynnościami
produkcyjnymi wykonywanymi poza firmware. W razie rozbieżności pierwszeństwo
mają stałe i struktury w kodzie źródłowym; rozbieżność należy następnie usunąć z
dokumentacji.

## Kanoniczny stan implementacji

| Obszar | Stan |
| --- | --- |
| MCU | STM32H753ZI, 2 MiB Flash, dual-bank |
| RTOS | FreeRTOS Kernel 11.3.0 |
| TLS | Mbed TLS 3.6.7 LTS, TLS 1.2, wzajemne uwierzytelnianie X.509 |
| Sieć | Ethernet/LwIP, domyślnie DHCP, opcjonalna konfiguracja statyczna i bezpieczny fallback DHCP |
| Czas | RTC LSE/LSI, SNTP, blokada TLS bez wiarygodnego czasu |
| MQTT | QoS 1 dla danych wymagających potwierdzenia, retained Last Will/status |
| Topic contract | `iot/v1/{environment}/devices/{thingName}/{channel}` |
| Device Shadow | named shadow `config`, konfiguracja schema v3 |
| Provisioning | AWS IoT Fleet Provisioning by claim, unikalny certyfikat urządzenia, rotacja i rollback poświadczeń |
| AWS Jobs | walidacja, pobieranie HTTPS, anulowanie, trwałe statusy i podpisane OTA |
| OTA | podpis ECDSA P-256, SHA-256, dual-slot, trial boot, automatic rollback i autoryzowany downgrade |
| Outbox | zdarzenia trwałe, 16 próbek offline, 8 wyników komend; jeden sektor Flash |
| Telemetria sprzętowa | temperatura MCU i VDDA; pozostałe metryki wymagają sterownika platformowego |
| Recovery | IWDG, HardFault diagnostics, wznowienie swapu OTA, dual-slot konfiguracji i poświadczeń |

Marker kompilacji raportowany przez aktualny firmware:

```text
fleet_auto_v3 credential_rotation_v2 lifecycle_v1 shadow_v3
network_config_v1 sensor_registry_v1 rtc_sntp_v1 iwdg_mpu_v1
durable_outbox_v3 topic_contract_v1 full_ota_v1
```

## Mapa dokumentów

| Dokument | Zakres |
| --- | --- |
| [AUTOMATIC_PROVISIONING.md](AUTOMATIC_PROVISIONING.md) | first boot, Fleet Provisioning, rotacja i awaryjne anulowanie aktywacji |
| [AWS_IOT_JOBS.md](AWS_IOT_JOBS.md) | protokół Jobs, dokumenty, retry, anulowanie i statusy |
| [FLASH_STAGING.md](FLASH_STAGING.md) | zapis pobranego artefaktu do Flash i jego integralność |
| [FULL_OTA.md](FULL_OTA.md) | format OTA3, bootloader, instalacja, trial, rollback i downgrade |
| [DURABLE_OUTBOX_AND_TOPIC_CONTRACT.md](DURABLE_OUTBOX_AND_TOPIC_CONTRACT.md) | dane offline, idempotencja i wersjonowanie tematów |
| [SENSOR_REGISTRY.md](SENSOR_REGISTRY.md) | rejestr sensorów i dodawanie sterowników |
| [DEVICE_SHADOW_CONFIGURATION.md](DEVICE_SHADOW_CONFIGURATION.md) | kontrakt konfiguracji, wersjonowanie, historia i zapis dual-slot |
| [MQTT_COMMAND_REFERENCE.md](MQTT_COMMAND_REFERENCE.md) | wszystkie komendy, wymagane pola, idempotencja i odpowiedzi |
| [INTERACTION_MODE.md](INTERACTION_MODE.md) | ograniczony czasowo tryb serwisowy i jego koszty |
| [RUNTIME_ARCHITECTURE.md](RUNTIME_ARCHITECTURE.md) | startup, taski, watchdog, pamięć, logger i reconnect |
| [REQUIREMENTS_TRACEABILITY.md](REQUIREMENTS_TRACEABILITY.md) | wymagania funkcjonalne/niefunkcjonalne i dowody realizacji |
| [TEST_ENVIRONMENT_AND_ACCEPTANCE.md](TEST_ENVIRONMENT_AND_ACCEPTANCE.md) | środowisko, testy E2E, power-cut, soak i pakiet dowodowy |
| [OPERATIONAL_COST_MODEL.md](OPERATIONAL_COST_MODEL.md) | model wiadomości, Jobs/S3/logów i optymalizacje kosztowe |
| [MIDDLEWARE_MIGRATION.md](MIDDLEWARE_MIGRATION.md) | wersje oraz pochodzenie Mbed TLS i FreeRTOS |
| [PRODUCTION_HARDENING.md](PRODUCTION_HARDENING.md) | pełna ocena utwardzenia oraz jawne ograniczenia |
| [PRODUCTION_OPTION_BYTES.md](PRODUCTION_OPTION_BYTES.md) | profil RDP/WRP/PCROP i bramka produkcyjna |
| [RELEASE_SIGNING.md](RELEASE_SIGNING.md) | podpisywanie wydania poza urządzeniem i repozytorium |

## Wersje kontraktów

Wersje poniżej dotyczą niezależnych kontraktów i nie mogą być traktowane jako
jedna wspólna liczba:

- topic contract: `v1`;
- AWS Job document: `schemaVersion: 1`;
- konfiguracja Device Shadow: `schemaVersion: 3`, minimalnie obsługiwana `1`;
- telemetry/capabilities: dynamiczne pomiary schema v2;
- status, alarm i diagnostyka: schema v1;
- credential storage: format 2;
- device configuration storage: format 3;
- durable outbox journal: format 2;
- OTA header: `OTA3`, header format 1;
- OTA control journal: format 2.

## Granica określenia „produkcyjny”

Kod zawiera kompletną ścieżkę funkcjonalną wymaganą w projekcie, lecz sam build
nie jest wystarczającym dowodem gotowości produktu. Przed produkcją pozostają
czynności zależne od organizacji i sprzętu:

1. zaprogramowanie i odczytowa weryfikacja RDP1/WRP zgodnie z profilem płytki;
2. przechowywanie klucza podpisującego w HSM lub chronionym CI;
3. procedura rotacji i awaryjnego unieważnienia claim certificate;
4. kwalifikacja power-cut podczas każdej fazy kasowania i programowania Flash;
5. testy długotrwałe, monitoring AWS, retencja danych, backup i alerty kosztowe;
6. decyzja, czy prywatny klucz urządzenia wymaga secure elementu;
7. sterowniki oraz kalibracja rzeczywistych sensorów wariantu sprzętowego.

Znane ograniczenie techniczne: outbox operacyjny jest dziennikiem w jednym
sektorze. Pojedyncze rekordy są odporne na niepełny zapis, ale kasowanie sektora
podczas kompakcji nie jest transakcją dual-slot. Produkt wymagający zachowania
każdej próbki przy dowolnym power-cut powinien użyć dwóch sektorów z generacją
i znacznikiem commit albo zewnętrznego FRAM.

## Zasady aktualizacji dokumentacji

Przy zmianie firmware należy jednocześnie zaktualizować:

- marker `TLS BUILD` i tabelę stanu powyżej;
- mapę Flash w dokumentach provisioning, staging i OTA;
- wersje kontraktów oraz przykładowe payloady;
- limity buforów, timeouty i liczbę prób;
- sekcję ograniczeń, jeśli test potwierdza mechanizm tylko częściowo.

Sekretów, prywatnych kluczy, pełnych certyfikatów i presigned URL nie wolno
umieszczać w dokumentacji, logach ani repozytorium.
