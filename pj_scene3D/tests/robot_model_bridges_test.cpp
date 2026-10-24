// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/robot_model_bridges.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <string>
#include <utility>

#include "pj_base/time.hpp"
#include "pj_scene3d_core/robot_model.h"
#include "pj_tf/tf_buffer.h"
#include "pj_tf/transform.h"

namespace pj::scene3d {
namespace {

RobotJoint fixedJoint(std::string parent, std::string child, glm::dvec3 xyz, glm::dvec3 rpy = {0, 0, 0}) {
  RobotJoint j;
  j.name = parent + "_to_" + child;
  j.parent = std::move(parent);
  j.child = std::move(child);
  j.type = JointType::kFixed;
  j.origin_xyz = xyz;
  j.origin_rpy = rpy;
  return j;
}

void setEdge(
    TransformBuffer& buf, const std::string& parent, const std::string& child, PJ::Timepoint stamp,
    glm::dvec3 translation = {0, 0, 0}) {
  StampedTransform st;
  st.stamp = stamp;
  st.parent_frame = parent;
  st.child_frame = child;
  st.transform = Transform{translation, glm::dquat(1, 0, 0, 0)};
  ASSERT_TRUE(buf.setTransform(st).has_value()) << "failed to seed edge " << parent << " -> " << child;
}

// Only FIXED joints become static bridges; movable joints are dropped.
TEST(RobotModelBridges, EmitsOneStaticTransformPerFixedJoint) {
  RobotModel model;
  model.joints.push_back(fixedJoint("a", "b", {1, 2, 3}));
  RobotJoint revolute;
  revolute.name = "b_to_c";
  revolute.parent = "b";
  revolute.child = "c";
  revolute.type = JointType::kRevolute;
  revolute.origin_xyz = {0, 0, 1};
  model.joints.push_back(revolute);

  const auto edges = fixedJointStaticTransforms(model);
  ASSERT_EQ(edges.size(), 1u);
  EXPECT_EQ(edges[0].parent_frame, "a");
  EXPECT_EQ(edges[0].child_frame, "b");
  EXPECT_DOUBLE_EQ(edges[0].transform.t.x, 1.0);
  EXPECT_DOUBLE_EQ(edges[0].transform.t.y, 2.0);
  EXPECT_DOUBLE_EQ(edges[0].transform.t.z, 3.0);
}

// rpyToQuat must use the SAME rotation convention as originToMat4 (URDF ZYX), or
// every bridged frame would be silently mis-oriented.
TEST(RobotModelBridges, RpyToQuatMatchesOriginToMat4) {
  const glm::dvec3 rpy{0.3, -0.7, 1.1};
  const glm::dmat3 from_mat = glm::dmat3(originToMat4({0, 0, 0}, rpy));
  const glm::dmat3 from_quat = glm::mat3_cast(rpyToQuat(rpy));
  for (int col = 0; col < 3; ++col) {
    for (int row = 0; row < 3; ++row) {
      EXPECT_NEAR(from_quat[col][row], from_mat[col][row], 1e-9) << "mismatch at col=" << col << " row=" << row;
    }
  }
}

// The DROID bug, reproduced at the buffer level: the gripper subtree is rooted at
// an unpublished base, so it is disconnected from `scene`. Injecting the URDF's
// fixed-joint bridges reconnects the whole subtree (the published knuckle edge
// cascades into resolvability once its base has a parent).
TEST(RobotModelBridges, InjectReconnectsDisconnectedGripperSubtree) {
  TransformBuffer buf;
  const PJ::Timepoint t = PJ::fromRaw(1'000'000);
  setEdge(buf, "scene", "panda_link7", t);
  setEdge(buf, "robotiq_85_base_link", "left_inner_knuckle", t, {0.01, 0, 0.07});

  EXPECT_FALSE(buf.tryLookupTransform("scene", "left_inner_knuckle", t, pj::tf::Clock::kHeader).has_value())
      << "precondition: gripper subtree must start disconnected";

  RobotModel model;
  model.joints.push_back(fixedJoint("panda_link7", "panda_link8", {0, 0, 0.107}));
  model.joints.push_back(fixedJoint("panda_link8", "robotiq_85_base_link", {0, 0, 0}, {0, 0, 1.57}));

  const int injected = injectMissingStaticTransforms(buf, fixedJointStaticTransforms(model));
  EXPECT_EQ(injected, 2);

  EXPECT_TRUE(buf.tryLookupTransform("scene", "left_inner_knuckle", t, pj::tf::Clock::kHeader).has_value())
      << "gripper subtree should resolve into scene after bridging";
}

// The guard must never overwrite a frame the data already publishes (the TF buffer
// is first-parent-wins; a conflicting reparent would freeze the real edge forever).
TEST(RobotModelBridges, InjectNeverOverridesRealTf) {
  TransformBuffer buf;
  const PJ::Timepoint t = PJ::fromRaw(1'000'000);
  setEdge(buf, "scene", "data_parent", t);
  setEdge(buf, "data_parent", "b", t, {5, 0, 0});

  RobotModel model;
  model.joints.push_back(fixedJoint("scene", "b", {0, 0, 0}));  // conflicting bridge

  const int injected = injectMissingStaticTransforms(buf, fixedJointStaticTransforms(model));
  EXPECT_EQ(injected, 0) << "guard must skip a child the data already owns";
  EXPECT_EQ(buf.getParent("b").value_or(""), "data_parent");

  const auto tf = buf.tryLookupTransform("scene", "b", t, pj::tf::Clock::kHeader);
  ASSERT_TRUE(tf.has_value());
  EXPECT_DOUBLE_EQ(tf->t.x, 5.0) << "real edge translation must survive, not the bridge's";
}

// Re-running the inject is a no-op once the edges are present (the per-tick
// self-healing path must not churn the buffer).
TEST(RobotModelBridges, InjectIsIdempotent) {
  TransformBuffer buf;
  const PJ::Timepoint t = PJ::fromRaw(1'000'000);
  setEdge(buf, "scene", "a", t);

  RobotModel model;
  model.joints.push_back(fixedJoint("a", "b", {0, 0, 1}));
  const auto edges = fixedJointStaticTransforms(model);

  EXPECT_EQ(injectMissingStaticTransforms(buf, edges), 1);
  EXPECT_EQ(injectMissingStaticTransforms(buf, edges), 0);
}

}  // namespace
}  // namespace pj::scene3d
