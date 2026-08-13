#include "sensor_manager.h"

#include "main.h"
#include "stm32h7xx_hal_adc.h"
#include "stm32h7xx_hal_adc_ex.h"
#include "stm32h7xx_ll_adc.h"

#include <string.h>

#define SENSOR_ADC_TIMEOUT_MS               20U
#define SENSOR_ADC_SAMPLE_COUNT              8U
#define SENSOR_TEMPERATURE_MIN_MILLI_C  (-40000L)
#define SENSOR_TEMPERATURE_MAX_MILLI_C  (150000L)
#define SENSOR_VDDA_MIN_MV                 1800U
#define SENSOR_VDDA_MAX_MV                 3600U
#define SENSOR_DISCOVERED_DEVICE_CAPACITY     8U

typedef struct
{
    SensorCapability capability;
    SensorReadFunction read;
    void *context;
    uint8_t occupied;
} SensorRegistryEntry;

static ADC_HandleTypeDef sensor_adc;
static SensorRegistryEntry sensor_registry[SENSOR_MANAGER_MAX_SENSORS];
static SensorDiscoveredDevice
    sensor_discovered_devices[SENSOR_DISCOVERED_DEVICE_CAPACITY];
static size_t sensor_discovered_count;
static uint8_t sensor_initialized;
static uint8_t sensor_adc_available;
static uint8_t sensor_registration_open;

static const SensorDefinition sensor_builtin_definitions[] = {
    { 0U, "temperature", "MCU temperature", "degC", "stm32h753.internal",
      SENSOR_BUS_INTERNAL, SENSOR_TEMPERATURE_MIN_MILLI_C,
      SENSOR_TEMPERATURE_MAX_MILLI_C },
    { 1U, "voltage", "Supply voltage", "V", "stm32h753.vrefint",
      SENSOR_BUS_INTERNAL, 1800, 3600 },
    { 2U, "current", "Current", "A", "platform.current",
      SENSOR_BUS_VIRTUAL, 0, 0 },
    { 3U, "signal", "Signal quality", "%", "platform.signal",
      SENSOR_BUS_VIRTUAL, 0, 100000 },
    { 4U, "humidity", "Humidity", "%", "platform.humidity",
      SENSOR_BUS_VIRTUAL, 0, 100000 }
};

static int SensorManager_ValidIdentifier(const char *value, size_t max_length)
{
    size_t length;
    size_t index;

    if ((value == NULL) || (value[0] == '\0'))
    {
        return 0;
    }

    length = strlen(value);
    if ((length == 0U) || (length > max_length) ||
        !((value[0] >= 'a') && (value[0] <= 'z')))
    {
        return 0;
    }

    for (index = 1U; index < length; index++)
    {
        char character = value[index];
        if (!(((character >= 'a') && (character <= 'z')) ||
              ((character >= '0') && (character <= '9')) ||
              (character == '_') || (character == '-') ||
              (character == '.')))
        {
            return 0;
        }
    }
    return 1;
}

static int SensorManager_ValidJsonText(const char *value, size_t max_length)
{
    size_t index;
    size_t length;

    if (value == NULL)
    {
        return 0;
    }
    length = strlen(value);
    if (length > max_length)
    {
        return 0;
    }
    for (index = 0U; index < length; index++)
    {
        unsigned char character = (unsigned char)value[index];
        if ((character < 0x20U) || (character > 0x7EU) ||
            (character == '"') || (character == '\\'))
        {
            return 0;
        }
    }
    return 1;
}

