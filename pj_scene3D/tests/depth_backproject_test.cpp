// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/depth_backproject.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace pj::scene3d {
namespace {

// K = [fx 0 cx; 0 fy cy; 0 0 1], row-major.
std::array<double, 9> makeK(double fx, double fy, double cx, double cy) {
  return {fx, 0.0, cx, 0.0, fy, cy, 0.0, 0.0, 1.0};
}

// Build a "32FC1" DepthImage (float32 metres) owning its bytes via the anchor, so
// the returned value's data Span stays valid for the test's lifetime.
PJ::sdk::DepthImage make32FC1(uint32_t w, uint32_t h, const std::vector<float>& depths_m) {
  auto buf = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h * 4U);
  std::memcpy(buf->data(), depths_m.data(), buf->size());
  PJ::sdk::DepthImage img;
  img.width = w;
  img.height = h;
  img.encoding = "32FC1";
  img.anchor = buf;
  img.data = PJ::Span<const uint8_t>(buf->data(), buf->size());
  return img;
}

PJ::sdk::DepthImage make16UC1(uint32_t w, uint32_t h, const std::vector<uint16_t>& depths_mm) {
  auto buf = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h * 2U);
  std::memcpy(buf->data(), depths_mm.data(), buf->size());
  PJ::sdk::DepthImage img;
  img.width = w;
  img.height = h;
  img.encoding = "16UC1";
  img.anchor = buf;
  img.data = PJ::Span<const uint8_t>(buf->data(), buf->size());
  return img;
}

constexpr float kEps = 1e-5f;

TEST(IntrinsicsFromK, ReadsComponentsAndValidity) {
  const auto intr = intrinsicsFromK(makeK(525.0, 530.0, 319.5, 239.5), 640, 480);
  EXPECT_TRUE(intr.valid());
  EXPECT_DOUBLE_EQ(intr.fx, 525.0);
  EXPECT_DOUBLE_EQ(intr.fy, 530.0);
  EXPECT_DOUBLE_EQ(intr.cx, 319.5);
  EXPECT_DOUBLE_EQ(intr.cy, 239.5);
  EXPECT_EQ(intr.width, 640U);
  EXPECT_EQ(intr.height, 480U);

  EXPECT_FALSE(intrinsicsFromK(std::array<double, 9>{}, 640, 480).valid());
}

TEST(DepthToPoints, Float32PinholeAndZeroCulling) {
  // 2x2 image; principal point at the pixel-grid centre (0.5, 0.5).
  // Depths (row-major): (0,0)=2, (1,0)=0 -> dropped, (0,1)=3, (1,1)=4.
  const auto img = make32FC1(2, 2, {2.0f, 0.0f, 3.0f, 4.0f});
  const auto intr = intrinsicsFromK(makeK(100.0, 100.0, 0.5, 0.5), 0, 0);

  std::vector<float> scalar;
  const auto pts = depthToPoints(img, intr, BackprojectOptions{}, &scalar);

  ASSERT_EQ(pts.size(), 3U);  // the z==0 pixel is dropped
  ASSERT_EQ(scalar.size(), 3U);

  // Iteration order is row-major over kept pixels: (0,0), (0,1), (1,1).
  EXPECT_NEAR(pts[0].x, -0.01f, kEps);
  EXPECT_NEAR(pts[0].y, -0.01f, kEps);
  EXPECT_NEAR(pts[0].z, 2.0f, kEps);

  EXPECT_NEAR(pts[1].x, -0.015f, kEps);
  EXPECT_NEAR(pts[1].y, 0.015f, kEps);
  EXPECT_NEAR(pts[1].z, 3.0f, kEps);

  EXPECT_NEAR(pts[2].x, 0.02f, kEps);
  EXPECT_NEAR(pts[2].y, 0.02f, kEps);
  EXPECT_NEAR(pts[2].z, 4.0f, kEps);

  EXPECT_NEAR(scalar[0], 2.0f, kEps);
  EXPECT_NEAR(scalar[1], 3.0f, kEps);
  EXPECT_NEAR(scalar[2], 4.0f, kEps);
}

