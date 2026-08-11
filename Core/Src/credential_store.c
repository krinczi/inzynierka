#include "credential_store.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The last two sectors of Bank 1 are outside the executable linker region.
 * Records are appended in the active sector. When it is full, the other
 * sector is erased and becomes active while the last verified generation
 * remains intact in the old sector. This preserves a rollback record during
 * multi-step credential rotation and minimizes sector erases. Firmware
 * updates do not touch either credential sector.
 */
#define CREDENTIAL_SLOT_A_ADDRESS       0x080C0000UL
#define CREDENTIAL_SLOT_B_ADDRESS       0x080E0000UL
#define CREDENTIAL_SLOT_A_SECTOR        FLASH_SECTOR_6
#define CREDENTIAL_SLOT_B_SECTOR        FLASH_SECTOR_7
#define CREDENTIAL_FLASH_MAGIC          0x43524431UL /* "CRD1" */
#define CREDENTIAL_STORAGE_FORMAT_LEGACY 1U
#define CREDENTIAL_STORAGE_FORMAT        2U
#define CREDENTIAL_FLASHWORD_SIZE       32U
#define CREDENTIAL_RECORD_SIZE          8192U
#define CREDENTIAL_SECTOR_SIZE          (128U * 1024U)
#define CREDENTIAL_RECORDS_PER_SECTOR   \
    (CREDENTIAL_SECTOR_SIZE / CREDENTIAL_RECORD_SIZE)
#define CREDENTIAL_PROGRAM_YIELD_WORDS     8U
#define CREDENTIAL_ROTATION_MAX_VALIDATION_FAILURES 3U

typedef struct
{
    uint32_t magic;
    uint16_t storage_format;
    uint16_t header_size;
    uint32_t generation;
    uint32_t state;
    uint16_t certificate_pem_length;
    uint16_t private_key_pem_length;
    uint16_t ownership_token_length;
    uint16_t certificate_id_length;
    uint16_t thing_name_length;
    uint16_t reserved;
    uint32_t crc32;
} CredentialFlashHeader;

typedef struct
{
    CredentialFlashHeader header;
    uint8_t payload[CREDENTIAL_RECORD_SIZE - sizeof(CredentialFlashHeader)];
} CredentialFlashRecord;

_Static_assert(sizeof(CredentialFlashHeader) == CREDENTIAL_FLASHWORD_SIZE,
               "Credential header must occupy one Flash word");
_Static_assert(sizeof(CredentialFlashRecord) == CREDENTIAL_RECORD_SIZE,
               "Credential record has an invalid size");
_Static_assert((sizeof(CredentialFlashRecord) % CREDENTIAL_FLASHWORD_SIZE) == 0U,
               "Credential record must be Flash-word aligned");

static CredentialStoreState current_state;
static CredentialStoreData current_data;
static uint32_t current_generation;
static uint32_t active_slot_address;
static uint16_t current_rotation_validation_failures;
static CredentialFlashRecord credential_write_record
    __attribute__((aligned(CREDENTIAL_FLASHWORD_SIZE)));

static int CredentialStore_RecordIsValid(const CredentialFlashRecord *record);

static void CredentialStore_SecureZero(void *buffer, size_t length)
{
    volatile uint8_t *cursor = (volatile uint8_t *)buffer;

    while (length-- > 0U)
    {
        *cursor++ = 0U;
    }
    __DSB();
}

static void CredentialStore_YieldToSupervisor(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
    {
        taskYIELD();
    }
}

static uint32_t CredentialStore_Crc32Update(uint32_t crc,
                                            const uint8_t *data,
                                            size_t length)
{
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

    return crc;
}

static uint32_t CredentialStore_RecordCrc(const CredentialFlashRecord *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    const size_t crc_offset = offsetof(CredentialFlashRecord, header.crc32);
    const uint32_t zero_crc_field = 0U;
    uint32_t crc = 0xFFFFFFFFUL;

    crc = CredentialStore_Crc32Update(crc, bytes, crc_offset);
    crc = CredentialStore_Crc32Update(crc,
                                      (const uint8_t *)&zero_crc_field,
                                      sizeof(zero_crc_field));
    crc = CredentialStore_Crc32Update(
        crc,
        bytes + crc_offset + sizeof(zero_crc_field),
        sizeof(*record) - crc_offset - sizeof(zero_crc_field));
    return ~crc;
}

