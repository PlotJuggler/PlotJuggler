// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Direct unit tests for the raw PointCloud byte readers in pointcloud_convert.
// These parse arbitrary little-endian field layouts straight from untrusted bytes,
// so the coverage targets sign extension, float64 truncation, and — crucially — the
// offset/buffer validation that is the renderer's last line of defense against an
// out-of-bounds heap read on a malformed/hostile cloud (M.23/M.24).

#include "pj_scene3d_core/pointcloud_convert.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

#include "pj_base/span.hpp"
#include "pj_scene3d_core/grid_map_view.h"  // finiteRange

namespace {

using PJ::Span;
using pj::scene3d::ColorLayout;
using pj::scene3d::convertCanonical;
using pj::scene3d::ConvertedPointCloud;
using pj::scene3d::detectColorLayout;
using pj::scene3d::PackedPointVertices;
using pj::scene3d::packPointVertices;
using pj::scene3d::planPointVertices;
using pj::scene3d::PointVertex;
using pj::scene3d::PointVertexFormat;
using pj::scene3d::PointVertexPlan;
using pj::scene3d::readFloat32At;
using pj::scene3d::readFloat64At;
using pj::scene3d::readScalarAt;
using PJ::sdk::PointCloud;
using PJ::sdk::PointField;
using DT = PointField::Datatype;

// Canonical packed color: byte0=R, byte1=G, byte2=B, byte3=A (little-endian uint32).
constexpr uint32_t packRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) |
         (static_cast<uint32_t>(a) << 24);
}

// --- Little-endian byte writers -------------------------------------------------

void putBytes(std::vector<uint8_t>& buffer, const void* src, std::size_t count) {
  const auto* bytes = static_cast<const uint8_t*>(src);
  buffer.insert(buffer.end(), bytes, bytes + count);
}

template <typename Scalar>
std::vector<uint8_t> leBytes(Scalar value) {
  // The host running the test suite is little-endian (x86); std::bit-pattern of the
  // native value already matches the wire layout the readers assemble byte-by-byte.
  std::vector<uint8_t> out(sizeof(Scalar));
  std::memcpy(out.data(), &value, sizeof(Scalar));
  return out;
}

// Wrap an owned byte buffer into a PointCloud, anchoring the bytes so the Span stays
// valid for the cloud's lifetime.
PointCloud makeCloud(uint32_t width, uint32_t point_step, std::vector<PointField> fields, std::vector<uint8_t> bytes) {
  auto owned = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  PointCloud cloud;
  cloud.width = width;
  cloud.height = 1;
  cloud.point_step = point_step;
  cloud.row_step = point_step * width;
  cloud.is_bigendian = false;
  cloud.frame_id = "lidar";
  cloud.fields = std::move(fields);
  cloud.data = Span<const uint8_t>(owned->data(), owned->size());
  cloud.anchor = owned;
  return cloud;
}

// --- Raw scalar reader: sign extension and float decode -------------------------

TEST(PointCloudConvertScalar, Int8SignExtends) {
  const auto bytes = leBytes<int8_t>(-5);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kInt8), -5.0f);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kUint8), 251.0f);  // same byte read unsigned
}

TEST(PointCloudConvertScalar, Int16SignExtends) {
  const auto bytes = leBytes<int16_t>(-1234);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kInt16), -1234.0f);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kUint16), 64302.0f);  // 65536 - 1234
}

TEST(PointCloudConvertScalar, Int32SignExtends) {
  const auto bytes = leBytes<int32_t>(-70000);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kInt32), -70000.0f);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kUint32), 4294897296.0f);  // 2^32 - 70000
}

TEST(PointCloudConvertScalar, Float32And64) {
  const auto f32 = leBytes<float>(3.5f);
  EXPECT_FLOAT_EQ(readFloat32At(f32.data()), 3.5f);
  EXPECT_FLOAT_EQ(readScalarAt(f32.data(), DT::kFloat32), 3.5f);

  const auto f64 = leBytes<double>(2.718281828459045);
  EXPECT_DOUBLE_EQ(readFloat64At(f64.data()), 2.718281828459045);
  // float64 -> float32 truncation: the scalar reader narrows to float.
  EXPECT_FLOAT_EQ(readScalarAt(f64.data(), DT::kFloat64), static_cast<float>(2.718281828459045));
}

