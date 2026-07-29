#include "ota_control.h"

#include "main.h"
#include "cmsis_os.h"

#include <string.h>

#define OTA_CONTROL_RECORD_COUNT \
    (OTA_CONTROL_LOG_SIZE / OTA_CONTROL_RECORD_SIZE)

static void OtaControl_InvalidateCache(uint32_t address, size_t length)
{
    uintptr_t aligned_start;
    uintptr_t aligned_end;

    if ((length == 0U) || ((SCB->CCR & SCB_CCR_DC_Msk) == 0U))
    {
        return;
    }

    aligned_start =
        (uintptr_t)address & ~(uintptr_t)(OTA_FLASHWORD_SIZE - 1U);
    aligned_end =
        ((uintptr_t)address + length + OTA_FLASHWORD_SIZE - 1U) &
        ~(uintptr_t)(OTA_FLASHWORD_SIZE - 1U);
    SCB_InvalidateDCache_by_Addr((uint32_t *)aligned_start,
                                (int32_t)(aligned_end - aligned_start));
    __DSB();
    __ISB();
}

uint32_t OtaControl_Crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    size_t index;

    if ((data == NULL) && (length != 0U))
    {
        return 0U;
    }

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

int OtaControl_ImageDescriptorIsValid(const OtaImageDescriptor *descriptor)
{
    uint32_t index;
    uint8_t digest_nonzero = 0U;
    uint8_t signature_nonzero = 0U;

    if ((descriptor == NULL) ||
        (descriptor->magic != OTA_IMAGE_DESCRIPTOR_MAGIC) ||
        (descriptor->image_size < 8U) ||
        (descriptor->image_size > OTA_SLOT_SIZE) ||
        (descriptor->image_version == 0U))
    {
        return 0;
    }

    for (index = 0U; index < OTA_SHA256_SIZE; index++)
    {
        digest_nonzero |= descriptor->sha256[index];
    }
    for (index = 0U; index < OTA_P256_SIGNATURE_SIZE; index++)
    {
        signature_nonzero |= descriptor->signature[index];
    }
    return (digest_nonzero != 0U) && (signature_nonzero != 0U);
}

static int OtaControl_RecordStateIsValid(uint32_t state)
{
    return (state >= OTA_CONTROL_STATE_CONFIRMED) &&
           (state <= OTA_CONTROL_STATE_REJECTED);
}

int OtaControl_RecordIsValid(const OtaControlRecord *record)
{
    if ((record == NULL) ||
        (record->magic != OTA_CONTROL_MAGIC) ||
        (record->storage_format != OTA_CONTROL_FORMAT) ||
        (record->record_size != sizeof(*record)) ||
        (record->sequence == 0U) ||
        !OtaControl_RecordStateIsValid(record->state) ||
        (record->committed != OTA_CONTROL_COMMITTED))
    {
        return 0;
    }

    return record->crc32 ==
           OtaControl_Crc32((const uint8_t *)record,
                            offsetof(OtaControlRecord, crc32));
}

static int OtaControl_RecordIsErased(uint32_t address)
{
    const uint32_t *words = (const uint32_t *)(uintptr_t)address;
    size_t index;

    for (index = 0U; index < (OTA_CONTROL_RECORD_SIZE / sizeof(uint32_t));
         index++)
    {
        if (words[index] != 0xFFFFFFFFUL)
        {
            return 0;
        }
    }
    return 1;
}

static uint32_t OtaControl_FreeRecordCount(void)
{
    uint32_t free_records = 0U;
    uint32_t index;

    OtaControl_InvalidateCache(OTA_CONTROL_LOG_ADDRESS,
                               OTA_CONTROL_LOG_SIZE);
    for (index = 0U; index < OTA_CONTROL_RECORD_COUNT; index++)
    {
        uint32_t address =
            OTA_CONTROL_LOG_ADDRESS + (index * OTA_CONTROL_RECORD_SIZE);
        if (OtaControl_RecordIsErased(address))
        {
            free_records++;
        }
    }
    return free_records;
}

