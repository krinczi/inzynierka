# Protected OTA release signing

The production signing key must not be present in this repository, firmware,
developer environment variables, build logs, or downloadable CI artifacts.

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

## CI controls

- use an isolated protected release environment with two-person approval;
- obtain cloud credentials through short-lived OIDC, never long-lived secrets;
- restrict the signing role to the single asymmetric signing key;
- allow only `Sign` and `GetPublicKey`, not key export or key creation;
- bind releases to protected tags and monotonically increasing image versions;
- retain immutable audit logs and revoke the release if verification differs;
- rotate the public trust anchor through a separately signed transition image.

The firmware and bootloader already verify the fixed OTA3 canonical message.
HSM integration changes the signer backend, not the on-device verification
format.
