#include "firmware_authenticity.h"
#include "firmware_trust_anchor_config.h"

const uint8_t *FirmwareTrustAnchor_GetPublicKey(size_t *length)
{
    if (length != NULL)
    {
        *length = FIRMWARE_TRUST_ANCHOR_CONFIGURED != 0U
                      ? sizeof(firmware_trust_anchor_public_key)
                      : 0U;
    }
    return FIRMWARE_TRUST_ANCHOR_CONFIGURED != 0U
               ? firmware_trust_anchor_public_key
               : NULL;
}
