#!/usr/bin/env node

import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

const TARGET_ID = 0x07530001;
const HEADER_SIZE = 1024;
const MAX_PAYLOAD_SIZE = 512 * 1024 - HEADER_SIZE;
const SIGNING_CONTEXT = Buffer.from("STM32H753-OTA3", "utf8");

function argument(name) {
  const index = process.argv.indexOf(name);
  if (index === -1 || index + 1 >= process.argv.length) {
    throw new Error(`Missing ${name}.`);
  }
  return process.argv[index + 1];
}

const inputPath = resolve(argument("--input"));
const messagePath = resolve(argument("--message-output"));
const manifestPath = resolve(argument("--manifest-output"));
const version = Number(argument("--version"));
if (!Number.isSafeInteger(version) || version <= 0 || version > 0xffffffff) {
  throw new Error("Firmware version must be a positive uint32.");
}

const payload = await readFile(inputPath);
if (payload.length < 8 || payload.length > MAX_PAYLOAD_SIZE) {
  throw new Error(`Application size ${payload.length} is outside the OTA slot.`);
}
const payloadSha256 = createHash("sha256").update(payload).digest();
const fields = Buffer.alloc(12);
fields.writeUInt32BE(TARGET_ID, 0);
fields.writeUInt32BE(version, 4);
fields.writeUInt32BE(payload.length, 8);
const message = Buffer.concat([SIGNING_CONTEXT, fields, payloadSha256]);

await mkdir(dirname(messagePath), { recursive: true });
await mkdir(dirname(manifestPath), { recursive: true });
await writeFile(messagePath, message);
await writeFile(
  manifestPath,
  `${JSON.stringify(
    {
      schemaVersion: 1,
      algorithm: "ECDSA_SHA_256",
      signatureEncoding: "DER-or-IEEE-P1363",
      targetId: TARGET_ID,
      version,
      payloadSize: payload.length,
      payloadSha256: payloadSha256.toString("hex"),
      signingMessageSha256: createHash("sha256").update(message).digest("hex"),
    },
    null,
    2,
  )}\n`,
);

console.log(`Signing request: ${messagePath}`);
console.log(`Signing manifest: ${manifestPath}`);