static int SensorManager_ValidDefinition(const SensorDefinition *definition)
{
    return (definition != NULL) &&
           (definition->slot < SENSOR_MANAGER_MAX_SENSORS) &&
           SensorManager_ValidIdentifier(definition->metric_id,
                                         SENSOR_MANAGER_METRIC_ID_MAX_LENGTH) &&
           SensorManager_ValidJsonText(definition->label,
                                       SENSOR_MANAGER_LABEL_MAX_LENGTH) &&
           SensorManager_ValidJsonText(definition->unit,
                                       SENSOR_MANAGER_UNIT_MAX_LENGTH) &&
           SensorManager_ValidJsonText(definition->driver,
                                       SENSOR_MANAGER_DRIVER_MAX_LENGTH) &&
           (definition->bus <= SENSOR_BUS_VIRTUAL);
}

static void SensorManager_RegisterBuiltins(void)
{
    size_t index;

    for (index = 0U;
         index < (sizeof(sensor_builtin_definitions) /
                  sizeof(sensor_builtin_definitions[0]));
         index++)
    {
        uint8_t slot = sensor_builtin_definitions[index].slot;
        sensor_registry[slot].capability.definition =
            sensor_builtin_definitions[index];
        sensor_registry[slot].capability.state = SENSOR_CAPABILITY_UNKNOWN;
        sensor_registry[slot].occupied = 1U;
    }
}

static int SensorManager_ReadChannel(uint32_t channel, uint32_t *average)
{
    ADC_ChannelConfTypeDef config = { 0 };
    uint64_t sum = 0U;
    uint32_t index;

    if (average == NULL)
    {
        return -1;
    }

    config.Channel = channel;
    config.Rank = ADC_REGULAR_RANK_1;
    config.SamplingTime = ADC_SAMPLETIME_810CYCLES_5;
    config.SingleDiff = ADC_SINGLE_ENDED;
    config.OffsetNumber = ADC_OFFSET_NONE;
    config.Offset = 0;
    config.OffsetRightShift = DISABLE;
    config.OffsetSignedSaturation = DISABLE;

    if (HAL_ADC_ConfigChannel(&sensor_adc, &config) != HAL_OK)
    {
        return -1;
    }

    for (index = 0U; index < SENSOR_ADC_SAMPLE_COUNT; index++)
    {
        if ((HAL_ADC_Start(&sensor_adc) != HAL_OK) ||
            (HAL_ADC_PollForConversion(&sensor_adc,
                                       SENSOR_ADC_TIMEOUT_MS) != HAL_OK))
        {
            (void)HAL_ADC_Stop(&sensor_adc);
            return -1;
        }

        sum += HAL_ADC_GetValue(&sensor_adc);
        if (HAL_ADC_Stop(&sensor_adc) != HAL_OK)
        {
            return -1;
        }
    }

    *average = (uint32_t)(sum / SENSOR_ADC_SAMPLE_COUNT);
    return 0;
}

int SensorManager_Register(const SensorDefinition *definition,
                           SensorReadFunction read,
                           void *context)
{
    size_t index;
    SensorRegistryEntry *entry;

    if ((sensor_registration_open == 0U) ||
        !SensorManager_ValidDefinition(definition) ||
        (definition->slot < SENSOR_MANAGER_PLATFORM_FIRST_SLOT) ||
        (read == NULL))
    {
        return -1;
    }

    entry = &sensor_registry[definition->slot];
    if (entry->occupied != 0U)
    {
        return -1;
    }

    for (index = 0U; index < SENSOR_MANAGER_MAX_SENSORS; index++)
    {
        if ((sensor_registry[index].occupied != 0U) &&
            (strcmp(sensor_registry[index].capability.definition.metric_id,
                    definition->metric_id) == 0))
        {
            return -1;
        }
    }

    entry->capability.definition = *definition;
    entry->capability.state = SENSOR_CAPABILITY_UNKNOWN;
    entry->read = read;
    entry->context = context;
    entry->occupied = 1U;
    return 0;
}

