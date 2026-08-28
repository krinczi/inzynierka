# Uniwersalny rejestr sensorów STM32H753

Stan implementacji: **rejestr schema v2 aktywny, konfiguracja schema v3,
zweryfikowano 2026-08-24**.

## Cel i granica rozwiązania

Firmware nie próbuje zgadywać protokołu dowolnego elementu podłączonego do płytki. Takie zachowanie byłoby nieprzewidywalne i mogłoby powodować kolizje magistrali albo błędne pomiary. Rdzeń udostępnia zamiast tego bezpieczny, ograniczony rejestr maksymalnie 32 metryk. Każdy obsługiwany sensor ma jawny, skompilowany sterownik, a wykryty sprzęt bez sterownika jest raportowany jako `UNSUPPORTED`.

Rozszerzenie o nowy model sensora nie wymaga zmian w TLS, MQTT, Device Shadow, backendzie ani UI. Integrator dodaje sterownik, stabilny slot i rejestruje definicję w `SensorPlatform_RegisterSensors()`.

„Uniwersalny” oznacza wspólny kontrakt danych i rozszerzalny rejestr, a nie
automatyczną obsługę dowolnego elementu elektrycznego. Firmware nie może
bezpiecznie rozpoznać protokołu, adresu, pinów, napięcia ani kalibracji nieznanego
sensora bez jawnego sterownika wariantu płytki.

## Stabilne sloty

- `0`: `temperature` — temperatura wewnętrzna MCU,
- `1`: `voltage` — VDDA z VREFINT,
- `2`: `current` — zgodny wstecznie hook platformowy,
- `3`: `signal` — zgodny wstecznie hook platformowy,
- `4`: `humidity` — zgodny wstecznie hook platformowy,
- `5..31`: sterowniki płytki i rozszerzeń.

Numer slotu jest częścią trwałej konfiguracji. Po opublikowaniu firmware nie wolno przypisać istniejącego slotu do innej metryki. Nowy sensor otrzymuje wolny slot.

### Faktycznie dostępne źródła na bieżącej płytce

| Slot | Źródło | Stan bez dodatkowego hardware |
| ---: | --- | --- |
| 0 | wewnętrzny kanał ADC temperatury MCU | dostępny po poprawnej inicjalizacji ADC3 |
| 1 | VREFINT i fabryczna kalibracja VDDA | dostępny po poprawnej inicjalizacji ADC3 |
| 2 | `SensorPlatform_ReadCurrentMicroamps()` | `UNAVAILABLE`, dopóki wariant płytki nie dostarczy implementacji |
| 3 | `SensorPlatform_ReadSignalPercent()` | `UNAVAILABLE`, dopóki warstwa sieci/radia nie dostarczy implementacji |
| 4 | `SensorPlatform_ReadHumidityMilliPercent()` | `UNAVAILABLE`, dopóki wariant płytki nie dostarczy implementacji |
| 5–31 | zarejestrowane sterowniki | zależnie od produktu |

Brak pomiaru nie jest zastępowany zerem ani wartością demonstracyjną. Payload
pomija nieważną wartość i raportuje stan capability.

## Kontrakt sterownika

```c
static int Pressure_Read(void *context, SensorMeasurement *measurement)
{
    PressureDriver *driver = context;
    int32_t pressure_pa;

    if (PressureDriver_ReadPa(driver, &pressure_pa) != 0)
    {
        return -1;
    }

    measurement->slot = 5U;
    measurement->value = pressure_pa;
    measurement->scale10 = -2; /* Pa -> hPa */
    measurement->quality = SENSOR_QUALITY_GOOD;
    measurement->sampled_at_uptime_ms = HAL_GetTick();
    return 0;
}

void SensorPlatform_RegisterSensors(void)
{
    static const SensorDefinition pressure = {
        .slot = 5U,
        .metric_id = "pressure",
        .label = "Pressure",
        .unit = "hPa",
        .driver = "bmp280",
        .bus = SENSOR_BUS_I2C,
        .minimum_milli = 300000,
        .maximum_milli = 1100000
    };

    (void)SensorManager_Register(&pressure, Pressure_Read, &pressure_driver);
}
```

