# AWS IoT Jobs control plane

Status: MQTT Jobs discovery, document validation, bounded HTTPS download,
service-file staging, signed firmware installation, retry, and durable terminal
status reporting implemented. Job cancellation and authenticated rollback jobs
are included. Reviewed against the firmware on **2026-08-24**.

## Device workflow

1. Establish the existing mutual-TLS MQTT connection.
2. Subscribe to Jobs response topics.
3. Subscribe to `notify-next` last, preventing an immediate notification from
   being mistaken for a SUBACK while the remaining subscriptions are created.
4. Publish `StartNextPendingJobExecution` after every MQTT connection.
5. If there is no pending execution, remain idle until `notify-next` arrives.
6. Validate the execution identity, version, and bounded job document.
7. Publish `SUCCEEDED` for a valid `validate-only` document or start a bounded
   HTTPS transfer for `download` mode. `start-next` has already moved the AWS
   execution to `IN_PROGRESS`.
8. For a download, validate HTTP metadata, stream the body into the secondary
   Flash slot, and verify the complete-package SHA-256.
9. For `service-file`, publish the terminal status after staging. For
   `firmware`, verify the embedded signed manifest, persist the Job identity,
   mark the candidate ready, and reset for bootloader installation.
10. Publish firmware `SUCCEEDED` only after the trial image confirms healthy.
    Candidate rejection or automatic rollback is reported after MQTT reconnect.
11. Wait for `update/accepted`; then request the next execution.

An owner-requested cancellation is sent through the normal device command topic
as `jobs.cancel`. STM accepts it only for the matching active job, aborts HTTPS
and Flash staging, clears the local execution and replies on `cmd/result`. The
backend keeps `CANCEL_REQUESTED` until this acknowledgement arrives; requesting
cancellation is not reported as completed merely because MQTT publish returned.

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

## Version 1 job documents

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
the URL.

For a real non-executable service-file download, set the mode and URL:

```json
{
  "schemaVersion": 1,
  "operation": "stage-file",
  "mode": "download",
  "artifact": {
    "artifactType": "service-file",
    "url": "https://presigned-s3-url",
    "size": 21,
    "sha256": "0b3ebe7968f366ae0a7e257a483dc6c27e7f19776c775c3aa1af6ac82f35dab4"
  }
}
```

The URL must be a real, unexpired HTTPS URL. It is retained only for the
duration of the transfer and is never written to UART logs. The matching
`docs/examples/aws-job-download-template.json` intentionally contains an
invalid placeholder URL and must not be submitted unchanged.

For a signed executable package:

```json
{
  "schemaVersion": 1,
  "operation": "stage-file",
  "mode": "download",
  "artifact": {
    "artifactType": "firmware",
    "version": 2,
    "url": "https://presigned-s3-url",
    "size": 329980,
    "sha256": "<sha256-of-complete-ota-package>"
  }
}
```

`version` must match the signed version in the embedded `OTA3` header and must
be different from the currently confirmed version. During an ordinary update
it must be greater than the confirmed baseline. The `sha256` in the Job
protects transport of the whole package; the package also contains a signed
payload digest verified independently by the application and bootloader. See
`docs/FULL_OTA.md`.

### Authorized rollback to an older signed release

Rollback to an arbitrary older release uses the same signed package and the
full trial-boot path; it is not a raw bootloader jump. The backend adds two
fields inside `artifact`:

```json
{
  "artifactType": "firmware",
  "version": 7,
  "rollbackAuthorized": 1,
  "rollbackFromVersion": 12,
  "url": "https://presigned-s3-url",
  "size": 329980,
  "sha256": "<sha256-of-complete-ota-package>"
}
```

The device accepts the downgrade only when all conditions hold:

1. both rollback fields occur together and `rollbackAuthorized` equals `1`;
2. local Device Shadow configuration has `allowRollback=true`;
3. `rollbackFromVersion` equals the currently confirmed version;
4. target version differs from the current version;
5. the package target, vector table, payload digest and ECDSA P-256 signature
   are valid, and its embedded version equals `version`.

The authorization flag is persisted in the OTA control record and rechecked by
the bootloader. After a healthy trial, the older image becomes the new
confirmed baseline. Later ordinary updates must again be newer than that
baseline. This enables explicit recovery without permitting silent replay of
an old signed image.

## Validation and safety limits

- No dynamic allocation is used.
- Waiting for a subscription acknowledgement does not assume that `SUBACK`
  is the next MQTT packet. Interleaved retained `PUBLISH` packets (for example
  a named Shadow `config` delta/update) are processed and acknowledged before
  the matching packet identifier is accepted.
- Shared MQTT RX buffer: 8192 bytes.
- Decoded incoming payload limit: 7679 bytes plus terminator.
- Maximum JSON nesting depth: 8.
- Maximum job ID: 64 characters, restricted to letters, numbers, `_`, and `-`.
- Only schema version 1 and operation `stage-file` are accepted.
- Modes `validate-only` and `download` are accepted.
- URL must use HTTPS, contain printable ASCII only, and fit in 2048 characters
  (including the query string of a presigned S3 URL).
