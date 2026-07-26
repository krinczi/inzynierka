#ifndef INC_SENSOR_MANAGER_H_
#define INC_SENSOR_MANAGER_H_

#include <stdint.h>

enum
{
    SENSOR_VALID_TEMPERATURE = (1U << 0),
    SENSOR_VALID_VOLTAGE     = (1U << 1),
    SENSOR_VALID_CURRENT     = (1U << 2),
    SENSOR_VALID_SIGNAL      = (1U << 3),
    SENSOR_VALID_HUMIDITY    = (1U << 4)
};

typedef enum
{
    SENSOR_STATUS_OK = 0,
    SENSOR_STATUS_PARTIAL,
    SENSOR_STATUS_ADC_ERROR,
    SENSOR_STATUS_NOT_INITIALIZED
} SensorStatus;

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

/*
 * Initializes ADC3 and the internal VREFINT/temperature measurement paths.
 * The call is idempotent and does not fabricate unavailable external values.
 */
int SensorManager_Init(void);
int SensorManager_Read(SensorSnapshot *snapshot);

/*
 * Board/application hooks. A real external sensor driver overrides these weak
 * functions and returns 0 with a valid value. The defaults return -1.
 */
int SensorPlatform_ReadCurrentMicroamps(int32_t *value);
int SensorPlatform_ReadHumidityMilliPercent(uint32_t *value);
int SensorPlatform_ReadSignalPercent(uint32_t *value);

const char *SensorManager_StatusName(SensorStatus status);

#endif /* INC_SENSOR_MANAGER_H_ */
