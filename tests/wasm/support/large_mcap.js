// SPDX-License-Identifier: MPL-2.0
const fs = require('fs');

// MCAP record writers (https://mcap.dev/spec). Little-endian; a record is opcode u8 + length u64 + body.
const u8 = value => Buffer.from([value]);
const u16 = value => { const b = Buffer.alloc(2); b.writeUInt16LE(value); return b; };
const u32 = value => { const b = Buffer.alloc(4); b.writeUInt32LE(value); return b; };
const u64 = value => { const b = Buffer.alloc(8); b.writeBigUInt64LE(BigInt(value)); return b; };
const str = value => Buffer.concat([u32(Buffer.byteLength(value)), Buffer.from(value)]);
const record = (opcode, ...fields) => {
  const body = Buffer.concat(fields);
  return Buffer.concat([u8(opcode), u64(body.length), body]);
};
const MAGIC = Buffer.from([0x89, 0x4d, 0x43, 0x41, 0x50, 0x30, 0x0d, 0x0a]);

// A valid ROS 2 MCAP of std_msgs/msg/Float64 on /ros2/value, one message per second, whose data starts past
// `holeBytes` of zeroed attachment. The file is sparse: the hole costs no disk, yet every offset the reader seeks to
// (the chunk, the summary, the footer) lies beyond it. Returns the file size.
function writeSparseRos2Float64Mcap(file, holeBytes, values) {
  const schema = record(0x03, u16(1), str('std_msgs/msg/Float64'), str('ros2msg'),
    u32(Buffer.byteLength('float64 data')), Buffer.from('float64 data'));
  const channel = record(0x04, u16(1), u16(1), str('/ros2/value'), str('cdr'), u32(0));
  const messages = values.map((value, index) => {
    const cdr = Buffer.alloc(12);
    cdr.writeUInt8(1, 1);  // CDR little-endian encapsulation
    cdr.writeDoubleLE(value, 4);
    const time = BigInt(index + 1) * 1_000_000_000n;
    return record(0x05, u16(1), u32(index), u64(time), u64(time), cdr);
  });
  const startTime = 1_000_000_000n;
  const endTime = BigInt(values.length) * 1_000_000_000n;
  const records = Buffer.concat([schema, channel, ...messages]);

  const header = record(0x01, str('ros2'), str('pj-wasm-test'));
  const attachmentFields = Buffer.concat([u64(0), u64(0), str('hole'), str('application/octet-stream'), u64(holeBytes)]);
  const attachmentPrefix = Buffer.concat([u8(0x09), u64(attachmentFields.length + holeBytes + 4), attachmentFields]);
  const attachmentStart = MAGIC.length + header.length;
  const holeStart = attachmentStart + attachmentPrefix.length;
  const afterHole = holeStart + holeBytes;

  const chunk = record(0x06, u64(startTime), u64(endTime), u64(records.length), u32(0), str(''),
    u64(records.length), records);
  const chunkStart = afterHole + 4;  // after the attachment CRC
  const dataEnd = record(0x0f, u32(0));
  const summaryStart = chunkStart + chunk.length + dataEnd.length;
  const chunkIndex = record(0x08, u64(startTime), u64(endTime), u64(chunkStart), u64(chunk.length), u32(0),
    u64(0), str(''), u64(records.length), u64(records.length));
  const statistics = record(0x0b, u64(values.length), u16(1), u32(1), u32(1), u32(0), u32(1),
    u64(startTime), u64(endTime), u32(10), u16(1), u64(values.length));
  const footer = record(0x02, u64(summaryStart), u64(0), u32(0));
  const tail = Buffer.concat([u32(0), chunk, dataEnd, schema, channel, chunkIndex, statistics, footer, MAGIC]);

  const fd = fs.openSync(file, 'w');
  try {
    fs.writeSync(fd, Buffer.concat([MAGIC, header, attachmentPrefix]), 0, undefined, 0);
    fs.writeSync(fd, tail, 0, tail.length, afterHole);
  } finally {
    fs.closeSync(fd);
  }
  return afterHole + tail.length;
}

module.exports = { writeSparseRos2Float64Mcap };
