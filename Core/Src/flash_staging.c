#include "flash_staging.h"
#include "mbedtls_compat.h"

#include "main.h"
#include "ota_image.h"

#include "mbedtls/sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FLASH_STAGING_FIRST_SECTOR       FLASH_SECTOR_1
#define FLASH_STAGING_SECTOR_COUNT       OTA_SLOT_SECTOR_COUNT

extern uint8_t __flash_staging_data_start__[];
extern uint8_t __flash_staging_data_end__[];

static FlashStagingInfo staging_info;
static FlashStagingRequest staging_request;
static uint32_t staging_prepare_sector;
static uint32_t staging_prepare_sector_count;
static uint32_t staging_programmed_bytes;
static size_t staging_write_buffer_used;
static uint8_t staging_write_buffer[OTA_FLASHWORD_SIZE]
    __attribute__((aligned(OTA_FLASHWORD_SIZE)));

static uint32_t FlashStaging_DataAddress(void)
{
    return (uint32_t)(uintptr_t)__flash_staging_data_start__;
}

static uint32_t FlashStaging_DataCapacity(void)
{
    return (uint32_t)((uintptr_t)__flash_staging_data_end__ -
                      (uintptr_t)__flash_staging_data_start__);
}

static void FlashStaging_InvalidateCache(uint32_t address, size_t length)
{
    uintptr_t start;
    uintptr_t end;

    if ((length == 0U) || ((SCB->CCR & SCB_CCR_DC_Msk) == 0U))
    {
        return;
    }
    start = (uintptr_t)address & ~(uintptr_t)(OTA_FLASHWORD_SIZE - 1U);
    end = ((uintptr_t)address + length + OTA_FLASHWORD_SIZE - 1U) &
          ~(uintptr_t)(OTA_FLASHWORD_SIZE - 1U);
    SCB_InvalidateDCache_by_Addr((uint32_t *)start,
                                (int32_t)(end - start));
    __DSB();
    __ISB();
}

static void FlashStaging_ResetRuntime(FlashStagingState state)
{
    memset(&staging_info, 0, sizeof(staging_info));
    memset(&staging_request, 0, sizeof(staging_request));
    staging_info.state = state;
    staging_info.capacity_bytes = FlashStaging_DataCapacity();
    staging_prepare_sector = 0U;
    staging_prepare_sector_count = 0U;
    staging_programmed_bytes = 0U;
    staging_write_buffer_used = 0U;
    memset(staging_write_buffer, 0xFF, sizeof(staging_write_buffer));
}

static FlashStagingResult FlashStaging_Fail(FlashStagingResult result)
{
    staging_info.state = FLASH_STAGING_STATE_INVALID;
    return result;
}

static FlashStagingResult FlashStaging_EraseSector(uint32_t sector)
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
        return FLASH_STAGING_FLASH_ERROR;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK2);
    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    (void)HAL_FLASH_Lock();
    return ((status == HAL_OK) && (sector_error == 0xFFFFFFFFUL))
               ? FLASH_STAGING_OK
               : FLASH_STAGING_FLASH_ERROR;
}

static int FlashStaging_RegionIsErased(uint32_t address, size_t length)
{
    const uint32_t *word;
    const uint32_t *end;

    FlashStaging_InvalidateCache(address, length);
    word = (const uint32_t *)(uintptr_t)address;
    end = (const uint32_t *)(uintptr_t)(address + length);
    while (word < end)
    {
        if (*word++ != 0xFFFFFFFFUL)
        {
            return 0;
        }
    }
    return 1;
}

