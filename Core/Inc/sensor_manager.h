#ifndef INC_SENSOR_MANAGER_H_
#define INC_SENSOR_MANAGER_H_

#include <stddef.h>
#include <stdint.h>

#define SENSOR_MANAGER_MAX_SENSORS             32U
#define SENSOR_MANAGER_PLATFORM_FIRST_SLOT       5U
#define SENSOR_MANAGER_METRIC_ID_MAX_LENGTH      31U
#define SENSOR_MANAGER_LABEL_MAX_LENGTH          47U
#define SENSOR_MANAGER_UNIT_MAX_LENGTH           15U
#define SENSOR_MANAGER_DRIVER_MAX_LENGTH         31U

enum
{
    SENSOR_VALID_TEMPERATURE = (1UL << 0),
    SENSOR_VALID_VOLTAGE     = (1UL << 1),
    SENSOR_VALID_CURRENT     = (1UL << 2),
    SENSOR_VALID_SIGNAL      = (1UL << 3),
    SENSOR_VALID_HUMIDITY    = (1UL << 4)
};

typedef enum
{
    SENSOR_STATUS_OK = 0,
    SENSOR_STATUS_PARTIAL,
    SENSOR_STATUS_ADC_ERROR,
    SENSOR_STATUS_NOT_INITIALIZED
} SensorStatus;

typedef enum
{
    SENSOR_BUS_INTERNAL = 0,
    SENSOR_BUS_ADC,
    SENSOR_BUS_I2C,
    SENSOR_BUS_SPI,
    SENSOR_BUS_UART,
    SENSOR_BUS_CAN,
    SENSOR_BUS_ONEWIRE,
    SENSOR_BUS_DIGITAL,
    SENSOR_BUS_VIRTUAL
} SensorBus;

typedef enum
{
    SENSOR_CAPABILITY_UNKNOWN = 0,
    SENSOR_CAPABILITY_AVAILABLE,
    SENSOR_CAPABILITY_UNAVAILABLE,
    SENSOR_CAPABILITY_ERROR,
    SENSOR_CAPABILITY_UNSUPPORTED
} SensorCapabilityState;

typedef enum
{
    SENSOR_QUALITY_GOOD = 0,
    SENSOR_QUALITY_STALE,
    SENSOR_QUALITY_ERROR,
    SENSOR_QUALITY_UNAVAILABLE
} SensorQuality;

/*
 * A slot is the stable, persisted identity of a metric (0..31). Slots 0..4
 * are reserved for the built-in compatibility metrics. Platform drivers use
 * slots 5..31 and must keep their assigned slot stable between releases.
 * Strings referenced by a definition must have static lifetime.
 */
typedef struct
{
    uint8_t slot;
    const char *metric_id;
    const char *label;
    const char *unit;
    const char *driver;
    SensorBus bus;
    int32_t minimum_milli;
    int32_t maximum_milli;
} SensorDefinition;

/* value * 10^scale10 is the value expressed in definition.unit. */
typedef struct
{
    uint8_t slot;
    int64_t value;
    int8_t scale10;
    SensorQuality quality;
    uint32_t sampled_at_uptime_ms;
} SensorMeasurement;

typedef int (*SensorReadFunction)(void *context, SensorMeasurement *measurement);

typedef struct
{
    SensorDefinition definition;
    SensorCapabilityState state;
} SensorCapability;

typedef struct
{
    SensorBus bus;
    uint16_t address;
    const char *identifier;
    const char *reason;
} SensorDiscoveredDevice;

typedef struct
{
    uint32_t valid_mask;
    SensorStatus status;
    int32_t temperature_milli_celsius;
    uint32_t supply_millivolts;
    int32_t current_microamps;
    uint32_t signal_percent;
    uint32_t humidity_milli_percent;
    uint32_t sampled_at_uptime_ms;
} SensorSnapshot;

int SensorManager_Init(void);
int SensorManager_Read(SensorSnapshot *snapshot);

/*
 * Register a compiled-in driver. Registration is deterministic and bounded;
 * duplicate slots/metric ids, invalid identifiers and registrations after
 * initialization are rejected. The callback must be non-blocking or use a
 * short, bounded timeout.
 */
int SensorManager_Register(const SensorDefinition *definition,
                           SensorReadFunction read,
                           void *context);
size_t SensorManager_GetCapabilities(SensorCapability *output, size_t capacity);
size_t SensorManager_GetDiscoveredDevices(SensorDiscoveredDevice *output,
                                          size_t capacity);
int SensorManager_ReadAll(uint32_t enabled_slots,
                          SensorMeasurement *output,
                          size_t capacity,
                          size_t *count);
uint32_t SensorManager_RegisteredMask(void);
uint32_t SensorManager_MetricMaskForName(const char *metric_id);
const SensorCapability *SensorManager_CapabilityForSlot(uint8_t slot);

/*
 * Board integration hooks. Override SensorPlatform_RegisterSensors() and call
 * SensorManager_Register() for every supported driver. Discovery reports
 * physically detected devices for which no driver is present; the core never
 * probes an uninitialized bus on its own.
 */
void SensorPlatform_RegisterSensors(void);
size_t SensorPlatform_DiscoverDevices(SensorDiscoveredDevice *output,
                                      size_t capacity);

/* Backward-compatible hooks for the original external measurements. */
int SensorPlatform_ReadCurrentMicroamps(int32_t *value);
int SensorPlatform_ReadHumidityMilliPercent(uint32_t *value);
int SensorPlatform_ReadSignalPercent(uint32_t *value);

const char *SensorManager_StatusName(SensorStatus status);
const char *SensorManager_BusName(SensorBus bus);
const char *SensorManager_CapabilityStateName(SensorCapabilityState state);
const char *SensorManager_QualityName(SensorQuality quality);

#endif /* INC_SENSOR_MANAGER_H_ */
