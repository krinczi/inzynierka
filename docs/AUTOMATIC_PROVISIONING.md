# Automatyczny provisioning AWS IoT dla STM32H753

Stan implementacji: **Fleet Provisioning, rotacja poświadczeń i awaryjne
anulowanie aktywacji aktywne; zweryfikowano 2026-08-24**.

## Cel i model

Firmware realizuje **AWS IoT Fleet Provisioning by claim**. Urządzenie nie wymaga
wpisywania tokenu w aplikacji ani ręcznego dostarczania docelowego certyfikatu.
Wspólne poświadczenie bootstrap służy wyłącznie do uzyskania unikalnej tożsamości.
Po zakończeniu procesu normalna komunikacja używa certyfikatu przypisanego do
konkretnego `Thing`.

Szablon po stronie AWS ma nazwę `stm32h753-fleet-template`. Konfiguracja chmury,
polityki least-privilege, allowlista urządzeń i pre-provisioning hook znajdują się
w repozytorium aplikacji w `infra/aws/fleet-provisioning/`.

## Flow first boot

1. `CredentialStore_Init()` wybiera najnowszy poprawny rekord z dwóch slotów.
2. Urządzenie pozyskuje wiarygodny czas z RTC/SNTP; pierwsze TLS jest blokowane,
   jeżeli czas jest nieważny.
3. Dla stanu `EMPTY` klient TLS używa wbudowanego certyfikatu bootstrap/claim.
4. Po połączeniu MQTT firmware subskrybuje odpowiedzi usług provisioningowych.
5. Publikuje pusty dokument do `$aws/certificates/create/json`.
6. Z odpowiedzi `accepted` pobiera certyfikat, klucz prywatny, identyfikator oraz
   `certificateOwnershipToken`.
7. Mbed TLS sprawdza format certyfikatu i zgodność pary certyfikat–klucz.
8. Dane są transakcyjnie zapisywane jako `PENDING_REGISTRATION`.
9. Firmware publikuje `RegisterThing` z `ThingName`, 96-bitowym UID sprzętowym i
   numerem seryjnym do szablonu Fleet Provisioning.
10. Pre-provisioning hook AWS sprawdza UID, nazwę Thing i allowlistę.
11. Po odpowiedzi `accepted` rekord zostaje transakcyjnie zatwierdzony jako
    `PROVISIONED`, a urządzenie rozłącza sesję bootstrap.
12. Kolejne połączenie TLS używa wyłącznie unikalnego certyfikatu i uruchamia
    standardowe tematy telemetrii, konfiguracji, komend i AWS Jobs.

## Tematy MQTT

| Kierunek | Temat |
| --- | --- |
| STM → AWS | `$aws/certificates/create/json` |
| AWS → STM | `$aws/certificates/create/json/accepted` |
| AWS → STM | `$aws/certificates/create/json/rejected` |
| STM → AWS | `$aws/provisioning-templates/stm32h753-fleet-template/provision/json` |
| AWS → STM | `$aws/provisioning-templates/stm32h753-fleet-template/provision/json/accepted` |
| AWS → STM | `$aws/provisioning-templates/stm32h753-fleet-template/provision/json/rejected` |

Odpowiedzi są subskrybowane przed wysłaniem żądania. Po zakończeniu provisioning
firmware wraca do wersjonowanych tematów `iot/v1/dev/devices/{deviceId}/...` i AWS Jobs.

Claim certificate nie jest certyfikatem urządzenia. Jest wspólną, ograniczoną
tożsamością bootstrap i nie może mieć dostępu do telemetrii, komend, Jobs ani
Device Shadow. Unikalny certyfikat utworzony przez AWS otrzymuje osobną politykę
urządzenia.

## Maszyna stanów i odporność

Stan trwały magazynu poświadczeń:

- `EMPTY` — brak docelowej tożsamości; wymagany bootstrap,
- `PENDING_REGISTRATION` — unikalny certyfikat jest zapisany, ale AWS nie
  potwierdził jeszcze rejestracji,
