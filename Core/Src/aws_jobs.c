#include "aws_jobs.h"

#include "debug_logger.h"
#include "device_config.h"
#include "flash_staging.h"
#include "https_downloader.h"
#include "main.h"
#include "ota_manager.h"
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define AWS_JOBS_JOB_ID_MAX_LENGTH       64U
#define AWS_JOBS_OPERATION_MAX_LENGTH    32U
#define AWS_JOBS_MODE_MAX_LENGTH         24U
#define AWS_JOBS_URL_MAX_LENGTH         HTTPS_DOWNLOADER_URL_MAX_LENGTH
#define AWS_JOBS_SHA256_HEX_LENGTH       64U
#define AWS_JOBS_JSON_MAX_DEPTH          8U
#define AWS_JOBS_STEP_TIMEOUT_MINUTES    10U
#define AWS_JOBS_RETRY_BASE_MS            5000U
#define AWS_JOBS_RETRY_MAX_MS             60000U
#define AWS_JOBS_RESPONSE_TIMEOUT_MS       15000U
#define AWS_JOBS_TOPIC_MAX_LENGTH            256U
#define AWS_JOBS_SUBSCRIPTION_COUNT            5U

typedef enum
{
    AWS_JOBS_STATE_IDLE = 0,
    AWS_JOBS_STATE_START_NEXT_PENDING,
    AWS_JOBS_STATE_EXECUTION_ACTIVE,
    AWS_JOBS_STATE_UPDATE_PENDING,
    AWS_JOBS_STATE_PAUSED
} AwsJobsState;

typedef struct
{
    const char *start;
    const char *end;
} JsonSlice;

typedef enum
{
    AWS_JOBS_DOCUMENT_VALIDATE_ONLY = 0,
    AWS_JOBS_DOCUMENT_DOWNLOAD
} AwsJobsDocumentMode;

typedef enum
{
    AWS_JOBS_ARTIFACT_SERVICE_FILE = 0,
    AWS_JOBS_ARTIFACT_FIRMWARE
} AwsJobsArtifactType;

typedef struct
{
    char url[AWS_JOBS_URL_MAX_LENGTH + 1U];
    uint32_t size_bytes;
    uint8_t sha256[FLASH_STAGING_SHA256_SIZE];
    uint32_t image_version;
    uint32_t rollback_from_version;
    uint8_t allow_downgrade;
    AwsJobsDocumentMode mode;
    AwsJobsArtifactType type;
} AwsJobsArtifact;

typedef struct
{
    AwsJobsPublishCallback publish;
    AwsJobsDownloadStartCallback download_start;
    AwsJobsState state;
    uint32_t token_counter;
    uint32_t execution_version;
    uint32_t retry_after_ms;
    uint32_t response_deadline_ms;
    uint8_t retry_attempt;
    uint8_t pending_install;
    uint8_t pending_ota_report;
    char active_job_id[AWS_JOBS_JOB_ID_MAX_LENGTH + 1U];
    char pending_status[16];
    char base_topic[AWS_JOBS_TOPIC_MAX_LENGTH];
    char start_next_topic[AWS_JOBS_TOPIC_MAX_LENGTH];
    char start_next_accepted_topic[AWS_JOBS_TOPIC_MAX_LENGTH];
    char start_next_rejected_topic[AWS_JOBS_TOPIC_MAX_LENGTH];
    char update_accepted_filter[AWS_JOBS_TOPIC_MAX_LENGTH];
    char update_rejected_filter[AWS_JOBS_TOPIC_MAX_LENGTH];
    char notify_next_topic[AWS_JOBS_TOPIC_MAX_LENGTH];
} AwsJobsContext;

enum
{
    JSON_MEMBER_FOUND = 0,
    JSON_MEMBER_NOT_FOUND = 1,
    JSON_MEMBER_INVALID = -1
};

static AwsJobsContext jobs_context;

static void Jobs_Log(const char *message)
{
    Debug_LogLine(DEBUG_SRC_TLS, message);
}

static void Jobs_Logf(const char *format, ...)
{
    char message[160];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Debug_LogLine(DEBUG_SRC_TLS, message);
}

static const char *Json_SkipWhitespace(const char *cursor, const char *end)
{
    while ((cursor < end) &&
           ((*cursor == ' ') || (*cursor == '\t') ||
            (*cursor == '\r') || (*cursor == '\n')))
    {
        cursor++;
    }

    return cursor;
}

static int Json_IsHexDigit(char value)
{
    return ((value >= '0') && (value <= '9')) ||
           ((value >= 'a') && (value <= 'f')) ||
           ((value >= 'A') && (value <= 'F'));
}

static int Json_HexValue(char value, uint8_t *output)
{
    if ((value >= '0') && (value <= '9'))
    {
        *output = (uint8_t)(value - '0');
        return 0;
    }
    if ((value >= 'a') && (value <= 'f'))
    {
        *output = (uint8_t)(value - 'a' + 10);
        return 0;
    }
    if ((value >= 'A') && (value <= 'F'))
    {
        *output = (uint8_t)(value - 'A' + 10);
        return 0;
    }

    return -1;
}

static int Json_SkipString(const char *cursor,
                           const char *end,
                           const char **after)
{
    if ((cursor >= end) || (*cursor != '"'))
    {
        return -1;
    }

    cursor++;
    while (cursor < end)
    {
        unsigned char value = (unsigned char)*cursor++;

        if (value == '"')
        {
            *after = cursor;
            return 0;
        }

        if (value < 0x20U)
        {
            return -1;
        }

        if (value == '\\')
        {
            char escape;

            if (cursor >= end)
            {
                return -1;
            }

            escape = *cursor++;
            if (escape == 'u')
            {
                size_t index;

                if ((size_t)(end - cursor) < 4U)
                {
                    return -1;
                }

                for (index = 0U; index < 4U; index++)
                {
                    if (!Json_IsHexDigit(cursor[index]))
                    {
                        return -1;
                    }
                }
                cursor += 4U;
            }
            else if ((escape != '"') && (escape != '\\') &&
                     (escape != '/') && (escape != 'b') &&
                     (escape != 'f') && (escape != 'n') &&
                     (escape != 'r') && (escape != 't'))
            {
                return -1;
            }
        }
    }

    return -1;
}

