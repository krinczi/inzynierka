#ifndef INC_OTA_CONTROL_H_
#define INC_OTA_CONTROL_H_

#include "ota_layout.h"

#include <stddef.h>
#include <stdint.h>

#define OTA_CONTROL_MAGIC              0x4F544132UL /* "OTA2" */
#define OTA_CONTROL_FORMAT             2U
#define OTA_CONTROL_COMMITTED          0xC01DB007UL
#define OTA_IMAGE_DESCRIPTOR_MAGIC     0x494D4732UL /* "IMG2" */
#define OTA_CONTROL_RECORD_SIZE        512U
#define OTA_CONTROL_JOB_ID_SIZE        65U
#define OTA_CONTROL_REQUIRED_INSTALL_RECORDS 24U
#define OTA_SHA256_SIZE                32U
#define OTA_P256_SIGNATURE_SIZE        64U
#define OTA_CONTROL_FLAG_FIRMWARE             0x00000001UL
#define OTA_CONTROL_FLAG_JOB_REPORT_PENDING   0x00000002UL
#define OTA_CONTROL_FLAG_ROLLBACK_PERFORMED   0x00000004UL
#define OTA_CONTROL_FLAG_INSTALL_REJECTED     0x00000008UL
#define OTA_CONTROL_FLAG_AUTHORIZED_DOWNGRADE 0x00000010UL

typedef enum
{
    OTA_CONTROL_STATE_CONFIRMED = 1,
    OTA_CONTROL_STATE_DOWNLOAD_IN_PROGRESS = 2,
    OTA_CONTROL_STATE_SERVICE_FILE_READY = 3,
    OTA_CONTROL_STATE_CANDIDATE_READY = 4,
    OTA_CONTROL_STATE_SWAPPING = 5,
    OTA_CONTROL_STATE_TRIAL = 6,
    OTA_CONTROL_STATE_ROLLBACK = 7,
    OTA_CONTROL_STATE_REJECTED = 8
} OtaControlState;

typedef enum
{
    OTA_SWAP_DIRECTION_FORWARD = 1,
    OTA_SWAP_DIRECTION_ROLLBACK = 2
} OtaSwapDirection;

typedef enum
{
    OTA_SWAP_STEP_PRIMARY_TO_SCRATCH = 0,
    OTA_SWAP_STEP_SECONDARY_TO_PRIMARY = 1,
    OTA_SWAP_STEP_SCRATCH_TO_SECONDARY = 2
} OtaSwapStep;

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint32_t image_size;
    uint32_t image_version;
    uint8_t sha256[OTA_SHA256_SIZE];
    uint8_t signature[OTA_P256_SIGNATURE_SIZE];
} OtaImageDescriptor;

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t storage_format;
    uint16_t record_size;
    uint32_t sequence;
    uint32_t state;
    uint32_t swap_direction;
    uint32_t swap_sector;
    uint32_t swap_step;
    uint32_t trial_boot_count;
    uint32_t confirmed_version;
    OtaImageDescriptor primary;
    OtaImageDescriptor secondary;
    char job_id[OTA_CONTROL_JOB_ID_SIZE];
    uint32_t job_execution_version;
    uint32_t flags;
    uint8_t reserved[179];
    uint32_t crc32;
    uint32_t committed;
} OtaControlRecord;

_Static_assert(sizeof(OtaImageDescriptor) == 108U,
               "Unexpected OTA image descriptor size");
_Static_assert(sizeof(OtaControlRecord) == OTA_CONTROL_RECORD_SIZE,
               "OTA control record must be exactly 512 bytes");
_Static_assert((sizeof(OtaControlRecord) % OTA_FLASHWORD_SIZE) == 0U,
               "OTA control record must use complete Flash words");

typedef enum
{
    OTA_CONTROL_OK = 0,
    OTA_CONTROL_NOT_FOUND = 1,
    OTA_CONTROL_INVALID_ARGUMENT = -1,
    OTA_CONTROL_FLASH_ERROR = -2,
    OTA_CONTROL_LOG_FULL = -3,
    OTA_CONTROL_CORRUPT = -4
} OtaControlResult;

uint32_t OtaControl_Crc32(const uint8_t *data, size_t length);
int OtaControl_ImageDescriptorIsValid(const OtaImageDescriptor *descriptor);
int OtaControl_RecordIsValid(const OtaControlRecord *record);

/* Scans without programming or erasing Flash. */
OtaControlResult OtaControl_LoadLatest(OtaControlRecord *record);

/*
 * Guarantees enough append slots for a complete four-sector swap, trial boot
 * and terminal job report. Stable logs are compacted power-loss-safely using
 * the scratch sector as a temporary copy of the latest committed record.
 */
OtaControlResult OtaControl_EnsureCapacity(uint32_t required_records);

/*
 * Atomically appends a record. Common header, sequence, CRC and commit marker
 * are assigned by this function. The log is never erased implicitly.
 */
OtaControlResult OtaControl_Append(OtaControlRecord *record);

const char *OtaControl_StateName(uint32_t state);

#endif /* INC_OTA_CONTROL_H_ */
