#include "iot_events.h"

#include "device_config.h"
#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define IOT_ALERT_INFO_COOLDOWN_MS          60000U
#define IOT_ALERT_WARNING_COOLDOWN_MS       30000U
#define IOT_ALERT_ALARM_COOLDOWN_MS         10000U
#define IOT_ALERT_CRITICAL_COOLDOWN_MS       5000U
#define IOT_DIAGNOSTIC_COOLDOWN_MS          10000U
#define IOT_THROTTLE_CAPACITY                   8U
#define IOT_EVENT_JOURNAL_ADDRESS       0x080A0000UL
#define IOT_EVENT_JOURNAL_SIZE             (128U * 1024U)
#define IOT_EVENT_JOURNAL_SECTOR       FLASH_SECTOR_5
#define IOT_EVENT_JOURNAL_RECORD_SIZE           1024U
#define IOT_EVENT_JOURNAL_MAGIC          0x45564A31UL
#define IOT_EVENT_JOURNAL_LEGACY_VERSION          1U
#define IOT_EVENT_JOURNAL_VERSION                 2U
#define IOT_EVENT_JOURNAL_KIND_PUT                1U
#define IOT_EVENT_JOURNAL_KIND_ACK                2U
#define IOT_EVENT_JOURNAL_KIND_TELEMETRY_PUT      3U
#define IOT_EVENT_JOURNAL_KIND_TELEMETRY_ACK      4U
#define IOT_EVENT_JOURNAL_KIND_COMMAND_PUT        5U
#define IOT_EVENT_JOURNAL_RECORD_COUNT \
    (IOT_EVENT_JOURNAL_SIZE / IOT_EVENT_JOURNAL_RECORD_SIZE)
#define IOT_EVENT_FLASHWORD_SIZE                  32U

typedef struct
{
    uint8_t used;
    uint8_t type;
    uint32_t last_queued_ms;
    uint32_t suppressed_count;
    char code[IOT_EVENT_CODE_CAPACITY];
} IotThrottleEntry;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t sequence;
    IotEventMessage event;
    uint32_t crc32;
    uint8_t reserved[IOT_EVENT_JOURNAL_RECORD_SIZE -
                     (sizeof(uint32_t) + sizeof(uint16_t) +
                      sizeof(uint16_t) + sizeof(uint32_t) +
                      sizeof(IotEventMessage) + sizeof(uint32_t))];
} IotEventLegacyJournalRecord;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t sequence;
    uint16_t payload_length;
    uint16_t reserved_header;
    uint8_t payload[IOT_EVENT_JOURNAL_RECORD_SIZE - 20U];
    uint32_t crc32;
} IotOutboxJournalRecord;

_Static_assert(sizeof(IotEventLegacyJournalRecord) == IOT_EVENT_JOURNAL_RECORD_SIZE,
               "Legacy event journal record must remain exactly 1024 bytes");
_Static_assert(sizeof(IotOutboxJournalRecord) == IOT_EVENT_JOURNAL_RECORD_SIZE,
               "Persistent outbox journal record must be exactly 1024 bytes");
_Static_assert(sizeof(IotEventMessage) == 496U,
               "Changing the event layout requires a journal migration");
_Static_assert(offsetof(IotEventMessage, flags) == 2U,
               "Durability flags must remain in the former v1 padding byte");
_Static_assert(offsetof(IotEventMessage, uptime_ms) == 4U,
               "Journal v1 event offsets must remain backward compatible");
_Static_assert(sizeof(IotTelemetryMeasurement) == 12U,
               "Persistent telemetry measurement must remain compact");
_Static_assert(sizeof(IotTelemetrySample) <=
                   sizeof(((IotOutboxJournalRecord *)0)->payload),
               "Telemetry sample exceeds one outbox record");
_Static_assert(sizeof(IotCommandResult) <=
                   sizeof(((IotOutboxJournalRecord *)0)->payload),
               "Command result exceeds one outbox record");

static IotEventMessage event_queue[IOT_EVENT_QUEUE_CAPACITY];
static IotTelemetrySample telemetry_queue[IOT_TELEMETRY_QUEUE_CAPACITY];
static IotCommandResult command_history[IOT_COMMAND_HISTORY_CAPACITY];
static IotThrottleEntry throttle_entries[IOT_THROTTLE_CAPACITY];
static size_t queue_count = 0U;
static size_t telemetry_count = 0U;
static size_t command_history_count = 0U;
static uint32_t dropped_count = 0U;
static uint32_t telemetry_dropped_count = 0U;
static uint32_t persistence_failure_count = 0U;
static uint32_t message_sequence = 0U;
static uint32_t boot_nonce = 0U;
static uint8_t initialized = 0U;
static uint32_t journal_next_index = 0U;
static uint32_t journal_sequence = 0U;
static StaticSemaphore_t event_mutex_storage;
static SemaphoreHandle_t event_mutex;

extern RNG_HandleTypeDef hrng;

