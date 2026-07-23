#include "mqtt_topics.h"

#include <stdio.h>
#include <string.h>

static MqttTopics mqtt_topics;
static uint8_t mqtt_topics_initialized;

static int MqttTopics_Build(char *output,
                            size_t output_size,
                            const char *environment,
                            const char *device_id,
                            const char *suffix)
{
    int written = snprintf(output,
                           output_size,
                           "iot/%s/devices/%s/%s",
                           environment,
                           device_id,
                           suffix);

    return ((written >= 0) && ((size_t)written < output_size)) ? 0 : -1;
}

int MqttTopics_Init(const char *environment, const char *device_id)
{
    if ((environment == NULL) || (environment[0] == '\0') ||
        (device_id == NULL) || (device_id[0] == '\0'))
    {
        return -1;
    }

    memset(&mqtt_topics, 0, sizeof(mqtt_topics));
    mqtt_topics_initialized = 0U;

    if ((MqttTopics_Build(mqtt_topics.status,
                          sizeof(mqtt_topics.status),
                          environment,
                          device_id,
                          "status") != 0) ||
        (MqttTopics_Build(mqtt_topics.telemetry,
                          sizeof(mqtt_topics.telemetry),
                          environment,
                          device_id,
                          "telemetry") != 0) ||
        (MqttTopics_Build(mqtt_topics.alerts,
                          sizeof(mqtt_topics.alerts),
                          environment,
                          device_id,
                          "alerts") != 0) ||
        (MqttTopics_Build(mqtt_topics.diagnostics,
                          sizeof(mqtt_topics.diagnostics),
                          environment,
                          device_id,
                          "diagnostics") != 0) ||
        (MqttTopics_Build(mqtt_topics.command,
                          sizeof(mqtt_topics.command),
                          environment,
                          device_id,
                          "cmd") != 0) ||
        (MqttTopics_Build(mqtt_topics.command_result,
                          sizeof(mqtt_topics.command_result),
                          environment,
                          device_id,
                          "cmd/result") != 0) ||
        (MqttTopics_Build(mqtt_topics.config_desired,
                          sizeof(mqtt_topics.config_desired),
                          environment,
                          device_id,
                          "config/desired") != 0) ||
        (MqttTopics_Build(mqtt_topics.config_reported,
                          sizeof(mqtt_topics.config_reported),
                          environment,
                          device_id,
                          "config/reported") != 0))
    {
        memset(&mqtt_topics, 0, sizeof(mqtt_topics));
        return -1;
    }

    mqtt_topics_initialized = 1U;
    return 0;
}

const MqttTopics *MqttTopics_Get(void)
{
    return mqtt_topics_initialized ? &mqtt_topics : NULL;
}
