#include "device_config.h"
#include "main.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * The final two 128 KiB sectors of Bank 2 are reserved for configuration.
 * Application code is restricted to Bank 1 and the preceding Bank 2 sectors
 * are reserved for download staging, so neither configuration slot can
 * overlap executable or staged data. A new record is always written to the
 * inactive slot; the previous valid slot remains available if power is lost.
 */
#define DEVICE_CONFIG_SLOT_A_ADDRESS       0x081C0000UL
#define DEVICE_CONFIG_SLOT_B_ADDRESS       0x081E0000UL
#define DEVICE_CONFIG_SLOT_A_SECTOR        FLASH_SECTOR_6
#define DEVICE_CONFIG_SLOT_B_SECTOR        FLASH_SECTOR_7
#define DEVICE_CONFIG_FLASH_MAGIC          0x43464731UL /* "CFG1" */
#define DEVICE_CONFIG_STORAGE_FORMAT       1U

typedef struct
{
    uint32_t magic;
    uint16_t storage_format;
    uint16_t record_size;
    uint32_t generation;
    uint32_t config_version;
    uint32_t telemetry_frequency_seconds;
    uint16_t diagnostics_max_message_bytes;
    uint16_t mqtt_keep_alive_seconds;
    uint8_t enabled_metrics;
    uint8_t log_level;
    uint8_t allow_rollback;
    uint8_t provisioning_state;
    uint32_t crc32;
} DeviceConfigFlashRecord;

_Static_assert(sizeof(DeviceConfigFlashRecord) == 32U,
               "Configuration record must occupy one STM32H7 flash word");

static DeviceConfig current_config;
static uint32_t current_generation;
static uint32_t active_slot_address;

static void DeviceConfig_SetDefaults(void)
{
    current_config.config_version = 0U;
    current_config.telemetry_frequency_seconds = 30U;
    current_config.diagnostics_max_message_bytes = 512U;
    current_config.mqtt_keep_alive_seconds = 60U;
    current_config.enabled_metrics = DEVICE_METRIC_TEMPERATURE |
                                     DEVICE_METRIC_VOLTAGE |
                                     DEVICE_METRIC_CURRENT |
                                     DEVICE_METRIC_SIGNAL;
    current_config.log_level = DEVICE_LOG_LEVEL_INFO;
    current_config.allow_rollback = 0U;
    current_config.provisioning_state = DEVICE_PROVISIONING_UNPROVISIONED;
    current_generation = 0U;
    active_slot_address = 0U;
}

static uint32_t DeviceConfig_Crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    size_t index;

    for (index = 0U; index < length; index++)
    {
        uint32_t bit;
        crc ^= data[index];

        for (bit = 0U; bit < 8U; bit++)
        {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
        }
    }

    return ~crc;
}

static void DeviceConfig_InvalidateCache(uint32_t address)
{
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    {
        SCB_InvalidateDCache_by_Addr((uint32_t *)address,
                                    (int32_t)sizeof(DeviceConfigFlashRecord));
    }

    __DSB();
    __ISB();
}

static void DeviceConfig_LoadRecord(uint32_t address, DeviceConfigFlashRecord *record)
{
    DeviceConfig_InvalidateCache(address);
    memcpy(record, (const void *)address, sizeof(*record));
}

static void DeviceConfig_RecordToConfig(const DeviceConfigFlashRecord *record,
                                        DeviceConfig *config)
{
    config->config_version = record->config_version;
    config->telemetry_frequency_seconds = record->telemetry_frequency_seconds;
    config->diagnostics_max_message_bytes = record->diagnostics_max_message_bytes;
    config->mqtt_keep_alive_seconds = record->mqtt_keep_alive_seconds;
    config->enabled_metrics = record->enabled_metrics;
    config->log_level = record->log_level;
    config->allow_rollback = record->allow_rollback;
    config->provisioning_state = record->provisioning_state;
}

static int DeviceConfig_RecordIsValid(const DeviceConfigFlashRecord *record)
{
    DeviceConfig decoded;
    uint32_t expected_crc;

    if ((record->magic != DEVICE_CONFIG_FLASH_MAGIC) ||
        (record->storage_format != DEVICE_CONFIG_STORAGE_FORMAT) ||
        (record->record_size != sizeof(DeviceConfigFlashRecord)))
    {
        return 0;
    }

    expected_crc = DeviceConfig_Crc32((const uint8_t *)record,
                                      offsetof(DeviceConfigFlashRecord, crc32));
    if (expected_crc != record->crc32)
    {
        return 0;
    }

    DeviceConfig_RecordToConfig(record, &decoded);
    return DeviceConfig_IsValid(&decoded);
}

