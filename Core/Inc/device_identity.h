#ifndef INC_DEVICE_IDENTITY_H_
#define INC_DEVICE_IDENTITY_H_

#include <stddef.h>
#include <stdint.h>

#include "credential_store.h"
#include "device_config.h"

#define DEVICE_IDENTITY_THING_PREFIX              "stm32h753zi-"
#define DEVICE_IDENTITY_THING_NAME_MAX_LENGTH      CREDENTIAL_STORE_THING_NAME_MAX_LENGTH
#define DEVICE_IDENTITY_CLIENT_ID_MAX_LENGTH       CREDENTIAL_STORE_THING_NAME_MAX_LENGTH

/*
 * Refreshes the runtime identity from the credential store. Before Fleet
 * Provisioning completes, a deterministic Thing name is derived from the
 * hardware UID. After provisioning, the Thing name committed by AWS is used.
 */
int DeviceIdentity_Refresh(void);

const char *DeviceIdentity_GetThingName(void);
const char *DeviceIdentity_GetClientId(void);
const char *DeviceIdentity_GetHardwareUid(void);

/*
 * Generates a stable, locally administered unicast MAC address from the
 * immutable STM32 hardware UID. This function is safe before RTOS startup.
 */
void DeviceIdentity_GetMacAddress(uint8_t output[6]);

#endif /* INC_DEVICE_IDENTITY_H_ */
