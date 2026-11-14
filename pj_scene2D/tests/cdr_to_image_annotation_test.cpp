// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "cdr_detection2d_to_image_annotation.h"
#include "cdr_yolo_to_image_annotation.h"
#include "marker_palette.h"
#include "nanocdr/nanocdr.hpp"
#include "pj_scene2d_core/scene_frame.h"

namespace pj_demos {
namespace {

// -----------------------------------------------------------------------------
// vision_msgs/Detection2DArray fixtures
// -----------------------------------------------------------------------------

std::vector<uint8_t> encodeDetection2DArray(
    int32_t header_sec, uint32_t header_nanosec, const std::string& frame_id,
    const std::vector<std::tuple<double, double, double, double>>& bboxes_cxcywh) {
  std::vector<uint8_t> storage;
  nanocdr::Encoder enc(nanocdr::CdrHeader{}, storage);

  enc.encode(static_cast<uint32_t>(header_sec));
  enc.encode(header_nanosec);
  enc.encode(frame_id);

  enc.encode(static_cast<uint32_t>(bboxes_cxcywh.size()));
  for (const auto& [cx, cy, w, h] : bboxes_cxcywh) {
    enc.encode(static_cast<uint32_t>(0));  // sec
    enc.encode(static_cast<uint32_t>(0));  // nanosec
    enc.encode(std::string(""));           // frame_id

    enc.encode(static_cast<uint32_t>(0));  // results[] size

    enc.encode(static_cast<double>(cx));
    enc.encode(static_cast<double>(cy));
    enc.encode(static_cast<double>(0.0));  // theta
    enc.encode(static_cast<double>(w));
    enc.encode(static_cast<double>(h));

    enc.encode(std::string(""));  // id
  }

  auto buf = enc.encodedBuffer();
  return std::vector<uint8_t>(buf.data(), buf.data() + buf.size());
}

TEST(CdrDetection2DTest, EmptyListProducesEmptyAnnotation) {
  auto bytes = encodeDetection2DArray(0, 0, "/camera", {});
  auto result = cdrDetection2DArrayToImageAnnotation(bytes.data(), bytes.size());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->points.empty());
  EXPECT_TRUE(result->texts.empty());
}

TEST(CdrDetection2DTest, ThreeBboxesProduceThreeLineLoops) {
  auto bytes = encodeDetection2DArray(
      1234, 567'000'000, "/camera/image",
      {
          {100.0, 200.0, 50.0, 30.0},
          {500.0, 400.0, 80.0, 80.0},
          {320.0, 240.0, 100.0, 60.0},
      });

  auto result = cdrDetection2DArrayToImageAnnotation(bytes.data(), bytes.size());
  ASSERT_TRUE(result.has_value());

  EXPECT_EQ(result->image_topic, "/camera/image");
  EXPECT_EQ(result->timestamp_ns, 1234LL * 1'000'000'000LL + 567'000'000LL);
  ASSERT_EQ(result->points.size(), 3u);

  for (const auto& bbox : result->points) {
    EXPECT_EQ(bbox.topology, PJ::AnnotationTopology::kLineLoop);
    EXPECT_EQ(bbox.points.size(), 4u);
  }

  // First bbox: cx=100, cy=200, w=50, h=30 → corners (75,185)-(125,185)-(125,215)-(75,215).
  const auto& first = result->points[0];
  EXPECT_DOUBLE_EQ(first.points[0].x, 75.0);
  EXPECT_DOUBLE_EQ(first.points[0].y, 185.0);
  EXPECT_DOUBLE_EQ(first.points[1].x, 125.0);
  EXPECT_DOUBLE_EQ(first.points[1].y, 185.0);
  EXPECT_DOUBLE_EQ(first.points[2].x, 125.0);
  EXPECT_DOUBLE_EQ(first.points[2].y, 215.0);
  EXPECT_DOUBLE_EQ(first.points[3].x, 75.0);
  EXPECT_DOUBLE_EQ(first.points[3].y, 215.0);
}

TEST(CdrDetection2DTest, TooSmallBufferReturnsError) {
  std::vector<uint8_t> tiny = {0, 0};
  auto result = cdrDetection2DArrayToImageAnnotation(tiny.data(), tiny.size());
  EXPECT_FALSE(result.has_value());
}

TEST(CdrDetection2DTest, NullDataReturnsError) {
  auto result = cdrDetection2DArrayToImageAnnotation(nullptr, 0);
  EXPECT_FALSE(result.has_value());
}

// -----------------------------------------------------------------------------
// yolo_msgs/DetectionArray fixtures
// -----------------------------------------------------------------------------

struct YoloDetection {
  int32_t class_id = 0;
  std::string class_name;
  double score = 0.0;
  std::string id;
  // BoundingBox2D
  double cx = 0.0;
  double cy = 0.0;
  double theta = 0.0;
  double sx = 0.0;
  double sy = 0.0;
  // Optional payloads — when empty, the decoder must still consume the (zero)
  // length prefix and continue cleanly to the next detection.
  int32_t mask_h = 0;
  int32_t mask_w = 0;
  std::vector<std::pair<double, double>> mask_points;
  std::vector<std::tuple<int32_t, double, double, double>> keypoints2d;  // {id, x, y, score}
  // KeyPoint3D = {id, x, y, z, score}
  std::vector<std::tuple<int32_t, double, double, double, double>> keypoints3d;
};

std::vector<uint8_t> encodeYoloDetectionArray(
    int32_t header_sec, uint32_t header_nanosec, const std::string& frame_id,
    const std::vector<YoloDetection>& detections) {
  std::vector<uint8_t> storage;
  nanocdr::Encoder enc(nanocdr::CdrHeader{}, storage);

  enc.encode(static_cast<uint32_t>(header_sec));
  enc.encode(header_nanosec);
  enc.encode(frame_id);

  enc.encode(static_cast<uint32_t>(detections.size()));
  for (const auto& d : detections) {
    enc.encode(d.class_id);
    enc.encode(d.class_name);
    enc.encode(d.score);
    enc.encode(d.id);

    // BoundingBox2D
    enc.encode(d.cx);
    enc.encode(d.cy);
    enc.encode(d.theta);
    enc.encode(d.sx);
    enc.encode(d.sy);

    // BoundingBox3D — 10 zero doubles + empty string
    for (int k = 0; k < 10; ++k) {
      enc.encode(static_cast<double>(0.0));
    }
    enc.encode(std::string(""));

    // Mask
    enc.encode(d.mask_h);
    enc.encode(d.mask_w);
    enc.encode(static_cast<uint32_t>(d.mask_points.size()));
    for (const auto& [mx, my] : d.mask_points) {
      enc.encode(mx);
      enc.encode(my);
    }

    // KeyPoint2DArray
    enc.encode(static_cast<uint32_t>(d.keypoints2d.size()));
    for (const auto& [kp_id, kx, ky, ks] : d.keypoints2d) {
      enc.encode(kp_id);
      enc.encode(kx);
      enc.encode(ky);
      enc.encode(ks);
    }

    // KeyPoint3DArray
    enc.encode(static_cast<uint32_t>(d.keypoints3d.size()));
    for (const auto& [kp_id, kx, ky, kz, ks] : d.keypoints3d) {
      enc.encode(kp_id);
      enc.encode(kx);
      enc.encode(ky);
      enc.encode(kz);
      enc.encode(ks);
    }
    enc.encode(std::string(""));  // KeyPoint3DArray frame_id
  }

  auto buf = enc.encodedBuffer();
  return std::vector<uint8_t>(buf.data(), buf.data() + buf.size());
}

TEST(CdrYoloTest, EmptyListProducesEmptyAnnotation) {
  auto bytes = encodeYoloDetectionArray(0, 0, "/camera", {});
  auto result = cdrYoloDetectionArrayToImageAnnotation(bytes.data(), bytes.size());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->points.empty());
  EXPECT_TRUE(result->texts.empty());
}

TEST(CdrYoloTest, TwoBboxesDistinctClassIdsGetDistinctColors) {
  // Two detections with class_id 0 (green) and class_id 1 (red) per
  // marker_palette.cpp:kClassPalette. The same class id between frames must
  // reproduce the same color — that's the property that keeps colors stable
  // during playback.
  YoloDetection d0;
  d0.class_id = 0;
  d0.class_name = "person";
  d0.score = 0.95;
  d0.cx = 100.0;
  d0.cy = 200.0;
  d0.sx = 50.0;
  d0.sy = 30.0;

  YoloDetection d1;
  d1.class_id = 1;
  d1.class_name = "car";
  d1.score = 0.80;
  d1.cx = 500.0;
  d1.cy = 400.0;
  d1.sx = 80.0;
  d1.sy = 80.0;

  auto bytes = encodeYoloDetectionArray(42, 0, "/camera", {d0, d1});
  auto result = cdrYoloDetectionArrayToImageAnnotation(bytes.data(), bytes.size());
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->points.size(), 2u);
  ASSERT_EQ(result->texts.size(), 2u);