static void IotEvents_Lock(void)
{
    if (event_mutex != NULL)
    {
        (void)xSemaphoreTake(event_mutex, portMAX_DELAY);
    }
}

static void IotEvents_Unlock(void)
{
    if (event_mutex != NULL)
    {
        (void)xSemaphoreGive(event_mutex);
    }
}

static uint32_t IotEvents_Crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    size_t index;

    for (index = 0U; index < length; index++)
    {
        uint32_t bit;
        crc ^= data[index];
        for (bit = 0U; bit < 8U; bit++)
        {
            crc = (crc & 1U) ? ((crc >> 1U) ^ 0xEDB88320UL)
                             : (crc >> 1U);
        }
    }
    return ~crc;
}

int IotEvents_IsDurable(const IotEventMessage *event)
{
    if (event == NULL)
    {
        return 0;
    }

    if ((event->flags & IOT_EVENT_FLAG_DURABLE) != 0U)
    {
        return 1;
    }

    if (event->type == IOT_MESSAGE_ALERT)
    {
        return event->level >= (uint8_t)IOT_ALERT_WARNING;
    }

    return (event->type == IOT_MESSAGE_DIAGNOSTIC) &&
           (event->level <= (uint8_t)IOT_DIAGNOSTIC_WARN);
}

static void IotEvents_InvalidateJournalCache(void)
{
#if (__DCACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    {
        SCB_InvalidateDCache_by_Addr(
            (uint32_t *)IOT_EVENT_JOURNAL_ADDRESS,
            (int32_t)IOT_EVENT_JOURNAL_SIZE);
    }
#endif
    __DSB();
    __ISB();
}

static int IotEvents_JournalRecordIsErased(uint32_t address)
{
    const uint32_t *words = (const uint32_t *)(uintptr_t)address;
    size_t index;

    for (index = 0U; index < (IOT_EVENT_FLASHWORD_SIZE / sizeof(uint32_t));
         index++)
    {
        if (words[index] != 0xFFFFFFFFUL)
        {
            return 0;
        }
    }
    return 1;
}

static int IotEvents_LegacyJournalRecordIsValid(
    const IotEventLegacyJournalRecord *record)
{
    if ((record->magic != IOT_EVENT_JOURNAL_MAGIC) ||
        (record->version != IOT_EVENT_JOURNAL_LEGACY_VERSION) ||
        ((record->kind != IOT_EVENT_JOURNAL_KIND_PUT) &&
         (record->kind != IOT_EVENT_JOURNAL_KIND_ACK)))
    {
        return 0;
    }

    return record->crc32 ==
           IotEvents_Crc32((const uint8_t *)record,
                           offsetof(IotEventLegacyJournalRecord, crc32));
}

static int IotEvents_OutboxJournalRecordIsValid(
    const IotOutboxJournalRecord *record)
{
    if ((record->magic != IOT_EVENT_JOURNAL_MAGIC) ||
        (record->version != IOT_EVENT_JOURNAL_VERSION) ||
        (record->payload_length > sizeof(record->payload)) ||
        (record->kind < IOT_EVENT_JOURNAL_KIND_PUT) ||
        (record->kind > IOT_EVENT_JOURNAL_KIND_COMMAND_PUT))
    {
        return 0;
    }

    return record->crc32 ==
           IotEvents_Crc32((const uint8_t *)record,
                           offsetof(IotOutboxJournalRecord, crc32));
}

static int IotEvents_JournalProgramRaw(const void *record)
{
    uint32_t address;
    uint32_t offset;
    HAL_StatusTypeDef status = HAL_OK;

    if ((record == NULL) ||
        (journal_next_index >= IOT_EVENT_JOURNAL_RECORD_COUNT))
    {
        return -1;
    }

    address = IOT_EVENT_JOURNAL_ADDRESS +
              (journal_next_index * IOT_EVENT_JOURNAL_RECORD_SIZE);
    if (!IotEvents_JournalRecordIsErased(address) ||
        (HAL_FLASH_Unlock() != HAL_OK))
    {
        return -1;
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1);
    for (offset = 0U;
         (offset < IOT_EVENT_JOURNAL_RECORD_SIZE) && (status == HAL_OK);
         offset += IOT_EVENT_FLASHWORD_SIZE)
    {
        status = HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_FLASHWORD,
            address + offset,
            (uint32_t)((uintptr_t)record + offset));
    }
    (void)HAL_FLASH_Lock();
    if (status != HAL_OK)
    {
        return -1;
    }

    IotEvents_InvalidateJournalCache();
    if (memcmp((const void *)(uintptr_t)address,
               record,
               IOT_EVENT_JOURNAL_RECORD_SIZE) != 0)
    {
        return -1;
    }

    journal_next_index++;
    return 0;
}

