#include "device_identity.h"

#include "stm32h7xx_hal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
    char hardware_uid[DEVICE_HARDWARE_UID_HEX_LENGTH + 1U];
    char thing_name[DEVICE_IDENTITY_THING_NAME_MAX_LENGTH + 1U];
    char client_id[DEVICE_IDENTITY_CLIENT_ID_MAX_LENGTH + 1U];
    uint8_t initialized;
} DeviceIdentityContext;

static DeviceIdentityContext identity_context;

static int DeviceIdentity_IsValidThingName(const char *thing_name)
{
    size_t index;
    size_t length;

    if (thing_name == NULL)
    {
        return 0;
    }

    length = strnlen(thing_name, DEVICE_IDENTITY_THING_NAME_MAX_LENGTH + 1U);
    if ((length == 0U) || (length > DEVICE_IDENTITY_THING_NAME_MAX_LENGTH))
    {
        return 0;
    }

    for (index = 0U; index < length; index++)
    {
        unsigned char character = (unsigned char)thing_name[index];
        if (!isalnum(character) &&
            (character != '-') &&
            (character != '_') &&
            (character != ':'))
        {
            return 0;
        }
    }

    return 1;
}

int DeviceIdentity_Refresh(void)
{
    const CredentialStoreData *credentials = CredentialStore_Get();
    const char *thing_name = NULL;
    int written;

    memset(&identity_context, 0, sizeof(identity_context));
    DeviceConfig_GetHardwareUid(identity_context.hardware_uid,
                                sizeof(identity_context.hardware_uid));

    if (CredentialStore_GetState() == CREDENTIAL_STORE_PROVISIONED)
    {
        if ((credentials == NULL) ||
            !DeviceIdentity_IsValidThingName(credentials->thing_name))
        {
            return -1;
        }
        thing_name = credentials->thing_name;
    }
    else
    {
        written = snprintf(identity_context.thing_name,
                           sizeof(identity_context.thing_name),
                           "%s%s",
                           DEVICE_IDENTITY_THING_PREFIX,
                           identity_context.hardware_uid);
        if ((written < 0) ||
            ((size_t)written >= sizeof(identity_context.thing_name)))
        {
            return -1;
        }
        thing_name = identity_context.thing_name;
    }

    if (thing_name != identity_context.thing_name)
    {
        written = snprintf(identity_context.thing_name,
                           sizeof(identity_context.thing_name),
                           "%s",
                           thing_name);
        if ((written < 0) ||
            ((size_t)written >= sizeof(identity_context.thing_name)))
        {
            return -1;
        }
    }

    written = snprintf(identity_context.client_id,
                       sizeof(identity_context.client_id),
                       "%s",
                       identity_context.thing_name);
    if ((written < 0) ||
        ((size_t)written >= sizeof(identity_context.client_id)))
    {
        return -1;
    }

    identity_context.initialized = 1U;
    return 0;
}

const char *DeviceIdentity_GetThingName(void)
{
    return identity_context.initialized ? identity_context.thing_name : "";
}

const char *DeviceIdentity_GetClientId(void)
{
    return identity_context.initialized ? identity_context.client_id : "";
}

const char *DeviceIdentity_GetHardwareUid(void)
{
    return identity_context.initialized ? identity_context.hardware_uid : "";
}

void DeviceIdentity_GetMacAddress(uint8_t output[6])
{
    uint32_t uid_words[3];
    uint64_t hash = 1469598103934665603ULL;
    size_t byte_index;
    size_t word_index;

    if (output == NULL)
    {
        return;
    }

    uid_words[0] = HAL_GetUIDw0();
    uid_words[1] = HAL_GetUIDw1();
    uid_words[2] = HAL_GetUIDw2();

    for (word_index = 0U; word_index < 3U; word_index++)
    {
        for (byte_index = 0U; byte_index < sizeof(uint32_t); byte_index++)
        {
            uint8_t byte =
                (uint8_t)((uid_words[word_index] >> (byte_index * 8U)) & 0xFFU);
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
    }

    /* 0x02 marks a locally administered, unicast Ethernet address. */
    output[0] = 0x02U;
    output[1] = (uint8_t)(hash >> 0U);
    output[2] = (uint8_t)(hash >> 8U);
    output[3] = (uint8_t)(hash >> 16U);
    output[4] = (uint8_t)(hash >> 24U);
    output[5] = (uint8_t)(hash >> 32U);
}
