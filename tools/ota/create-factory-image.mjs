#!/usr/bin/env node

import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

const PRIMARY_SLOT_OFFSET = 0x20000;
const BOOTLOADER_MAX_SIZE = PRIMARY_SLOT_OFFSET;
const PRIMARY_SLOT_SIZE = 512 * 1024;
const OTA_HEADER_MAGIC = 0x3341544f;

function usage() {
  console.error(
    "Usage: node create-factory-image.mjs --bootloader <bootloader.bin> " +
      "--firmware <image.ota.bin> --output <factory.bin>",
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

const bootloaderPath = resolve(argument("--bootloader"));
const firmwarePath = resolve(argument("--firmware"));
const outputPath = resolve(argument("--output"));
const bootloader = await readFile(bootloaderPath);
const firmware = await readFile(firmwarePath);

if (bootloader.length === 0 || bootloader.length > BOOTLOADER_MAX_SIZE) {
  throw new Error("Bootloader does not fit in Bank 1 sector 0.");
}
if (firmware.length < 1024 || firmware.readUInt32LE(0) !== OTA_HEADER_MAGIC) {
  throw new Error("Firmware input is not an OTA3 package.");
}
if (firmware.length > PRIMARY_SLOT_SIZE) {
  throw new Error("Firmware package does not fit in the primary slot.");
}

/*
 * Include the complete primary slot. The factory programming operation then
 * removes stale bytes from every application sector, not only from sectors
 * occupied by the current (possibly smaller) image.
 */
const factory = Buffer.alloc(PRIMARY_SLOT_OFFSET + PRIMARY_SLOT_SIZE, 0xff);
bootloader.copy(factory, 0);
firmware.copy(factory, PRIMARY_SLOT_OFFSET);
await mkdir(dirname(outputPath), { recursive: true });
await writeFile(outputPath, factory);

console.log(`Factory image: ${outputPath}`);
console.log(`Factory bytes: ${factory.length}`);
console.log(`Factory SHA-256: ${createHash("sha256").update(factory).digest("hex")}`);
