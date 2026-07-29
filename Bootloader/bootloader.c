#include "firmware_trust_anchor_config.h"
#include "ota_control.h"
#include "ota_image.h"

#include "stm32h753xx.h"
#include "stm32h7xx_hal_flash.h"
#include "stm32h7xx_hal_flash_ex.h"

#include "mbedtls/bignum.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#define BOOT_CONTROL_RECORD_COUNT \
    (OTA_CONTROL_LOG_SIZE / OTA_CONTROL_RECORD_SIZE)
#define BOOT_FLASH_ERROR_MASK_BANK1 \
    ((uint32_t)FLASH_FLAG_ALL_ERRORS_BANK1)
#define BOOT_FLASH_ERROR_MASK_BANK2 \
    ((uint32_t)FLASH_FLAG_ALL_ERRORS_BANK2 & 0x7FFFFFFFUL)
#define BOOT_FLASH_WAIT_LIMIT 0x10000000UL

extern uint8_t end;
extern uint8_t __heap_end__;

static uint8_t *boot_heap = &end;

static void Boot_WatchdogRefresh(void)
{
    /*
     * Reload is harmless before IWDG start and mandatory after a software
     * reset from an application that already enabled the independent
     * watchdog.
     */
    IWDG1->KR = 0x0000AAAAUL;
}

static uint32_t Boot_Crc32(const uint8_t *data, size_t length)
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

static int Boot_RecordValid(const OtaControlRecord *record)
{
    return (record->magic == OTA_CONTROL_MAGIC) &&
           (record->storage_format == OTA_CONTROL_FORMAT) &&
           (record->record_size == OTA_CONTROL_RECORD_SIZE) &&
           (record->sequence != 0U) &&
           (record->state >= OTA_CONTROL_STATE_CONFIRMED) &&
           (record->state <= OTA_CONTROL_STATE_REJECTED) &&
           (record->committed == OTA_CONTROL_COMMITTED) &&
           (record->crc32 ==
            Boot_Crc32((const uint8_t *)record,
                       offsetof(OtaControlRecord, crc32)));
}

static int Boot_RecordErased(uint32_t address)
{
    const uint32_t *word = (const uint32_t *)(uintptr_t)address;
    uint32_t index;

    for (index = 0U;
         index < (OTA_CONTROL_RECORD_SIZE / sizeof(uint32_t));
         index++)
    {
        if (word[index] != 0xFFFFFFFFUL)
        {
            return 0;
        }
    }
    return 1;
}

static int Boot_LoadLatest(OtaControlRecord *record)
{
    OtaControlRecord candidate;
    uint32_t index;
    int found = 0;

    memset(record, 0, sizeof(*record));
    for (index = 0U; index < BOOT_CONTROL_RECORD_COUNT; index++)
    {
        uint32_t address =
            OTA_CONTROL_LOG_ADDRESS +
            (index * OTA_CONTROL_RECORD_SIZE);
        if (Boot_RecordErased(address))
        {
            continue;
        }
        memcpy(&candidate,
               (const void *)(uintptr_t)address,
               sizeof(candidate));
        if (Boot_RecordValid(&candidate) &&
            ((!found) ||
             ((int32_t)(candidate.sequence - record->sequence) > 0)))
        {
            *record = candidate;
            found = 1;
        }
    }
    if (!found)
    {
        memcpy(&candidate,
               (const void *)(uintptr_t)OTA_SCRATCH_ADDRESS,
               sizeof(candidate));
        if (Boot_RecordValid(&candidate) &&
            ((candidate.state == OTA_CONTROL_STATE_CONFIRMED) ||
             (candidate.state == OTA_CONTROL_STATE_SERVICE_FILE_READY) ||
             (candidate.state == OTA_CONTROL_STATE_REJECTED)))
        {
            *record = candidate;
            found = 1;
        }
    }
    return found;
}

