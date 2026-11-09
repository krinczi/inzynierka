#!/usr/bin/env node

import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

const CONTROL_LOG_SIZE = 128 * 1024;
const CONTROL_RECORD_SIZE = 512;
const CONTROL_MAGIC = 0x4f544132;
const CONTROL_FORMAT = 2;
const CONTROL_COMMITTED = 0xc01db007;

function usage() {
  console.error(
    "Usage: node append-control-record.mjs --input <control-log.bin> " +
      "--record <control-record.bin> --output <updated-control-log.bin>",
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

const inputPath = resolve(argument("--input"));
const recordPath = resolve(argument("--record"));
const outputPath = resolve(argument("--output"));
const controlLog = await readFile(inputPath);
const record = await readFile(recordPath);

if (controlLog.length !== CONTROL_LOG_SIZE) {
  throw new Error(
    `Control log must contain exactly ${CONTROL_LOG_SIZE} bytes.`,
  );
}
if (record.length !== CONTROL_RECORD_SIZE) {
  throw new Error(
    `Control record must contain exactly ${CONTROL_RECORD_SIZE} bytes.`,
  );
}
if (
  record.readUInt32LE(0) !== CONTROL_MAGIC ||
  record.readUInt16LE(4) !== CONTROL_FORMAT ||
  record.readUInt16LE(6) !== CONTROL_RECORD_SIZE ||
  record.readUInt32LE(CONTROL_RECORD_SIZE - 4) !== CONTROL_COMMITTED ||
  record.readUInt32LE(CONTROL_RECORD_SIZE - 8) !==
    crc32(record.subarray(0, CONTROL_RECORD_SIZE - 8))
) {
  throw new Error("Control record header, CRC or commit marker is invalid.");
}

let targetIndex = -1;
for (
  let index = 0;
  index < CONTROL_LOG_SIZE / CONTROL_RECORD_SIZE;
  index += 1
) {
  const offset = index * CONTROL_RECORD_SIZE;
  const slot = controlLog.subarray(offset, offset + CONTROL_RECORD_SIZE);
  if (slot.every((value) => value === 0xff)) {
    targetIndex = index;
    break;
  }
}
if (targetIndex < 0) {
  throw new Error("Control log does not contain an erased record slot.");
}

const output = Buffer.from(controlLog);
record.copy(output, targetIndex * CONTROL_RECORD_SIZE);
await mkdir(dirname(outputPath), { recursive: true });
await writeFile(outputPath, output);

console.log(`Control record appended at index ${targetIndex}.`);
console.log(`Sequence: ${record.readUInt32LE(8)}`);
console.log(`Updated control log: ${outputPath}`);