int SensorManager_Init(void)
{
    size_t discovered;

    if (sensor_initialized != 0U)
    {
        return 0;
    }

    memset(sensor_registry, 0, sizeof(sensor_registry));
    memset(sensor_discovered_devices, 0, sizeof(sensor_discovered_devices));
    sensor_discovered_count = 0U;
    SensorManager_RegisterBuiltins();

    sensor_registration_open = 1U;
    SensorPlatform_RegisterSensors();
    sensor_registration_open = 0U;

    discovered = SensorPlatform_DiscoverDevices(
        sensor_discovered_devices,
        SENSOR_DISCOVERED_DEVICE_CAPACITY);
    sensor_discovered_count =
        (discovered <= SENSOR_DISCOVERED_DEVICE_CAPACITY)
            ? discovered
            : SENSOR_DISCOVERED_DEVICE_CAPACITY;

    memset(&sensor_adc, 0, sizeof(sensor_adc));
    sensor_adc.Instance = ADC3;
    sensor_adc.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV2;
    sensor_adc.Init.Resolution = ADC_RESOLUTION_16B;
    sensor_adc.Init.ScanConvMode = DISABLE;
    sensor_adc.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    sensor_adc.Init.LowPowerAutoWait = DISABLE;
    sensor_adc.Init.ContinuousConvMode = DISABLE;
    sensor_adc.Init.NbrOfConversion = 1;
    sensor_adc.Init.DiscontinuousConvMode = DISABLE;
    sensor_adc.Init.NbrOfDiscConversion = 1;
    sensor_adc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    sensor_adc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    sensor_adc.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    sensor_adc.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
    sensor_adc.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    sensor_adc.Init.OversamplingMode = DISABLE;

    if ((HAL_ADC_Init(&sensor_adc) != HAL_OK) ||
        (HAL_ADCEx_Calibration_Start(&sensor_adc,
                                    ADC_CALIB_OFFSET_LINEARITY,
                                    ADC_SINGLE_ENDED) != HAL_OK))
    {
        sensor_registry[0].capability.state = SENSOR_CAPABILITY_ERROR;
        sensor_registry[1].capability.state = SENSOR_CAPABILITY_ERROR;
        (void)HAL_ADC_DeInit(&sensor_adc);
        /* External, independently initialized drivers remain usable. */
        sensor_initialized = 1U;
        sensor_adc_available = 0U;
        return -1;
    }

    sensor_adc_available = 1U;
    sensor_initialized = 1U;
    return 0;
}