static int Boot_FlashWait(uint32_t bank)
{
    volatile uint32_t *status =
        bank == FLASH_BANK_1 ? &FLASH->SR1 : &FLASH->SR2;
    uint32_t error_mask =
        bank == FLASH_BANK_1 ? BOOT_FLASH_ERROR_MASK_BANK1
                             : BOOT_FLASH_ERROR_MASK_BANK2;
    volatile uint32_t *clear =
        bank == FLASH_BANK_1 ? &FLASH->CCR1 : &FLASH->CCR2;
    uint32_t limit = BOOT_FLASH_WAIT_LIMIT;

    while (((*status & FLASH_SR_QW) != 0U) && (limit-- > 0U))
    {
        Boot_WatchdogRefresh();
        __NOP();
    }
    if (limit == 0U)
    {
        return -1;
    }
    if ((*status & error_mask) != 0U)
    {
        *clear = *status & error_mask;
        return -1;
    }
    if ((*status & FLASH_SR_EOP) != 0U)
    {
        *clear = FLASH_SR_EOP;
    }
    return 0;
}

static int Boot_FlashUnlock(void)
{
    if ((FLASH->CR1 & FLASH_CR_LOCK) != 0U)
    {
        FLASH->KEYR1 = FLASH_KEY1;
        FLASH->KEYR1 = FLASH_KEY2;
    }
    if ((FLASH->CR2 & FLASH_CR_LOCK) != 0U)
    {
        FLASH->KEYR2 = FLASH_KEY1;
        FLASH->KEYR2 = FLASH_KEY2;
    }
    return (((FLASH->CR1 | FLASH->CR2) & FLASH_CR_LOCK) == 0U)
               ? 0
               : -1;
}

static void Boot_FlashLock(void)
{
    FLASH->CR1 |= FLASH_CR_LOCK;
    FLASH->CR2 |= FLASH_CR_LOCK;
}

static int Boot_FlashEraseSector(uint32_t bank, uint32_t sector)
{
    volatile uint32_t *control =
        bank == FLASH_BANK_1 ? &FLASH->CR1 : &FLASH->CR2;
    int result;

    if ((sector > 7U) || (Boot_FlashUnlock() != 0) ||
        (Boot_FlashWait(bank) != 0))
    {
        Boot_FlashLock();
        return -1;
    }

    if (bank == FLASH_BANK_1)
    {
        FLASH->CCR1 =
            BOOT_FLASH_ERROR_MASK_BANK1 | FLASH_SR_EOP;
    }
    else
    {
        FLASH->CCR2 =
            BOOT_FLASH_ERROR_MASK_BANK2 | FLASH_SR_EOP;
    }
    *control &= ~(FLASH_CR_PSIZE | FLASH_CR_SNB |
                  FLASH_CR_SER | FLASH_CR_PG);
    *control |= FLASH_CR_SER | FLASH_VOLTAGE_RANGE_3 |
                (sector << FLASH_CR_SNB_Pos);
    *control |= FLASH_CR_START;
    result = Boot_FlashWait(bank);
    *control &= ~(FLASH_CR_SER | FLASH_CR_SNB);
    Boot_FlashLock();
    return result;
}

