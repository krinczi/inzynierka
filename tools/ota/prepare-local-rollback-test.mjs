#!/usr/bin/env node

import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

const IMAGE_MAGIC = 0x3341544f;
const DESCRIPTOR_MAGIC = 0x494d4732;
const CONTROL_MAGIC = 0x4f544132;
const CONTROL_FORMAT = 2;
const CONTROL_RECORD_SIZE = 512;
const CONTROL_COMMITTED = 0xc01db007;
const STATE_TRIAL = 6;

function usage() {
  console.error(
    "Usage: node prepare-local-rollback-test.mjs --active <v2.ota.bin> " +
      "--rollback <v1.ota.bin> --sequence <next sequence> --output <record.bin>",
  );
  process.exit(2);
}

function argument(name) {
  const index = process.argv.indexOf(name);
  if (index === -1 || index + 1 >= process.argv.length) {
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

function assertPackage(image, label) {
  if (
    image.length < 1032 ||
    image.readUInt32LE(0) !== IMAGE_MAGIC ||
    image.readUInt32LE(16) + 1024 !== image.length
  ) {
    throw new Error(`${label} is not a complete OTA3 package.`);
  }
}

function copyDescriptor(image, destination, offset) {
  destination.writeUInt32LE(DESCRIPTOR_MAGIC, offset);
  destination.writeUInt32LE(image.readUInt32LE(16), offset + 4);
  destination.writeUInt32LE(image.readUInt32LE(12), offset + 8);
  image.copy(destination, offset + 12, 32, 64);
  image.copy(destination, offset + 44, 64, 128);
}

const active = await readFile(resolve(argument("--active")));
const rollback = await readFile(resolve(argument("--rollback")));
const output = resolve(argument("--output"));
const sequence = Number(argument("--sequence"));
assertPackage(active, "Active image");
assertPackage(rollback, "Rollback image");
if (
  !Number.isSafeInteger(sequence) ||
  sequence <= 1 ||
  sequence > 0xffffffff
) {
  throw new Error("Sequence must be a valid monotonic control-log number.");
}

const activeVersion = active.readUInt32LE(12);
const rollbackVersion = rollback.readUInt32LE(12);
if (rollbackVersion >= activeVersion) {
  throw new Error("Rollback image must be older than the active trial image.");
}

const control = Buffer.alloc(CONTROL_RECORD_SIZE, 0);
control.writeUInt32LE(CONTROL_MAGIC, 0);
control.writeUInt16LE(CONTROL_FORMAT, 4);
control.writeUInt16LE(CONTROL_RECORD_SIZE, 6);
control.writeUInt32LE(sequence, 8);
control.writeUInt32LE(STATE_TRIAL, 12);
control.writeUInt32LE(1, 16);
control.writeUInt32LE(0, 20);
control.writeUInt32LE(0, 24);
control.writeUInt32LE(3, 28);
control.writeUInt32LE(rollbackVersion, 32);
copyDescriptor(active, control, 36);
copyDescriptor(rollback, control, 144);
control.write("local-rollback-test", 252, 65, "ascii");
control.writeUInt32LE(1, 317);
control.writeUInt32LE(1, 321);
control.writeUInt32LE(crc32(control.subarray(0, 504)), 504);
control.writeUInt32LE(CONTROL_COMMITTED, 508);

await mkdir(dirname(output), { recursive: true });
await writeFile(output, control);
console.log(
  `Rollback trigger prepared: active v${activeVersion} -> rollback v${rollbackVersion}`,
);
console.log(`Control sequence: ${sequence}`);
console.log(`Output: ${output}`);