static int IotEvents_PrepareJournalRecord(
    IotOutboxJournalRecord *record,
    uint16_t kind,
    const void *payload,
    size_t payload_length)
{
    if ((record == NULL) ||
        ((payload_length > 0U) && (payload == NULL)) ||
        (payload_length > sizeof(record->payload)))
    {
        return -1;
    }

    memset(record, 0xFF, sizeof(*record));
    record->magic = IOT_EVENT_JOURNAL_MAGIC;
    record->version = IOT_EVENT_JOURNAL_VERSION;
    record->kind = kind;
    record->sequence = ++journal_sequence;
    record->payload_length = (uint16_t)payload_length;
    record->reserved_header = 0U;
    if (payload_length > 0U)
    {
        memcpy(record->payload, payload, payload_length);
    }
    record->crc32 =
        IotEvents_Crc32((const uint8_t *)record,
                        offsetof(IotOutboxJournalRecord, crc32));
    return 0;
}

static int IotEvents_JournalErase(void)
{
    FLASH_EraseInitTypeDef erase = { 0 };
    uint32_t sector_error = 0xFFFFFFFFUL;
    HAL_StatusTypeDef status;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_1;
    erase.Sector = IOT_EVENT_JOURNAL_SECTOR;
    erase.NbSectors = 1U;
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

    IotEvents_InvalidateJournalCache();
    journal_next_index = 0U;
    return 0;
}

static int IotEvents_JournalCompact(void)
{
    size_t index;

    if (IotEvents_JournalErase() != 0)
    {
        return -1;
    }

    for (index = 0U; index < queue_count; index++)
    {
        IotOutboxJournalRecord record
            __attribute__((aligned(IOT_EVENT_FLASHWORD_SIZE)));

        if (!IotEvents_IsDurable(&event_queue[index]))
        {
            continue;
        }
        if ((IotEvents_PrepareJournalRecord(&record,
                                            IOT_EVENT_JOURNAL_KIND_PUT,
                                            &event_queue[index],
                                            sizeof(event_queue[index])) != 0) ||
            (IotEvents_JournalProgramRaw(&record) != 0))
        {
            return -1;
        }
    }

    for (index = 0U; index < telemetry_count; index++)
    {
        IotOutboxJournalRecord record
            __attribute__((aligned(IOT_EVENT_FLASHWORD_SIZE)));

        if ((IotEvents_PrepareJournalRecord(
                 &record,
                 IOT_EVENT_JOURNAL_KIND_TELEMETRY_PUT,
                 &telemetry_queue[index],
                 sizeof(telemetry_queue[index])) != 0) ||
            (IotEvents_JournalProgramRaw(&record) != 0))
        {
            return -1;
        }
    }

    for (index = 0U; index < command_history_count; index++)
    {
        IotOutboxJournalRecord record
            __attribute__((aligned(IOT_EVENT_FLASHWORD_SIZE)));

        if ((IotEvents_PrepareJournalRecord(
                 &record,
                 IOT_EVENT_JOURNAL_KIND_COMMAND_PUT,
                 &command_history[index],
                 sizeof(command_history[index])) != 0) ||
            (IotEvents_JournalProgramRaw(&record) != 0))
        {
            return -1;
        }
    }
    return 0;
}

static int IotEvents_JournalAppend(uint16_t kind,
                                   const void *payload,
                                   size_t payload_length)
{
    IotOutboxJournalRecord record
        __attribute__((aligned(IOT_EVENT_FLASHWORD_SIZE)));

    if (journal_next_index >= IOT_EVENT_JOURNAL_RECORD_COUNT)
    {
        if (IotEvents_JournalCompact() != 0)
        {
            return -1;
        }
    }

    if (IotEvents_PrepareJournalRecord(&record,
                                       kind,
                                       payload,
                                       payload_length) != 0)
    {
        return -1;
    }
    return IotEvents_JournalProgramRaw(&record);
}

static uint32_t IotEvents_CooldownMs(IotMessageType type, uint8_t level)
{
    if (type == IOT_MESSAGE_DIAGNOSTIC)
    {
        return IOT_DIAGNOSTIC_COOLDOWN_MS;
    }

    switch ((IotAlertSeverity)level)
    {
        case IOT_ALERT_CRITICAL:
            return IOT_ALERT_CRITICAL_COOLDOWN_MS;
        case IOT_ALERT_ALARM:
            return IOT_ALERT_ALARM_COOLDOWN_MS;
        case IOT_ALERT_WARNING:
            return IOT_ALERT_WARNING_COOLDOWN_MS;
        case IOT_ALERT_INFO:
        default:
            return IOT_ALERT_INFO_COOLDOWN_MS;
    }
}

static void IotEvents_CopyToken(char *destination,
                                size_t destination_size,
                                const char *source,
                                const char *fallback)
{
    size_t output_index = 0U;
    const char *input = ((source != NULL) && (source[0] != '\0')) ? source : fallback;

    if ((destination == NULL) || (destination_size == 0U))
    {
        return;
    }

    while ((*input != '\0') && (output_index + 1U < destination_size))
    {
        unsigned char character = (unsigned char)*input++;

        if (isalnum(character) || (character == '_') || (character == '-') || (character == '.'))
        {
            destination[output_index++] = (char)toupper(character);
        }
        else
        {
            destination[output_index++] = '_';
        }
    }

    destination[output_index] = '\0';
}