static int Boot_FlashProgram(uint32_t address,
                             const uint8_t data[OTA_FLASHWORD_SIZE])
{
    uint32_t bank =
        address < (OTA_FLASH_BASE + OTA_FLASH_BANK_SIZE)
            ? FLASH_BANK_1
            : FLASH_BANK_2;
    volatile uint32_t *control =
        bank == FLASH_BANK_1 ? &FLASH->CR1 : &FLASH->CR2;
    volatile uint32_t *destination =
        (volatile uint32_t *)(uintptr_t)address;
    const uint32_t *source = (const uint32_t *)(const void *)data;
    uint32_t index;
    int result;

    if (((address % OTA_FLASHWORD_SIZE) != 0U) ||
        (((uintptr_t)data % sizeof(uint32_t)) != 0U) ||
        (Boot_FlashUnlock() != 0) ||
        (Boot_FlashWait(bank) != 0))
    {
        Boot_FlashLock();
        return -1;
    }

    if (bank == FLASH_BANK_1)
    {
        FLASH->CCR1 =
            BOOT_FLASH_ERROR_MASK_BANK1 | FLASH_SR_EOP;
    }
    else
    {
        FLASH->CCR2 =
            BOOT_FLASH_ERROR_MASK_BANK2 | FLASH_SR_EOP;
    }
    *control |= FLASH_CR_PG;
    __DSB();
    __ISB();
    for (index = 0U; index < 8U; index++)
    {
        destination[index] = source[index];
    }
    __DSB();
    __ISB();
    result = Boot_FlashWait(bank);
    *control &= ~FLASH_CR_PG;
    Boot_FlashLock();
    if ((result != 0) ||
        (memcmp((const void *)(uintptr_t)address,
                data,
                OTA_FLASHWORD_SIZE) != 0))
    {
        return -1;
    }
    return 0;
}

static int Boot_ControlAppend(OtaControlRecord *record)
{
    OtaControlRecord latest;
    OtaControlRecord write_record
        __attribute__((aligned(OTA_FLASHWORD_SIZE)));
    uint32_t address = 0U;
    uint32_t index;
    uint32_t offset;
    int found = Boot_LoadLatest(&latest);

    for (index = 0U; index < BOOT_CONTROL_RECORD_COUNT; index++)
    {
        uint32_t candidate =
            OTA_CONTROL_LOG_ADDRESS +
            (index * OTA_CONTROL_RECORD_SIZE);
        if (Boot_RecordErased(candidate))
        {
            address = candidate;
            break;
        }
    }
    if (address == 0U)
    {
        return -1;
    }

    write_record = *record;
    write_record.magic = OTA_CONTROL_MAGIC;
    write_record.storage_format = OTA_CONTROL_FORMAT;
    write_record.record_size = OTA_CONTROL_RECORD_SIZE;
    write_record.sequence = found ? latest.sequence + 1U : 1U;
    write_record.crc32 =
        Boot_Crc32((const uint8_t *)&write_record,
                   offsetof(OtaControlRecord, crc32));
    write_record.committed = OTA_CONTROL_COMMITTED;

    for (offset = 0U;
         offset < OTA_CONTROL_RECORD_SIZE - OTA_FLASHWORD_SIZE;
         offset += OTA_FLASHWORD_SIZE)
    {
        if (Boot_FlashProgram(address + offset,
                              ((const uint8_t *)&write_record) +
                                  offset) != 0)
        {
            return -1;
        }
    }
    if (Boot_FlashProgram(
            address + OTA_CONTROL_RECORD_SIZE - OTA_FLASHWORD_SIZE,
            ((const uint8_t *)&write_record) +
                OTA_CONTROL_RECORD_SIZE - OTA_FLASHWORD_SIZE) != 0)
    {
        return -1;
    }
    *record = write_record;
    return 0;
}

static int Boot_HeaderValid(const OtaImageHeader *header)
{
    uint8_t digest_nonzero = 0U;
    uint8_t signature_nonzero = 0U;
    uint32_t index;

    if ((header->magic != OTA_IMAGE_HEADER_MAGIC) ||
        (header->format != OTA_IMAGE_HEADER_FORMAT) ||
        (header->header_size != OTA_IMAGE_HEADER_SIZE) ||
        (header->target_id != OTA_IMAGE_TARGET_STM32H753) ||
        (header->image_version == 0U) ||
        (header->image_size < 8U) ||
        (header->image_size > OTA_IMAGE_MAX_PAYLOAD_SIZE) ||
        (header->payload_offset != OTA_IMAGE_HEADER_SIZE) ||
        (header->primary_vector_address != OTA_PRIMARY_IMAGE_ADDRESS) ||
        (header->header_crc32 !=
         Boot_Crc32((const uint8_t *)header,
                    offsetof(OtaImageHeader, header_crc32))))
    {
        return 0;
    }
    for (index = 0U; index < OTA_SHA256_SIZE; index++)
    {
        digest_nonzero |= header->payload_sha256[index];
    }
    for (index = 0U; index < OTA_P256_SIGNATURE_SIZE; index++)
    {
        signature_nonzero |= header->signature[index];
    }
    return (digest_nonzero != 0U) && (signature_nonzero != 0U);
}