TEST(PointCloudConvertScalar, UnknownDatatypeYieldsZero) {
  const std::vector<uint8_t> bytes(8, 0xFF);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kUnknown), 0.0f);
}

// --- convertCanonical: happy path -----------------------------------------------

TEST(PointCloudConvert, DecodesXyzAndIntScalarWithSignExtension) {
  // Layout per point (point_step = 16): x f32, y f32, z f32, intensity int16 (+ 2 pad).
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat32, 1},
      {"y", 4, DT::kFloat32, 1},
      {"z", 8, DT::kFloat32, 1},
      {"intensity", 12, DT::kInt16, 1},
  };
  std::vector<uint8_t> bytes;
  auto push_point = [&](float x, float y, float z, int16_t scalar) {
    putBytes(bytes, leBytes(x).data(), 4);
    putBytes(bytes, leBytes(y).data(), 4);
    putBytes(bytes, leBytes(z).data(), 4);
    putBytes(bytes, leBytes(scalar).data(), 2);
    bytes.insert(bytes.end(), {0, 0});  // 2 bytes padding to reach point_step 16
  };
  push_point(1.0f, 2.0f, 3.0f, -100);
  push_point(-4.0f, 5.0f, 6.0f, 200);

  const PointCloud cloud = makeCloud(/*width=*/2, /*point_step=*/16, std::move(fields), std::move(bytes));
  const ConvertedPointCloud out = convertCanonical(cloud, "intensity");

  ASSERT_EQ(out.cloud.positions.size(), 2u);
  EXPECT_FLOAT_EQ(out.cloud.positions[0].x, 1.0f);
  EXPECT_FLOAT_EQ(out.cloud.positions[0].y, 2.0f);
  EXPECT_FLOAT_EQ(out.cloud.positions[0].z, 3.0f);
  EXPECT_FLOAT_EQ(out.cloud.positions[1].x, -4.0f);

  ASSERT_EQ(out.cloud.scalar.size(), 2u);
  EXPECT_FLOAT_EQ(out.cloud.scalar[0], -100.0f);  // signed read, not 65436
  EXPECT_FLOAT_EQ(out.cloud.scalar[1], 200.0f);
  EXPECT_EQ(out.cloud.scalar_field_name, "intensity");
  EXPECT_EQ(out.cloud.frame_id, "lidar");

  // AABB accumulated in the same pass spans both points (L.50).
  ASSERT_TRUE(out.bounds.valid);
  EXPECT_FLOAT_EQ(out.bounds.min.x, -4.0f);
  EXPECT_FLOAT_EQ(out.bounds.max.x, 1.0f);
  EXPECT_FLOAT_EQ(out.bounds.max.z, 6.0f);
}

TEST(PointCloudConvert, Float64Coordinates) {
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat64, 1},
      {"y", 8, DT::kFloat64, 1},
      {"z", 16, DT::kFloat64, 1},
  };
  std::vector<uint8_t> bytes;
  putBytes(bytes, leBytes<double>(1.25).data(), 8);
  putBytes(bytes, leBytes<double>(-2.5).data(), 8);
  putBytes(bytes, leBytes<double>(100.0).data(), 8);

  const PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/24, std::move(fields), std::move(bytes));
  const ConvertedPointCloud out = convertCanonical(cloud, "");

  ASSERT_EQ(out.cloud.positions.size(), 1u);
  EXPECT_FLOAT_EQ(out.cloud.positions[0].x, 1.25f);
  EXPECT_FLOAT_EQ(out.cloud.positions[0].y, -2.5f);
  EXPECT_FLOAT_EQ(out.cloud.positions[0].z, 100.0f);
  EXPECT_TRUE(out.cloud.scalar.empty());
}

TEST(PointCloudConvert, NonFinitePointsExcludedFromBounds) {
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat32, 1},
      {"y", 4, DT::kFloat32, 1},
      {"z", 8, DT::kFloat32, 1},
  };
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::vector<uint8_t> bytes;
  auto push_point = [&](float x, float y, float z) {
    putBytes(bytes, leBytes(x).data(), 4);
    putBytes(bytes, leBytes(y).data(), 4);
    putBytes(bytes, leBytes(z).data(), 4);
  };
  push_point(nan, nan, nan);     // dropped from AABB
  push_point(7.0f, 8.0f, 9.0f);  // sole finite point defines the box

  const PointCloud cloud = makeCloud(/*width=*/2, /*point_step=*/12, std::move(fields), std::move(bytes));
  const ConvertedPointCloud out = convertCanonical(cloud, "");

  ASSERT_EQ(out.cloud.positions.size(), 2u);  // both points still decoded into positions
  ASSERT_TRUE(out.bounds.valid);
  EXPECT_FLOAT_EQ(out.bounds.min.x, 7.0f);
  EXPECT_FLOAT_EQ(out.bounds.max.x, 7.0f);
}

