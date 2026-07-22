#include "fleet_provisioning.h"

#include "credential_store.h"
#include "debug_logger.h"
#include "device_config.h"
#include "main.h"

#include "mbedtls/pk.h"
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLEET_TOPIC_MAX_LENGTH             192U
#define FLEET_TEMPLATE_NAME_MAX_LENGTH      64U
#define FLEET_THING_NAME_MAX_LENGTH        128U
#define FLEET_HARDWARE_UID_MAX_LENGTH       24U
#define FLEET_REGISTER_PAYLOAD_MAX_LENGTH  2560U
#define FLEET_ERROR_TEXT_MAX_LENGTH         160U
#define FLEET_MAX_ATTEMPTS                    5U
#define FLEET_RETRY_BASE_MS                 2000U
#define FLEET_RETRY_MAX_MS                 60000U
#define FLEET_RESPONSE_TIMEOUT_MS           30000U

#define FLEET_CREATE_REQUEST_TOPIC  "$aws/certificates/create/json"
#define FLEET_CREATE_ACCEPTED_TOPIC "$aws/certificates/create/json/accepted"
#define FLEET_CREATE_REJECTED_TOPIC "$aws/certificates/create/json/rejected"

typedef enum
{
    FLEET_STATE_IDLE = 0,
    FLEET_STATE_REQUEST_CERTIFICATE,
    FLEET_STATE_WAIT_CERTIFICATE,
    FLEET_STATE_REQUEST_REGISTRATION,
    FLEET_STATE_WAIT_REGISTRATION,
    FLEET_STATE_COMPLETE,
    FLEET_STATE_FAILED
} FleetRuntimeState;

static FleetProvisioningPublishCallback fleet_publish;
static FleetRuntimeState fleet_state;
static uint8_t fleet_connected;
static uint8_t fleet_attempts;
static uint8_t fleet_reconnect_required;
static uint32_t fleet_retry_after_ms;
static uint32_t fleet_response_deadline_ms;
static char fleet_template_name[FLEET_TEMPLATE_NAME_MAX_LENGTH + 1U];
static char fleet_thing_name[FLEET_THING_NAME_MAX_LENGTH + 1U];
static char fleet_hardware_uid[FLEET_HARDWARE_UID_MAX_LENGTH + 1U];
static char fleet_register_request_topic[FLEET_TOPIC_MAX_LENGTH];
static char fleet_register_accepted_topic[FLEET_TOPIC_MAX_LENGTH];
static char fleet_register_rejected_topic[FLEET_TOPIC_MAX_LENGTH];

static void Fleet_Log(const char *text)
{
    Debug_LogLine(DEBUG_SRC_TLS, text);
}

static void Fleet_Logf(const char *format, ...)
{
    char buffer[256];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Debug_LogLine(DEBUG_SRC_TLS, buffer);
}

static const char *Fleet_JsonValueStart(const char *json, const char *key)
{
    char pattern[96];
    const char *value;
    int written;

    written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if ((written < 0) || ((size_t)written >= sizeof(pattern)))
    {
        return NULL;
    }

    value = strstr(json, pattern);
    if (value == NULL)
    {
        return NULL;
    }
    value += strlen(pattern);
    while (isspace((unsigned char)*value))
    {
        value++;
    }
    if (*value++ != ':')
    {
        return NULL;
    }
    while (isspace((unsigned char)*value))
    {
        value++;
    }
    return value;
}

static int Fleet_JsonGetString(const char *json,
                               const char *key,
                               char *output,
                               size_t output_size)
{
    const char *input = Fleet_JsonValueStart(json, key);
    size_t used = 0U;

    if ((input == NULL) || (*input != '"') || (output_size == 0U))
    {
        return -1;
    }
    input++;

    while ((*input != '\0') && (*input != '"'))
    {
        char decoded = *input++;
        if (decoded == '\\')
        {
            char escaped = *input++;
            switch (escaped)
            {
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                default: return -1;
            }
        }

        if ((used + 1U) >= output_size)
        {
            return -1;
        }
        output[used++] = decoded;
    }

    if (*input != '"')
    {
        return -1;
    }
    output[used] = '\0';
    return 0;
}

static int Fleet_JsonGetUint32(const char *json, const char *key, uint32_t *output)
{
    const char *value = Fleet_JsonValueStart(json, key);
    char *end;
    unsigned long parsed;

    if ((value == NULL) || (output == NULL))
    {
        return -1;
    }
    parsed = strtoul(value, &end, 10);
    if ((end == value) || (parsed > 0xFFFFFFFFUL))
    {
        return -1;
    }
    *output = (uint32_t)parsed;
    return 0;
}

