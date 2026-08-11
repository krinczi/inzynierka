# Production hardening status

This document separates mechanisms implemented in the application and
bootloader from controls that require board hardware, AWS operations, or a
middleware migration.

The exact option-byte baseline, read-only audit command and irreversible
manufacturing gate are defined in `PRODUCTION_OPTION_BYTES.md`.

The maintained Mbed TLS and FreeRTOS baseline, source provenance and regression
gate are defined in `MIDDLEWARE_MIGRATION.md`.

## Implemented in the application

### Hardware telemetry

- ADC3 measures the internal STM32H753 temperature sensor and VREFINT.
- Supply voltage is calculated from the factory VREFINT calibration value.
- Multiple samples are averaged and range-checked.
- Current, humidity, and signal values are never fabricated. Weak board hooks
  return `unavailable` until a real driver overrides them.
- A telemetry payload contains only valid numeric measurements and identifies
  unavailable requested metrics.

Board integrations override:

- `SensorPlatform_ReadCurrentMicroamps`
- `SensorPlatform_ReadHumidityMilliPercent`
- `SensorPlatform_ReadSignalPercent`

### Time

- The battery-backed RTC uses LSE when available and falls back to LSI.
- Network time is obtained from three NTP servers with bounded timeouts.
- NTP responses are checked for mode, stratum, leap indicator, request nonce,
  and an acceptable Unix time range.
- TLS is blocked on first boot when neither RTC nor NTP provides a valid time.
- A valid RTC value is accepted as holdover when a later NTP refresh fails.
- Network time is refreshed every six hours while the network is available;
  a failed refresh is retried after 15 minutes without invalidating RTC
  holdover.
- `_gettimeofday()` no longer returns a compiled-in test date.

### Recovery and memory protection

- IWDG has an approximately 32 second hardware timeout.
- The supervisor refreshes IWDG only while both the network supervisor and the
  MQTT service publish fresh heartbeats.
- An MPU marks credential Flash, event journal Flash, and Bank 2
  staging/configuration Flash privileged-writable, unprivileged-read-only,
  execute-never, and non-cacheable. The non-cacheable normal-memory mapping
  makes runtime Flash programming synchronous while permitting the dedicated
  Flash managers to program their regions without granting unprivileged write
  access.
- The production boot-button Flash erase self-test remains disabled.

The current Cube package contains only the non-MPU FreeRTOS port. The hardware
MPU therefore protects memory attributes, but tasks remain privileged. Full
per-task isolation requires a FreeRTOS MPU-port migration.

### Durable operational events

- Bank 1 sector 5 is reserved as a 128 KiB append-only event journal.
- `WARNING`, `ALARM`, and `CRITICAL` alerts plus `ERROR`/`WARN`
  diagnostics are restored after reset.
- Low-volume service audit transitions (`MODE_STARTED`, `MODE_STOPPED`, and
  `MODE_EXPIRED`) explicitly request durable delivery regardless of the
  configured diagnostic log level.
- Periodic `MODE_ACTIVE`, ordinary `INFO`, and raw `DEBUG`/`TRACE` output stay
  non-durable. This prevents a service session from exhausting Flash endurance.
- The RAM delivery queue contains 32 structured events. When it is full, a
  durable event may evict a non-durable entry, but never another durable entry.
- Durable events use MQTT QoS 1; non-durable diagnostics use QoS 0.
- An event is removed only after the matching broker PUBACK and a durable ACK
  journal record.
- Journal records use a versioned header, sequence number, CRC32, read-back
  verification, and compaction.
- MQTT `eventId` remains stable across retries, allowing backend deduplication.
- A live connection loss queues a durable warning. The first successful MQTT
  reconnect queues a durable recovery diagnostic containing the outage
  duration, so an offline interval is visible after connectivity returns.
- MQTT CONNECT configures a retained QoS 1 Last Will on the device status
  topic. A successful session publishes retained `online` status, so an
  ungraceful disconnect is represented as retained `offline` state without
  changing the application topic contract.

The internal-Flash implementation is the board-compatible fallback. A high
write-rate product should replace it with FRAM or another endurance-qualified
store through the same queue semantics.

### Credentials and security posture

- Fleet ownership tokens are removed after `RegisterThing` and migrated out of
  an existing provisioned record.
- Temporary credential buffers and Flash programming buffers are explicitly
  zeroed.
- Unique device credentials support claim-based transactional rotation. The
  previous provisioned record remains available until the new certificate has
  maintained a healthy MQTT session for 30 seconds.
- Three persisted validation failures automatically restore the previous
  provisioned pair, including across MCU resets.
- Runtime diagnostics report RDP, debugger, MPU, mbedTLS, and FreeRTOS state.
- Production diagnostics warn when RDP is disabled, a debugger is attached, or
  a legacy dependency baseline is detected.