static FlashStagingResult FlashStaging_ProgramFlashword(
    uint32_t address,
    const uint8_t *data)
{
    HAL_StatusTypeDef status;

    if (((address % OTA_FLASHWORD_SIZE) != 0U) ||
        (data == NULL) ||
        (((uintptr_t)data % OTA_FLASHWORD_SIZE) != 0U))
    {
        return FLASH_STAGING_INVALID_ARGUMENT;
    }
    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return FLASH_STAGING_FLASH_ERROR;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK2);
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                               address,
                               (uint32_t)(uintptr_t)data);
    (void)HAL_FLASH_Lock();
    if (status != HAL_OK)
    {
        return FLASH_STAGING_FLASH_ERROR;
    }

    FlashStaging_InvalidateCache(address, OTA_FLASHWORD_SIZE);
    return memcmp((const void *)(uintptr_t)address,
                  data,
                  OTA_FLASHWORD_SIZE) == 0
               ? FLASH_STAGING_OK
               : FLASH_STAGING_FLASH_ERROR;
}

static FlashStagingResult FlashStaging_CalculateSha256(
    uint32_t size,
    uint8_t output[FLASH_STAGING_SHA256_SIZE])
{
    mbedtls_sha256_context context;
    const uint8_t *data =
        (const uint8_t *)(uintptr_t)FlashStaging_DataAddress();
    uint32_t offset = 0U;
    int result;

    FlashStaging_InvalidateCache(FlashStaging_DataAddress(), size);
    mbedtls_sha256_init(&context);
    result = MbedtlsCompat_Sha256Starts(&context, 0);
    while ((result == 0) && (offset < size))
    {
        size_t chunk = size - offset;
        if (chunk > 2048U)
        {
            chunk = 2048U;
        }
        result = MbedtlsCompat_Sha256Update(&context,
                                           data + offset,
                                           chunk);
        offset += (uint32_t)chunk;
    }
    if (result == 0)
    {
        result = MbedtlsCompat_Sha256Finish(&context, output);
    }
    mbedtls_sha256_free(&context);
    return result == 0 ? FLASH_STAGING_OK : FLASH_STAGING_CRYPTO_ERROR;
}

static int FlashStaging_ServiceRecordIsUsable(
    const OtaControlRecord *record)
{
    return (record->state == OTA_CONTROL_STATE_SERVICE_FILE_READY) &&
           (record->secondary.image_size > 0U) &&
           (record->secondary.image_size <= FlashStaging_DataCapacity());
}

FlashStagingResult FlashStaging_Init(void)
{
    OtaControlRecord record;
    OtaControlResult control_result;
    OtaImageDescriptor verified;

    FlashStaging_ResetRuntime(FLASH_STAGING_STATE_EMPTY);
    control_result = OtaControl_LoadLatest(&record);
    if (control_result == OTA_CONTROL_NOT_FOUND)
    {
        return FLASH_STAGING_OK;
    }
    if (control_result != OTA_CONTROL_OK)
    {
        return FlashStaging_Fail(FLASH_STAGING_METADATA_ERROR);
    }

    if (FlashStaging_ServiceRecordIsUsable(&record))
    {
        staging_info.state = FLASH_STAGING_STATE_READY;
        staging_info.artifact_type = FLASH_STAGING_ARTIFACT_SERVICE_FILE;
        staging_info.file_size_bytes = record.secondary.image_size;
        staging_info.bytes_received = record.secondary.image_size;
        memcpy(staging_info.expected_sha256,
               record.secondary.sha256,
               FLASH_STAGING_SHA256_SIZE);
        return FlashStaging_Verify();
    }

    if (record.state == OTA_CONTROL_STATE_CANDIDATE_READY)
    {
        OtaImageResult image_result =
            OtaImage_VerifySlot(OTA_SECONDARY_SLOT_ADDRESS, &verified);
        if ((image_result != OTA_IMAGE_OK) ||
            (memcmp(&verified,
                    &record.secondary,
                    sizeof(verified)) != 0))
        {
            return FlashStaging_Fail(FLASH_STAGING_INTEGRITY_ERROR);
        }
        staging_info.state = FLASH_STAGING_STATE_READY;
        staging_info.artifact_type = FLASH_STAGING_ARTIFACT_FIRMWARE;
        staging_info.image_version = verified.image_version;
        staging_info.file_size_bytes =
            OTA_IMAGE_HEADER_SIZE + verified.image_size;
        staging_info.bytes_received = staging_info.file_size_bytes;
        memcpy(staging_info.expected_sha256,
               verified.sha256,
               FLASH_STAGING_SHA256_SIZE);
    }
    return FLASH_STAGING_OK;
}

