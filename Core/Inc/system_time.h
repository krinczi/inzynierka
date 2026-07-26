#ifndef INC_SYSTEM_TIME_H_
#define INC_SYSTEM_TIME_H_

#include <stdint.h>
#include <sys/time.h>

typedef enum
{
    SYSTEM_TIME_INVALID = 0,
    SYSTEM_TIME_RTC_HOLDOVER,
    SYSTEM_TIME_SNTP_SYNCHRONIZED
} SystemTimeState;

/* Initializes the battery-backed RTC. It never installs a fabricated date. */
int SystemTime_Init(void);

/*
 * Synchronizes from authenticated network context prerequisites using NTP.
 * The function uses bounded retries and keeps a previously valid RTC value
 * when all servers are unavailable.
 */
int SystemTime_SyncNetwork(void);

int SystemTime_GetTimeval(struct timeval *value);
int SystemTime_IsValid(void);
SystemTimeState SystemTime_GetState(void);
const char *SystemTime_StateName(SystemTimeState state);

#endif /* INC_SYSTEM_TIME_H_ */