TEST(DepthToPoints, Uint16MillimetreDecodeMatchesFloat) {
  const auto img = make16UC1(2, 2, {2000, 0, 3000, 4000});
  const auto intr = intrinsicsFromK(makeK(100.0, 100.0, 0.5, 0.5), 0, 0);

  const auto pts = depthToPoints(img, intr, BackprojectOptions{}, nullptr);
  ASSERT_EQ(pts.size(), 3U);
  EXPECT_NEAR(pts[0].z, 2.0f, kEps);
  EXPECT_NEAR(pts[1].z, 3.0f, kEps);
  EXPECT_NEAR(pts[2].z, 4.0f, kEps);
  EXPECT_NEAR(pts[2].x, 0.02f, kEps);
}

TEST(DepthToPoints, MaxDepthClip) {
  const auto img = make32FC1(2, 2, {2.0f, 0.0f, 3.0f, 4.0f});
  const auto intr = intrinsicsFromK(makeK(100.0, 100.0, 0.5, 0.5), 0, 0);

  BackprojectOptions opts;
  opts.max_depth_m = 3.5f;  // drops the z==4 point
  const auto pts = depthToPoints(img, intr, opts, nullptr);
  ASSERT_EQ(pts.size(), 2U);
  EXPECT_NEAR(pts[0].z, 2.0f, kEps);
  EXPECT_NEAR(pts[1].z, 3.0f, kEps);
}

TEST(DepthToPoints, NonFiniteDropped) {
  const float kInf = std::numeric_limits<float>::infinity();
  const float kNan = std::numeric_limits<float>::quiet_NaN();
  const auto img = make32FC1(2, 2, {kInf, kNan, 5.0f, 6.0f});
  const auto intr = intrinsicsFromK(makeK(100.0, 100.0, 0.5, 0.5), 0, 0);

  const auto pts = depthToPoints(img, intr, BackprojectOptions{}, nullptr);
  ASSERT_EQ(pts.size(), 2U);  // inf and nan dropped
  EXPECT_NEAR(pts[0].z, 5.0f, kEps);
  EXPECT_NEAR(pts[1].z, 6.0f, kEps);
}

TEST(DepthToPoints, StrideDecimates) {
  // 4x4 all-valid; stride 2 keeps u,v in {0,2} -> 4 points.
  std::vector<float> depths(16, 1.0f);
  const auto img = make32FC1(4, 4, depths);
  const auto intr = intrinsicsFromK(makeK(100.0, 100.0, 1.5, 1.5), 0, 0);

  BackprojectOptions opts;
  opts.stride = 2;
  EXPECT_EQ(depthToPoints(img, intr, opts, nullptr).size(), 4U);
  opts.stride = 0;
  EXPECT_EQ(depthToPoints(img, intr, opts, nullptr).size(), 16U);
}

TEST(DepthToPoints, IntrinsicsRescaledToImageResolution) {
  // Intrinsics calibrated at 4x4 (cx=cy=1, fx=fy=200) but the depth image is 2x2.
  // Effective scale = 2/4 = 0.5 -> fx_eff=100, cx_eff=0.5. Single centre-ish pixel.
  const auto img = make32FC1(2, 2, {0.0f, 0.0f, 0.0f, 7.0f});  // only (1,1) valid
  const auto intr = intrinsicsFromK(makeK(200.0, 200.0, 1.0, 1.0), 4, 4);

  const auto pts = depthToPoints(img, intr, BackprojectOptions{}, nullptr);
  ASSERT_EQ(pts.size(), 1U);
  // u=v=1, z=7, fx_eff=100, cx_eff=0.5 -> X=Y=(1-0.5)*7/100=0.035.
  EXPECT_NEAR(pts[0].x, 0.035f, kEps);
  EXPECT_NEAR(pts[0].y, 0.035f, kEps);
  EXPECT_NEAR(pts[0].z, 7.0f, kEps);
}

