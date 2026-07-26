#include "flash_staging.h"
#include "main.h"

#include "mbedtls/sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FLASH_STAGING_FLASHWORD_SIZE       32U
#define FLASH_STAGING_METADATA_MAGIC       0x53544731UL /* "STG1" */
#define FLASH_STAGING_METADATA_FORMAT      1U
#define FLASH_STAGING_COMMITTED_STATE      0xC01117EDUL
#define FLASH_STAGING_FIRST_SECTOR         FLASH_SECTOR_0
#define FLASH_STAGING_SECTOR_COUNT         6U
#define FLASH_STAGING_METADATA_SECTOR      FLASH_SECTOR_0

extern uint8_t __flash_staging_metadata_start__[];
extern uint8_t __flash_staging_metadata_end__[];
extern uint8_t __flash_staging_data_start__[];
extern uint8_t __flash_staging_data_end__[];

typedef struct
{
    uint32_t magic;
    uint16_t storage_format;
    uint16_t record_size;
    uint32_t file_size_bytes;
    uint32_t committed_state;
    uint32_t crc32;
    uint8_t reserved[12];
    uint8_t sha256[FLASH_STAGING_SHA256_SIZE];
} FlashStagingMetadata;

_Static_assert(sizeof(FlashStagingMetadata) == 64U,
               "Staging metadata must occupy two STM32H7 Flash words");

static FlashStagingInfo staging_info;
static mbedtls_sha256_context staging_sha256;
static int staging_sha256_active;
static uint32_t staging_prepare_sector;
static uint32_t staging_prepare_sector_count;
static uint32_t staging_programmed_bytes;
static size_t staging_write_buffer_used;
static uint8_t staging_write_buffer[FLASH_STAGING_FLASHWORD_SIZE]
    __attribute__((aligned(FLASH_STAGING_FLASHWORD_SIZE)));

static uint32_t FlashStaging_MetadataAddress(void)
{
    return (uint32_t)(uintptr_t)__flash_staging_metadata_start__;
}

static uint32_t FlashStaging_MetadataSize(void)
{
    return (uint32_t)((uintptr_t)__flash_staging_metadata_end__ -
                      (uintptr_t)__flash_staging_metadata_start__);
}

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
    uintptr_t aligned_start;
    uintptr_t aligned_end;

    if ((length == 0U) || ((SCB->CCR & SCB_CCR_DC_Msk) == 0U))
    {
        return;
    }

    aligned_start = (uintptr_t)address & ~(uintptr_t)(FLASH_STAGING_FLASHWORD_SIZE - 1U);
    aligned_end = ((uintptr_t)address + length + FLASH_STAGING_FLASHWORD_SIZE - 1U) &
                  ~(uintptr_t)(FLASH_STAGING_FLASHWORD_SIZE - 1U);

    SCB_InvalidateDCache_by_Addr((uint32_t *)aligned_start,
                                (int32_t)(aligned_end - aligned_start));
    __DSB();
    __ISB();
}

static uint32_t FlashStaging_Crc32(const uint8_t *data, size_t length)
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

static uint32_t FlashStaging_MetadataCrc(const FlashStagingMetadata *metadata)
{
    FlashStagingMetadata copy = *metadata;

    copy.crc32 = 0U;
    return FlashStaging_Crc32((const uint8_t *)&copy, sizeof(copy));
}

static void FlashStaging_LoadMetadata(FlashStagingMetadata *metadata)
{
    FlashStaging_InvalidateCache(FlashStaging_MetadataAddress(), sizeof(*metadata));
    memcpy(metadata, (const void *)(uintptr_t)FlashStaging_MetadataAddress(), sizeof(*metadata));
}

static int FlashStaging_MetadataIsErased(const FlashStagingMetadata *metadata)
{
    const uint32_t *word = (const uint32_t *)metadata;
    size_t index;

    for (index = 0U; index < (sizeof(*metadata) / sizeof(uint32_t)); index++)
    {
        if (word[index] != 0xFFFFFFFFUL)
        {
            return 0;
        }
    }

    return 1;
}

static int FlashStaging_MetadataIsValid(const FlashStagingMetadata *metadata)
{
    if ((metadata->magic != FLASH_STAGING_METADATA_MAGIC) ||
        (metadata->storage_format != FLASH_STAGING_METADATA_FORMAT) ||
        (metadata->record_size != sizeof(*metadata)) ||
        (metadata->committed_state != FLASH_STAGING_COMMITTED_STATE) ||
        (metadata->file_size_bytes == 0U) ||
        (metadata->file_size_bytes > FlashStaging_DataCapacity()))
    {
        return 0;
    }

    return metadata->crc32 == FlashStaging_MetadataCrc(metadata);
}