- `PROVISIONED` — certyfikat jest zatwierdzony i może być użyty do normalnej sesji,
- `ROTATION_REQUESTED` — zachowano działający certyfikat i zaplanowano reconnect
  z poświadczeniem claim,
- `ROTATION_PENDING` — nowa para jest zapisana i oczekuje na `RegisterThing`,
- `ROTATION_VALIDATING` — AWS zatwierdził rejestrację, ale nowa para musi jeszcze
  utrzymać poprawną sesję MQTT przez 30 sekund.

Po utracie zasilania w stanie `PENDING_REGISTRATION` firmware ponawia wyłącznie
`RegisterThing`; nie tworzy bez potrzeby kolejnego certyfikatu. Jeżeli AWS zgłosi
wygaśnięcie ownership tokenu, rekord pending jest kasowany i proces zaczyna się od
nowego certyfikatu.

Każde żądanie ma timeout 30 s. Obowiązuje maksymalnie pięć prób na cykl pracy z
backoff 4, 8, 16 i 32 s. Licznik nie zeruje się przy reconnect MQTT, dzięki czemu
chwilowe błędy nie tworzą nieograniczonej pętli wywołań AWS.

Odpowiedź `rejected` jest walidowana jako ograniczony JSON. Firmware nie zapisuje
klucza, jeżeli brakuje ownership tokenu, certyfikat nie pasuje do klucza, nazwa
Thing jest niewłaściwa albo odpowiedź przekracza bufor. Bufory tymczasowe klucza
i ownership tokenu są zerowane po użyciu.

## Pamięć Flash

| Obszar | Adres | Rozmiar | Przeznaczenie |
| --- | ---: | ---: | --- |
| Bootloader | `0x08000000` | 128 KiB | weryfikacja, instalacja i rollback OTA |
| Primary firmware slot | `0x08020000` | 512 KiB | aktywny podpisany obraz |
| Durable outbox | `0x080A0000` | 128 KiB | alarmy, diagnostyka, telemetria offline i historia komend |
| Credential slot A | `0x080C0000` | 128 KiB | aktywny lub zapasowy rekord |
| Credential slot B | `0x080E0000` | 128 KiB | aktywny lub zapasowy rekord |
| OTA control journal | `0x08100000` | 128 KiB | stan instalacji i anti-rollback |
| Secondary firmware slot | `0x08120000` | 512 KiB | kandydat lub poprzedni obraz |
| OTA scratch | `0x081A0000` | 128 KiB | bezpieczna wymiana sektorów |
| Device config | `0x081C0000` | 256 KiB | dual-slot konfiguracji |

Rekord poświadczeń ma stałe 8 KiB i zawiera nagłówek, numer generacji, stan,
długości pól oraz CRC32 całego rekordu. Nowe rekordy są dopisywane do aktywnego
sektora. Dopiero po jego zapełnieniu drugi sektor jest kasowany i przejmuje rolę
aktywnego, podczas gdy ostatnia zweryfikowana generacja nadal istnieje w starym
sektorze. Format programowania jest zgodny z 32-bajtowym Flash word STM32H753.

Linker ogranicza obraz aplikacji do `512 KiB - 1024 B` i zawiera asercje
wykrywające nakładanie się bootloadera, slotów OTA, dziennika zdarzeń,
credential store i konfiguracji. Szczegóły znajdują się w `docs/FULL_OTA.md`.

„Atomowy” w tym dokumencie oznacza wybór kompletnego rekordu z prawidłowym CRC i
znacznikiem commit. Przy rotacji sektorów poprzednia zweryfikowana generacja
pozostaje w drugim sektorze do czasu zapisania nowej. Nie oznacza to szyfrowania
klucza prywatnego przed fizycznym odczytem.

## Interfejsy firmware’u

`credential_store.h`:

