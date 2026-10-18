// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The native point-cloud and scene-entities operations, and the memory budget they charge, tested
// directly against their C++ API.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "native/memory_budget.h"
#include "native/point_cloud_ops.h"
#include "native/record_walk.h"
#include "native/scene_entities_ops.h"
#include "pj_base/buffer_anchor.hpp"
#include "pj_base/sdk/platform.hpp"

namespace PJ::scripting::native {
namespace {

using sdk::PointCloud;
using sdk::PointField;
using sdk::Vector3;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr Vector3 kEverywhereMin{-1e9, -1e9, -1e9};
constexpr Vector3 kEverywhereMax{1e9, 1e9, 1e9};

std::vector<PointField> xyziFields() {
  return {
      PointField{.name = "x", .offset = 0, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "y", .offset = 4, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "z", .offset = 8, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "intensity", .offset = 12, .datatype = PointField::Datatype::kFloat32, .count = 1},
  };
}

void setBytes(PointCloud& cloud, std::vector<uint8_t> bytes) {
  sdk::PayloadView view = sdk::makePayloadView(std::move(bytes));
  cloud.data = view.bytes;
  cloud.anchor = view.anchor;
}

// A flat XYZI cloud: point_step 16, x/y/z/intensity as float32.
PointCloud makeXyziCloud(const std::vector<std::array<float, 4>>& points) {
  PointCloud cloud;
  cloud.width = static_cast<uint32_t>(points.size());
  cloud.height = 1;
  cloud.point_step = 16;
  cloud.row_step = cloud.point_step * cloud.width;
  cloud.frame_id = "base_link";
  cloud.fields = xyziFields();
  cloud.timestamp_ns = 1'000'000'000;
  std::vector<uint8_t> bytes(static_cast<size_t>(cloud.point_step) * points.size());
  for (size_t index = 0; index < points.size(); ++index) {
    std::memcpy(bytes.data() + index * cloud.point_step, points[index].data(), sizeof(float) * 4);
  }
  setBytes(cloud, std::move(bytes));
  return cloud;
}

// `count` points along x, at x = 0, 1, 2, ...
PointCloud makeLineCloud(size_t count) {
  std::vector<std::array<float, 4>> points(count);
  for (size_t index = 0; index < count; ++index) {
    points[index] = {static_cast<float>(index), 0, 0, 0};
  }
  return makeXyziCloud(points);
}

// A 2x2 organized cloud whose rows carry 8 bytes of padding (row_step 40, not 32).
// Row 0: (0,0,0), (1,1,1). Row 1: (2,2,2), (3,3,3). Intensity = 10 * index.
PointCloud makeOrganizedCloud() {
  PointCloud cloud;
  cloud.width = 2;
  cloud.height = 2;
  cloud.point_step = 16;
  cloud.row_step = 40;
  cloud.is_dense = false;
  cloud.frame_id = "camera";
  cloud.fields = xyziFields();
  cloud.timestamp_ns = 42;
  std::vector<uint8_t> bytes(static_cast<size_t>(cloud.row_step) * cloud.height, 0);
  for (uint32_t index = 0; index < 4; ++index) {
    const float coord = static_cast<float>(index);
    const float record[4] = {coord, coord, coord, 10.0f * coord};
    const size_t offset = (index / cloud.width) * cloud.row_step + (index % cloud.width) * cloud.point_step;
    std::memcpy(bytes.data() + offset, record, sizeof(record));
  }
  setBytes(cloud, std::move(bytes));
  return cloud;
}

// x/y/z as float64 behind a uint16 ring channel: point_step 26.
PointCloud makeFloat64Cloud(const std::vector<std::array<double, 3>>& points) {
  PointCloud cloud;
  cloud.width = static_cast<uint32_t>(points.size());
  cloud.point_step = 26;
  cloud.row_step = cloud.point_step * cloud.width;
  cloud.frame_id = "lidar";
  cloud.fields = {
      PointField{.name = "ring", .offset = 0, .datatype = PointField::Datatype::kUint16, .count = 1},
      PointField{.name = "x", .offset = 2, .datatype = PointField::Datatype::kFloat64, .count = 1},
      PointField{.name = "y", .offset = 10, .datatype = PointField::Datatype::kFloat64, .count = 1},
      PointField{.name = "z", .offset = 18, .datatype = PointField::Datatype::kFloat64, .count = 1},
  };
  std::vector<uint8_t> bytes(static_cast<size_t>(cloud.point_step) * points.size());
  for (size_t index = 0; index < points.size(); ++index) {
    const auto ring = static_cast<uint16_t>(index + 7);
    std::memcpy(bytes.data() + index * cloud.point_step, &ring, sizeof(ring));
    std::memcpy(bytes.data() + index * cloud.point_step + 2, points[index].data(), sizeof(double) * 3);
  }
  setBytes(cloud, std::move(bytes));
  return cloud;
}

template <class T>
T valueAt(const PointCloud& cloud, size_t record, uint32_t offset) {
  T value{};
  std::memcpy(&value, cloud.data.data() + record * cloud.point_step + offset, sizeof(value));
  return value;
}

float floatAt(const PointCloud& cloud, size_t record, uint32_t offset) {
  return valueAt<float>(cloud, record, offset);
}

bool contains(const std::string& text, const char* part) {
  return text.find(part) != std::string::npos;
}

NativeLimits limitedTo(std::size_t bytes) {
  NativeLimits limits;
  limits.budget = std::make_shared<MemoryBudget>(bytes);
  return limits;
}

// ---- point-cloud operations -------------------------------------------------

TEST(PointCloudOps, CountBoundsExtremeOnSyntheticCloud) {
  const auto cloud = makeXyziCloud({{0, 0, 0, 0.1f}, {1, 2, 3, 0.5f}, {-1, -2, -3, 0.9f}});
  EXPECT_EQ(countPoints(cloud), 3u);
  const auto box = bounds(cloud);
  ASSERT_TRUE(box) << box.error();
  EXPECT_EQ(box->count, 3u);
  EXPECT_DOUBLE_EQ(box->min.x, -1.0);
  EXPECT_DOUBLE_EQ(box->max.x, 1.0);
  EXPECT_DOUBLE_EQ(box->max.z, 3.0);
  const auto top = extremePoint(cloud, "z", Extreme::kMax);
  ASSERT_TRUE(top) << top.error();
  ASSERT_TRUE(top->has_value());
  EXPECT_EQ((*top)->point.index, 1u);
  EXPECT_DOUBLE_EQ((*top)->point.position.z, 3.0);
  const auto dimmest = extremePoint(cloud, "intensity", Extreme::kMin);
  ASSERT_TRUE(dimmest) << dimmest.error();
  EXPECT_EQ((*dimmest)->point.index, 0u);
  EXPECT_FALSE(extremePoint(cloud, "ring", Extreme::kMax).has_value());  // no such channel
}

TEST(PointCloudOps, CropBoxPreservesWholeRecords) {
  const auto cloud = makeXyziCloud({{0, 0, 0, 0.1f}, {10, 10, 10, 0.5f}, {1, 1, 1, 0.9f}});
  const auto out = cropBox(cloud, Vector3{-5, -5, -5}, Vector3{5, 5, 5}, {});
  ASSERT_TRUE(out) << out.error();
  EXPECT_EQ(out->width, 2u);  // (10,10,10) is cropped out
  EXPECT_EQ(out->height, 1u);
  EXPECT_TRUE(out->is_dense);
  EXPECT_EQ(out->point_step, cloud.point_step);
  EXPECT_EQ(out->row_step, out->point_step * out->width);
  EXPECT_EQ(out->fields.size(), cloud.fields.size());
  ASSERT_EQ(out->data.size(), static_cast<size_t>(out->point_step) * out->width);
  EXPECT_FLOAT_EQ(floatAt(*out, 1, 0), 1.0f);
  EXPECT_FLOAT_EQ(floatAt(*out, 1, 12), 0.9f);  // the intensity travels with its record
}

// The scene places an object at its own stamp, so no output may be restamped.
TEST(PointCloudOps, CropKeepsTheInputStampAndFrame) {
  const auto cloud = makeOrganizedCloud();
  const auto cropped = cropBox(cloud, kEverywhereMin, kEverywhereMax, {});
  ASSERT_TRUE(cropped) << cropped.error();
  EXPECT_EQ(cropped->timestamp_ns, 42);
  EXPECT_EQ(cropped->frame_id, "camera");
  const auto filtered = filterByField(cloud, "intensity", CompareOp::kGe, 0.0, {});
  ASSERT_TRUE(filtered) << filtered.error();
  EXPECT_EQ(filtered->timestamp_ns, 42);
  EXPECT_EQ(filtered->frame_id, "camera");
  sdk::FrameTransform identity;
  identity.parent_frame_id = "map";
  const auto moved = transformXyz(cloud, identity, {});
  ASSERT_TRUE(moved) << moved.error();
  EXPECT_EQ(moved->timestamp_ns, 42);
}

TEST(PointCloudOps, OrganizedCloudHonoursRowStep) {
  const auto cloud = makeOrganizedCloud();
  // Record 3 is row 1, col 1: byte offset row_step + point_step = 56, not the flat 3 * 16 = 48.
  const auto top = extremePoint(cloud, "x", Extreme::kMax);
  ASSERT_TRUE(top) << top.error();
  ASSERT_TRUE(top->has_value());
  EXPECT_EQ((*top)->point.index, 3u);
  EXPECT_DOUBLE_EQ((*top)->point.position.x, 3.0);
  EXPECT_DOUBLE_EQ((*top)->point.position.y, 3.0);
  EXPECT_DOUBLE_EQ((*top)->point.position.z, 3.0);
  // A crop reads the second row past its padding and packs it.
  const auto cropped = cropBox(cloud, Vector3{1.5, 1.5, 1.5}, Vector3{9, 9, 9}, {});
  ASSERT_TRUE(cropped) << cropped.error();
  ASSERT_EQ(cropped->width, 2u);
  EXPECT_FLOAT_EQ(floatAt(*cropped, 0, 0), 2.0f);
  EXPECT_FLOAT_EQ(floatAt(*cropped, 1, 12), 30.0f);
}

TEST(PointCloudOps, InvalidLayoutIsAnError) {
  auto cloud = makeXyziCloud({{0, 0, 0, 0}});  // 16 bytes of data
  cloud.width = 5;                             // claims 80
  cloud.row_step = cloud.point_step * cloud.width;
  const auto box = bounds(cloud);
  ASSERT_FALSE(box);
  EXPECT_TRUE(contains(box.error(), "smaller than its declared layout")) << box.error();

  auto wrapping = makeXyziCloud({{0, 0, 0, 0}});
  wrapping.fields[3].offset = 0xFFFFFFFEu;  // offset + 4 wraps around uint32
  const auto filtered = filterByField(wrapping, "intensity", CompareOp::kGt, 0.0, {});
  ASSERT_FALSE(filtered);
  EXPECT_TRUE(contains(filtered.error(), "point_step")) << filtered.error();

  auto integer_x = makeXyziCloud({{0, 0, 0, 0}});
  integer_x.fields[0].datatype = PointField::Datatype::kInt32;
  EXPECT_FALSE(resolveXyz(integer_x).has_value());
}

// A header whose extent wraps a 64-bit size must not pass for an empty buffer.
TEST(PointCloudOps, LayoutThatWrapsSizeIsAnError) {
  PointCloud cloud;
  cloud.width = 1u << 30;
  cloud.height = 1u << 30;
  cloud.point_step = 16;  // 16 * 2^60 bytes wraps to 0
  cloud.row_step = 0;
  cloud.fields = xyziFields();
  const auto box = bounds(cloud);
  ASSERT_FALSE(box);
  EXPECT_TRUE(contains(box.error(), "smaller than its declared layout")) << box.error();
}

// Rows closer than a row's bytes make records alias, and a scan could pass the buffer's end.
TEST(PointCloudOps, OverlappingRowsAreRejected) {
  auto cloud = makeOrganizedCloud();
  cloud.row_step = 8;  // a row holds 2 * 16 bytes
  const auto box = bounds(cloud);
  ASSERT_FALSE(box);
  EXPECT_TRUE(contains(box.error(), "overlap")) << box.error();
}

TEST(PointCloudOps, BigEndianIsRejected) {
  auto cloud = makeXyziCloud({{0, 0, 0, 0}});
  cloud.is_bigendian = true;
  const auto box = bounds(cloud);
  ASSERT_FALSE(box);
  EXPECT_TRUE(contains(box.error(), "big-endian")) << box.error();
  EXPECT_FALSE(cropBox(cloud, kEverywhereMin, kEverywhereMax, {}).has_value());
}

TEST(PointCloudOps, XyzPointsSkipNaN) {
  const auto cloud = makeXyziCloud({{1, 0, 0, 0}, {kNaN, 1, 1, 0}, {3, 0, 0, 0}, {kNaN, kNaN, kNaN, 0}});
  const auto access = resolveXyz(cloud);
  ASSERT_TRUE(access) << access.error();
  std::vector<size_t> indices;
  double sum_x = 0.0;
  size_t next = 0;
  while (const auto point = nextFinitePoint(cloud, *access, next)) {
    indices.push_back(point->index);
    sum_x += point->position.x;
  }
  EXPECT_EQ(indices, (std::vector<size_t>{0, 2}));
  EXPECT_DOUBLE_EQ(sum_x, 4.0);
}

TEST(PointCloudOps, ExtremePointSkipsNonFinitePoints) {
  const auto cloud = makeXyziCloud({{kNaN, 0, 100, 0}, {1, 1, 1, 0}});
  const auto top = extremePoint(cloud, "z", Extreme::kMax);
  ASSERT_TRUE(top) << top.error();
  ASSERT_TRUE(top->has_value());
  EXPECT_EQ((*top)->point.index, 1u);
  EXPECT_DOUBLE_EQ((*top)->point.position.z, 1.0);
}

TEST(PointCloudOps, FilterByFieldKeepsMatchingRecords) {
  const auto cloud = makeXyziCloud({{0, 0, 0, 0.1f}, {1, 1, 1, kNaN}, {2, 2, 2, 0.9f}});
  const auto out = filterByField(cloud, "intensity", CompareOp::kGt, 0.5, {});
  ASSERT_TRUE(out) << out.error();
  ASSERT_EQ(out->width, 1u);  // a NaN intensity never matches
  EXPECT_FLOAT_EQ(floatAt(*out, 0, 0), 2.0f);
}

// Only a crop guarantees finite coordinates; a filter on another channel keeps the input's density.
TEST(PointCloudOps, FilterKeepsTheInputDensity) {
  const auto cloud = makeOrganizedCloud();  // is_dense = false
  const auto out = filterByField(cloud, "intensity", CompareOp::kGe, 0.0, {});
  ASSERT_TRUE(out) << out.error();
  EXPECT_FALSE(out->is_dense);
}

TEST(PointCloudOps, CloudTransformAppliesRigidMotion) {
  auto cloud = makeXyziCloud({{1, 0, 0, 7}, {0, 1, 0, 0}, {0, 0, 1, 0}});
  cloud.frame_id = "lidar";
  // map <- lidar: a 90 degree yaw (x -> y, y -> -x) then a translation of (10, 0, 0).
  constexpr double kHalfSqrt2 = 0.70710678118654752440;
  sdk::FrameTransform tf;
  tf.parent_frame_id = "map";
  tf.child_frame_id = "lidar";
  tf.translation = Vector3{10.0, 0.0, 0.0};
  tf.rotation = sdk::Quaternion{0.0, 0.0, kHalfSqrt2, kHalfSqrt2};
  const auto out = transformXyz(cloud, tf, {});
  ASSERT_TRUE(out) << out.error();
  EXPECT_EQ(out->frame_id, "map");
  const float expected[3][3] = {{10, 1, 0}, {9, 0, 0}, {10, 0, 1}};
  for (size_t index = 0; index < 3; ++index) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
      EXPECT_NEAR(floatAt(*out, index, axis * 4), expected[index][axis], 1e-6) << "point " << index << " axis " << axis;
    }
  }
  EXPECT_FLOAT_EQ(floatAt(*out, 0, 12), 7.0f);  // other channels are copied verbatim
}