OtaControlResult OtaControl_LoadLatest(OtaControlRecord *record)
{
    OtaControlRecord candidate;
    uint32_t index;
    uint8_t found = 0U;

    if (record == NULL)
    {
        return OTA_CONTROL_INVALID_ARGUMENT;
    }

    memset(record, 0, sizeof(*record));
    OtaControl_InvalidateCache(OTA_CONTROL_LOG_ADDRESS,
                               OTA_CONTROL_LOG_SIZE);

    for (index = 0U; index < OTA_CONTROL_RECORD_COUNT; index++)
    {
        uint32_t address =
            OTA_CONTROL_LOG_ADDRESS + (index * OTA_CONTROL_RECORD_SIZE);

        if (OtaControl_RecordIsErased(address))
        {
            continue;
        }

        memcpy(&candidate,
               (const void *)(uintptr_t)address,
               sizeof(candidate));
        if (!OtaControl_RecordIsValid(&candidate))
        {
            continue;
        }

        if ((found == 0U) ||
            ((int32_t)(candidate.sequence - record->sequence) > 0))
        {
            *record = candidate;
            found = 1U;
        }
    }

    if (found == 0U)
    {
        OtaControl_InvalidateCache(OTA_SCRATCH_ADDRESS,
                                   OTA_CONTROL_RECORD_SIZE);
        memcpy(&candidate,
               (const void *)(uintptr_t)OTA_SCRATCH_ADDRESS,
               sizeof(candidate));
        if (OtaControl_RecordIsValid(&candidate) &&
            ((candidate.state == OTA_CONTROL_STATE_CONFIRMED) ||
             (candidate.state == OTA_CONTROL_STATE_SERVICE_FILE_READY) ||
             (candidate.state == OTA_CONTROL_STATE_REJECTED)))
        {
            *record = candidate;
            found = 1U;
        }
    }

    return found != 0U ? OTA_CONTROL_OK : OTA_CONTROL_NOT_FOUND;
}

static OtaControlResult OtaControl_ProgramFlashword(
    uint32_t address,
    const uint8_t *source)
{
    HAL_StatusTypeDef status;

    if (((address % OTA_FLASHWORD_SIZE) != 0U) ||
        (source == NULL) ||
        (((uintptr_t)source % OTA_FLASHWORD_SIZE) != 0U))
    {
        return OTA_CONTROL_INVALID_ARGUMENT;
    }

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return OTA_CONTROL_FLASH_ERROR;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK2);
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                               address,
                               (uint32_t)(uintptr_t)source);
    (void)HAL_FLASH_Lock();
    if (status != HAL_OK)
    {
        return OTA_CONTROL_FLASH_ERROR;
    }

    OtaControl_InvalidateCache(address, OTA_FLASHWORD_SIZE);
    return memcmp((const void *)(uintptr_t)address,
                  source,
                  OTA_FLASHWORD_SIZE) == 0
               ? OTA_CONTROL_OK
               : OTA_CONTROL_FLASH_ERROR;
}

static OtaControlResult OtaControl_EraseBank2Sector(uint32_t sector)
{
    FLASH_EraseInitTypeDef erase = { 0 };
    uint32_t sector_error = 0xFFFFFFFFUL;
    HAL_StatusTypeDef status;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_2;
    erase.Sector = sector;
    erase.NbSectors = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return OTA_CONTROL_FLASH_ERROR;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK2);
    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    (void)HAL_FLASH_Lock();
    if ((status != HAL_OK) || (sector_error != 0xFFFFFFFFUL))
    {
        return OTA_CONTROL_FLASH_ERROR;
    }
    return OTA_CONTROL_OK;
}