static int Json_SkipValue(const char *cursor,
                          const char *end,
                          uint32_t depth,
                          const char **after)
{
    const char *next;

    if (depth > AWS_JOBS_JSON_MAX_DEPTH)
    {
        return -1;
    }

    cursor = Json_SkipWhitespace(cursor, end);
    if (cursor >= end)
    {
        return -1;
    }

    if (*cursor == '"')
    {
        return Json_SkipString(cursor, end, after);
    }

    if (*cursor == '{')
    {
        cursor = Json_SkipWhitespace(cursor + 1, end);
        if ((cursor < end) && (*cursor == '}'))
        {
            *after = cursor + 1;
            return 0;
        }

        for (;;)
        {
            if (Json_SkipString(cursor, end, &next) != 0)
            {
                return -1;
            }

            cursor = Json_SkipWhitespace(next, end);
            if ((cursor >= end) || (*cursor != ':'))
            {
                return -1;
            }

            if (Json_SkipValue(cursor + 1, end, depth + 1U, &next) != 0)
            {
                return -1;
            }

            cursor = Json_SkipWhitespace(next, end);
            if ((cursor < end) && (*cursor == '}'))
            {
                *after = cursor + 1;
                return 0;
            }

            if ((cursor >= end) || (*cursor != ','))
            {
                return -1;
            }
            cursor = Json_SkipWhitespace(cursor + 1, end);
        }
    }

    if (*cursor == '[')
    {
        cursor = Json_SkipWhitespace(cursor + 1, end);
        if ((cursor < end) && (*cursor == ']'))
        {
            *after = cursor + 1;
            return 0;
        }

        for (;;)
        {
            if (Json_SkipValue(cursor, end, depth + 1U, &next) != 0)
            {
                return -1;
            }

            cursor = Json_SkipWhitespace(next, end);
            if ((cursor < end) && (*cursor == ']'))
            {
                *after = cursor + 1;
                return 0;
            }

            if ((cursor >= end) || (*cursor != ','))
            {
                return -1;
            }
            cursor = Json_SkipWhitespace(cursor + 1, end);
        }
    }

    next = cursor;
    while ((next < end) && (*next != ',') && (*next != '}') &&
           (*next != ']') && (*next != ' ') && (*next != '\t') &&
           (*next != '\r') && (*next != '\n'))
    {
        next++;
    }

    if (next == cursor)
    {
        return -1;
    }

    *after = next;
    return 0;
}

static int Json_KeyEquals(const char *start,
                          const char *end,
                          const char *expected)
{
    size_t expected_length = strlen(expected);
    size_t actual_length = (size_t)(end - start);

    if ((memchr(start, '\\', actual_length) != NULL) ||
        (actual_length != expected_length))
    {
        return 0;
    }

    return memcmp(start, expected, expected_length) == 0;
}

static int Json_ObjectFind(JsonSlice object,
                           const char *member_name,
                           JsonSlice *value)
{
    const char *cursor = Json_SkipWhitespace(object.start, object.end);
    int found = 0;

    if ((cursor >= object.end) || (*cursor != '{'))
    {
        return JSON_MEMBER_INVALID;
    }

    cursor = Json_SkipWhitespace(cursor + 1, object.end);
    if ((cursor < object.end) && (*cursor == '}'))
    {
        return JSON_MEMBER_NOT_FOUND;
    }

    for (;;)
    {
        const char *key_start;
        const char *key_after;
        const char *value_start;
        const char *value_after;

        if ((cursor >= object.end) || (*cursor != '"'))
        {
            return JSON_MEMBER_INVALID;
        }

        key_start = cursor + 1;
        if (Json_SkipString(cursor, object.end, &key_after) != 0)
        {
            return JSON_MEMBER_INVALID;
        }

        cursor = Json_SkipWhitespace(key_after, object.end);
        if ((cursor >= object.end) || (*cursor != ':'))
        {
            return JSON_MEMBER_INVALID;
        }

        value_start = Json_SkipWhitespace(cursor + 1, object.end);
        if (Json_SkipValue(value_start, object.end, 1U, &value_after) != 0)
        {
            return JSON_MEMBER_INVALID;
        }

        if (Json_KeyEquals(key_start, key_after - 1, member_name))
        {
            if (found)
            {
                return JSON_MEMBER_INVALID;
            }

            value->start = value_start;
            value->end = value_after;
            found = 1;
        }

        cursor = Json_SkipWhitespace(value_after, object.end);
        if ((cursor < object.end) && (*cursor == '}'))
        {
            return found ? JSON_MEMBER_FOUND : JSON_MEMBER_NOT_FOUND;
        }

        if ((cursor >= object.end) || (*cursor != ','))
        {
            return JSON_MEMBER_INVALID;
        }
        cursor = Json_SkipWhitespace(cursor + 1, object.end);
    }
}

static int Json_SliceIsObject(JsonSlice value)
{
    const char *cursor = Json_SkipWhitespace(value.start, value.end);
    const char *after;

    if ((cursor >= value.end) || (*cursor != '{') ||
        (Json_SkipValue(cursor, value.end, 1U, &after) != 0))
    {
        return 0;
    }

    return Json_SkipWhitespace(after, value.end) == value.end;
}

static int Json_SliceIsNull(JsonSlice value)
{
    const char *start = Json_SkipWhitespace(value.start, value.end);
    const char *end = value.end;

    while ((end > start) &&
           ((end[-1] == ' ') || (end[-1] == '\t') ||
            (end[-1] == '\r') || (end[-1] == '\n')))
    {
        end--;
    }

    return ((size_t)(end - start) == 4U) &&
           (memcmp(start, "null", 4U) == 0);
}

static int Json_CopyString(JsonSlice value, char *output, size_t output_size)
{
    const char *cursor = Json_SkipWhitespace(value.start, value.end);
    size_t length = 0U;

    if ((output_size == 0U) || (cursor >= value.end) || (*cursor != '"'))
    {
        return -1;
    }

    cursor++;
    while (cursor < value.end)
    {
        unsigned char decoded = (unsigned char)*cursor++;

        if (decoded == '"')
        {
            if (Json_SkipWhitespace(cursor, value.end) != value.end)
            {
                return -1;
            }

            output[length] = '\0';
            return 0;
        }

        if ((decoded < 0x20U) || ((length + 1U) >= output_size))
        {
            return -1;
        }

        if (decoded == '\\')
        {
            char escape;

            if (cursor >= value.end)
            {
                return -1;
            }

            escape = *cursor++;
            switch (escape)
            {
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'u':
            {
                uint8_t digit;
                uint16_t code_point = 0U;
                size_t index;

                if ((size_t)(value.end - cursor) < 4U)
                {
                    return -1;
                }

                for (index = 0U; index < 4U; index++)
                {
                    if (Json_HexValue(cursor[index], &digit) != 0)
                    {
                        return -1;
                    }
                    code_point = (uint16_t)((code_point << 4U) | digit);
                }
                cursor += 4U;

                /* AWS may JSON-escape printable URL delimiters such as '&'. */
                if ((code_point < 0x20U) || (code_point >= 0x7FU))
                {
                    return -1;
                }
                decoded = (unsigned char)code_point;
                break;
            }
            default:
                return -1;
            }
        }

        if (decoded < 0x20U)
        {
            return -1;
        }

        output[length++] = (char)decoded;
    }

    return -1;
}