static void CredentialStore_InvalidateCache(uint32_t address, size_t length)
{
    uintptr_t aligned_start;
    uintptr_t aligned_end;

    if ((length == 0U) || ((SCB->CCR & SCB_CCR_DC_Msk) == 0U))
    {
        return;
    }

    aligned_start = (uintptr_t)address & ~(uintptr_t)(CREDENTIAL_FLASHWORD_SIZE - 1U);
    aligned_end = ((uintptr_t)address + length + CREDENTIAL_FLASHWORD_SIZE - 1U) &
                  ~(uintptr_t)(CREDENTIAL_FLASHWORD_SIZE - 1U);
    SCB_InvalidateDCache_by_Addr((uint32_t *)aligned_start,
                                (int32_t)(aligned_end - aligned_start));
    __DSB();
    __ISB();
}

static int CredentialStore_AddressInSector(uint32_t address, uint32_t sector_base)
{
    return (address >= sector_base) &&
           (address < (sector_base + CREDENTIAL_SECTOR_SIZE));
}

static int CredentialStore_RecordIsErased(uint32_t address)
{
    const uint32_t *word = (const uint32_t *)(uintptr_t)address;
    const uint32_t *end =
        (const uint32_t *)(uintptr_t)(address + CREDENTIAL_RECORD_SIZE);

    CredentialStore_InvalidateCache(address, CREDENTIAL_RECORD_SIZE);
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

static uint32_t CredentialStore_FindErasedRecord(uint32_t sector_base)
{
    uint32_t index;

    for (index = 0U; index < CREDENTIAL_RECORDS_PER_SECTOR; index++)
    {
        uint32_t address =
            sector_base + (index * CREDENTIAL_RECORD_SIZE);
        if (CredentialStore_RecordIsErased(address))
        {
            return address;
        }
    }
    return 0U;
}

static const CredentialFlashRecord *CredentialStore_FindPreviousProvisioned(void)
{
    const CredentialFlashRecord *selected = NULL;
    const uint32_t sector_bases[] = {
        CREDENTIAL_SLOT_A_ADDRESS,
        CREDENTIAL_SLOT_B_ADDRESS
    };
    size_t sector_index;
    uint32_t record_index;

    CredentialStore_InvalidateCache(CREDENTIAL_SLOT_A_ADDRESS,
                                    CREDENTIAL_SECTOR_SIZE);
    CredentialStore_InvalidateCache(CREDENTIAL_SLOT_B_ADDRESS,
                                    CREDENTIAL_SECTOR_SIZE);

    for (sector_index = 0U;
         sector_index < (sizeof(sector_bases) / sizeof(sector_bases[0]));
         sector_index++)
    {
        for (record_index = 0U;
             record_index < CREDENTIAL_RECORDS_PER_SECTOR;
             record_index++)
        {
            uint32_t address = sector_bases[sector_index] +
                               (record_index * CREDENTIAL_RECORD_SIZE);
            const CredentialFlashRecord *record =
                (const CredentialFlashRecord *)(uintptr_t)address;

            if (CredentialStore_RecordIsValid(record) &&
                (record->header.state == CREDENTIAL_STORE_PROVISIONED) &&
                (record->header.generation < current_generation) &&
                ((selected == NULL) ||
                 (record->header.generation > selected->header.generation)))
            {
                selected = record;
            }
        }
    }

    return selected;
}

static int CredentialStore_LengthsAreValid(const CredentialFlashHeader *header)
{
    size_t total;

    if ((header->certificate_pem_length == 0U) ||
        (header->certificate_pem_length > CREDENTIAL_STORE_CERTIFICATE_PEM_MAX_LENGTH) ||
        (header->private_key_pem_length == 0U) ||
        (header->private_key_pem_length > CREDENTIAL_STORE_PRIVATE_KEY_PEM_MAX_LENGTH) ||
        (header->ownership_token_length > CREDENTIAL_STORE_OWNERSHIP_TOKEN_MAX_LENGTH) ||
        (header->certificate_id_length == 0U) ||
        (header->certificate_id_length > CREDENTIAL_STORE_CERTIFICATE_ID_MAX_LENGTH) ||
        (header->thing_name_length > CREDENTIAL_STORE_THING_NAME_MAX_LENGTH))
    {
        return 0;
    }

    if (((header->state == CREDENTIAL_STORE_PROVISIONED) ||
         (header->state == CREDENTIAL_STORE_ROTATION_REQUESTED) ||
         (header->state == CREDENTIAL_STORE_ROTATION_PENDING) ||
         (header->state == CREDENTIAL_STORE_ROTATION_VALIDATING)) &&
        (header->thing_name_length == 0U))
    {
        return 0;
    }
    if (((header->state == CREDENTIAL_STORE_PENDING_REGISTRATION) ||
         (header->state == CREDENTIAL_STORE_ROTATION_PENDING)) &&
        (header->ownership_token_length == 0U))
    {
        return 0;
    }

    total = (size_t)header->certificate_pem_length + 1U +
            (size_t)header->private_key_pem_length + 1U +
            (size_t)header->ownership_token_length + 1U +
            (size_t)header->certificate_id_length + 1U +
            (size_t)header->thing_name_length + 1U;
    return total <= sizeof(((CredentialFlashRecord *)0)->payload);
}

static int CredentialStore_RecordIsValid(const CredentialFlashRecord *record)
{
    if ((record->header.magic != CREDENTIAL_FLASH_MAGIC) ||
        ((record->header.storage_format != CREDENTIAL_STORAGE_FORMAT_LEGACY) &&
         (record->header.storage_format != CREDENTIAL_STORAGE_FORMAT)) ||
        (record->header.header_size != sizeof(CredentialFlashHeader)) ||
        ((record->header.state != CREDENTIAL_STORE_PENDING_REGISTRATION) &&
         (record->header.state != CREDENTIAL_STORE_PROVISIONED) &&
         (record->header.state != CREDENTIAL_STORE_ROTATION_REQUESTED) &&
         (record->header.state != CREDENTIAL_STORE_ROTATION_PENDING) &&
         (record->header.state != CREDENTIAL_STORE_ROTATION_VALIDATING)) ||
        !CredentialStore_LengthsAreValid(&record->header))
    {
        return 0;
    }

    return record->header.crc32 == CredentialStore_RecordCrc(record);
}

static void CredentialStore_RecordToData(const CredentialFlashRecord *record,
                                         CredentialStoreData *data)
{
    const uint8_t *cursor = record->payload;

    memset(data, 0, sizeof(*data));
    memcpy(data->certificate_pem, cursor, record->header.certificate_pem_length);
    cursor += record->header.certificate_pem_length + 1U;
    memcpy(data->private_key_pem, cursor, record->header.private_key_pem_length);
    cursor += record->header.private_key_pem_length + 1U;
    memcpy(data->ownership_token, cursor, record->header.ownership_token_length);
    cursor += record->header.ownership_token_length + 1U;
    memcpy(data->certificate_id, cursor, record->header.certificate_id_length);
    cursor += record->header.certificate_id_length + 1U;
    memcpy(data->thing_name, cursor, record->header.thing_name_length);
}

static int CredentialStore_PrepareRecord(CredentialStoreState state,
                                         const CredentialStoreData *data,
                                         CredentialFlashRecord *record)
{
    uint8_t *cursor;
    size_t certificate_length;
    size_t private_key_length;
    size_t ownership_token_length;
    size_t certificate_id_length;
    size_t thing_name_length;

    if (data == NULL)
    {
        return -1;
    }

    certificate_length = strnlen(data->certificate_pem,
                                 CREDENTIAL_STORE_CERTIFICATE_PEM_MAX_LENGTH + 1U);
    private_key_length = strnlen(data->private_key_pem,
                                 CREDENTIAL_STORE_PRIVATE_KEY_PEM_MAX_LENGTH + 1U);
    ownership_token_length = strnlen(data->ownership_token,
                                     CREDENTIAL_STORE_OWNERSHIP_TOKEN_MAX_LENGTH + 1U);
    certificate_id_length = strnlen(data->certificate_id,
                                    CREDENTIAL_STORE_CERTIFICATE_ID_MAX_LENGTH + 1U);
    thing_name_length = strnlen(data->thing_name,
                                CREDENTIAL_STORE_THING_NAME_MAX_LENGTH + 1U);

    if ((certificate_length == 0U) ||
        (certificate_length > CREDENTIAL_STORE_CERTIFICATE_PEM_MAX_LENGTH) ||
        (private_key_length == 0U) ||
        (private_key_length > CREDENTIAL_STORE_PRIVATE_KEY_PEM_MAX_LENGTH) ||
        (ownership_token_length > CREDENTIAL_STORE_OWNERSHIP_TOKEN_MAX_LENGTH) ||
        (certificate_id_length == 0U) ||
        (certificate_id_length > CREDENTIAL_STORE_CERTIFICATE_ID_MAX_LENGTH) ||
        (thing_name_length > CREDENTIAL_STORE_THING_NAME_MAX_LENGTH) ||
        (((state == CREDENTIAL_STORE_PENDING_REGISTRATION) ||
          (state == CREDENTIAL_STORE_ROTATION_PENDING)) &&
         (ownership_token_length == 0U)) ||
        (((state == CREDENTIAL_STORE_PROVISIONED) ||
          (state == CREDENTIAL_STORE_ROTATION_REQUESTED) ||
          (state == CREDENTIAL_STORE_ROTATION_PENDING) ||
          (state == CREDENTIAL_STORE_ROTATION_VALIDATING)) &&
         (thing_name_length == 0U)))
    {
        return -1;
    }

    /*
     * Keep the unused tail erased. The CRC still protects the complete record,
     * but only the occupied Flash words have to be programmed.
     */
    memset(record, 0xFF, sizeof(*record));
    record->header.magic = CREDENTIAL_FLASH_MAGIC;
    record->header.storage_format = CREDENTIAL_STORAGE_FORMAT;
    record->header.header_size = sizeof(CredentialFlashHeader);
    record->header.generation = current_generation + 1U;
    record->header.state = (uint32_t)state;
    record->header.certificate_pem_length = (uint16_t)certificate_length;
    record->header.private_key_pem_length = (uint16_t)private_key_length;
    record->header.ownership_token_length = (uint16_t)ownership_token_length;
    record->header.certificate_id_length = (uint16_t)certificate_id_length;
    record->header.thing_name_length = (uint16_t)thing_name_length;
    record->header.reserved =
        (state == CREDENTIAL_STORE_ROTATION_VALIDATING)
            ? current_rotation_validation_failures
            : 0U;

    cursor = record->payload;
#define CREDENTIAL_COPY_FIELD(source, length)       \
    do                                                \
    {                                                 \
        memcpy(cursor, (source), (length));           \
        cursor[(length)] = '\0';                     \
        cursor += (length) + 1U;                      \
    } while (0)
    CREDENTIAL_COPY_FIELD(data->certificate_pem, certificate_length);
    CREDENTIAL_COPY_FIELD(data->private_key_pem, private_key_length);
    CREDENTIAL_COPY_FIELD(data->ownership_token, ownership_token_length);
    CREDENTIAL_COPY_FIELD(data->certificate_id, certificate_id_length);
    CREDENTIAL_COPY_FIELD(data->thing_name, thing_name_length);
#undef CREDENTIAL_COPY_FIELD

    record->header.crc32 = CredentialStore_RecordCrc(record);
    return 0;
}

static size_t CredentialStore_ProgrammedLength(const CredentialFlashRecord *record)
{
    size_t length = sizeof(record->header) +
                    (size_t)record->header.certificate_pem_length + 1U +
                    (size_t)record->header.private_key_pem_length + 1U +
                    (size_t)record->header.ownership_token_length + 1U +
                    (size_t)record->header.certificate_id_length + 1U +
                    (size_t)record->header.thing_name_length + 1U;

    return (length + CREDENTIAL_FLASHWORD_SIZE - 1U) &
           ~(size_t)(CREDENTIAL_FLASHWORD_SIZE - 1U);
}

static int CredentialStore_Write(CredentialStoreState state,
                                 const CredentialStoreData *data)
{
    CredentialFlashRecord *record = &credential_write_record;
    const CredentialFlashRecord *persisted_record;
    FLASH_EraseInitTypeDef erase = { 0 };
    uint32_t sector_error = 0xFFFFFFFFUL;
    uint32_t target_address;
    uint32_t target_sector;
    size_t programmed_length;
    size_t offset;
    HAL_StatusTypeDef status;

    if (CredentialStore_PrepareRecord(state, data, record) != 0)
    {
        return -1;
    }
    programmed_length = CredentialStore_ProgrammedLength(record);

    if (CredentialStore_AddressInSector(active_slot_address,
                                        CREDENTIAL_SLOT_A_ADDRESS))
    {
        target_address =
            CredentialStore_FindErasedRecord(CREDENTIAL_SLOT_A_ADDRESS);
        target_sector = CREDENTIAL_SLOT_A_SECTOR;
        if (target_address == 0U)
        {
            target_address =
                CredentialStore_FindErasedRecord(CREDENTIAL_SLOT_B_ADDRESS);
            target_sector = CREDENTIAL_SLOT_B_SECTOR;
        }
    }
    else if (CredentialStore_AddressInSector(active_slot_address,
                                             CREDENTIAL_SLOT_B_ADDRESS))
    {
        target_address =
            CredentialStore_FindErasedRecord(CREDENTIAL_SLOT_B_ADDRESS);
        target_sector = CREDENTIAL_SLOT_B_SECTOR;
        if (target_address == 0U)
        {
            target_address =
                CredentialStore_FindErasedRecord(CREDENTIAL_SLOT_A_ADDRESS);
            target_sector = CREDENTIAL_SLOT_A_SECTOR;
        }
    }
    else
    {
        target_address =
            CredentialStore_FindErasedRecord(CREDENTIAL_SLOT_A_ADDRESS);
        target_sector = CREDENTIAL_SLOT_A_SECTOR;
        if (target_address == 0U)
        {
            target_address =
                CredentialStore_FindErasedRecord(CREDENTIAL_SLOT_B_ADDRESS);
            target_sector = CREDENTIAL_SLOT_B_SECTOR;
        }
    }

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_1;
    erase.Sector = target_sector;
    erase.NbSectors = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        CredentialStore_SecureZero(record, sizeof(*record));
        return -1;
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1);
    if (target_address == 0U)
    {
        status = HAL_FLASHEx_Erase(&erase, &sector_error);
        target_address = (target_sector == CREDENTIAL_SLOT_A_SECTOR)
                             ? CREDENTIAL_SLOT_A_ADDRESS
                             : CREDENTIAL_SLOT_B_ADDRESS;
    }
    else
    {
        status = HAL_OK;
    }
    for (offset = 0U; (status == HAL_OK) && (offset < programmed_length);
         offset += CREDENTIAL_FLASHWORD_SIZE)
    {
        status = HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_FLASHWORD,
            target_address + (uint32_t)offset,
            (uint32_t)(uintptr_t)(((const uint8_t *)record) + offset));
        if ((((offset / CREDENTIAL_FLASHWORD_SIZE) + 1U) %
             CREDENTIAL_PROGRAM_YIELD_WORDS) == 0U)
        {
            CredentialStore_YieldToSupervisor();
        }
    }
    (void)HAL_FLASH_Lock();

    if ((status != HAL_OK) || (sector_error != 0xFFFFFFFFUL))
    {
        CredentialStore_SecureZero(record, sizeof(*record));
        return -1;
    }

    CredentialStore_InvalidateCache(target_address, sizeof(*record));
    persisted_record = (const CredentialFlashRecord *)(uintptr_t)target_address;
    if (!CredentialStore_RecordIsValid(persisted_record) ||
        (memcmp(persisted_record, record, sizeof(*record)) != 0))
    {
        CredentialStore_SecureZero(record, sizeof(*record));
        return -1;
    }

    current_state = state;
    current_data = *data;
    current_generation = record->header.generation;
    active_slot_address = target_address;
    current_rotation_validation_failures = record->header.reserved;
    CredentialStore_SecureZero(record, sizeof(*record));
    return 0;
}