int SensorManager_Read(SensorSnapshot *snapshot)
{
    uint32_t vref_raw;
    uint32_t temperature_raw;
    uint32_t vdda_mv;
    int32_t temperature_c;

    if (snapshot == NULL)
    {
        return -1;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->sampled_at_uptime_ms = HAL_GetTick();

    if (sensor_initialized == 0U)
    {
        snapshot->status = SENSOR_STATUS_NOT_INITIALIZED;
        return -1;
    }

    if ((sensor_adc_available != 0U) &&
        (SensorManager_ReadChannel(ADC_CHANNEL_VREFINT, &vref_raw) == 0) &&
        (vref_raw != 0U))
    {
        vdda_mv = __LL_ADC_CALC_VREFANALOG_VOLTAGE(
            vref_raw,
            LL_ADC_RESOLUTION_16B);
        if ((vdda_mv >= SENSOR_VDDA_MIN_MV) &&
            (vdda_mv <= SENSOR_VDDA_MAX_MV))
        {
            snapshot->supply_millivolts = vdda_mv;
            snapshot->valid_mask |= SENSOR_VALID_VOLTAGE;
        }

        if (SensorManager_ReadChannel(ADC_CHANNEL_TEMPSENSOR,
                                      &temperature_raw) == 0)
        {
            temperature_c =
                __LL_ADC_CALC_TEMPERATURE(vdda_mv,
                                         temperature_raw,
                                         LL_ADC_RESOLUTION_16B);
            snapshot->temperature_milli_celsius = temperature_c * 1000L;
            if ((snapshot->temperature_milli_celsius >=
                 SENSOR_TEMPERATURE_MIN_MILLI_C) &&
                (snapshot->temperature_milli_celsius <=
                 SENSOR_TEMPERATURE_MAX_MILLI_C))
            {
                snapshot->valid_mask |= SENSOR_VALID_TEMPERATURE;
            }
        }
    }

    if ((SensorPlatform_ReadCurrentMicroamps(&snapshot->current_microamps) == 0) &&
        (snapshot->current_microamps >= 0))
    {
        snapshot->valid_mask |= SENSOR_VALID_CURRENT;
    }

    if ((SensorPlatform_ReadHumidityMilliPercent(
             &snapshot->humidity_milli_percent) == 0) &&
        (snapshot->humidity_milli_percent <= 100000U))
    {
        snapshot->valid_mask |= SENSOR_VALID_HUMIDITY;
    }

    if ((SensorPlatform_ReadSignalPercent(&snapshot->signal_percent) == 0) &&
        (snapshot->signal_percent <= 100U))
    {
        snapshot->valid_mask |= SENSOR_VALID_SIGNAL;
    }

    sensor_registry[0].capability.state =
        ((snapshot->valid_mask & SENSOR_VALID_TEMPERATURE) != 0U)
            ? SENSOR_CAPABILITY_AVAILABLE
            : SENSOR_CAPABILITY_ERROR;
    sensor_registry[1].capability.state =
        ((snapshot->valid_mask & SENSOR_VALID_VOLTAGE) != 0U)
            ? SENSOR_CAPABILITY_AVAILABLE
            : SENSOR_CAPABILITY_ERROR;
    sensor_registry[2].capability.state =
        ((snapshot->valid_mask & SENSOR_VALID_CURRENT) != 0U)
            ? SENSOR_CAPABILITY_AVAILABLE
            : SENSOR_CAPABILITY_UNAVAILABLE;
    sensor_registry[3].capability.state =
        ((snapshot->valid_mask & SENSOR_VALID_SIGNAL) != 0U)
            ? SENSOR_CAPABILITY_AVAILABLE
            : SENSOR_CAPABILITY_UNAVAILABLE;
    sensor_registry[4].capability.state =
        ((snapshot->valid_mask & SENSOR_VALID_HUMIDITY) != 0U)
            ? SENSOR_CAPABILITY_AVAILABLE
            : SENSOR_CAPABILITY_UNAVAILABLE;

    snapshot->status =
        ((snapshot->valid_mask &
          (SENSOR_VALID_TEMPERATURE | SENSOR_VALID_VOLTAGE)) ==
         (SENSOR_VALID_TEMPERATURE | SENSOR_VALID_VOLTAGE))
            ? SENSOR_STATUS_OK
            : SENSOR_STATUS_PARTIAL;
    return (snapshot->valid_mask != 0U) ? 0 : -1;
}

static void SensorManager_AppendMeasurement(SensorMeasurement *output,
                                            size_t capacity,
                                            size_t *count,
                                            uint8_t slot,
                                            int64_t value,
                                            int8_t scale10,
                                            uint32_t sampled_at)
{
    if (*count >= capacity)
    {
        return;
    }

    output[*count].slot = slot;
    output[*count].value = value;
    output[*count].scale10 = scale10;
    output[*count].quality = SENSOR_QUALITY_GOOD;
    output[*count].sampled_at_uptime_ms = sampled_at;
    (*count)++;
}

int SensorManager_ReadAll(uint32_t enabled_slots,
                          SensorMeasurement *output,
                          size_t capacity,
                          size_t *count)
{
    SensorSnapshot snapshot;
    size_t produced = 0U;
    uint8_t slot;
    int legacy_result;

    if ((output == NULL) || (count == NULL) || (capacity == 0U) ||
        (sensor_initialized == 0U))
    {
        return -1;
    }

    legacy_result = SensorManager_Read(&snapshot);
    if (((enabled_slots & SENSOR_VALID_TEMPERATURE) != 0U) &&
        ((snapshot.valid_mask & SENSOR_VALID_TEMPERATURE) != 0U))
    {
        SensorManager_AppendMeasurement(output, capacity, &produced, 0U,
                                        snapshot.temperature_milli_celsius,
                                        -3, snapshot.sampled_at_uptime_ms);
    }
    if (((enabled_slots & SENSOR_VALID_VOLTAGE) != 0U) &&
        ((snapshot.valid_mask & SENSOR_VALID_VOLTAGE) != 0U))
    {
        SensorManager_AppendMeasurement(output, capacity, &produced, 1U,
                                        snapshot.supply_millivolts,
                                        -3, snapshot.sampled_at_uptime_ms);
    }
    if (((enabled_slots & SENSOR_VALID_CURRENT) != 0U) &&
        ((snapshot.valid_mask & SENSOR_VALID_CURRENT) != 0U))
    {
        SensorManager_AppendMeasurement(output, capacity, &produced, 2U,
                                        snapshot.current_microamps,
                                        -6, snapshot.sampled_at_uptime_ms);
    }
    if (((enabled_slots & SENSOR_VALID_SIGNAL) != 0U) &&
        ((snapshot.valid_mask & SENSOR_VALID_SIGNAL) != 0U))
    {
        SensorManager_AppendMeasurement(output, capacity, &produced, 3U,
                                        snapshot.signal_percent,
                                        0, snapshot.sampled_at_uptime_ms);
    }
    if (((enabled_slots & SENSOR_VALID_HUMIDITY) != 0U) &&
        ((snapshot.valid_mask & SENSOR_VALID_HUMIDITY) != 0U))
    {
        SensorManager_AppendMeasurement(output, capacity, &produced, 4U,
                                        snapshot.humidity_milli_percent,
                                        -3, snapshot.sampled_at_uptime_ms);
    }

    for (slot = SENSOR_MANAGER_PLATFORM_FIRST_SLOT;
         (slot < SENSOR_MANAGER_MAX_SENSORS) && (produced < capacity);
         slot++)
    {
        SensorRegistryEntry *entry = &sensor_registry[slot];
        SensorMeasurement measurement;

        if ((entry->occupied == 0U) ||
            ((enabled_slots & (1UL << slot)) == 0U))
        {
            continue;
        }

        memset(&measurement, 0, sizeof(measurement));
        measurement.slot = slot;
        measurement.quality = SENSOR_QUALITY_UNAVAILABLE;
        measurement.sampled_at_uptime_ms = HAL_GetTick();
        if ((entry->read(entry->context, &measurement) == 0) &&
            (measurement.slot == slot) &&
            (measurement.scale10 >= -9) && (measurement.scale10 <= 9))
        {
            entry->capability.state = SENSOR_CAPABILITY_AVAILABLE;
            if (measurement.quality == SENSOR_QUALITY_UNAVAILABLE)
            {
                measurement.quality = SENSOR_QUALITY_GOOD;
            }
            output[produced++] = measurement;
        }
        else
        {
            entry->capability.state = SENSOR_CAPABILITY_UNAVAILABLE;
        }
    }

    *count = produced;
    return ((produced > 0U) || (legacy_result == 0)) ? 0 : -1;
}

size_t SensorManager_GetCapabilities(SensorCapability *output, size_t capacity)
{
    size_t count = 0U;
    uint8_t slot;

    if ((output == NULL) || (capacity == 0U))
    {
        return 0U;
    }

    for (slot = 0U;
         (slot < SENSOR_MANAGER_MAX_SENSORS) && (count < capacity);
         slot++)
    {
        if (sensor_registry[slot].occupied != 0U)
        {
            output[count++] = sensor_registry[slot].capability;
        }
    }
    return count;
}

size_t SensorManager_GetDiscoveredDevices(SensorDiscoveredDevice *output,
                                          size_t capacity)
{
    size_t count = sensor_discovered_count;

    if ((output == NULL) || (capacity == 0U))
    {
        return 0U;
    }
    if (count > capacity)
    {
        count = capacity;
    }
    memcpy(output, sensor_discovered_devices, count * sizeof(*output));
    return count;
}

uint32_t SensorManager_RegisteredMask(void)
{
    uint32_t mask = 0U;
    uint8_t slot;

    for (slot = 0U; slot < SENSOR_MANAGER_MAX_SENSORS; slot++)
    {
        if (sensor_registry[slot].occupied != 0U)
        {
            mask |= (1UL << slot);
        }
    }
    return mask;
}

uint32_t SensorManager_MetricMaskForName(const char *metric_id)
{
    uint8_t slot;

    if (metric_id == NULL)
    {
        return 0U;
    }

    for (slot = 0U; slot < SENSOR_MANAGER_MAX_SENSORS; slot++)
    {
        if ((sensor_registry[slot].occupied != 0U) &&
            (strcmp(sensor_registry[slot].capability.definition.metric_id,
                    metric_id) == 0))
        {
            return (1UL << slot);
        }
    }
    return 0U;
}

const SensorCapability *SensorManager_CapabilityForSlot(uint8_t slot)
{
    if ((slot >= SENSOR_MANAGER_MAX_SENSORS) ||
        (sensor_registry[slot].occupied == 0U))
    {
        return NULL;
    }
    return &sensor_registry[slot].capability;
}

__weak void SensorPlatform_RegisterSensors(void)
{
}

__weak size_t SensorPlatform_DiscoverDevices(SensorDiscoveredDevice *output,
                                             size_t capacity)
{
    (void)output;
    (void)capacity;
    return 0U;
}

__weak int SensorPlatform_ReadCurrentMicroamps(int32_t *value)
{
    (void)value;
    return -1;
}

__weak int SensorPlatform_ReadHumidityMilliPercent(uint32_t *value)
{
    (void)value;
    return -1;
}

__weak int SensorPlatform_ReadSignalPercent(uint32_t *value)
{
    (void)value;
    return -1;
}

const char *SensorManager_StatusName(SensorStatus status)
{
    switch (status)
    {
        case SENSOR_STATUS_OK:
            return "OK";
        case SENSOR_STATUS_PARTIAL:
            return "PARTIAL";
        case SENSOR_STATUS_ADC_ERROR:
            return "ADC_ERROR";
        case SENSOR_STATUS_NOT_INITIALIZED:
        default:
            return "NOT_INITIALIZED";
    }
}

const char *SensorManager_BusName(SensorBus bus)
{
    static const char *const names[] = {
        "INTERNAL", "ADC", "I2C", "SPI", "UART",
        "CAN", "ONEWIRE", "DIGITAL", "VIRTUAL"
    };
    return (bus <= SENSOR_BUS_VIRTUAL) ? names[bus] : "UNKNOWN";
}

const char *SensorManager_CapabilityStateName(SensorCapabilityState state)
{
    switch (state)
    {
        case SENSOR_CAPABILITY_AVAILABLE:
            return "AVAILABLE";
        case SENSOR_CAPABILITY_UNAVAILABLE:
            return "UNAVAILABLE";
        case SENSOR_CAPABILITY_ERROR:
            return "ERROR";
        case SENSOR_CAPABILITY_UNSUPPORTED:
            return "UNSUPPORTED";
        case SENSOR_CAPABILITY_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

const char *SensorManager_QualityName(SensorQuality quality)
{
    switch (quality)
    {
        case SENSOR_QUALITY_GOOD:
            return "GOOD";
        case SENSOR_QUALITY_STALE:
            return "STALE";
        case SENSOR_QUALITY_ERROR:
            return "ERROR";
        case SENSOR_QUALITY_UNAVAILABLE:
        default:
            return "UNAVAILABLE";
    }
}