- `CredentialStore_Init()` — odczyt i wybór najnowszego poprawnego slotu,
- `CredentialStore_SavePending()` — atomowy zapis odpowiedzi AWS,
- `CredentialStore_CommitProvisioned()` — finalne zatwierdzenie po RegisterThing,
- `CredentialStore_RequestRotation()` — trwałe rozpoczęcie rotacji,
- `CredentialStore_CommitRotationValidated()` — promocja nowej pary po health gate,
- `CredentialStore_RecordRotationFailure()` — trwały licznik nieudanych walidacji,
- `CredentialStore_RollbackRotation()` — powrót do ostatniego rekordu `PROVISIONED`,
- `CredentialStore_Get()` / `CredentialStore_GetState()` — wybór danych dla TLS,
- `CredentialStore_Erase()` — jawna operacja fabryczna/serwisowa.

`fleet_provisioning.h`:

- `FleetProvisioning_Init()` — inicjalizacja kontraktu i tożsamości,
- `FleetProvisioning_OnMqttConnected()` — start lub wznowienie procesu,
- `FleetProvisioning_HandleMessage()` — obsługa odpowiedzi reserved topics,
- `FleetProvisioning_Process()` — timeouty, backoff i retry,
- `FleetProvisioning_ReconnectRequired()` — przełączenie z claim na unique cert.
- `FleetProvisioning_RequestRotation()` — rozpoczęcie kontrolowanej rotacji.

## Awaryjne anulowanie i recommission

Backend może unieważnić token/aktywację administracyjną przed zakończeniem
provisioningu. Jeżeli urządzenie jest osiągalne, wysyła komendę
`provisioning.cancel` zawierającą `confirm=true`, identyfikator aktywacji,
96-bitowy `hardwareUid` i świeży timestamp. Firmware:

1. odrzuca niepełną, przeterminowaną lub skierowaną do innego UID komendę;
2. zapisuje żądanie resetu poświadczeń przed ACK;
3. publikuje wynik komendy;
4. po restarcie usuwa niedokończone poświadczenia i wraca do kontrolowanego
   stanu fabrycznego.

Jeżeli urządzenie jest offline, revocation pozostaje stanem oczekującym w
backendzie. Samo unieważnienie rekordu chmurowego nie dowodzi fizycznego
wyczyszczenia Flash — wymagane jest późniejsze potwierdzenie urządzenia.

Ponowne dopuszczenie urządzenia po decommissioningu nie jest zwykłym retry.
Operacja recommission w aplikacji wymaga jednorazowego, wygasającego challenge,
uprawnień właściciela i ponownego wpisu UID do allowlisty. Lokalny stan
`DECOMMISSIONED` musi zostać usunięty w kontrolowanej procedurze serwisowej.

## Rotacja unikalnego certyfikatu urządzenia

Rotację rozpoczyna uwierzytelniona komenda serwisowa wysłana na istniejący temat
komend urządzenia:

```json
{
  "commandId": "rotate-2026-001",
  "type": "credentials.rotate",
  "confirm": true,
  "hardwareUid": "001F00423438511136353232"
}
```

Firmware odrzuca komendę bez jawnego `confirm=true`, z niezgodnym UID albo gdy
poprzednia rotacja nie została zakończona. Po zapisie `ROTATION_REQUESTED`
urządzenie przełącza się na claim, wykonuje `CreateKeysAndCertificate` oraz
`RegisterThing` dla tej samej nazwy Thing. Stary rekord `PROVISIONED` nie jest
kasowany. Nowa para przechodzi do `ROTATION_VALIDATING` i zostaje zatwierdzona
dopiero po 30 sekundach stabilnego MQTT. Trzy nieudane próby TLS/MQTT powodują
automatyczny rollback do poprzedniego certyfikatu; licznik przetrwa restart.

Firmware publikuje diagnostykę `CREDENTIAL_ROTATION_REQUESTED`,
`CREDENTIAL_ROTATION_SUCCEEDED` albo alert `CREDENTIAL_ROTATION_ROLLED_BACK`.
Po sukcesie firmware raportuje wyłącznie identyfikator użytego certyfikatu.
Backend wykrywa przejście z poprzedniego identyfikatora na nowy i oznacza stary
certyfikat jako oczekujący na wycofanie. Właściciel musi jawnie potwierdzić
operację w aplikacji. Przed modyfikacją AWS backend sprawdza, czy nowy certyfikat
ma stan `ACTIVE` i jest przypięty do właściwego Thing. Dopiero wtedy stary
certyfikat otrzymuje stan `INACTIVE`, traci polityki i powiązanie z Thing.
Obiekt certyfikatu nie jest automatycznie usuwany, dzięki czemu pozostaje ślad
audytowy, a operację można bezpiecznie ponowić po częściowym błędzie.