int CredentialStore_Init(void)
{
    const CredentialFlashRecord *selected = NULL;
    const uint32_t sector_bases[] = {
        CREDENTIAL_SLOT_A_ADDRESS,
        CREDENTIAL_SLOT_B_ADDRESS
    };
    size_t sector_index;
    uint32_t record_index;

    current_state = CREDENTIAL_STORE_EMPTY;
    memset(&current_data, 0, sizeof(current_data));
    current_generation = 0U;
    active_slot_address = 0U;
    current_rotation_validation_failures = 0U;

    CredentialStore_InvalidateCache(CREDENTIAL_SLOT_A_ADDRESS,
                                    CREDENTIAL_SECTOR_SIZE);
    CredentialStore_InvalidateCache(CREDENTIAL_SLOT_B_ADDRESS,
                                    CREDENTIAL_SECTOR_SIZE);
    for (sector_index = 0U;
         sector_index < (sizeof(sector_bases) / sizeof(sector_bases[0]));
         sector_index++)
    {
        for (record_index = 0U;
             record_index < CREDENTIAL_RECORDS_PER_SECTOR;
             record_index++)
        {
            uint32_t address = sector_bases[sector_index] +
                               (record_index * CREDENTIAL_RECORD_SIZE);
            const CredentialFlashRecord *record =
                (const CredentialFlashRecord *)(uintptr_t)address;

            if (CredentialStore_RecordIsValid(record) &&
                ((selected == NULL) ||
                 (record->header.generation > selected->header.generation)))
            {
                selected = record;
                active_slot_address = address;
            }
        }
    }

    if (selected != NULL)
    {
        CredentialStoreData sanitized;
        current_state = (CredentialStoreState)selected->header.state;
        current_generation = selected->header.generation;
        current_rotation_validation_failures =
            ((selected->header.storage_format >= CREDENTIAL_STORAGE_FORMAT) &&
             (current_state == CREDENTIAL_STORE_ROTATION_VALIDATING))
                ? selected->header.reserved
                : 0U;
        CredentialStore_RecordToData(selected, &current_data);

        if ((current_state == CREDENTIAL_STORE_PROVISIONED) &&
            (current_data.ownership_token[0] != '\0'))
        {
            sanitized = current_data;
            CredentialStore_SecureZero(sanitized.ownership_token,
                                       sizeof(sanitized.ownership_token));
            (void)CredentialStore_Write(CREDENTIAL_STORE_PROVISIONED,
                                        &sanitized);
            CredentialStore_SecureZero(&sanitized, sizeof(sanitized));
        }
    }

    return 0;
}

