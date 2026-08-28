# Model kosztów utrzymania AWS i transmisji

Stan modelu: **2026-08-24**. Dokument jest modelem parametrycznym, nie ofertą
cenową. Przed oddaniem pracy albo wdrożeniem należy wstawić aktualne stawki dla
wybranego regionu z oficjalnych stron AWS.

Źródła zasad naliczania:

- [AWS IoT Core pricing](https://aws.amazon.com/iot-core/pricing/);
- [AWS IoT additional metering details](https://aws.amazon.com/iot-core/pricing/additional-details/);
- [Amazon S3 pricing](https://aws.amazon.com/s3/pricing/);
- [Amazon CloudWatch pricing](https://aws.amazon.com/cloudwatch/pricing/);
- [Amazon DynamoDB pricing](https://aws.amazon.com/dynamodb/pricing/);
- [AWS Lambda pricing](https://aws.amazon.com/lambda/pricing/).

AWS IoT MQTT rozlicza wiadomości w blokach po 5 KiB. Do rozmiaru PUBLISH
wliczają się topic i payload. Wiadomość 8 KiB zużywa dwa bloki. Device Shadow i
Registry są rozliczane jako operacje w blokach po 1 KiB rekordu. PINGREQ,
PINGRESP, DISCONNECT, CONNACK, SUBACK i UNSUBSCRIBE nie są naliczane jako
wiadomości; pozostałe operacje należy sprawdzać według aktualnej tabeli AWS.

## Zmienne modelu

| Symbol | Znaczenie |
| --- | --- |
| `N` | liczba aktywnych urządzeń |
| `D` | liczba dni okresu, zwykle 30 |
| `Tt` | okres telemetry w sekundach |
| `Bt` | topic + payload jednej telemetry w bajtach |
| `Md` | diagnostyki na urządzenie dziennie |
| `Ma` | alarmy/zdarzenia na urządzenie dziennie |
| `Mc` | komendy na urządzenie dziennie |
| `Sj` | średni rozmiar artefaktu Job/OTA w bajtach |
| `J` | liczba pobrań artefaktu w miesiącu |
| `R` | liczba odbiorców/subskrybentów danej publikacji |

Funkcja bloków MQTT:

```text
blocks5k(bytes) = ceil(bytes / 5120)
```

Liczba bazowych próbek telemetry:

```text
telemetrySamples = N * D * 86400 / Tt
```

Minimalne jednostki dla publikacji do jednego konsumenta:

```text
telemetryUnits = telemetrySamples * blocks5k(Bt) * (1 + R)
```

Pierwszy składnik to przyjęcie publikacji przez broker, drugi — dostarczenie do
subskrybenta. Dla QoS 1 należy dodatkowo doliczyć naliczane PUBACK odebrane od
klientów zgodnie z wersją MQTT i aktualnymi regułami AWS. Retained publish może
również generować osobną pozycję retained.

## Scenariusz referencyjny bez stawek

Dla jednego urządzenia, 30 dni i telemetry co 60 s:

```text
1 * 30 * 86400 / 60 = 43 200 próbek/miesiąc
```

Jeżeli topic + payload mieszczą się w 5 KiB i wiadomość trafia do jednego
workera, daje to co najmniej 86 400 jednostek publish/delivery, przed PUBACK,
statusami, komendami i Shadow. Zmiana okresu z 60 s na 5 s zwiększa ten składnik
12-krotnie.

Jedna 30-minutowa sesja interakcji generuje przy 5 s około 360 próbek zamiast
30 próbek przy 60 s, czyli około 330 dodatkowych publikacji na urządzenie.

## Składniki kosztu

### IoT Core connectivity i messaging

- czas aktywnych połączeń MQTT urządzeń i backendu;
- inbound PUBLISH urządzenia/backendu;
- delivery do każdego subskrybenta;
- PUBACK odbierane przez usługę, jeśli podlegają naliczaniu;
- retained status/LWT;
- SUBSCRIBE i CONNECT podczas częstych reconnectów;
- Device Shadow get/update/delta oraz Registry/Fleet operations.

Heartbeat jako osobny PUBLISH zwykle zwiększa koszt bez potrzeby. Obecny system
używa retained status/LWT, regularnej telemetry i czasu ostatniego kontaktu.
MQTT PING utrzymuje sesję, a według bieżących zasad PINGREQ/PINGRESP nie są
naliczane jako wiadomości.

### Jobs i S3

- tworzenie i aktualizacje execution AWS Jobs;
- S3 storage artefaktów;
- PUT podczas publikacji wydania;
- GET dla każdego urządzenia;
- transfer danych artefaktu do urządzenia;
- nieudane retry, które mogą ponownie pobrać cały plik.

Przy pełnym OTA miesięczny wolumen danych można oszacować jako `J * Sj`.
Artefakty powinny mieć lifecycle/retencję, a aplikacja nie powinna generować
nowego obiektu dla identycznego SHA-256.

### Provisioning

- wywołania pre-provisioning Lambda i czas wykonania;
- odczyt allowlisty DynamoDB;
- operacje Registry, utworzenie certyfikatu i RegisterThing;
- CloudWatch logs hooka, jeśli są włączone.

Tabela DynamoDB w trybie on-demand nie wymaga uruchomionej instancji, ale
generuje koszty przechowywania i żądań. Samo istnienie niewielkiej tabeli jest
innym profilem kosztowym niż intensywny scan; hook powinien używać punktowego
odczytu po `hardwareUid`, nie pełnego skanowania.

### Diagnostyka i retencja

CloudWatch może naliczać ingestion, storage i skanowanie Logs Insights.
Produkcja powinna ustawić jawny okres retencji. `DEBUG`/`TRACE` i okresowy
`MODE_ACTIVE` należy ograniczać czasowo, agregować i nie przesyłać pełnych
powtarzalnych logów UART do chmury.

Lokalna baza aplikacji nie generuje kosztu zarządzanej bazy AWS. Po migracji do
RDS/Aurora należy osobno policzyć compute, storage, backup, I/O i transfer.

## Optymalizacje zaimplementowane

- okres telemetry konfigurowalny 5–3600 s;
- brak osobnego płatnego heartbeat PUBLISH;
- tryb interakcji ograniczony do maksymalnie 1800 s;
- limity poziomu i wielkości diagnostyki;
- bounded offline telemetry i redukcja częstotliwości po problemach;
- małe JSON-y i topic contract bez duplikowania danych binarnych;
- Jobs/OTA tylko na żądanie, SHA-256 i retry z limitem;
- capability publikowane jako osobny, rzadszy dokument;
- CloudFormation zamiast ręcznie tworzonych, zapomnianych zasobów.

## Kontrole operacyjne

1. Ustawić AWS Budget z powiadomieniem oraz Cost Anomaly Detection.
2. Tagować zasoby według projektu, środowiska i właściciela.
3. Co miesiąc raportować wiadomości według kanału, średni/p95 payload, liczbę
   reconnectów, Shadow operations, Jobs, S3 GB i log ingestion GB.
4. Alarmować nagły wzrost telemetry, reconnect storm, błędne Jobs i Lambda
   errors/throttles.
5. Usuwać nieużywane wersje firmware według retencji, ale zachować zatwierdzone
   obrazy wymagane przez rollback i audyt.
6. Nie traktować Free Tier jako mechanizmu bezpieczeństwa kosztowego; limity i
   alerty muszą działać również po jego zakończeniu.

## Tabela do uzupełnienia w pracy

| Scenariusz | N | Tt | Śr. payload | Msg units/mies. | S3 GB | Logs GB | Koszt wg regionu |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| development | 1 | 60 s | pomiar | obliczyć | obliczyć | obliczyć | obliczyć |
| pilot | 10 | 60 s | pomiar | obliczyć | obliczyć | obliczyć | obliczyć |
| produkcja | 1000 | wymaganie | pomiar | obliczyć | obliczyć | obliczyć | obliczyć |
| service mode | N aktywnych | 5 s | pomiar | obliczyć | 0 | podwyższone | obliczyć |

Wartości `pomiar` należy wziąć z rzeczywistych payloadów, nie z przybliżenia
samego JSON. Kalkulację trzeba powtórzyć po każdej zmianie częstotliwości,
retencji, liczby subskrybentów albo regionu.