static void IotEvents_CopyMessage(char *destination,
                                  size_t destination_size,
                                  const char *source,
                                  size_t configured_limit)
{
    size_t output_index = 0U;
    size_t effective_limit;

    if ((destination == NULL) || (destination_size == 0U))
    {
        return;
    }

    effective_limit = configured_limit;
    if ((effective_limit == 0U) || (effective_limit >= destination_size))
    {
        effective_limit = destination_size - 1U;
    }

    if (source == NULL)
    {
        source = "No details";
    }

    while ((*source != '\0') && (output_index < effective_limit))
    {
        unsigned char character = (unsigned char)*source++;

        if ((character < 0x20U) || (character == '\\'))
        {
            destination[output_index++] = ' ';
        }
        else if (character == '"')
        {
            destination[output_index++] = '\'';
        }
        else
        {
            destination[output_index++] = (char)character;
        }
    }

    destination[output_index] = '\0';
}

static IotThrottleEntry *IotEvents_ThrottleEntry(IotMessageType type, const char *code)
{
    size_t index;
    IotThrottleEntry *free_entry = NULL;
    IotThrottleEntry *oldest_entry = &throttle_entries[0];

    for (index = 0U; index < IOT_THROTTLE_CAPACITY; index++)
    {
        IotThrottleEntry *entry = &throttle_entries[index];

        if (entry->used && (entry->type == (uint8_t)type) &&
            (strncmp(entry->code, code, sizeof(entry->code)) == 0))
        {
            return entry;
        }

        if (!entry->used && (free_entry == NULL))
        {
            free_entry = entry;
        }

        if (entry->last_queued_ms < oldest_entry->last_queued_ms)
        {
            oldest_entry = entry;
        }
    }

    return (free_entry != NULL) ? free_entry : oldest_entry;
}

static void IotEvents_RemoveAt(size_t index)
{
    if (index >= queue_count)
    {
        return;
    }

    if ((index + 1U) < queue_count)
    {
        memmove(&event_queue[index],
                &event_queue[index + 1U],
                (queue_count - index - 1U) * sizeof(event_queue[0]));
    }

    queue_count--;
    memset(&event_queue[queue_count], 0, sizeof(event_queue[queue_count]));
}

static void IotEvents_RemoveTelemetryAt(size_t index)
{
    if (index >= telemetry_count)
    {
        return;
    }

    if ((index + 1U) < telemetry_count)
    {
        memmove(&telemetry_queue[index],
                &telemetry_queue[index + 1U],
                (telemetry_count - index - 1U) * sizeof(telemetry_queue[0]));
    }
    telemetry_count--;
    memset(&telemetry_queue[telemetry_count],
           0,
           sizeof(telemetry_queue[telemetry_count]));
}

static void IotEvents_RemoveCommandAt(size_t index)
{
    if (index >= command_history_count)
    {
        return;
    }

    if ((index + 1U) < command_history_count)
    {
        memmove(&command_history[index],
                &command_history[index + 1U],
                (command_history_count - index - 1U) *
                    sizeof(command_history[0]));
    }
    command_history_count--;
    memset(&command_history[command_history_count],
           0,
           sizeof(command_history[command_history_count]));
}

static void IotEvents_ApplyEventRecord(uint16_t kind,
                                       const IotEventMessage *event)
{
    size_t index;

    if ((event == NULL) || !IotEvents_IsDurable(event))
    {
        return;
    }

    for (index = 0U; index < queue_count; index++)
    {
        if (strncmp(event_queue[index].message_id,
                    event->message_id,
                    IOT_EVENT_ID_CAPACITY) == 0)
        {
            if (kind == IOT_EVENT_JOURNAL_KIND_ACK)
            {
                IotEvents_RemoveAt(index);
            }
            return;
        }
    }

    if ((kind == IOT_EVENT_JOURNAL_KIND_PUT) &&
        (queue_count < IOT_EVENT_QUEUE_CAPACITY))
    {
        event_queue[queue_count++] = *event;
    }
}

static void IotEvents_ApplyTelemetryRecord(uint16_t kind,
                                           const IotTelemetrySample *sample)
{
    size_t index;

    if ((sample == NULL) || (sample->sample_id[0] == '\0') ||
        (sample->measurement_count == 0U) ||
        (sample->measurement_count > IOT_TELEMETRY_MAX_MEASUREMENTS))
    {
        return;
    }

    for (index = 0U; index < telemetry_count; index++)
    {
        if (strncmp(telemetry_queue[index].sample_id,
                    sample->sample_id,
                    IOT_EVENT_ID_CAPACITY) == 0)
        {
            if (kind == IOT_EVENT_JOURNAL_KIND_TELEMETRY_ACK)
            {
                IotEvents_RemoveTelemetryAt(index);
            }
            return;
        }
    }

    if (kind != IOT_EVENT_JOURNAL_KIND_TELEMETRY_PUT)
    {
        return;
    }
    if (telemetry_count >= IOT_TELEMETRY_QUEUE_CAPACITY)
    {
        IotEvents_RemoveTelemetryAt(0U);
        telemetry_dropped_count++;
    }
    telemetry_queue[telemetry_count++] = *sample;
}

