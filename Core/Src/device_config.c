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
#define DEVICE_CONFIG_STORAGE_FORMAT       3U
#define DEVICE_CONFIG_V2_FORMAT            2U
#define DEVICE_CONFIG_LEGACY_FORMAT        1U
#define DEVICE_CONFIG_SECTOR_SIZE          (128U * 1024U)
#define DEVICE_CONFIG_FLASHWORD_SIZE       32U

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
} DeviceConfigLegacyFlashRecord;

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
    uint8_t network_addressing_mode;
    uint8_t network_dhcp_fallback_enabled;
    uint8_t local_lifecycle_state;
    uint8_t reserved;
    uint8_t ipv4_address[4];
    uint8_t ipv4_netmask[4];
    uint8_t ipv4_gateway[4];
    uint8_t dns_primary[4];
    uint8_t dns_secondary[4];
} DeviceConfigV2;

typedef struct
{
    uint32_t magic;
    uint16_t storage_format;
    uint16_t record_size;
    uint32_t generation;
    DeviceConfigV2 config;
    uint8_t reserved[8];
    uint32_t crc32;
} DeviceConfigV2FlashRecord;

typedef struct
{
    uint32_t magic;
    uint16_t storage_format;
    uint16_t record_size;
    uint32_t generation;
    DeviceConfig config;
    uint8_t reserved[4];
    uint32_t crc32;
} DeviceConfigFlashRecord;

_Static_assert(sizeof(DeviceConfigLegacyFlashRecord) == 32U,
               "Legacy configuration record size changed");
_Static_assert(sizeof(DeviceConfigV2) == 40U,
               "Version 2 configuration layout changed");
_Static_assert(sizeof(DeviceConfigV2FlashRecord) == 64U,
               "Version 2 configuration record size changed");
_Static_assert(sizeof(DeviceConfig) == 44U,
               "Device configuration layout changed unexpectedly");
_Static_assert(sizeof(DeviceConfigFlashRecord) == 64U,
               "Configuration record must occupy two STM32H7 flash words");

static DeviceConfig current_config;
static uint32_t current_generation;
static uint32_t active_slot_address;

static int DeviceConfig_AddressInSector(uint32_t address, uint32_t sector_base)
{
    return (address >= sector_base) &&
           (address < (sector_base + DEVICE_CONFIG_SECTOR_SIZE));
}

static void DeviceConfig_FillDefaults(DeviceConfig *config)
{
    memset(config, 0, sizeof(*config));
    config->config_version = 0U;
    config->telemetry_frequency_seconds = 30U;
    config->diagnostics_max_message_bytes = 512U;
    config->mqtt_keep_alive_seconds = 60U;
    config->enabled_metrics = DEVICE_METRIC_TEMPERATURE |
                              DEVICE_METRIC_VOLTAGE |
                              DEVICE_METRIC_CURRENT |
                              DEVICE_METRIC_SIGNAL;
    config->log_level = DEVICE_LOG_LEVEL_INFO;
    config->allow_rollback = 0U;
    config->provisioning_state = DEVICE_PROVISIONING_UNPROVISIONED;
    config->network_addressing_mode = DEVICE_NETWORK_DHCP;
    config->network_dhcp_fallback_enabled = 1U;
    config->local_lifecycle_state = DEVICE_LOCAL_LIFECYCLE_NORMAL;
}

