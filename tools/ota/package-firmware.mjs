#!/usr/bin/env node

import {
  createHash,
  createPrivateKey,
  createPublicKey,
  sign,
  verify,
} from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

const HEADER_SIZE = 1024;
const IMAGE_MAGIC = 0x3341544f;
const IMAGE_FORMAT = 1;
const TARGET_ID = 0x07530001;
const PRIMARY_VECTOR_ADDRESS = 0x08020400;
const MAX_PAYLOAD_SIZE = 512 * 1024 - HEADER_SIZE;
const SIGNING_CONTEXT = Buffer.from("STM32H753-OTA3", "utf8");

function usage() {
  console.error(
    "Usage: node package-firmware.mjs --input <application.bin> " +
      "--key <private.pem> --version <positive integer> --output <image.ota.bin> " +
      "[--job-output <job-fragment.json>]",
  );
  process.exit(2);
}

function argument(name, fallback = undefined) {
  const index = process.argv.indexOf(name);
  if (index === -1) {
    return fallback;
  }
  if (index + 1 >= process.argv.length) {
    usage();
  }
  return process.argv[index + 1];
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

function buildSignedMessage(version, imageSize, payloadSha256) {
  const fields = Buffer.alloc(12);
  fields.writeUInt32BE(TARGET_ID, 0);
  fields.writeUInt32BE(version, 4);
  fields.writeUInt32BE(imageSize, 8);
  return Buffer.concat([SIGNING_CONTEXT, fields, payloadSha256]);
}

function isValidInitialMsp(value) {
  const inKnownRam =
    (value > 0x20000000 && value <= 0x20020000) ||
    (value > 0x24000000 && value <= 0x24080000) ||
    (value > 0x30000000 && value <= 0x30048000) ||
    (value > 0x38000000 && value <= 0x38010000);
  return inKnownRam && (value & 0x7) === 0;
}

const inputArgument = argument("--input");
const keyArgument = argument("--key");
const outputArgument = argument("--output");
const versionText = argument("--version");
if (!inputArgument || !keyArgument || !outputArgument || !versionText) {
  usage();
}

const version = Number(versionText);
if (!Number.isSafeInteger(version) || version <= 0 || version > 0xffffffff) {
  throw new Error("Firmware version must be an integer from 1 to 4294967295.");
}

const inputPath = resolve(inputArgument);
const keyPath = resolve(keyArgument);
const outputPath = resolve(outputArgument);
const jobOutputPath = resolve(
  argument("--job-output", `${outputPath}.job.json`),
);
const payload = await readFile(inputPath);
if (payload.length < 8 || payload.length > MAX_PAYLOAD_SIZE) {
  throw new Error(
    `Application binary size ${payload.length} is outside 8..${MAX_PAYLOAD_SIZE}.`,
  );
}

const initialMsp = payload.readUInt32LE(0);
const resetHandler = payload.readUInt32LE(4);
if (!isValidInitialMsp(initialMsp)) {
  throw new Error(
    `Invalid application MSP 0x${initialMsp.toString(16).padStart(8, "0")}.`,
  );
}
if (
  (resetHandler & 1) === 0 ||
  resetHandler < PRIMARY_VECTOR_ADDRESS ||
  resetHandler >= 0x080a0000
) {
  throw new Error(
    `Invalid reset handler 0x${resetHandler.toString(16).padStart(8, "0")}.`,
  );
}

const privateKey = createPrivateKey(await readFile(keyPath));
if (
  privateKey.asymmetricKeyType !== "ec" ||
  privateKey.asymmetricKeyDetails?.namedCurve !== "prime256v1"
) {
  throw new Error("The signing key must be an ECDSA P-256 private key.");
}

const publicKey = createPublicKey(privateKey);
const payloadSha256 = createHash("sha256").update(payload).digest();
const signedMessage = buildSignedMessage(
  version,
  payload.length,
  payloadSha256,
);
const signature = sign("sha256", signedMessage, {
  key: privateKey,
  dsaEncoding: "ieee-p1363",
});
if (
  signature.length !== 64 ||
  !verify(
    "sha256",
    signedMessage,
    { key: publicKey, dsaEncoding: "ieee-p1363" },
    signature,
  )
) {
  throw new Error("Generated firmware signature did not verify.");
}

const header = Buffer.alloc(HEADER_SIZE, 0);
header.writeUInt32LE(IMAGE_MAGIC, 0);
header.writeUInt16LE(IMAGE_FORMAT, 4);
header.writeUInt16LE(HEADER_SIZE, 6);
header.writeUInt32LE(TARGET_ID, 8);
header.writeUInt32LE(version, 12);
header.writeUInt32LE(payload.length, 16);
header.writeUInt32LE(HEADER_SIZE, 20);
header.writeUInt32LE(PRIMARY_VECTOR_ADDRESS, 24);
header.writeUInt32LE(0, 28);
payloadSha256.copy(header, 32);
signature.copy(header, 64);
header.writeUInt32LE(crc32(header.subarray(0, HEADER_SIZE - 4)), HEADER_SIZE - 4);

const packageBytes = Buffer.concat([header, payload]);
const packageSha256 = createHash("sha256").update(packageBytes).digest("hex");
await mkdir(dirname(outputPath), { recursive: true });
await mkdir(dirname(jobOutputPath), { recursive: true });
await writeFile(outputPath, packageBytes);
await writeFile(
  jobOutputPath,
  `${JSON.stringify(
    {
      schemaVersion: 1,
      operation: "stage-file",
      mode: "download",
      artifact: {
        artifactType: "firmware",
        version,
        size: packageBytes.length,
        sha256: packageSha256,
        url: "https://REPLACE_WITH_SHORT_LIVED_ARTIFACT_URL",
      },
    },
    null,
    2,
  )}\n`,
);

console.log(`Signed OTA package: ${outputPath}`);
console.log(`Firmware version: ${version}`);
console.log(`Payload bytes: ${payload.length}`);
console.log(`Package bytes: ${packageBytes.length}`);
console.log(`Package SHA-256: ${packageSha256}`);
console.log(`AWS Jobs document template: ${jobOutputPath}`);
