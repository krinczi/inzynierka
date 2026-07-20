#ifndef INC_AWS_JOBS_H_
#define INC_AWS_JOBS_H_

#include <stddef.h>

typedef int (*AwsJobsPublishCallback)(const char *topic, const char *payload);

/* Initializes the session-independent Jobs state machine. */
void AwsJobs_Init(AwsJobsPublishCallback publish_callback);

/* Topics are ordered so response topics are subscribed before notify-next. */
size_t AwsJobs_SubscriptionCount(void);
const char *AwsJobs_SubscriptionTopic(size_t index);

/* Starts Jobs discovery after all Jobs subscriptions are active. */
int AwsJobs_OnMqttConnected(void);
void AwsJobs_OnMqttDisconnected(void);

/* Runs bounded retry/backoff work from the existing MQTT service loop. */
int AwsJobs_Process(void);

/*
 * Returns 1 when the topic was handled, 0 when it is not a Jobs topic,
 * and -1 when handling required an MQTT publish that failed.
 */
int AwsJobs_HandleMessage(const char *topic, const char *payload);

#endif /* INC_AWS_JOBS_H_ */