static OtaControlResult OtaControl_ProgramRawRecord(
    uint32_t address,
    const OtaControlRecord *record)
{
    uint32_t offset;

    if ((record == NULL) ||
        ((address % OTA_FLASHWORD_SIZE) != 0U) ||
        (((uintptr_t)record % OTA_FLASHWORD_SIZE) != 0U))
    {
        return OTA_CONTROL_INVALID_ARGUMENT;
    }

    for (offset = 0U;
         offset < OTA_CONTROL_RECORD_SIZE;
         offset += OTA_FLASHWORD_SIZE)
    {
        OtaControlResult result =
            OtaControl_ProgramFlashword(address + offset,
                                        ((const uint8_t *)record) + offset);
        if (result != OTA_CONTROL_OK)
        {
            return result;
        }
        osThreadYield();
    }
    return OTA_CONTROL_OK;
}

OtaControlResult OtaControl_EnsureCapacity(uint32_t required_records)
{
    OtaControlRecord latest
        __attribute__((aligned(OTA_FLASHWORD_SIZE)));
    OtaControlRecord backup;
    OtaControlResult latest_result;

    if ((required_records == 0U) ||
        (required_records >= OTA_CONTROL_RECORD_COUNT))
    {
        return OTA_CONTROL_INVALID_ARGUMENT;
    }
    if (OtaControl_FreeRecordCount() >= required_records)
    {
        return OTA_CONTROL_OK;
    }

    latest_result = OtaControl_LoadLatest(&latest);
    if ((latest_result != OTA_CONTROL_OK) &&
        (latest_result != OTA_CONTROL_NOT_FOUND))
    {
        return latest_result;
    }
    if ((latest_result == OTA_CONTROL_OK) &&
        (latest.state != OTA_CONTROL_STATE_CONFIRMED) &&
        (latest.state != OTA_CONTROL_STATE_SERVICE_FILE_READY) &&
        (latest.state != OTA_CONTROL_STATE_REJECTED))
    {
        return OTA_CONTROL_LOG_FULL;
    }

    if (latest_result == OTA_CONTROL_OK)
    {
        if (OtaControl_EraseBank2Sector(OTA_SCRATCH_SECTOR) !=
            OTA_CONTROL_OK)
        {
            return OTA_CONTROL_FLASH_ERROR;
        }
        if (OtaControl_ProgramRawRecord(OTA_SCRATCH_ADDRESS, &latest) !=
            OTA_CONTROL_OK)
        {
            return OTA_CONTROL_FLASH_ERROR;
        }
        OtaControl_InvalidateCache(OTA_SCRATCH_ADDRESS, sizeof(backup));
        memcpy(&backup,
               (const void *)(uintptr_t)OTA_SCRATCH_ADDRESS,
               sizeof(backup));
        if (!OtaControl_RecordIsValid(&backup) ||
            (memcmp(&backup, &latest, sizeof(backup)) != 0))
        {
            return OTA_CONTROL_CORRUPT;
        }
    }

    if (OtaControl_EraseBank2Sector(OTA_CONTROL_LOG_SECTOR) !=
        OTA_CONTROL_OK)
    {
        return OTA_CONTROL_FLASH_ERROR;
    }
    OtaControl_InvalidateCache(OTA_CONTROL_LOG_ADDRESS,
                               OTA_CONTROL_LOG_SIZE);
    if (OtaControl_FreeRecordCount() != OTA_CONTROL_RECORD_COUNT)
    {
        return OTA_CONTROL_FLASH_ERROR;
    }

    if (latest_result == OTA_CONTROL_OK)
    {
        return OtaControl_Append(&latest);
    }
    return OTA_CONTROL_OK;
}

