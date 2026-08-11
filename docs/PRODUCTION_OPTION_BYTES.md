# STM32H753 production option-byte profile

## Current development profile

The connected NUCLEO-H753ZI was audited read-only with STM32CubeProgrammer
2.17.0. Its current profile is intentionally suitable for development:

- RDP level 0 (`RDP=0xAA`);
- all sectors writable (`nWRP0..nWRP15=1`);
- PCROP disabled in both banks;
- SWD remains available.

Reproduce the audit without changing the target:

```powershell
.\tools\security\audit-option-bytes.ps1 `
  -ProgrammerCli '<path-to-STM32_Programmer_CLI.exe>' `
  -StLinkSerial '<exact-serial>' `
  -ExpectedProfile Development
```

## Approved production profile

For this memory layout the baseline production profile is:

- RDP level 1 (`RDP=0xBB`) to prevent external Flash/RAM reads and ordinary
  debug access;
- Bank 1 sector 0 write-protected (`nWRP0=0`) because it contains the immutable
  bootloader;
- sectors 1..15 writable (`nWRP1..nWRP15=1`) because OTA swap, credentials,
  event journal and configuration legitimately update them;
- PCROP disabled until executable-only sections have been separated from
  constants read as data.

PCROP is not a generic stronger RDP switch. On STM32H7 it makes the protected
range execute-only. Applying it to the complete bootloader sector can break
literal pools, image descriptors and public-key reads. It must not be enabled
without a dedicated linker section and a boot test of every recovery path.

## Irreversible-operation gate

Do not program production option bytes until all conditions are true:

1. a signed factory/recovery image matching the released source is archived;
2. normal boot, OTA installation, rollback and hardware-reset recovery pass;
3. claim and firmware-signing key incident procedures are approved;
4. the middleware security migration has passed regression testing;
5. the exact board serial and option-byte readback are recorded;
6. a separate operator explicitly approves the programming operation.

RDP level 2 is forbidden for this project. Moving from RDP1 back to RDP0 causes
a mass erase, so RDP1 must also be treated as a manufacturing transition, not a
normal development setting.

After production programming, run the audit with
`-ExpectedProfile Production`. The audit is read-only; this repository
intentionally contains no unattended command that writes option bytes.
