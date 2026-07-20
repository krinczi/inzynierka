#include "iot_events.h"

#include "device_config.h"
#include "main.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define IOT_ALERT_INFO_COOLDOWN_MS          60000U
#define IOT_ALERT_WARNING_COOLDOWN_MS       30000U
#define IOT_ALERT_ALARM_COOLDOWN_MS         10000U
#define IOT_ALERT_CRITICAL_COOLDOWN_MS       5000U
#define IOT_DIAGNOSTIC_COOLDOWN_MS          10000U
#define IOT_THROTTLE_CAPACITY                   8U

typedef struct
{
    uint8_t used;
    uint8_t type;
    uint32_t last_queued_ms;
    uint32_t suppressed_count;
    char code[IOT_EVENT_CODE_CAPACITY];
} IotThrottleEntry;

static IotEventMessage event_queue[IOT_EVENT_QUEUE_CAPACITY];
static IotThrottleEntry throttle_entries[IOT_THROTTLE_CAPACITY];
static size_t queue_count = 0U;
static uint32_t dropped_count = 0U;
static uint32_t message_sequence = 0U;
static uint32_t boot_nonce = 0U;
static uint8_t initialized = 0U;

extern RNG_HandleTypeDef hrng;

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

static int IotEvents_MakeRoom(IotMessageType type, uint8_t level)
{
    size_t index;
    int important = (type == IOT_MESSAGE_ALERT) &&
                    ((level == (uint8_t)IOT_ALERT_ALARM) ||
                     (level == (uint8_t)IOT_ALERT_CRITICAL));

    if (queue_count < IOT_EVENT_QUEUE_CAPACITY)
    {
        return 0;
    }

    if (!important)
    {
        dropped_count++;
        return -1;
    }

    for (index = 0U; index < queue_count; index++)
    {
        if ((event_queue[index].type == IOT_MESSAGE_DIAGNOSTIC) ||
            (event_queue[index].level < (uint8_t)IOT_ALERT_ALARM))
        {
            IotEvents_RemoveAt(index);
            dropped_count++;
            return 0;
        }
    }

    IotEvents_RemoveAt(0U);
    dropped_count++;
    return 0;
}

static IotEventResult IotEvents_Queue(IotMessageType type,
                                      uint8_t level,
                                      const char *source,
                                      const char *code,
                                      const char *message)
{
    const DeviceConfig *config;
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

    config = DeviceConfig_Get();
    if ((type == IOT_MESSAGE_DIAGNOSTIC) && (level > config->log_level))
    {
        return IOT_EVENT_FILTERED;
    }

    IotEvents_CopyToken(normalized_code, sizeof(normalized_code), code, "UNSPECIFIED");
    now = HAL_GetTick();
    cooldown_ms = IotEvents_CooldownMs(type, level);
    throttle = IotEvents_ThrottleEntry(type, normalized_code);

    if (throttle->used && ((now - throttle->last_queued_ms) < cooldown_ms))
    {
        throttle->suppressed_count++;
        return IOT_EVENT_SUPPRESSED;
    }

    suppressed_count = throttle->used ? throttle->suppressed_count : 0U;
    if (IotEvents_MakeRoom(type, level) != 0)
    {
        return IOT_EVENT_QUEUE_FULL;
    }

    entry = &event_queue[queue_count];
    memset(entry, 0, sizeof(*entry));
    entry->type = type;
    entry->level = level;
    entry->uptime_ms = now;
    entry->suppressed_count = suppressed_count;
    IotEvents_CopyToken(entry->code, sizeof(entry->code), normalized_code, "UNSPECIFIED");
    IotEvents_CopyToken(entry->source, sizeof(entry->source), source, "SYSTEM");
    IotEvents_CopyMessage(entry->message,
                          sizeof(entry->message),
                          message,
                          config->diagnostics_max_message_bytes);

    message_sequence++;
    id_prefix = (type == IOT_MESSAGE_ALERT) ? "evt" : "log";
    (void)snprintf(entry->message_id,
                   sizeof(entry->message_id),
                   "%s-%08lx-%08lx",
                   id_prefix,
                   (unsigned long)boot_nonce,
                   (unsigned long)message_sequence);
    entry->message_id[sizeof(entry->message_id) - 1U] = '\0';
    queue_count++;

    memset(throttle, 0, sizeof(*throttle));
    throttle->used = 1U;
    throttle->type = (uint8_t)type;
    throttle->last_queued_ms = now;
    IotEvents_CopyToken(throttle->code,
                        sizeof(throttle->code),
                        normalized_code,
                        "UNSPECIFIED");

    return IOT_EVENT_QUEUED;
}

void IotEvents_Init(void)
{
    uint32_t random_value = 0U;

    memset(event_queue, 0, sizeof(event_queue));
    memset(throttle_entries, 0, sizeof(throttle_entries));
    queue_count = 0U;
    dropped_count = 0U;
    message_sequence = 0U;

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

IotEventResult IotEvents_QueueAlert(IotAlertSeverity severity,
                                    const char *code,
                                    const char *message)
{
    return IotEvents_Queue(IOT_MESSAGE_ALERT,
                           (uint8_t)severity,
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
                           source,
                           code,
                           message);
}

const IotEventMessage *IotEvents_Peek(void)
{
    return (queue_count > 0U) ? &event_queue[0] : NULL;
}

void IotEvents_Pop(void)
{
    IotEvents_RemoveAt(0U);
}

size_t IotEvents_PendingCount(void)
{
    return queue_count;
}

uint32_t IotEvents_DroppedCount(void)
{
    return dropped_count;
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