static void IotEvents_ApplyCommandRecord(const IotCommandResult *result)
{
    size_t index;

    if ((result == NULL) || (result->command_id[0] == '\0') ||
        (result->status[0] == '\0'))
    {
        return;
    }

    for (index = 0U; index < command_history_count; index++)
    {
        if (strncmp(command_history[index].command_id,
                    result->command_id,
                    IOT_COMMAND_ID_CAPACITY) == 0)
        {
            command_history[index] = *result;
            return;
        }
    }

    if (command_history_count >= IOT_COMMAND_HISTORY_CAPACITY)
    {
        IotEvents_RemoveCommandAt(0U);
    }
    command_history[command_history_count++] = *result;
}

static int IotEvents_MakeRoom(IotMessageType type,
                              uint8_t level,
                              uint8_t flags)
{
    size_t index;
    IotEventMessage candidate = { 0 };
    int durable;

    candidate.type = type;
    candidate.level = level;
    candidate.flags = flags;
    durable = IotEvents_IsDurable(&candidate);

    if (queue_count < IOT_EVENT_QUEUE_CAPACITY)
    {
        return 0;
    }

    if (!durable)
    {
        dropped_count++;
        return -1;
    }

    for (index = 0U; index < queue_count; index++)
    {
        if (!IotEvents_IsDurable(&event_queue[index]))
        {
            IotEvents_RemoveAt(index);
            dropped_count++;
            return 0;
        }
    }

    /*
     * Never discard an event covered by the durability policy to make room
     * for a newer one. The producer receives QUEUE_FULL and can retry later.
     */
    dropped_count++;
    return -1;
}

static IotEventResult IotEvents_Queue(IotMessageType type,
                                      uint8_t level,
                                      uint8_t flags,
                                      const char *source,
                                      const char *code,
                                      const char *message)
{
    DeviceConfig config;
    IotThrottleEntry *throttle;
    IotEventMessage *entry;
    char normalized_code[IOT_EVENT_CODE_CAPACITY];
    uint32_t now;
    uint32_t cooldown_ms;
    uint32_t suppressed_count;
    const char *id_prefix;

    if (!initialized || (code == NULL) || (code[0] == '\0') ||
        (message == NULL) || (message[0] == '\0'))
    {
        return IOT_EVENT_INVALID;
    }

    if (((type == IOT_MESSAGE_ALERT) && (level > (uint8_t)IOT_ALERT_CRITICAL)) ||
        ((type == IOT_MESSAGE_DIAGNOSTIC) && (level > (uint8_t)IOT_DIAGNOSTIC_TRACE)))
    {
        return IOT_EVENT_INVALID;
    }

    DeviceConfig_GetSnapshot(&config);
    if ((type == IOT_MESSAGE_DIAGNOSTIC) &&
        ((flags & IOT_EVENT_FLAG_DURABLE) == 0U) &&
        (level > config.log_level))
    {
        return IOT_EVENT_FILTERED;
    }

    IotEvents_CopyToken(normalized_code, sizeof(normalized_code), code, "UNSPECIFIED");
    now = HAL_GetTick();
    cooldown_ms = IotEvents_CooldownMs(type, level);
    IotEvents_Lock();
    throttle = IotEvents_ThrottleEntry(type, normalized_code);

    if (throttle->used && ((now - throttle->last_queued_ms) < cooldown_ms))
    {
        throttle->suppressed_count++;
        IotEvents_Unlock();
        return IOT_EVENT_SUPPRESSED;
    }

    suppressed_count = throttle->used ? throttle->suppressed_count : 0U;
    if (IotEvents_MakeRoom(type, level, flags) != 0)
    {
        IotEvents_Unlock();
        return IOT_EVENT_QUEUE_FULL;
    }

    entry = &event_queue[queue_count];
    memset(entry, 0, sizeof(*entry));
    entry->type = type;
    entry->level = level;
    entry->flags = flags;
    entry->uptime_ms = now;
    entry->suppressed_count = suppressed_count;
    IotEvents_CopyToken(entry->code, sizeof(entry->code), normalized_code, "UNSPECIFIED");
    IotEvents_CopyToken(entry->source, sizeof(entry->source), source, "SYSTEM");
    IotEvents_CopyMessage(entry->message,
                          sizeof(entry->message),
                          message,
                          config.diagnostics_max_message_bytes);

    message_sequence++;
    id_prefix = (type == IOT_MESSAGE_ALERT) ? "evt" : "log";
    (void)snprintf(entry->message_id,
                   sizeof(entry->message_id),
                   "%s-%08lx-%08lx",
                   id_prefix,
                   (unsigned long)boot_nonce,
                   (unsigned long)message_sequence);
    entry->message_id[sizeof(entry->message_id) - 1U] = '\0';

    if (IotEvents_IsDurable(entry) &&
        (IotEvents_JournalAppend(IOT_EVENT_JOURNAL_KIND_PUT,
                                 entry,
                                 sizeof(*entry)) != 0))
    {
        /*
         * Keep the event in RAM so it can still be delivered. The explicit
         * failure counter makes the degraded durability observable.
         */
        persistence_failure_count++;
    }
    queue_count++;

    memset(throttle, 0, sizeof(*throttle));
    throttle->used = 1U;
    throttle->type = (uint8_t)type;
    throttle->last_queued_ms = now;
    IotEvents_CopyToken(throttle->code,
                        sizeof(throttle->code),
                        normalized_code,
                        "UNSPECIFIED");

    IotEvents_Unlock();
    return IOT_EVENT_QUEUED;
}