  EXPECT_EQ(result->points[0].color, colorForClass(0));
  EXPECT_EQ(result->points[1].color, colorForClass(1));
  EXPECT_NE(result->points[0].color, result->points[1].color);

  EXPECT_EQ(result->texts[0].text, "person 0.95");
  EXPECT_EQ(result->texts[1].text, "car 0.80");
}

TEST(CdrYoloTest, PayloadWithMaskAndKeypointsDecodesWithoutOvershoot) {
  // Regression guard for the manual offset arithmetic inside the yolo
  // decoder. If the mask/keypoint loops over- or under-consume the wire,
  // the next detection's class_id reads garbage. Two detections with
  // non-empty payloads + correct outputs ⇒ we walked the wire correctly.
  YoloDetection d0;
  d0.class_id = 5;
  d0.class_name = "dog";
  d0.score = 0.7;
  d0.cx = 50.0;
  d0.cy = 60.0;
  d0.sx = 20.0;
  d0.sy = 20.0;
  d0.mask_h = 32;
  d0.mask_w = 32;
  d0.mask_points = {{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}};
  d0.keypoints2d = {{0, 1.0, 2.0, 0.9}, {1, 3.0, 4.0, 0.8}};
  d0.keypoints3d = {{0, 1.0, 2.0, 3.0, 0.9}};

  YoloDetection d1;
  d1.class_id = 7;  // distinct class so we can tell decoder didn't drift
  d1.class_name = "cat";
  d1.score = 0.85;
  d1.cx = 200.0;
  d1.cy = 300.0;
  d1.sx = 40.0;
  d1.sy = 40.0;

  auto bytes = encodeYoloDetectionArray(123, 456, "/camera", {d0, d1});
  auto result = cdrYoloDetectionArrayToImageAnnotation(bytes.data(), bytes.size());
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->points.size(), 2u);

  // If mask/keypoint walk overshot, d1's class_id would be wrong (and so its color).
  EXPECT_EQ(result->points[0].color, colorForClass(5));
  EXPECT_EQ(result->points[1].color, colorForClass(7));
  ASSERT_EQ(result->texts.size(), 2u);
  EXPECT_EQ(result->texts[1].text, "cat 0.85");

  // Bbox geometry of the second detection — proves we landed at the right offset.
  const auto& bbox1 = result->points[1];
  EXPECT_DOUBLE_EQ(bbox1.points[0].x, 180.0);
  EXPECT_DOUBLE_EQ(bbox1.points[0].y, 280.0);
  EXPECT_DOUBLE_EQ(bbox1.points[2].x, 220.0);
  EXPECT_DOUBLE_EQ(bbox1.points[2].y, 320.0);
}

TEST(CdrYoloTest, NullDataReturnsError) {
  auto result = cdrYoloDetectionArrayToImageAnnotation(nullptr, 0);
  EXPECT_FALSE(result.has_value());
}

}  // namespace
}  // namespace pj_demos
