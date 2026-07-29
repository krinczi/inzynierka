#include "ota_image.h"

#include "firmware_authenticity.h"
#include "main.h"

#include "mbedtls/sha256.h"

#include <stddef.h>
#include <string.h>

static void OtaImage_InvalidateCache(uint32_t address, size_t length)
{
    uintptr_t start;
    uintptr_t end;

    if ((length == 0U) || ((SCB->CCR & SCB_CCR_DC_Msk) == 0U))
    {
        return;
    }
    start = (uintptr_t)address & ~(uintptr_t)(OTA_FLASHWORD_SIZE - 1U);
    end = ((uintptr_t)address + length + OTA_FLASHWORD_SIZE - 1U) &
          ~(uintptr_t)(OTA_FLASHWORD_SIZE - 1U);
    SCB_InvalidateDCache_by_Addr((uint32_t *)start,
                                (int32_t)(end - start));
    __DSB();
    __ISB();
}

uint32_t OtaImage_HeaderCrc32(const OtaImageHeader *header)
{
    if (header == NULL)
    {
        return 0U;
    }
    return OtaControl_Crc32((const uint8_t *)header,
                            offsetof(OtaImageHeader, header_crc32));
}

int OtaImage_HeaderIsValid(const OtaImageHeader *header)
{
    uint32_t index;
    uint8_t digest_nonzero = 0U;
    uint8_t signature_nonzero = 0U;

    if ((header == NULL) ||
        (header->magic != OTA_IMAGE_HEADER_MAGIC) ||
        (header->format != OTA_IMAGE_HEADER_FORMAT) ||
        (header->header_size != OTA_IMAGE_HEADER_SIZE) ||
        (header->target_id != OTA_IMAGE_TARGET_STM32H753) ||
        (header->image_version == 0U) ||
        (header->image_size < 8U) ||
        (header->image_size > OTA_IMAGE_MAX_PAYLOAD_SIZE) ||
        (header->payload_offset != OTA_IMAGE_HEADER_SIZE) ||
        (header->primary_vector_address != OTA_PRIMARY_IMAGE_ADDRESS) ||
        (header->header_crc32 != OtaImage_HeaderCrc32(header)))
    {
        return 0;
    }

    for (index = 0U; index < OTA_SHA256_SIZE; index++)
    {
        digest_nonzero |= header->payload_sha256[index];
    }
    for (index = 0U; index < OTA_P256_SIGNATURE_SIZE; index++)
    {
        signature_nonzero |= header->signature[index];
    }
    return (digest_nonzero != 0U) && (signature_nonzero != 0U);
}

int OtaImage_VectorIsValid(uint32_t slot_address,
                           const OtaImageHeader *header)
{
    const uint32_t *vectors;
    uint32_t stack;
    uint32_t reset;
    uint32_t vector_address;
    int stack_valid;

    if (!OtaImage_HeaderIsValid(header))
    {
        return 0;
    }

    vector_address = slot_address + header->payload_offset;
    OtaImage_InvalidateCache(vector_address, 8U);
    vectors = (const uint32_t *)(uintptr_t)vector_address;
    stack = vectors[0];
    reset = vectors[1];

    stack_valid =
        ((stack > 0x20000000UL) && (stack <= 0x20020000UL)) ||
        ((stack > 0x24000000UL) && (stack <= 0x24080000UL)) ||
        ((stack > 0x30000000UL) && (stack <= 0x30048000UL)) ||
        ((stack > 0x38000000UL) && (stack <= 0x38010000UL));

    return stack_valid &&
           ((stack & 0x7U) == 0U) &&
           ((reset & 1U) != 0U) &&
           ((reset & ~1UL) >= OTA_PRIMARY_IMAGE_ADDRESS) &&
           ((reset & ~1UL) <
            (OTA_PRIMARY_IMAGE_ADDRESS + header->image_size));
}

