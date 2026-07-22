#include "credential_store.h"
#include "main.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The last two sectors of Bank 1 are outside the executable linker region.
 * Each update erases and writes only the inactive slot, so a power loss leaves
 * the previously verified record available. Firmware updates that use sector
 * erase for the application region do not touch either credential slot.
 */
#define CREDENTIAL_SLOT_A_ADDRESS       0x080C0000UL
#define CREDENTIAL_SLOT_B_ADDRESS       0x080E0000UL
#define CREDENTIAL_SLOT_A_SECTOR        FLASH_SECTOR_6
#define CREDENTIAL_SLOT_B_SECTOR        FLASH_SECTOR_7
#define CREDENTIAL_FLASH_MAGIC          0x43524431UL /* "CRD1" */
#define CREDENTIAL_STORAGE_FORMAT       1U
#define CREDENTIAL_FLASHWORD_SIZE       32U
#define CREDENTIAL_RECORD_SIZE          8192U

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

static int CredentialStore_LengthsAreValid(const CredentialFlashHeader *header)
{
    size_t total;

    if ((header->certificate_pem_length == 0U) ||
        (header->certificate_pem_length > CREDENTIAL_STORE_CERTIFICATE_PEM_MAX_LENGTH) ||
        (header->private_key_pem_length == 0U) ||
        (header->private_key_pem_length > CREDENTIAL_STORE_PRIVATE_KEY_PEM_MAX_LENGTH) ||
        (header->ownership_token_length == 0U) ||
        (header->ownership_token_length > CREDENTIAL_STORE_OWNERSHIP_TOKEN_MAX_LENGTH) ||
        (header->certificate_id_length == 0U) ||
        (header->certificate_id_length > CREDENTIAL_STORE_CERTIFICATE_ID_MAX_LENGTH) ||
        (header->thing_name_length > CREDENTIAL_STORE_THING_NAME_MAX_LENGTH))
    {
        return 0;
    }

    if ((header->state == CREDENTIAL_STORE_PROVISIONED) &&
        (header->thing_name_length == 0U))
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
        (record->header.storage_format != CREDENTIAL_STORAGE_FORMAT) ||
        (record->header.header_size != sizeof(CredentialFlashHeader)) ||
        ((record->header.state != CREDENTIAL_STORE_PENDING_REGISTRATION) &&
         (record->header.state != CREDENTIAL_STORE_PROVISIONED)) ||
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
        (ownership_token_length == 0U) ||
        (ownership_token_length > CREDENTIAL_STORE_OWNERSHIP_TOKEN_MAX_LENGTH) ||
        (certificate_id_length == 0U) ||
        (certificate_id_length > CREDENTIAL_STORE_CERTIFICATE_ID_MAX_LENGTH) ||
        (thing_name_length > CREDENTIAL_STORE_THING_NAME_MAX_LENGTH) ||
        ((state == CREDENTIAL_STORE_PROVISIONED) && (thing_name_length == 0U)))
    {
        return -1;
    }

    memset(record, 0, sizeof(*record));
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

static int CredentialStore_Write(CredentialStoreState state,
                                 const CredentialStoreData *data)
{
    CredentialFlashRecord record __attribute__((aligned(CREDENTIAL_FLASHWORD_SIZE)));
    const CredentialFlashRecord *persisted_record;
    FLASH_EraseInitTypeDef erase = { 0 };
    uint32_t sector_error = 0xFFFFFFFFUL;
    uint32_t target_address;
    uint32_t target_sector;
    size_t offset;
    HAL_StatusTypeDef status;

    if (CredentialStore_PrepareRecord(state, data, &record) != 0)
    {
        return -1;
    }

    if (active_slot_address == CREDENTIAL_SLOT_A_ADDRESS)
    {
        target_address = CREDENTIAL_SLOT_B_ADDRESS;
        target_sector = CREDENTIAL_SLOT_B_SECTOR;
    }
    else
    {
        target_address = CREDENTIAL_SLOT_A_ADDRESS;
        target_sector = CREDENTIAL_SLOT_A_SECTOR;
    }

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_1;
    erase.Sector = target_sector;
    erase.NbSectors = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return -1;
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1);
    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    for (offset = 0U; (status == HAL_OK) && (offset < sizeof(record));
         offset += CREDENTIAL_FLASHWORD_SIZE)
    {
        status = HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_FLASHWORD,
            target_address + (uint32_t)offset,
            (uint32_t)(uintptr_t)(((const uint8_t *)&record) + offset));
    }
    (void)HAL_FLASH_Lock();

    if ((status != HAL_OK) || (sector_error != 0xFFFFFFFFUL))
    {
        return -1;
    }

    CredentialStore_InvalidateCache(target_address, sizeof(record));
    persisted_record = (const CredentialFlashRecord *)(uintptr_t)target_address;
    if (!CredentialStore_RecordIsValid(persisted_record) ||
        (memcmp(persisted_record, &record, sizeof(record)) != 0))
    {
        return -1;
    }

    current_state = state;
    current_data = *data;
    current_generation = record.header.generation;
    active_slot_address = target_address;
    return 0;
}