Callback odczytu musi mieć ograniczony timeout. Nie może oczekiwać bez końca, wykonywać długiego skanowania magistrali ani publikować MQTT. Wartość jest liczbą całkowitą ze skalą dziesiętną: `value * 10^scale10` w jednostce definicji.

Sterownik odpowiada również za inicjalizację magistrali, kontrolę zakresu,
obsługę błędów CRC sensora, kalibrację i bezpieczny powrót po odłączeniu. Rdzeń
rejestru nie może odróżnić błędu elektrycznego od błędu protokołu, jeżeli driver
zwróci jedynie ogólne `-1`.

## Discovery

`SensorPlatform_DiscoverDevices()` jest opcjonalnym hookiem płytki. Zwraca maksymalnie osiem fizycznie wykrytych urządzeń, dla których nie zarejestrowano sterownika. Rdzeń nie skanuje sam I2C, SPI, UART, CAN ani 1-Wire, ponieważ nie zna konfiguracji pinów, zegarów i zasad współdzielenia magistrali.

Discovery jest informacją diagnostyczną, a nie zgodą na odczyt. Wykryty adres
bez sterownika pozostaje `UNSUPPORTED` i nie otrzymuje dynamicznie pobieranego
kodu.

## MQTT

Po zestawieniu sesji urządzenie publikuje:

- `iot/v1/{env}/devices/{deviceId}/capabilities` — katalog slotów, jednostek, sterowników, magistral i stanów;
- `iot/v1/{env}/devices/{deviceId}/telemetry` — telemetrię schema v2 z tablicą `measurements`.

Payload telemetrii zachowuje również stare pola `temperature`, `voltage`, `current`, `signal` i `humidity`, jeśli odczyt jest dostępny. Pozwala to wdrażać nowy firmware bez jednoczesnej migracji starszych konsumentów.

## Konfiguracja

Named Device Shadow `config` schema v3 nadal używa czytelnych identyfikatorów
w `enabledMetrics`. Firmware mapuje je na stabilne bity slotów i zapisuje
32-bitową maskę w dual-slot konfiguracji Flash. Rekordy storage format 1 i 2 są
migrowane podczas odczytu; nowy zapis używa formatu 3. Aktualizacja Shadow musi
zawierać nowszy `configVersion`; duplikat nie powoduje ponownego zapisu.

## Zasady produkcyjne

1. ID metryki musi pasować do `[a-z][a-z0-9_.-]{0,30}`.
2. Slot i ID muszą być unikalne.
3. Sterownik rejestruje się wyłącznie podczas `SensorManager_Init()`.
4. Liczba sensorów, długości tekstów, payload i lista discovery są ograniczone.
5. Backend odrzuca duplikaty, nieprawidłowe sloty, jednostki i metryki niezgodne z opublikowanym katalogiem.
6. Brak sterownika oznacza `UNSUPPORTED`, a nie wygenerowaną wartość zastępczą.
7. Jeden payload zawiera maksymalnie 32 pomiary, a lista discovery maksymalnie
   osiem urządzeń.
8. Identyfikator i slot są częścią publicznego kontraktu i nie mogą zmienić
   znaczenia między wersjami firmware.

## Test akceptacyjny nowego sensora

1. Zarejestrować driver w stałym slocie i zbudować firmware bez ostrzeżeń.
2. Sprawdzić topic `capabilities` oraz zgodność slotu, ID, jednostki i magistrali.
3. Włączyć metrykę w aplikacji i potwierdzić Device Shadow schema v3.
4. Sprawdzić `measurements[]`, jakość, zakres i timestamp.
5. Odłączyć sensor: firmware ma raportować brak pomiaru bez zawieszenia MQTT.
6. Podłączyć ponownie i potwierdzić powrót stanu `AVAILABLE` po ponownym zestawieniu sesji lub publikacji katalogu.
7. Zrestartować urządzenie i potwierdzić zachowanie maski w konfiguracji.
8. Wymusić timeout/CRC error sterownika i potwierdzić, że MQTT oraz watchdog
   nadal pracują, a capability przechodzi do `UNAVAILABLE`/`ERROR`.
9. Zweryfikować jednostkę i `scale10` w backendzie; UI nie może interpretować
   surowej liczby bez metadanych capability.