static int Json_GetUInt32(JsonSlice value, uint32_t *output)
{
    const char *cursor = Json_SkipWhitespace(value.start, value.end);
    uint32_t result = 0U;
    int has_digit = 0;

    while ((cursor < value.end) && (*cursor >= '0') && (*cursor <= '9'))
    {
        uint32_t digit = (uint32_t)(*cursor - '0');

        if (result > ((UINT32_MAX - digit) / 10U))
        {
            return -1;
        }

        result = (result * 10U) + digit;
        has_digit = 1;
        cursor++;
    }

    if (!has_digit || (Json_SkipWhitespace(cursor, value.end) != value.end))
    {
        return -1;
    }

    *output = result;
    return 0;
}

static int Json_GetRequiredObjectMember(JsonSlice object,
                                        const char *name,
                                        JsonSlice *value)
{
    return (Json_ObjectFind(object, name, value) == JSON_MEMBER_FOUND) &&
           Json_SliceIsObject(*value)
               ? 0
               : -1;
}

static int Json_GetRequiredString(JsonSlice object,
                                  const char *name,
                                  char *output,
                                  size_t output_size)
{
    JsonSlice value;

    return (Json_ObjectFind(object, name, &value) == JSON_MEMBER_FOUND) &&
           (Json_CopyString(value, output, output_size) == 0)
               ? 0
               : -1;
}

static int Json_GetRequiredUInt32(JsonSlice object,
                                  const char *name,
                                  uint32_t *output)
{
    JsonSlice value;

    return (Json_ObjectFind(object, name, &value) == JSON_MEMBER_FOUND) &&
           (Json_GetUInt32(value, output) == 0)
               ? 0
               : -1;
}

static int Json_GetOptionalUInt32(JsonSlice object,
                                  const char *name,
                                  uint32_t *output,
                                  int *present)
{
    JsonSlice value;
    int result = Json_ObjectFind(object, name, &value);

    *present = result == JSON_MEMBER_FOUND;
    if (result == JSON_MEMBER_NOT_FOUND)
    {
        *output = 0U;
        return 0;
    }
    return (result == JSON_MEMBER_FOUND) &&
           (Json_GetUInt32(value, output) == 0)
               ? 0
               : -1;
}

static int Jobs_GetAcceptedExecutionVersion(const char *payload,
                                             uint32_t *execution_version)
{
    JsonSlice root;
    JsonSlice execution_state;

    if ((payload == NULL) || (execution_version == NULL))
    {
        return -1;
    }

    root.start = payload;
    root.end = payload + strlen(payload);
    if (!Json_SliceIsObject(root) ||
        (Json_GetRequiredObjectMember(root,
                                      "executionState",
                                      &execution_state) != 0))
    {
        return -1;
    }

    return Json_GetRequiredUInt32(execution_state,
                                  "versionNumber",
                                  execution_version);
}

static int Jobs_IsTerminalExecutionStatus(const char *status)
{
    return (strcmp(status, "SUCCEEDED") == 0) ||
           (strcmp(status, "FAILED") == 0) ||
           (strcmp(status, "TIMED_OUT") == 0) ||
           (strcmp(status, "REJECTED") == 0) ||
           (strcmp(status, "REMOVED") == 0) ||
           (strcmp(status, "CANCELED") == 0);
}

static int Jobs_DurableReportIsObsolete(const char *payload,
                                        char *error_code,
                                        size_t error_code_size,
                                        char *execution_status,
                                        size_t execution_status_size)
{
    JsonSlice root;
    JsonSlice execution_state;

    if ((payload == NULL) || (error_code == NULL) ||
        (error_code_size == 0U) || (execution_status == NULL) ||
        (execution_status_size == 0U))
    {
        return 0;
    }

    (void)snprintf(error_code, error_code_size, "%s", "unknown");
    execution_status[0] = '\0';
    root.start = payload;
    root.end = payload + strlen(payload);
    if (!Json_SliceIsObject(root) ||
        (Json_GetRequiredString(root,
                                "code",
                                error_code,
                                error_code_size) != 0))
    {
        return 0;
    }

    if ((strcmp(error_code, "ResourceNotFound") == 0) ||
        (strcmp(error_code, "TerminalStateReached") == 0))
    {
        return 1;
    }

    if ((strcmp(error_code, "InvalidStateTransition") != 0) &&
        (strcmp(error_code, "VersionMismatch") != 0))
    {
        return 0;
    }

    if ((Json_GetRequiredObjectMember(root,
                                      "executionState",
                                      &execution_state) != 0) ||
        (Json_GetRequiredString(execution_state,
                                "status",
                                execution_status,
                                execution_status_size) != 0))
    {
        return 0;
    }

    return Jobs_IsTerminalExecutionStatus(execution_status);
}

static int Jobs_IsValidJobId(const char *job_id)
{
    size_t index;
    size_t length = strlen(job_id);

    if ((length == 0U) || (length > AWS_JOBS_JOB_ID_MAX_LENGTH))
    {
        return 0;
    }

    for (index = 0U; index < length; index++)
    {
        char value = job_id[index];

        if (!(((value >= 'a') && (value <= 'z')) ||
              ((value >= 'A') && (value <= 'Z')) ||
              ((value >= '0') && (value <= '9')) ||
              (value == '-') || (value == '_')))
        {
            return 0;
        }
    }

    return 1;
}

static int Jobs_HexNibble(char value, uint8_t *output)
{
    if ((value >= '0') && (value <= '9'))
    {
        *output = (uint8_t)(value - '0');
        return 0;
    }

    if ((value >= 'a') && (value <= 'f'))
    {
        *output = (uint8_t)(value - 'a' + 10);
        return 0;
    }

    if ((value >= 'A') && (value <= 'F'))
    {
        *output = (uint8_t)(value - 'A' + 10);
        return 0;
    }

    return -1;
}