void IotEvents_Init(void)
{
    uint32_t random_value = 0U;
    uint32_t index;

    event_mutex = xSemaphoreCreateMutexStatic(&event_mutex_storage);
    memset(event_queue, 0, sizeof(event_queue));
    memset(telemetry_queue, 0, sizeof(telemetry_queue));
    memset(command_history, 0, sizeof(command_history));
    memset(throttle_entries, 0, sizeof(throttle_entries));
    queue_count = 0U;
    telemetry_count = 0U;
    command_history_count = 0U;
    dropped_count = 0U;
    telemetry_dropped_count = 0U;
    persistence_failure_count = 0U;
    message_sequence = 0U;
    journal_next_index = 0U;
    journal_sequence = 0U;

    IotEvents_InvalidateJournalCache();
    for (index = 0U; index < IOT_EVENT_JOURNAL_RECORD_COUNT; index++)
    {
        uint32_t address =
            IOT_EVENT_JOURNAL_ADDRESS +
            (index * IOT_EVENT_JOURNAL_RECORD_SIZE);
        const IotOutboxJournalRecord *record =
            (const IotOutboxJournalRecord *)(uintptr_t)address;

        if (IotEvents_JournalRecordIsErased(address))
        {
            journal_next_index = index;
            break;
        }

        journal_next_index = index + 1U;
        if (record->version == IOT_EVENT_JOURNAL_LEGACY_VERSION)
        {
            const IotEventLegacyJournalRecord *legacy =
                (const IotEventLegacyJournalRecord *)(uintptr_t)address;

            if (!IotEvents_LegacyJournalRecordIsValid(legacy))
            {
                persistence_failure_count++;
                continue;
            }
            if (legacy->sequence > journal_sequence)
            {
                journal_sequence = legacy->sequence;
            }
            IotEvents_ApplyEventRecord(legacy->kind, &legacy->event);
            continue;
        }

        if (!IotEvents_OutboxJournalRecordIsValid(record))
        {
            persistence_failure_count++;
            continue;
        }
        if (record->sequence > journal_sequence)
        {
            journal_sequence = record->sequence;
        }

        if (((record->kind == IOT_EVENT_JOURNAL_KIND_PUT) ||
             (record->kind == IOT_EVENT_JOURNAL_KIND_ACK)) &&
            (record->payload_length == sizeof(IotEventMessage)))
        {
            IotEventMessage event;
            memcpy(&event, record->payload, sizeof(event));
            event.message_id[sizeof(event.message_id) - 1U] = '\0';
            event.code[sizeof(event.code) - 1U] = '\0';
            event.source[sizeof(event.source) - 1U] = '\0';
            event.message[sizeof(event.message) - 1U] = '\0';
            IotEvents_ApplyEventRecord(record->kind, &event);
        }
        else if (((record->kind == IOT_EVENT_JOURNAL_KIND_TELEMETRY_PUT) ||
                  (record->kind == IOT_EVENT_JOURNAL_KIND_TELEMETRY_ACK)) &&
                 (record->payload_length == sizeof(IotTelemetrySample)))
        {
            IotTelemetrySample sample;
            memcpy(&sample, record->payload, sizeof(sample));
            sample.sample_id[sizeof(sample.sample_id) - 1U] = '\0';
            IotEvents_ApplyTelemetryRecord(record->kind, &sample);
        }
        else if ((record->kind == IOT_EVENT_JOURNAL_KIND_COMMAND_PUT) &&
                 (record->payload_length == sizeof(IotCommandResult)))
        {
            IotCommandResult result;
            memcpy(&result, record->payload, sizeof(result));
            result.command_id[sizeof(result.command_id) - 1U] = '\0';
            result.status[sizeof(result.status) - 1U] = '\0';
            result.message[sizeof(result.message) - 1U] = '\0';
            IotEvents_ApplyCommandRecord(&result);
        }
        else
        {
            persistence_failure_count++;
        }
    }

    if (HAL_RNG_GenerateRandomNumber(&hrng, &random_value) == HAL_OK)
    {
        boot_nonce = random_value;
    }
    else
    {
        boot_nonce = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2() ^ HAL_GetTick();
    }

    initialized = 1U;
}