TEST(PointCloudOps, TransformKeepsTheFrameWithoutAParent) {
  const auto cloud = makeOrganizedCloud();
  const auto out = transformXyz(cloud, sdk::FrameTransform{}, {});
  ASSERT_TRUE(out) << out.error();
  EXPECT_EQ(out->frame_id, "camera");
}

TEST(PointCloudOps, TransformRejectsANonFiniteTransformAndNormalizesTheRotation) {
  const auto cloud = makeXyziCloud({{1, 0, 0, 0}});
  sdk::FrameTransform scaled;
  scaled.rotation = sdk::Quaternion{0.0, 0.0, 2.0, 2.0};  // a 90 degree yaw, norm 2*sqrt(2)
  const auto out = transformXyz(cloud, scaled, {});
  ASSERT_TRUE(out) << out.error();
  EXPECT_NEAR(floatAt(*out, 0, 0), 0.0f, 1e-6);
  EXPECT_NEAR(floatAt(*out, 0, 4), 1.0f, 1e-6);
  sdk::FrameTransform broken;
  broken.translation.x = std::numeric_limits<double>::quiet_NaN();
  const auto rejected = transformXyz(cloud, broken, {});
  ASSERT_FALSE(rejected);
  EXPECT_TRUE(contains(rejected.error(), "finite")) << rejected.error();
  sdk::FrameTransform zero;
  zero.rotation = sdk::Quaternion{0.0, 0.0, 0.0, 0.0};
  EXPECT_FALSE(transformXyz(cloud, zero, {}).has_value());
}