static int Jobs_DecodeSha256(const char *hex,
                             uint8_t output[FLASH_STAGING_SHA256_SIZE])
{
    size_t index;

    if (strlen(hex) != AWS_JOBS_SHA256_HEX_LENGTH)
    {
        return -1;
    }

    for (index = 0U; index < FLASH_STAGING_SHA256_SIZE; index++)
    {
        uint8_t high;
        uint8_t low;

        if ((Jobs_HexNibble(hex[index * 2U], &high) != 0) ||
            (Jobs_HexNibble(hex[(index * 2U) + 1U], &low) != 0))
        {
            return -1;
        }

        output[index] = (uint8_t)((high << 4U) | low);
    }

    return 0;
}

static int Jobs_IsValidHttpsUrl(const char *url)
{
    size_t index;

    if ((strncmp(url, "https://", 8U) != 0) || (url[8] == '\0'))
    {
        return 0;
    }

    for (index = 0U; url[index] != '\0'; index++)
    {
        unsigned char value = (unsigned char)url[index];

        if ((value <= 0x20U) || (value >= 0x7FU))
        {
            return 0;
        }
    }

    return 1;
}

static int Jobs_ValidateDocument(JsonSlice document,
                                 AwsJobsArtifact *artifact,
                                 const char **reason)
{
    JsonSlice artifact_object;
    JsonSlice url_value;
    uint32_t schema_version;
    char operation[AWS_JOBS_OPERATION_MAX_LENGTH + 1U];
    char mode[AWS_JOBS_MODE_MAX_LENGTH + 1U];
    char sha256_hex[AWS_JOBS_SHA256_HEX_LENGTH + 1U];
    char artifact_type[24];
    const FlashStagingInfo *staging_info = FlashStaging_GetInfo();

    if ((Json_GetRequiredUInt32(document, "schemaVersion", &schema_version) != 0) ||
        (schema_version != 1U))
    {
        *reason = "unsupported_schema";
        return -1;
    }

    if ((Json_GetRequiredString(document,
                                "operation",
                                operation,
                                sizeof(operation)) != 0) ||
        (strcmp(operation, "stage-file") != 0))
    {
        *reason = "unsupported_operation";
        return -1;
    }

    if (Json_GetRequiredString(document, "mode", mode, sizeof(mode)) != 0)
    {
        *reason = "unsupported_mode";
        return -1;
    }
    if (strcmp(mode, "validate-only") == 0)
    {
        artifact->mode = AWS_JOBS_DOCUMENT_VALIDATE_ONLY;
    }
    else if (strcmp(mode, "download") == 0)
    {
        artifact->mode = AWS_JOBS_DOCUMENT_DOWNLOAD;
    }
    else
    {
        *reason = "unsupported_mode";
        return -1;
    }

    if (Json_GetRequiredObjectMember(document,
                                     "artifact",
                                     &artifact_object) != 0)
    {
        *reason = "invalid_artifact";
        return -1;
    }

    if (Json_ObjectFind(artifact_object, "url", &url_value) != JSON_MEMBER_FOUND)
    {
        Jobs_Log("JOBS: artifact URL field missing or invalid");
        *reason = "invalid_https_url";
        return -1;
    }

    if (Json_CopyString(url_value, artifact->url, sizeof(artifact->url)) != 0)
    {
        Jobs_Logf("JOBS: artifact URL decode failed encodedBytes=%lu capacity=%lu",
                  (unsigned long)(url_value.end - url_value.start),
                  (unsigned long)(sizeof(artifact->url) - 1U));
        *reason = "invalid_https_url";
        return -1;
    }

    if (!Jobs_IsValidHttpsUrl(artifact->url))
    {
        Jobs_Logf("JOBS: artifact URL validation failed decodedBytes=%lu",
                  (unsigned long)strlen(artifact->url));
        *reason = "invalid_https_url";
        return -1;
    }

    if ((Json_GetRequiredUInt32(artifact_object,
                                "size",
                                &artifact->size_bytes) != 0) ||
        (artifact->size_bytes == 0U) ||
        (artifact->size_bytes > staging_info->capacity_bytes))
    {
        *reason = "invalid_artifact_size";
        return -1;
    }

    if ((Json_GetRequiredString(artifact_object,
                                "sha256",
                                sha256_hex,
                                sizeof(sha256_hex)) != 0) ||
        (Jobs_DecodeSha256(sha256_hex, artifact->sha256) != 0))
    {
        *reason = "invalid_sha256";
        return -1;
    }

    artifact->type = AWS_JOBS_ARTIFACT_SERVICE_FILE;
    {
        JsonSlice artifact_type_value;
        int artifact_type_result =
            Json_ObjectFind(artifact_object,
                            "artifactType",
                            &artifact_type_value);

        if (artifact_type_result == JSON_MEMBER_FOUND)
        {
            if (Json_CopyString(artifact_type_value,
                                artifact_type,
                                sizeof(artifact_type)) != 0)
            {
                *reason = "invalid_artifact_type";
                return -1;
            }

            if (strcmp(artifact_type, "firmware") == 0)
            {
                int rollback_authorized_present;
                int rollback_from_present;
                uint32_t rollback_authorized;

                artifact->type = AWS_JOBS_ARTIFACT_FIRMWARE;
                if ((Json_GetRequiredUInt32(artifact_object,
                                            "version",
                                            &artifact->image_version) != 0) ||
                    (artifact->image_version == 0U))
                {
                    *reason = "firmware_version_required";
                    return -1;
                }
                if ((Json_GetOptionalUInt32(artifact_object,
                                            "rollbackAuthorized",
                                            &rollback_authorized,
                                            &rollback_authorized_present) != 0) ||
                    (Json_GetOptionalUInt32(artifact_object,
                                            "rollbackFromVersion",
                                            &artifact->rollback_from_version,
                                            &rollback_from_present) != 0) ||
                    (rollback_authorized_present != rollback_from_present) ||
                    (rollback_authorized_present &&
                     ((rollback_authorized != 1U) ||
                      (artifact->rollback_from_version == 0U))))
                {
                    *reason = "invalid_rollback_authorization";
                    return -1;
                }
                artifact->allow_downgrade =
                    rollback_authorized_present ? 1U : 0U;
                if (artifact->allow_downgrade &&
                    (DeviceConfig_Get()->allow_rollback == 0U))
                {
                    *reason = "rollback_disabled";
                    return -1;
                }
            }
            else if (strcmp(artifact_type, "service-file") != 0)
            {
                *reason = "unsupported_artifact_type";
                return -1;
            }
        }
        else if (artifact_type_result == JSON_MEMBER_INVALID)
        {
            *reason = "invalid_artifact_type";
            return -1;
        }
    }

    *reason = "document_valid";
    return 0;
}

