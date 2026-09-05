#!/usr/bin/env python3
"""
Publisher ROS2 con mensajes stamped para probar embedded timestamp.

Publica dos topics con el mismo valor Y pero timestamps distintos:
  /pj/sensor_a  — Header.stamp = t (correcto)
  /pj/sensor_b  — Header.stamp = t + 30 s (reloj adelantado 30 s)

Con ☑ Use embedded timestamp en parser_ros:
  sensor_a/data en X=t, sensor_b/data en X=t+30 → separadas 30 s

Con ☐ desactivado (receive time):
  Ambas en X=receive_time → se superponen

Usa std_msgs/msg/Header que tiene solo stamp + frame_id.
Para un campo numérico usa geometry_msgs/msg/PointStamped (x,y,z).

Run:
  python3 scripts/ros2_stamped_pub.py
"""
# Run under the ROS 2 environment for the sourced distro, e.g.
#   source /opt/ros/$ROS_DISTRO/setup.bash
#   python3 -m venv --system-site-packages .venv && . .venv/bin/activate
#   pip install "numpy>=1.26,<2"
# rclpy and its dependencies are resolved from that environment.

import math
import rclpy
from rclpy.node import Node
from geometry_msgs.msg import PointStamped
from builtin_interfaces.msg import Time

RATE_HZ    = 20
DT         = 1.0 / RATE_HZ
CLOCK_BIAS = 20.0   # sensor_b: 20 s adelantado


def to_ros_time(t_sec: float) -> Time:
    msg = Time()
    msg.sec     = int(t_sec)
    msg.nanosec = int((t_sec - int(t_sec)) * 1e9)
    return msg


class StampedPublisher(Node):
    def __init__(self):
        super().__init__("pj4_stamped_publisher")
        self.pub_a = self.create_publisher(PointStamped, "/pj/sensor_a", 10)
        self.pub_b = self.create_publisher(PointStamped, "/pj/sensor_b", 10)
        self.timer = self.create_timer(DT, self._tick)
        self._t = 0.0
        self.get_logger().info(
            f"Publishing /pj/sensor_a (stamp=t) and /pj/sensor_b (stamp=t+{CLOCK_BIAS}s)"
        )

    def _tick(self):
        y = 5.0 * math.sin(2 * math.pi * 0.5 * self._t)

        msg_a = PointStamped()
        msg_a.header.stamp    = to_ros_time(self._t)
        msg_a.header.frame_id = "sensor_a"
        msg_a.point.x = y
        msg_a.point.y = self._t

        msg_b = PointStamped()
        msg_b.header.stamp    = to_ros_time(self._t + CLOCK_BIAS)
        msg_b.header.frame_id = "sensor_b"
        msg_b.point.x = y   # mismo valor físico
        msg_b.point.y = self._t

        self.pub_a.publish(msg_a)
        self.pub_b.publish(msg_b)
        self._t += DT


def main():
    rclpy.init()
    node = StampedPublisher()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
