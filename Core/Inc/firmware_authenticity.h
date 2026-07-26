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
 * Verifies a signature over the 32-byte SHA-256 artifact digest. Firmware
 * artifacts are rejected unless the board integration supplies a trust anchor.
 */
FirmwareAuthenticityResult FirmwareAuthenticity_VerifySha256(
    const uint8_t sha256[32],
    const char *signature_base64);

/*
 * Override in a board-specific, non-generated source. This is a public trust
 * anchor, never a signing private key. The default returns NULL and fails
 * closed.
 */
const unsigned char *FirmwareTrustAnchor_GetPublicKeyPem(size_t *length);

const char *FirmwareAuthenticity_ResultName(
    FirmwareAuthenticityResult result);

#endif /* INC_FIRMWARE_AUTHENTICITY_H_ */
