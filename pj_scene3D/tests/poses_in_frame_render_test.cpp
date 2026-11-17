// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/poses_in_frame_render.h"

#include <gtest/gtest.h>

#include <cmath>
#include <glm/glm.hpp>

#include "pj_base/builtin/poses_in_frame.hpp"

namespace pj::scene3d {
namespace {

using PJ::sdk::Pose;
using PJ::sdk::PosesInFrame;

// A two-pose message: pose 0 at (1,2,3) with identity orientation, pose 1 at the
// origin yawed +90 deg about +Z (so its local +X points along world +Y).
PosesInFrame twoPoseMessage() {
  PosesInFrame msg;
  msg.frame_id = "map";
  Pose first;
  first.position = {1.0, 2.0, 3.0};
  msg.poses.push_back(first);
  Pose second;
  const double half_sqrt2 = std::sqrt(2.0) / 2.0;
  second.orientation = {0.0, 0.0, half_sqrt2, half_sqrt2};  // x,y,z,w
  msg.poses.push_back(second);
  return msg;
}

glm::vec4 apply(const glm::mat4& m, glm::vec4 v) {
  return m * v;
}

TEST(PosesInFrameRenderTest, ProducesThreeInstancesPerPose) {
  const PosesInFrame msg = twoPoseMessage();
  const std::vector<PoseTriadInstance> out = buildPoseTriadInstances(msg, {.axis_length = 0.2f, .opacity = 1.0f});
  EXPECT_EQ(out.size(), 6U);
}

// For an identity-orientation pose, each arm rotates the unit arrow's local +X
// onto world +X / +Y / +Z respectively, scaled by axis_length.
TEST(PosesInFrameRenderTest, ArmsOrientLocalXOntoWorldAxes) {
  PosesInFrame msg;
  msg.frame_id = "map";
  msg.poses.push_back(Pose{});  // origin, identity
  const float len = 0.5f;
  const std::vector<PoseTriadInstance> out = buildPoseTriadInstances(msg, {.axis_length = len, .opacity = 1.0f});
  ASSERT_EQ(out.size(), 3U);

  const glm::vec4 local_x{1.0f, 0.0f, 0.0f, 0.0f};  // direction (w=0)
  const glm::vec4 x_dir = apply(out[0].model, local_x);
  EXPECT_NEAR(x_dir.x, len, 1e-5f);
  EXPECT_NEAR(x_dir.y, 0.0f, 1e-5f);
  EXPECT_NEAR(x_dir.z, 0.0f, 1e-5f);

  const glm::vec4 y_dir = apply(out[1].model, local_x);
  EXPECT_NEAR(y_dir.x, 0.0f, 1e-5f);
  EXPECT_NEAR(y_dir.y, len, 1e-5f);
  EXPECT_NEAR(y_dir.z, 0.0f, 1e-5f);

  const glm::vec4 z_dir = apply(out[2].model, local_x);
  EXPECT_NEAR(z_dir.x, 0.0f, 1e-5f);
  EXPECT_NEAR(z_dir.y, 0.0f, 1e-5f);
  EXPECT_NEAR(z_dir.z, len, 1e-5f);
}

TEST(PosesInFrameRenderTest, ArmOriginSitsAtPosePosition) {
  const PosesInFrame msg = twoPoseMessage();
  const std::vector<PoseTriadInstance> out = buildPoseTriadInstances(msg, {.axis_length = 0.3f, .opacity = 1.0f});
  ASSERT_EQ(out.size(), 6U);
  const glm::vec4 origin{0.0f, 0.0f, 0.0f, 1.0f};
  for (int arm = 0; arm < 3; ++arm) {
    const glm::vec4 p = apply(out[arm].model, origin);  // pose 0 at (1,2,3)
    EXPECT_NEAR(p.x, 1.0f, 1e-5f);
    EXPECT_NEAR(p.y, 2.0f, 1e-5f);
    EXPECT_NEAR(p.z, 3.0f, 1e-5f);
  }
}

// A yawed pose carries its arms with it: pose 1 is yawed +90 about +Z, so its
// X arm (the arrow axis) points along world +Y.
TEST(PosesInFrameRenderTest, YawedPoseRotatesArms) {
  const PosesInFrame msg = twoPoseMessage();
  const float len = 1.0f;
  const std::vector<PoseTriadInstance> out = buildPoseTriadInstances(msg, {.axis_length = len, .opacity = 1.0f});
  ASSERT_EQ(out.size(), 6U);
  const glm::vec4 local_x{1.0f, 0.0f, 0.0f, 0.0f};
  const glm::vec4 x_dir = apply(out[3].model, local_x);  // pose 1, X arm
  EXPECT_NEAR(x_dir.x, 0.0f, 1e-5f);
  EXPECT_NEAR(x_dir.y, len, 1e-5f);
  EXPECT_NEAR(x_dir.z, 0.0f, 1e-5f);
}

TEST(PosesInFrameRenderTest, OpacityGoesIntoAlphaClamped) {
  PosesInFrame msg;
  msg.poses.push_back(Pose{});
  for (const auto& inst : buildPoseTriadInstances(msg, {.axis_length = 0.2f, .opacity = 0.4f})) {
    EXPECT_NEAR(inst.color.a, 0.4f, 1e-6f);
  }
  for (const auto& inst : buildPoseTriadInstances(msg, {.axis_length = 0.2f, .opacity = 1.5f})) {
    EXPECT_NEAR(inst.color.a, 1.0f, 1e-6f);
  }
  for (const auto& inst : buildPoseTriadInstances(msg, {.axis_length = 0.2f, .opacity = -0.3f})) {
    EXPECT_NEAR(inst.color.a, 0.0f, 1e-6f);
  }
}

TEST(PosesInFrameRenderTest, PerAxisColorDominance) {
  PosesInFrame msg;
  msg.poses.push_back(Pose{});
  const std::vector<PoseTriadInstance> out = buildPoseTriadInstances(msg, {.axis_length = 0.2f, .opacity = 1.0f});
  ASSERT_EQ(out.size(), 3U);
  EXPECT_GT(out[0].color.r, out[0].color.g);
  EXPECT_GT(out[0].color.r, out[0].color.b);
  EXPECT_GT(out[1].color.g, out[1].color.r);
  EXPECT_GT(out[1].color.g, out[1].color.b);
  EXPECT_GT(out[2].color.b, out[2].color.r);
  EXPECT_GT(out[2].color.b, out[2].color.g);
}

TEST(PosesInFrameRenderTest, EmptyPosesYieldEmpty) {
  PosesInFrame msg;
  msg.frame_id = "odom";
  EXPECT_TRUE(buildPoseTriadInstances(msg, {.axis_length = 0.2f, .opacity = 1.0f}).empty());
}

TEST(PosesInFrameRenderTest, XArrowOnlyProducesOneArmPerPose) {
  const PosesInFrame msg = twoPoseMessage();  // 2 poses
  const std::vector<PoseTriadInstance> out = buildPoseTriadInstances(msg, {.axis_length = 0.3f, .x_arrow_only = true});
  EXPECT_EQ(out.size(), 2U);
}

TEST(PosesInFrameRenderTest, XArrowOnlyArmPointsAlongLocalX) {
  PosesInFrame msg;
  msg.poses.push_back(Pose{});  // origin, identity
  const float len = 0.5f;
  const std::vector<PoseTriadInstance> out = buildPoseTriadInstances(msg, {.axis_length = len, .x_arrow_only = true});
  ASSERT_EQ(out.size(), 1U);
  const glm::vec4 dir = out[0].model * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
  EXPECT_NEAR(dir.x, len, 1e-5f);
  EXPECT_NEAR(dir.y, 0.0f, 1e-5f);
  EXPECT_NEAR(dir.z, 0.0f, 1e-5f);
}

// Without override, the single X-only arm keeps its NATURAL axis color (red),
// not a user-picked color — override coloring is a separate, orthogonal toggle.
TEST(PosesInFrameRenderTest, XArrowOnlyWithoutOverrideIsAxisRed) {
  PosesInFrame msg;
  msg.poses.push_back(Pose{});
  const std::vector<PoseTriadInstance> out = buildPoseTriadInstances(msg, {.x_arrow_only = true});
  ASSERT_EQ(out.size(), 1U);
  EXPECT_GT(out[0].color.r, out[0].color.g);
  EXPECT_GT(out[0].color.r, out[0].color.b);
}

// Override colour in FULL-TRIAD mode: all three arms carry the override colour instead of the per-axis R/G/B.
TEST(PosesInFrameRenderTest, OverrideColorRecolorsWholeTriad) {
  PosesInFrame msg;
  msg.poses.push_back(Pose{});
  const std::vector<PoseTriadInstance> out =
      buildPoseTriadInstances(msg, {.override_color = true, .color = {0.1f, 0.2f, 0.7f}});
  ASSERT_EQ(out.size(), 3U);
  for (const auto& inst : out) {
    EXPECT_NEAR(inst.color.r, 0.1f, 1e-5f);
    EXPECT_NEAR(inst.color.g, 0.2f, 1e-5f);
    EXPECT_NEAR(inst.color.b, 0.7f, 1e-5f);
  }
}

// Override color in X-ONLY mode: the single arm carries the override color, with
// opacity still in alpha.
TEST(PosesInFrameRenderTest, OverrideColorAppliesInXOnlyMode) {
  PosesInFrame msg;
  msg.poses.push_back(Pose{});
  const std::vector<PoseTriadInstance> out = buildPoseTriadInstances(
      msg, {.opacity = 0.5f, .x_arrow_only = true, .override_color = true, .color = {0.1f, 0.2f, 0.7f}});
  ASSERT_EQ(out.size(), 1U);
  EXPECT_NEAR(out[0].color.r, 0.1f, 1e-5f);
  EXPECT_NEAR(out[0].color.g, 0.2f, 1e-5f);
  EXPECT_NEAR(out[0].color.b, 0.7f, 1e-5f);
  EXPECT_NEAR(out[0].color.a, 0.5f, 1e-5f);
}

}  // namespace
}  // namespace pj::scene3d
