# Protected OTA release signing

Operational design status: **device-side verification implemented; production
HSM/protected-CI integration remains environment-specific, reviewed
2026-08-24**.

The production signing key must not be present in this repository, firmware,
developer environment variables, build logs, or downloadable CI artifacts.

The device and bootloader contain only the public P-256 trust anchor. A package
signature authorizes the publisher and exact target/version/payload tuple; it
does not independently authorize a device downgrade. Downgrade authorization is
a separate owner/AWS Job/device-policy decision documented in `FULL_OTA.md`.

## Split build/sign/assemble flow

1. Build and independently test `Release/application-payload.bin`.
2. Generate the canonical signing message:

```powershell
node tools/ota/prepare-signing-request.mjs `
  --input Release/application-payload.bin `
  --version 42 `
  --message-output Release/signing-message.bin `
  --manifest-output Release/signing-manifest.json
```

3. Send only `signing-message.bin` to an ECDSA P-256 HSM/KMS signing operation
   using SHA-256. The HSM returns a DER or 64-byte IEEE-P1363 signature; its
   private key remains non-exportable.
4. The protected release job assembles the package and verifies it against the
   public trust anchor. The current local packager remains development-only
   because it accepts a private PEM file.
5. Publish the package, manifest, package SHA-256, image version, source commit,
   signer key ID and CI provenance. Do not publish the raw signing credential.

Before upload, the release job must run `verify-firmware.mjs` with the same
public trust anchor compiled into both application and bootloader. The package
digest stored in object metadata/release manifest must match the digest placed
in the AWS Job document.

## CI controls

- use an isolated protected release environment with two-person approval;
- obtain cloud credentials through short-lived OIDC, never long-lived secrets;
- restrict the signing role to the single asymmetric signing key;
- allow only `Sign` and `GetPublicKey`, not key export or key creation;
- bind releases to protected tags and monotonically increasing image versions;
- retain immutable audit logs and revoke the release if verification differs;
- rotate the public trust anchor through a separately signed transition image.

## Key lifecycle and incident response

Maintain at least these records outside the repository:

- immutable key identifier and public-key fingerprint;
- creation, activation, rotation and planned retirement dates;
- persons/roles allowed to approve signing;
- every signed image version, digest, target and source commit;
- incident decision for stopping Jobs, disabling the signer and distributing a
  trust-anchor transition.

If the private signing key may be compromised, stop creating firmware Jobs
before rotating anything. Disabling the signer prevents new malicious
signatures but cannot make already signed vulnerable images disappear. Devices
must receive a trusted transition/revocation policy while the old chain is
still accepted. Removing the old trust anchor without a recovery plan can make
field updates impossible.

The firmware and bootloader already verify the fixed OTA3 canonical message.
HSM integration changes the signer backend, not the on-device verification
format.
