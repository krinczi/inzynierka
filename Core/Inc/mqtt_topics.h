#ifndef MQTT_TOPICS_H
#define MQTT_TOPICS_H

#include "aws_iot_config.h"

#ifndef MQTT_ENV
#define MQTT_ENV "dev"
#endif

#ifndef MQTT_DEVICE_ID
#define MQTT_DEVICE_ID AWS_THING_NAME
#endif

#define MQTT_TOPIC_BASE \
    "iot/" MQTT_ENV "/devices/" MQTT_DEVICE_ID

#define MQTT_TOPIC_STATUS          MQTT_TOPIC_BASE "/status"
#define MQTT_TOPIC_TELEMETRY       MQTT_TOPIC_BASE "/telemetry"
#define MQTT_TOPIC_ALERTS          MQTT_TOPIC_BASE "/alerts"
#define MQTT_TOPIC_DIAGNOSTICS     MQTT_TOPIC_BASE "/diagnostics"

#define MQTT_TOPIC_CMD             MQTT_TOPIC_BASE "/cmd"
#define MQTT_TOPIC_CMD_RESULT      MQTT_TOPIC_BASE "/cmd/result"
#define MQTT_TOPIC_CONFIG_DESIRED  MQTT_TOPIC_BASE "/config/desired"
#define MQTT_TOPIC_CONFIG_REPORTED MQTT_TOPIC_BASE "/config/reported"

#define MQTT_TOPIC_JOBS            MQTT_TOPIC_BASE "/jobs"
#define MQTT_TOPIC_JOBS_RESULT     MQTT_TOPIC_BASE "/jobs/result"

#endif /* MQTT_TOPICS_H */