FlashStagingResult FlashStaging_Begin(
    uint32_t file_size_bytes,
    const uint8_t expected_sha256[FLASH_STAGING_SHA256_SIZE])
{
    FlashStagingResult result =
        FlashStaging_PrepareStart(file_size_bytes, expected_sha256);
    while (result == FLASH_STAGING_IN_PROGRESS)
    {
        result = FlashStaging_PrepareStep();
    }
    return result;
}

FlashStagingResult FlashStaging_PrepareStart(
    uint32_t file_size_bytes,
    const uint8_t expected_sha256[FLASH_STAGING_SHA256_SIZE])
{
    FlashStagingRequest request;

    if (expected_sha256 == NULL)
    {
        return FLASH_STAGING_INVALID_ARGUMENT;
    }
    memset(&request, 0, sizeof(request));
    request.type = FLASH_STAGING_ARTIFACT_SERVICE_FILE;
    request.file_size_bytes = file_size_bytes;
    memcpy(request.expected_sha256,
           expected_sha256,
           sizeof(request.expected_sha256));
    return FlashStaging_PrepareArtifact(&request);
}

FlashStagingResult FlashStaging_PrepareArtifact(
    const FlashStagingRequest *request)
{
    OtaControlRecord record;
    OtaControlResult control_result;

    if ((request == NULL) ||
        ((request->type != FLASH_STAGING_ARTIFACT_SERVICE_FILE) &&
         (request->type != FLASH_STAGING_ARTIFACT_FIRMWARE)) ||
        (request->file_size_bytes == 0U) ||
        (request->file_size_bytes > FlashStaging_DataCapacity()))
    {
        return FLASH_STAGING_INVALID_ARGUMENT;
    }
    if ((staging_info.state == FLASH_STAGING_STATE_PREPARING) ||
        (staging_info.state == FLASH_STAGING_STATE_RECEIVING))
    {
        return FLASH_STAGING_INVALID_STATE;
    }

    memset(&record, 0, sizeof(record));
    control_result = OtaControl_LoadLatest(&record);
    if ((control_result != OTA_CONTROL_OK) &&
        (control_result != OTA_CONTROL_NOT_FOUND))
    {
        return FLASH_STAGING_METADATA_ERROR;
    }
    if ((control_result == OTA_CONTROL_OK) &&
        ((record.state == OTA_CONTROL_STATE_CANDIDATE_READY) ||
         (record.state == OTA_CONTROL_STATE_SWAPPING) ||
         (record.state == OTA_CONTROL_STATE_TRIAL) ||
         (record.state == OTA_CONTROL_STATE_ROLLBACK)))
    {
        return FLASH_STAGING_INVALID_STATE;
    }
    if (OtaControl_EnsureCapacity(
            OTA_CONTROL_REQUIRED_INSTALL_RECORDS) != OTA_CONTROL_OK)
    {
        return FLASH_STAGING_METADATA_ERROR;
    }

    if (control_result == OTA_CONTROL_NOT_FOUND)
    {
        memset(&record, 0, sizeof(record));
    }
    record.state = OTA_CONTROL_STATE_DOWNLOAD_IN_PROGRESS;
    record.flags =
        request->type == FLASH_STAGING_ARTIFACT_FIRMWARE
            ? OTA_CONTROL_FLAG_FIRMWARE
            : 0U;
    memset(&record.secondary, 0, sizeof(record.secondary));
    memset(record.job_id, 0, sizeof(record.job_id));
    (void)snprintf(record.job_id,
                   sizeof(record.job_id),
                   "%s",
                   request->job_id);
    record.job_execution_version = request->job_execution_version;
    if (OtaControl_Append(&record) != OTA_CONTROL_OK)
    {
        return FLASH_STAGING_METADATA_ERROR;
    }

    FlashStaging_ResetRuntime(FLASH_STAGING_STATE_PREPARING);
    staging_request = *request;
    staging_info.file_size_bytes = request->file_size_bytes;
    staging_info.artifact_type = request->type;
    memcpy(staging_info.expected_sha256,
           request->expected_sha256,
           sizeof(staging_info.expected_sha256));
    staging_prepare_sector_count =
        request->type == FLASH_STAGING_ARTIFACT_FIRMWARE
            ? OTA_SLOT_SECTOR_COUNT
            : (request->file_size_bytes + OTA_FLASH_SECTOR_SIZE - 1U) /
                  OTA_FLASH_SECTOR_SIZE;
    if (staging_prepare_sector_count > FLASH_STAGING_SECTOR_COUNT)
    {
        return FlashStaging_Fail(FLASH_STAGING_TOO_LARGE);
    }
    return FLASH_STAGING_IN_PROGRESS;
}

