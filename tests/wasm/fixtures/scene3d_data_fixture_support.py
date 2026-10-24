# SPDX-License-Identifier: MPL-2.0
"""Shared ROS2 payloads for the mixed Scene3D browser fixture."""

import math
import struct
from functools import cache


STEP_NS = 1_000_000_000

class CdrWriter:
    def __init__(self) -> None:
        self.data = bytearray(b"\x00\x01\x00\x00")

    def align(self, alignment: int) -> None:
        # XCDR alignment is relative to the payload after the four-byte
        # encapsulation header, not to the start of the serialized buffer.
        self.data.extend(b"\x00" * (-(len(self.data) - 4) % alignment))

    def uint8(self, value: int) -> None:
        self.data.extend(struct.pack("<B", value))

    def boolean(self, value: bool) -> None:
        self.uint8(1 if value else 0)

    def int32(self, value: int) -> None:
        self.align(4)
        self.data.extend(struct.pack("<i", value))

    def uint32(self, value: int) -> None:
        self.align(4)
        self.data.extend(struct.pack("<I", value))

    def float32(self, value: float) -> None:
        self.align(4)
        self.data.extend(struct.pack("<f", value))

    def float64(self, value: float) -> None:
        self.align(8)
        self.data.extend(struct.pack("<d", value))

    def string(self, value: str) -> None:
        encoded = value.encode("utf-8") + b"\x00"
        self.uint32(len(encoded))
        self.data.extend(encoded)

    def byte_sequence(self, value: bytes) -> None:
        self.uint32(len(value))
        self.data.extend(value)

def timestamp(cdr: CdrWriter, timestamp_ns: int) -> None:
    cdr.int32(timestamp_ns // STEP_NS)
    cdr.uint32(timestamp_ns % STEP_NS)



POINTCLOUD_SCHEMA = b"""std_msgs/Header header
uint32 height
uint32 width
sensor_msgs/PointField[] fields
bool is_bigendian
uint32 point_step
uint32 row_step
uint8[] data
bool is_dense
================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
================================================================================
MSG: sensor_msgs/PointField
uint8 INT8=1
uint8 UINT8=2
uint8 INT16=3
uint8 UINT16=4
uint8 INT32=5
uint8 UINT32=6
uint8 FLOAT32=7
uint8 FLOAT64=8
string name
uint32 offset
uint8 datatype
uint32 count
"""

POSE_SCHEMA = b"""std_msgs/Header header
geometry_msgs/Pose[] poses
================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
================================================================================
MSG: geometry_msgs/Pose
geometry_msgs/Point position
geometry_msgs/Quaternion orientation
================================================================================
MSG: geometry_msgs/Point
float64 x
float64 y
float64 z
================================================================================
MSG: geometry_msgs/Quaternion
float64 x
float64 y
float64 z
float64 w
"""

CLOUD_WIDTH = 300
CLOUD_HEIGHT = 200
POSE_COUNT = 256


def quaternion_from_yaw(yaw: float) -> tuple[float, float, float, float]:
    return (0.0, 0.0, math.sin(yaw / 2.0), math.cos(yaw / 2.0))


@cache
def point_bytes() -> bytes:
    output = bytearray(CLOUD_WIDTH * CLOUD_HEIGHT * 20)
    invalid = {17, 31_337, (CLOUD_WIDTH * CLOUD_HEIGHT) - 9}
    for row in range(CLOUD_HEIGHT):
        radial_fraction = (row % (CLOUD_HEIGHT // 2)) / ((CLOUD_HEIGHT // 2) - 1)
        radius = 1.5 + (3.5 * radial_fraction)
        for column in range(CLOUD_WIDTH):
            index = (row * CLOUD_WIDTH) + column
            angle = (2.0 * math.pi * column) / CLOUD_WIDTH
            x = radius * math.cos(angle)
            y = radius * math.sin(angle)
            z = 0.45 * math.sin(3.0 * angle) + (0.6 * radial_fraction)
            if index in invalid:
                x = math.nan
            intensity = (0.7 * radial_fraction) + (0.3 * ((math.sin(angle) + 1.0) * 0.5))
            sector = (3 * column) // CLOUD_WIDTH
            red, green, blue = ((255, 24, 24), (24, 255, 24), (24, 24, 255))[sector]
            struct.pack_into("<ffffBBBB", output, index * 20, x, y, z, intensity, red, green, blue, 255)
    return bytes(output)


def pointcloud_cdr(sequence: int) -> bytes:
    fields = (("x", 0, 7), ("y", 4, 7), ("z", 8, 7), ("intensity", 12, 7), ("rgba", 16, 6))
    cdr = CdrWriter()
    timestamp(cdr, sequence * STEP_NS)
    cdr.string("laser")
    cdr.uint32(CLOUD_HEIGHT)
    cdr.uint32(CLOUD_WIDTH)
    cdr.uint32(len(fields))
    for name, offset, datatype in fields:
        cdr.string(name)
        cdr.uint32(offset)
        cdr.uint8(datatype)
        cdr.uint32(1)
    cdr.boolean(False)
    cdr.uint32(20)
    cdr.uint32(CLOUD_WIDTH * 20)
    cdr.byte_sequence(point_bytes())
    cdr.boolean(False)
    return bytes(cdr.data)


def pose_array_cdr(sequence: int, x_offset: float = 0.0) -> bytes:
    cdr = CdrWriter()
    timestamp(cdr, sequence * STEP_NS)
    cdr.string("particles")
    cdr.uint32(POSE_COUNT)
    for index in range(POSE_COUNT):
        row, column = divmod(index, 16)
        values = (
            (column - 7.5) * 0.35 + x_offset,
            (row - 7.5) * 0.35,
            0.25 * math.sin(column * 0.55) * math.cos(row * 0.45),
        ) + quaternion_from_yaw((row + column) * 0.09)
        for value in values:
            cdr.float64(value)
    return bytes(cdr.data)


def append_transform(
    cdr: CdrWriter,
    timestamp_ns: int,
    parent: str,
    child: str,
    translation: tuple[float, float, float],
    rotation: tuple[float, float, float, float],
) -> None:
    timestamp(cdr, timestamp_ns)
    cdr.string(parent)
    cdr.string(child)
    for value in translation + rotation:
        cdr.float64(value)