static void OtaImage_StoreBe32(uint8_t output[4], uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

OtaImageResult OtaImage_BuildSignedDigest(
    const OtaImageHeader *header,
    uint8_t digest[OTA_SHA256_SIZE])
{
    mbedtls_sha256_context context;
    uint8_t encoded[12];
    int result;

    if ((header == NULL) || (digest == NULL) ||
        !OtaImage_HeaderIsValid(header))
    {
        return OTA_IMAGE_INVALID_ARGUMENT;
    }

    OtaImage_StoreBe32(encoded, header->target_id);
    OtaImage_StoreBe32(encoded + 4U, header->image_version);
    OtaImage_StoreBe32(encoded + 8U, header->image_size);

    mbedtls_sha256_init(&context);
    result = mbedtls_sha256_starts_ret(&context, 0);
    if (result == 0)
    {
        result = mbedtls_sha256_update_ret(
            &context,
            (const uint8_t *)OTA_IMAGE_SIGNING_CONTEXT,
            OTA_IMAGE_SIGNING_CONTEXT_SIZE);
    }
    if (result == 0)
    {
        result = mbedtls_sha256_update_ret(&context,
                                           encoded,
                                           sizeof(encoded));
    }
    if (result == 0)
    {
        result = mbedtls_sha256_update_ret(&context,
                                           header->payload_sha256,
                                           OTA_SHA256_SIZE);
    }
    if (result == 0)
    {
        result = mbedtls_sha256_finish_ret(&context, digest);
    }
    mbedtls_sha256_free(&context);
    memset(encoded, 0, sizeof(encoded));
    return result == 0 ? OTA_IMAGE_OK : OTA_IMAGE_HASH_MISMATCH;
}

OtaImageResult OtaImage_VerifySlot(uint32_t slot_address,
                                   OtaImageDescriptor *descriptor)
{
    OtaImageHeader header;
    mbedtls_sha256_context context;
    uint8_t calculated[OTA_SHA256_SIZE];
    uint8_t signed_digest[OTA_SHA256_SIZE];
    const uint8_t *payload;
    uint32_t offset = 0U;
    int result;
    FirmwareAuthenticityResult auth;

    if ((descriptor == NULL) ||
        ((slot_address != OTA_PRIMARY_SLOT_ADDRESS) &&
         (slot_address != OTA_SECONDARY_SLOT_ADDRESS)))
    {
        return OTA_IMAGE_INVALID_ARGUMENT;
    }

    memset(descriptor, 0, sizeof(*descriptor));
    OtaImage_InvalidateCache(slot_address, sizeof(header));
    memcpy(&header, (const void *)(uintptr_t)slot_address, sizeof(header));
    if (!OtaImage_HeaderIsValid(&header))
    {
        return OTA_IMAGE_INVALID_HEADER;
    }
    if (!OtaImage_VectorIsValid(slot_address, &header))
    {
        return OTA_IMAGE_INVALID_VECTOR;
    }

    payload = (const uint8_t *)(uintptr_t)
        (slot_address + header.payload_offset);
    OtaImage_InvalidateCache((uint32_t)(uintptr_t)payload,
                             header.image_size);
    mbedtls_sha256_init(&context);
    result = mbedtls_sha256_starts_ret(&context, 0);
    while ((result == 0) && (offset < header.image_size))
    {
        size_t chunk = header.image_size - offset;
        if (chunk > 2048U)
        {
            chunk = 2048U;
        }
        result = mbedtls_sha256_update_ret(&context,
                                           payload + offset,
                                           chunk);
        offset += (uint32_t)chunk;
    }
    if (result == 0)
    {
        result = mbedtls_sha256_finish_ret(&context, calculated);
    }
    mbedtls_sha256_free(&context);
    if ((result != 0) ||
        (memcmp(calculated,
                header.payload_sha256,
                OTA_SHA256_SIZE) != 0))
    {
        return OTA_IMAGE_HASH_MISMATCH;
    }

    if (OtaImage_BuildSignedDigest(&header, signed_digest) != OTA_IMAGE_OK)
    {
        return OTA_IMAGE_HASH_MISMATCH;
    }
    auth = FirmwareAuthenticity_VerifyDigest(signed_digest,
                                             header.signature);
    memset(signed_digest, 0, sizeof(signed_digest));
    if (auth == FIRMWARE_AUTH_TRUST_ANCHOR_MISSING)
    {
        return OTA_IMAGE_TRUST_ANCHOR_MISSING;
    }
    if (auth != FIRMWARE_AUTH_OK)
    {
        return OTA_IMAGE_SIGNATURE_INVALID;
    }

    descriptor->magic = OTA_IMAGE_DESCRIPTOR_MAGIC;
    descriptor->image_size = header.image_size;
    descriptor->image_version = header.image_version;
    memcpy(descriptor->sha256,
           header.payload_sha256,
           sizeof(descriptor->sha256));
    memcpy(descriptor->signature,
           header.signature,
           sizeof(descriptor->signature));
    return OTA_IMAGE_OK;
}

const char *OtaImage_ResultName(OtaImageResult result)
{
    switch (result)
    {
        case OTA_IMAGE_OK:
            return "OK";
        case OTA_IMAGE_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case OTA_IMAGE_INVALID_HEADER:
            return "INVALID_HEADER";
        case OTA_IMAGE_INVALID_VECTOR:
            return "INVALID_VECTOR";
        case OTA_IMAGE_HASH_MISMATCH:
            return "HASH_MISMATCH";
        case OTA_IMAGE_SIGNATURE_INVALID:
            return "SIGNATURE_INVALID";
        case OTA_IMAGE_TRUST_ANCHOR_MISSING:
            return "TRUST_ANCHOR_MISSING";
        default:
            return "UNKNOWN";
    }
}