FlashStagingResult FlashStaging_PrepareStep(void)
{
    uint32_t sector_address;
    FlashStagingResult result;

    if (staging_info.state != FLASH_STAGING_STATE_PREPARING)
    {
        return FLASH_STAGING_INVALID_STATE;
    }
    result = FlashStaging_EraseSector(FLASH_STAGING_FIRST_SECTOR +
                                      staging_prepare_sector);
    if (result != FLASH_STAGING_OK)
    {
        return FlashStaging_Fail(result);
    }

    sector_address = FlashStaging_DataAddress() +
                     (staging_prepare_sector * OTA_FLASH_SECTOR_SIZE);
    if (!FlashStaging_RegionIsErased(sector_address,
                                     OTA_FLASH_SECTOR_SIZE))
    {
        return FlashStaging_Fail(FLASH_STAGING_FLASH_ERROR);
    }
    staging_prepare_sector++;
    if (staging_prepare_sector < staging_prepare_sector_count)
    {
        return FLASH_STAGING_IN_PROGRESS;
    }

    staging_info.state = FLASH_STAGING_STATE_RECEIVING;
    return FLASH_STAGING_OK;
}

FlashStagingResult FlashStaging_Write(const uint8_t *data, size_t length)
{
    size_t offset = 0U;

    if (staging_info.state != FLASH_STAGING_STATE_RECEIVING)
    {
        return FLASH_STAGING_INVALID_STATE;
    }
    if ((length > 0U) && (data == NULL))
    {
        return FLASH_STAGING_INVALID_ARGUMENT;
    }
    if ((length > staging_info.file_size_bytes) ||
        (staging_info.bytes_received >
         staging_info.file_size_bytes - length))
    {
        return FLASH_STAGING_INVALID_ARGUMENT;
    }
    if (length == 0U)
    {
        return FLASH_STAGING_OK;
    }
    while (offset < length)
    {
        size_t available =
            OTA_FLASHWORD_SIZE - staging_write_buffer_used;
        size_t copy_length = length - offset;
        if (copy_length > available)
        {
            copy_length = available;
        }
        memcpy(staging_write_buffer + staging_write_buffer_used,
               data + offset,
               copy_length);
        staging_write_buffer_used += copy_length;
        offset += copy_length;

        if (staging_write_buffer_used == OTA_FLASHWORD_SIZE)
        {
            FlashStagingResult result = FlashStaging_ProgramFlashword(
                FlashStaging_DataAddress() + staging_programmed_bytes,
                staging_write_buffer);
            if (result != FLASH_STAGING_OK)
            {
                return FlashStaging_Fail(result);
            }
            staging_programmed_bytes += OTA_FLASHWORD_SIZE;
            staging_write_buffer_used = 0U;
            memset(staging_write_buffer, 0xFF,
                   sizeof(staging_write_buffer));
        }
    }
    staging_info.bytes_received += (uint32_t)length;
    return FLASH_STAGING_OK;
}

