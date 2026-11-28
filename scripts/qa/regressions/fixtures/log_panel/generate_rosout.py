#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the two ROS 2 /rosout MCAPs of the Log panel regression scripts.

rosout_a.mcap and rosout_b.mcap each hold 60 rcl_interfaces/msg/Log entries on /rosout, half a second apart, from
nodes node_a and node_b; b's start a quarter second after a's, so a merge interleaves them. 60 rows are more than the
panel shows at once, so a script can tell where the list scrolled.

Reproduce with mcap==1.3.1:

    python3 scripts/qa/regressions/fixtures/log_panel/generate_rosout.py
"""

import struct
from pathlib import Path

from mcap.writer import CompressionType, Writer

HERE = Path(__file__).parent
START_NS = 1_700_000_000 * 1_000_000_000
STEP_NS = 500_000_000
COUNT = 60
INFO, WARN, ERROR = 20, 30, 40

LOG_SCHEMA = b"""byte DEBUG=10
byte INFO=20
byte WARN=30
byte ERROR=40
byte FATAL=50
builtin_interfaces/Time stamp
uint8 level
string name
string msg
string file
string function
uint32 line
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
"""


def cdr_log(stamp_ns: int, level: int, name: str, msg: str) -> bytes:
    data = bytearray(b"\x00\x01\x00\x00")  # CDR, little endian

    def align(n: int) -> None:
        data.extend(b"\x00" * (-(len(data) - 4) % n))

    def string(value: str) -> None:
        encoded = value.encode() + b"\x00"
        align(4)
        data.extend(struct.pack("<I", len(encoded)) + encoded)

    data.extend(struct.pack("<iI", stamp_ns // 1_000_000_000, stamp_ns % 1_000_000_000))
    data.extend(struct.pack("<B", level))
    for value in (name, msg, "node.cpp", "tick"):
        string(value)
    align(4)
    data.extend(struct.pack("<I", 42))
    return bytes(data)


def write(prefix: str, offset_ns: int) -> None:
    with open(HERE / f"rosout_{prefix}.mcap", "wb") as stream:
        writer = Writer(stream, compression=CompressionType.NONE)
        writer.start(profile="ros2", library="pj-qa-fixture")
        schema = writer.register_schema(name="rcl_interfaces/msg/Log", encoding="ros2msg", data=LOG_SCHEMA)
        channel = writer.register_channel(topic="/rosout", message_encoding="cdr", schema_id=schema)
        for i in range(COUNT):
            stamp = START_NS + offset_ns + i * STEP_NS
            level = ERROR if i % 5 == 4 else WARN if i % 5 == 2 else INFO
            payload = cdr_log(stamp, level, f"node_{prefix}", f"{prefix}-{i:02d}")
            writer.add_message(channel_id=channel, log_time=stamp, publish_time=stamp, data=payload, sequence=i)
        writer.finish()


write("a", 0)
write("b", STEP_NS // 2)
