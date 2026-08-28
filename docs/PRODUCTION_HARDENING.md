# Production hardening status

Assessment date: **2026-08-24**. Current middleware baseline: **Mbed TLS 3.6.7
LTS and FreeRTOS Kernel 11.3.0**.

This document separates mechanisms implemented in the application and
bootloader from controls that require board hardware, AWS operations,
manufacturing or release infrastructure.

The exact option-byte baseline, read-only audit command and irreversible
manufacturing gate are defined in `PRODUCTION_OPTION_BYTES.md`.

The maintained Mbed TLS and FreeRTOS baseline, source provenance and regression
gate are defined in `MIDDLEWARE_MIGRATION.md`. Older references to Mbed TLS
2.16.2 and FreeRTOS 10.3.1 are obsolete.

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
- An MPU marks credential Flash, durable outbox Flash, and Bank 2
  staging/configuration Flash privileged-writable, unprivileged-read-only,
  execute-never, and non-cacheable. The non-cacheable normal-memory mapping
  makes runtime Flash programming synchronous while permitting the dedicated
  Flash managers to program their regions without granting unprivileged write
  access.
- The production boot-button Flash erase self-test remains disabled.

The project currently builds the non-MPU ARM CM4F FreeRTOS port. The hardware
MPU therefore enforces region memory attributes and execute-never behavior, but
application tasks remain privileged and are not mutually isolated. Full
per-task least privilege requires migration to a matching FreeRTOS MPU port,
task-region definitions and regression of every HAL/LwIP callback; setting
`configENABLE_MPU` alone is unsafe.

The current Release map places approximately 0x5E7DC bytes of `.bss` in the
512 KiB D1 RAM region, including the 192 KiB FreeRTOS heap. This leaves useful
headroom but should be treated as a budget: every release must check linker
usage, minimum free heap and task stack high-water marks. DTCM and other SRAM
regions are not automatic substitutes for DMA-visible LwIP/Ethernet buffers.

### Durable operational outbox

- Bank 1 sector 5 is reserved as a 128 KiB versioned append-only outbox.
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
- Up to 16 telemetry samples are retained while MQTT is unavailable and are
  replayed oldest-first with QoS 1. A stable `sampleId` lets the backend
  deduplicate retransmission after a lost PUBACK.
- The first four offline samples use the configured interval. A five-minute
  minimum interval is then applied to limit Flash wear during a long outage.
- Results of the eight most recent commands are restored after reset. A
  repeated `commandId` republishes the stored result without executing the
  command again.
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

Important boundary: the outbox uses one Flash sector. Individual appended
records fail closed after an incomplete program, but compaction erases that
sector before reconstructing live records from RAM. A power cut in this window
can lose the operational queue. This implementation is therefore resilient and
bounded, but not a fully atomic store across every power interruption. A strict
store-and-forward requirement needs dual-sector copy-forward with a committed
generation or external FRAM/MRAM. Flash endurance must be calculated from the
real telemetry/event rate; throttling is not proof of lifetime by itself.

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
- The backend can optionally schedule rotation by credential age and retire the
  previous AWS certificate after both device-side validation and an additional
  operator-defined grace period. Scheduling is disabled until the production
  environment explicitly enables its policy variables.
- Runtime diagnostics report RDP, debugger, MPU, mbedTLS, and FreeRTOS state.
- Production diagnostics warn when RDP is disabled, a debugger is attached, or
  a legacy dependency baseline is detected. The current 3.6.7/11.3.0 baseline
  does not trigger the legacy-version warning.
- Emergency provisioning revocation is persisted in the backend before a
  best-effort `provisioning.cancel` command is sent. Online device erasure has
  a distinct completion acknowledgement; an offline revoke is never presented
  as proof that local Flash was cleared.
- Recommissioning requires an owner-only, one-use, expiring challenge and a
  controlled allow-list transition. It does not bypass local
  `DECOMMISSIONED` quarantine.

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
active certificate ID, and include a human-readable reason. For an online
device, the backend first sends a fresh owner-authorized command containing the
exact hardware UID. The STM32 persists a decommission-pending marker before it
acknowledges the command, then erases staged data, durable events and unique
credentials after reset. Finally it stores a local `DECOMMISSIONED` quarantine
marker which prevents both cloud connection and automatic re-provisioning.
Only after the command acknowledgement does the backend atomically set
`enabled=false` in the Fleet Provisioning allow-list, verify the Thing
association, set the active certificate to `INACTIVE`, detach its policies and
remove the Thing-principal association. An explicit cloud-only emergency path
is available for an unreachable device and is reported separately in the
audit result because local erasure cannot then be confirmed.
The Thing, application record, telemetry and event history are retained for
audit. After completion, commands are rejected and delayed MQTT messages cannot
reactivate the application record. Erasing the STM32 credential store cannot
re-enroll a decommissioned board because the pre-provisioning hook rejects its
disabled UID. Re-enrollment is intentionally a separate, audited service
procedure.

