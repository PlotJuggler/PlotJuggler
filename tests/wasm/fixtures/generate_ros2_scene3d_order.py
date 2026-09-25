#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the deterministic mixed-family Scene3D ordering fixture.

Reproduce with mcap==1.3.1. One raw PointCloud2, two PoseArrays (the second
shifted 1 m along x, so a pose trail has two distinct points), one translucent
OccupancyGrid, a second PointCloud2, and their TF chain share a dataset so
browser acceptance can physically reorder unlike/interleaved layer families
and verify the renderer's actual command submission order.
"""

import hashlib
import io
from importlib.metadata import version
from pathlib import Path

from mcap.reader import make_reader
from mcap.writer import CompressionType, IndexType, Writer

import generate_ros2_tf_grid as occupancy
import scene3d_data_fixture_support as data


OUTPUT = Path(__file__).with_name("ros2_scene3d_order_real.mcap")
POINT_TOPIC = "/points"
SECOND_POINT_TOPIC = "/zz_points"
POSE_TOPIC = "/poses"
TF_TOPIC = "/tf"
GRID_TOPIC = "/zz_grid"


def occupancy_grid_cdr() -> bytes:
    width = 80
    height = 80
    cdr = occupancy.CdrWriter()
    occupancy.timestamp(cdr, 0)
    cdr.string("map")
    occupancy.timestamp(cdr, 0)
    cdr.float32(0.1)
    cdr.uint32(width)
    cdr.uint32(height)
    for value in (-4.0, -4.0, -0.05):
        cdr.float64(value)
    for value in (0.0, 0.0, 0.0, 1.0):
        cdr.float64(value)
    cells = bytes(100 if row in (0, height - 1) or column in (0, width - 1) else 35
                  for row in range(height) for column in range(width))
    cdr.byte_sequence(cells)
    return bytes(cdr.data)


def tf_message_cdr() -> bytes:
    cdr = data.CdrWriter()
    cdr.uint32(3)
    identity = (0.0, 0.0, 0.0, 1.0)
    data.append_transform(cdr, 0, "map", "base_link", (0.0, 0.0, 0.15), identity)
    data.append_transform(cdr, 0, "base_link", "laser", (0.0, 0.0, 0.45), identity)
    data.append_transform(cdr, 0, "base_link", "particles", (0.0, 0.0, 0.5), identity)
    return bytes(cdr.data)


def build_fixture() -> bytes:
    output = io.BytesIO()
    writer = Writer(
        output,
        chunk_size=8 * 1024 * 1024,
        compression=CompressionType.ZSTD,
        index_types=IndexType.ALL,
        enable_crcs=True,
        enable_data_crcs=True,
    )
    writer.start(profile="ros2", library="pj-w19d-scene3d-order-generator/1")
    point_schema = writer.register_schema(
        name="sensor_msgs/msg/PointCloud2", encoding="ros2msg", data=data.POINTCLOUD_SCHEMA
    )
    pose_schema = writer.register_schema(
        name="geometry_msgs/msg/PoseArray", encoding="ros2msg", data=data.POSE_SCHEMA
    )
    tf_schema = writer.register_schema(
        name="tf2_msgs/msg/TFMessage", encoding="ros2msg", data=occupancy.TF_SCHEMA
    )
    grid_schema = writer.register_schema(
        name="nav_msgs/msg/OccupancyGrid", encoding="ros2msg", data=occupancy.GRID_SCHEMA
    )
    point_channel = writer.register_channel(topic=POINT_TOPIC, message_encoding="cdr", schema_id=point_schema)
    second_point_channel = writer.register_channel(
        topic=SECOND_POINT_TOPIC, message_encoding="cdr", schema_id=point_schema
    )
    pose_channel = writer.register_channel(topic=POSE_TOPIC, message_encoding="cdr", schema_id=pose_schema)
    tf_channel = writer.register_channel(topic=TF_TOPIC, message_encoding="cdr", schema_id=tf_schema)
    grid_channel = writer.register_channel(topic=GRID_TOPIC, message_encoding="cdr", schema_id=grid_schema)
    writer.add_message(
        channel_id=point_channel,
        log_time=0,
        publish_time=0,
        sequence=0,
        data=data.pointcloud_cdr(0),
    )
    writer.add_message(
        channel_id=pose_channel,
        log_time=0,
        publish_time=0,
        sequence=0,
        data=data.pose_array_cdr(0),
    )
    writer.add_message(
        channel_id=pose_channel,
        log_time=0,
        publish_time=0,
        sequence=1,
        data=data.pose_array_cdr(0, x_offset=1.0),
    )
    writer.add_message(
        channel_id=grid_channel,
        log_time=0,
        publish_time=0,
        sequence=0,
        data=occupancy_grid_cdr(),
    )
    writer.add_message(
        channel_id=second_point_channel,
        log_time=0,
        publish_time=0,
        sequence=0,
        data=data.pointcloud_cdr(0),
    )
    writer.add_message(
        channel_id=tf_channel,
        log_time=0,
        publish_time=0,
        sequence=0,
        data=tf_message_cdr(),
    )
    writer.finish()
    return output.getvalue()


def verify_fixture(payload: bytes) -> None:
    messages = list(make_reader(io.BytesIO(payload), validate_crcs=True).iter_messages(log_time_order=True))
    topics = [channel.topic for _, channel, _ in messages]
    if topics != [POINT_TOPIC, POSE_TOPIC, POSE_TOPIC, GRID_TOPIC, SECOND_POINT_TOPIC, TF_TOPIC]:
        raise RuntimeError(f"fixture topic order/count changed: {topics}")
    if any(message.log_time != 0 or message.publish_time != 0 for _, _, message in messages):
        raise RuntimeError("fixture timestamps changed")


def main() -> None:
    if version("mcap") != "1.3.1":
        raise RuntimeError(f"mcap==1.3.1 is required, found {version('mcap')}")
    payload = build_fixture()
    verify_fixture(payload)
    OUTPUT.write_bytes(payload)
    print(f"wrote {OUTPUT} ({len(payload)} bytes, sha256={hashlib.sha256(payload).hexdigest()})")


if __name__ == "__main__":
    main()