// --- Color detection + extraction (RGB-direct mode) -----------------------------

TEST(PointCloudColor, DetectsPackedRgbaField) {
  // Canonical normalized layout: a single uint32 'rgba' field.
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}, {"rgba", 12, DT::kUint32, 1}};
  const PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/16, std::move(fields), std::vector<uint8_t>(16, 0));

  const ColorLayout layout = detectColorLayout(cloud);
  ASSERT_TRUE(layout.valid);
  EXPECT_EQ(layout.r_offset, 12u);
  EXPECT_EQ(layout.g_offset, 13u);
  EXPECT_EQ(layout.b_offset, 14u);
  EXPECT_TRUE(layout.has_alpha);
  EXPECT_EQ(layout.a_offset, 15u);
}

TEST(PointCloudColor, DetectsSeparateRgbaChannels) {
  // The raw foxglove layout the parser has not yet normalized: red/green/blue/alpha uint8.
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1},   {"y", 4, DT::kFloat32, 1},
                                    {"z", 8, DT::kFloat32, 1},   {"intensity", 12, DT::kFloat32, 1},
                                    {"red", 16, DT::kUint8, 1},  {"green", 17, DT::kUint8, 1},
                                    {"blue", 18, DT::kUint8, 1}, {"alpha", 19, DT::kUint8, 1}};
  const PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/20, std::move(fields), std::vector<uint8_t>(20, 0));

  const ColorLayout layout = detectColorLayout(cloud);
  ASSERT_TRUE(layout.valid);
  EXPECT_EQ(layout.r_offset, 16u);
  EXPECT_EQ(layout.g_offset, 17u);
  EXPECT_EQ(layout.b_offset, 18u);
  EXPECT_TRUE(layout.has_alpha);
  EXPECT_EQ(layout.a_offset, 19u);
}

TEST(PointCloudColor, SeparateRgbWithoutAlpha) {
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1},    {"y", 4, DT::kFloat32, 1},
                                    {"z", 8, DT::kFloat32, 1},    {"red", 12, DT::kUint8, 1},
                                    {"green", 13, DT::kUint8, 1}, {"blue", 14, DT::kUint8, 1}};
  const PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/16, std::move(fields), std::vector<uint8_t>(16, 0));

  const ColorLayout layout = detectColorLayout(cloud);
  ASSERT_TRUE(layout.valid);
  EXPECT_FALSE(layout.has_alpha);
}

TEST(PointCloudColor, NoColorFieldsReturnsInvalid) {
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat32, 1},
      {"y", 4, DT::kFloat32, 1},
      {"z", 8, DT::kFloat32, 1},
      {"intensity", 12, DT::kFloat32, 1}};
  const PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/16, std::move(fields), std::vector<uint8_t>(16, 0));

  EXPECT_FALSE(detectColorLayout(cloud).valid);
}

TEST(PointCloudConvert, ExtractsPackedRgbaPerPoint) {
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}, {"rgba", 12, DT::kUint32, 1}};
  std::vector<uint8_t> bytes;
  auto push_point = [&](float x, float y, float z, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    putBytes(bytes, leBytes(x).data(), 4);
    putBytes(bytes, leBytes(y).data(), 4);
    putBytes(bytes, leBytes(z).data(), 4);
    bytes.insert(bytes.end(), {r, g, b, a});  // canonical R,G,B,A increasing-address order
  };
  push_point(1.0f, 2.0f, 3.0f, 107, 142, 35, 255);  // olivedrab (the av-ds semantic color)
  push_point(4.0f, 5.0f, 6.0f, 10, 20, 30, 200);

  const PointCloud cloud = makeCloud(/*width=*/2, /*point_step=*/16, std::move(fields), std::move(bytes));
  const ConvertedPointCloud out = convertCanonical(cloud, /*scalar_field=*/"", /*extract_rgba=*/true);

  ASSERT_EQ(out.cloud.positions.size(), 2u);
  ASSERT_EQ(out.cloud.rgba.size(), 2u);
  EXPECT_EQ(out.cloud.rgba[0], packRgba(107, 142, 35, 255));
  EXPECT_EQ(out.cloud.rgba[1], packRgba(10, 20, 30, 200));
  EXPECT_TRUE(out.cloud.scalar.empty());  // RGB mode bypasses the colormap scalar
}

