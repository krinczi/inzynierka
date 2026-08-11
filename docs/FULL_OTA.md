# Signed dual-slot OTA

Status: implemented and verified on STM32H753 hardware.

The OTA chain downloads a signed package through AWS IoT Jobs and HTTPS,
validates it in the application, installs it through a separate bootloader,
boots it as a trial image, and either confirms it or automatically restores the
previous image. A downloaded file is never executed only because its transport
hash is correct.

## Trust and update flow

1. AWS IoT Jobs supplies an HTTPS URL, the complete package size, package
   SHA-256, artifact type `firmware`, and a monotonically increasing version.
2. The application downloads the package into the secondary slot. HTTPS uses
   TLS 1.2 with hostname and server-certificate verification.
3. The application checks the transport size and SHA-256, then validates the
   embedded OTA header, target, vector table, payload SHA-256, and ECDSA-P256
   signature.
4. The application records `CANDIDATE_READY` and resets the MCU.
5. The bootloader verifies the candidate again and rejects versions not greater
   than the confirmed anti-rollback floor.
6. The bootloader swaps four sectors using a dedicated scratch sector. Every
   completed sub-step is appended to the control journal, so an interrupted
   swap resumes instead of guessing the Flash state.
7. The new primary image starts in `TRIAL`. The application confirms it only
   after persistent local services and the independent watchdog are operating,
   and after the MQTT session has remained healthy for at least 30 seconds.
   A trial that cannot establish stable cloud communication is not promoted to
   the confirmed anti-rollback baseline.
8. A confirmed trial advances the monotonic version floor. If the application
   fails to confirm in three boots, the bootloader swaps the previous image back
   and records a rollback result for later AWS Jobs reporting.

Invalid signatures, hashes, vector tables, target identifiers, and downgrade
attempts fail closed. The known-good primary image remains bootable.

## Flash layout

STM32H753ZI has two 1 MiB Flash banks with 128 KiB sectors.

| Range | Size | Purpose |
| --- | ---: | --- |
| `0x08000000-0x0801FFFF` | 128 KiB | Bootloader |
| `0x08020000-0x0809FFFF` | 512 KiB | Primary signed-image slot |
| `0x080A0000-0x080BFFFF` | 128 KiB | Durable critical-event journal |
| `0x080C0000-0x080DFFFF` | 128 KiB | Credential slot A |
| `0x080E0000-0x080FFFFF` | 128 KiB | Credential slot B |
| `0x08100000-0x0811FFFF` | 128 KiB | Append-only OTA control journal |
| `0x08120000-0x0819FFFF` | 512 KiB | Secondary signed-image slot |
| `0x081A0000-0x081BFFFF` | 128 KiB | Swap scratch and journal backup |
| `0x081C0000-0x081DFFFF` | 128 KiB | Configuration slot A |
| `0x081E0000-0x081FFFFF` | 128 KiB | Configuration slot B |

Each signed slot starts with a 1024-byte `OTA3` header. The application vector
table and payload begin at `0x08020400` in the primary slot and `0x08120400` in
the secondary slot. The linker rejects an application exceeding
`512 KiB - 1024 bytes`.

The bootloader executes its Flash-changing routines from RAM. This is required
while Bank 1 sectors containing the primary application are erased or
programmed.

## Signed package format

The fixed 1024-byte header contains:

- magic and header format;
- STM32H753 target identifier;
- image version and payload size;
- application load/vector address;
- payload SHA-256;
- raw ECDSA-P256 signature (`r || s`, 64 bytes);
- header CRC-32.

The signature is calculated over this canonical digest:

```text
SHA256(
  "STM32H753-OTA3" ||
  target_id_be32 ||
  image_version_be32 ||
  payload_size_be32 ||
  payload_sha256
)
```

The embedded firmware contains only the public P-256 trust anchor. The private
signing key must remain outside the repository and device, preferably in a CI
secret store, KMS/HSM, or an offline release-signing environment. Changing the
public key requires a deliberately signed trust-anchor migration strategy.

## Build and signing

Example release flow from the repository root:

