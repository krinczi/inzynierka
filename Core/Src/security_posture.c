#include "security_posture.h"

#include "debug_logger.h"
#include "iot_events.h"
#include "main.h"
#include "stm32h7xx_hal_cortex.h"
#include "stm32h7xx_hal_flash_ex.h"
#include "mbedtls/version.h"
#include "FreeRTOS.h"
#include "task.h"

#define SECURITY_EVENT_FLASH_BASE       0x080A0000UL
#define SECURITY_CREDENTIAL_FLASH_BASE  0x080C0000UL
#define SECURITY_BANK2_FLASH_BASE       0x08100000UL

void SecurityPosture_ConfigureMpu(void)
{
#if (__MPU_PRESENT == 1U)
    MPU_Region_InitTypeDef region = { 0 };

    HAL_MPU_Disable();

    region.Enable = MPU_REGION_ENABLE;
    region.Number = MPU_REGION_NUMBER0;
    region.BaseAddress = SECURITY_CREDENTIAL_FLASH_BASE;
    region.Size = MPU_REGION_SIZE_256KB;
    region.SubRegionDisable = 0x00U;
    /*
     * These Flash areas are programmed at runtime. Map them as normal,
     * non-cacheable memory so HAL Flash stores reach the controller
     * synchronously instead of becoming deferred AXI writes. A cacheable
     * writable mapping can otherwise surface later as IMPRECISERR at a
     * barrier or cache-maintenance instruction.
     */
    region.TypeExtField = MPU_TEX_LEVEL1;
    /*
     * Flash managers execute in privileged mode and must be able to program
     * their assigned regions. Unprivileged application code receives
     * read-only access after the FreeRTOS MPU-port migration.
     */
    region.AccessPermission = MPU_REGION_PRIV_RW_URO;
    region.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    region.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
    region.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    region.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&region);

    region.Number = MPU_REGION_NUMBER1;
    region.BaseAddress = SECURITY_BANK2_FLASH_BASE;
    region.Size = MPU_REGION_SIZE_1MB;
    HAL_MPU_ConfigRegion(&region);

    region.Number = MPU_REGION_NUMBER2;
    region.BaseAddress = SECURITY_EVENT_FLASH_BASE;
    region.Size = MPU_REGION_SIZE_128KB;
    HAL_MPU_ConfigRegion(&region);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
#endif
}

SecurityPosture SecurityPosture_Evaluate(void)
{
    FLASH_OBProgramInitTypeDef options = { 0 };
    SecurityPosture posture = { 0 };

    HAL_FLASHEx_OBGetConfig(&options);
    if (options.RDPLevel == OB_RDP_LEVEL_0)
    {
        posture.rdp_level = SECURITY_RDP_LEVEL_0;
    }
    else if (options.RDPLevel == OB_RDP_LEVEL_2)
    {
        posture.rdp_level = SECURITY_RDP_LEVEL_2;
    }
    else if (options.RDPLevel == OB_RDP_LEVEL_1)
    {
        posture.rdp_level = SECURITY_RDP_LEVEL_1;
    }
    else
    {
        posture.rdp_level = SECURITY_RDP_UNKNOWN;
    }

    posture.debugger_attached =
        ((CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0U) ? 1U : 0U;
    posture.mpu_enabled = ((MPU->CTRL & MPU_CTRL_ENABLE_Msk) != 0U) ? 1U : 0U;
    return posture;
}

void SecurityPosture_Report(void)
{
    SecurityPosture posture = SecurityPosture_Evaluate();

    Debug_Logf(DEBUG_SRC_SYSTEM,
               "SECURITY: rdp=%s debugger=%s mpu=%s",
               SecurityPosture_RdpName(posture.rdp_level),
               posture.debugger_attached ? "attached" : "detached",
               posture.mpu_enabled ? "enabled" : "disabled");
    Debug_Logf(DEBUG_SRC_SYSTEM,
               "SECURITY: mbedTLS=0x%08lx FreeRTOS=%s",
               (unsigned long)MBEDTLS_VERSION_NUMBER,
               tskKERNEL_VERSION_NUMBER);

    if (posture.rdp_level == SECURITY_RDP_LEVEL_0)
    {
        (void)IotEvents_QueueAlert(
            IOT_ALERT_CRITICAL,
            "SECURITY_RDP_DISABLED",
            "Readout protection is disabled; production credentials are physically readable");
    }
    if (posture.debugger_attached != 0U)
    {
        (void)IotEvents_QueueAlert(
            IOT_ALERT_WARNING,
            "SECURITY_DEBUGGER_ATTACHED",
            "A hardware debugger is attached in the production firmware profile");
    }
    if (posture.mpu_enabled == 0U)
    {
        (void)IotEvents_QueueAlert(
            IOT_ALERT_CRITICAL,
            "SECURITY_MPU_DISABLED",
            "Memory protection unit is disabled");
    }
#if MBEDTLS_VERSION_NUMBER < 0x03060000
    (void)IotEvents_QueueAlert(
        IOT_ALERT_WARNING,
        "SECURITY_MBEDTLS_MIGRATION_REQUIRED",
        "Legacy mbedTLS branch detected; migrate the Cube middleware baseline to mbedTLS 3.6 LTS");
#endif
#if (tskKERNEL_VERSION_MAJOR < 11)
    (void)IotEvents_QueueAlert(
        IOT_ALERT_WARNING,
        "SECURITY_FREERTOS_MIGRATION_REQUIRED",
        "Legacy FreeRTOS kernel detected; migrate the generated Cube middleware baseline");
#endif
}

const char *SecurityPosture_RdpName(SecurityRdpLevel level)
{
    switch (level)
    {
        case SECURITY_RDP_LEVEL_0:
            return "LEVEL_0";
        case SECURITY_RDP_LEVEL_1:
            return "LEVEL_1";
        case SECURITY_RDP_LEVEL_2:
            return "LEVEL_2";
        case SECURITY_RDP_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}