static FlashStagingResult FlashStaging_CommitReady(void)
{
    OtaControlRecord record;
    OtaControlResult control_result;

    memset(&record, 0, sizeof(record));
    control_result = OtaControl_LoadLatest(&record);
    if ((control_result != OTA_CONTROL_OK) ||
        (record.state != OTA_CONTROL_STATE_DOWNLOAD_IN_PROGRESS))
    {
        return FLASH_STAGING_METADATA_ERROR;
    }

    if (staging_request.type == FLASH_STAGING_ARTIFACT_FIRMWARE)
    {
        OtaImageDescriptor candidate;
        OtaImageDescriptor current;
        OtaImageResult candidate_result =
            OtaImage_VerifySlot(OTA_SECONDARY_SLOT_ADDRESS, &candidate);
        OtaImageResult current_result =
            OtaImage_VerifySlot(OTA_PRIMARY_SLOT_ADDRESS, &current);
        uint32_t confirmed_version = record.confirmed_version;

        if (candidate_result != OTA_IMAGE_OK)
        {
            return FLASH_STAGING_INTEGRITY_ERROR;
        }
        if (current_result != OTA_IMAGE_OK)
        {
            return FLASH_STAGING_INTEGRITY_ERROR;
        }
        if ((staging_request.expected_image_version == 0U) ||
            (candidate.image_version !=
             staging_request.expected_image_version))
        {
            return FLASH_STAGING_INTEGRITY_ERROR;
        }
        if (confirmed_version == 0U)
        {
            confirmed_version = current.image_version;
        }
        if ((candidate.image_version == confirmed_version) ||
            ((candidate.image_version < confirmed_version) &&
             (!staging_request.allow_downgrade ||
              (staging_request.rollback_from_version != confirmed_version))))
        {
            return FLASH_STAGING_INTEGRITY_ERROR;
        }

        record.state = OTA_CONTROL_STATE_CANDIDATE_READY;
        record.swap_direction = OTA_SWAP_DIRECTION_FORWARD;
        record.swap_sector = 0U;
        record.swap_step = OTA_SWAP_STEP_PRIMARY_TO_SCRATCH;
        record.trial_boot_count = 0U;
        record.confirmed_version = confirmed_version;
        record.primary = current;
        record.secondary = candidate;
        record.flags = OTA_CONTROL_FLAG_FIRMWARE |
            (staging_request.allow_downgrade
                 ? OTA_CONTROL_FLAG_AUTHORIZED_DOWNGRADE
                 : 0U);
        staging_info.image_version = candidate.image_version;
    }
    else
    {
        record.state = OTA_CONTROL_STATE_SERVICE_FILE_READY;
        memset(&record.secondary, 0, sizeof(record.secondary));
        record.secondary.image_size = staging_request.file_size_bytes;
        memcpy(record.secondary.sha256,
               staging_request.expected_sha256,
               sizeof(record.secondary.sha256));
        record.flags = 0U;
    }

    return OtaControl_Append(&record) == OTA_CONTROL_OK
               ? FLASH_STAGING_OK
               : FLASH_STAGING_METADATA_ERROR;
}

FlashStagingResult FlashStaging_Finalize(void)
{
    uint8_t calculated[FLASH_STAGING_SHA256_SIZE];
    FlashStagingResult result;

    if ((staging_info.state != FLASH_STAGING_STATE_RECEIVING) ||
        (staging_info.bytes_received != staging_info.file_size_bytes))
    {
        return FLASH_STAGING_INVALID_STATE;
    }
    if (staging_write_buffer_used > 0U)
    {
        result = FlashStaging_ProgramFlashword(
            FlashStaging_DataAddress() + staging_programmed_bytes,
            staging_write_buffer);
        if (result != FLASH_STAGING_OK)
        {
            return FlashStaging_Fail(result);
        }
    }

    result = FlashStaging_CalculateSha256(staging_info.file_size_bytes,
                                          calculated);
    if (result != FLASH_STAGING_OK)
    {
        return FlashStaging_Fail(result);
    }
    if (memcmp(calculated,
               staging_info.expected_sha256,
               sizeof(calculated)) != 0)
    {
        return FlashStaging_Fail(FLASH_STAGING_HASH_MISMATCH);
    }

    result = FlashStaging_CommitReady();
    if (result != FLASH_STAGING_OK)
    {
        return FlashStaging_Fail(result);
    }
    staging_info.state = FLASH_STAGING_STATE_READY;
    return FLASH_STAGING_OK;
}

