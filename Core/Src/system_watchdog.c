#include "system_watchdog.h"

#include "main.h"
#include "stm32h7xx_hal_iwdg.h"

#include <stddef.h>

#define SYSTEM_WATCHDOG_RELOAD             3999U
#define SYSTEM_WATCHDOG_HEARTBEAT_LIMIT_MS 25000U

static IWDG_HandleTypeDef watchdog;
static volatile uint32_t heartbeat_ms[SYSTEM_WATCHDOG_CHANNEL_COUNT];
static volatile uint8_t watchdog_started;

int SystemWatchdog_InitAndStart(void)
{
    uint32_t now;
    size_t index;

    if (watchdog_started != 0U)
    {
        return 0;
    }

    now = HAL_GetTick();
    for (index = 0U; index < SYSTEM_WATCHDOG_CHANNEL_COUNT; index++)
    {
        heartbeat_ms[index] = now;
    }

    watchdog.Instance = IWDG1;
    watchdog.Init.Prescaler = IWDG_PRESCALER_256;
    watchdog.Init.Window = IWDG_WINDOW_DISABLE;
    watchdog.Init.Reload = SYSTEM_WATCHDOG_RELOAD;

    if (HAL_IWDG_Init(&watchdog) != HAL_OK)
    {
        return -1;
    }

    watchdog_started = 1U;
    return 0;
}

void SystemWatchdog_Heartbeat(SystemWatchdogChannel channel)
{
    if ((unsigned int)channel < SYSTEM_WATCHDOG_CHANNEL_COUNT)
    {
        heartbeat_ms[channel] = HAL_GetTick();
    }
}

void SystemWatchdog_Service(void)
{
    uint32_t now;
    size_t index;

    if (watchdog_started == 0U)
    {
        return;
    }

    now = HAL_GetTick();
    for (index = 0U; index < SYSTEM_WATCHDOG_CHANNEL_COUNT; index++)
    {
        if ((now - heartbeat_ms[index]) >
            SYSTEM_WATCHDOG_HEARTBEAT_LIMIT_MS)
        {
            return;
        }
    }

    (void)HAL_IWDG_Refresh(&watchdog);
}

int SystemWatchdog_IsStarted(void)
{
    return watchdog_started != 0U;
}
