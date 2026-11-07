// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// EVAL-05: the summary of an object output that an evaluation report carries instead of its bytes.

#include "pj_scripting/object_summary.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "pj_base/buffer_anchor.hpp"
#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_base/builtin/image_annotations.hpp"
#include "pj_base/builtin/log.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/scene_entities.hpp"

namespace PJ::scripting {
namespace {

using sdk::PointCloud;
using sdk::PointField;

/// A flat XYZI cloud of float32 points (16 bytes each).
PointCloud makeXyziCloud(const std::vector<std::array<float, 4>>& points) {
  PointCloud cloud;
  cloud.width = static_cast<uint32_t>(points.size());
  cloud.height = 1;
  cloud.point_step = 16;
  cloud.row_step = cloud.point_step * cloud.width;
  cloud.is_dense = true;
  cloud.frame_id = "base_link";
  cloud.fields = {
      PointField{.name = "x", .offset = 0, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "y", .offset = 4, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "z", .offset = 8, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "intensity", .offset = 12, .datatype = PointField::Datatype::kFloat32, .count = 1},
  };
  cloud.timestamp_ns = 1'000'000'000;

  std::vector<uint8_t> bytes(static_cast<size_t>(cloud.point_step) * points.size());
  for (size_t i = 0; i < points.size(); ++i) {
    std::memcpy(bytes.data() + i * cloud.point_step, points[i].data(), sizeof(float) * 4);
  }
  sdk::PayloadView view = sdk::makePayloadView(std::move(bytes));
  cloud.data = view.bytes;
  cloud.anchor = view.anchor;
  return cloud;
}

TEST(ObjectSummaryTest, PointCloudCountsAndBounds) {
  const PointCloud cloud = makeXyziCloud({{0, 0, 0, 0}, {1, 2, 3, 0}, {-1, -2, -3, 0}});
  const nlohmann::json doc = summarizeObject(sdk::BuiltinObject(cloud));

  EXPECT_EQ(doc.at("type").get<std::string>(), "kPointCloud");
  EXPECT_EQ(doc.at("frame_id").get<std::string>(), "base_link");
  EXPECT_EQ(doc.at("points").get<size_t>(), 3u);
  ASSERT_TRUE(doc.contains("bounds"));
  EXPECT_EQ(doc["bounds"]["min"].get<std::vector<double>>(), (std::vector<double>{-1, -2, -3}));
  EXPECT_EQ(doc["bounds"]["max"].get<std::vector<double>>(), (std::vector<double>{1, 2, 3}));
  EXPECT_FALSE(isEmptyObject(sdk::BuiltinObject(cloud)));
}

TEST(ObjectSummaryTest, EmptyPointCloudHasNoBoundsKey) {
  const PointCloud cloud = makeXyziCloud({});
  const nlohmann::json doc = summarizeObject(sdk::BuiltinObject(cloud));

  EXPECT_EQ(doc.at("points").get<size_t>(), 0u);
  EXPECT_FALSE(doc.contains("bounds"));
  EXPECT_TRUE(isEmptyObject(sdk::BuiltinObject(cloud)));
}

TEST(ObjectSummaryTest, SceneEntitiesCountsAndFrameIds) {
  sdk::SceneEntities scene;
  sdk::SceneEntity a;
  a.frame_id = "map";
  a.id = "a";
  sdk::SceneEntity b;
  b.frame_id = "map";  // a repeated frame id is listed once
  b.id = "b";
  sdk::SceneEntity c;
  c.frame_id = "base_link";
  c.id = "c";
  scene.entities = {a, b, c};
  scene.deletions.push_back(sdk::SceneEntityDeletion{sdk::SceneEntityDeletion::Type::kAll, 0, {}});

  const nlohmann::json doc = summarizeObject(sdk::BuiltinObject(scene));
  EXPECT_EQ(doc.at("type").get<std::string>(), "kSceneEntities");
  EXPECT_EQ(doc.at("entities").get<size_t>(), 3u);
  EXPECT_EQ(doc.at("deletions").get<size_t>(), 1u);
  EXPECT_EQ(doc.at("frame_ids").get<std::vector<std::string>>(), (std::vector<std::string>{"map", "base_link"}));
  EXPECT_FALSE(isEmptyObject(sdk::BuiltinObject(scene)));

  // A clear-only snapshot has no content: a deletion is not content.
  sdk::SceneEntities cleared;
  cleared.deletions.push_back(sdk::SceneEntityDeletion{sdk::SceneEntityDeletion::Type::kAll, 0, {}});
  EXPECT_TRUE(isEmptyObject(sdk::BuiltinObject(cleared)));
}

TEST(ObjectSummaryTest, ImageAnnotationsCountsPrimitiveLists) {
  sdk::ImageAnnotations annotations;
  annotations.circles.resize(2);
  annotations.points.resize(1);
  annotations.texts.resize(3);

  const nlohmann::json doc = summarizeObject(sdk::BuiltinObject(annotations));
  EXPECT_EQ(doc.at("type").get<std::string>(), "kImageAnnotations");
  EXPECT_EQ(doc.at("circles").get<size_t>(), 2u);
  EXPECT_EQ(doc.at("points").get<size_t>(), 1u);
  EXPECT_EQ(doc.at("texts").get<size_t>(), 3u);
  EXPECT_FALSE(isEmptyObject(sdk::BuiltinObject(annotations)));
  EXPECT_TRUE(isEmptyObject(sdk::BuiltinObject(sdk::ImageAnnotations{})));
}

TEST(ObjectSummaryTest, FrameTransformsCountsAndParents) {
  sdk::FrameTransforms transforms;
  sdk::FrameTransform t1;
  t1.parent_frame_id = "map";
  t1.child_frame_id = "odom";
  sdk::FrameTransform t2;
  t2.parent_frame_id = "map";  // a repeated parent is listed once
  t2.child_frame_id = "base_link";
  transforms.transforms = {t1, t2};

  const nlohmann::json doc = summarizeObject(sdk::BuiltinObject(transforms));
  EXPECT_EQ(doc.at("type").get<std::string>(), "kFrameTransforms");
  EXPECT_EQ(doc.at("transforms").get<size_t>(), 2u);
  EXPECT_EQ(doc.at("parents").get<std::vector<std::string>>(), (std::vector<std::string>{"map"}));
  EXPECT_FALSE(isEmptyObject(sdk::BuiltinObject(transforms)));
  EXPECT_TRUE(isEmptyObject(sdk::BuiltinObject(sdk::FrameTransforms{})));
}

TEST(ObjectSummaryTest, UnrecognizedTypeSummarizesAsTypeOnly) {
  sdk::Log log;
  log.level = sdk::Log::Level::kInfo;
  log.message = "hello";
  log.name = "node";

  const nlohmann::json doc = summarizeObject(sdk::BuiltinObject(log));
  EXPECT_EQ(doc.at("type").get<std::string>(), "kLog");
  EXPECT_EQ(doc.size(), 1u);
  EXPECT_FALSE(isEmptyObject(sdk::BuiltinObject(log)));
}

}  // namespace
}  // namespace PJ::scripting