static void FlashStaging_StopHash(void)
{
    if (staging_sha256_active)
    {
        mbedtls_sha256_free(&staging_sha256);
        staging_sha256_active = 0;
    }
}

static void FlashStaging_ResetRuntime(FlashStagingState state)
{
    FlashStaging_StopHash();
    memset(&staging_info, 0, sizeof(staging_info));
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
    FlashStaging_StopHash();
    staging_info.state = FLASH_STAGING_STATE_INVALID;
    return result;
}

static FlashStagingResult FlashStaging_Erase(uint32_t first_sector, uint32_t sector_count)
{
    FLASH_EraseInitTypeDef erase = { 0 };
    HAL_StatusTypeDef status;
    uint32_t sector_error = 0xFFFFFFFFUL;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_2;
    erase.Sector = first_sector;
    erase.NbSectors = sector_count;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return FLASH_STAGING_FLASH_ERROR;
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK2);
    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    (void)HAL_FLASH_Lock();

    if ((status != HAL_OK) || (sector_error != 0xFFFFFFFFUL))
    {
        return FLASH_STAGING_FLASH_ERROR;
    }

    return FLASH_STAGING_OK;
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
        if (*word != 0xFFFFFFFFUL)
        {
            return 0;
        }
        word++;
    }

    return 1;
}

static FlashStagingResult FlashStaging_ProgramFlashword(uint32_t address,
                                                        const uint8_t *flashword)
{
    HAL_StatusTypeDef status;

    if (((address % FLASH_STAGING_FLASHWORD_SIZE) != 0U) ||
        (((uintptr_t)flashword % FLASH_STAGING_FLASHWORD_SIZE) != 0U))
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
                               (uint32_t)(uintptr_t)flashword);
    (void)HAL_FLASH_Lock();

    if (status != HAL_OK)
    {
        return FLASH_STAGING_FLASH_ERROR;
    }

    FlashStaging_InvalidateCache(address, FLASH_STAGING_FLASHWORD_SIZE);
    if (memcmp((const void *)(uintptr_t)address,
               flashword,
               FLASH_STAGING_FLASHWORD_SIZE) != 0)
    {
        return FLASH_STAGING_FLASH_ERROR;
    }

    return FLASH_STAGING_OK;
}

static FlashStagingResult FlashStaging_CommitMetadata(
    uint32_t file_size_bytes,
    const uint8_t sha256[FLASH_STAGING_SHA256_SIZE])
{
    FlashStagingMetadata metadata __attribute__((aligned(FLASH_STAGING_FLASHWORD_SIZE)));
    FlashStagingMetadata verify_metadata;
    FlashStagingResult result;
    const uint8_t *bytes;

    memset(&metadata, 0, sizeof(metadata));
    metadata.magic = FLASH_STAGING_METADATA_MAGIC;
    metadata.storage_format = FLASH_STAGING_METADATA_FORMAT;
    metadata.record_size = sizeof(metadata);
    metadata.file_size_bytes = file_size_bytes;
    metadata.committed_state = FLASH_STAGING_COMMITTED_STATE;
    memcpy(metadata.sha256, sha256, sizeof(metadata.sha256));
    metadata.crc32 = FlashStaging_MetadataCrc(&metadata);
    bytes = (const uint8_t *)&metadata;

    /* Program the digest first and the word containing magic last. */
    result = FlashStaging_ProgramFlashword(FlashStaging_MetadataAddress() +
                                              FLASH_STAGING_FLASHWORD_SIZE,
                                          bytes + FLASH_STAGING_FLASHWORD_SIZE);
    if (result != FLASH_STAGING_OK)
    {
        return result;
    }

    result = FlashStaging_ProgramFlashword(FlashStaging_MetadataAddress(), bytes);
    if (result != FLASH_STAGING_OK)
    {
        return result;
    }

    FlashStaging_LoadMetadata(&verify_metadata);
    if (!FlashStaging_MetadataIsValid(&verify_metadata) ||
        (memcmp(&verify_metadata, &metadata, sizeof(metadata)) != 0))
    {
        return FLASH_STAGING_METADATA_ERROR;
    }

    return FLASH_STAGING_OK;
}