static void DeviceConfig_SetDefaults(void)
{
    DeviceConfig_FillDefaults(&current_config);
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

static void DeviceConfig_InvalidateCache(uint32_t address, size_t length)
{
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    {
        uintptr_t start = (uintptr_t)address &
                          ~(uintptr_t)(DEVICE_CONFIG_FLASHWORD_SIZE - 1U);
        uintptr_t end = ((uintptr_t)address + length +
                         DEVICE_CONFIG_FLASHWORD_SIZE - 1U) &
                        ~(uintptr_t)(DEVICE_CONFIG_FLASHWORD_SIZE - 1U);
        SCB_InvalidateDCache_by_Addr((uint32_t *)start,
                                    (int32_t)(end - start));
    }

    __DSB();
    __ISB();
}

static void DeviceConfig_LoadRecord(uint32_t address, DeviceConfigFlashRecord *record)
{
    DeviceConfig_InvalidateCache(address, sizeof(*record));
    memcpy(record, (const void *)address, sizeof(*record));
}

static void DeviceConfig_LoadV2Record(uint32_t address,
                                      DeviceConfigV2FlashRecord *record)
{
    DeviceConfig_InvalidateCache(address, sizeof(*record));
    memcpy(record, (const void *)address, sizeof(*record));
}

static void DeviceConfig_LoadLegacyRecord(uint32_t address,
                                          DeviceConfigLegacyFlashRecord *record)
{
    DeviceConfig_InvalidateCache(address, sizeof(*record));
    memcpy(record, (const void *)address, sizeof(*record));
}

static int DeviceConfig_MemoryIsErased(const void *memory, size_t length)
{
    const uint32_t *word = (const uint32_t *)memory;
    size_t index;

    for (index = 0U; index < (length / sizeof(*word)); index++)
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
    uint8_t flashwords[2U * DEVICE_CONFIG_FLASHWORD_SIZE]
        __attribute__((aligned(DEVICE_CONFIG_FLASHWORD_SIZE)));

    for (offset = 0U;
         offset <= (DEVICE_CONFIG_SECTOR_SIZE - sizeof(flashwords));
         offset += DEVICE_CONFIG_FLASHWORD_SIZE)
    {
        DeviceConfig_InvalidateCache(sector_base + offset,
                                     sizeof(flashwords));
        memcpy(flashwords,
               (const void *)(uintptr_t)(sector_base + offset),
               sizeof(flashwords));
        if (DeviceConfig_MemoryIsErased(flashwords, sizeof(flashwords)))
        {
            return sector_base + offset;
        }
    }
    return 0U;
}

static void DeviceConfig_LegacyRecordToConfig(
    const DeviceConfigLegacyFlashRecord *record,
    DeviceConfig *config)
{
    DeviceConfig_FillDefaults(config);
    config->config_version = record->config_version;
    config->telemetry_frequency_seconds = record->telemetry_frequency_seconds;
    config->diagnostics_max_message_bytes = record->diagnostics_max_message_bytes;
    config->mqtt_keep_alive_seconds = record->mqtt_keep_alive_seconds;
    config->enabled_metrics = record->enabled_metrics;
    config->log_level = record->log_level;
    config->allow_rollback = record->allow_rollback;
    config->provisioning_state = record->provisioning_state;
}

static int DeviceConfig_LegacyRecordIsValid(
    const DeviceConfigLegacyFlashRecord *record,
    DeviceConfig *decoded)
{
    uint32_t expected_crc;

    if ((record->magic != DEVICE_CONFIG_FLASH_MAGIC) ||
        (record->storage_format != DEVICE_CONFIG_LEGACY_FORMAT) ||
        (record->record_size != sizeof(DeviceConfigLegacyFlashRecord)))
    {
        return 0;
    }

    expected_crc = DeviceConfig_Crc32((const uint8_t *)record,
                                      offsetof(DeviceConfigLegacyFlashRecord, crc32));
    if (expected_crc != record->crc32)
    {
        return 0;
    }

    DeviceConfig_LegacyRecordToConfig(record, decoded);
    return DeviceConfig_IsValid(decoded);
}

static void DeviceConfig_V2ToConfig(const DeviceConfigV2 *source,
                                    DeviceConfig *destination)
{
    DeviceConfig_FillDefaults(destination);
    destination->config_version = source->config_version;
    destination->telemetry_frequency_seconds =
        source->telemetry_frequency_seconds;
    destination->diagnostics_max_message_bytes =
        source->diagnostics_max_message_bytes;
    destination->mqtt_keep_alive_seconds = source->mqtt_keep_alive_seconds;
    destination->enabled_metrics = (uint32_t)source->enabled_metrics;
    destination->log_level = source->log_level;
    destination->allow_rollback = source->allow_rollback;
    destination->provisioning_state = source->provisioning_state;
    destination->network_addressing_mode = source->network_addressing_mode;
    destination->network_dhcp_fallback_enabled =
        source->network_dhcp_fallback_enabled;
    destination->local_lifecycle_state = source->local_lifecycle_state;
    memcpy(destination->ipv4_address, source->ipv4_address,
           sizeof(destination->ipv4_address));
    memcpy(destination->ipv4_netmask, source->ipv4_netmask,
           sizeof(destination->ipv4_netmask));
    memcpy(destination->ipv4_gateway, source->ipv4_gateway,
           sizeof(destination->ipv4_gateway));
    memcpy(destination->dns_primary, source->dns_primary,
           sizeof(destination->dns_primary));
    memcpy(destination->dns_secondary, source->dns_secondary,
           sizeof(destination->dns_secondary));
}

static int DeviceConfig_V2RecordIsValid(const DeviceConfigV2FlashRecord *record,
                                        DeviceConfig *decoded)
{
    uint32_t expected_crc;

    if ((record->magic != DEVICE_CONFIG_FLASH_MAGIC) ||
        (record->storage_format != DEVICE_CONFIG_V2_FORMAT) ||
        (record->record_size != sizeof(DeviceConfigV2FlashRecord)))
    {
        return 0;
    }

    expected_crc = DeviceConfig_Crc32(
        (const uint8_t *)record,
        offsetof(DeviceConfigV2FlashRecord, crc32));
    if (expected_crc != record->crc32)
    {
        return 0;
    }

    DeviceConfig_V2ToConfig(&record->config, decoded);
    return DeviceConfig_IsValid(decoded);
}

static int DeviceConfig_RecordIsValid(const DeviceConfigFlashRecord *record)
{
    uint32_t expected_crc;

    if ((record->magic != DEVICE_CONFIG_FLASH_MAGIC) ||
        (record->storage_format != DEVICE_CONFIG_STORAGE_FORMAT) ||
        (record->record_size != sizeof(DeviceConfigFlashRecord)))
    {
        return 0;
    }

    expected_crc = DeviceConfig_Crc32((const uint8_t *)record,
                                      offsetof(DeviceConfigFlashRecord, crc32));
    return (expected_crc == record->crc32) &&
           DeviceConfig_IsValid(&record->config);
}

static void DeviceConfig_PrepareRecord(const DeviceConfig *config,
                                       DeviceConfigFlashRecord *record)
{
    memset(record, 0, sizeof(*record));
    record->magic = DEVICE_CONFIG_FLASH_MAGIC;
    record->storage_format = DEVICE_CONFIG_STORAGE_FORMAT;
    record->record_size = sizeof(*record);
    record->generation = current_generation + 1U;
    record->config = *config;
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
        uint32_t offset;

        DeviceConfig_InvalidateCache(target_address, sizeof(record));
        for (offset = 0U;
             (offset < sizeof(record)) && (status == HAL_OK);
             offset += DEVICE_CONFIG_FLASHWORD_SIZE)
        {
            status = HAL_FLASH_Program(
                FLASH_TYPEPROGRAM_FLASHWORD,
                target_address + offset,
                (uint32_t)(uintptr_t)((const uint8_t *)&record + offset));
        }
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
    DeviceConfigV2FlashRecord v2_record;
    DeviceConfigLegacyFlashRecord legacy;
    DeviceConfig selected_config;
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
             offset += DEVICE_CONFIG_FLASHWORD_SIZE)
        {
            uint32_t address = sector_bases[sector_index] + offset;

            DeviceConfig_LoadLegacyRecord(address, &legacy);
            if (DeviceConfig_MemoryIsErased(&legacy, sizeof(legacy)))
            {
                break;
            }

            if ((legacy.magic == DEVICE_CONFIG_FLASH_MAGIC) &&
                (legacy.storage_format == DEVICE_CONFIG_STORAGE_FORMAT) &&
                (legacy.record_size == sizeof(DeviceConfigFlashRecord)) &&
                (offset <= (DEVICE_CONFIG_SECTOR_SIZE -
                            sizeof(DeviceConfigFlashRecord))))
            {
                DeviceConfig_LoadRecord(address, &record);
                if (DeviceConfig_RecordIsValid(&record) &&
                    (!selected_valid ||
                     (record.generation > current_generation)))
                {
                    selected_config = record.config;
                    current_generation = record.generation;
                    selected_valid = 1;
                    active_slot_address = address;
                }
                offset += DEVICE_CONFIG_FLASHWORD_SIZE;
            }
            else if ((legacy.magic == DEVICE_CONFIG_FLASH_MAGIC) &&
                     (legacy.storage_format == DEVICE_CONFIG_V2_FORMAT) &&
                     (legacy.record_size == sizeof(DeviceConfigV2FlashRecord)) &&
                     (offset <= (DEVICE_CONFIG_SECTOR_SIZE -
                                 sizeof(DeviceConfigV2FlashRecord))))
            {
                DeviceConfig v2_config;

                DeviceConfig_LoadV2Record(address, &v2_record);
                if (DeviceConfig_V2RecordIsValid(&v2_record, &v2_config) &&
                    (!selected_valid ||
                     (v2_record.generation > current_generation)))
                {
                    selected_config = v2_config;
                    current_generation = v2_record.generation;
                    selected_valid = 1;
                    active_slot_address = address;
                }
                offset += DEVICE_CONFIG_FLASHWORD_SIZE;
            }
            else
            {
                DeviceConfig legacy_config;

                if (DeviceConfig_LegacyRecordIsValid(&legacy,
                                                     &legacy_config) &&
                    (!selected_valid ||
                     (legacy.generation > current_generation)))
                {
                    selected_config = legacy_config;
                    current_generation = legacy.generation;
                    selected_valid = 1;
                    active_slot_address = address;
                }
            }
        }
    }

    if (selected_valid)
    {
        current_config = selected_config;
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

static uint32_t DeviceConfig_Ipv4ToU32(const uint8_t address[4])
{
    return ((uint32_t)address[0] << 24U) |
           ((uint32_t)address[1] << 16U) |
           ((uint32_t)address[2] << 8U) |
           (uint32_t)address[3];
}

static int DeviceConfig_StaticNetworkIsValid(const DeviceConfig *config)
{
    uint32_t address = DeviceConfig_Ipv4ToU32(config->ipv4_address);
    uint32_t netmask = DeviceConfig_Ipv4ToU32(config->ipv4_netmask);
    uint32_t gateway = DeviceConfig_Ipv4ToU32(config->ipv4_gateway);
    uint32_t primary_dns = DeviceConfig_Ipv4ToU32(config->dns_primary);
    uint32_t inverted_mask = ~netmask;

    if ((address == 0U) || (gateway == 0U) || (primary_dns == 0U) ||
        (netmask == 0U) || (netmask == 0xFFFFFFFFUL) ||
        ((inverted_mask & (inverted_mask + 1U)) != 0U) ||
        (config->ipv4_address[0] == 0U) ||
        (config->ipv4_address[0] == 127U) ||
        (config->ipv4_address[0] >= 224U) ||
        ((address & netmask) != (gateway & netmask)) ||
        ((address & inverted_mask) == 0U) ||
        ((address & inverted_mask) == inverted_mask))
    {
        return 0;
    }

    return 1;
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
        (config->log_level > DEVICE_LOG_LEVEL_TRACE) ||
        (config->allow_rollback > 1U) ||
        (config->provisioning_state > DEVICE_PROVISIONING_PROVISIONED) ||
        (config->network_addressing_mode > DEVICE_NETWORK_STATIC) ||
        (config->network_dhcp_fallback_enabled > 1U) ||
        (config->local_lifecycle_state >
         DEVICE_LOCAL_LIFECYCLE_DECOMMISSIONED) ||
        ((config->network_addressing_mode == DEVICE_NETWORK_STATIC) &&
         !DeviceConfig_StaticNetworkIsValid(config)))
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
           (left->provisioning_state == right->provisioning_state) &&
           (left->network_addressing_mode ==
            right->network_addressing_mode) &&
           (left->network_dhcp_fallback_enabled ==
            right->network_dhcp_fallback_enabled) &&
           (left->local_lifecycle_state == right->local_lifecycle_state) &&
           (memcmp(left->ipv4_address,
                   right->ipv4_address,
                   sizeof(left->ipv4_address)) == 0) &&
           (memcmp(left->ipv4_netmask,
                   right->ipv4_netmask,
                   sizeof(left->ipv4_netmask)) == 0) &&
           (memcmp(left->ipv4_gateway,
                   right->ipv4_gateway,
                   sizeof(left->ipv4_gateway)) == 0) &&
           (memcmp(left->dns_primary,
                   right->dns_primary,
                   sizeof(left->dns_primary)) == 0) &&
           (memcmp(left->dns_secondary,
                   right->dns_secondary,
                   sizeof(left->dns_secondary)) == 0);
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

    if (current_config.local_lifecycle_state !=
        DEVICE_LOCAL_LIFECYCLE_NORMAL)
    {
        return -1;
    }

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

const char *DeviceConfig_NetworkModeName(uint8_t mode)
{
    return mode == DEVICE_NETWORK_STATIC ? "STATIC" : "DHCP";
}

const char *DeviceConfig_LocalLifecycleStateName(uint8_t state)
{
    switch ((DeviceLocalLifecycleState)state)
    {
        case DEVICE_LOCAL_LIFECYCLE_FACTORY_RESET_PENDING:
            return "FACTORY_RESET_PENDING";
        case DEVICE_LOCAL_LIFECYCLE_DECOMMISSION_PENDING:
            return "DECOMMISSION_PENDING";
        case DEVICE_LOCAL_LIFECYCLE_DECOMMISSIONED:
            return "DECOMMISSIONED";
        case DEVICE_LOCAL_LIFECYCLE_NORMAL:
        default:
            return "NORMAL";
    }
}

static int DeviceConfig_RequestLifecycleState(
    DeviceLocalLifecycleState state)
{
    DeviceConfig candidate = current_config;

    if ((current_config.local_lifecycle_state !=
         DEVICE_LOCAL_LIFECYCLE_NORMAL) ||
        ((state != DEVICE_LOCAL_LIFECYCLE_FACTORY_RESET_PENDING) &&
         (state != DEVICE_LOCAL_LIFECYCLE_DECOMMISSION_PENDING)))
    {
        return -1;
    }

    candidate.local_lifecycle_state = (uint8_t)state;
    if (DeviceConfig_Persist(&candidate) != 0)
    {
        return -1;
    }

    taskENTER_CRITICAL();
    current_config = candidate;
    taskEXIT_CRITICAL();
    return 0;
}

int DeviceConfig_RequestFactoryReset(void)
{
    return DeviceConfig_RequestLifecycleState(
        DEVICE_LOCAL_LIFECYCLE_FACTORY_RESET_PENDING);
}

int DeviceConfig_RequestDecommission(void)
{
    return DeviceConfig_RequestLifecycleState(
        DEVICE_LOCAL_LIFECYCLE_DECOMMISSION_PENDING);
}

static int DeviceConfig_EraseStorage(void)
{
    FLASH_EraseInitTypeDef erase = { 0 };
    uint32_t sector_error = 0xFFFFFFFFUL;
    HAL_StatusTypeDef status;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_2;
    erase.Sector = DEVICE_CONFIG_SLOT_A_SECTOR;
    erase.NbSectors = 2U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return -1;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK2);
    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    (void)HAL_FLASH_Lock();
    if ((status != HAL_OK) || (sector_error != 0xFFFFFFFFUL))
    {
        return -1;
    }

    DeviceConfig_InvalidateCache(DEVICE_CONFIG_SLOT_A_ADDRESS,
                                 2U * DEVICE_CONFIG_SECTOR_SIZE);
    DeviceConfig_SetDefaults();
    return 0;
}

int DeviceConfig_CompletePendingLifecycleAction(void)
{
    DeviceLocalLifecycleState pending =
        (DeviceLocalLifecycleState)current_config.local_lifecycle_state;

    if ((pending != DEVICE_LOCAL_LIFECYCLE_FACTORY_RESET_PENDING) &&
        (pending != DEVICE_LOCAL_LIFECYCLE_DECOMMISSION_PENDING))
    {
        return 0;
    }

    if (DeviceConfig_EraseStorage() != 0)
    {
        return -1;
    }

    if (pending == DEVICE_LOCAL_LIFECYCLE_DECOMMISSION_PENDING)
    {
        DeviceConfig decommissioned = current_config;

        decommissioned.local_lifecycle_state =
            DEVICE_LOCAL_LIFECYCLE_DECOMMISSIONED;
        if (DeviceConfig_Persist(&decommissioned) != 0)
        {
            return -1;
        }
        current_config = decommissioned;
    }

    return 0;
}

int DeviceConfig_IsCloudAccessAllowed(void)
{
    return current_config.local_lifecycle_state !=
           DEVICE_LOCAL_LIFECYCLE_DECOMMISSIONED;
}
