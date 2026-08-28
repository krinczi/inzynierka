#ifndef INC_FLASH_STAGING_H_
#define INC_FLASH_STAGING_H_

#include <stddef.h>
#include <stdint.h>

#include "ota_control.h"

#define FLASH_STAGING_SHA256_SIZE OTA_SHA256_SIZE

typedef enum
{
    FLASH_STAGING_ARTIFACT_SERVICE_FILE = 0,
    FLASH_STAGING_ARTIFACT_FIRMWARE = 1
} FlashStagingArtifactType;

typedef struct
{
    FlashStagingArtifactType type;
    uint32_t file_size_bytes;
    uint8_t expected_sha256[FLASH_STAGING_SHA256_SIZE];
    uint32_t expected_image_version;
    uint32_t rollback_from_version;
    uint8_t allow_downgrade;
    char job_id[OTA_CONTROL_JOB_ID_SIZE];
    uint32_t job_execution_version;
} FlashStagingRequest;

typedef enum
{
    FLASH_STAGING_STATE_EMPTY = 0,
    FLASH_STAGING_STATE_PREPARING,
    FLASH_STAGING_STATE_RECEIVING,
    FLASH_STAGING_STATE_READY,
    FLASH_STAGING_STATE_INVALID
} FlashStagingState;

typedef enum
{
    FLASH_STAGING_IN_PROGRESS = 1,
    FLASH_STAGING_OK = 0,
    FLASH_STAGING_INVALID_ARGUMENT = -1,
    FLASH_STAGING_TOO_LARGE = -2,
    FLASH_STAGING_INVALID_STATE = -3,
    FLASH_STAGING_FLASH_ERROR = -4,
    FLASH_STAGING_INTEGRITY_ERROR = -5,
    FLASH_STAGING_METADATA_ERROR = -6,
    FLASH_STAGING_CRYPTO_ERROR = -7,
    FLASH_STAGING_HASH_MISMATCH = -8
} FlashStagingResult;

typedef struct
{
    FlashStagingState state;
    uint32_t capacity_bytes;
    uint32_t file_size_bytes;
    uint32_t bytes_received;
    uint8_t expected_sha256[FLASH_STAGING_SHA256_SIZE];
    FlashStagingArtifactType artifact_type;
    uint32_t image_version;
} FlashStagingInfo;

/*
 * Initializes the staging module and verifies a previously committed file.
 * No Flash erase or program operation is performed by this function.
 */
FlashStagingResult FlashStaging_Init(void);

/* Erases the staging region and starts a new sequential transfer. */
FlashStagingResult FlashStaging_Begin(
    uint32_t file_size_bytes,
    const uint8_t expected_sha256[FLASH_STAGING_SHA256_SIZE]);

/*
 * Starts a transfer without erasing the whole region in one call. Each Step
 * erases and verifies at most one sector and returns IN_PROGRESS until ready.
 */
FlashStagingResult FlashStaging_PrepareStart(
    uint32_t file_size_bytes,
    const uint8_t expected_sha256[FLASH_STAGING_SHA256_SIZE]);
FlashStagingResult FlashStaging_PrepareArtifact(
    const FlashStagingRequest *request);
FlashStagingResult FlashStaging_PrepareStep(void);

/* Accepts arbitrary chunk sizes. Chunks must be supplied in file order. */
FlashStagingResult FlashStaging_Write(const uint8_t *data, size_t length);

/*
 * Flushes the final Flash word, validates SHA-256 and atomically commits
 * metadata. A file is READY only after this function succeeds.
 */
FlashStagingResult FlashStaging_Finalize(void);

/* Invalidates the staged file. Download data is erased by the next Begin. */
FlashStagingResult FlashStaging_Abort(void);

/* Recomputes SHA-256 directly from the staged Flash contents. */
FlashStagingResult FlashStaging_Verify(void);

/* Reads bytes only from a committed and verified file. */
FlashStagingResult FlashStaging_Read(uint32_t offset, uint8_t *destination, size_t length);

const FlashStagingInfo *FlashStaging_GetInfo(void);
const char *FlashStaging_StateName(FlashStagingState state);

#endif /* INC_FLASH_STAGING_H_ */
