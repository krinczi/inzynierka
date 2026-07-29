#include "firmware_authenticity.h"
#include "main.h"

#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"

#include <string.h>

FirmwareAuthenticityResult FirmwareAuthenticity_VerifyDigest(
    const uint8_t digest[32],
    const uint8_t signature[64])
{
    mbedtls_ecp_group group;
    mbedtls_ecp_point public_point;
    mbedtls_mpi r;
    mbedtls_mpi s;
    const uint8_t *public_key;
    size_t public_key_length = 0U;
    int result;

    if ((digest == NULL) || (signature == NULL))
    {
        return FIRMWARE_AUTH_INVALID_ARGUMENT;
    }

    public_key = FirmwareTrustAnchor_GetPublicKey(&public_key_length);
    if ((public_key == NULL) || (public_key_length != 65U) ||
        (public_key[0] != 0x04U))
    {
        return FIRMWARE_AUTH_TRUST_ANCHOR_MISSING;
    }

    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&public_point);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0)
    {
        result = mbedtls_ecp_point_read_binary(&group,
                                               &public_point,
                                               public_key,
                                               public_key_length);
    }
    if (result == 0)
    {
        result = mbedtls_ecp_check_pubkey(&group, &public_point);
    }
    if (result == 0)
    {
        result = mbedtls_mpi_read_binary(&r, signature, 32U);
    }
    if (result == 0)
    {
        result = mbedtls_mpi_read_binary(&s, signature + 32U, 32U);
    }
    if (result == 0)
    {
        result = mbedtls_ecdsa_verify(&group,
                                      digest,
                                      32U,
                                      &public_point,
                                      &r,
                                      &s);
    }
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&public_point);
    mbedtls_ecp_group_free(&group);

    return (result == 0) ? FIRMWARE_AUTH_OK
                         : FIRMWARE_AUTH_SIGNATURE_INVALID;
}

FirmwareAuthenticityResult FirmwareAuthenticity_DecodeAndVerifyDigest(
    const uint8_t digest[32],
    const char *signature_base64,
    uint8_t decoded_signature[64])
{
    uint8_t signature[64];
    size_t signature_length = 0U;
    FirmwareAuthenticityResult result;

    if ((digest == NULL) || (signature_base64 == NULL) ||
        (signature_base64[0] == '\0'))
    {
        return FIRMWARE_AUTH_INVALID_ARGUMENT;
    }

    if ((mbedtls_base64_decode(signature,
                               sizeof(signature),
                               &signature_length,
                               (const unsigned char *)signature_base64,
                               strlen(signature_base64)) != 0) ||
        (signature_length != sizeof(signature)))
    {
        memset(signature, 0, sizeof(signature));
        return FIRMWARE_AUTH_SIGNATURE_ENCODING_INVALID;
    }

    result = FirmwareAuthenticity_VerifyDigest(digest, signature);
    if ((result == FIRMWARE_AUTH_OK) && (decoded_signature != NULL))
    {
        memcpy(decoded_signature, signature, sizeof(signature));
    }
    memset(signature, 0, sizeof(signature));
    return result;
}

__weak const uint8_t *FirmwareTrustAnchor_GetPublicKey(size_t *length)
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
