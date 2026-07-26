#ifndef INC_SYSTEM_WATCHDOG_H_
#define INC_SYSTEM_WATCHDOG_H_

typedef enum
{
    SYSTEM_WATCHDOG_SUPERVISOR = 0,
    SYSTEM_WATCHDOG_MQTT,
    SYSTEM_WATCHDOG_CHANNEL_COUNT
} SystemWatchdogChannel;

/*
 * Starts IWDG with an approximately 32 second hardware timeout. Only the
 * supervisor services the counter and only while every registered service
 * heartbeat is fresh.
 */
int SystemWatchdog_InitAndStart(void);
void SystemWatchdog_Heartbeat(SystemWatchdogChannel channel);
void SystemWatchdog_Service(void);
int SystemWatchdog_IsStarted(void);

#endif /* INC_SYSTEM_WATCHDOG_H_ */