TEST(PointCloudConvert, ExtractsSeparateChannelsAsPackedRgba) {
  // Exactly the user's foxglove cloud layout (red/green/blue/alpha uint8 at 16..19).
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1},   {"y", 4, DT::kFloat32, 1},
                                    {"z", 8, DT::kFloat32, 1},   {"intensity", 12, DT::kFloat32, 1},
                                    {"red", 16, DT::kUint8, 1},  {"green", 17, DT::kUint8, 1},
                                    {"blue", 18, DT::kUint8, 1}, {"alpha", 19, DT::kUint8, 1}};
  std::vector<uint8_t> bytes;
  putBytes(bytes, leBytes(1.0f).data(), 4);
  putBytes(bytes, leBytes(2.0f).data(), 4);
  putBytes(bytes, leBytes(3.0f).data(), 4);
  putBytes(bytes, leBytes(0.0f).data(), 4);  // intensity
  bytes.insert(bytes.end(), {107, 142, 35, 255});

  const PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/20, std::move(fields), std::move(bytes));
  const ConvertedPointCloud out = convertCanonical(cloud, /*scalar_field=*/"", /*extract_rgba=*/true);

  ASSERT_EQ(out.cloud.rgba.size(), 1u);
  EXPECT_EQ(out.cloud.rgba[0], packRgba(107, 142, 35, 255));
}

TEST(PointCloudConvert, RgbWithoutAlphaDefaultsOpaque) {
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1},    {"y", 4, DT::kFloat32, 1},
                                    {"z", 8, DT::kFloat32, 1},    {"red", 12, DT::kUint8, 1},
                                    {"green", 13, DT::kUint8, 1}, {"blue", 14, DT::kUint8, 1}};
  std::vector<uint8_t> bytes;
  putBytes(bytes, leBytes(1.0f).data(), 4);
  putBytes(bytes, leBytes(2.0f).data(), 4);
  putBytes(bytes, leBytes(3.0f).data(), 4);
  bytes.insert(bytes.end(), {10, 20, 30, 0});  // 4th byte is padding, NOT alpha

  const PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/16, std::move(fields), std::move(bytes));
  const ConvertedPointCloud out = convertCanonical(cloud, /*scalar_field=*/"", /*extract_rgba=*/true);

  ASSERT_EQ(out.cloud.rgba.size(), 1u);
  EXPECT_EQ(out.cloud.rgba[0], packRgba(10, 20, 30, 255));  // alpha forced opaque
}

TEST(PointCloudConvert, RejectsColorChannelOffsetPastPointStep) {
  // alpha at offset 19 but point_step=19 -> read of byte 19 is past the point. Reject (empty).
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1},    {"y", 4, DT::kFloat32, 1},
                                    {"z", 8, DT::kFloat32, 1},    {"red", 16, DT::kUint8, 1},
                                    {"green", 17, DT::kUint8, 1}, {"blue", 18, DT::kUint8, 1},
                                    {"alpha", 19, DT::kUint8, 1}};
  const PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/19, std::move(fields), std::vector<uint8_t>(19, 0));

  const ConvertedPointCloud out = convertCanonical(cloud, /*scalar_field=*/"", /*extract_rgba=*/true);
  EXPECT_TRUE(out.cloud.positions.empty());
  EXPECT_TRUE(out.cloud.rgba.empty());
}

// --- convertCanonical: rejection (returns empty) --------------------------------

TEST(PointCloudConvert, RejectsBigEndian) {
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
  PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/12, std::move(fields), std::vector<uint8_t>(12, 0));
  cloud.is_bigendian = true;

  const ConvertedPointCloud out = convertCanonical(cloud, "");
  EXPECT_TRUE(out.cloud.positions.empty());
  EXPECT_FALSE(out.bounds.valid);
}

TEST(PointCloudConvert, RejectsBufferTooSmallForDeclaredPoints) {
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
  // width=4 points * point_step=12 wants 48 bytes; supply only 24.
  PointCloud cloud = makeCloud(/*width=*/4, /*point_step=*/12, std::move(fields), std::vector<uint8_t>(24, 0));

  const ConvertedPointCloud out = convertCanonical(cloud, "");
  EXPECT_TRUE(out.cloud.positions.empty());
}