Rola IAM backendu wymaga do tej operacji: `iot:DescribeCertificate`,
`iot:ListThingPrincipals`, `iot:UpdateCertificate`,
`iot:ListPrincipalPolicies`, `iot:DetachPolicy` i
`iot:DetachThingPrincipal`. Urządzenie celowo nie otrzymuje żadnego z tych
uprawnień administracyjnych, ponieważ naruszałoby to zasadę least privilege.

Szablon Fleet Provisioning używany do rotacji musi dopuszczać ponowne
`RegisterThing` dla istniejącej nazwy Thing. Zalecane ustawienia zasobu Thing to
`AttributePayload: MERGE` oraz `ThingGroups: DO_NOTHING`; certyfikat tworzony w
rotacji musi otrzymać status `ACTIVE` i właściwą politykę urządzenia.

## Dane widoczne w aplikacji

Po docelowym reconnect firmware publikuje status z:

```json
{
  "provisioning": {
    "state": "PROVISIONED",
    "mode": "AUTO_FLEET",
    "credentialState": "PROVISIONED",
    "certificateId": "258f4e35ed1f03bd8a7a32c41e83b9ff9bbcbb87ddad9dde3fa991142601657e",
    "thingName": "stm32h753zi-dev-01",
    "hardwareUid": "001F00423438511136353232"
  }
}
```

Certyfikat, klucz prywatny i ownership token nigdy nie są publikowane w statusie,
telemetrii, diagnostyce ani logach UART.

## Test akceptacyjny

1. Wdrożyć stos CloudFormation i dodać UID płytki do allowlisty.
2. Wgrać firmware i potwierdzić marker `fleet_auto_v3 credential_rotation_v2`.
3. Potwierdzić sekwencję `EMPTY → PENDING_REGISTRATION → PROVISIONED`.
4. Potwierdzić reconnect z logiem `credential source=unique fleet certificate`.
5. Sprawdzić telemetrię i status `AUTO_FLEET` w aplikacji.
6. Zrestartować płytkę i sprawdzić, że nie występuje nowe żądanie
   `CreateKeysAndCertificate`.
7. W osobnym teście przerwać zasilanie po zapisie pending i potwierdzić wznowienie.
8. Usunąć UID z allowlisty lub użyć nieznanego UID i potwierdzić odrzucenie.
9. Odłączyć Ethernet podczas procesu i potwierdzić reconnect bez utraty stanu.
10. Unieważnić aktywację w trakcie procesu, potwierdzić ACK
    `provisioning.cancel`, restart i brak użycia niedokończonej pary.
11. Wykonać rotację, przerwać trzy walidacje nowej pary i potwierdzić powrót do
    poprzedniego certyfikatu.
12. Sprawdzić, że UART, status i diagnostyka nie zawierają PEM, ownership tokenu
    ani presigned URL.

## Ograniczenia i dalsze utwardzenie

Klucz urządzenia jest chroniony integralnością, dual-slotem i granicami linkera,
ale nie jest szyfrowany sprzętowo. W produkcie należy zastosować profil RDP/WRP
z `PRODUCTION_OPTION_BYTES.md` lub secure element. PCROP nie może być włączany
na cały sektor bez wydzielenia sekcji execute-only. Wspólny claim certificate powinien mieć wyłącznie minimalną claim policy;
po testach trzeba odłączyć od niego dawne szerokie polityki developerskie. Dla
większej floty zalecana jest rotacja claim certificate i alarmowanie AWS IoT Device
Defender o nietypowej liczbie prób provisioningowych.

Kod nie może samodzielnie unieważnić wspólnego claim certificate w AWS. Jest to
operacja incydentowa po stronie chmury, która musi mieć udokumentowany wpływ na
wszystkie jeszcze niezarejestrowane urządzenia.
