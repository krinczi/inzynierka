# Middleware security baseline

## Locked versions

| Component | Version | Official release | Source archive SHA-256 |
| --- | --- | --- | --- |
| Mbed TLS | 3.6.7 LTS | `https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7` | `A7E8BCBEC0E6F761B4AF24F25677626B35F762F68EEF79C08677A363212D11F6` |
| FreeRTOS Kernel | 11.3.0 | `https://github.com/FreeRTOS/FreeRTOS-Kernel/releases/tag/V11.3.0` | `9EF2600D5F9DD009D29AE80588B8987DA5A25F626332D054900E60514868730F` |

The hashes above apply to the release archives downloaded from the official
release pages. They are provenance records, not hashes of the modified vendor
trees committed to this repository.

## Imported scope

- Mbed TLS `include/` and `library/` were replaced with the 3.6.7 LTS release.
- Upstream `library/net_sockets.c` is intentionally excluded. The firmware uses
  the LwIP adapter in `MBEDTLS/App/net_sockets.c`.
- FreeRTOS kernel sources, public headers, GCC ARM CM4F port and `heap_4.c` were
  replaced with 11.3.0.
- The STM32 CMSIS-RTOS v2 adaptation layer is retained because it is generated
  for this CubeIDE project and is not supplied by the kernel release archive.
- The bootloader links only the Mbed TLS modules needed for ECDSA P-256 and
  SHA-256 verification.

## Compatibility changes

- Mbed TLS 3 public build and entropy headers replace deprecated includes.
- SHA-256 calls use local compatibility wrappers, allowing the application and
  bootloader to share one source-level API.
- TLS code no longer reads the private `mbedtls_ssl_context.state` member.
- Fleet key parsing and key-pair verification provide an explicit hardware RNG
  callback required by Mbed TLS 3.
- The hardware entropy adapter copies complete RNG words and returns an error
  instead of invoking a permanent error loop.
- `mbedtls_ms_time()` is backed by `HAL_GetTick()`.
- The CMSIS wrapper uses the FreeRTOS 11 stack-overflow hook signature.

## Verification gate

Run from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File tools/middleware/audit-versions.ps1
```

Then build both targets:

1. CubeIDE `Release` application image.
2. `Bootloader/Makefile` target `clean all`.

The migration is acceptable only if both builds complete, the application
fits its slot, the bootloader fits sector 0, and the signed hardware regression
passes TLS, MQTT, Fleet Provisioning, Device Shadow, Jobs HTTPS and OTA boot
confirmation. A successful host build alone does not authorize flashing an
unsigned application into a secure-boot layout.

## Update procedure

For each later security update:

1. download only an official tagged release;
2. verify and record its archive SHA-256;
3. import the same scoped directories and preserve the documented adapters;
4. review the upstream security and migration notes;
5. run static builds and the complete signed hardware regression;
6. publish a signed OTA image through the protected release process;
7. retain the previous confirmed image until the new image reports healthy.