TEST(PointCloudConvert, RejectsMissingSpatialField) {
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}};  // no z
  PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/8, std::move(fields), std::vector<uint8_t>(8, 0));

  const ConvertedPointCloud out = convertCanonical(cloud, "");
  EXPECT_TRUE(out.cloud.positions.empty());
}

TEST(PointCloudConvert, RejectsFieldOffsetPastPointStep) {
  // M.23/M.24: z declares a float32 at offset 8 with point_step=10 -> read crosses
  // the point boundary (8 + 4 > 10). Must be rejected BEFORE any read.
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
  PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/10, std::move(fields), std::vector<uint8_t>(10, 0));

  const ConvertedPointCloud out = convertCanonical(cloud, "");
  EXPECT_TRUE(out.cloud.positions.empty());
}

TEST(PointCloudConvert, RejectsScalarOffsetPastPointStep) {
  // x/y/z fit (12 bytes), but the selected scalar at offset 14 reads past point_step=16.
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat32, 1},
      {"y", 4, DT::kFloat32, 1},
      {"z", 8, DT::kFloat32, 1},
      {"intensity", 14, DT::kFloat32, 1},  // 14 + 4 = 18 > 16
  };
  PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/16, std::move(fields), std::vector<uint8_t>(16, 0));

  const ConvertedPointCloud out = convertCanonical(cloud, "intensity");
  EXPECT_TRUE(out.cloud.positions.empty());
}

TEST(PointCloudConvert, MalformedScalarOffsetDoesNotPoisonHugeAdversarialValue) {
  // An adversarial uint32 offset (4 GiB) would index arbitrarily far out of bounds;
  // the offset+size computed in uint64 must still reject without overflowing.
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat32, 1},
      {"y", 4, DT::kFloat32, 1},
      {"z", 8, DT::kFloat32, 1},
      {"intensity", 0xFFFFFFFCu, DT::kFloat32, 1},  // offset+4 wraps uint32 if not widened
  };
  PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/12, std::move(fields), std::vector<uint8_t>(12, 0));

  const ConvertedPointCloud out = convertCanonical(cloud, "intensity");
  EXPECT_TRUE(out.cloud.positions.empty());
}

// --- packPointVertices: equivalence with convertCanonical + packing ------------

// The two-step path packPointVertices replaces: convertCanonical, then every point
// with a finite position packed with its scalar or colour in the format they need,
// the scalar range taken over every decoded scalar.
void expectPackMatches(const PointCloud& cloud, std::string_view scalar_field, bool extract_rgba, bool store_scalar) {
  SCOPED_TRACE(
      ::testing::Message() << "scalar='" << scalar_field << "' rgba=" << extract_rgba << " store=" << store_scalar);
  const ConvertedPointCloud reference = convertCanonical(cloud, scalar_field, extract_rgba);
  const auto& decoded = reference.cloud;
  const bool has_scalar = !decoded.positions.empty() && decoded.scalar.size() == decoded.positions.size();
  const bool has_rgba = !decoded.positions.empty() && decoded.rgba.size() == decoded.positions.size();
  const PointVertexFormat format = has_rgba                     ? PointVertexFormat::kXyzRgba
                                   : has_scalar && store_scalar ? PointVertexFormat::kXyzScalar
                                                                : PointVertexFormat::kXyz;
  const std::size_t stride = pj::scene3d::pointVertexStride(format);
  std::vector<std::byte> expected;
  for (std::size_t index = 0; index < decoded.positions.size(); ++index) {
    const auto& position = decoded.positions[index];
    if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) {
      const PointVertex vertex{
          .x = position.x,
          .y = position.y,
          .z = position.z,
          .attribute = has_rgba     ? decoded.rgba[index]
                       : has_scalar ? std::bit_cast<uint32_t>(decoded.scalar[index])
                                    : 0u};
      const auto* bytes = reinterpret_cast<const std::byte*>(&vertex);
      expected.insert(expected.end(), bytes, bytes + stride);
    }
  }

  const std::size_t points = static_cast<std::size_t>(cloud.width) * cloud.height;
  const std::optional<PointVertexPlan> plan = planPointVertices(cloud, scalar_field, extract_rgba, store_scalar);
  EXPECT_EQ(plan.has_value(), !decoded.positions.empty());
  if (!plan.has_value()) {
    return;
  }
  EXPECT_EQ(plan->format, format);
  EXPECT_EQ(plan->maxBytes(), points * stride);
  std::vector<std::byte> out(plan->maxBytes());
  const PackedPointVertices packed = packPointVertices(cloud, *plan, out);
  EXPECT_EQ(packed.format, format);
  ASSERT_EQ(packed.count * stride, expected.size());
  if (!expected.empty()) {
    EXPECT_EQ(std::memcmp(out.data(), expected.data(), expected.size()), 0) << "vertices differ bitwise";
  }
  EXPECT_EQ(packed.bounds, reference.bounds);
  const pj::scene3d::FiniteRange range = pj::scene3d::finiteRange(decoded.scalar);
  ASSERT_EQ(packed.scalar_range.has_value(), range.valid);
  if (range.valid) {
    EXPECT_EQ(packed.scalar_range->first, range.lo);
    EXPECT_EQ(packed.scalar_range->second, range.hi);
  }
}

