# Durable outbox and MQTT topic contract

## Scope

The firmware uses Bank 1 sector 5 (`0x080A0000-0x080BFFFF`) as one bounded,
append-only operational outbox. No new Flash area is introduced. Journal
version 2 stores three independent record families while retaining read
compatibility with legacy version-1 event records:

- durable alerts and diagnostic events;
- telemetry captured while the MQTT session is unavailable;
- results of recently executed commands.

Each record has a magic value, format version, kind, monotonically increasing
journal sequence, payload length and CRC-32. Incomplete or corrupt records are
ignored during recovery and counted as persistence failures.

## Offline telemetry

The queue contains at most 16 complete samples. A sample stores its stable
`sampleId`, original uptime and UTC time (when synchronized), configuration
version, enabled metric mask and up to 32 compact sensor measurements.

The first four samples of an outage are captured at the configured rate. Once
four samples are pending, the storage interval is no shorter than five minutes.
This preserves detail for short outages while bounding internal-Flash wear for
long outages. When the queue is full, a durable new sample replaces the oldest.
The write ordering guarantees that a power loss cannot erase the old sample
before its replacement is present in Flash.

After reconnection, samples are sent oldest-first with MQTT QoS 1. A sample is
removed only after PUBACK and after its acknowledgement record is stored. If a
connection is lost after the broker accepted a sample but before its local ACK,
the same `sampleId` may be sent again. The API therefore creates deterministic
telemetry record identifiers and treats the replay as an idempotent duplicate.
The original `ts` remains the measurement time; broker ingest time updates the
device's `lastSeenAt`.

## Command idempotency

The results of the eight most recent command identifiers are persistent. Before
a result is published, the firmware stores `commandId`, status, message and
execution time. After reset, a repeated command identifier returns that result
without invoking the command handler again. This protects retries caused by a
lost MQTT acknowledgement. As with any MCU-side replay cache, an irreversible
hardware action should still be designed so it is safe if power fails exactly
between the action and the result journal write.

## Versioned topic contract

Application topics use this structure:

```text
iot/v1/{environment}/devices/{thingName}/{channel}
```

For the development environment the prefix is `iot/v1/dev/devices`. AWS Jobs,
Fleet Provisioning and Device Shadow retain their AWS-reserved `$aws/...`
topics. The version is centralized as:

- `MQTT_TOPIC_CONTRACT_VERSION` in STM32 firmware;
- `AWS_IOT_TOPIC_PREFIX` in the API and worker;
- `TopicContractVersion` in the CloudFormation policy.

A future `v2` must be deployed as an explicit coordinated migration: allow the
new prefix in AWS IoT policy, deploy compatible consumers, update devices, and
only then retire `v1`. Topic versions are not inferred from payload versions.