OtaControlResult OtaControl_Append(OtaControlRecord *record)
{
    OtaControlRecord latest;
    OtaControlRecord write_record
        __attribute__((aligned(OTA_FLASHWORD_SIZE)));
    OtaControlRecord verify_record;
    OtaControlResult latest_result;
    uint32_t target_address = 0U;
    uint32_t index;
    uint32_t offset;

    if ((record == NULL) ||
        !OtaControl_RecordStateIsValid(record->state))
    {
        return OTA_CONTROL_INVALID_ARGUMENT;
    }

    latest_result = OtaControl_LoadLatest(&latest);
    if ((latest_result != OTA_CONTROL_OK) &&
        (latest_result != OTA_CONTROL_NOT_FOUND))
    {
        return latest_result;
    }

    OtaControl_InvalidateCache(OTA_CONTROL_LOG_ADDRESS,
                               OTA_CONTROL_LOG_SIZE);
    for (index = 0U; index < OTA_CONTROL_RECORD_COUNT; index++)
    {
        uint32_t address =
            OTA_CONTROL_LOG_ADDRESS + (index * OTA_CONTROL_RECORD_SIZE);
        if (OtaControl_RecordIsErased(address))
        {
            target_address = address;
            break;
        }
    }
    if (target_address == 0U)
    {
        return OTA_CONTROL_LOG_FULL;
    }

    write_record = *record;
    write_record.magic = OTA_CONTROL_MAGIC;
    write_record.storage_format = OTA_CONTROL_FORMAT;
    write_record.record_size = sizeof(write_record);
    write_record.sequence =
        latest_result == OTA_CONTROL_OK ? latest.sequence + 1U : 1U;
    write_record.crc32 =
        OtaControl_Crc32((const uint8_t *)&write_record,
                         offsetof(OtaControlRecord, crc32));
    write_record.committed = OTA_CONTROL_COMMITTED;

    /*
     * The final Flash word contains both CRC and commit marker. It is written
     * last, so a power loss can only leave an ignored, uncommitted record.
     */
    for (offset = 0U;
         offset < (OTA_CONTROL_RECORD_SIZE - OTA_FLASHWORD_SIZE);
         offset += OTA_FLASHWORD_SIZE)
    {
        OtaControlResult result =
            OtaControl_ProgramFlashword(target_address + offset,
                                        ((const uint8_t *)&write_record) +
                                            offset);
        if (result != OTA_CONTROL_OK)
        {
            return result;
        }
        osThreadYield();
    }

    if (OtaControl_ProgramFlashword(
            target_address + OTA_CONTROL_RECORD_SIZE - OTA_FLASHWORD_SIZE,
            ((const uint8_t *)&write_record) +
                OTA_CONTROL_RECORD_SIZE - OTA_FLASHWORD_SIZE) !=
        OTA_CONTROL_OK)
    {
        return OTA_CONTROL_FLASH_ERROR;
    }

    OtaControl_InvalidateCache(target_address, sizeof(verify_record));
    memcpy(&verify_record,
           (const void *)(uintptr_t)target_address,
           sizeof(verify_record));
    if (!OtaControl_RecordIsValid(&verify_record) ||
        (memcmp(&verify_record, &write_record, sizeof(write_record)) != 0))
    {
        return OTA_CONTROL_CORRUPT;
    }

    *record = write_record;
    return OTA_CONTROL_OK;
}

const char *OtaControl_StateName(uint32_t state)
{
    switch (state)
    {
        case OTA_CONTROL_STATE_CONFIRMED:
            return "CONFIRMED";
        case OTA_CONTROL_STATE_DOWNLOAD_IN_PROGRESS:
            return "DOWNLOAD_IN_PROGRESS";
        case OTA_CONTROL_STATE_SERVICE_FILE_READY:
            return "SERVICE_FILE_READY";
        case OTA_CONTROL_STATE_CANDIDATE_READY:
            return "CANDIDATE_READY";
        case OTA_CONTROL_STATE_SWAPPING:
            return "SWAPPING";
        case OTA_CONTROL_STATE_TRIAL:
            return "TRIAL";
        case OTA_CONTROL_STATE_ROLLBACK:
            return "ROLLBACK";
        case OTA_CONTROL_STATE_REJECTED:
            return "REJECTED";
        default:
            return "UNKNOWN";
    }
}