CredentialStoreState CredentialStore_GetState(void)
{
    return current_state;
}

const CredentialStoreData *CredentialStore_Get(void)
{
    return &current_data;
}

const char *CredentialStore_StateName(CredentialStoreState state)
{
    switch (state)
    {
        case CREDENTIAL_STORE_PENDING_REGISTRATION:
            return "PENDING_REGISTRATION";
        case CREDENTIAL_STORE_PROVISIONED:
            return "PROVISIONED";
        case CREDENTIAL_STORE_ROTATION_REQUESTED:
            return "ROTATION_REQUESTED";
        case CREDENTIAL_STORE_ROTATION_PENDING:
            return "ROTATION_PENDING";
        case CREDENTIAL_STORE_ROTATION_VALIDATING:
            return "ROTATION_VALIDATING";
        default:
            return "EMPTY";
    }
}

int CredentialStore_UsesDeviceCredentials(void)
{
    return (current_state == CREDENTIAL_STORE_PROVISIONED) ||
           (current_state == CREDENTIAL_STORE_ROTATION_VALIDATING);
}

int CredentialStore_IsProvisioningRequired(void)
{
    return !CredentialStore_UsesDeviceCredentials();
}

int CredentialStore_IsRotationInProgress(void)
{
    return (current_state == CREDENTIAL_STORE_ROTATION_REQUESTED) ||
           (current_state == CREDENTIAL_STORE_ROTATION_PENDING) ||
           (current_state == CREDENTIAL_STORE_ROTATION_VALIDATING);
}