static void Jobs_ClearActiveExecution(void)
{
    jobs_context.execution_version = 0U;
    jobs_context.active_job_id[0] = '\0';
    jobs_context.pending_status[0] = '\0';
}

static void Jobs_ScheduleRetry(const char *reason)
{
    uint32_t delay_ms = AWS_JOBS_RETRY_BASE_MS;
    uint8_t shift = jobs_context.retry_attempt;

    if (shift > 3U)
    {
        shift = 3U;
    }
    delay_ms <<= shift;
    if (delay_ms > AWS_JOBS_RETRY_MAX_MS)
    {
        delay_ms = AWS_JOBS_RETRY_MAX_MS;
    }

    if (jobs_context.retry_attempt < UINT8_MAX)
    {
        jobs_context.retry_attempt++;
    }

    jobs_context.retry_after_ms = HAL_GetTick() + delay_ms;
    jobs_context.response_deadline_ms = 0U;
    jobs_context.state = AWS_JOBS_STATE_PAUSED;
    Jobs_Logf("JOBS: retry scheduled in %lu ms reason=%s",
              (unsigned long)delay_ms,
              reason);
}

static int Jobs_RequestNext(void)
{
    char payload[256];
    int written;

    if (jobs_context.state != AWS_JOBS_STATE_IDLE)
    {
        return 0;
    }

    jobs_context.token_counter++;
    written = snprintf(payload,
                       sizeof(payload),
                       "{\"statusDetails\":{\"phase\":\"STARTING\"},"
                       "\"stepTimeoutInMinutes\":%u,"
                       "\"clientToken\":\"stm32-start-%lu\"}",
                       (unsigned int)AWS_JOBS_STEP_TIMEOUT_MINUTES,
                       (unsigned long)jobs_context.token_counter);

    if ((written < 0) || ((size_t)written >= sizeof(payload)))
    {
        Jobs_Log("JOBS: start-next payload too large");
        return -1;
    }

    if ((jobs_context.publish == NULL) ||
        (jobs_context.publish(jobs_context.start_next_topic, payload) != 0))
    {
        Jobs_Log("JOBS: start-next publish failed");
        return -1;
    }

    jobs_context.state = AWS_JOBS_STATE_START_NEXT_PENDING;
    jobs_context.response_deadline_ms = HAL_GetTick() + AWS_JOBS_RESPONSE_TIMEOUT_MS;
    Jobs_Log("JOBS: start-next requested");
    return 0;
}

static int Jobs_PublishTerminalStatus(const char *status,
                                      const char *phase,
                                      const char *detail)
{
    char topic[AWS_JOBS_TOPIC_MAX_LENGTH];
    char payload[512];
    int topic_written;
    int payload_written;

    jobs_context.token_counter++;
    topic_written = snprintf(topic,
                             sizeof(topic),
                             "%s/%s/update",
                             jobs_context.base_topic,
                             jobs_context.active_job_id);
    if (jobs_context.pending_ota_report != 0U)
    {
        /* Post-reset reports may omit an unpersisted expectedVersion. */
        payload_written = snprintf(payload,
                                   sizeof(payload),
                                   "{\"status\":\"%s\",\"statusDetails\":{"
                                   "\"phase\":\"%s\",\"detail\":\"%s\"},"
                                   "\"includeJobExecutionState\":true,"
                                   "\"clientToken\":\"stm32-update-%lu\"}",
                                   status,
                                   phase,
                                   detail,
                                   (unsigned long)jobs_context.token_counter);
    }
    else
    {
        payload_written = snprintf(payload,
                                   sizeof(payload),
                                   "{\"status\":\"%s\",\"statusDetails\":{"
                                   "\"phase\":\"%s\",\"detail\":\"%s\"},"
                                   "\"expectedVersion\":%lu,"
                                   "\"includeJobExecutionState\":true,"
                                   "\"clientToken\":\"stm32-update-%lu\"}",
                                   status,
                                   phase,
                                   detail,
                                   (unsigned long)jobs_context.execution_version,
                                   (unsigned long)jobs_context.token_counter);
    }

    if ((topic_written < 0) || ((size_t)topic_written >= sizeof(topic)) ||
        (payload_written < 0) || ((size_t)payload_written >= sizeof(payload)))
    {
        Jobs_Log("JOBS: update payload or topic too large");
        return -1;
    }

    if ((jobs_context.publish == NULL) ||
        (jobs_context.publish(topic, payload) != 0))
    {
        Jobs_Logf("JOBS: update publish failed job=%s",
                  jobs_context.active_job_id);
        return -1;
    }

    (void)snprintf(jobs_context.pending_status,
                   sizeof(jobs_context.pending_status),
                   "%s",
                   status);
    jobs_context.state = AWS_JOBS_STATE_UPDATE_PENDING;
    jobs_context.response_deadline_ms = HAL_GetTick() + AWS_JOBS_RESPONSE_TIMEOUT_MS;
    Jobs_Logf("JOBS: status=%s published job=%s",
              status,
              jobs_context.active_job_id);
    return 0;
}