int CredentialStore_Init(void)
{
    const CredentialFlashRecord *slot_a =
        (const CredentialFlashRecord *)(uintptr_t)CREDENTIAL_SLOT_A_ADDRESS;
    const CredentialFlashRecord *slot_b =
        (const CredentialFlashRecord *)(uintptr_t)CREDENTIAL_SLOT_B_ADDRESS;
    const CredentialFlashRecord *selected = NULL;

    current_state = CREDENTIAL_STORE_EMPTY;
    memset(&current_data, 0, sizeof(current_data));
    current_generation = 0U;
    active_slot_address = 0U;

    CredentialStore_InvalidateCache(CREDENTIAL_SLOT_A_ADDRESS, sizeof(*slot_a));
    CredentialStore_InvalidateCache(CREDENTIAL_SLOT_B_ADDRESS, sizeof(*slot_b));
    if (CredentialStore_RecordIsValid(slot_a))
    {
        selected = slot_a;
        active_slot_address = CREDENTIAL_SLOT_A_ADDRESS;
    }
    if (CredentialStore_RecordIsValid(slot_b) &&
        ((selected == NULL) || (slot_b->header.generation > selected->header.generation)))
    {
        selected = slot_b;
        active_slot_address = CREDENTIAL_SLOT_B_ADDRESS;
    }

    if (selected != NULL)
    {
        current_state = (CredentialStoreState)selected->header.state;
        current_generation = selected->header.generation;
        CredentialStore_RecordToData(selected, &current_data);
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
        default:
            return "EMPTY";
    }
}

int CredentialStore_SavePending(const char *certificate_pem,
                                const char *private_key_pem,
                                const char *ownership_token,
                                const char *certificate_id)
{
    CredentialStoreData data;

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
    return CredentialStore_Write(CREDENTIAL_STORE_PENDING_REGISTRATION, &data);
}

int CredentialStore_CommitProvisioned(const char *thing_name)
{
    CredentialStoreData data;

    if ((current_state != CREDENTIAL_STORE_PENDING_REGISTRATION) ||
        (thing_name == NULL) ||
        (strnlen(thing_name, sizeof(data.thing_name)) >= sizeof(data.thing_name)))
    {
        return -1;
    }

    data = current_data;
    strcpy(data.thing_name, thing_name);
    return CredentialStore_Write(CREDENTIAL_STORE_PROVISIONED, &data);
}

int CredentialStore_Erase(void)
{
    FLASH_EraseInitTypeDef erase = { 0 };
    uint32_t sector_error = 0xFFFFFFFFUL;
    HAL_StatusTypeDef status;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_1;
    erase.Sector = CREDENTIAL_SLOT_A_SECTOR;
    erase.NbSectors = 2U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return -1;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1);
    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    (void)HAL_FLASH_Lock();
    if ((status != HAL_OK) || (sector_error != 0xFFFFFFFFUL))
    {
        return -1;
    }

    current_state = CREDENTIAL_STORE_EMPTY;
    memset(&current_data, 0, sizeof(current_data));
    current_generation = 0U;
    active_slot_address = 0U;
    return 0;
}