static int Fleet_ValidateCertificatePair(const char *certificate_pem,
                                         const char *private_key_pem)
{
    mbedtls_x509_crt certificate;
    mbedtls_pk_context private_key;
    int result;

    mbedtls_x509_crt_init(&certificate);
    mbedtls_pk_init(&private_key);
    result = mbedtls_x509_crt_parse(
        &certificate,
        (const unsigned char *)certificate_pem,
        strlen(certificate_pem) + 1U);
    if (result == 0)
    {
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
        result = -1;
#else
        result = mbedtls_pk_parse_key(
            &private_key,
            (const unsigned char *)private_key_pem,
            strlen(private_key_pem) + 1U,
            NULL,
            0U);
#endif
    }
    if (result == 0)
    {
        result = mbedtls_pk_check_pair(&certificate.pk, &private_key);
    }
    mbedtls_pk_free(&private_key);
    mbedtls_x509_crt_free(&certificate);
    return result == 0 ? 0 : -1;
}

static uint32_t Fleet_RetryDelayMs(uint8_t attempts)
{
    uint32_t delay = FLEET_RETRY_BASE_MS << (attempts > 5U ? 5U : attempts);
    return delay > FLEET_RETRY_MAX_MS ? FLEET_RETRY_MAX_MS : delay;
}

static void Fleet_ScheduleRetry(FleetRuntimeState request_state, const char *reason)
{
    fleet_response_deadline_ms = 0U;
    fleet_attempts++;
    if (fleet_attempts >= FLEET_MAX_ATTEMPTS)
    {
        fleet_state = FLEET_STATE_FAILED;
        Fleet_Logf("FLEET: failed after %u attempts reason=%s",
                   (unsigned int)fleet_attempts,
                   reason);
        return;
    }

    fleet_state = request_state;
    fleet_retry_after_ms = HAL_GetTick() + Fleet_RetryDelayMs(fleet_attempts);
    Fleet_Logf("FLEET: retry scheduled attempt=%u reason=%s",
               (unsigned int)(fleet_attempts + 1U),
               reason);
}

static int Fleet_PublishCreateRequest(void)
{
    if (fleet_publish(FLEET_CREATE_REQUEST_TOPIC, "") != 0)
    {
        Fleet_ScheduleRetry(FLEET_STATE_REQUEST_CERTIFICATE, "create_publish_failed");
        return 0;
    }

    fleet_state = FLEET_STATE_WAIT_CERTIFICATE;
    fleet_response_deadline_ms = HAL_GetTick() + FLEET_RESPONSE_TIMEOUT_MS;
    Fleet_Log("FLEET: CreateKeysAndCertificate requested");
    return 0;
}

static int Fleet_PublishRegisterRequest(void)
{
    const CredentialStoreData *credentials = CredentialStore_Get();
    char payload[FLEET_REGISTER_PAYLOAD_MAX_LENGTH];
    int written;

    if (CredentialStore_GetState() != CREDENTIAL_STORE_PENDING_REGISTRATION)
    {
        return -1;
    }

    written = snprintf(
        payload,
        sizeof(payload),
        "{\"certificateOwnershipToken\":\"%s\","
        "\"parameters\":{\"ThingName\":\"%s\","
        "\"SerialNumber\":\"%s\",\"HardwareUid\":\"%s\"}}",
        credentials->ownership_token,
        fleet_thing_name,
        fleet_hardware_uid,
        fleet_hardware_uid);
    if ((written < 0) || ((size_t)written >= sizeof(payload)))
    {
        return -1;
    }

    if (fleet_publish(fleet_register_request_topic, payload) != 0)
    {
        Fleet_ScheduleRetry(FLEET_STATE_REQUEST_REGISTRATION,
                            "register_publish_failed");
        return 0;
    }

    fleet_state = FLEET_STATE_WAIT_REGISTRATION;
    fleet_response_deadline_ms = HAL_GetTick() + FLEET_RESPONSE_TIMEOUT_MS;
    Fleet_Log("FLEET: RegisterThing requested");
    return 0;
}