// float64 coordinates are read and written back at full precision, after a leading non-xyz channel.
TEST(PointCloudOps, Float64CoordinatesAreReadAndWritten) {
  const auto cloud = makeFloat64Cloud({{0.1, 0.2, 0.3}, {1e6 + 0.25, 5, 5}});
  const auto top = extremePoint(cloud, "x", Extreme::kMax);
  ASSERT_TRUE(top) << top.error();
  EXPECT_DOUBLE_EQ((*top)->point.position.x, 1e6 + 0.25);
  sdk::FrameTransform shift;
  shift.translation = Vector3{0.0, 0.0, 1.0};
  const auto moved = transformXyz(cloud, shift, {});
  ASSERT_TRUE(moved) << moved.error();
  EXPECT_DOUBLE_EQ(valueAt<double>(*moved, 1, 2), 1e6 + 0.25);
  EXPECT_DOUBLE_EQ(valueAt<double>(*moved, 0, 18), 1.3);
  const auto cropped = cropBox(cloud, Vector3{1, 1, 1}, kEverywhereMax, {});
  ASSERT_TRUE(cropped) << cropped.error();
  ASSERT_EQ(cropped->width, 1u);
  EXPECT_EQ(valueAt<uint16_t>(*cropped, 0, 0), 8u);  // the ring travels with its record
}

// CON-15: a long native loop stops once the run is cancelled, mid-scan, and holds no memory after.
TEST(PointCloudOps, NativeOpsHonourTheCancelToken) {
  const auto cloud = makeLineCloud(2'000'000);
  for (int op = 0; op < 3; ++op) {
    int polls = 0;
    NativeLimits limits = limitedTo(std::size_t{1} << 30);
    limits.cancelled = [&polls] { return ++polls > 3; };  // cancelled a few chunks into the scan
    const auto out = op == 0   ? cropBox(cloud, kEverywhereMin, kEverywhereMax, limits)
                     : op == 1 ? filterByField(cloud, "x", CompareOp::kGe, 0.0, limits)
                               : transformXyz(cloud, sdk::FrameTransform{}, limits);
    ASSERT_FALSE(out) << "op " << op;
    EXPECT_TRUE(contains(out.error(), "cancelled")) << out.error();
    EXPECT_EQ(polls, 4) << "op " << op;  // stopped at the first poll that saw the cancel
    EXPECT_EQ(limits.budget->used(), 0u) << "op " << op;
  }
}

// ---- equivalence with plain reference loops -----------------------------------

using Datatype = PointField::Datatype;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN64 = std::numeric_limits<double>::quiet_NaN();

// Byte offset of record `index`, computed the plain way, with a division per record.
size_t referenceOffset(const PointCloud& cloud, size_t index) {
  if (cloud.row_step == 0 || cloud.height <= 1) {
    return index * cloud.point_step;
  }
  return (index / cloud.width) * cloud.row_step + (index % cloud.width) * cloud.point_step;
}

template <class T>
double loadAs(const uint8_t* bytes) {
  T value;
  std::memcpy(&value, bytes, sizeof(value));
  return static_cast<double>(value);
}

template <class T>
void storeAs(uint8_t* bytes, double value) {
  const auto typed = static_cast<T>(value);
  std::memcpy(bytes, &typed, sizeof(typed));
}

double referenceRead(const PointCloud& cloud, size_t index, const PointField& field) {
  const uint8_t* bytes = cloud.data.data() + referenceOffset(cloud, index) + field.offset;
  switch (field.datatype) {
    case Datatype::kInt8:
      return loadAs<int8_t>(bytes);
    case Datatype::kUint8:
      return loadAs<uint8_t>(bytes);
    case Datatype::kInt16:
      return loadAs<int16_t>(bytes);
    case Datatype::kUint16:
      return loadAs<uint16_t>(bytes);
    case Datatype::kInt32:
      return loadAs<int32_t>(bytes);
    case Datatype::kUint32:
      return loadAs<uint32_t>(bytes);
    case Datatype::kFloat32:
      return loadAs<float>(bytes);
    case Datatype::kFloat64:
      return loadAs<double>(bytes);
    case Datatype::kUnknown:
      break;
  }
  ADD_FAILURE() << "unknown datatype";
  return 0.0;
}

// Writes `value` as `datatype`; `value` must fit it.
void referenceWrite(uint8_t* bytes, Datatype datatype, double value) {
  switch (datatype) {
    case Datatype::kInt8:
      return storeAs<int8_t>(bytes, value);
    case Datatype::kUint8:
      return storeAs<uint8_t>(bytes, value);
    case Datatype::kInt16:
      return storeAs<int16_t>(bytes, value);
    case Datatype::kUint16:
      return storeAs<uint16_t>(bytes, value);
    case Datatype::kInt32:
      return storeAs<int32_t>(bytes, value);
    case Datatype::kUint32:
      return storeAs<uint32_t>(bytes, value);
    case Datatype::kFloat32:
      return storeAs<float>(bytes, value);
    case Datatype::kFloat64:
      return storeAs<double>(bytes, value);
    case Datatype::kUnknown:
      break;
  }
  ADD_FAILURE() << "unknown datatype";
}