### Factory reset and recovery

- `lifecycle.factory_reset` is owner-only and requires `confirm=true`, the exact
  96-bit hardware UID and a command timestamp no older than five minutes.
- The request is written to the dual-sector configuration journal before the
  MQTT acknowledgement is sent. A power loss at any later point resumes the
  reset during the next boot.
- The reset clears staged downloads, durable event records, unique device
  credentials and the persisted Device Shadow configuration, then reboots into
  factory defaults and Fleet Provisioning.
- Factory reset deliberately does not alter the AWS allow-list. Decommissioning
  must be used when the device must not enroll again.
- A locally decommissioned unit stays quarantined even if Ethernet is restored;
  re-enrollment requires an explicit controlled service procedure.

### Remote network configuration

- Named Device Shadow `config` schema version 3 includes DHCP/static selection,
  IPv4 address,
  contiguous subnet mask, gateway, primary/secondary DNS and DHCP recovery.
- API, web UI and firmware independently validate canonical IPv4 syntax,
  unicast host addressing and gateway membership in the configured subnet.
- A valid Shadow update is committed atomically before it is acknowledged.
  Network reconfiguration starts only after the reported-state publication.
- After three failed AWS connection attempts with a static configuration, the
  optional recovery policy temporarily uses DHCP so the device can be repaired
  remotely. The desired static configuration remains persisted and visible.
- Configuration storage formats 1 and 2 are migrated to format 3 with safe
  DHCP defaults, preserving compatibility with devices upgraded in the field.
- Shadow writes are versioned. The API uses optimistic locking and the device
  applies only a newer `configVersion`, preventing an older retained/update
  message from silently overwriting the active configuration.

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
- Ordinary updates must be newer than the confirmed baseline. An owner may
  select an older signed and compatible release only through an audited AWS Job
  containing `rollbackAuthorized=1` and a matching `rollbackFromVersion`, and
  only when local `allowRollback` is enabled.
- `firmware.rollback` is a separate immediate recovery operation limited to the
  authenticated previous image already retained in the secondary slot.
- AWS Job completion is retained across reset and reported only after
  confirmation, rejection, or rollback.
- `jobs.cancel` aborts only a matching active execution and reports completion
  through `cmd/result`; the management application exposes an intermediate
  `CANCEL_REQUESTED` state.

Jobs remain compatible with non-executable `service-file` staging. The
firmware signing private key is never stored in the repository or device; only
the public P-256 trust anchor is compiled into the boot chain. The complete
format, release commands, state machine, and hardware evidence are documented
in `docs/FULL_OTA.md`.

## Remaining production deliverables

### Middleware lifecycle

The security migration is complete: the committed tree builds Mbed TLS 3.6.7
LTS and FreeRTOS Kernel 11.3.0. Remaining work is operational rather than a
missing feature:

1. generate an SBOM/component manifest for every release;
2. monitor upstream advisories and define CVE response deadlines;
3. update only through a dedicated branch preserving the Cube/LwIP adapters;
4. repeat TLS, Fleet Provisioning, Jobs, Shadow, reconnect, OTA, power-fault
   and long-run regression after every middleware update;
5. migrate to a supported FreeRTOS MPU port only if per-task isolation is a
   product requirement.

Version audit and import provenance are documented in
`MIDDLEWARE_MIGRATION.md`.

### Boot-chain deployment controls

The OTA software path is implemented. Production manufacturing must still
write-protect the bootloader, apply the approved RDP/PCROP/WRP option-byte
policy, move release signing to an HSM or protected CI service, define signing
key rotation/revocation, and execute repeated power-cut qualification at every
Flash transition. These controls deliberately remain outside normal
application firmware.

### Storage, hardware and operations

The following items cannot be closed by compiling the application alone:

- choose dual-sector outbox or FRAM when loss of queued data during compaction
  is unacceptable;
- implement and calibrate real current, humidity, signal and product-specific
  sensor drivers;
- decide whether RDP1 is sufficient or a secure element is required for the
  device private key;
- qualify Flash endurance and power-cut recovery with production voltage,
  temperature and update sizes;
- configure AWS monitoring, log retention, database backup, cost alerts and an
  on-call incident path;
- record option bytes, firmware hash, certificate identity and manufacturing
  serial for each released unit.

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

## Sensor extensibility boundary

Production builds use the bounded registry described in `docs/SENSOR_REGISTRY.md`.
Only compiled and explicitly registered drivers may read hardware. Runtime bus
scanning must be implemented by the board layer with bounded timeouts; an
unknown address is reported as `UNSUPPORTED` and is never interpreted as a
valid measurement. Stable slots, capability validation and schema v2 dynamic
telemetry allow different product variants to share the same cloud and UI
without permitting runtime code loading or unbounded payloads.