static FlashStagingResult FlashStaging_CalculateStoredSha256(
    uint32_t file_size_bytes,
    uint8_t output[FLASH_STAGING_SHA256_SIZE])
{
    mbedtls_sha256_context context;
    const uint8_t *data = (const uint8_t *)(uintptr_t)FlashStaging_DataAddress();
    uint32_t offset = 0U;
    int result;

    FlashStaging_InvalidateCache(FlashStaging_DataAddress(), file_size_bytes);
    mbedtls_sha256_init(&context);
    result = mbedtls_sha256_starts_ret(&context, 0);

    while ((result == 0) && (offset < file_size_bytes))
    {
        size_t chunk = file_size_bytes - offset;
        if (chunk > 1024U)
        {
            chunk = 1024U;
        }

        result = mbedtls_sha256_update_ret(&context, data + offset, chunk);
        offset += (uint32_t)chunk;
    }

    if (result == 0)
    {
        result = mbedtls_sha256_finish_ret(&context, output);
    }

    mbedtls_sha256_free(&context);
    return result == 0 ? FLASH_STAGING_OK : FLASH_STAGING_CRYPTO_ERROR;
}

FlashStagingResult FlashStaging_Init(void)
{
    FlashStagingMetadata metadata;
    FlashStagingResult result;

    FlashStaging_ResetRuntime(FLASH_STAGING_STATE_EMPTY);
    FlashStaging_LoadMetadata(&metadata);

    if (FlashStaging_MetadataIsErased(&metadata))
    {
        return FLASH_STAGING_OK;
    }

    if (!FlashStaging_MetadataIsValid(&metadata))
    {
        staging_info.state = FLASH_STAGING_STATE_INVALID;
        return FLASH_STAGING_METADATA_ERROR;
    }

    staging_info.state = FLASH_STAGING_STATE_READY;
    staging_info.file_size_bytes = metadata.file_size_bytes;
    staging_info.bytes_received = metadata.file_size_bytes;
    memcpy(staging_info.expected_sha256, metadata.sha256, sizeof(metadata.sha256));

    result = FlashStaging_Verify();
    if (result != FLASH_STAGING_OK)
    {
        staging_info.state = FLASH_STAGING_STATE_INVALID;
    }

    return result;
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
    uint32_t sector_size;

    if (expected_sha256 == NULL)
    {
        return FLASH_STAGING_INVALID_ARGUMENT;
    }

    if ((file_size_bytes == 0U) || (file_size_bytes > FlashStaging_DataCapacity()))
    {
        return FLASH_STAGING_TOO_LARGE;
    }

    if ((staging_info.state == FLASH_STAGING_STATE_PREPARING) ||
        (staging_info.state == FLASH_STAGING_STATE_RECEIVING))
    {
        return FLASH_STAGING_INVALID_STATE;
    }

    FlashStaging_ResetRuntime(FLASH_STAGING_STATE_PREPARING);
    staging_info.file_size_bytes = file_size_bytes;
    memcpy(staging_info.expected_sha256,
           expected_sha256,
           FLASH_STAGING_SHA256_SIZE);

    sector_size = FlashStaging_MetadataSize();
    if (sector_size == 0U)
    {
        return FlashStaging_Fail(FLASH_STAGING_FLASH_ERROR);
    }
    staging_prepare_sector_count =
        1U + ((file_size_bytes + sector_size - 1U) / sector_size);
    if (staging_prepare_sector_count > FLASH_STAGING_SECTOR_COUNT)
    {
        return FlashStaging_Fail(FLASH_STAGING_TOO_LARGE);
    }
    return FLASH_STAGING_IN_PROGRESS;
}

FlashStagingResult FlashStaging_PrepareStep(void)
{
    FlashStagingResult result;
    uint32_t sector_address;
    uint32_t sector_size = FlashStaging_MetadataSize();

    if (staging_info.state != FLASH_STAGING_STATE_PREPARING)
    {
        return FLASH_STAGING_INVALID_STATE;
    }

    result = FlashStaging_Erase(FLASH_STAGING_FIRST_SECTOR +
                                    staging_prepare_sector,
                                1U);
    if (result != FLASH_STAGING_OK)
    {
        return FlashStaging_Fail(result);
    }

    sector_address = FlashStaging_MetadataAddress() +
                     (staging_prepare_sector * sector_size);
    FlashStaging_InvalidateCache(sector_address, sector_size);
    if (!FlashStaging_RegionIsErased(sector_address, sector_size))
    {
        return FlashStaging_Fail(FLASH_STAGING_FLASH_ERROR);
    }

    staging_prepare_sector++;
    if (staging_prepare_sector < staging_prepare_sector_count)
    {
        return FLASH_STAGING_IN_PROGRESS;
    }

    mbedtls_sha256_init(&staging_sha256);
    staging_sha256_active = 1;
    if (mbedtls_sha256_starts_ret(&staging_sha256, 0) != 0)
    {
        return FlashStaging_Fail(FLASH_STAGING_CRYPTO_ERROR);
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
        (staging_info.bytes_received > staging_info.file_size_bytes - length))
    {
        return FLASH_STAGING_INVALID_ARGUMENT;
    }

    if (length == 0U)
    {
        return FLASH_STAGING_OK;
    }

    if (mbedtls_sha256_update_ret(&staging_sha256, data, length) != 0)
    {
        return FlashStaging_Fail(FLASH_STAGING_CRYPTO_ERROR);
    }

    while (offset < length)
    {
        size_t available = FLASH_STAGING_FLASHWORD_SIZE - staging_write_buffer_used;
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

        if (staging_write_buffer_used == FLASH_STAGING_FLASHWORD_SIZE)
        {
            FlashStagingResult result = FlashStaging_ProgramFlashword(
                FlashStaging_DataAddress() + staging_programmed_bytes,
                staging_write_buffer);
            if (result != FLASH_STAGING_OK)
            {
                return FlashStaging_Fail(result);
            }

            staging_programmed_bytes += FLASH_STAGING_FLASHWORD_SIZE;
            staging_write_buffer_used = 0U;
            memset(staging_write_buffer, 0xFF, sizeof(staging_write_buffer));
        }
    }

    staging_info.bytes_received += (uint32_t)length;
    return FLASH_STAGING_OK;
}

