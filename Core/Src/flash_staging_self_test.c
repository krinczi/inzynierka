#include "flash_staging_self_test.h"

#include "debug_logger.h"

#include <stdint.h>
#include <string.h>

#define FLASH_SELF_TEST_PAYLOAD_SIZE 3U

static const uint8_t flash_self_test_payload[FLASH_SELF_TEST_PAYLOAD_SIZE] = {
    'a', 'b', 'c'
};

/* SHA-256("abc") from FIPS 180-4. */
static const uint8_t flash_self_test_sha256[FLASH_STAGING_SHA256_SIZE] = {
    0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
    0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
    0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
    0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD
};

static int FlashStagingSelfTest_IsOwnReadyFile(const FlashStagingInfo *info)
{
    return (info->state == FLASH_STAGING_STATE_READY) &&
           (info->file_size_bytes == FLASH_SELF_TEST_PAYLOAD_SIZE) &&
           (memcmp(info->expected_sha256,
                   flash_self_test_sha256,
                   FLASH_STAGING_SHA256_SIZE) == 0);
}

static FlashStagingResult FlashStagingSelfTest_VerifyReadback(void)
{
    uint8_t readback[FLASH_SELF_TEST_PAYLOAD_SIZE] = { 0U };
    FlashStagingResult result;

    result = FlashStaging_Verify();
    Debug_Logf(DEBUG_SRC_SYSTEM,
               "FLASH SELFTEST: verify result=%d",
               (int)result);
    if (result != FLASH_STAGING_OK)
    {
        return result;
    }

    result = FlashStaging_Read(0U, readback, sizeof(readback));
    if (result != FLASH_STAGING_OK)
    {
        Debug_Logf(DEBUG_SRC_SYSTEM,
                   "FLASH SELFTEST: readback result=%d",
                   (int)result);
        return result;
    }

    Debug_Logf(DEBUG_SRC_SYSTEM,
               "FLASH SELFTEST: readback result=0 value=%c%c%c",
               readback[0],
               readback[1],
               readback[2]);

    return memcmp(readback,
                  flash_self_test_payload,
                  sizeof(readback)) == 0
               ? FLASH_STAGING_OK
               : FLASH_STAGING_INTEGRITY_ERROR;
}

FlashStagingResult FlashStagingSelfTest_Run(void)
{
    const FlashStagingInfo *info = FlashStaging_GetInfo();
    FlashStagingResult result;

    Debug_Logf(DEBUG_SRC_SYSTEM,
               "FLASH SELFTEST: start state=%s size=%lu",
               FlashStaging_StateName(info->state),
               (unsigned long)info->file_size_bytes);

    if (info->state == FLASH_STAGING_STATE_RECEIVING)
    {
        Debug_LogLine(DEBUG_SRC_SYSTEM,
                      "FLASH SELFTEST: REFUSED - transfer is active");
        return FLASH_STAGING_INVALID_STATE;
    }

    if ((info->state == FLASH_STAGING_STATE_READY) &&
        !FlashStagingSelfTest_IsOwnReadyFile(info))
    {
        Debug_LogLine(DEBUG_SRC_SYSTEM,
                      "FLASH SELFTEST: REFUSED - another READY file is protected");
        return FLASH_STAGING_INVALID_STATE;
    }

    if (FlashStagingSelfTest_IsOwnReadyFile(info))
    {
        result = FlashStagingSelfTest_VerifyReadback();
        if (result == FLASH_STAGING_OK)
        {
            Debug_LogLine(DEBUG_SRC_SYSTEM,
                          "FLASH SELFTEST: PASS (existing test file)");
            return FLASH_STAGING_OK;
        }

        Debug_Logf(DEBUG_SRC_SYSTEM,
                   "FLASH SELFTEST: existing test file invalid, rewriting result=%d",
                   (int)result);
    }

    Debug_LogLine(DEBUG_SRC_SYSTEM,
                  "FLASH SELFTEST: erasing staging and writing payload abc");
    result = FlashStaging_Begin(FLASH_SELF_TEST_PAYLOAD_SIZE,
                                flash_self_test_sha256);
    Debug_Logf(DEBUG_SRC_SYSTEM,
               "FLASH SELFTEST: begin result=%d",
               (int)result);
    if (result != FLASH_STAGING_OK)
    {
        return result;
    }

    /* Intentionally non-aligned chunks exercise the Flash-word buffer. */
    result = FlashStaging_Write(flash_self_test_payload, 1U);
    if (result == FLASH_STAGING_OK)
    {
        result = FlashStaging_Write(flash_self_test_payload + 1U, 2U);
    }
    Debug_Logf(DEBUG_SRC_SYSTEM,
               "FLASH SELFTEST: write chunks=1+2 result=%d received=%lu",
               (int)result,
               (unsigned long)FlashStaging_GetInfo()->bytes_received);
    if (result != FLASH_STAGING_OK)
    {
        return result;
    }

    result = FlashStaging_Finalize();
    Debug_Logf(DEBUG_SRC_SYSTEM,
               "FLASH SELFTEST: finalize result=%d state=%s",
               (int)result,
               FlashStaging_StateName(FlashStaging_GetInfo()->state));
    if (result != FLASH_STAGING_OK)
    {
        return result;
    }

    result = FlashStagingSelfTest_VerifyReadback();
    if (result == FLASH_STAGING_OK)
    {
        Debug_LogLine(DEBUG_SRC_SYSTEM,
                      "FLASH SELFTEST: PASS (written, hashed and read back)");
    }
    else
    {
        Debug_Logf(DEBUG_SRC_SYSTEM,
                   "FLASH SELFTEST: FAIL result=%d",
                   (int)result);
    }

    return result;
}
