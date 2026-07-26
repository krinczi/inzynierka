#include "firmware_authenticity.h"
#include "main.h"

#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"

#include <string.h>

#define FIRMWARE_SIGNATURE_MAX_BYTES 512U

FirmwareAuthenticityResult FirmwareAuthenticity_VerifySha256(
    const uint8_t sha256[32],
    const char *signature_base64)
{
    mbedtls_pk_context public_key;
    const unsigned char *public_key_pem;
    size_t public_key_length = 0U;
    unsigned char signature[FIRMWARE_SIGNATURE_MAX_BYTES];
    size_t signature_length = 0U;
    size_t encoded_length;
    int result;

    if ((sha256 == NULL) || (signature_base64 == NULL) ||
        (signature_base64[0] == '\0'))
    {
        return FIRMWARE_AUTH_INVALID_ARGUMENT;
    }

    public_key_pem =
        FirmwareTrustAnchor_GetPublicKeyPem(&public_key_length);
    if ((public_key_pem == NULL) || (public_key_length == 0U))
    {
        return FIRMWARE_AUTH_TRUST_ANCHOR_MISSING;
    }

    encoded_length = strlen(signature_base64);
    if ((encoded_length == 0U) ||
        (mbedtls_base64_decode(signature,
                               sizeof(signature),
                               &signature_length,
                               (const unsigned char *)signature_base64,
                               encoded_length) != 0))
    {
        memset(signature, 0, sizeof(signature));
        return FIRMWARE_AUTH_SIGNATURE_ENCODING_INVALID;
    }

    mbedtls_pk_init(&public_key);
    result = mbedtls_pk_parse_public_key(&public_key,
                                         public_key_pem,
                                         public_key_length);
    if (result == 0)
    {
        result = mbedtls_pk_verify(&public_key,
                                   MBEDTLS_MD_SHA256,
                                   sha256,
                                   32U,
                                   signature,
                                   signature_length);
    }
    mbedtls_pk_free(&public_key);
    memset(signature, 0, sizeof(signature));

    return (result == 0) ? FIRMWARE_AUTH_OK
                         : FIRMWARE_AUTH_SIGNATURE_INVALID;
}

__weak const unsigned char *FirmwareTrustAnchor_GetPublicKeyPem(
    size_t *length)
{
    if (length != NULL)
    {
        *length = 0U;
    }
    return NULL;
}

const char *FirmwareAuthenticity_ResultName(
    FirmwareAuthenticityResult result)
{
    switch (result)
    {
        case FIRMWARE_AUTH_OK:
            return "OK";
        case FIRMWARE_AUTH_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case FIRMWARE_AUTH_TRUST_ANCHOR_MISSING:
            return "TRUST_ANCHOR_MISSING";
        case FIRMWARE_AUTH_SIGNATURE_ENCODING_INVALID:
            return "SIGNATURE_ENCODING_INVALID";
        case FIRMWARE_AUTH_SIGNATURE_INVALID:
        default:
            return "SIGNATURE_INVALID";
    }
}