- Printable ASCII `\\uXXXX` escapes emitted by a JSON serializer are decoded
  before URL validation; control characters and non-ASCII escapes are rejected.
- The MQTT receive path accepts a bounded packet of 8192 bytes and a maximum
  decoded payload of 7679 bytes. This accommodates the AWS execution envelope
  plus the supported 2048-character URL while still rejecting oversized job
  responses before copying them to the parser stack.
- Artifact size must be from 1 byte through the secondary-slot capacity.
- `artifactType` may be `service-file` or `firmware`. An omitted type is
  treated as a service file for compatibility with existing Jobs.
- A firmware artifact requires a nonzero `version`. Release numbering remains
  monotonic, but an explicitly authorized rollback may select an older signed
  release as described above.
- `rollbackAuthorized` and `rollbackFromVersion` are rejected when only one is
  present, rollback is disabled locally, or the declared source version is
  stale.
- Firmware package size includes the fixed 1024-byte signed header and cannot
  exceed the 512 KiB secondary slot.
- SHA-256 must contain exactly 64 hexadecimal characters and is decoded into
  32 bytes during validation.
- The URL is never written to UART logs because a future presigned URL contains
  credentials in its query string.
- No Flash erase or write is performed by `validate-only`.
- The downloader enforces TLS 1.2, SNI/hostname validation, and mandatory
  server-certificate verification using the already loaded trust anchor.
- Only HTTP status `200` with one exact `Content-Length` is accepted.
- Redirects, chunked transfer encoding, compressed content, duplicate length
  headers, oversized headers, and a body longer than declared are rejected.
- Flash erase starts only after the HTTP status and exact length are valid.
- Network/DNS/TLS/truncated-transfer failures are retried from byte zero up to
  three attempts with backoff. A reconnect also restarts safely from zero.
- Firmware is not reported as successful merely because the download
  completed. Job identity and execution version survive reset; terminal status
  follows trial confirmation, candidate rejection, or rollback.
- Each network/TLS/Flash call is bounded; MQTT processing and keep-alive run
  between downloader steps.
- Total download time is limited to eight minutes, below the ten-minute Jobs
  step timer requested by the device.
- AWS update requests use the returned execution `versionNumber` as
  `expectedVersion`. Before the reboot that installs firmware, the
  `versionNumber` returned in the accepted `REBOOTING` response is appended to
  the durable OTA control log. A terminal report restored after reset omits the
  optional `expectedVersion`, allowing one safe migration from older firmware
  that could not persist the accepted version. The persisted job id, mutual-TLS
  device identity and durable pending marker still bind that report to the
  exact device execution.
- A restored terminal report is not retried forever when AWS has already moved
  the execution to a terminal state. `ResourceNotFound`,
  `TerminalStateReached`, or a terminal `executionState` returned with
  `InvalidStateTransition`/`VersionMismatch` clears only the durable report
  marker and immediately requests the next pending execution. Transient
  rejections such as `InternalError` and `RequestThrottled` retain the marker
  and continue through bounded backoff.
- Rejected control-plane requests use exponential retry delays of 5, 10, 20,
  and then at most 60 seconds, preventing a tight publish loop.
- A missing `start-next` or `update` response times out after 15 seconds and
  enters the same bounded retry path. Repeating `start-next` is safe because
  AWS returns an existing `IN_PROGRESS` execution before queued executions.
- `jobs.cancel` is idempotent at the command layer. A repeated `commandId`
  returns the persisted result; canceling a job that is no longer locally
  active returns an acknowledged no-op.
- Cancellation is cooperative. If the board is offline, the backend can record
  `CANCEL_REQUESTED`, but local HTTPS/Flash activity can stop only after the
  device receives the command or AWS reports a terminal state.

## STM32Cube integration invariant

`MX_LWIP_Init()` is application-owned and must execute exactly once, in
`StartDefaultTask`. `mbedtls_net_init()` in `MBEDTLS/App/net_sockets.c` only
sets the new context descriptor to `-1`; it must not initialize LwIP. Recheck
this customization after regenerating code with STM32CubeMX. Calling
`MX_LWIP_Init()` for the downloader's second TLS context causes the LwIP
`netif already added` assertion.

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

With a valid 21-byte download:

```text
JOBS: download active job=stm32-download-... artifactSize=21
HTTPS: download started size=21 attempt=1/3
HTTPS: DNS resolved
HTTPS: TCP connected
HTTPS: TLS handshake started
HTTPS: TLS certificate verified
HTTPS: request sent
HTTPS: response accepted size=21
HTTPS: SUCCEEDED received=21 sha256=verified
JOBS: download completed job=stm32-download-... result=success detail=artifact_staged
JOBS: status=SUCCEEDED published job=stm32-download-...
JOBS: status=SUCCEEDED accepted job=stm32-download-...
```