FlashStagingResult FlashStaging_Finalize(void)
{
    uint8_t calculated_sha256[FLASH_STAGING_SHA256_SIZE];
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
        staging_programmed_bytes += FLASH_STAGING_FLASHWORD_SIZE;
    }

    if (mbedtls_sha256_finish_ret(&staging_sha256, calculated_sha256) != 0)
    {
        return FlashStaging_Fail(FLASH_STAGING_CRYPTO_ERROR);
    }
    FlashStaging_StopHash();

    if (memcmp(calculated_sha256,
               staging_info.expected_sha256,
               FLASH_STAGING_SHA256_SIZE) != 0)
    {
        return FlashStaging_Fail(FLASH_STAGING_INTEGRITY_ERROR);
    }

    result = FlashStaging_CommitMetadata(staging_info.file_size_bytes,
                                         calculated_sha256);
    if (result != FLASH_STAGING_OK)
    {
        return FlashStaging_Fail(result);
    }

    staging_info.state = FLASH_STAGING_STATE_READY;
    /*
     * Each Flash word was read back after programming and the streaming
     * SHA-256 was checked above. Rehashing the complete image here would stall
     * MQTT for no additional runtime guarantee. Boot-time Init still performs
     * the independent full-image verification.
     */
    return FLASH_STAGING_OK;
}

FlashStagingResult FlashStaging_Abort(void)
{
    FlashStagingResult result;

    FlashStaging_StopHash();

    /*
     * Once preparation erased sector 0, the committed metadata is already
     * invalid. Avoid another full-sector erase on a retry or link loss.
     */
    if (((staging_info.state == FLASH_STAGING_STATE_PREPARING) &&
         (staging_prepare_sector > 0U)) ||
        (staging_info.state == FLASH_STAGING_STATE_RECEIVING) ||
        (staging_info.state == FLASH_STAGING_STATE_EMPTY))
    {
        FlashStaging_ResetRuntime(FLASH_STAGING_STATE_EMPTY);
        return FLASH_STAGING_OK;
    }

    result = FlashStaging_Erase(FLASH_STAGING_METADATA_SECTOR, 1U);
    if (result != FLASH_STAGING_OK)
    {
        return FlashStaging_Fail(result);
    }

    FlashStaging_InvalidateCache(FlashStaging_MetadataAddress(),
                                 FlashStaging_MetadataSize());
    if (!FlashStaging_RegionIsErased(FlashStaging_MetadataAddress(),
                                     FlashStaging_MetadataSize()))
    {
        return FlashStaging_Fail(FLASH_STAGING_FLASH_ERROR);
    }

    FlashStaging_ResetRuntime(FLASH_STAGING_STATE_EMPTY);
    return FLASH_STAGING_OK;
}

FlashStagingResult FlashStaging_Verify(void)
{
    uint8_t calculated_sha256[FLASH_STAGING_SHA256_SIZE];
    FlashStagingResult result;

    if (staging_info.state != FLASH_STAGING_STATE_READY)
    {
        return FLASH_STAGING_INVALID_STATE;
    }

    result = FlashStaging_CalculateStoredSha256(staging_info.file_size_bytes,
                                                calculated_sha256);
    if (result != FLASH_STAGING_OK)
    {
        return result;
    }

    return memcmp(calculated_sha256,
                  staging_info.expected_sha256,
                  FLASH_STAGING_SHA256_SIZE) == 0
               ? FLASH_STAGING_OK
               : FLASH_STAGING_INTEGRITY_ERROR;
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

    FlashStaging_InvalidateCache(FlashStaging_DataAddress() + offset, length);
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