FlashStagingResult FlashStaging_Abort(void)
{
    OtaControlRecord record;

    if ((OtaControl_LoadLatest(&record) == OTA_CONTROL_OK) &&
        (record.state == OTA_CONTROL_STATE_DOWNLOAD_IN_PROGRESS))
    {
        record.state = OTA_CONTROL_STATE_REJECTED;
        record.flags = 0U;
        if (OtaControl_Append(&record) != OTA_CONTROL_OK)
        {
            return FlashStaging_Fail(FLASH_STAGING_METADATA_ERROR);
        }
    }
    FlashStaging_ResetRuntime(FLASH_STAGING_STATE_EMPTY);
    return FLASH_STAGING_OK;
}

FlashStagingResult FlashStaging_Verify(void)
{
    uint8_t calculated[FLASH_STAGING_SHA256_SIZE];

    if (staging_info.state != FLASH_STAGING_STATE_READY)
    {
        return FLASH_STAGING_INVALID_STATE;
    }
    if (staging_info.artifact_type == FLASH_STAGING_ARTIFACT_FIRMWARE)
    {
        OtaImageDescriptor descriptor;
        return OtaImage_VerifySlot(OTA_SECONDARY_SLOT_ADDRESS,
                                   &descriptor) == OTA_IMAGE_OK
                   ? FLASH_STAGING_OK
                   : FLASH_STAGING_INTEGRITY_ERROR;
    }
    if (FlashStaging_CalculateSha256(staging_info.file_size_bytes,
                                     calculated) != FLASH_STAGING_OK)
    {
        return FLASH_STAGING_CRYPTO_ERROR;
    }
    return memcmp(calculated,
                  staging_info.expected_sha256,
                  sizeof(calculated)) == 0
               ? FLASH_STAGING_OK
               : FLASH_STAGING_HASH_MISMATCH;
}

FlashStagingResult FlashStaging_Read(uint32_t offset,
                                     uint8_t *destination,
                                     size_t length)
{
    if (staging_info.state != FLASH_STAGING_STATE_READY)
    {
        return FLASH_STAGING_INVALID_STATE;
    }
    if ((length > 0U) && (destination == NULL))
    {
        return FLASH_STAGING_INVALID_ARGUMENT;
    }
    if ((length > staging_info.file_size_bytes) ||
        (offset > staging_info.file_size_bytes - length))
    {
        return FLASH_STAGING_INVALID_ARGUMENT;
    }
    FlashStaging_InvalidateCache(FlashStaging_DataAddress() + offset,
                                 length);
    memcpy(destination,
           (const void *)(uintptr_t)(FlashStaging_DataAddress() + offset),
           length);
    return FLASH_STAGING_OK;
}

const FlashStagingInfo *FlashStaging_GetInfo(void)
{
    return &staging_info;
}

const char *FlashStaging_StateName(FlashStagingState state)
{
    switch (state)
    {
        case FLASH_STAGING_STATE_EMPTY:
            return "EMPTY";
        case FLASH_STAGING_STATE_PREPARING:
            return "PREPARING";
        case FLASH_STAGING_STATE_RECEIVING:
            return "RECEIVING";
        case FLASH_STAGING_STATE_READY:
            return "READY";
        case FLASH_STAGING_STATE_INVALID:
            return "INVALID";
        default:
            return "UNKNOWN";
    }
}