static int Boot_VectorValid(uint32_t slot,
                            const OtaImageHeader *header)
{
    const uint32_t *vectors =
        (const uint32_t *)(uintptr_t)(slot + header->payload_offset);
    uint32_t stack = vectors[0];
    uint32_t reset = vectors[1];
    int stack_valid =
        ((stack > 0x20000000UL) && (stack <= 0x20020000UL)) ||
        ((stack > 0x24000000UL) && (stack <= 0x24080000UL)) ||
        ((stack > 0x30000000UL) && (stack <= 0x30048000UL)) ||
        ((stack > 0x38000000UL) && (stack <= 0x38010000UL));

    return stack_valid &&
           ((stack & 0x7U) == 0U) &&
           ((reset & 1U) != 0U) &&
           ((reset & ~1UL) >= OTA_PRIMARY_IMAGE_ADDRESS) &&
           ((reset & ~1UL) <
            OTA_PRIMARY_IMAGE_ADDRESS + header->image_size);
}

static int Boot_VerifySignature(const OtaImageHeader *header)
{
#if FIRMWARE_TRUST_ANCHOR_CONFIGURED
    uint8_t *field;
    mbedtls_sha256_context sha;
    mbedtls_ecp_group group;
    mbedtls_ecp_point point;
    mbedtls_mpi r;
    mbedtls_mpi s;
    uint8_t fields[12];
    uint8_t digest[OTA_SHA256_SIZE];
    int result;

    field = fields;
    field[0] = (uint8_t)(header->target_id >> 24U);
    field[1] = (uint8_t)(header->target_id >> 16U);
    field[2] = (uint8_t)(header->target_id >> 8U);
    field[3] = (uint8_t)header->target_id;
    field += 4U;
    field[0] = (uint8_t)(header->image_version >> 24U);
    field[1] = (uint8_t)(header->image_version >> 16U);
    field[2] = (uint8_t)(header->image_version >> 8U);
    field[3] = (uint8_t)header->image_version;
    field += 4U;
    field[0] = (uint8_t)(header->image_size >> 24U);
    field[1] = (uint8_t)(header->image_size >> 16U);
    field[2] = (uint8_t)(header->image_size >> 8U);
    field[3] = (uint8_t)header->image_size;
    mbedtls_sha256_init(&sha);
    result = mbedtls_sha256_starts_ret(&sha, 0);
    if (result == 0)
    {
        result = mbedtls_sha256_update_ret(
            &sha,
            (const uint8_t *)OTA_IMAGE_SIGNING_CONTEXT,
            OTA_IMAGE_SIGNING_CONTEXT_SIZE);
    }
    if (result == 0)
    {
        result = mbedtls_sha256_update_ret(&sha,
                                           fields,
                                           sizeof(fields));
    }
    if (result == 0)
    {
        result = mbedtls_sha256_update_ret(&sha,
                                           header->payload_sha256,
                                           OTA_SHA256_SIZE);
    }
    if (result == 0)
    {
        result = mbedtls_sha256_finish_ret(&sha, digest);
    }
    mbedtls_sha256_free(&sha);
    if (result != 0)
    {
        return -1;
    }

    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&point);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    result = mbedtls_ecp_group_load(&group,
                                    MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0)
    {
        result = mbedtls_ecp_point_read_binary(
            &group,
            &point,
            firmware_trust_anchor_public_key,
            sizeof(firmware_trust_anchor_public_key));
    }
    if (result == 0)
    {
        result = mbedtls_ecp_check_pubkey(&group, &point);
    }
    if (result == 0)
    {
        result = mbedtls_mpi_read_binary(&r,
                                         header->signature,
                                         32U);
    }
    if (result == 0)
    {
        result = mbedtls_mpi_read_binary(&s,
                                         header->signature + 32U,
                                         32U);
    }
    if (result == 0)
    {
        result = mbedtls_ecdsa_verify(&group,
                                      digest,
                                      sizeof(digest),
                                      &point,
                                      &r,
                                      &s);
    }
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&point);
    mbedtls_ecp_group_free(&group);
    memset(digest, 0, sizeof(digest));
    return result == 0 ? 0 : -1;