// A cloud of `width * height` records, each written by `fill(record, index)`. Rows are `row_padding` bytes
// longer than their records, and the padding holds junk a misplaced read would pick up.
template <class Fill>
PointCloud makeCloud(
    std::vector<PointField> fields, uint32_t point_step, uint32_t width, uint32_t height, uint32_t row_padding,
    Fill&& fill) {
  PointCloud cloud;
  cloud.width = width;
  cloud.height = height;
  cloud.point_step = point_step;
  cloud.row_step = width * point_step + row_padding;
  cloud.frame_id = "sensor";
  cloud.fields = std::move(fields);
  cloud.timestamp_ns = 7;
  std::vector<uint8_t> bytes(static_cast<size_t>(cloud.row_step) * height, 0xAB);
  for (size_t index = 0; index < static_cast<size_t>(width) * height; ++index) {
    fill(bytes.data() + referenceOffset(cloud, index), index);
  }
  setBytes(cloud, std::move(bytes));
  return cloud;
}

enum class Layout { kFlat, kOrganized };

const char* layoutName(Layout layout) {
  return layout == Layout::kFlat ? "flat" : "organized";
}

// `records` records, flat or as rows of 5 with 7 bytes of padding after each row.
template <class Fill>
PointCloud makeLaidOutCloud(
    Layout layout, std::vector<PointField> fields, uint32_t point_step, uint32_t records, Fill&& fill) {
  if (layout == Layout::kFlat) {
    return makeCloud(std::move(fields), point_step, records, 1, 0, fill);
  }
  return makeCloud(std::move(fields), point_step, 5, records / 5, 7, fill);
}

// The bytes of `indices`' records, packed: what a compacting operation must output.
std::vector<uint8_t> packedRecords(const PointCloud& cloud, const std::vector<size_t>& indices) {
  std::vector<uint8_t> bytes;
  for (const size_t index : indices) {
    const uint8_t* record = cloud.data.data() + referenceOffset(cloud, index);
    bytes.insert(bytes.end(), record, record + cloud.point_step);
  }
  return bytes;
}

std::vector<uint8_t> bytesOf(const PointCloud& cloud) {
  return {cloud.data.begin(), cloud.data.end()};
}

bool referenceCompare(CompareOp op, double value, double threshold) {
  switch (op) {
    case CompareOp::kLt:
      return value < threshold;
    case CompareOp::kLe:
      return value <= threshold;
    case CompareOp::kGt:
      return value > threshold;
    case CompareOp::kGe:
      return value >= threshold;
    case CompareOp::kEq:
      return value == threshold;
    case CompareOp::kNe:
      return value != threshold;
  }
  return false;
}

// The values a `datatype` channel holds in the filter table: its limits, ties with the thresholds below and,
// for floating types, NaN, the infinities and -0.
std::vector<double> channelValues(Datatype datatype) {
  const std::vector<double> candidates = {-1e10, -129, -128, -3, -0.5, 0, 1, 2.5, 3, 7, 127, 128, 255, 65535, 1e10};
  std::vector<double> values;
  double lowest = 0;
  double highest = 0;
  switch (datatype) {
    case Datatype::kInt8:
      lowest = std::numeric_limits<int8_t>::lowest();
      highest = std::numeric_limits<int8_t>::max();
      break;
    case Datatype::kUint8:
      highest = std::numeric_limits<uint8_t>::max();
      break;
    case Datatype::kInt16:
      lowest = std::numeric_limits<int16_t>::lowest();
      highest = std::numeric_limits<int16_t>::max();
      break;
    case Datatype::kUint16:
      highest = std::numeric_limits<uint16_t>::max();
      break;
    case Datatype::kInt32:
      lowest = std::numeric_limits<int32_t>::lowest();
      highest = std::numeric_limits<int32_t>::max();
      break;
    case Datatype::kUint32:
      highest = std::numeric_limits<uint32_t>::max();
      break;
    case Datatype::kFloat32:
    case Datatype::kFloat64:
    case Datatype::kUnknown:
      break;
  }
  const bool floating = datatype == Datatype::kFloat32 || datatype == Datatype::kFloat64;
  if (floating) {
    values = candidates;
    values.insert(values.end(), {kNaN64, kInf, -kInf, -0.0, 3.0000001});
    return values;
  }
  values = {lowest, highest};
  for (const double value : candidates) {
    if (value >= lowest && value <= highest && value == std::floor(value)) {
      values.push_back(value);
    }
  }
  return values;
}

// filterByField matches a plain loop for every datatype and comparison, NaN and the infinities included, on
// a flat and an organized cloud. The thresholds sit on, between and beyond the stored values, so comparing in
// the channel's own type (a truncated or rounded threshold) would show.
TEST(PointCloudOpsEquivalence, FilterByFieldMatchesAPlainLoopForEveryDatatypeAndOperator) {
  const Datatype datatypes[] = {Datatype::kInt8,  Datatype::kUint8,  Datatype::kInt16,   Datatype::kUint16,
                                Datatype::kInt32, Datatype::kUint32, Datatype::kFloat32, Datatype::kFloat64};
  const CompareOp ops[] = {CompareOp::kLt, CompareOp::kLe, CompareOp::kGt,
                           CompareOp::kGe, CompareOp::kEq, CompareOp::kNe};
  const double thresholds[] = {-kInf, -1e10, -129, -3, -0.5, 0, 2.5, 3, 7, 127.5, 255, 65535, 1e10, kInf, kNaN64};
  for (const Layout layout : {Layout::kFlat, Layout::kOrganized}) {
    for (const Datatype datatype : datatypes) {
      const std::vector<double> values = channelValues(datatype);
      std::vector<PointField> fields = xyziFields();
      fields[3] = PointField{.name = "v", .offset = 13, .datatype = datatype, .count = 1};  // unaligned
      const auto cloud = makeLaidOutCloud(layout, fields, 24, 40, [&](uint8_t* record, size_t index) {
        const float xyz[3] = {static_cast<float>(index), index % 9 == 0 ? kNaN : 1.0f, 2.0f};
        std::memcpy(record, xyz, sizeof(xyz));
        referenceWrite(record + 13, datatype, values[index % values.size()]);
      });
      for (const CompareOp op : ops) {
        for (const double threshold : thresholds) {
          std::vector<size_t> kept;
          for (size_t index = 0; index < 40; ++index) {
            const double value = referenceRead(cloud, index, cloud.fields[3]);
            if (std::isfinite(value) && referenceCompare(op, value, threshold)) {
              kept.push_back(index);
            }
          }
          const auto out = filterByField(cloud, "v", op, threshold, {});
          ASSERT_TRUE(out) << out.error();
          const std::string where = std::string(layoutName(layout)) + " datatype " +
                                    std::to_string(static_cast<int>(datatype)) + " op " +
                                    std::to_string(static_cast<int>(op)) + " threshold " + std::to_string(threshold);
          EXPECT_EQ(out->width, kept.size()) << where;
          EXPECT_EQ(out->height, 1u) << where;
          EXPECT_EQ(out->row_step, out->width * cloud.point_step) << where;
          EXPECT_EQ(bytesOf(*out), packedRecords(cloud, kept)) << where;
        }
      }
    }
  }
}

