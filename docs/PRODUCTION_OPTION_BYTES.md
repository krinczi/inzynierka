# STM32H753 production option-byte profile

Document status: **approved profile, not an unattended programming procedure;
reviewed 2026-08-24**.

## Current development profile

The development NUCLEO-H753ZI used during the recorded audit was inspected
read-only with STM32CubeProgrammer 2.17.0. That evidence applies only to the
recorded board and audit time; every physical unit must be read back again
before classification. The development profile is:

- RDP level 0 (`RDP=0xAA`);
- all sectors writable (`nWRP0..nWRP15=1`);
- PCROP disabled in both banks;
- SWD remains available.

Firmware startup also reports the observed RDP level, debugger attachment and
MPU state. This diagnostic is evidence, not enforcement: the application never
programs option bytes.

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

RDP1 reduces straightforward debug/readout access but does not encrypt Flash
and is not equivalent to a secure element. Its use must be combined with
bootloader signature verification, protected release keys and an incident
procedure for device certificates.

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

## Post-programming acceptance

For each locked unit record:

1. MCU UID, board serial and ST-LINK serial used during manufacturing;
2. full option-byte readback and CubeProgrammer version;
3. bootloader and application hashes plus signed image version;
4. successful boot with SWD behavior matching the approved profile;
5. TLS/MQTT connection using the unique device certificate;
6. signed OTA install, healthy confirmation and rollback recovery;
7. power-cycle recovery and absence of the production self-test trigger.

Failure at any step quarantines the unit. Do not attempt ad-hoc option-byte
changes on a provisioned production device, because an RDP transition can mass
erase credentials and recovery images.
