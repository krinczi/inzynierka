#ifndef INC_DEVICE_CONFIG_H_
#define INC_DEVICE_CONFIG_H_

#include <stddef.h>
#include <stdint.h>

#define DEVICE_CONFIG_SCHEMA_VERSION              1U

#define DEVICE_CONFIG_TELEMETRY_MIN_SECONDS       5U
#define DEVICE_CONFIG_TELEMETRY_MAX_SECONDS       3600U
#define DEVICE_CONFIG_LOG_MIN_BYTES               64U
#define DEVICE_CONFIG_LOG_MAX_BYTES               4096U
#define DEVICE_CONFIG_KEEP_ALIVE_MIN_SECONDS      15U
#define DEVICE_CONFIG_KEEP_ALIVE_MAX_SECONDS      900U
#define DEVICE_HARDWARE_UID_HEX_LENGTH             24U

#define DEVICE_METRIC_TEMPERATURE                 (1U << 0)
#define DEVICE_METRIC_VOLTAGE                     (1U << 1)
#define DEVICE_METRIC_CURRENT                     (1U << 2)
#define DEVICE_METRIC_SIGNAL                      (1U << 3)
#define DEVICE_METRIC_HUMIDITY                    (1U << 4)
#define DEVICE_METRIC_ALL                         (DEVICE_METRIC_TEMPERATURE | \
                                                   DEVICE_METRIC_VOLTAGE | \
                                                   DEVICE_METRIC_CURRENT | \
                                                   DEVICE_METRIC_SIGNAL | \
                                                   DEVICE_METRIC_HUMIDITY)

typedef enum
{
    DEVICE_LOG_LEVEL_ERROR = 0,
    DEVICE_LOG_LEVEL_WARN,
    DEVICE_LOG_LEVEL_INFO,
    DEVICE_LOG_LEVEL_DEBUG,
    DEVICE_LOG_LEVEL_TRACE
} DeviceLogLevel;

typedef enum
{
    DEVICE_PROVISIONING_UNPROVISIONED = 0,
    DEVICE_PROVISIONING_PROVISIONED = 1
} DeviceProvisioningState;

typedef struct
{
    uint32_t config_version;
    uint32_t telemetry_frequency_seconds;
    uint16_t diagnostics_max_message_bytes;
    uint16_t mqtt_keep_alive_seconds;
    uint8_t enabled_metrics;
    uint8_t log_level;
    uint8_t allow_rollback;
    uint8_t provisioning_state;
} DeviceConfig;

typedef enum
{
    DEVICE_CONFIG_APPLIED = 0,
    DEVICE_CONFIG_UNCHANGED,
    DEVICE_CONFIG_REJECTED_INVALID,
    DEVICE_CONFIG_REJECTED_STALE,
    DEVICE_CONFIG_REJECTED_CONFLICT,
    DEVICE_CONFIG_PERSISTENCE_ERROR
} DeviceConfigApplyResult;

void DeviceConfig_Init(void);
const DeviceConfig *DeviceConfig_Get(void);
void DeviceConfig_GetSnapshot(DeviceConfig *config);
int DeviceConfig_IsValid(const DeviceConfig *config);
int DeviceConfig_Equals(const DeviceConfig *left, const DeviceConfig *right);
DeviceConfigApplyResult DeviceConfig_Apply(const DeviceConfig *candidate);
const char *DeviceConfig_LogLevelName(uint8_t level);
int DeviceConfig_SetProvisioned(void);
void DeviceConfig_GetHardwareUid(char *output, size_t output_size);
const char *DeviceConfig_ProvisioningStateName(uint8_t state);

#endif /* INC_DEVICE_CONFIG_H_ */