// x/y/z as float32, float64, or mixed (the generic path), at offsets 0/8/16, then a float32 intensity and a
// uint16 ring; point_step 32. Coordinates lie on a 0.25 grid in [-2, 2] and intensities are small integers,
// so ties abound; some records carry NaN or infinite coordinates or intensities.
PointCloud makeGeometryCloud(Layout layout, Datatype x_type, Datatype yz_type, uint32_t seed) {
  const std::vector<PointField> fields = {
      PointField{.name = "x", .offset = 0, .datatype = x_type, .count = 1},
      PointField{.name = "y", .offset = 8, .datatype = yz_type, .count = 1},
      PointField{.name = "z", .offset = 16, .datatype = yz_type, .count = 1},
      PointField{.name = "intensity", .offset = 24, .datatype = Datatype::kFloat32, .count = 1},
      PointField{.name = "ring", .offset = 28, .datatype = Datatype::kUint16, .count = 1},
  };
  std::mt19937 random(seed);
  std::uniform_int_distribution<int> grid(-8, 8);
  std::uniform_int_distribution<int> level(0, 4);
  return makeLaidOutCloud(layout, fields, 32, 60, [&](uint8_t* record, size_t index) {
    double xyz[3] = {grid(random) / 4.0, grid(random) / 4.0, grid(random) / 4.0};
    double intensity = level(random);
    if (index % 7 == 3) {
      xyz[0] = kNaN64;
    }
    if (index % 11 == 5) {
      xyz[1] = kInf;
    }
    if (index == 13) {
      xyz[2] = -kInf;
    }
    if (index == 2) {
      intensity = kNaN64;
    }
    if (index == 8) {
      intensity = kInf;
    }
    referenceWrite(record + 0, x_type, xyz[0]);
    referenceWrite(record + 8, yz_type, xyz[1]);
    referenceWrite(record + 16, yz_type, xyz[2]);
    referenceWrite(record + 24, Datatype::kFloat32, intensity);
    referenceWrite(record + 28, Datatype::kUint16, static_cast<double>(index % 5));
  });
}

std::optional<Vector3> referenceXyz(const PointCloud& cloud, size_t index) {
  const Vector3 position{
      referenceRead(cloud, index, cloud.fields[0]), referenceRead(cloud, index, cloud.fields[1]),
      referenceRead(cloud, index, cloud.fields[2])};
  if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
    return std::nullopt;
  }
  return position;
}

// cropBox, bounds, extremePoint (ties keep the first index), nextFinitePoint and transformXyz match plain
// loops on flat and organized clouds, for float32, float64 and mixed x/y/z.
TEST(PointCloudOpsEquivalence, GeometryOperationsMatchPlainLoops) {
  const std::pair<Datatype, Datatype> xyz_types[] = {
      {Datatype::kFloat32, Datatype::kFloat32},
      {Datatype::kFloat64, Datatype::kFloat64},
      {Datatype::kFloat64, Datatype::kFloat32}};
  const std::pair<Vector3, Vector3> boxes[] = {
      {Vector3{-1, -1, -1}, Vector3{1, 1, 1}},
      {Vector3{0, -2, -0.5}, Vector3{2, 0.25, 0.5}},
      {Vector3{-kInf, -kInf, -kInf}, Vector3{kInf, kInf, kInf}},
      {Vector3{1, 1, 1}, Vector3{-1, -1, -1}}};  // empty
  uint32_t seed = 1;
  for (const Layout layout : {Layout::kFlat, Layout::kOrganized}) {
    for (const auto& [x_type, yz_type] : xyz_types) {
      const auto cloud = makeGeometryCloud(layout, x_type, yz_type, seed++);
      const std::string where = std::string(layoutName(layout)) + " x " + std::to_string(static_cast<int>(x_type)) +
                                " yz " + std::to_string(static_cast<int>(yz_type));
      const size_t records = 60;

      Bounds expected_box{Vector3{kInf, kInf, kInf}, Vector3{-kInf, -kInf, -kInf}, 0};
      std::vector<size_t> finite;
      for (size_t index = 0; index < records; ++index) {
        if (const auto position = referenceXyz(cloud, index)) {
          finite.push_back(index);
          expected_box.min = Vector3{
              std::min(expected_box.min.x, position->x), std::min(expected_box.min.y, position->y),
              std::min(expected_box.min.z, position->z)};
          expected_box.max = Vector3{
              std::max(expected_box.max.x, position->x), std::max(expected_box.max.y, position->y),
              std::max(expected_box.max.z, position->z)};
          ++expected_box.count;
        }
      }
      const auto box = bounds(cloud);
      ASSERT_TRUE(box) << box.error();
      EXPECT_EQ(box->count, expected_box.count) << where;
      EXPECT_EQ(box->min, expected_box.min) << where;
      EXPECT_EQ(box->max, expected_box.max) << where;

      const auto access = resolveXyz(cloud);
      ASSERT_TRUE(access) << access.error();
      std::vector<size_t> iterated;
      size_t next = 0;
      while (const auto point = nextFinitePoint(cloud, *access, next)) {
        iterated.push_back(point->index);
        EXPECT_EQ(point->position, *referenceXyz(cloud, point->index)) << where;
      }
      EXPECT_EQ(iterated, finite) << where;

      for (const size_t field : std::array<size_t, 4>{0, 2, 3, 4}) {  // x, z, intensity, ring
        const std::string& channel = cloud.fields[field].name;
        for (const Extreme which : {Extreme::kMin, Extreme::kMax}) {
          std::optional<size_t> best;
          double best_value = 0.0;
          for (size_t index = 0; index < records; ++index) {
            const double value = referenceRead(cloud, index, cloud.fields[field]);
            const bool better = !best || (which == Extreme::kMax ? value > best_value : value < best_value);
            if (std::isfinite(value) && better && referenceXyz(cloud, index)) {
              best = index;
              best_value = value;
            }
          }
          const auto extreme = extremePoint(cloud, channel, which);
          ASSERT_TRUE(extreme) << extreme.error();
          ASSERT_EQ(extreme->has_value(), best.has_value()) << where << " " << channel;
          if (!best) {
            continue;
          }
          EXPECT_EQ((*extreme)->point.index, *best) << where << " " << channel << " max " << (which == Extreme::kMax);
          EXPECT_EQ((*extreme)->value, best_value) << where << " " << channel;
          EXPECT_EQ((*extreme)->point.position, *referenceXyz(cloud, *best)) << where << " " << channel;
        }
      }

      for (const auto& [low, high] : boxes) {
        std::vector<size_t> inside;
        for (const size_t index : finite) {
          const Vector3 p = *referenceXyz(cloud, index);
          if (p.x >= low.x && p.x <= high.x && p.y >= low.y && p.y <= high.y && p.z >= low.z && p.z <= high.z) {
            inside.push_back(index);
          }
        }
        const auto cropped = cropBox(cloud, low, high, {});
        ASSERT_TRUE(cropped) << cropped.error();
        EXPECT_EQ(cropped->width, inside.size()) << where;
        EXPECT_TRUE(cropped->is_dense) << where;
        EXPECT_EQ(bytesOf(*cropped), packedRecords(cloud, inside)) << where;
      }

      // A pure translation is exact in double, so the expected bytes are the input's with x/y/z moved, in
      // each channel's own type; non-finite records and the row padding stay as they were.
      sdk::FrameTransform shift;
      shift.translation = Vector3{0.5, -1.25, 2.0};
      std::vector<uint8_t> expected_bytes = bytesOf(cloud);
      for (const size_t index : finite) {
        const Vector3 p = *referenceXyz(cloud, index);
        uint8_t* record = expected_bytes.data() + referenceOffset(cloud, index);
        referenceWrite(record + cloud.fields[0].offset, cloud.fields[0].datatype, p.x + 0.5);
        referenceWrite(record + cloud.fields[1].offset, cloud.fields[1].datatype, p.y - 1.25);
        referenceWrite(record + cloud.fields[2].offset, cloud.fields[2].datatype, p.z + 2.0);
      }
      const auto moved = transformXyz(cloud, shift, {});
      ASSERT_TRUE(moved) << moved.error();
      EXPECT_EQ(bytesOf(*moved), expected_bytes) << where;
    }
  }
}

