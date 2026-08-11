#ifndef INC_FLEET_PROVISIONING_H_
#define INC_FLEET_PROVISIONING_H_

#include <stddef.h>
#include <stdint.h>

typedef int (*FleetProvisioningPublishCallback)(const char *topic,
                                                const char *payload);

void FleetProvisioning_Init(FleetProvisioningPublishCallback publish_callback,
                            const char *template_name,
                            const char *thing_name,
                            const char *hardware_uid);

int FleetProvisioning_IsRequired(void);
size_t FleetProvisioning_SubscriptionCount(void);
const char *FleetProvisioning_SubscriptionTopic(size_t index);

/* Starts or resumes provisioning after all response topics are subscribed. */
int FleetProvisioning_OnMqttConnected(void);
void FleetProvisioning_OnMqttDisconnected(void);

/* Returns 1 when handled, 0 when unrelated, and -1 for a fatal local error. */
int FleetProvisioning_HandleMessage(const char *topic, const char *payload);

/* Advances bounded retries. Returns -1 only after a terminal local failure. */
int FleetProvisioning_Process(uint32_t now_ms);

/* Becomes true after credentials are committed and a reconnect is required. */
int FleetProvisioning_ReconnectRequired(void);

/*
 * Persists a service-requested certificate rotation. The current provisioned
 * pair remains available for automatic rollback until the new pair proves a
 * stable MQTT session.
 */
int FleetProvisioning_RequestRotation(void);

#endif /* INC_FLEET_PROVISIONING_H_ */
