# Uniwersalny rejestr sensorów STM32H753

## Cel i granica rozwiązania

Firmware nie próbuje zgadywać protokołu dowolnego elementu podłączonego do płytki. Takie zachowanie byłoby nieprzewidywalne i mogłoby powodować kolizje magistrali albo błędne pomiary. Rdzeń udostępnia zamiast tego bezpieczny, ograniczony rejestr maksymalnie 32 metryk. Każdy obsługiwany sensor ma jawny, skompilowany sterownik, a wykryty sprzęt bez sterownika jest raportowany jako `UNSUPPORTED`.

Rozszerzenie o nowy model sensora nie wymaga zmian w TLS, MQTT, Device Shadow, backendzie ani UI. Integrator dodaje sterownik, stabilny slot i rejestruje definicję w `SensorPlatform_RegisterSensors()`.

## Stabilne sloty

- `0`: `temperature` — temperatura wewnętrzna MCU,
- `1`: `voltage` — VDDA z VREFINT,
- `2`: `current` — zgodny wstecznie hook platformowy,
- `3`: `signal` — zgodny wstecznie hook platformowy,
- `4`: `humidity` — zgodny wstecznie hook platformowy,
- `5..31`: sterowniki płytki i rozszerzeń.

Numer slotu jest częścią trwałej konfiguracji. Po opublikowaniu firmware nie wolno przypisać istniejącego slotu do innej metryki. Nowy sensor otrzymuje wolny slot.

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

## Discovery

`SensorPlatform_DiscoverDevices()` jest opcjonalnym hookiem płytki. Zwraca maksymalnie osiem fizycznie wykrytych urządzeń, dla których nie zarejestrowano sterownika. Rdzeń nie skanuje sam I2C, SPI, UART, CAN ani 1-Wire, ponieważ nie zna konfiguracji pinów, zegarów i zasad współdzielenia magistrali.

## MQTT

Po zestawieniu sesji urządzenie publikuje:

- `iot/{env}/devices/{deviceId}/capabilities` — katalog slotów, jednostek, sterowników, magistral i stanów;
- `iot/{env}/devices/{deviceId}/telemetry` — telemetrię schema v2 z tablicą `measurements`.

Payload telemetrii zachowuje również stare pola `temperature`, `voltage`, `current`, `signal` i `humidity`, jeśli odczyt jest dostępny. Pozwala to wdrażać nowy firmware bez jednoczesnej migracji starszych konsumentów.

## Konfiguracja

Device Shadow schema v3 nadal używa czytelnych identyfikatorów w `enabledMetrics`. Firmware mapuje je na stabilne bity slotów i zapisuje 32-bitową maskę atomowo w konfiguracji Flash. Rekordy storage format 1 i 2 są migrowane podczas odczytu; nowy zapis używa formatu 3 i nadal zajmuje dwa flashwordy.

## Zasady produkcyjne

1. ID metryki musi pasować do `[a-z][a-z0-9_.-]{0,30}`.
2. Slot i ID muszą być unikalne.
3. Sterownik rejestruje się wyłącznie podczas `SensorManager_Init()`.
4. Liczba sensorów, długości tekstów, payload i lista discovery są ograniczone.
5. Backend odrzuca duplikaty, nieprawidłowe sloty, jednostki i metryki niezgodne z opublikowanym katalogiem.
6. Brak sterownika oznacza `UNSUPPORTED`, a nie wygenerowaną wartość zastępczą.

## Test akceptacyjny nowego sensora

1. Zarejestrować driver w stałym slocie i zbudować firmware bez ostrzeżeń.
2. Sprawdzić topic `capabilities` oraz zgodność slotu, ID, jednostki i magistrali.
3. Włączyć metrykę w aplikacji i potwierdzić Device Shadow schema v3.
4. Sprawdzić `measurements[]`, jakość, zakres i timestamp.
5. Odłączyć sensor: firmware ma raportować brak pomiaru bez zawieszenia MQTT.
6. Podłączyć ponownie i potwierdzić powrót stanu `AVAILABLE` po ponownym zestawieniu sesji lub publikacji katalogu.
7. Zrestartować urządzenie i potwierdzić zachowanie maski w konfiguracji.