Option bytes are deliberately not programmed by the application. Before a
production release, an authorized manufacturing step must:

1. retain a recovery image and verify the exact MCU revision;
2. configure RDP level 1 (never level 2 during development);
3. configure the intended PCROP/WRP ranges using STM32CubeProgrammer;
4. disable or authenticate debug access as required by the product;
5. power-cycle and verify provisioning, TLS, event recovery, and service
   procedures;
6. record a tested recovery/unlock procedure and its data-loss consequences.

For stronger extraction resistance, use a secure element and override the
credential backend. CRC and MPU do not encrypt private keys against invasive
physical access.

After a successful device-side rotation, firmware reports the non-secret AWS
certificate identifier. The backend records both identities but performs no
automatic AWS mutation during MQTT ingest. An explicit owner action starts an
idempotent retirement operation which first verifies that the new certificate
is `ACTIVE` and attached to the expected Thing, then marks the replaced
certificate `INACTIVE`, detaches its policies, and detaches it from the Thing.
The certificate object is retained for audit. The device policy must not grant
certificate-administration permissions.

### Device decommissioning

The management application provides an explicit owner-only decommissioning
operation. The request must repeat the current device ID, hardware UID and
active certificate ID, and include a human-readable reason. The backend first
atomically sets `enabled=false` for the matching hardware UID and Thing in the
Fleet Provisioning allow-list. Only then does it verify that the certificate is
attached to the expected Thing, set it to `INACTIVE`, detach its policies and
remove the Thing-principal association. Missing allow-list configuration or an
identity mismatch aborts the operation before the certificate is changed.
The Thing, application record, telemetry and event history are retained for
audit. After completion, commands are rejected and delayed MQTT messages cannot
reactivate the application record. Erasing the STM32 credential store cannot
re-enroll a decommissioned board because the pre-provisioning hook rejects its
disabled UID. Re-enrollment is intentionally a separate, audited service
procedure.

### Complete signed OTA boot chain

- A separate bootloader occupies a dedicated 128 KiB sector.
- Primary and secondary 512 KiB slots hold complete signed OTA packages.
- Every package contains an `OTA3` manifest, payload SHA-256, target ID,
  monotonic version, vector metadata, and raw ECDSA-P256 signature.
- The application and bootloader independently verify the package.
- Installation uses a power-loss-resumable sector swap and dedicated scratch
  sector.
- Trial images are confirmed only after local persistent services and IWDG are
  operational and MQTT has remained healthy for at least 30 seconds.
- Three unconfirmed trial boots trigger automatic restoration of the previous
  image.
- The confirmed version is a monotonic anti-rollback floor.
- AWS Job completion is retained across reset and reported only after
  confirmation, rejection, or rollback.

Jobs remain compatible with non-executable `service-file` staging. The
firmware signing private key is never stored in the repository or device; only
the public P-256 trust anchor is compiled into the boot chain. The complete
format, release commands, state machine, and hardware evidence are documented
in `docs/FULL_OTA.md`.

## Remaining production deliverables

### Middleware migration

The generated project currently contains mbedTLS 2.16.2 and FreeRTOS 10.3.1.
Replacing individual files is unsafe because the Cube integration, APIs,
configuration, ports, and test matrix must move together. The release process
must include:

1. migration to a maintained STM32Cube middleware baseline with mbedTLS 3.6
   LTS and a supported FreeRTOS release;
2. regeneration of integration code in a dedicated branch;
3. TLS, Fleet Provisioning, Jobs, Shadow, reconnect, fault-injection, and
   long-run regression tests;
4. an SBOM and periodic CVE review with documented response deadlines.

Until that migration is complete, the runtime emits an explicit legacy
dependency warning and the image must not be classified as production-ready.

### Boot-chain deployment controls

The OTA software path is implemented. Production manufacturing must still
write-protect the bootloader, apply the approved RDP/PCROP/WRP option-byte
policy, move release signing to an HSM or protected CI service, define signing
key rotation/revocation, and execute repeated power-cut qualification at every
Flash transition. These controls deliberately remain outside normal
application firmware.

## Claim-certificate operations

The detailed planned-rotation and emergency-disable procedure is maintained in
the application repository as `docs/CLAIM_CERTIFICATE_ROTATION.md`. Protected
OTA signing controls and the HSM-neutral signing request format are documented
in `docs/RELEASE_SIGNING.md`.

Fleet claim credentials are shared bootstrap credentials and therefore remain
an operational risk even when device credentials are unique. Production AWS
operations must:

- restrict the claim policy to Fleet Provisioning topics only;
- validate hardware UID in the pre-provisioning hook;
- rate-limit and alert on provisioning failures;
- inventory issued device certificates;
- rotate and revoke the claim certificate on a defined schedule;
- provide an incident procedure for immediate claim revocation.