int IotEvents_FactoryReset(void)
{
    if (IotEvents_JournalErase() != 0)
    {
        return -1;
    }

    memset(event_queue, 0, sizeof(event_queue));
    memset(telemetry_queue, 0, sizeof(telemetry_queue));
    memset(command_history, 0, sizeof(command_history));
    memset(throttle_entries, 0, sizeof(throttle_entries));
    queue_count = 0U;
    telemetry_count = 0U;
    command_history_count = 0U;
    dropped_count = 0U;
    telemetry_dropped_count = 0U;
    persistence_failure_count = 0U;
    message_sequence = 0U;
    journal_sequence = 0U;
    return 0;
}

IotEventResult IotEvents_QueueAlert(IotAlertSeverity severity,
                                    const char *code,
                                    const char *message)
{
    return IotEvents_Queue(IOT_MESSAGE_ALERT,
                           (uint8_t)severity,
                           0U,
                           "SYSTEM",
                           code,
                           message);
}

IotEventResult IotEvents_QueueDiagnostic(IotDiagnosticLevel level,
                                         const char *source,
                                         const char *code,
                                         const char *message)
{
    return IotEvents_Queue(IOT_MESSAGE_DIAGNOSTIC,
                           (uint8_t)level,
                           0U,
                           source,
                           code,
                           message);
}

IotEventResult IotEvents_QueueDiagnosticDurable(IotDiagnosticLevel level,
                                                const char *source,
                                                const char *code,
                                                const char *message)
{
    return IotEvents_Queue(IOT_MESSAGE_DIAGNOSTIC,
                           (uint8_t)level,
                           IOT_EVENT_FLAG_DURABLE,
                           source,
                           code,
                           message);
}

int IotEvents_PeekCopy(IotEventMessage *event)
{
    int available = 0;

    if (event == NULL)
    {
        return 0;
    }

    IotEvents_Lock();
    if (queue_count > 0U)
    {
        *event = event_queue[0];
        available = 1;
    }
    IotEvents_Unlock();
    return available;
}

int IotEvents_PopIfMatch(const char *message_id)
{
    if (message_id == NULL)
    {
        return -1;
    }

    IotEvents_Lock();
    if ((queue_count > 0U) &&
        (strncmp(event_queue[0].message_id,
                 message_id,
                 sizeof(event_queue[0].message_id)) == 0))
    {
        if (IotEvents_IsDurable(&event_queue[0]) &&
            (IotEvents_JournalAppend(IOT_EVENT_JOURNAL_KIND_ACK,
                                     &event_queue[0],
                                     sizeof(event_queue[0])) != 0))
        {
            persistence_failure_count++;
            IotEvents_Unlock();
            return -1;
        }
        IotEvents_RemoveAt(0U);
    }
    IotEvents_Unlock();
    return 0;
}

int IotEvents_AssignTelemetryId(IotTelemetrySample *sample)
{
    if (sample == NULL)
    {
        return -1;
    }

    IotEvents_Lock();
    message_sequence++;
    (void)snprintf(sample->sample_id,
                   sizeof(sample->sample_id),
                   "tlm-%08lx-%08lx",
                   (unsigned long)boot_nonce,
                   (unsigned long)message_sequence);
    sample->sample_id[sizeof(sample->sample_id) - 1U] = '\0';
    IotEvents_Unlock();
    return 0;
}

int IotEvents_QueueTelemetry(const IotTelemetrySample *sample)
{
    size_t index;

    if ((sample == NULL) || (sample->sample_id[0] == '\0') ||
        (strnlen(sample->sample_id, sizeof(sample->sample_id)) >=
         sizeof(sample->sample_id)) ||
        (sample->measurement_count == 0U) ||
        (sample->measurement_count > IOT_TELEMETRY_MAX_MEASUREMENTS))
    {
        return -1;
    }

    IotEvents_Lock();
    for (index = 0U; index < telemetry_count; index++)
    {
        if (strncmp(telemetry_queue[index].sample_id,
                    sample->sample_id,
                    IOT_EVENT_ID_CAPACITY) == 0)
        {
            IotEvents_Unlock();
            return 0;
        }
    }

    /* Append the replacement first. During recovery, applying a PUT to a full
     * bounded queue deterministically evicts the oldest sample. This ordering
     * means a power loss can leave the old sample or the new sample, but never
     * erase the old one before the replacement is durable. */
    if (IotEvents_JournalAppend(IOT_EVENT_JOURNAL_KIND_TELEMETRY_PUT,
                                sample,
                                sizeof(*sample)) != 0)
    {
        persistence_failure_count++;
        IotEvents_Unlock();
        return -1;
    }
    if (telemetry_count >= IOT_TELEMETRY_QUEUE_CAPACITY)
    {
        IotEvents_RemoveTelemetryAt(0U);
        telemetry_dropped_count++;
    }
    telemetry_queue[telemetry_count++] = *sample;
    IotEvents_Unlock();
    return 0;
}