void expectPackMatchesConvert(const PointCloud& cloud, std::string_view scalar_field, bool extract_rgba) {
  expectPackMatches(cloud, scalar_field, extract_rgba, true);
  expectPackMatches(cloud, scalar_field, extract_rgba, false);
}

// Random point bytes under every coordinate type, scalar type and colour layout:
// random float bits hit NaN/Inf, and every few points get an explicit NaN, Inf or
// -0.0 coordinate or a NaN scalar.
TEST(PointCloudPack, MatchesConvertOnRandomClouds) {
  constexpr uint32_t kStep = 40;  // xyz up to 24 bytes, scalar at 24 (8), colour at 32
  constexpr uint32_t kPoints = 257;
  std::mt19937 random(11);
  std::uniform_int_distribution<int> byte(0, 255);
  const DT coord_types[] = {DT::kFloat32, DT::kFloat64, DT::kUint16, DT::kInt32};
  const DT scalar_types[] = {DT::kInt8,   DT::kUint8,   DT::kInt16,   DT::kUint16, DT::kInt32,
                             DT::kUint32, DT::kFloat32, DT::kFloat64, DT::kUnknown};
  enum class Colour { kNone, kPackedRgba, kPackedRgb, kChannels, kChannelsNoAlpha };
  const Colour colours[] = {
      Colour::kNone, Colour::kPackedRgba, Colour::kPackedRgb, Colour::kChannels, Colour::kChannelsNoAlpha};
  for (const DT coord : coord_types) {
    for (const DT scalar_type : scalar_types) {
      for (const Colour colour : colours) {
        const uint32_t width = PJ::sdk::bytesPerElement(coord);
        std::vector<PointField> fields = {
            {"x", 0, coord, 1}, {"y", width, coord, 1}, {"z", 2 * width, coord, 1}, {"intensity", 24, scalar_type, 1}};
        if (colour == Colour::kPackedRgba || colour == Colour::kPackedRgb) {
          fields.push_back({colour == Colour::kPackedRgba ? "rgba" : "rgb", 32, DT::kUint32, 1});
        } else if (colour != Colour::kNone) {
          fields.push_back({"red", 33, DT::kUint8, 1});
          fields.push_back({"green", 32, DT::kUint8, 1});
          fields.push_back({"blue", 35, DT::kUint8, 1});
          if (colour == Colour::kChannels) {
            fields.push_back({"alpha", 34, DT::kUint8, 1});
          }
        }
        std::vector<uint8_t> bytes(static_cast<std::size_t>(kPoints) * kStep);
        for (uint8_t& value : bytes) {
          value = static_cast<uint8_t>(byte(random));
        }
        for (uint32_t index = 0; index < kPoints; ++index) {
          uint8_t* point = bytes.data() + static_cast<std::size_t>(index) * kStep;
          const auto put_coord = [&](uint32_t axis, double value) {
            if (coord == DT::kFloat32) {
              const auto stored = static_cast<float>(value);
              std::memcpy(point + axis * width, &stored, sizeof(stored));
            } else if (coord == DT::kFloat64) {
              std::memcpy(point + axis * width, &value, sizeof(value));
            }
          };
          if (index % 7 == 0) {
            put_coord(0, std::numeric_limits<double>::quiet_NaN());
          }
          if (index % 11 == 0) {
            put_coord(2, -std::numeric_limits<double>::infinity());
          }
          if (index % 13 == 0) {
            put_coord(1, -0.0);
          }
          if (index % 5 == 0 && (scalar_type == DT::kFloat32 || scalar_type == DT::kFloat64)) {
            const double nan = std::numeric_limits<double>::quiet_NaN();
            const auto nan32 = static_cast<float>(nan);
            std::memcpy(
                point + 24, scalar_type == DT::kFloat32 ? static_cast<const void*>(&nan32) : &nan,
                PJ::sdk::bytesPerElement(scalar_type));
          }
        }
        const PointCloud cloud = makeCloud(kPoints, kStep, fields, std::move(bytes));
        SCOPED_TRACE(
            ::testing::Message() << "coord " << static_cast<int>(coord) << " scalar " << static_cast<int>(scalar_type)
                                 << " colour " << static_cast<int>(colour));
        expectPackMatchesConvert(cloud, "", false);
        expectPackMatchesConvert(cloud, "intensity", false);
        expectPackMatchesConvert(cloud, "missing", false);
        expectPackMatchesConvert(cloud, "", true);
        expectPackMatchesConvert(cloud, "intensity", true);
      }
    }
  }
}

