#include "device_config.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * The final two 128 KiB sectors of Bank 2 are reserved for configuration.
 * Application code is restricted to Bank 1 and the preceding Bank 2 sectors
 * are reserved for download staging, so neither configuration slot can
 * overlap executable or staged data. Records are appended in alternating
 * sectors; normal configuration updates program one Flash word and do not
 * erase an entire sector. The previous valid generation remains available if
 * power is lost.
 */
#define DEVICE_CONFIG_SLOT_A_ADDRESS       0x081C0000UL
#define DEVICE_CONFIG_SLOT_B_ADDRESS       0x081E0000UL
#define DEVICE_CONFIG_SLOT_A_SECTOR        FLASH_SECTOR_6
#define DEVICE_CONFIG_SLOT_B_SECTOR        FLASH_SECTOR_7
#define DEVICE_CONFIG_FLASH_MAGIC          0x43464731UL /* "CFG1" */
#define DEVICE_CONFIG_STORAGE_FORMAT       1U
#define DEVICE_CONFIG_SECTOR_SIZE          (128U * 1024U)

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

static int DeviceConfig_AddressInSector(uint32_t address, uint32_t sector_base)
{
    return (address >= sector_base) &&
           (address < (sector_base + DEVICE_CONFIG_SECTOR_SIZE));
}

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

static int DeviceConfig_RecordIsErased(const DeviceConfigFlashRecord *record)
{
    const uint32_t *word = (const uint32_t *)record;
    size_t index;

    for (index = 0U; index < (sizeof(*record) / sizeof(*word)); index++)
    {
        if (word[index] != 0xFFFFFFFFUL)
        {
            return 0;
        }
    }
    return 1;
}

static uint32_t DeviceConfig_FindErasedRecord(uint32_t sector_base)
{
    uint32_t offset;
    DeviceConfigFlashRecord record;

    for (offset = 0U;
         offset < DEVICE_CONFIG_SECTOR_SIZE;
         offset += sizeof(DeviceConfigFlashRecord))
    {
        DeviceConfig_LoadRecord(sector_base + offset, &record);
        if (DeviceConfig_RecordIsErased(&record))
        {
            return sector_base + offset;
        }
    }
    return 0U;
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
    uint32_t sector_error = 0xFFFFFFFFUL;
    uint32_t target_address;
    uint32_t target_sector;
    HAL_StatusTypeDef status;

    if (DeviceConfig_AddressInSector(active_slot_address,
                                     DEVICE_CONFIG_SLOT_A_ADDRESS))
    {
        target_address =
            DeviceConfig_FindErasedRecord(DEVICE_CONFIG_SLOT_B_ADDRESS);
        target_sector = DEVICE_CONFIG_SLOT_B_SECTOR;
    }
    else
    {
        target_address =
            DeviceConfig_FindErasedRecord(DEVICE_CONFIG_SLOT_A_ADDRESS);
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
    if (target_address == 0U)
    {
        status = HAL_FLASHEx_Erase(&erase, &sector_error);
        target_address = (target_sector == DEVICE_CONFIG_SLOT_A_SECTOR)
                             ? DEVICE_CONFIG_SLOT_A_ADDRESS
                             : DEVICE_CONFIG_SLOT_B_ADDRESS;
    }
    else
    {
        status = HAL_OK;
    }
    if (status == HAL_OK)
    {
        DeviceConfig_InvalidateCache(target_address);
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                                   target_address,
                                   (uint32_t)&record);
    }

    (void)HAL_FLASH_Lock();
    if ((status != HAL_OK) || (sector_error != 0xFFFFFFFFUL))
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
    DeviceConfigFlashRecord record;
    DeviceConfigFlashRecord selected;
    const uint32_t sector_bases[] = {
        DEVICE_CONFIG_SLOT_A_ADDRESS,
        DEVICE_CONFIG_SLOT_B_ADDRESS
    };
    size_t sector_index;
    uint32_t offset;
    int selected_valid = 0;

    DeviceConfig_SetDefaults();
    for (sector_index = 0U;
         sector_index < (sizeof(sector_bases) / sizeof(sector_bases[0]));
         sector_index++)
    {
        for (offset = 0U;
             offset < DEVICE_CONFIG_SECTOR_SIZE;
             offset += sizeof(DeviceConfigFlashRecord))
        {
            uint32_t address = sector_bases[sector_index] + offset;

            DeviceConfig_LoadRecord(address, &record);
            if (DeviceConfig_RecordIsErased(&record))
            {
                break;
            }
            if (DeviceConfig_RecordIsValid(&record) &&
                (!selected_valid ||
                 (record.generation > selected.generation)))
            {
                selected = record;
                selected_valid = 1;
                active_slot_address = address;
            }
        }
    }

    if (selected_valid)
    {
        DeviceConfig_RecordToConfig(&selected, &current_config);
        current_generation = selected.generation;
    }
}

const DeviceConfig *DeviceConfig_Get(void)
{
    return &current_config;
}

void DeviceConfig_GetSnapshot(DeviceConfig *config)
{
    if (config == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    *config = current_config;
    taskEXIT_CRITICAL();
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

    taskENTER_CRITICAL();
    current_config = *candidate;
    taskEXIT_CRITICAL();
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

    taskENTER_CRITICAL();
    current_config = candidate;
    taskEXIT_CRITICAL();
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