static int Fleet_HandleCreateAccepted(const char *payload)
{
    CredentialStoreData credentials;

    fleet_response_deadline_ms = 0U;
    memset(&credentials, 0, sizeof(credentials));
    if ((Fleet_JsonGetString(payload,
                             "certificatePem",
                             credentials.certificate_pem,
                             sizeof(credentials.certificate_pem)) != 0) ||
        (Fleet_JsonGetString(payload,
                             "privateKey",
                             credentials.private_key_pem,
                             sizeof(credentials.private_key_pem)) != 0) ||
        (Fleet_JsonGetString(payload,
                             "certificateOwnershipToken",
                             credentials.ownership_token,
                             sizeof(credentials.ownership_token)) != 0) ||
        (Fleet_JsonGetString(payload,
                             "certificateId",
                             credentials.certificate_id,
                             sizeof(credentials.certificate_id)) != 0))
    {
        Fleet_ScheduleRetry(FLEET_STATE_REQUEST_CERTIFICATE,
                            "invalid_create_response");
        return 1;
    }

    if (Fleet_ValidateCertificatePair(credentials.certificate_pem,
                                      credentials.private_key_pem) != 0)
    {
        Fleet_ScheduleRetry(FLEET_STATE_REQUEST_CERTIFICATE,
                            "invalid_certificate_pair");
        return 1;
    }

    if (CredentialStore_SavePending(credentials.certificate_pem,
                                    credentials.private_key_pem,
                                    credentials.ownership_token,
                                    credentials.certificate_id) != 0)
    {
        Fleet_Log("FLEET: credential Flash write failed");
        fleet_state = FLEET_STATE_FAILED;
        return -1;
    }

    memset(&credentials, 0, sizeof(credentials));
    fleet_attempts = 0U;
    fleet_state = FLEET_STATE_REQUEST_REGISTRATION;
    fleet_retry_after_ms = HAL_GetTick();
    Fleet_Log("FLEET: unique certificate verified and staged");
    return 1;
}

static int Fleet_HandleRegisterAccepted(const char *payload)
{
    char thing_name[CREDENTIAL_STORE_THING_NAME_MAX_LENGTH + 1U];

    fleet_response_deadline_ms = 0U;
    if ((Fleet_JsonGetString(payload, "thingName", thing_name, sizeof(thing_name)) != 0) ||
        (strcmp(thing_name, fleet_thing_name) != 0))
    {
        Fleet_Log("FLEET: RegisterThing response identity mismatch");
        fleet_state = FLEET_STATE_FAILED;
        return -1;
    }

    if (CredentialStore_CommitProvisioned(thing_name) != 0)
    {
        Fleet_Log("FLEET: final credential commit failed");
        fleet_state = FLEET_STATE_FAILED;
        return -1;
    }

    if (DeviceConfig_SetProvisioned() != 0)
    {
        Fleet_Log("FLEET: provisioning flag persistence deferred");
    }

    fleet_state = FLEET_STATE_COMPLETE;
    fleet_reconnect_required = 1U;
    Fleet_Logf("FLEET: PROVISIONED thing=%s reconnect=unique_credentials",
               thing_name);
    return 1;
}

static int Fleet_HandleRejected(FleetRuntimeState request_state,
                                const char *payload)
{
    char error_code[FLEET_ERROR_TEXT_MAX_LENGTH] = "unknown";
    char error_message[FLEET_ERROR_TEXT_MAX_LENGTH] = "rejected";
    uint32_t status_code = 0U;

    (void)Fleet_JsonGetString(payload, "errorCode", error_code, sizeof(error_code));
    (void)Fleet_JsonGetString(payload,
                              "errorMessage",
                              error_message,
                              sizeof(error_message));
    (void)Fleet_JsonGetUint32(payload, "statusCode", &status_code);
    Fleet_Logf("FLEET: rejected status=%lu code=%s",
               (unsigned long)status_code,
               error_code);

    if ((request_state == FLEET_STATE_REQUEST_REGISTRATION) &&
        ((strstr(error_message, "expired") != NULL) ||
         (strstr(error_message, "Expired") != NULL)))
    {
        if (CredentialStore_Erase() != 0)
        {
            fleet_state = FLEET_STATE_FAILED;
            return -1;
        }
        request_state = FLEET_STATE_REQUEST_CERTIFICATE;
    }

    Fleet_ScheduleRetry(request_state, error_code);
    return 1;
}

void FleetProvisioning_Init(FleetProvisioningPublishCallback publish_callback,
                            const char *template_name,
                            const char *thing_name,
                            const char *hardware_uid)
{
    fleet_publish = publish_callback;
    fleet_state = FLEET_STATE_IDLE;
    fleet_connected = 0U;
    fleet_attempts = 0U;
    fleet_reconnect_required = 0U;
    fleet_retry_after_ms = 0U;
    fleet_response_deadline_ms = 0U;
    memset(fleet_template_name, 0, sizeof(fleet_template_name));
    memset(fleet_thing_name, 0, sizeof(fleet_thing_name));
    memset(fleet_hardware_uid, 0, sizeof(fleet_hardware_uid));

    if ((publish_callback == NULL) || (template_name == NULL) ||
        (thing_name == NULL) || (hardware_uid == NULL) ||
        (strnlen(template_name, sizeof(fleet_template_name)) >= sizeof(fleet_template_name)) ||
        (strnlen(thing_name, sizeof(fleet_thing_name)) >= sizeof(fleet_thing_name)) ||
        (strlen(hardware_uid) != FLEET_HARDWARE_UID_MAX_LENGTH))
    {
        fleet_state = FLEET_STATE_FAILED;
        return;
    }

    strcpy(fleet_template_name, template_name);
    strcpy(fleet_thing_name, thing_name);
    strcpy(fleet_hardware_uid, hardware_uid);
    (void)snprintf(fleet_register_request_topic,
                   sizeof(fleet_register_request_topic),
                   "$aws/provisioning-templates/%s/provision/json",
                   fleet_template_name);
    (void)snprintf(fleet_register_accepted_topic,
                   sizeof(fleet_register_accepted_topic),
                   "$aws/provisioning-templates/%s/provision/json/accepted",
                   fleet_template_name);
    (void)snprintf(fleet_register_rejected_topic,
                   sizeof(fleet_register_rejected_topic),
                   "$aws/provisioning-templates/%s/provision/json/rejected",
                   fleet_template_name);
}