// A zero-width cloud is valid and empty, flat or with rows: every operation that walks records returns an
// empty result without stepping through its empty `data`.
TEST(PointCloudOps, ZeroWidthCloudsAreEmpty) {
  for (const uint32_t height : {1u, 3u}) {
    PointCloud cloud;
    cloud.width = 0;
    cloud.height = height;
    cloud.point_step = 16;
    cloud.row_step = 8;  // padding only: a walk over the rows would step past the empty buffer
    cloud.fields = xyziFields();
    const std::string where = "height " + std::to_string(height);
    const auto box = bounds(cloud);
    ASSERT_TRUE(box) << box.error();
    EXPECT_EQ(box->count, 0u) << where;
    const auto extreme = extremePoint(cloud, "intensity", Extreme::kMax);
    ASSERT_TRUE(extreme) << extreme.error();
    EXPECT_FALSE(extreme->has_value()) << where;
    const auto cropped = cropBox(cloud, kEverywhereMin, kEverywhereMax, {});
    ASSERT_TRUE(cropped) << cropped.error();
    EXPECT_EQ(cropped->width, 0u) << where;
    EXPECT_TRUE(cropped->data.empty()) << where;
    const auto filtered = filterByField(cloud, "intensity", CompareOp::kGe, 0.0, {});
    ASSERT_TRUE(filtered) << filtered.error();
    EXPECT_EQ(filtered->width, 0u) << where;
    const auto moved = transformXyz(cloud, sdk::FrameTransform{}, {});
    ASSERT_TRUE(moved) << moved.error();
    EXPECT_EQ(moved->height, height) << where;
    EXPECT_TRUE(moved->data.empty()) << where;
    const auto access = resolveXyz(cloud);
    ASSERT_TRUE(access) << access.error();
    size_t next = 0;
    EXPECT_FALSE(nextFinitePoint(cloud, *access, next).has_value()) << where;
  }
}