int IotEvents_PeekTelemetry(IotTelemetrySample *sample)
{
    int available = 0;

    if (sample == NULL)
    {
        return 0;
    }

    IotEvents_Lock();
    if (telemetry_count > 0U)
    {
        *sample = telemetry_queue[0];
        available = 1;
    }
    IotEvents_Unlock();
    return available;
}

int IotEvents_PopTelemetryIfMatch(const char *sample_id)
{
    if (sample_id == NULL)
    {
        return -1;
    }

    IotEvents_Lock();
    if ((telemetry_count > 0U) &&
        (strncmp(telemetry_queue[0].sample_id,
                 sample_id,
                 sizeof(telemetry_queue[0].sample_id)) == 0))
    {
        if (IotEvents_JournalAppend(
                IOT_EVENT_JOURNAL_KIND_TELEMETRY_ACK,
                &telemetry_queue[0],
                sizeof(telemetry_queue[0])) != 0)
        {
            persistence_failure_count++;
            IotEvents_Unlock();
            return -1;
        }
        IotEvents_RemoveTelemetryAt(0U);
    }
    IotEvents_Unlock();
    return 0;
}

size_t IotEvents_TelemetryPendingCount(void)
{
    size_t count;
    IotEvents_Lock();
    count = telemetry_count;
    IotEvents_Unlock();
    return count;
}

uint32_t IotEvents_TelemetryDroppedCount(void)
{
    uint32_t count;
    IotEvents_Lock();
    count = telemetry_dropped_count;
    IotEvents_Unlock();
    return count;
}

int IotEvents_RememberCommandResult(const IotCommandResult *result)
{
    if ((result == NULL) || (result->command_id[0] == '\0') ||
        (result->status[0] == '\0') ||
        (strnlen(result->command_id, sizeof(result->command_id)) >=
         sizeof(result->command_id)) ||
        (strnlen(result->status, sizeof(result->status)) >=
         sizeof(result->status)) ||
        (strnlen(result->message, sizeof(result->message)) >=
         sizeof(result->message)))
    {
        return -1;
    }

    IotEvents_Lock();
    if (IotEvents_JournalAppend(IOT_EVENT_JOURNAL_KIND_COMMAND_PUT,
                                result,
                                sizeof(*result)) != 0)
    {
        persistence_failure_count++;
        IotEvents_Unlock();
        return -1;
    }
    IotEvents_ApplyCommandRecord(result);
    IotEvents_Unlock();
    return 0;
}

int IotEvents_FindCommandResult(const char *command_id,
                                IotCommandResult *result)
{
    size_t index;

    if ((command_id == NULL) || (result == NULL))
    {
        return 0;
    }

    IotEvents_Lock();
    for (index = command_history_count; index > 0U; index--)
    {
        if (strncmp(command_history[index - 1U].command_id,
                    command_id,
                    IOT_COMMAND_ID_CAPACITY) == 0)
        {
            *result = command_history[index - 1U];
            IotEvents_Unlock();
            return 1;
        }
    }
    IotEvents_Unlock();
    return 0;
}

size_t IotEvents_CommandHistoryCount(void)
{
    size_t count;
    IotEvents_Lock();
    count = command_history_count;
    IotEvents_Unlock();
    return count;
}

size_t IotEvents_PendingCount(void)
{
    size_t count;

    IotEvents_Lock();
    count = queue_count;
    IotEvents_Unlock();
    return count;
}

size_t IotEvents_DurablePendingCount(void)
{
    size_t count = 0U;
    size_t index;

    IotEvents_Lock();
    for (index = 0U; index < queue_count; index++)
    {
        if (IotEvents_IsDurable(&event_queue[index]))
        {
            count++;
        }
    }
    IotEvents_Unlock();
    return count;
}

uint32_t IotEvents_DroppedCount(void)
{
    uint32_t count;

    IotEvents_Lock();
    count = dropped_count;
    IotEvents_Unlock();
    return count;
}

uint32_t IotEvents_PersistenceFailureCount(void)
{
    uint32_t count;

    IotEvents_Lock();
    count = persistence_failure_count;
    IotEvents_Unlock();
    return count;
}

const char *IotEvents_SeverityName(uint8_t severity)
{
    switch ((IotAlertSeverity)severity)
    {
        case IOT_ALERT_WARNING:
            return "WARNING";
        case IOT_ALERT_ALARM:
            return "ALARM";
        case IOT_ALERT_CRITICAL:
            return "CRITICAL";
        case IOT_ALERT_INFO:
        default:
            return "INFO";
    }
}

const char *IotEvents_DiagnosticLevelName(uint8_t level)
{
    switch ((IotDiagnosticLevel)level)
    {
        case IOT_DIAGNOSTIC_ERROR:
            return "ERROR";
        case IOT_DIAGNOSTIC_WARN:
            return "WARN";
        case IOT_DIAGNOSTIC_DEBUG:
            return "DEBUG";
        case IOT_DIAGNOSTIC_TRACE:
            return "TRACE";
        case IOT_DIAGNOSTIC_INFO:
        default:
            return "INFO";
    }
}
