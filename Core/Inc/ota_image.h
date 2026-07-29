#ifndef INC_OTA_IMAGE_H_
#define INC_OTA_IMAGE_H_

#include "ota_control.h"

#include <stdint.h>

#define OTA_IMAGE_HEADER_MAGIC          0x3341544FUL /* "OTA3" */
#define OTA_IMAGE_HEADER_FORMAT         1U
#define OTA_IMAGE_TARGET_STM32H753      0x07530001UL
#define OTA_IMAGE_SIGNING_CONTEXT       "STM32H753-OTA3"
#define OTA_IMAGE_SIGNING_CONTEXT_SIZE  14U

/*
 * The header occupies the first 1024 bytes of every executable slot. The
 * application vector table follows at +0x400, satisfying Cortex-M7 VTOR
 * alignment. The signature is raw ECDSA-P256 r||s and signs the canonical
 * digest produced by OtaImage_BuildSignedDigest().
 */
typedef struct
{
    uint32_t magic;
    uint16_t format;
    uint16_t header_size;
    uint32_t target_id;
    uint32_t image_version;
    uint32_t image_size;
    uint32_t payload_offset;
    uint32_t primary_vector_address;
    uint32_t flags;
    uint8_t payload_sha256[OTA_SHA256_SIZE];
    uint8_t signature[OTA_P256_SIGNATURE_SIZE];
    uint8_t reserved[892];
    uint32_t header_crc32;
} OtaImageHeader;

_Static_assert(sizeof(OtaImageHeader) == OTA_IMAGE_HEADER_SIZE,
               "OTA image header must be exactly 1024 bytes");

typedef enum
{
    OTA_IMAGE_OK = 0,
    OTA_IMAGE_INVALID_ARGUMENT = -1,
    OTA_IMAGE_INVALID_HEADER = -2,
    OTA_IMAGE_INVALID_VECTOR = -3,
    OTA_IMAGE_HASH_MISMATCH = -4,
    OTA_IMAGE_SIGNATURE_INVALID = -5,
    OTA_IMAGE_TRUST_ANCHOR_MISSING = -6
} OtaImageResult;

uint32_t OtaImage_HeaderCrc32(const OtaImageHeader *header);
int OtaImage_HeaderIsValid(const OtaImageHeader *header);
int OtaImage_VectorIsValid(uint32_t slot_address,
                           const OtaImageHeader *header);

/*
 * Canonical signed digest:
 * SHA256(context || target_id || version || size || payload_sha256).
 * Integer fields are encoded big-endian, so the host signer is portable.
 */
OtaImageResult OtaImage_BuildSignedDigest(
    const OtaImageHeader *header,
    uint8_t digest[OTA_SHA256_SIZE]);

/* Reads, hashes and authenticates an image package directly from Flash. */
OtaImageResult OtaImage_VerifySlot(uint32_t slot_address,
                                   OtaImageDescriptor *descriptor);

const char *OtaImage_ResultName(OtaImageResult result);

#endif /* INC_OTA_IMAGE_H_ */