TEST(PointCloudPack, AllNonFiniteCloudIsAcceptedEmpty) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::vector<uint8_t> bytes;
  for (int index = 0; index < 3; ++index) {
    putBytes(bytes, leBytes(nan).data(), 4);
    putBytes(bytes, leBytes(1.0f).data(), 4);
    putBytes(bytes, leBytes(2.0f).data(), 4);
  }
  const PointCloud cloud =
      makeCloud(3, 12, {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}}, bytes);
  const std::optional<PointVertexPlan> plan = planPointVertices(cloud, "", false, true);
  ASSERT_TRUE(plan.has_value());
  std::vector<std::byte> out(plan->maxBytes());
  const PackedPointVertices packed = packPointVertices(cloud, *plan, out);
  EXPECT_EQ(packed.count, 0u);
  EXPECT_FALSE(packed.bounds.valid);
  expectPackMatchesConvert(cloud, "", false);
}

// Every cloud convertCanonical rejects has no plan.
TEST(PointCloudPack, RejectsWhatConvertRejects) {
  const std::vector<PointField> xyz = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
  std::vector<std::pair<const char*, PointCloud>> clouds;
  PointCloud big_endian = makeCloud(1, 12, xyz, std::vector<uint8_t>(12, 0));
  big_endian.is_bigendian = true;
  clouds.emplace_back("big-endian", big_endian);
  clouds.emplace_back("short buffer", makeCloud(4, 12, xyz, std::vector<uint8_t>(24, 0)));
  clouds.emplace_back("no z", makeCloud(1, 8, {xyz[0], xyz[1]}, std::vector<uint8_t>(8, 0)));
  clouds.emplace_back("z past step", makeCloud(1, 10, xyz, std::vector<uint8_t>(10, 0)));
  clouds.emplace_back("zero width", makeCloud(0, 12, xyz, std::vector<uint8_t>(12, 0)));
  std::vector<PointField> scalar_past = xyz;
  scalar_past.push_back({"intensity", 14, DT::kFloat32, 1});
  clouds.emplace_back("scalar past step", makeCloud(1, 16, scalar_past, std::vector<uint8_t>(16, 0)));
  std::vector<PointField> scalar_huge = xyz;
  scalar_huge.push_back({"intensity", 0xFFFFFFFCu, DT::kFloat32, 1});
  clouds.emplace_back("adversarial scalar offset", makeCloud(1, 12, scalar_huge, std::vector<uint8_t>(12, 0)));
  std::vector<PointField> colour_past = xyz;
  colour_past.insert(
      colour_past.end(), {{"red", 16, DT::kUint8, 1},
                          {"green", 17, DT::kUint8, 1},
                          {"blue", 18, DT::kUint8, 1},
                          {"alpha", 19, DT::kUint8, 1}});
  clouds.emplace_back("colour past step", makeCloud(1, 19, colour_past, std::vector<uint8_t>(19, 0)));
  for (const auto& [name, cloud] : clouds) {
    SCOPED_TRACE(name);
    int rejections = 0;
    for (const bool rgba : {false, true}) {
      if (!convertCanonical(cloud, "intensity", rgba).cloud.positions.empty()) {
        continue;  // this mode does not read the malformed field
      }
      ++rejections;
      EXPECT_FALSE(planPointVertices(cloud, "intensity", rgba, true).has_value());
    }
    EXPECT_GT(rejections, 0);
    expectPackMatchesConvert(cloud, "intensity", true);
    expectPackMatchesConvert(cloud, "intensity", false);
  }
}