#else
    (void)header;
    return -1;
#endif
}

static int Boot_VerifyImage(uint32_t slot,
                            OtaImageDescriptor *descriptor)
{
    OtaImageHeader header;
    mbedtls_sha256_context sha;
    uint8_t digest[OTA_SHA256_SIZE];
    const uint8_t *payload;
    uint32_t offset = 0U;
    int result;

    memcpy(&header, (const void *)(uintptr_t)slot, sizeof(header));
    if (!Boot_HeaderValid(&header) ||
        !Boot_VectorValid(slot, &header))
    {
        return -1;
    }
    payload = (const uint8_t *)(uintptr_t)
        (slot + header.payload_offset);
    mbedtls_sha256_init(&sha);
    result = mbedtls_sha256_starts_ret(&sha, 0);
    while ((result == 0) && (offset < header.image_size))
    {
        size_t chunk = header.image_size - offset;
        if (chunk > 4096U)
        {
            chunk = 4096U;
        }
        result = mbedtls_sha256_update_ret(&sha,
                                           payload + offset,
                                           chunk);
        Boot_WatchdogRefresh();
        offset += (uint32_t)chunk;
    }
    if (result == 0)
    {
        result = mbedtls_sha256_finish_ret(&sha, digest);
    }
    mbedtls_sha256_free(&sha);
    if ((result != 0) ||
        (memcmp(digest,
                header.payload_sha256,
                sizeof(digest)) != 0) ||
        (Boot_VerifySignature(&header) != 0))
    {
        return -1;
    }

    descriptor->magic = OTA_IMAGE_DESCRIPTOR_MAGIC;
    descriptor->image_size = header.image_size;
    descriptor->image_version = header.image_version;
    memcpy(descriptor->sha256,
           header.payload_sha256,
           sizeof(descriptor->sha256));
    memcpy(descriptor->signature,
           header.signature,
           sizeof(descriptor->signature));
    return 0;
}

static int Boot_CopySector(uint32_t source, uint32_t destination)
{
    uint8_t flashword[OTA_FLASHWORD_SIZE]
        __attribute__((aligned(OTA_FLASHWORD_SIZE)));
    uint32_t offset;

    for (offset = 0U;
         offset < OTA_FLASH_SECTOR_SIZE;
         offset += OTA_FLASHWORD_SIZE)
    {
        memcpy(flashword,
               (const void *)(uintptr_t)(source + offset),
               sizeof(flashword));
        if (Boot_FlashProgram(destination + offset,
                              flashword) != 0)
        {
            return -1;
        }
        Boot_WatchdogRefresh();
    }
    return memcmp((const void *)(uintptr_t)source,
                  (const void *)(uintptr_t)destination,
                  OTA_FLASH_SECTOR_SIZE) == 0
               ? 0
               : -1;
}

static int Boot_StartSwap(OtaControlRecord *record,
                          uint32_t direction)
{
    record->state = OTA_CONTROL_STATE_SWAPPING;
    record->swap_direction = direction;
    record->swap_sector = 0U;
    record->swap_step = OTA_SWAP_STEP_PRIMARY_TO_SCRATCH;
    return Boot_ControlAppend(record);
}