int FleetProvisioning_IsRequired(void)
{
    return CredentialStore_GetState() != CREDENTIAL_STORE_PROVISIONED;
}

size_t FleetProvisioning_SubscriptionCount(void)
{
    return FleetProvisioning_IsRequired() ? 4U : 0U;
}

const char *FleetProvisioning_SubscriptionTopic(size_t index)
{
    switch (index)
    {
        case 0U: return FLEET_CREATE_ACCEPTED_TOPIC;
        case 1U: return FLEET_CREATE_REJECTED_TOPIC;
        case 2U: return fleet_register_accepted_topic;
        case 3U: return fleet_register_rejected_topic;
        default: return NULL;
    }
}

int FleetProvisioning_OnMqttConnected(void)
{
    if (fleet_state == FLEET_STATE_FAILED)
    {
        return -1;
    }

    fleet_connected = 1U;
    fleet_retry_after_ms = HAL_GetTick();
    fleet_response_deadline_ms = 0U;
    fleet_state = CredentialStore_GetState() == CREDENTIAL_STORE_PENDING_REGISTRATION
                      ? FLEET_STATE_REQUEST_REGISTRATION
                      : FLEET_STATE_REQUEST_CERTIFICATE;
    return FleetProvisioning_Process(HAL_GetTick());
}

void FleetProvisioning_OnMqttDisconnected(void)
{
    fleet_connected = 0U;
    if ((CredentialStore_GetState() == CREDENTIAL_STORE_PROVISIONED) &&
        fleet_reconnect_required)
    {
        fleet_reconnect_required = 0U;
    }
}

int FleetProvisioning_HandleMessage(const char *topic, const char *payload)
{
    if ((topic == NULL) || (payload == NULL) || !FleetProvisioning_IsRequired())
    {
        return 0;
    }
    if (strcmp(topic, FLEET_CREATE_ACCEPTED_TOPIC) == 0)
    {
        return Fleet_HandleCreateAccepted(payload);
    }
    if (strcmp(topic, FLEET_CREATE_REJECTED_TOPIC) == 0)
    {
        return Fleet_HandleRejected(FLEET_STATE_REQUEST_CERTIFICATE, payload);
    }
    if (strcmp(topic, fleet_register_accepted_topic) == 0)
    {
        return Fleet_HandleRegisterAccepted(payload);
    }
    if (strcmp(topic, fleet_register_rejected_topic) == 0)
    {
        return Fleet_HandleRejected(FLEET_STATE_REQUEST_REGISTRATION, payload);
    }
    return 0;
}

int FleetProvisioning_Process(uint32_t now_ms)
{
    if (!fleet_connected)
    {
        return 0;
    }
    if ((fleet_state == FLEET_STATE_WAIT_CERTIFICATE) &&
        (fleet_response_deadline_ms != 0U) &&
        ((int32_t)(now_ms - fleet_response_deadline_ms) >= 0))
    {
        Fleet_ScheduleRetry(FLEET_STATE_REQUEST_CERTIFICATE,
                            "create_response_timeout");
    }
    else if ((fleet_state == FLEET_STATE_WAIT_REGISTRATION) &&
             (fleet_response_deadline_ms != 0U) &&
             ((int32_t)(now_ms - fleet_response_deadline_ms) >= 0))
    {
        Fleet_ScheduleRetry(FLEET_STATE_REQUEST_REGISTRATION,
                            "register_response_timeout");
    }

    if ((int32_t)(now_ms - fleet_retry_after_ms) < 0)
    {
        return 0;
    }
    if (fleet_state == FLEET_STATE_REQUEST_CERTIFICATE)
    {
        return Fleet_PublishCreateRequest();
    }
    if (fleet_state == FLEET_STATE_REQUEST_REGISTRATION)
    {
        return Fleet_PublishRegisterRequest();
    }
    return fleet_state == FLEET_STATE_FAILED ? -1 : 0;
}

int FleetProvisioning_ReconnectRequired(void)
{
    return fleet_reconnect_required != 0U;
}
