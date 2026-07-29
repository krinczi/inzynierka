#ifndef INC_FIRMWARE_AUTHENTICITY_H_
#define INC_FIRMWARE_AUTHENTICITY_H_

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    FIRMWARE_AUTH_OK = 0,
    FIRMWARE_AUTH_INVALID_ARGUMENT,
    FIRMWARE_AUTH_TRUST_ANCHOR_MISSING,
    FIRMWARE_AUTH_SIGNATURE_ENCODING_INVALID,
    FIRMWARE_AUTH_SIGNATURE_INVALID
} FirmwareAuthenticityResult;

/*
 * Verifies a raw ECDSA-P256 signature (r||s, 64 bytes) over a 32-byte digest.
 * Firmware artifacts are rejected unless the board integration supplies the
 * uncompressed 65-byte P-256 public point (0x04 || X || Y).
 */
FirmwareAuthenticityResult FirmwareAuthenticity_VerifyDigest(
    const uint8_t digest[32],
    const uint8_t signature[64]);

/*
 * Decodes a Base64 raw P-256 signature, verifies it and optionally returns the
 * decoded bytes for durable OTA metadata.
 */
FirmwareAuthenticityResult FirmwareAuthenticity_DecodeAndVerifyDigest(
    const uint8_t digest[32],
    const char *signature_base64,
    uint8_t decoded_signature[64]);

/*
 * Override in a board-specific source. This is only a public trust anchor,
 * never the signing private key. The default returns NULL and fails closed.
 */
const uint8_t *FirmwareTrustAnchor_GetPublicKey(size_t *length);

const char *FirmwareAuthenticity_ResultName(
    FirmwareAuthenticityResult result);

#endif /* INC_FIRMWARE_AUTHENTICITY_H_ */