static void DeviceConfig_PrepareRecord(const DeviceConfig *config,
                                       DeviceConfigFlashRecord *record)
{
    memset(record, 0, sizeof(*record));
    record->magic = DEVICE_CONFIG_FLASH_MAGIC;
    record->storage_format = DEVICE_CONFIG_STORAGE_FORMAT;
    record->record_size = sizeof(*record);
    record->generation = current_generation + 1U;
    record->config_version = config->config_version;
    record->telemetry_frequency_seconds = config->telemetry_frequency_seconds;
    record->diagnostics_max_message_bytes = config->diagnostics_max_message_bytes;
    record->mqtt_keep_alive_seconds = config->mqtt_keep_alive_seconds;
    record->enabled_metrics = config->enabled_metrics;
    record->log_level = config->log_level;
    record->allow_rollback = config->allow_rollback;
    record->provisioning_state = config->provisioning_state;
    record->crc32 = DeviceConfig_Crc32((const uint8_t *)record,
                                       offsetof(DeviceConfigFlashRecord, crc32));
}

static int DeviceConfig_Persist(const DeviceConfig *config)
{
    DeviceConfigFlashRecord record __attribute__((aligned(32)));
    DeviceConfigFlashRecord verify_record;
    FLASH_EraseInitTypeDef erase = { 0 };
    uint32_t sector_error = 0U;
    uint32_t target_address;
    uint32_t target_sector;
    HAL_StatusTypeDef status;

    if (active_slot_address == DEVICE_CONFIG_SLOT_A_ADDRESS)
    {
        target_address = DEVICE_CONFIG_SLOT_B_ADDRESS;
        target_sector = DEVICE_CONFIG_SLOT_B_SECTOR;
    }
    else
    {
        target_address = DEVICE_CONFIG_SLOT_A_ADDRESS;
        target_sector = DEVICE_CONFIG_SLOT_A_SECTOR;
    }

    DeviceConfig_PrepareRecord(config, &record);

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_2;
    erase.Sector = target_sector;
    erase.NbSectors = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return -1;
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK2);
    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    if (status == HAL_OK)
    {
        DeviceConfig_InvalidateCache(target_address);
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                                   target_address,
                                   (uint32_t)&record);
    }

    (void)HAL_FLASH_Lock();
    if (status != HAL_OK)
    {
        return -1;
    }

    DeviceConfig_LoadRecord(target_address, &verify_record);
    if (!DeviceConfig_RecordIsValid(&verify_record) ||
        (memcmp(&verify_record, &record, sizeof(record)) != 0))
    {
        return -1;
    }

    current_generation = record.generation;
    active_slot_address = target_address;
    return 0;
}

void DeviceConfig_Init(void)
{
    DeviceConfigFlashRecord slot_a;
    DeviceConfigFlashRecord slot_b;
    const DeviceConfigFlashRecord *selected = NULL;

    DeviceConfig_SetDefaults();
    DeviceConfig_LoadRecord(DEVICE_CONFIG_SLOT_A_ADDRESS, &slot_a);
    DeviceConfig_LoadRecord(DEVICE_CONFIG_SLOT_B_ADDRESS, &slot_b);

    if (DeviceConfig_RecordIsValid(&slot_a))
    {
        selected = &slot_a;
        active_slot_address = DEVICE_CONFIG_SLOT_A_ADDRESS;
    }

    if (DeviceConfig_RecordIsValid(&slot_b) &&
        ((selected == NULL) || (slot_b.generation > selected->generation)))
    {
        selected = &slot_b;
        active_slot_address = DEVICE_CONFIG_SLOT_B_ADDRESS;
    }

    if (selected != NULL)
    {
        DeviceConfig_RecordToConfig(selected, &current_config);
        current_generation = selected->generation;
    }
}

const DeviceConfig *DeviceConfig_Get(void)
{
    return &current_config;
}

