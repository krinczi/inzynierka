#!/usr/bin/env node

import { readFile } from 'node:fs/promises';
import process from 'node:process';

const RECORD_SIZE = 512;
const MAGIC = 0x4f544132;
const FORMAT = 2;
const COMMITTED = 0xc01db007;

const states = new Map([
  [1, 'CONFIRMED'],
  [2, 'DOWNLOAD_IN_PROGRESS'],
  [3, 'SERVICE_FILE_READY'],
  [4, 'CANDIDATE_READY'],
  [5, 'SWAPPING'],
  [6, 'TRIAL'],
  [7, 'ROLLBACK'],
  [8, 'REJECTED'],
]);

function argument(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

function crc32(buffer) {
  let crc = 0xffffffff;
  for (const byte of buffer) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc >>> 1) ^ ((crc & 1) !== 0 ? 0xedb88320 : 0);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function parseRecord(record, index) {
  const magic = record.readUInt32LE(0);
  const format = record.readUInt16LE(4);
  const size = record.readUInt16LE(6);
  const sequence = record.readUInt32LE(8);
  const state = record.readUInt32LE(12);
  const storedCrc = record.readUInt32LE(504);
  const committed = record.readUInt32LE(508);
  const calculatedCrc = crc32(record.subarray(0, 504));
  const valid =
    magic === MAGIC &&
    format === FORMAT &&
    size === RECORD_SIZE &&
    states.has(state) &&
    committed === COMMITTED &&
    storedCrc === calculatedCrc;

  if (!valid) return null;

  return {
    index,
    sequence,
    state: states.get(state),
    swapDirection: record.readUInt32LE(16),
    swapSector: record.readUInt32LE(20),
    swapStep: record.readUInt32LE(24),
    trialBootCount: record.readUInt32LE(28),
    confirmedVersion: record.readUInt32LE(32),
    primaryVersion: record.readUInt32LE(44),
    secondaryVersion: record.readUInt32LE(152),
    jobExecutionVersion: record.readUInt32LE(317),
    flags: `0x${record.readUInt32LE(321).toString(16).padStart(8, '0')}`,
  };
}

const input = argument('--input');
if (!input) {
  console.error('Usage: node inspect-control-log.mjs --input <control-log.bin>');
  process.exit(2);
}

const data = await readFile(input);
if (data.length < RECORD_SIZE) {
  throw new Error(`Control-log dump is too short: ${data.length} byte(s)`);
}

const records = [];
const recordCount = Math.floor(data.length / RECORD_SIZE);
for (let index = 0; index < recordCount; index += 1) {
  const offset = index * RECORD_SIZE;
  const parsed = parseRecord(data.subarray(offset, offset + RECORD_SIZE), index);
  if (parsed) records.push(parsed);
}

records.sort((left, right) => left.sequence - right.sequence);
const report = {
  input,
  bytesRead: data.length,
  recordsScanned: recordCount,
  validRecords: records.length,
  latest: records.at(-1) ?? null,
};

console.log(JSON.stringify(report, null, 2));
if (!report.latest) process.exitCode = 1;
