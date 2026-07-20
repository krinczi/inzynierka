# AWS IoT Jobs control plane

Status: MQTT Jobs discovery, document validation, and terminal status reporting
implemented. HTTPS artifact download is intentionally not part of this stage.

## Device workflow

1. Establish the existing mutual-TLS MQTT connection.
2. Subscribe to Jobs response topics.
3. Subscribe to `notify-next` last, preventing an immediate notification from
   being mistaken for a SUBACK while the remaining subscriptions are created.
4. Publish `StartNextPendingJobExecution` after every MQTT connection.
5. If there is no pending execution, remain idle until `notify-next` arrives.
6. Validate the execution identity, version, and bounded job document.
7. Publish `SUCCEEDED` for a valid `validate-only` document or `REJECTED` for
   an unsupported/invalid document.
8. Wait for `update/accepted`; then request the next execution.

After a disconnect, local state is cleared. On reconnect, `start-next` asks AWS
for the current in-progress execution first, making recovery deterministic.

## MQTT topics

For thing name `stm32h753zi-dev-01`, the module uses:

- subscribe: `$aws/things/stm32h753zi-dev-01/jobs/start-next/accepted`
- subscribe: `$aws/things/stm32h753zi-dev-01/jobs/start-next/rejected`
- subscribe: `$aws/things/stm32h753zi-dev-01/jobs/+/update/accepted`
- subscribe: `$aws/things/stm32h753zi-dev-01/jobs/+/update/rejected`
- subscribe: `$aws/things/stm32h753zi-dev-01/jobs/notify-next`
- publish: `$aws/things/stm32h753zi-dev-01/jobs/start-next`
- publish: `$aws/things/stm32h753zi-dev-01/jobs/{jobId}/update`

The thing name comes from the existing non-secret device configuration; it is
not duplicated in the Jobs implementation.

## Version 1 job document

Use the following non-destructive test document:

```json
{
  "schemaVersion": 1,
  "operation": "stage-file",
  "mode": "validate-only",
  "artifact": {
    "url": "https://example.invalid/artifacts/abc.bin",
    "size": 3,
    "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
  }
}
```

The same payload is stored in
`docs/examples/aws-job-validate-staging.json` for repeatable AWS tests.

`example.invalid` is deliberate: this stage validates metadata and never opens
the URL. A real S3 presigned HTTPS URL will be used by the downloader stage.

## Validation and safety limits

- No dynamic allocation is used.
- Waiting for a subscription acknowledgement does not assume that `SUBACK`
  is the next MQTT packet. Interleaved retained `PUBLISH` packets (for example
  `config/desired`) are processed and acknowledged before the matching
  packet identifier is accepted.
- Incoming MQTT packet limit: 2048 bytes.
- Parsed Jobs payload limit: 1791 bytes plus terminator.
- Maximum JSON nesting depth: 8.
- Maximum job ID: 64 characters, restricted to letters, numbers, `_`, and `-`.
- Only schema version 1 and operation `stage-file` are accepted.
- Only `validate-only` mode is executed at this stage.
- URL must use HTTPS, contain printable ASCII only, and fit in 512 characters.
- Artifact size must be from 1 byte through the actual Flash staging capacity.
- SHA-256 must contain exactly 64 hexadecimal characters and is decoded into
  32 bytes during validation.
- The URL is never written to UART logs because a future presigned URL contains
  credentials in its query string.
- No Flash erase or write is performed by `validate-only`.
- AWS update requests use the returned execution `versionNumber` as
  `expectedVersion`.
- Rejected control-plane requests use exponential retry delays of 5, 10, 20,
  and then at most 60 seconds, preventing a tight publish loop.
- A missing `start-next` or `update` response times out after 15 seconds and
  enters the same bounded retry path. Repeating `start-next` is safe because
  AWS returns an existing `IN_PROGRESS` execution before queued executions.

## Expected UART log

With no pending job:

```text
MQTT: SUBSCRIBE $aws/things/stm32h753zi-dev-01/jobs/notify-next
JOBS: start-next requested
JOBS: no pending execution
```

With the valid test document:

```text
JOBS: notify-next received
JOBS: start-next requested
JOBS: validated job=stm32-validate-staging-001 artifactSize=3 mode=validate-only
JOBS: status=SUCCEEDED published job=stm32-validate-staging-001
JOBS: status=SUCCEEDED accepted job=stm32-validate-staging-001
JOBS: start-next requested
JOBS: no pending execution
```

AWS should show the job execution as `SUCCEEDED`. The Flash staging file and
device configuration must remain unchanged.

## Next stage

The downloader will replace `validate-only` with an explicit download mode. It
will stream bounded HTTPS chunks into `FlashStaging_Write`, enforce content
length and SHA-256, apply retry/backoff, and report progress through
`IN_PROGRESS` status details before publishing a terminal state.

AWS protocol references:

- <https://docs.aws.amazon.com/iot/latest/developerguide/jobs-mqtt-api.html>
- <https://docs.aws.amazon.com/iot/latest/developerguide/jobs-workflow-device-online.html>
- <https://docs.aws.amazon.com/iot/latest/developerguide/reserved-topics.html>
