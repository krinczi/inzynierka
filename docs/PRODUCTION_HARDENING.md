# Production hardening status

This document separates mechanisms implemented in the application firmware
from controls that require board hardware, AWS operations, a middleware
migration, or a separate bootloader. It intentionally avoids calling secure
artifact staging a complete OTA update.

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

### Durable critical events

- Bank 1 sector 5 is reserved as a 128 KiB append-only event journal.
- `ALARM` and `CRITICAL` messages are restored after reset.
- Critical events use MQTT QoS 1.
- An event is removed only after the matching broker PUBACK and a durable ACK
  journal record.
- Journal records use a versioned header, sequence number, CRC32, read-back
  verification, and compaction.
- MQTT `eventId` remains stable across retries, allowing backend deduplication.

The internal-Flash implementation is the board-compatible fallback. A high
write-rate product should replace it with FRAM or another endurance-qualified
store through the same queue semantics.

### Credentials and security posture

- Fleet ownership tokens are removed after `RegisterThing` and migrated out of
  an existing provisioned record.
- Temporary credential buffers and Flash programming buffers are explicitly
  zeroed.
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

### Signed firmware contract

Jobs remain compatible with `service-file` staging. A job whose
`artifactType` is `firmware` must also contain a Base64 `signature` over the
artifact SHA-256 digest. The signature is verified before download. Firmware
is rejected when the public trust anchor is missing or verification fails.

The board integration supplies only a public key by overriding:

`FirmwareTrustAnchor_GetPublicKeyPem`

The offline signing private key must never be stored in this repository or on
the device.

## Separate production deliverables

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

### Complete OTA boot chain

The application securely downloads, hashes, authenticates, and stages an
artifact. A complete production OTA still requires a separately reviewed
bootloader that provides:

- immutable or protected root of trust;
- image signature verification during every boot;
- A/B installation or a recoverable swap procedure;
- monotonic anti-rollback version/counter;
- trial boot and application health confirmation;
- automatic rollback after failed confirmation;
- power-loss tests for every state transition.

Without that bootloader, the supported capability name is **secure firmware
artifact staging**, not production OTA.

## Claim-certificate operations

Fleet claim credentials are shared bootstrap credentials and therefore remain
an operational risk even when device credentials are unique. Production AWS
operations must:

- restrict the claim policy to Fleet Provisioning topics only;
- validate hardware UID in the pre-provisioning hook;
- rate-limit and alert on provisioning failures;
- inventory issued device certificates;
- rotate and revoke the claim certificate on a defined schedule;
- provide an incident procedure for immediate claim revocation.