int CredentialStore_SavePending(const char *certificate_pem,
                                const char *private_key_pem,
                                const char *ownership_token,
                                const char *certificate_id)
{
    CredentialStoreData data;
    CredentialStoreState target_state = CREDENTIAL_STORE_PENDING_REGISTRATION;
    int result;

    if ((certificate_pem == NULL) || (private_key_pem == NULL) ||
        (ownership_token == NULL) || (certificate_id == NULL))
    {
        return -1;
    }

    memset(&data, 0, sizeof(data));
    if ((strnlen(certificate_pem, sizeof(data.certificate_pem)) >=
         sizeof(data.certificate_pem)) ||
        (strnlen(private_key_pem, sizeof(data.private_key_pem)) >=
         sizeof(data.private_key_pem)) ||
        (strnlen(ownership_token, sizeof(data.ownership_token)) >=
         sizeof(data.ownership_token)) ||
        (strnlen(certificate_id, sizeof(data.certificate_id)) >=
         sizeof(data.certificate_id)))
    {
        return -1;
    }

    strcpy(data.certificate_pem, certificate_pem);
    strcpy(data.private_key_pem, private_key_pem);
    strcpy(data.ownership_token, ownership_token);
    strcpy(data.certificate_id, certificate_id);

    if ((current_state == CREDENTIAL_STORE_ROTATION_REQUESTED) ||
        (current_state == CREDENTIAL_STORE_ROTATION_PENDING))
    {
        strcpy(data.thing_name, current_data.thing_name);
        target_state = CREDENTIAL_STORE_ROTATION_PENDING;
    }

    current_rotation_validation_failures = 0U;
    result = CredentialStore_Write(target_state, &data);
    CredentialStore_SecureZero(&data, sizeof(data));
    return result;
}