With a valid signed firmware package, the download phase ends by scheduling a
bootloader installation. After reset and local health confirmation, the
reconnected application reports:

```text
JOBS: firmware candidate ready job=stm32-firmware-... version=2
OTA: trial image confirmed version=2
JOBS: status=SUCCEEDED published job=stm32-firmware-...
JOBS: status=SUCCEEDED accepted job=stm32-firmware-...
```

If the candidate is invalid, the terminal detail is `candidate_rejected`. If
the trial never becomes healthy and the previous image is restored, it is
`trial_health_failed`.

For cancellation of an active transfer, the important sequence is:

```text
JOBS: local execution cancelled job=<jobId>
MQTT: PUBLISH iot/v1/dev/devices/<thingName>/cmd/result
```

The management application changes `CANCEL_REQUESTED` to `CANCELED` only after
that command result or an equivalent terminal AWS state. A successful command
publish alone is not final confirmation.

For `validate-only`, Flash staging and device configuration remain unchanged.
For a service-file download, staging changes to `READY` only after exact size
and SHA-256 verification. For firmware, AWS should show `SUCCEEDED` only after
the newly installed image has passed the local trial health gate.

## End-to-end S3 test from AWS CloudShell

Run these commands in the AWS console CloudShell in `eu-north-1`. The test
artifact contains exactly the 21 ASCII bytes `STM32-JOB-DOWNLOAD-V1`. This is
different from the three-byte Flash self-test artifact, so persistence can be
verified unambiguously after restart.

```bash
REGION=eu-north-1
THING_NAME=stm32h753zi-dev-01
ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
BUCKET="stm32-iot-jobs-${ACCOUNT_ID}-${REGION}"

aws s3api create-bucket \
  --bucket "$BUCKET" \
  --region "$REGION" \
  --create-bucket-configuration LocationConstraint="$REGION"

printf 'STM32-JOB-DOWNLOAD-V1' > /tmp/stm32-job-test.bin
aws s3 cp /tmp/stm32-job-test.bin \
  "s3://$BUCKET/tests/stm32-job-test.bin" \
  --region "$REGION"

URL=$(aws s3 presign \
  "s3://$BUCKET/tests/stm32-job-test.bin" \
  --region "$REGION" \
  --expires-in 3600)

test "${#URL}" -le 2048 || { echo "Presigned URL exceeds device limit"; false; }

THING_ARN=$(aws iot describe-thing \
  --region "$REGION" \
  --thing-name "$THING_NAME" \
  --query thingArn \
  --output text)

DOCUMENT=$(jq -nc --arg url "$URL" '{
  schemaVersion: 1,
  operation: "stage-file",
  mode: "download",
  artifact: {
    artifactType: "service-file",
    url: $url,
    size: 21,
    sha256: "0b3ebe7968f366ae0a7e257a483dc6c27e7f19776c775c3aa1af6ac82f35dab4"
  }
}')

JOB_ID="stm32-download-$(date +%Y%m%d%H%M%S)"
aws iot create-job \
  --region "$REGION" \
  --job-id "$JOB_ID" \
  --targets "$THING_ARN" \
  --target-selection SNAPSHOT \
  --description "STM32 HTTPS Flash staging test" \
  --document "$DOCUMENT"
```

If bucket creation reports `BucketAlreadyOwnedByYou`, continue with the
existing bucket. Do not print or commit `$URL`; it is a temporary credential.
After the UART reports `status=SUCCEEDED accepted`, inspect AWS:

```bash
aws iot describe-job-execution \
  --region "$REGION" \
  --job-id "$JOB_ID" \
  --thing-name "$THING_NAME"
```

Then restart the board and confirm that boot reports
`FLASH STAGING: state=READY ... size=21`. Finally remove the temporary object:

```bash
aws s3 rm "s3://$BUCKET/tests/stm32-job-test.bin" --region "$REGION"
```

## Implemented OTA continuation

For `service-file`, a successful download remains in
`FLASH_STAGING_STATE_READY` and is never executable. For `firmware`, staging
validates the OTA3 package, persists the AWS Job identity, marks
`CANDIDATE_READY` and hands control to the bootloader. The bootloader performs a
power-loss-resumable swap; the application then confirms a healthy trial or
the previous image is restored. Details and remaining manufacturing controls
are in `FULL_OTA.md`.

The implemented software path does not replace release operations. The private
signing key must remain in HSM/protected CI, the bootloader sector must be
write-protected for production, and repeated power-cut qualification is still
required.

AWS protocol references:

- <https://docs.aws.amazon.com/iot/latest/developerguide/jobs-mqtt-api.html>
- <https://docs.aws.amazon.com/iot/latest/developerguide/jobs-workflow-device-online.html>
- <https://docs.aws.amazon.com/iot/latest/developerguide/reserved-topics.html>
