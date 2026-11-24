#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the Foxglove SceneUpdate line-marker fixture: one TF (map -> lines) and one update.

Reproduce with mcap==1.3.1 and protobuf==7.35.0. On a +-2 m square of frame `lines`, a LINE_LIST holds the 41
lines along y and the 20 lines along x below y = 0, and a LINE_STRIP snakes along x through the 21 rows from
y = 0 to y = 2: the 4 m x 0.1 m line grid of scene3d_foundation.spec.js, made of marker lines.
"""

import hashlib
import io
from importlib.metadata import version
from pathlib import Path

from mcap.reader import make_reader
from mcap.writer import CompressionType, IndexType, Writer

from generate_foxglove_scene_entities import (
    color,
    double_field,
    foxglove_descriptor_set,
    frame_transform,
    message_field,
    point,
    pose,
    scene_entity,
    varint_field,
)

OUTPUT = Path(__file__).with_name("foxglove_marker_lines_real.mcap")
LINE_STRIP = 0
LINE_LIST = 2
HALF_EXTENT = 2.0
ROWS = 40  # cells per side, 0.1 m each
TINT = (1.0, 0.45, 0.1, 1.0)


def coordinate(index: int) -> float:
    return -HALF_EXTENT + (2.0 * HALF_EXTENT * index / ROWS)


def line_primitive(line_type: int, points) -> bytes:
    output = bytearray(varint_field(1, line_type))
    output.extend(message_field(2, pose()))
    output.extend(double_field(3, 1.0))
    output.extend(varint_field(4, 1))
    for value in points:
        output.extend(message_field(5, point(value)))
    output.extend(message_field(6, color(TINT)))
    return bytes(output)


def scene_update() -> bytes:
    segments = []
    for index in range(ROWS + 1):
        segments += [(coordinate(index), -HALF_EXTENT, 0.0), (coordinate(index), HALF_EXTENT, 0.0)]
    for index in range(ROWS // 2):
        segments += [(-HALF_EXTENT, coordinate(index), 0.0), (HALF_EXTENT, coordinate(index), 0.0)]
    strip = []
    for row, index in enumerate(range(ROWS // 2, ROWS + 1)):
        ends = [(-HALF_EXTENT, coordinate(index), 0.0), (HALF_EXTENT, coordinate(index), 0.0)]
        strip += ends if row % 2 == 0 else ends[::-1]
    entity = scene_entity(0, "lines", "lines", [
        (11, line_primitive(LINE_LIST, segments)),
        (11, line_primitive(LINE_STRIP, strip)),
    ])
    return message_field(2, entity)


def build_fixture() -> bytes:
    output = io.BytesIO()
    writer = Writer(
        output,
        compression=CompressionType.ZSTD,
        index_types=IndexType.ALL,
        enable_crcs=True,
        enable_data_crcs=True,
    )
    writer.start(profile="", library="pj-marker-lines-generator/1")
    descriptor = foxglove_descriptor_set()
    scene_schema = writer.register_schema(name="foxglove.SceneUpdate", encoding="protobuf", data=descriptor)
    tf_schema = writer.register_schema(name="foxglove.FrameTransform", encoding="protobuf", data=descriptor)
    marker_channel = writer.register_channel(topic="/markers", message_encoding="protobuf", schema_id=scene_schema)
    tf_channel = writer.register_channel(topic="/tf", message_encoding="protobuf", schema_id=tf_schema)
    for channel_id, payload in (
        (tf_channel, frame_transform(0, "lines", (0.0, 0.0, 0.0), 0.0)),
        (marker_channel, scene_update()),
    ):
        writer.add_message(channel_id=channel_id, log_time=0, publish_time=0, sequence=0, data=payload)
    writer.finish()
    return output.getvalue()


def main() -> None:
    required = {"mcap": "1.3.1", "protobuf": "7.35.0"}
    for package, expected in required.items():
        actual = version(package)
        if actual != expected:
            raise RuntimeError(f"{package}=={expected} is required, found {actual}")
    payload = build_fixture()
    topics = [channel.topic for _, channel, _ in make_reader(io.BytesIO(payload), validate_crcs=True).iter_messages()]
    if sorted(topics) != ["/markers", "/tf"]:
        raise RuntimeError(f"fixture topics changed: {topics}")
    OUTPUT.write_bytes(payload)
    print(f"wrote {OUTPUT} ({len(payload)} bytes, sha256={hashlib.sha256(payload).hexdigest()})")


if __name__ == "__main__":
    main()