int CredentialStore_CommitProvisioned(const char *thing_name)
{
    CredentialStoreData data;
    CredentialStoreState target_state;
    int result;

    if (((current_state != CREDENTIAL_STORE_PENDING_REGISTRATION) &&
         (current_state != CREDENTIAL_STORE_ROTATION_PENDING)) ||
        (thing_name == NULL) ||
        (strnlen(thing_name, sizeof(data.thing_name)) >= sizeof(data.thing_name)))
    {
        return -1;
    }

    if ((current_state == CREDENTIAL_STORE_ROTATION_PENDING) &&
        (strcmp(thing_name, current_data.thing_name) != 0))
    {
        return -1;
    }

    target_state = (current_state == CREDENTIAL_STORE_ROTATION_PENDING)
                       ? CREDENTIAL_STORE_ROTATION_VALIDATING
                       : CREDENTIAL_STORE_PROVISIONED;
    data = current_data;
    strcpy(data.thing_name, thing_name);
    /*
     * The Fleet Provisioning ownership token is single-use and must not stay
     * in the long-lived provisioned record.
     */
    CredentialStore_SecureZero(data.ownership_token,
                               sizeof(data.ownership_token));
    current_rotation_validation_failures = 0U;
    result = CredentialStore_Write(target_state, &data);
    CredentialStore_SecureZero(&data, sizeof(data));
    return result;
}

