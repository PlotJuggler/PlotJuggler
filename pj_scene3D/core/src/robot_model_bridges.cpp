// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/robot_model_bridges.h"

#include <utility>

namespace pj::scene3d {

std::vector<StampedTransform> fixedJointStaticTransforms(const RobotModel& model, TimePoint stamp) {
  std::vector<StampedTransform> edges;
  for (const RobotJoint& joint : model.joints) {
    if (joint.type != JointType::kFixed || joint.parent.empty() || joint.child.empty()) {
      continue;
    }
    StampedTransform edge;
    edge.stamp = stamp;
    edge.parent_frame = joint.parent;
    edge.child_frame = joint.child;
    edge.transform = Transform{joint.origin_xyz, rpyToQuat(joint.origin_rpy)};
    edges.push_back(std::move(edge));
  }
  return edges;
}

int injectMissingStaticTransforms(TransformBuffer& buf, const std::vector<StampedTransform>& edges) {
  int injected = 0;
  for (const StampedTransform& edge : edges) {
    if (buf.getParent(edge.child_frame).has_value()) {
      continue;
    }
    if (buf.setTransform(edge).has_value()) {
      ++injected;
    }
  }
  return injected;
}

}  // namespace pj::scene3d