int DeviceConfig_IsValid(const DeviceConfig *config)
{
    if (config == NULL)
    {
        return 0;
    }

    if ((config->telemetry_frequency_seconds < DEVICE_CONFIG_TELEMETRY_MIN_SECONDS) ||
        (config->telemetry_frequency_seconds > DEVICE_CONFIG_TELEMETRY_MAX_SECONDS) ||
        (config->diagnostics_max_message_bytes < DEVICE_CONFIG_LOG_MIN_BYTES) ||
        (config->diagnostics_max_message_bytes > DEVICE_CONFIG_LOG_MAX_BYTES) ||
        (config->mqtt_keep_alive_seconds < DEVICE_CONFIG_KEEP_ALIVE_MIN_SECONDS) ||
        (config->mqtt_keep_alive_seconds > DEVICE_CONFIG_KEEP_ALIVE_MAX_SECONDS) ||
        (config->enabled_metrics == 0U) ||
        ((config->enabled_metrics & (uint8_t)(~DEVICE_METRIC_ALL)) != 0U) ||
        (config->log_level > DEVICE_LOG_LEVEL_TRACE) ||
        (config->allow_rollback > 1U) ||
        (config->provisioning_state > DEVICE_PROVISIONING_PROVISIONED))
    {
        return 0;
    }

    return 1;
}

int DeviceConfig_Equals(const DeviceConfig *left, const DeviceConfig *right)
{
    if ((left == NULL) || (right == NULL))
    {
        return 0;
    }

    return (left->config_version == right->config_version) &&
           (left->telemetry_frequency_seconds == right->telemetry_frequency_seconds) &&
           (left->diagnostics_max_message_bytes == right->diagnostics_max_message_bytes) &&
           (left->mqtt_keep_alive_seconds == right->mqtt_keep_alive_seconds) &&
           (left->enabled_metrics == right->enabled_metrics) &&
           (left->log_level == right->log_level) &&
           (left->allow_rollback == right->allow_rollback) &&
           (left->provisioning_state == right->provisioning_state);
}

DeviceConfigApplyResult DeviceConfig_Apply(const DeviceConfig *candidate)
{
    if (!DeviceConfig_IsValid(candidate))
    {
        return DEVICE_CONFIG_REJECTED_INVALID;
    }

    if (candidate->config_version < current_config.config_version)
    {
        return DEVICE_CONFIG_REJECTED_STALE;
    }

    if (candidate->config_version == current_config.config_version)
    {
        return DeviceConfig_Equals(candidate, &current_config)
                   ? DEVICE_CONFIG_UNCHANGED
                   : DEVICE_CONFIG_REJECTED_CONFLICT;
    }

    if (DeviceConfig_Persist(candidate) != 0)
    {
        return DEVICE_CONFIG_PERSISTENCE_ERROR;
    }

    current_config = *candidate;
    return DEVICE_CONFIG_APPLIED;
}

const char *DeviceConfig_LogLevelName(uint8_t level)
{
    static const char *const names[] = { "ERROR", "WARN", "INFO", "DEBUG", "TRACE" };

    if (level > DEVICE_LOG_LEVEL_TRACE)
    {
        return "UNKNOWN";
    }

    return names[level];
}

int DeviceConfig_SetProvisioned(void)
{
    DeviceConfig candidate = current_config;

    if (current_config.provisioning_state == DEVICE_PROVISIONING_PROVISIONED)
    {
        return 0;
    }

    candidate.provisioning_state = DEVICE_PROVISIONING_PROVISIONED;
    if (DeviceConfig_Persist(&candidate) != 0)
    {
        return -1;
    }

    current_config = candidate;
    return 0;
}

void DeviceConfig_GetHardwareUid(char *output, size_t output_size)
{
    if ((output == NULL) || (output_size < (DEVICE_HARDWARE_UID_HEX_LENGTH + 1U)))
    {
        if ((output != NULL) && (output_size > 0U))
        {
            output[0] = '\0';
        }
        return;
    }

    (void)snprintf(output,
                   output_size,
                   "%08lX%08lX%08lX",
                   (unsigned long)HAL_GetUIDw0(),
                   (unsigned long)HAL_GetUIDw1(),
                   (unsigned long)HAL_GetUIDw2());
}

const char *DeviceConfig_ProvisioningStateName(uint8_t state)
{
    if (state == DEVICE_PROVISIONING_PROVISIONED)
    {
        return "PROVISIONED";
    }

    return "UNPROVISIONED";
}
