# Durable outbox and MQTT topic contract

Implementation status: **active, journal format 2, reviewed 2026-08-24**.

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

The in-memory limits are deliberately bounded:

| Record family | Capacity | Full-queue behavior |
| --- | ---: | --- |
| Events | 32 | a durable event may replace a non-durable event; a queue containing only durable events rejects a new entry |
| Offline telemetry | 16 | the oldest sample is evicted after the replacement record is appended |
| Command results | 8 | the oldest replay-cache entry is removed |

The Flash sector contains 128 records of 1024 bytes. Both PUT and ACK records
consume journal space. When no erased record remains, the firmware erases the
sector and reconstructs the current live set from RAM.

## Offline telemetry

The queue contains at most 16 complete samples. A sample stores its stable
`sampleId`, original uptime and UTC time (when synchronized), configuration
version, enabled metric mask and up to 32 compact sensor measurements.

The first four samples of an outage are captured at the configured rate. Once
four samples are pending, the storage interval is no shorter than five minutes.
This preserves detail for short outages while bounding internal-Flash wear for
long outages. When the queue is full, a durable new sample replaces the oldest.
During normal append operation the replacement is committed before the oldest
RAM entry is removed.

After reconnection, samples are sent oldest-first with MQTT QoS 1. A sample is
removed only after PUBACK and after its acknowledgement record is stored. If a
connection is lost after the broker accepted a sample but before its local ACK,
the same `sampleId` may be sent again. The API therefore creates deterministic
telemetry record identifiers and treats the replay as an idempotent duplicate.
The original `ts` remains the measurement time; broker ingest time updates the
device's `lastSeenAt`.

### Power-loss and endurance boundary

A record program is fail-closed: its CRC and complete header must be valid
after reset. Sector compaction is different. The current board-compatible
implementation has only one outbox sector, so a power loss after erase and
before reconstruction finishes can lose previously live records. It therefore
provides durable best-effort delivery, not a mathematically atomic queue across
every possible power cut.

For a product that must preserve every critical record, replace the backend
with either:

1. two Flash sectors with generation numbers, copy-forward and a commit marker;
2. an endurance-qualified FRAM/MRAM journal;
3. a dedicated external storage device with transactional metadata.

The five-minute long-outage interval reduces wear but does not replace an
endurance calculation based on the actual event rate and guaranteed Flash
erase-cycle count.

## Command idempotency

The results of the eight most recent command identifiers are persistent. Before
a result is published, the firmware stores `commandId`, status, message and
execution time. After reset, a repeated command identifier returns that result
without invoking the command handler again. This protects retries caused by a
lost MQTT acknowledgement. As with any MCU-side replay cache, an irreversible
hardware action should still be designed so it is safe if power fails exactly
between the action and the result journal write.

The eight-entry cache is intentionally finite. A command repeated after its
identifier has aged out must also be safe at the domain level. Destructive
commands additionally require confirmation fields, hardware identity and/or a
fresh timestamp rather than relying only on the replay cache.

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

Current application channels are `status`, `capabilities`, `telemetry`,
`alerts`, `diagnostics`, `cmd` and `cmd/result`. Configuration does not use a
custom `config/desired` channel in the current contract; it uses the named AWS
IoT Device Shadow `config` under `$aws/things/{thingName}/shadow/name/config/...`.

## Delivery semantics

- Status uses retained MQTT state and a retained QoS 1 Last Will.
- Durable events and offline telemetry use QoS 1 and are removed only after
  the matching PUBACK and local ACK-record write.
- Non-durable diagnostics use QoS 0 and may be dropped during congestion.
- Duplicate QoS 1 delivery is expected and handled by stable `eventId`,
  `sampleId` or `commandId`; exactly-once delivery is not claimed.
- A successful MQTT write is not equivalent to completion of a command. The
  application action is confirmed separately on `cmd/result`.