static int Jobs_HandleStartNextAccepted(const char *payload)
{
    JsonSlice root = { payload, payload + strlen(payload) };
    JsonSlice execution;
    JsonSlice document;
    AwsJobsArtifact artifact;
    FlashStagingRequest download_request;
    const char *reason;
    int member_result;

    if (!Json_SliceIsObject(root))
    {
        Jobs_ScheduleRetry("invalid_start_response");
        Jobs_Log("JOBS: invalid start-next response");
        return 1;
    }

    member_result = Json_ObjectFind(root, "execution", &execution);
    if ((member_result == JSON_MEMBER_NOT_FOUND) ||
        ((member_result == JSON_MEMBER_FOUND) && Json_SliceIsNull(execution)))
    {
        jobs_context.state = AWS_JOBS_STATE_IDLE;
        jobs_context.retry_attempt = 0U;
        jobs_context.response_deadline_ms = 0U;
        Jobs_ClearActiveExecution();
        Jobs_Log("JOBS: no pending execution");
        return 1;
    }

    if ((member_result != JSON_MEMBER_FOUND) || !Json_SliceIsObject(execution))
    {
        Jobs_ScheduleRetry("invalid_start_response");
        Jobs_Log("JOBS: invalid start-next response");
        return 1;
    }

    if ((Json_GetRequiredString(execution,
                                "jobId",
                                jobs_context.active_job_id,
                                sizeof(jobs_context.active_job_id)) != 0) ||
        !Jobs_IsValidJobId(jobs_context.active_job_id) ||
        (Json_GetRequiredUInt32(execution,
                                "versionNumber",
                                &jobs_context.execution_version) != 0))
    {
        Jobs_ScheduleRetry("invalid_job_identity");
        Jobs_Log("JOBS: invalid job identity or version");
        return 1;
    }

    jobs_context.state = AWS_JOBS_STATE_EXECUTION_ACTIVE;
    if (Json_GetRequiredObjectMember(execution, "jobDocument", &document) != 0)
    {
        Jobs_Logf("JOBS: rejected job=%s reason=missing_job_document",
                  jobs_context.active_job_id);
        return Jobs_PublishTerminalStatus("REJECTED",
                                          "REJECTED",
                                          "missing_job_document") == 0
                   ? 1
                   : -1;
    }

    memset(&artifact, 0, sizeof(artifact));
    if (Jobs_ValidateDocument(document, &artifact, &reason) != 0)
    {
        Jobs_Logf("JOBS: rejected job=%s reason=%s",
                  jobs_context.active_job_id,
                  reason);
        return Jobs_PublishTerminalStatus("REJECTED", "REJECTED", reason) == 0
                   ? 1
                   : -1;
    }

    if (artifact.mode == AWS_JOBS_DOCUMENT_VALIDATE_ONLY)
    {
        Jobs_Logf("JOBS: validated job=%s artifactSize=%lu mode=validate-only",
                  jobs_context.active_job_id,
                  (unsigned long)artifact.size_bytes);
        return Jobs_PublishTerminalStatus("SUCCEEDED",
                                          "VALIDATED",
                                          "document_valid") == 0
                   ? 1
                   : -1;
    }

    memset(&download_request, 0, sizeof(download_request));
    download_request.type =
        artifact.type == AWS_JOBS_ARTIFACT_FIRMWARE
            ? FLASH_STAGING_ARTIFACT_FIRMWARE
            : FLASH_STAGING_ARTIFACT_SERVICE_FILE;
    download_request.file_size_bytes = artifact.size_bytes;
    memcpy(download_request.expected_sha256,
           artifact.sha256,
           sizeof(download_request.expected_sha256));
    download_request.expected_image_version = artifact.image_version;
    download_request.allow_downgrade = artifact.allow_downgrade;
    download_request.rollback_from_version = artifact.rollback_from_version;
    (void)snprintf(download_request.job_id,
                   sizeof(download_request.job_id),
                   "%s",
                   jobs_context.active_job_id);
    download_request.job_execution_version =
        jobs_context.execution_version;

    if ((jobs_context.download_start == NULL) ||
        (jobs_context.download_start(artifact.url,
                                     &download_request) != 0))
    {
        Jobs_Logf("JOBS: download start failed job=%s",
                  jobs_context.active_job_id);
        return Jobs_PublishTerminalStatus("FAILED",
                                          "DOWNLOAD_FAILED",
                                          "download_start_failed") == 0
                   ? 1
                   : -1;
    }

    jobs_context.state = AWS_JOBS_STATE_EXECUTION_ACTIVE;
    jobs_context.response_deadline_ms = 0U;
    Jobs_Logf("JOBS: download active job=%s artifactSize=%lu",
              jobs_context.active_job_id,
              (unsigned long)artifact.size_bytes);
    return 1;
}

static int Jobs_TopicHasUpdateSuffix(const char *topic, const char *suffix)
{
    const char *job_start;
    const char *job_end;
    size_t prefix_length = strlen(jobs_context.base_topic);

    if ((strncmp(topic, jobs_context.base_topic, prefix_length) != 0) ||
        (topic[prefix_length] != '/'))
    {
        return 0;
    }

    job_start = topic + prefix_length + 1U;
    job_end = strchr(job_start, '/');
    if ((job_end == NULL) || (job_end == job_start))
    {
        return 0;
    }

    return strcmp(job_end, suffix) == 0;
}

static int Jobs_TopicMatchesActiveUpdate(const char *topic, const char *suffix)
{
    char expected[AWS_JOBS_TOPIC_MAX_LENGTH];
    int written;

    if (jobs_context.active_job_id[0] == '\0')
    {
        return 0;
    }

    written = snprintf(expected,
                       sizeof(expected),
                       "%s/%s%s",
                       jobs_context.base_topic,
                       jobs_context.active_job_id,
                       suffix);
    return (written >= 0) && ((size_t)written < sizeof(expected)) &&
           (strcmp(topic, expected) == 0);
}

static int Jobs_BuildTopic(char *output,
                           size_t output_size,
                           const char *format,
                           const char *value)
{
    int written = snprintf(output, output_size, format, value);
    return ((written >= 0) && ((size_t)written < output_size)) ? 0 : -1;
}

int AwsJobs_Init(AwsJobsPublishCallback publish_callback,
                 AwsJobsDownloadStartCallback download_start_callback,
                 const char *thing_name)
{
    memset(&jobs_context, 0, sizeof(jobs_context));
    if ((publish_callback == NULL) || (download_start_callback == NULL) ||
        (thing_name == NULL) || (thing_name[0] == '\0'))
    {
        return -1;
    }

    if ((Jobs_BuildTopic(jobs_context.base_topic,
                         sizeof(jobs_context.base_topic),
                         "$aws/things/%s/jobs",
                         thing_name) != 0) ||
        (Jobs_BuildTopic(jobs_context.start_next_topic,
                         sizeof(jobs_context.start_next_topic),
                         "%s/start-next",
                         jobs_context.base_topic) != 0) ||
        (Jobs_BuildTopic(jobs_context.start_next_accepted_topic,
                         sizeof(jobs_context.start_next_accepted_topic),
                         "%s/accepted",
                         jobs_context.start_next_topic) != 0) ||
        (Jobs_BuildTopic(jobs_context.start_next_rejected_topic,
                         sizeof(jobs_context.start_next_rejected_topic),
                         "%s/rejected",
                         jobs_context.start_next_topic) != 0) ||
        (Jobs_BuildTopic(jobs_context.update_accepted_filter,
                         sizeof(jobs_context.update_accepted_filter),
                         "%s/+/update/accepted",
                         jobs_context.base_topic) != 0) ||
        (Jobs_BuildTopic(jobs_context.update_rejected_filter,
                         sizeof(jobs_context.update_rejected_filter),
                         "%s/+/update/rejected",
                         jobs_context.base_topic) != 0) ||
        (Jobs_BuildTopic(jobs_context.notify_next_topic,
                         sizeof(jobs_context.notify_next_topic),
                         "%s/notify-next",
                         jobs_context.base_topic) != 0))
    {
        memset(&jobs_context, 0, sizeof(jobs_context));
        return -1;
    }

    jobs_context.publish = publish_callback;
    jobs_context.download_start = download_start_callback;
    jobs_context.state = AWS_JOBS_STATE_IDLE;
    return 0;
}

