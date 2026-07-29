# Flash staging for AWS IoT Jobs

Status: bounded HTTPS artifact download, service-file staging, and signed
dual-slot firmware OTA implemented. See `docs/FULL_OTA.md` for installation,
trial boot, anti-rollback, and automatic rollback.

## Memory layout

STM32H753ZI provides 2 MiB of internal Flash split into two 1 MiB banks with
128 KiB sectors.

| Range | Size | Purpose |
| --- | ---: | --- |
| `0x08000000-0x0801FFFF` | 128 KiB | Bootloader (Bank 1) |
| `0x08020000-0x0809FFFF` | 512 KiB | Primary signed-image slot |
| `0x080A0000-0x080BFFFF` | 128 KiB | Durable critical-event journal |
| `0x080C0000-0x080DFFFF` | 128 KiB | Credential log A (Bank 1) |
| `0x080E0000-0x080FFFFF` | 128 KiB | Credential log B (Bank 1) |
| `0x08100000-0x0811FFFF` | 128 KiB | OTA control journal (Bank 2) |
| `0x08120000-0x0819FFFF` | 512 KiB | Secondary signed-image/staging slot |
| `0x081A0000-0x081BFFFF` | 128 KiB | Swap scratch and journal backup |
| `0x081C0000-0x081DFFFF` | 128 KiB | Configuration slot A |
| `0x081E0000-0x081FFFFF` | 128 KiB | Configuration slot B |

The linker fails the build if executable sections exceed the dedicated
primary payload area (`512 KiB - 1024 bytes`).

## Safety properties

- Cloud input never selects a raw Flash address. The firmware owns the layout.
- No dynamic allocation is used.
- Data is written sequentially in native 32-byte STM32H7 Flash words.
- Every programmed Flash word is read back and compared.
- The expected file size is checked before erase and before finalization.
- SHA-256 is calculated from the programmed Bank 2 bytes after the complete
  transfer and independently recalculated during the next boot. The final
  acceptance check therefore covers transport, Flash programming, and cache
  coherency rather than trusting only an in-RAM streaming context.
- A digest mismatch has a dedicated `FLASH_STAGING_HASH_MISMATCH` result.
  Signature, image-version, anti-rollback, and primary-image validation
  failures remain `FLASH_STAGING_INTEGRITY_ERROR`. AWS therefore receives
  `sha256_mismatch` only for a real digest mismatch and
  `firmware_validation_failed` for an invalid image; only the former is
  retried.
- Metadata is protected with CRC-32.
- The metadata word containing the magic and committed state is written last.
- A power loss before metadata commit leaves no valid staged file.
- On boot, committed metadata and the complete file SHA-256 are verified.
- Application code executes from Bank 1 while download writes target Bank 2.
- Firmware packages contain a signed `OTA3` header and are revalidated by the
  bootloader before installation.
- Service files remain non-executable and are never selected by the bootloader.

## API lifecycle

1. `FlashStaging_Init()` inspects and verifies an existing committed file.
2. `FlashStaging_PrepareStart(size, sha256)` validates the transfer.
3. Repeated `FlashStaging_PrepareStep()` calls erase and verify at most one
   required sector per service iteration.
4. `FlashStaging_Write(data, length)` accepts ordered chunks of any size.
5. `FlashStaging_Finalize()` verifies and atomically commits the file.
6. `FlashStaging_Read()` exposes only a READY file.
7. `FlashStaging_Abort()` invalidates metadata; the next preparation erases
   only the sectors required by the new file.

An interrupted download is deliberately restarted from byte zero.
This provides deterministic retry without trusting partially written data.

## Hardware self-test for this stage

The firmware contains a physically triggered diagnostic test. It does not
require Ethernet, AWS, or the web application.

Persistent configuration, event queues, and Flash staging are initialized
before waiting for Ethernet. A missing link therefore cannot prevent stored
file verification. If Ethernet appears after the initial wait timeout, the
firmware configures the interface, logs a Flash staging snapshot, and starts
TLS/MQTT without requiring another reset.

1. Open the UART log.
2. Hold the blue USER button (B1).
3. While holding B1, press and release RESET. Keep B1 pressed for at least
   1.5 seconds, then release it.
4. Expect the following essential log sequence:
   - `FLASH SELFTEST: begin result=0`
   - `FLASH SELFTEST: write chunks=1+2 result=0 received=3`
   - `FLASH SELFTEST: finalize result=0 state=READY`
   - `FLASH SELFTEST: readback result=0 value=abc`
   - `FLASH SELFTEST: PASS (written, hashed and read back)`
5. Press RESET normally, without B1. During TLS initialization expect
   `FLASH STAGING: state=READY ... size=3 result=0`. This proves that metadata
   and data survive restart and are verified during initialization.
6. Trigger the self-test once more. It recognizes the existing test file,
   verifies it without an erase, and prints
   `FLASH SELFTEST: PASS (existing test file)`.

The payload is `abc`, written as deliberately non-aligned chunks of one and
two bytes. Its expected SHA-256 is the standard FIPS 180-4 test vector.

Safety guard: the self-test refuses to erase staging when a different valid
file is in state `READY`. It is disabled by the default production profile and
can be enabled explicitly with `FLASH_STAGING_BUTTON_SELF_TEST_ENABLED=1`.

Further fault-injection tests for the AWS Jobs integration should reset the
device during transfer and use an incorrect digest. After restart, an
interrupted or corrupted transfer must never be reported as `READY`.

The AWS Jobs downloader serializes Flash staging operations in the existing
MQTT service loop. It restarts an interrupted transfer from byte zero and
reports terminal status through the reserved AWS Jobs update topic.