int CredentialStore_RequestRotation(void)
{
    CredentialStoreData data;
    int result;

    if (current_state != CREDENTIAL_STORE_PROVISIONED)
    {
        return -1;
    }

    data = current_data;
    CredentialStore_SecureZero(data.ownership_token,
                               sizeof(data.ownership_token));
    current_rotation_validation_failures = 0U;
    result = CredentialStore_Write(CREDENTIAL_STORE_ROTATION_REQUESTED, &data);
    CredentialStore_SecureZero(&data, sizeof(data));
    return result;
}

int CredentialStore_CommitRotationValidated(void)
{
    CredentialStoreData data;
    int result;

    if (current_state != CREDENTIAL_STORE_ROTATION_VALIDATING)
    {
        return -1;
    }

    data = current_data;
    current_rotation_validation_failures = 0U;
    result = CredentialStore_Write(CREDENTIAL_STORE_PROVISIONED, &data);
    CredentialStore_SecureZero(&data, sizeof(data));
    return result;
}

int CredentialStore_RollbackRotation(void)
{
    const CredentialFlashRecord *fallback;
    CredentialStoreData data;
    int result;

    if (!CredentialStore_IsRotationInProgress())
    {
        return -1;
    }

    fallback = CredentialStore_FindPreviousProvisioned();
    if (fallback == NULL)
    {
        return -1;
    }

    CredentialStore_RecordToData(fallback, &data);
    CredentialStore_SecureZero(data.ownership_token,
                               sizeof(data.ownership_token));
    current_rotation_validation_failures = 0U;
    result = CredentialStore_Write(CREDENTIAL_STORE_PROVISIONED, &data);
    CredentialStore_SecureZero(&data, sizeof(data));
    return result;
}