static int Boot_PerformSwap(OtaControlRecord *record)
{
    while ((record->state == OTA_CONTROL_STATE_SWAPPING) &&
           (record->swap_sector < OTA_SLOT_SECTOR_COUNT))
    {
        uint32_t primary =
            OTA_PRIMARY_SLOT_ADDRESS +
            (record->swap_sector * OTA_FLASH_SECTOR_SIZE);
        uint32_t secondary =
            OTA_SECONDARY_SLOT_ADDRESS +
            (record->swap_sector * OTA_FLASH_SECTOR_SIZE);

        if (record->swap_step ==
            OTA_SWAP_STEP_PRIMARY_TO_SCRATCH)
        {
            if ((Boot_FlashEraseSector(FLASH_BANK_2,
                                       OTA_SCRATCH_SECTOR) != 0) ||
                (Boot_CopySector(primary, OTA_SCRATCH_ADDRESS) != 0))
            {
                return -1;
            }
            record->swap_step =
                OTA_SWAP_STEP_SECONDARY_TO_PRIMARY;
            if (Boot_ControlAppend(record) != 0)
            {
                return -1;
            }
        }
        else if (record->swap_step ==
                 OTA_SWAP_STEP_SECONDARY_TO_PRIMARY)
        {
            if ((Boot_FlashEraseSector(
                     FLASH_BANK_1,
                     OTA_PRIMARY_FIRST_SECTOR +
                         record->swap_sector) != 0) ||
                (Boot_CopySector(secondary, primary) != 0))
            {
                return -1;
            }
            record->swap_step =
                OTA_SWAP_STEP_SCRATCH_TO_SECONDARY;
            if (Boot_ControlAppend(record) != 0)
            {
                return -1;
            }
        }
        else if (record->swap_step ==
                 OTA_SWAP_STEP_SCRATCH_TO_SECONDARY)
        {
            OtaImageDescriptor temporary;

            if ((Boot_FlashEraseSector(
                     FLASH_BANK_2,
                     OTA_SECONDARY_FIRST_SECTOR +
                         record->swap_sector) != 0) ||
                (Boot_CopySector(OTA_SCRATCH_ADDRESS,
                                 secondary) != 0))
            {
                return -1;
            }

            record->swap_sector++;
            record->swap_step =
                OTA_SWAP_STEP_PRIMARY_TO_SCRATCH;
            if (record->swap_sector < OTA_SLOT_SECTOR_COUNT)
            {
                if (Boot_ControlAppend(record) != 0)
                {
                    return -1;
                }
                continue;
            }

            temporary = record->primary;
            record->primary = record->secondary;
            record->secondary = temporary;
            record->swap_sector = 0U;
            record->swap_step = 0U;
            record->trial_boot_count = 0U;
            if (record->swap_direction ==
                OTA_SWAP_DIRECTION_FORWARD)
            {
                record->state = OTA_CONTROL_STATE_TRIAL;
            }
            else
            {
                record->state = OTA_CONTROL_STATE_CONFIRMED;
                record->confirmed_version =
                    record->primary.image_version;
                record->flags |=
                    OTA_CONTROL_FLAG_JOB_REPORT_PENDING |
                    OTA_CONTROL_FLAG_ROLLBACK_PERFORMED;
            }
            return Boot_ControlAppend(record);
        }
        else
        {
            return -1;
        }
    }
    return record->state == OTA_CONTROL_STATE_SWAPPING ? -1 : 0;
}

__attribute__((noreturn))
static void Boot_JumpToPrimary(void)
{
    const uint32_t *vectors =
        (const uint32_t *)(uintptr_t)OTA_PRIMARY_IMAGE_ADDRESS;
    void (*application_reset)(void) =
        (void (*)(void))(uintptr_t)vectors[1];

    __disable_irq();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;
    SCB->VTOR = OTA_PRIMARY_IMAGE_ADDRESS;
    __set_CONTROL(0U);
    __set_BASEPRI(0U);
    __set_FAULTMASK(0U);
    __set_MSP(vectors[0]);
    __DSB();
    __ISB();
    __enable_irq();
    application_reset();
    for (;;)
    {
        __WFI();
    }
}