// A cancel that arrives once the output is complete does not undo it: the copy stops at the last kept
// record, and an empty result needs no copy at all.
TEST(PointCloudOps, ACancelAfterTheOutputIsCompleteKeepsTheResult) {
  const auto cloud = makeLineCloud(200'000);  // marking polls 4 times: at records 0, 65536, 131072, 196608
  int polls = 0;
  NativeLimits limits;
  limits.cancelled = [&polls] { return ++polls > 5; };  // the copy's first poll passes, any later one cancels
  const auto first_only = filterByField(cloud, "x", CompareOp::kEq, 0.0, limits);
  ASSERT_TRUE(first_only) << first_only.error();
  EXPECT_EQ(first_only->width, 1u);
  EXPECT_EQ(polls, 5);

  polls = 0;
  limits.cancelled = [&polls] { return ++polls > 4; };  // any poll of the copy cancels
  const auto none = filterByField(cloud, "x", CompareOp::kLt, 0.0, limits);
  ASSERT_TRUE(none) << none.error();
  EXPECT_EQ(none->width, 0u);
  EXPECT_EQ(polls, 4);
}

// compactWhere runs its predicate exactly once per record, flat or organized and across a partial last
// bitmap word, and outputs exactly the records it accepted.
TEST(RecordWalk, CompactionRunsThePredicateOncePerRecord) {
  constexpr uint32_t kRecords = 1000;  // 15 full bitmap words and 40 bits
  for (const Layout layout : {Layout::kFlat, Layout::kOrganized}) {
    const auto cloud = makeLaidOutCloud(layout, xyziFields(), 16, kRecords, [](uint8_t* record, size_t index) {
      const auto x = static_cast<float>(index);
      std::memcpy(record, &x, sizeof(x));
    });
    std::vector<int> calls(kRecords, 0);
    std::vector<size_t> accepted;
    const auto out = compactWhere(cloud, kRecords, {}, [&](const uint8_t* record) {
      float x = 0.0f;
      std::memcpy(&x, record, sizeof(x));
      const auto index = static_cast<size_t>(x);
      ++calls[index];
      const bool keep = index % 3 == 0;
      if (keep) {
        accepted.push_back(index);
      }
      return keep;
    });
    ASSERT_TRUE(out) << out.error();
    EXPECT_EQ(calls, std::vector<int>(kRecords, 1)) << layoutName(layout);
    EXPECT_EQ(bytesOf(*out), packedRecords(cloud, accepted)) << layoutName(layout);
  }
}

// The bitmap is charged while a compaction runs: a budget that holds the output alone fails, one that holds
// the output and the bitmap succeeds, and one byte less fails again.
TEST(PointCloudOps, TheCompactionBitmapIsCharged) {
  const auto cloud = makeLineCloud(600'000);
  constexpr std::size_t kBitmapBytes = (600'000 + 63) / 64 * sizeof(uint64_t);
  const Vector3 low{10, -1, -1};
  const Vector3 high{19, 1, 1};  // keeps 10 records
  const NativeLimits measuring = limitedTo(std::size_t{1} << 30);
  const auto measured = cropBox(cloud, low, high, measuring);
  ASSERT_TRUE(measured) << measured.error();
  const std::size_t output_charge = measuring.budget->used();  // the bitmap's charge is gone by now

  const auto output_only = cropBox(cloud, low, high, limitedTo(output_charge));
  ASSERT_FALSE(output_only);
  EXPECT_TRUE(contains(output_only.error(), "native memory limit")) << output_only.error();
  const auto both = cropBox(cloud, low, high, limitedTo(output_charge + kBitmapBytes));
  ASSERT_TRUE(both) << both.error();
  EXPECT_EQ(both->width, 10u);
  EXPECT_FALSE(cropBox(cloud, low, high, limitedTo(output_charge + kBitmapBytes - 1)).has_value());
}

// PJ_BENCH=1 prints each point-cloud operation's median time per point over 20 runs, on two 260k-point
// XYZI float32 scans: flat, and organized as 64 rows of 4096 with 64 bytes of padding per row. The calls
// are the binders' own, with an evaluation budget and a cancel check, so production adds no per-point cost.
TEST(PointCloudOpsBench, NanosecondsPerPoint) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  std::mt19937 random(42);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  const auto fill = [&](uint8_t* record, size_t) {
    const float values[4] = {unit(random), unit(random), unit(random), unit(random)};
    std::memcpy(record, values, sizeof(values));
  };
  const PointCloud flat = makeCloud(xyziFields(), 16, 260'000, 1, 0, fill);
  const PointCloud organized = makeCloud(xyziFields(), 16, 4096, 64, 64, fill);

  NativeLimits limits = limitedTo(std::size_t{1} << 30);
  limits.cancelled = [] { return false; };
  const Vector3 crop_min{0, 0, 0};
  const Vector3 crop_max{0.669, 0.669, 0.669};  // 0.669^3 ~ 30% kept
  sdk::FrameTransform tf;
  tf.translation = Vector3{1.0, 2.0, 3.0};
  tf.rotation = sdk::Quaternion{0.0, 0.0, 0.38268343, 0.92387953};  // a 45 degree yaw

  for (const auto* cloud : {&flat, &organized}) {
    const double points = static_cast<double>(countPoints(*cloud));
    const auto measure = [&](const char* name, auto&& call) {
      std::vector<double> ns_per_point;
      double result = 0.0;  // what the call counted: records kept, finite points, or points visited
      for (int run = 0; run < 20; ++run) {
        const auto start = std::chrono::steady_clock::now();
        result = static_cast<double>(call());
        const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start);
        ns_per_point.push_back(elapsed.count() / points);
      }
      std::sort(ns_per_point.begin(), ns_per_point.end());
      std::printf(
          "[bench] %-9s %-15s median %6.2f ns/point (min %6.2f), result %5.1f%% of the points\n",
          cloud == &flat ? "flat" : "organized", name, (ns_per_point[9] + ns_per_point[10]) / 2.0, ns_per_point[0],
          100.0 * result / points);
    };
    measure("filterByField", [&] {
      const auto out = filterByField(*cloud, "intensity", CompareOp::kGt, 0.5, limits);
      EXPECT_TRUE(out);
      return out ? out->width : 0u;
    });
    measure("cropBox", [&] {
      const auto out = cropBox(*cloud, crop_min, crop_max, limits);
      EXPECT_TRUE(out);
      return out ? out->width : 0u;
    });
    measure("bounds", [&] {
      const auto box = bounds(*cloud);
      EXPECT_TRUE(box);
      return box ? box->count : 0u;
    });
    measure("extremePoint", [&] {
      const auto top = extremePoint(*cloud, "intensity", Extreme::kMax);
      EXPECT_TRUE(top && top->has_value());
      return top && top->has_value() ? std::size_t{1} : 0u;
    });
    measure("transformXyz", [&] {
      const auto out = transformXyz(*cloud, tf, limits);
      EXPECT_TRUE(out);
      return out ? countPoints(*out) : 0u;
    });
    const auto access = resolveXyz(*cloud);
    ASSERT_TRUE(access) << access.error();
    measure("nextFinitePoint", [&] {
      size_t next = 0;
      size_t visited = 0;
      while (nextFinitePoint(*cloud, *access, next)) {
        ++visited;
      }
      return visited;
    });
  }
}

// ---- memory accounting ------------------------------------------------------

// BUD-13 / BUD-14: a crop whose output would pass the evaluation's native limit fails before allocating,
// and the error names that limit.
TEST(PointCloudOps, ByteBudgetTripsOnHugeCrop) {
  const auto cloud = makeLineCloud(600'000);  // 9.6 MB of records
  const NativeLimits limits = limitedTo(std::size_t{8} << 20);
  const auto everything = cropBox(cloud, kEverywhereMin, kEverywhereMax, limits);
  ASSERT_FALSE(everything);
  EXPECT_EQ(everything.error(), "script native memory limit (8 MB) exceeded");
  EXPECT_EQ(limits.budget->used(), 0u);
  EXPECT_FALSE(transformXyz(cloud, sdk::FrameTransform{}, limits).has_value());
  EXPECT_EQ(limits.budget->used(), 0u);
  // A crop is charged what it keeps, so a small region of the same scan fits.
  const auto region = cropBox(cloud, Vector3{10, -1, -1}, Vector3{19, 1, 1}, limits);
  ASSERT_TRUE(region) << region.error();
  EXPECT_EQ(region->width, 10u);
  EXPECT_GE(limits.budget->used(), 10u * 16u + sizeof(PointCloud));  // the output header is charged too
  EXPECT_LT(limits.budget->used(), 10u * 16u + sizeof(PointCloud) + 1024u);
}

// BUD-15: retained outputs count against one limit per evaluation and release it when destroyed.
TEST(PointCloudOps, AggregateNativeCloudBudgetRejectsRetainedSecondOutput) {
  const auto cloud = makeXyziCloud(std::vector<std::array<float, 4>>(327'680, {0, 0, 0, 1}));  // 5 MiB
  const NativeLimits limits = limitedTo(std::size_t{8} << 20);
  const Vector3 low{-1, -1, -1};
  const Vector3 high{1, 1, 1};
  auto cropped = cropBox(cloud, low, high, limits);
  ASSERT_TRUE(cropped) << cropped.error();
  std::optional<PointCloud> first = std::move(*cropped);
  EXPECT_GE(limits.budget->used(), (std::size_t{5} << 20) + sizeof(PointCloud));
  const auto second = cropBox(cloud, low, high, limits);
  ASSERT_FALSE(second);
  EXPECT_TRUE(contains(second.error(), "native memory limit")) << second.error();
  first.reset();  // the only owner of the first output's buffer
  EXPECT_EQ(limits.budget->used(), 0u);
  EXPECT_TRUE(cropBox(cloud, low, high, limits).has_value());
}

// A copy of an output shares its buffer, so the charge lasts until the last copy dies.
TEST(PointCloudOps, ACopyOfAChargedCloudKeepsTheCharge) {
  const auto cloud = makeLineCloud(100);
  const NativeLimits limits = limitedTo(std::size_t{1} << 20);
  auto cropped = cropBox(cloud, kEverywhereMin, kEverywhereMax, limits);
  ASSERT_TRUE(cropped) << cropped.error();
  std::optional<PointCloud> original = std::move(*cropped);
  const std::size_t charged = limits.budget->used();
  ASSERT_GT(charged, 0u);
  std::optional<PointCloud> copy = original;
  original.reset();
  EXPECT_EQ(limits.budget->used(), charged);
  EXPECT_FLOAT_EQ(floatAt(*copy, 99, 0), 99.0f);
  copy.reset();
  EXPECT_EQ(limits.budget->used(), 0u);
}

// Measurement: a crop of a 2M-point scan, half of it kept. Prints its time and the bytes it charged,
// which are its peak native allocation (the output is allocated once, at its exact size).
TEST(PointCloudOps, CropOfTwoMillionPointsReportsTimeAndPeakBytes) {
  constexpr size_t kPoints = 2'000'000;
  const auto cloud = makeLineCloud(kPoints);
  NativeLimits limits = limitedTo(std::size_t{1} << 30);
  limits.cancelled = [] { return false; };
  const auto start = std::chrono::steady_clock::now();
  const auto out = cropBox(cloud, Vector3{0, -1, -1}, Vector3{kPoints / 2.0 - 1.0, 1, 1}, limits);
  const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
  ASSERT_TRUE(out) << out.error();
  EXPECT_EQ(out->width, kPoints / 2);
  EXPECT_GE(limits.budget->used(), kPoints / 2 * 16);
  std::printf(
      "[ MEASURE  ] cropBox of %zu points (%zu bytes in): %.2f ms, %zu bytes charged at peak\n", kPoints,
      cloud.data.size(), elapsed.count(), limits.budget->used());
}

// ---- scene-entities builder -------------------------------------------------

TEST(SceneEntityBuilder, SceneBuilderProducesSnapshotIdiom) {
  // An empty builder is a clear-only snapshot, not a bare entity.
  auto empty = SceneEntityBuilder::create("base_link", "witness", nullptr);
  ASSERT_TRUE(empty) << empty.error();
  const auto cleared = std::move(*empty).finish();
  ASSERT_EQ(cleared.scene.deletions.size(), 1u);
  EXPECT_EQ(cleared.scene.deletions[0].type, sdk::SceneEntityDeletion::Type::kAll);
  EXPECT_TRUE(cleared.scene.entities.empty());

  auto builder = SceneEntityBuilder::create("base_link", "witness", nullptr);
  ASSERT_TRUE(builder) << builder.error();
  ASSERT_TRUE(builder->addSphere(Vector3{1, 2, 3}, 0.2, sdk::ColorRGBA{255, 0, 0, 255}));
  ASSERT_TRUE(builder->addText(Vector3{1, 2, 3.3}, "1.00", 0.25, sdk::ColorRGBA{}, false));
  const auto full = std::move(*builder).finish();
  ASSERT_EQ(full.scene.deletions.size(), 1u);
  EXPECT_EQ(full.scene.deletions[0].type, sdk::SceneEntityDeletion::Type::kAll);
  EXPECT_EQ(full.scene.deletions[0].timestamp, 0);
  ASSERT_EQ(full.scene.entities.size(), 1u);
  const auto& entity = full.scene.entities[0];
  EXPECT_EQ(entity.frame_id, "base_link");
  EXPECT_EQ(entity.id, "witness");
  EXPECT_EQ(entity.timestamp, 0);  // stamped when the runtime publishes it
  ASSERT_EQ(entity.spheres.size(), 1u);
  EXPECT_EQ(entity.spheres[0].pose.position, (Vector3{1, 2, 3}));
  EXPECT_DOUBLE_EQ(entity.spheres[0].pose.orientation.w, 1.0);
  EXPECT_DOUBLE_EQ(entity.spheres[0].size.x, 0.2);
  EXPECT_EQ(entity.spheres[0].color.r, 255);
  ASSERT_EQ(entity.texts.size(), 1u);
  EXPECT_EQ(entity.texts[0].text, "1.00");
  EXPECT_EQ(entity.texts[0].pose.position, (Vector3{1, 2, 3.3}));
  EXPECT_DOUBLE_EQ(entity.texts[0].font_size, 0.25);
}

TEST(SceneEntityBuilder, SceneBuilderChainsAllPrimitives) {
  auto builder = SceneEntityBuilder::create("map", "e1", nullptr);
  ASSERT_TRUE(builder) << builder.error();
  ASSERT_TRUE(builder->addSphere(Vector3{0, 0, 0}, 1.0, sdk::ColorRGBA{255, 0, 0, 255}));
  ASSERT_TRUE(builder->addText(Vector3{1, 1, 1}, "hi", 0.5, sdk::ColorRGBA{}, true));
  ASSERT_TRUE(builder->addCube(Vector3{2, 2, 2}, Vector3{1, 2, 3}, sdk::ColorRGBA{0, 255, 0, 128}));
  ASSERT_TRUE(builder->addLineStrip({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}}, 0.05, sdk::ColorRGBA{}));
  const auto out = std::move(*builder).finish();
  ASSERT_EQ(out.scene.entities.size(), 1u);
  const auto& entity = out.scene.entities[0];
  ASSERT_EQ(entity.spheres.size(), 1u);
  ASSERT_EQ(entity.texts.size(), 1u);
  EXPECT_TRUE(entity.texts[0].billboard);
  ASSERT_EQ(entity.cubes.size(), 1u);
  EXPECT_EQ(entity.cubes[0].pose.position, (Vector3{2, 2, 2}));
  EXPECT_EQ(entity.cubes[0].size, (Vector3{1, 2, 3}));
  EXPECT_EQ(entity.cubes[0].color.a, 128);
  ASSERT_EQ(entity.lines.size(), 1u);
  EXPECT_EQ(entity.lines[0].type, sdk::LineType::kLineStrip);
  EXPECT_DOUBLE_EQ(entity.lines[0].thickness, 0.05);
  ASSERT_EQ(entity.lines[0].points.size(), 4u);
  EXPECT_EQ(entity.lines[0].points[3], (sdk::Point3{1, 1, 1}));
}

// D27's builder names the field a script got wrong.
TEST(SceneEntityBuilder, SceneBuilderRejectsBadArgs) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const Vector3 bad{nan, 0, 0};
  const Vector3 origin{0, 0, 0};
  const auto missing_id = SceneEntityBuilder::create("map", "", nullptr);
  ASSERT_FALSE(missing_id);
  EXPECT_TRUE(contains(missing_id.error(), "'id'")) << missing_id.error();

  auto builder = SceneEntityBuilder::create("map", "e1", nullptr);
  ASSERT_TRUE(builder) << builder.error();
  const auto expectField = [](const Status& status, const char* field) {
    ASSERT_FALSE(status) << field;
    EXPECT_TRUE(contains(status.error(), field)) << status.error();
  };
  expectField(builder->addSphere(bad, 1.0, {}), "'center'");
  expectField(builder->addSphere(origin, nan, {}), "'diameter'");
  expectField(builder->addCube(origin, bad, {}), "'size'");
  expectField(builder->addText(bad, "x", 1.0, {}, false), "'position'");
  expectField(builder->addText(origin, "x", 0.0, {}, false), "'font_size'");
  expectField(builder->addText(origin, "", 1.0, {}, false), "'text'");
  expectField(builder->addLineStrip({{0, 0, 0}}, 0.1, {}), "'points'");
  expectField(builder->addLineStrip({{0, 0, 0}, {nan, 0, 0}}, 0.1, {}), "'points'");
  expectField(builder->addLineStrip({{0, 0, 0}, {1, 0, 0}}, nan, {}), "'thickness'");
  EXPECT_TRUE(std::move(*builder).finish().scene.entities.empty());  // a rejected primitive is not stored
}

// A size is a finite number greater than 0: zero and negative sizes are rejected like NaN, naming the field.
TEST(SceneEntityBuilder, SceneBuilderRejectsNonPositiveSizes) {
  const Vector3 origin{0, 0, 0};
  auto builder = SceneEntityBuilder::create("map", "e1", nullptr);
  ASSERT_TRUE(builder) << builder.error();
  const auto expectError = [](const Status& status, const std::string& message) {
    ASSERT_FALSE(status) << message;
    EXPECT_EQ(status.error(), message);
  };
  for (const double size : {0.0, -0.0, -1.0, -1e-9, std::numeric_limits<double>::quiet_NaN(), -kInf, kInf}) {
    expectError(builder->addSphere(origin, size, {}), "sphere: 'diameter' must be a finite number greater than 0");
    for (const Vector3& cube : {Vector3{size, 1, 1}, Vector3{1, size, 1}, Vector3{1, 1, size}}) {
      expectError(builder->addCube(origin, cube, {}), "cube: 'size' must be a finite number greater than 0");
    }
    expectError(
        builder->addLineStrip({{0, 0, 0}, {1, 0, 0}}, size, {}),
        "line_strip: 'thickness' must be a finite number greater than 0");
    expectError(
        builder->addText(origin, "x", size, {}, false), "text: 'font_size' must be a finite number greater than 0");
  }
  EXPECT_TRUE(std::move(*builder).finish().scene.entities.empty());
}

// A builder grown in a loop stops at the evaluation's limit; finishing moves the charge into the snapshot,
// and every byte comes back when the snapshot is gone.
TEST(SceneEntityBuilder, SceneBuilderChargesTheBudget) {
  auto budget = std::make_shared<MemoryBudget>(4096);
  {
    auto builder = SceneEntityBuilder::create("map", "e1", budget);
    ASSERT_TRUE(builder) << builder.error();
    Status added = okStatus();
    int spheres = 0;
    while (added && spheres < 1000) {
      added = builder->addSphere(Vector3{0, 0, 0}, 1.0, {});
      spheres += added ? 1 : 0;
    }
    ASSERT_FALSE(added);
    EXPECT_TRUE(contains(added.error(), "native memory limit")) << added.error();
    EXPECT_GT(spheres, 0);
    EXPECT_LE(budget->used(), budget->limit());
  }
  EXPECT_EQ(budget->used(), 0u);
  {
    auto builder = SceneEntityBuilder::create("map", "e1", budget);
    ASSERT_TRUE(builder) << builder.error();
    ASSERT_TRUE(builder->addSphere(Vector3{0, 0, 0}, 1.0, {}));
    const std::size_t builder_bytes = budget->used();
    const auto snapshot = std::move(*builder).finish();
    EXPECT_EQ(budget->used(), builder_bytes);
  }
  EXPECT_EQ(budget->used(), 0u);
}

}  // namespace
}  // namespace PJ::scripting::native