int CredentialStore_RecordRotationFailure(void)
{
    CredentialStoreData data;
    uint16_t previous_failures;
    int result;

    if (current_state != CREDENTIAL_STORE_ROTATION_VALIDATING)
    {
        return 0;
    }

    if ((uint32_t)current_rotation_validation_failures + 1U >=
        CREDENTIAL_ROTATION_MAX_VALIDATION_FAILURES)
    {
        return CredentialStore_RollbackRotation() == 0 ? 1 : -1;
    }

    previous_failures = current_rotation_validation_failures;
    current_rotation_validation_failures++;
    data = current_data;
    result = CredentialStore_Write(CREDENTIAL_STORE_ROTATION_VALIDATING, &data);
    CredentialStore_SecureZero(&data, sizeof(data));
    if (result != 0)
    {
        current_rotation_validation_failures = previous_failures;
        return -1;
    }
    return 0;
}

int CredentialStore_Erase(void)
{
    FLASH_EraseInitTypeDef erase = { 0 };
    uint32_t sector_error = 0xFFFFFFFFUL;
    HAL_StatusTypeDef status;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_1;
    erase.Sector = CredentialStore_AddressInSector(
                       active_slot_address,
                       CREDENTIAL_SLOT_B_ADDRESS)
                       ? CREDENTIAL_SLOT_B_SECTOR
                       : CREDENTIAL_SLOT_A_SECTOR;
    erase.NbSectors = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return -1;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1);
    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    CredentialStore_YieldToSupervisor();
    if ((status == HAL_OK) && (sector_error == 0xFFFFFFFFUL))
    {
        erase.Sector = (erase.Sector == CREDENTIAL_SLOT_A_SECTOR)
                           ? CREDENTIAL_SLOT_B_SECTOR
                           : CREDENTIAL_SLOT_A_SECTOR;
        sector_error = 0xFFFFFFFFUL;
        status = HAL_FLASHEx_Erase(&erase, &sector_error);
    }
    (void)HAL_FLASH_Lock();
    if ((status != HAL_OK) || (sector_error != 0xFFFFFFFFUL))
    {
        return -1;
    }

    current_state = CREDENTIAL_STORE_EMPTY;
    memset(&current_data, 0, sizeof(current_data));
    current_generation = 0U;
    active_slot_address = 0U;
    current_rotation_validation_failures = 0U;
    return 0;
}