```powershell
make -C Release
make -C Bootloader
arm-none-eabi-objcopy -O binary `
  Release/inzynierkastm.elf `
  Release/application-payload.bin

node tools/ota/package-firmware.mjs `
  --input Release/application-payload.bin `
  --key <private-signing-key.pem> `
  --version 2 `
  --output Release/inzynierkastm-v2.ota.bin

node tools/ota/verify-firmware.mjs `
  --firmware Release/inzynierkastm-v2.ota.bin

node tools/ota/create-factory-image.mjs `
  --bootloader Bootloader/build/bootloader.bin `
  --application Release/inzynierkastm-v2.ota.bin `
  --output Release/factory-v2.bin
```

`package-firmware.mjs` refuses invalid versions and oversized payloads.
`verify-firmware.mjs` independently checks the package using the configured
public key. `create-factory-image.mjs` pads the complete primary slot with
`0xFF`, preventing stale bytes from a previous image from becoming part of a
factory installation.

## AWS IoT Job document

The `sha256` field below protects the complete `.ota.bin` package during
transport. The embedded header separately authenticates the firmware payload.

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

The document version and embedded signed version must match. A firmware
candidate must also be newer than the last confirmed image. Ordinary
`service-file` Jobs remain supported but are never installed as executable
firmware.

AWS Job success is intentionally deferred until the trial image is confirmed.
After reconnect, the device reports one of:

- `SUCCEEDED` / `firmware_confirmed`;
- `FAILED` / `trial_health_failed` after automatic rollback;
- `REJECTED` / `candidate_rejected` for an invalid or non-installable image.

## Recovery guarantees

- The bootloader verifies every image before jumping to it.
- The previous image is retained in the secondary slot after a forward swap.
- Swap progress is recorded after each
  `primary -> scratch -> secondary -> primary` sector operation.
- A reset or power loss resumes from the last committed journal record.
- Control records use sequence numbers, CRC-32, read-back verification, and a
  commit marker written last.
- Before an installation, the journal reserves enough records for a complete
  swap. Compaction backs up the last stable record in the scratch sector before
  erasing the control sector.
- The bootloader refreshes the independent watchdog during hashing and Flash
  operations.
- The first signed factory image creates the initial confirmed version floor.

## Hardware verification performed

The implementation was verified on the target board without changing AWS:

1. A signed factory image version 1 booted through the bootloader.
2. A signed version 2 package was placed in the secondary slot.
3. The bootloader completed all four sector swaps and the application confirmed
   version 2.
4. The primary header reported version 2 and the secondary header retained
   version 1.
5. An intentionally unconfirmed trial reached the attempt limit.
6. The bootloader automatically restored version 1 and retained version 2 in
   the secondary slot.
7. The final board state was restored to a clean, signed, confirmed version 1
   image with the anti-rollback floor set to 1.

These tests cover installation, confirmation, rollback, slot exchange, and
control-journal state transitions. Release qualification should additionally
run repeated power-cut fault injection at every erase/program transition and a
long-duration update campaign.

The repository includes a guarded reset-recovery harness:

```powershell
.\tools\ota\test-reset-recovery.ps1 `
  -ProgrammerCli '<path-to-STM32_Programmer_CLI.exe>' `
  -StLinkSerial '<serial>' `
  -ConfirmStLinkSerial '<serial>'
```

Its default mode is a dry run. `-ExecuteResets` requires PowerShell
confirmation, targets one exact ST-LINK serial number, performs hardware
resets, reads the OTA control journal without programming it, and validates
every snapshot with `inspect-control-log.mjs`. Run it only while a newly signed
candidate matching the current source is active. A hardware reset verifies
reset recovery; qualification against an actual loss of power additionally
requires a controllable power supply or USB relay.

## Manufacturing and operations

The software boot chain is complete, but deployment security also requires:

- write-protecting the bootloader sector with option bytes;
- enabling an appropriate RDP/PCROP/WRP policy after a tested recovery plan;
- keeping the private signing key outside developer workstations in production;
- issuing strictly increasing firmware versions;
- retaining and testing a signed recovery image;
- monitoring rejected candidates and repeated rollbacks;
- defining public-key rotation and signing-key revocation procedures.

Option bytes are not changed automatically by this firmware because a wrong
production lock configuration can make development recovery impossible.
