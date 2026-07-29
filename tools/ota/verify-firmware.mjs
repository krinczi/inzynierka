#!/usr/bin/env node

import { createHash, createPublicKey, verify } from "node:crypto";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HEADER_SIZE = 1024;
const IMAGE_MAGIC = 0x3341544f;
const IMAGE_FORMAT = 1;
const TARGET_ID = 0x07530001;
const PRIMARY_VECTOR_ADDRESS = 0x08020400;
const SIGNING_CONTEXT = Buffer.from("STM32H753-OTA3", "utf8");

function usage() {
  console.error(
    "Usage: node verify-firmware.mjs --firmware <image.ota.bin> " +
      "[--key <public-or-private.pem>] " +
      "[--trust-anchor-header <firmware_trust_anchor_config.h>]",
  );
  process.exit(2);
}

function argument(name, required = true) {
  const index = process.argv.indexOf(name);
  if (index === -1) {
    if (!required) {
      return undefined;
    }
    usage();
  }
  if (index + 1 >= process.argv.length) {
    usage();
  }
  return process.argv[index + 1];
}

function base64Url(bytes) {
  return bytes
    .toString("base64")
    .replaceAll("+", "-")
    .replaceAll("/", "_")
    .replace(/=+$/u, "");
}

async function loadVerificationKey() {
  const keyArgument = argument("--key", false);
  if (keyArgument) {
    return createPublicKey(await readFile(resolve(keyArgument)));
  }

  const defaultHeader = fileURLToPath(
    new URL(
      "../../Core/Inc/firmware_trust_anchor_config.h",
      import.meta.url,
    ),
  );
  const headerPath = resolve(
    argument("--trust-anchor-header", false) ?? defaultHeader,
  );
  const source = await readFile(headerPath, "utf8");
  const configured = source.match(
    /#define\s+FIRMWARE_TRUST_ANCHOR_CONFIGURED\s+1U/u,
  );
  const raw = Buffer.from(
    [...source.matchAll(/0x([0-9a-f]{2})U/giu)].map((match) =>
      Number.parseInt(match[1], 16),
    ),
  );
  if (!configured || raw.length !== 65 || raw[0] !== 0x04) {
    throw new Error("Configured raw P-256 public trust anchor is invalid.");
  }

  return createPublicKey({
    key: {
      kty: "EC",
      crv: "P-256",
      x: base64Url(raw.subarray(1, 33)),
      y: base64Url(raw.subarray(33, 65)),
    },
    format: "jwk",
  });
}

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const value of bytes) {
    crc ^= value;
    for (let bit = 0; bit < 8; bit += 1) {
      const mask = -(crc & 1);
      crc = (crc >>> 1) ^ (0xedb88320 & mask);
    }
  }
  return (~crc) >>> 0;
}

function isValidInitialMsp(value) {
  const inKnownRam =
    (value > 0x20000000 && value <= 0x20020000) ||
    (value > 0x24000000 && value <= 0x24080000) ||
    (value > 0x30000000 && value <= 0x30048000) ||
    (value > 0x38000000 && value <= 0x38010000);
  return inKnownRam && (value & 0x7) === 0;
}

const firmwarePath = resolve(argument("--firmware"));
const image = await readFile(firmwarePath);
const key = await loadVerificationKey();

if (image.length < HEADER_SIZE + 8) {
  throw new Error("OTA package is truncated.");
}
const header = image.subarray(0, HEADER_SIZE);
const payload = image.subarray(HEADER_SIZE);
const version = header.readUInt32LE(12);
const imageSize = header.readUInt32LE(16);
if (
  header.readUInt32LE(0) !== IMAGE_MAGIC ||
  header.readUInt16LE(4) !== IMAGE_FORMAT ||
  header.readUInt16LE(6) !== HEADER_SIZE ||
  header.readUInt32LE(8) !== TARGET_ID ||
  version === 0 ||
  imageSize !== payload.length ||
  header.readUInt32LE(20) !== HEADER_SIZE ||
  header.readUInt32LE(24) !== PRIMARY_VECTOR_ADDRESS ||
  header.readUInt32LE(HEADER_SIZE - 4) !==
    crc32(header.subarray(0, HEADER_SIZE - 4))
) {
  throw new Error("OTA package header is invalid.");
}

const initialMsp = payload.readUInt32LE(0);
const resetHandler = payload.readUInt32LE(4);
if (
  !isValidInitialMsp(initialMsp) ||
  (resetHandler & 1) === 0 ||
  resetHandler < PRIMARY_VECTOR_ADDRESS ||
  resetHandler >= PRIMARY_VECTOR_ADDRESS + imageSize
) {
  throw new Error("Application vector table is invalid.");
}

const expectedPayloadSha256 = header.subarray(32, 64);
const actualPayloadSha256 = createHash("sha256").update(payload).digest();
if (!actualPayloadSha256.equals(expectedPayloadSha256)) {
  throw new Error("Firmware payload SHA-256 mismatch.");
}

const fields = Buffer.alloc(12);
fields.writeUInt32BE(TARGET_ID, 0);
fields.writeUInt32BE(version, 4);
fields.writeUInt32BE(imageSize, 8);
const signedMessage = Buffer.concat([
  SIGNING_CONTEXT,
  fields,
  expectedPayloadSha256,
]);
if (
  !verify(
    "sha256",
    signedMessage,
    { key, dsaEncoding: "ieee-p1363" },
    header.subarray(64, 128),
  )
) {
  throw new Error("Firmware ECDSA-P256 signature is invalid.");
}

console.log("OTA package verification: OK");
console.log(`Firmware version: ${version}`);
console.log(`Payload bytes: ${imageSize}`);
console.log(`Payload SHA-256: ${actualPayloadSha256.toString("hex")}`);