__attribute__((noreturn))
static void Boot_Fatal(void)
{
    for (;;)
    {
        __WFI();
    }
}

void Boot_Main(void)
{
    OtaControlRecord record;
    OtaImageDescriptor primary;
    OtaImageDescriptor secondary;
    int have_record;

    __disable_irq();
    SCB->VTOR = OTA_BOOTLOADER_ADDRESS;
    have_record = Boot_LoadLatest(&record);

    if (have_record &&
        (record.state == OTA_CONTROL_STATE_CANDIDATE_READY))
    {
        if ((Boot_VerifyImage(OTA_PRIMARY_SLOT_ADDRESS,
                              &primary) != 0) ||
            (memcmp(&primary,
                    &record.primary,
                    sizeof(primary)) != 0))
        {
            Boot_Fatal();
        }
        if ((Boot_VerifyImage(OTA_SECONDARY_SLOT_ADDRESS,
                              &secondary) != 0) ||
            (memcmp(&secondary,
                    &record.secondary,
                    sizeof(secondary)) != 0) ||
            (secondary.image_version <= record.confirmed_version))
        {
            record.state = OTA_CONTROL_STATE_REJECTED;
            record.flags |=
                OTA_CONTROL_FLAG_JOB_REPORT_PENDING |
                OTA_CONTROL_FLAG_INSTALL_REJECTED;
            record.flags &= ~OTA_CONTROL_FLAG_ROLLBACK_PERFORMED;
            record.swap_sector = 0U;
            record.swap_step = 0U;
            if (Boot_ControlAppend(&record) != 0)
            {
                Boot_Fatal();
            }
        }
        else if (Boot_StartSwap(&record,
                                OTA_SWAP_DIRECTION_FORWARD) != 0)
        {
            Boot_Fatal();
        }
    }

    if (have_record &&
        (record.state == OTA_CONTROL_STATE_SWAPPING))
    {
        if (Boot_PerformSwap(&record) != 0)
        {
            Boot_Fatal();
        }
    }

    if (have_record &&
        (record.state == OTA_CONTROL_STATE_TRIAL))
    {
        if ((Boot_VerifyImage(OTA_PRIMARY_SLOT_ADDRESS,
                              &primary) != 0) ||
            (memcmp(&primary,
                    &record.primary,
                    sizeof(primary)) != 0) ||
            (record.trial_boot_count >=
             OTA_TRIAL_MAX_BOOT_ATTEMPTS))
        {
            if ((Boot_VerifyImage(OTA_SECONDARY_SLOT_ADDRESS,
                                  &secondary) != 0) ||
                (memcmp(&secondary,
                        &record.secondary,
                        sizeof(secondary)) != 0) ||
                (Boot_StartSwap(&record,
                                OTA_SWAP_DIRECTION_ROLLBACK) != 0) ||
                (Boot_PerformSwap(&record) != 0))
            {
                Boot_Fatal();
            }
        }
        else
        {
            record.trial_boot_count++;
            if (Boot_ControlAppend(&record) != 0)
            {
                Boot_Fatal();
            }
        }
    }

    if ((Boot_VerifyImage(OTA_PRIMARY_SLOT_ADDRESS,
                          &primary) != 0) ||
        (have_record &&
         (record.state == OTA_CONTROL_STATE_CONFIRMED) &&
         (record.primary.magic == OTA_IMAGE_DESCRIPTOR_MAGIC) &&
         (memcmp(&primary,
                 &record.primary,
                 sizeof(primary)) != 0)) ||
        (have_record &&
         (record.confirmed_version != 0U) &&
         (primary.image_version < record.confirmed_version)))
    {
        Boot_Fatal();
    }
    Boot_JumpToPrimary();
}

void *_sbrk(ptrdiff_t increment)
{
    uint8_t *previous = boot_heap;
    uint8_t *next = boot_heap + increment;

    if ((increment < 0) || (next > &__heap_end__))
    {
        return (void *)-1;
    }
    boot_heap = next;
    return previous;
}
