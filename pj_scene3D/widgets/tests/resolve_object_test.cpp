// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// resolveObject() canonical fallback. With NO MessageParser bound (an empty
// ParserBinding — the path a data-source/toolbox canonical producer takes, e.g.
// the Mosaico cloud toolbox), the helper must decode a serialized pj_base blob
// via the canonical codec selected by builtin_object_type. This is the host-side
// half that lets a parser-less canonical object render in 3D. Pure unit test:
// no SessionManager session, no ObjectStore, no GL.

#include "pj_scene3d_widgets/resolve_object.h"

#include <gtest/gtest.h>

#include <any>
#include <cstdint>
#include <memory>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_base/builtin/frame_transforms_codec.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/point_cloud_codec.hpp"
#include "pj_base/builtin/poses_in_frame.hpp"
#include "pj_base/builtin/poses_in_frame_codec.hpp"

namespace {

using BT = PJ::sdk::BuiltinObjectType;

// Wrap serialized bytes as a self-owning PayloadView (anchors its own buffer).
PJ::sdk::PayloadView payloadOf(std::vector<std::uint8_t> bytes) {
  return PJ::sdk::PayloadView(std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)));
}

TEST(ResolveObject, HasCanonical3DCodecCoversThe3DTypesNotImageOrLog) {
  EXPECT_TRUE(pj::scene3d::hasCanonical3DCodec(BT::kPointCloud));
  EXPECT_TRUE(pj::scene3d::hasCanonical3DCodec(BT::kCompressedPointCloud));
  EXPECT_TRUE(pj::scene3d::hasCanonical3DCodec(BT::kFrameTransforms));
  EXPECT_TRUE(pj::scene3d::hasCanonical3DCodec(BT::kPosesInFrame));
  EXPECT_TRUE(pj::scene3d::hasCanonical3DCodec(BT::kOccupancyGrid));
  EXPECT_TRUE(pj::scene3d::hasCanonical3DCodec(BT::kVoxelGrid));
  EXPECT_TRUE(pj::scene3d::hasCanonical3DCodec(BT::kSceneEntities));
  EXPECT_FALSE(pj::scene3d::hasCanonical3DCodec(BT::kImage));  // 2D
  EXPECT_FALSE(pj::scene3d::hasCanonical3DCodec(BT::kLog));
  EXPECT_FALSE(pj::scene3d::hasCanonical3DCodec(BT::kNone));
}

TEST(ResolveObject, DecodesCanonicalPointCloudWithoutParser) {
  PJ::sdk::PointCloud cloud;
  cloud.width = 1;
  cloud.height = 1;
  cloud.point_step = 12;
  cloud.row_step = 12;
  cloud.frame_id = "lidar";
  cloud.fields.push_back(
      PJ::sdk::PointField{.name = "x", .offset = 0, .datatype = PJ::sdk::PointField::Datatype::kFloat32, .count = 1});
  const std::vector<std::uint8_t> data(12, 0);
  cloud.data = PJ::Span<const std::uint8_t>(data.data(), data.size());
  auto blob = PJ::serializePointCloud(cloud);

  const PJ::SessionManager::ParserBinding empty;  // parser == nullptr -> canonical path
  auto rec = pj::scene3d::resolveObject(empty, BT::kPointCloud, 1234, payloadOf(blob));
  ASSERT_TRUE(rec.has_value()) << rec.error();
  const auto* out = rec->object.get<PJ::sdk::PointCloud>();
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->frame_id, "lidar");
  EXPECT_EQ(out->width, 1u);
}

TEST(ResolveObject, DecodesCanonicalFrameTransformsWithoutParser) {
  PJ::sdk::FrameTransforms transforms;
  PJ::sdk::FrameTransform edge;
  edge.timestamp = 5;
  edge.parent_frame_id = "odom";
  edge.child_frame_id = "base_link";
  edge.rotation.w = 1.0;
  transforms.transforms.push_back(edge);
  auto blob = PJ::serializeFrameTransforms(transforms);

  const PJ::SessionManager::ParserBinding empty;
  auto rec = pj::scene3d::resolveObject(empty, BT::kFrameTransforms, 5, payloadOf(blob));
  ASSERT_TRUE(rec.has_value()) << rec.error();
  const auto* out = rec->object.get<PJ::sdk::FrameTransforms>();
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->transforms.size(), 1u);
  EXPECT_EQ(out->transforms[0].parent_frame_id, "odom");
  EXPECT_EQ(out->transforms[0].child_frame_id, "base_link");
}

TEST(ResolveObject, DecodesCanonicalPosesInFrameWithoutParser) {
  PJ::sdk::PosesInFrame poses;
  poses.frame_id = "map";
  poses.timestamp_ns = 77;
  PJ::sdk::Pose p;
  p.position = PJ::sdk::Vector3{.x = 1.0, .y = 2.0, .z = 3.0};
  p.orientation.w = 1.0;
  poses.poses.push_back(p);
  auto blob = PJ::serializePosesInFrame(poses);

  const PJ::SessionManager::ParserBinding empty;
  auto rec = pj::scene3d::resolveObject(empty, BT::kPosesInFrame, 77, payloadOf(blob));
  ASSERT_TRUE(rec.has_value()) << rec.error();
  const auto* out = rec->object.get<PJ::sdk::PosesInFrame>();
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->frame_id, "map");
  ASSERT_EQ(out->poses.size(), 1u);
  EXPECT_DOUBLE_EQ(out->poses[0].position.x, 1.0);
}

TEST(ResolveObject, ErrorsWhenNoParserAndTypeHasNoCanonicalCodec) {
  // kImage is a 2D type (no 3D canonical decode here) -> error, not a crash.
  const PJ::SessionManager::ParserBinding empty;
  auto rec = pj::scene3d::resolveObject(empty, BT::kImage, 0, payloadOf({1, 2, 3, 4}));
  EXPECT_FALSE(rec.has_value());
}

}  // namespace
