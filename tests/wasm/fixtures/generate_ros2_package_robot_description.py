#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the TF + robot_description browser fixture whose URDF names its
meshes by package (package_robot.urdf).

The fixture is synthetic project test data covered by the repository license.
It is generate_ros2_robot_description.py with another URDF. Reproduce it with:

    python3 -m pip install mcap==1.3.1
    python3 tests/wasm/fixtures/generate_ros2_package_robot_description.py
"""

from pathlib import Path

import generate_ros2_robot_description as base

base.URDF = Path(__file__).with_name("package_robot.urdf")
base.OUTPUT = Path(__file__).with_name("ros2_package_robot_description_real.mcap")

if __name__ == "__main__":
    base.main()