TEST(DepthToPoints, BoundsAndScalarRangeAccumulatedInOnePass) {
  // Same 3-point geometry as Float32PinholeAndZeroCulling: kept points are
  //   (-0.01,-0.01,2), (-0.015,0.015,3), (0.02,0.02,4).
  const auto img = make32FC1(2, 2, {2.0f, 0.0f, 3.0f, 4.0f});
  const auto intr = intrinsicsFromK(makeK(100.0, 100.0, 0.5, 0.5), 0, 0);

  std::vector<float> scalar;
  AABB bounds;
  PJ::Range<float> range{-99.0f, -99.0f};
  const auto pts = depthToPoints(img, intr, BackprojectOptions{}, &scalar, &bounds, &range);
  ASSERT_EQ(pts.size(), 3U);

  ASSERT_TRUE(bounds.valid);
  EXPECT_NEAR(bounds.min.x, -0.015f, kEps);
  EXPECT_NEAR(bounds.min.y, -0.01f, kEps);
  EXPECT_NEAR(bounds.min.z, 2.0f, kEps);
  EXPECT_NEAR(bounds.max.x, 0.02f, kEps);
  EXPECT_NEAR(bounds.max.y, 0.02f, kEps);
  EXPECT_NEAR(bounds.max.z, 4.0f, kEps);

  EXPECT_NEAR(range.min, 2.0f, kEps);
  EXPECT_NEAR(range.max, 4.0f, kEps);
}

TEST(DepthToPoints, EmptyResultGivesInvalidBoundsAndUnitRange) {
  const auto img = make32FC1(2, 2, {0.0f, 0.0f, 0.0f, 0.0f});
  const auto intr = intrinsicsFromK(makeK(100.0, 100.0, 0.5, 0.5), 0, 0);

  AABB bounds;
  PJ::Range<float> range{-99.0f, -99.0f};
  const auto pts = depthToPoints(img, intr, BackprojectOptions{}, nullptr, &bounds, &range);
  ASSERT_TRUE(pts.empty());
  EXPECT_FALSE(bounds.valid);
  EXPECT_FLOAT_EQ(range.min, 0.0f);
  EXPECT_FLOAT_EQ(range.max, 1.0f);
}

TEST(DepthToPoints, DegenerateScalarRangeWidened) {
  // All kept points share one depth -> a non-degenerate range {z, z+1}.
  const auto img = make32FC1(2, 2, {5.0f, 5.0f, 5.0f, 5.0f});
  const auto intr = intrinsicsFromK(makeK(100.0, 100.0, 0.5, 0.5), 0, 0);

  PJ::Range<float> range{-99.0f, -99.0f};
  const auto pts = depthToPoints(img, intr, BackprojectOptions{}, nullptr, nullptr, &range);
  ASSERT_EQ(pts.size(), 4U);
  EXPECT_FLOAT_EQ(range.min, 5.0f);
  EXPECT_FLOAT_EQ(range.max, 6.0f);
}

TEST(DepthToPoints, RejectsBadEncodingAndShortBuffer) {
  const auto intr = intrinsicsFromK(makeK(100.0, 100.0, 0.5, 0.5), 0, 0);

  auto bad_enc = make32FC1(2, 2, {1.0f, 1.0f, 1.0f, 1.0f});
  bad_enc.encoding = "rgb8";
  EXPECT_TRUE(depthToPoints(bad_enc, intr, BackprojectOptions{}, nullptr).empty());

  // Truncated buffer: declares 2x2 but only one float of data.
  auto truncated = make32FC1(2, 2, {1.0f, 1.0f, 1.0f, 1.0f});
  truncated.data = PJ::Span<const uint8_t>(truncated.data.data(), 4U);
  EXPECT_TRUE(depthToPoints(truncated, intr, BackprojectOptions{}, nullptr).empty());

  EXPECT_TRUE(depthToPoints(make32FC1(2, 2, {1, 1, 1, 1}), DepthIntrinsics{}, BackprojectOptions{}, nullptr).empty());
}

}  // namespace
}  // namespace pj::scene3d
