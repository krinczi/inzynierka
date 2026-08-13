#ifndef INC_IOT_EVENTS_H_
#define INC_IOT_EVENTS_H_

#include <stddef.h>
#include <stdint.h>

#define IOT_EVENT_SCHEMA_VERSION       1U
#define IOT_EVENT_QUEUE_CAPACITY       32U
#define IOT_EVENT_ID_CAPACITY          32U
#define IOT_EVENT_CODE_CAPACITY        40U
#define IOT_EVENT_SOURCE_CAPACITY      24U
#define IOT_EVENT_MESSAGE_CAPACITY     385U
#define IOT_TELEMETRY_QUEUE_CAPACITY    16U
#define IOT_TELEMETRY_MAX_MEASUREMENTS  32U
#define IOT_COMMAND_HISTORY_CAPACITY      8U
#define IOT_COMMAND_ID_CAPACITY          96U
#define IOT_COMMAND_STATUS_CAPACITY      16U
#define IOT_COMMAND_MESSAGE_CAPACITY    128U

#define IOT_EVENT_FLAG_DURABLE         0x01U

typedef enum
{
    IOT_MESSAGE_ALERT = 0,
    IOT_MESSAGE_DIAGNOSTIC
} IotMessageType;

typedef enum
{
    IOT_ALERT_INFO = 0,
    IOT_ALERT_WARNING,
    IOT_ALERT_ALARM,
    IOT_ALERT_CRITICAL
} IotAlertSeverity;

/* Values intentionally match DeviceLogLevel from device_config.h. */
typedef enum
{
    IOT_DIAGNOSTIC_ERROR = 0,
    IOT_DIAGNOSTIC_WARN,
    IOT_DIAGNOSTIC_INFO,
    IOT_DIAGNOSTIC_DEBUG,
    IOT_DIAGNOSTIC_TRACE
} IotDiagnosticLevel;

typedef enum
{
    IOT_EVENT_QUEUED = 0,
    IOT_EVENT_SUPPRESSED,
    IOT_EVENT_FILTERED,
    IOT_EVENT_QUEUE_FULL,
    IOT_EVENT_INVALID
} IotEventResult;

typedef struct
{
    IotMessageType type;
    uint8_t level;
    /* Uses padding that existed in journal v1, preserving its binary layout. */
    uint8_t flags;
    uint32_t uptime_ms;
    uint32_t suppressed_count;
    char message_id[IOT_EVENT_ID_CAPACITY];
    char code[IOT_EVENT_CODE_CAPACITY];
    char source[IOT_EVENT_SOURCE_CAPACITY];
    char message[IOT_EVENT_MESSAGE_CAPACITY];
} IotEventMessage;

/*
 * Compact, topic-independent representation used by the persistent outbox.
 * Metric ids and units are resolved from the stable sensor slot registry when
 * a stored sample is replayed. This keeps a complete 32-metric sample below a
 * single 1024-byte Flash journal record.
 */
typedef struct __attribute__((packed))
{
    uint8_t slot;
    int8_t scale10;
    uint8_t quality;
    uint8_t reserved;
    int64_t value;
} IotTelemetryMeasurement;

typedef struct
{
    char sample_id[IOT_EVENT_ID_CAPACITY];
    uint32_t sampled_uptime_ms;
    uint32_t sampled_unix_seconds;
    uint32_t config_version;
    uint32_t enabled_metrics;
    uint8_t measurement_count;
    uint8_t interaction_mode;
    uint16_t reserved;
    IotTelemetryMeasurement measurements[IOT_TELEMETRY_MAX_MEASUREMENTS];
} IotTelemetrySample;

typedef struct
{
    char command_id[IOT_COMMAND_ID_CAPACITY];
    char status[IOT_COMMAND_STATUS_CAPACITY];
    char message[IOT_COMMAND_MESSAGE_CAPACITY];
    uint32_t executed_uptime_ms;
    uint32_t executed_unix_seconds;
} IotCommandResult;

void IotEvents_Init(void);
/* Service lifecycle operation. Erases the durable local event journal. */
int IotEvents_FactoryReset(void);
IotEventResult IotEvents_QueueAlert(IotAlertSeverity severity,
                                    const char *code,
                                    const char *message);
IotEventResult IotEvents_QueueDiagnostic(IotDiagnosticLevel level,
                                         const char *source,
                                         const char *code,
                                         const char *message);
/*
 * Queues a low-volume audit diagnostic that must survive reset. This API is
 * intended for state transitions, not periodic DEBUG/TRACE output.
 */
IotEventResult IotEvents_QueueDiagnosticDurable(IotDiagnosticLevel level,
                                                const char *source,
                                                const char *code,
                                                const char *message);
int IotEvents_PeekCopy(IotEventMessage *event);
/*
 * Removes the event only after a durable acknowledgement has been written.
 * Returns 0 when removed or not matched, -1 on persistent journal failure.
 */
int IotEvents_PopIfMatch(const char *message_id);

/* Persistent bounded telemetry store-and-forward queue. */
int IotEvents_AssignTelemetryId(IotTelemetrySample *sample);
int IotEvents_QueueTelemetry(const IotTelemetrySample *sample);
int IotEvents_PeekTelemetry(IotTelemetrySample *sample);
int IotEvents_PopTelemetryIfMatch(const char *sample_id);
size_t IotEvents_TelemetryPendingCount(void);
uint32_t IotEvents_TelemetryDroppedCount(void);

/* Persistent replay cache protecting command execution across resets. */
int IotEvents_RememberCommandResult(const IotCommandResult *result);
int IotEvents_FindCommandResult(const char *command_id,
                                IotCommandResult *result);
size_t IotEvents_CommandHistoryCount(void);

size_t IotEvents_PendingCount(void);
size_t IotEvents_DurablePendingCount(void);
uint32_t IotEvents_DroppedCount(void);
uint32_t IotEvents_PersistenceFailureCount(void);
int IotEvents_IsDurable(const IotEventMessage *event);
const char *IotEvents_SeverityName(uint8_t severity);
const char *IotEvents_DiagnosticLevelName(uint8_t level);

#endif /* INC_IOT_EVENTS_H_ */
