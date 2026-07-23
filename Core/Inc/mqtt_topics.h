#ifndef MQTT_TOPICS_H
#define MQTT_TOPICS_H

#include <stdint.h>

#ifndef MQTT_ENV
#define MQTT_ENV "dev"
#endif

#define MQTT_TOPICS_MAX_LENGTH 256U

typedef struct
{
    char status[MQTT_TOPICS_MAX_LENGTH];
    char telemetry[MQTT_TOPICS_MAX_LENGTH];
    char alerts[MQTT_TOPICS_MAX_LENGTH];
    char diagnostics[MQTT_TOPICS_MAX_LENGTH];
    char command[MQTT_TOPICS_MAX_LENGTH];
    char command_result[MQTT_TOPICS_MAX_LENGTH];
    char config_desired[MQTT_TOPICS_MAX_LENGTH];
    char config_reported[MQTT_TOPICS_MAX_LENGTH];
} MqttTopics;

/* Builds all application topics from the runtime device identity. */
int MqttTopics_Init(const char *environment, const char *device_id);
const MqttTopics *MqttTopics_Get(void);

#endif /* MQTT_TOPICS_H */
