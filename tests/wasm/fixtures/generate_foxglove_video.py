#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the deterministic Foxglove H.264 video Scene2D fixture.

Reproduce it with:

    python3 -m pip install mcap==1.3.1 protobuf==7.35.0 av==18.1.0
    python3 tests/wasm/fixtures/generate_foxglove_video.py

The H.264 access units are the repository's own pj_scene2D/testdata/
test_h264_bframes.mp4 (128x128, B-frames), rewritten to Annex B with FFmpeg's
h264_mp4toannexb bitstream filter, one foxglove.CompressedVideo message per
frame in decode order. The browser decodes them through the cross-compiled
FFmpeg behind Scene2D's VideoLayer, so B-frame reordering is exercised too.
"""

import base64
import hashlib
import io
import textwrap
from importlib.metadata import version
from pathlib import Path

import av
from google.protobuf import descriptor_pb2
from mcap.reader import make_reader
from mcap.writer import CompressionType, IndexType, Writer

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "pj_scene2D" / "testdata" / "test_h264_bframes.mp4"
OUTPUT = Path(__file__).with_name("foxglove_video.mcap.b64")
TOPIC = "/foxglove/video"
STEP_NS = 100_000_000
FRAME_COUNT = 60


def add_field(message, name, number, field_type, *, type_name=""):
    field = message.field.add()
    field.name = name
    field.number = number
    field.type = field_type
    field.label = descriptor_pb2.FieldDescriptorProto.LABEL_OPTIONAL
    if type_name:
        field.type_name = type_name


def foxglove_descriptor_set() -> bytes:
    field = descriptor_pb2.FieldDescriptorProto
    source = descriptor_pb2.FileDescriptorProto(
        name="foxglove/VideoWasmAcceptance.proto",
        package="foxglove",
        syntax="proto3",
    )
    timestamp = source.message_type.add(name="Timestamp")
    add_field(timestamp, "sec", 1, field.TYPE_INT64)
    add_field(timestamp, "nsec", 2, field.TYPE_UINT32)
    video = source.message_type.add(name="CompressedVideo")
    add_field(video, "timestamp", 1, field.TYPE_MESSAGE, type_name=".foxglove.Timestamp")
    add_field(video, "frame_id", 2, field.TYPE_STRING)
    add_field(video, "data", 3, field.TYPE_BYTES)
    add_field(video, "format", 4, field.TYPE_STRING)
    descriptor_set = descriptor_pb2.FileDescriptorSet()
    descriptor_set.file.add().CopyFrom(source)
    return descriptor_set.SerializeToString(deterministic=True)


def encode_varint(value: int) -> bytes:
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def message_field(number: int, payload: bytes) -> bytes:
    return encode_varint((number << 3) | 2) + encode_varint(len(payload)) + payload


def varint_field(number: int, value: int) -> bytes:
    return encode_varint(number << 3) + encode_varint(value)


def compressed_video(timestamp_ns: int, annexb: bytes) -> bytes:
    stamp = varint_field(1, timestamp_ns // 1_000_000_000) + varint_field(2, timestamp_ns % 1_000_000_000)
    return b"".join(
        (
            message_field(1, stamp),
            message_field(2, b"camera_optical"),
            message_field(3, annexb),
            message_field(4, b"h264"),
        )
    )


def annexb_access_units() -> list[bytes]:
    """Annex B access units in decode order, SPS/PPS inlined on keyframes."""
    units = []
    with av.open(str(SOURCE)) as container:
        stream = container.streams.video[0]
        bsf = av.BitStreamFilterContext("h264_mp4toannexb", stream)
        for packet in container.demux(stream):
            for filtered in bsf.filter(packet if packet.size > 0 else None):
                units.append(bytes(filtered))
    return units


def build_fixture(units: list[bytes]) -> bytes:
    output = io.BytesIO()
    writer = Writer(
        output,
        compression=CompressionType.NONE,
        index_types=IndexType.ALL,
        enable_crcs=True,
        enable_data_crcs=True,
    )
    writer.start(profile="", library="pj-wasm-video-generator/1")
    schema = writer.register_schema(
        name="foxglove.CompressedVideo",
        encoding="protobuf",
        data=foxglove_descriptor_set(),
    )
    channel = writer.register_channel(topic=TOPIC, message_encoding="protobuf", schema_id=schema)
    for sequence, unit in enumerate(units):
        timestamp_ns = sequence * STEP_NS
        writer.add_message(
            channel_id=channel,
            log_time=timestamp_ns,
            publish_time=timestamp_ns,
            sequence=sequence,
            data=compressed_video(timestamp_ns, unit),
        )
    writer.finish()
    return output.getvalue()


def verify_fixture(payload: bytes) -> None:
    messages = list(make_reader(io.BytesIO(payload), validate_crcs=True).iter_messages(log_time_order=True))
    if len(messages) != FRAME_COUNT or {channel.topic for _, channel, _ in messages} != {TOPIC}:
        raise RuntimeError("fixture topic/message matrix changed")
    first = messages[0][2].data
    if b"\x00\x00\x00\x01\x67" not in first:
        raise RuntimeError("first access unit does not carry an inline SPS")


def main() -> None:
    required = {"mcap": "1.3.1", "protobuf": "7.35.0", "av": "18.1.0"}
    for package, expected in required.items():
        actual = version(package)
        if actual != expected:
            raise RuntimeError(f"{package}=={expected} is required, found {actual}")
    units = annexb_access_units()
    if len(units) != FRAME_COUNT:
        raise RuntimeError(f"expected {FRAME_COUNT} access units, got {len(units)}")
    payload = build_fixture(units)
    verify_fixture(payload)
    encoded = base64.b64encode(payload).decode("ascii")
    OUTPUT.write_text("\n".join(textwrap.wrap(encoded, width=76)) + "\n", encoding="ascii")
    print(f"wrote {OUTPUT} ({len(payload)} bytes, sha256={hashlib.sha256(payload).hexdigest()})")


if __name__ == "__main__":
    main()