size_t AwsJobs_SubscriptionCount(void)
{
    return AWS_JOBS_SUBSCRIPTION_COUNT;
}

const char *AwsJobs_SubscriptionTopic(size_t index)
{
    /* Subscribe to response topics first; notify-next is intentionally last. */
    switch (index)
    {
        case 0U:
            return jobs_context.start_next_accepted_topic;
        case 1U:
            return jobs_context.start_next_rejected_topic;
        case 2U:
            return jobs_context.update_accepted_filter;
        case 3U:
            return jobs_context.update_rejected_filter;
        case 4U:
            return jobs_context.notify_next_topic;
        default:
            return NULL;
    }
}

int AwsJobs_OnMqttConnected(void)
{
    OtaPendingJob ota_job;

    jobs_context.state = AWS_JOBS_STATE_IDLE;
    jobs_context.retry_attempt = 0U;
    jobs_context.retry_after_ms = 0U;
    jobs_context.response_deadline_ms = 0U;
    Jobs_ClearActiveExecution();

    if (OtaManager_ConfirmHealthy() != 0)
    {
        Jobs_Log("JOBS: OTA health confirmation failed");
        return -1;
    }

    if (OtaManager_GetReadyInstallJob(&ota_job) != 0)
    {
        (void)snprintf(jobs_context.active_job_id,
                       sizeof(jobs_context.active_job_id),
                       "%s",
                       ota_job.job_id);
        jobs_context.execution_version = ota_job.execution_version;
        jobs_context.state = AWS_JOBS_STATE_EXECUTION_ACTIVE;
        jobs_context.pending_install = 1U;
        return Jobs_PublishTerminalStatus("IN_PROGRESS",
                                          "REBOOTING",
                                          "candidate_verified");
    }

    if (OtaManager_GetPendingJob(&ota_job) != 0)
    {
        const char *phase =
            ota_job.succeeded ? "HEALTHY"
                              : (ota_job.rejected ? "REJECTED"
                                                  : "ROLLED_BACK");
        const char *detail =
            ota_job.succeeded ? "image_confirmed"
                              : (ota_job.rejected ? "candidate_rejected"
                                                  : "trial_health_failed");

        (void)snprintf(jobs_context.active_job_id,
                       sizeof(jobs_context.active_job_id),
                       "%s",
                       ota_job.job_id);
        jobs_context.execution_version = ota_job.execution_version;
        jobs_context.state = AWS_JOBS_STATE_EXECUTION_ACTIVE;
        jobs_context.pending_ota_report = 1U;
        return Jobs_PublishTerminalStatus(
            ota_job.succeeded ? "SUCCEEDED" : "FAILED",
            phase,
            detail);
    }
    return Jobs_RequestNext();
}

void AwsJobs_OnMqttDisconnected(void)
{
    jobs_context.state = AWS_JOBS_STATE_IDLE;
    jobs_context.retry_attempt = 0U;
    jobs_context.retry_after_ms = 0U;
    jobs_context.response_deadline_ms = 0U;
    jobs_context.pending_install = 0U;
    jobs_context.pending_ota_report = 0U;
    Jobs_ClearActiveExecution();
}

int AwsJobs_Process(void)
{
    if (((jobs_context.state == AWS_JOBS_STATE_START_NEXT_PENDING) ||
         (jobs_context.state == AWS_JOBS_STATE_UPDATE_PENDING)) &&
        ((int32_t)(HAL_GetTick() - jobs_context.response_deadline_ms) >= 0))
    {
        Jobs_ScheduleRetry("response_timeout");
    }

    if ((jobs_context.state == AWS_JOBS_STATE_PAUSED) &&
        ((int32_t)(HAL_GetTick() - jobs_context.retry_after_ms) >= 0))
    {
        if (jobs_context.pending_install != 0U)
        {
            jobs_context.state = AWS_JOBS_STATE_EXECUTION_ACTIVE;
            return Jobs_PublishTerminalStatus("IN_PROGRESS",
                                              "REBOOTING",
                                              "candidate_verified");
        }
        if (jobs_context.pending_ota_report != 0U)
        {
            OtaPendingJob ota_job;
            if (OtaManager_GetPendingJob(&ota_job) != 0)
            {
                const char *phase =
                    ota_job.succeeded
                        ? "HEALTHY"
                        : (ota_job.rejected ? "REJECTED"
                                            : "ROLLED_BACK");
                const char *detail =
                    ota_job.succeeded
                        ? "image_confirmed"
                        : (ota_job.rejected ? "candidate_rejected"
                                            : "trial_health_failed");

                jobs_context.state = AWS_JOBS_STATE_EXECUTION_ACTIVE;
                return Jobs_PublishTerminalStatus(
                    ota_job.succeeded ? "SUCCEEDED" : "FAILED",
                    phase,
                    detail);
            }
        }
        jobs_context.state = AWS_JOBS_STATE_IDLE;
        Jobs_ClearActiveExecution();
        return Jobs_RequestNext();
    }

    return 0;
}

static int Jobs_IsSafeDetail(const char *detail)
{
    size_t index;

    if ((detail == NULL) || (detail[0] == '\0') || (strlen(detail) > 40U))
    {
        return 0;
    }

    for (index = 0U; detail[index] != '\0'; index++)
    {
        char value = detail[index];
        if (!(((value >= 'a') && (value <= 'z')) ||
              ((value >= '0') && (value <= '9')) ||
              (value == '_')))
        {
            return 0;
        }
    }
    return 1;
}