TEST(PointCloudPack, TooSmallOutputWritesNothing) {
  const PointCloud cloud = makeCloud(
      2, 12, {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}},
      std::vector<uint8_t>(24, 0));
  const std::optional<PointVertexPlan> plan = planPointVertices(cloud, "", false, true);
  ASSERT_TRUE(plan.has_value());
  std::vector<std::byte> out(plan->maxBytes() - 1, std::byte{42});
  EXPECT_EQ(packPointVertices(cloud, *plan, out).count, 0u);
  EXPECT_EQ(out[0], std::byte{42});
}

// Chunked packing (run here in reverse chunk order) gives the serial pass's bytes,
// count, bounds and scalar range, with dropped points inside and across chunks.
TEST(PointCloudPack, ChunkedMatchesSerial) {
  constexpr uint32_t kStep = 16;
  const uint32_t points = static_cast<uint32_t>(pj::scene3d::kPackChunkPoints * 3 + 12345);
  std::mt19937 random(5);
  std::uniform_real_distribution<float> value(-50.0f, 50.0f);
  std::vector<uint8_t> bytes(static_cast<std::size_t>(points) * kStep);
  for (uint32_t index = 0; index < points; ++index) {
    float point[4] = {value(random), value(random), value(random), value(random)};
    if (index % 3 == 0 || (index > points / 5 && index < points / 2)) {  // sparse drops, and a whole chunk
      point[index % 2 == 0 ? 0 : 2] = std::numeric_limits<float>::quiet_NaN();
    }
    if (index % 17 == 0) {
      point[3] = std::numeric_limits<float>::infinity();
    }
    std::memcpy(bytes.data() + static_cast<std::size_t>(index) * kStep, point, sizeof(point));
  }
  const PointCloud cloud = makeCloud(
      points, kStep,
      {{"x", 0, DT::kFloat32, 1},
       {"y", 4, DT::kFloat32, 1},
       {"z", 8, DT::kFloat32, 1},
       {"intensity", 12, DT::kFloat32, 1}},
      std::move(bytes));
  const std::optional<PointVertexPlan> plan = planPointVertices(cloud, "intensity", false, true);
  ASSERT_TRUE(plan.has_value());
  std::vector<std::byte> serial(plan->maxBytes());
  std::vector<std::byte> chunked(plan->maxBytes());
  const PackedPointVertices expected = packPointVertices(cloud, *plan, serial);
  std::size_t calls = 0;
  const PackedPointVertices actual =
      packPointVertices(cloud, *plan, chunked, [&](std::size_t count, const std::function<void(std::size_t)>& body) {
        calls = count;
        for (std::size_t chunk = count; chunk-- > 0;) {
          body(chunk);
        }
      });
  EXPECT_GT(calls, 1u);
  ASSERT_EQ(actual.count, expected.count);
  EXPECT_LT(actual.count, static_cast<std::size_t>(points));
  EXPECT_EQ(actual.format, PointVertexFormat::kXyzScalar);
  EXPECT_EQ(std::memcmp(chunked.data(), serial.data(), expected.count * sizeof(PointVertex)), 0);
  EXPECT_EQ(actual.bounds, expected.bounds);
  EXPECT_EQ(actual.scalar_range, expected.scalar_range);
  expectPackMatchesConvert(cloud, "intensity", false);

  // A lower chunk cap packs the same bytes in fewer, larger chunks.
  std::vector<std::byte> capped(plan->maxBytes());
  const PackedPointVertices two = packPointVertices(
      cloud, *plan, capped,
      [&](std::size_t count, const std::function<void(std::size_t)>& body) {
        calls = count;
        for (std::size_t chunk = 0; chunk < count; ++chunk) {
          body(chunk);
        }
      },
      /*max_chunks=*/2);
  EXPECT_EQ(calls, 2u);
  ASSERT_EQ(two.count, expected.count);
  EXPECT_EQ(std::memcmp(capped.data(), serial.data(), expected.count * sizeof(PointVertex)), 0);
}

}  // namespace