int AwsJobs_OnDownloadComplete(int succeeded, const char *detail)
{
    const FlashStagingInfo *staging = FlashStaging_GetInfo();
    const char *safe_detail = Jobs_IsSafeDetail(detail)
                                  ? detail
                                  : "download_failed";

    if ((jobs_context.state != AWS_JOBS_STATE_EXECUTION_ACTIVE) ||
        (jobs_context.active_job_id[0] == '\0'))
    {
        Jobs_Log("JOBS: ignored download completion without active execution");
        return -1;
    }

    Jobs_Logf("JOBS: download completed job=%s result=%s detail=%s",
              jobs_context.active_job_id,
              succeeded ? "success" : "failure",
              safe_detail);
    if (succeeded &&
        (staging->artifact_type == FLASH_STAGING_ARTIFACT_FIRMWARE))
    {
        jobs_context.pending_install = 1U;
        return Jobs_PublishTerminalStatus("IN_PROGRESS",
                                          "REBOOTING",
                                          "candidate_verified");
    }

    return Jobs_PublishTerminalStatus(succeeded ? "SUCCEEDED" : "FAILED",
                                      succeeded ? "STAGED" : "DOWNLOAD_FAILED",
                                      succeeded ? "artifact_staged" : safe_detail);
}

int AwsJobs_CancelActive(const char *job_id)
{
    if ((job_id == NULL) || !Jobs_IsValidJobId(job_id))
    {
        return -1;
    }
    if ((jobs_context.active_job_id[0] == '\0') ||
        (strcmp(jobs_context.active_job_id, job_id) != 0))
    {
        return 0;
    }

    Jobs_Logf("JOBS: local execution cancelled job=%s", job_id);
    jobs_context.state = AWS_JOBS_STATE_IDLE;
    jobs_context.retry_attempt = 0U;
    jobs_context.retry_after_ms = 0U;
    jobs_context.response_deadline_ms = 0U;
    jobs_context.pending_install = 0U;
    jobs_context.pending_ota_report = 0U;
    Jobs_ClearActiveExecution();
    return 1;
}

int AwsJobs_HandleMessage(const char *topic, const char *payload)
{
    if ((topic == NULL) || (payload == NULL))
    {
        return 0;
    }

    if (strcmp(topic, jobs_context.notify_next_topic) == 0)
    {
        Jobs_Log("JOBS: notify-next received");
        if (jobs_context.state == AWS_JOBS_STATE_IDLE)
        {
            return Jobs_RequestNext() == 0 ? 1 : -1;
        }
        return 1;
    }

    if (strcmp(topic, jobs_context.start_next_accepted_topic) == 0)
    {
        return Jobs_HandleStartNextAccepted(payload);
    }

    if (strcmp(topic, jobs_context.start_next_rejected_topic) == 0)
    {
        Jobs_ClearActiveExecution();
        Jobs_ScheduleRetry("start_next_rejected");
        Jobs_Log("JOBS: start-next rejected by AWS");
        return 1;
    }

    if (Jobs_TopicHasUpdateSuffix(topic, "/update/accepted"))
    {
        if ((jobs_context.state == AWS_JOBS_STATE_UPDATE_PENDING) &&
            Jobs_TopicMatchesActiveUpdate(topic, "/update/accepted"))
        {
            Jobs_Logf("JOBS: status=%s accepted job=%s",
                      jobs_context.pending_status,
                      jobs_context.active_job_id);
            if (jobs_context.pending_install != 0U)
            {
                uint32_t accepted_execution_version;

                if ((Jobs_GetAcceptedExecutionVersion(
                         payload,
                         &accepted_execution_version) != 0) ||
                    (accepted_execution_version <=
                     jobs_context.execution_version) ||
                    (OtaManager_UpdateJobExecutionVersion(
                         accepted_execution_version) != 0))
                {
                    Jobs_Log("JOBS: accepted execution version persist failed");
                    Jobs_ScheduleRetry("execution_version_persist_failed");
                    return 1;
                }

                jobs_context.execution_version =
                    accepted_execution_version;
                jobs_context.pending_install = 0U;
                jobs_context.state = AWS_JOBS_STATE_IDLE;
                jobs_context.response_deadline_ms = 0U;
                return OtaManager_RequestInstall() == 0 ? 1 : -1;
            }
            if (jobs_context.pending_ota_report != 0U)
            {
                if (OtaManager_MarkJobReported() != 0)
                {
                    Jobs_Log("JOBS: OTA completion marker clear failed");
                    return -1;
                }
                jobs_context.pending_ota_report = 0U;
            }
            jobs_context.state = AWS_JOBS_STATE_IDLE;
            jobs_context.retry_attempt = 0U;
            jobs_context.response_deadline_ms = 0U;
            Jobs_ClearActiveExecution();
            return Jobs_RequestNext() == 0 ? 1 : -1;
        }

        Jobs_Log("JOBS: ignored unexpected update/accepted");
        return 1;
    }

    if (Jobs_TopicHasUpdateSuffix(topic, "/update/rejected"))
    {
        if (Jobs_TopicMatchesActiveUpdate(topic, "/update/rejected"))
        {
            char error_code[40];
            char execution_status[16];

            if ((jobs_context.pending_ota_report != 0U) &&
                Jobs_DurableReportIsObsolete(payload,
                                             error_code,
                                             sizeof(error_code),
                                             execution_status,
                                             sizeof(execution_status)))
            {
                Jobs_Logf("JOBS: obsolete completion rejected job=%s code=%s",
                          jobs_context.active_job_id,
                          error_code);
                if (OtaManager_MarkJobReported() != 0)
                {
                    Jobs_Log("JOBS: obsolete completion marker clear failed");
                    Jobs_ScheduleRetry("completion_marker_clear_failed");
                    return 1;
                }

                jobs_context.pending_ota_report = 0U;
                jobs_context.state = AWS_JOBS_STATE_IDLE;
                jobs_context.retry_attempt = 0U;
                jobs_context.retry_after_ms = 0U;
                jobs_context.response_deadline_ms = 0U;
                Jobs_ClearActiveExecution();
                Jobs_Log("JOBS: obsolete completion cleared, requesting next");
                return Jobs_RequestNext() == 0 ? 1 : -1;
            }

            Jobs_Logf("JOBS: status update rejected job=%s code=%s state=%s",
                      jobs_context.active_job_id,
                      error_code,
                      execution_status[0] != '\0' ? execution_status : "unknown");
            Jobs_ScheduleRetry("status_update_rejected");
        }
        else
        {
            Jobs_Log("JOBS: ignored unexpected update/rejected");
        }
        return 1;
    }

    return 0;
}
