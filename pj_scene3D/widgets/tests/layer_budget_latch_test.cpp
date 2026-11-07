// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The per-view budget latch of every budgeted layer (point cloud, depth cloud,
// poses, voxels, occupancy grid), driven the way the QRhi view drives it:
// setTrackerTime() and then prepareForRender() against the remaining budget. A
// sample that does not fit is dropped and latched; the tracker then moves to a
// smaller sample that fits the same budget, and the layer must draw it on the
// next frame instead of waiting for the old sample's size to fit.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/layers/depth_cloud_layer.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"
#include "pj_scene3d_widgets/layers/voxel_grid_layer.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;
using BT = PJ::sdk::BuiltinObjectType;

// Every mock sample carries its size in its first payload byte.
std::uint8_t sizeOf(const PJ::sdk::PayloadView& payload) {
  return payload.bytes.empty() ? 0 : payload.bytes.data()[0];
}

// A static zero buffer every mock sample views (zero-copy Spans must outlive the parse).
const std::array<std::uint8_t, 4096>& zeros() {
  static const std::array<std::uint8_t, 4096> buffer{};
  return buffer;
}

// `size` float32 xyz points (all at the origin: finite, which is what counts).
PJ::Expected<PJ::sdk::ObjectRecord> emitCloud(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  PJ::sdk::PointCloud cloud;
  cloud.timestamp_ns = ts;
  cloud.frame_id = "map";
  cloud.width = sizeOf(payload);
  cloud.height = 1;
  cloud.point_step = 12;
  cloud.row_step = 12U * cloud.width;
  cloud.fields = {
      PJ::sdk::PointField{"x", 0, PJ::sdk::PointField::Datatype::kFloat32, 1},
      PJ::sdk::PointField{"y", 4, PJ::sdk::PointField::Datatype::kFloat32, 1},
      PJ::sdk::PointField{"z", 8, PJ::sdk::PointField::Datatype::kFloat32, 1}};
  cloud.data = PJ::Span<const std::uint8_t>(zeros().data(), cloud.row_step);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = cloud};
}

// A 2x2 32FC1 depth image whose first `size` pixels are valid (1 m), the rest 0.
PJ::Expected<PJ::sdk::ObjectRecord> emitDepth(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  static const std::array<float, 4> kFour = {1.0F, 1.0F, 1.0F, 1.0F};
  static const std::array<float, 4> kOne = {1.0F, 0.0F, 0.0F, 0.0F};
  const auto& depths = sizeOf(payload) >= 4 ? kFour : kOne;
  PJ::sdk::Image image;
  image.width = 2;
  image.height = 2;
  image.encoding = "32FC1";
  image.frame_id = "cam";
  image.timestamp_ns = ts;
  image.data = PJ::Span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(depths.data()), sizeof(depths));
  return PJ::sdk::ObjectRecord{.ts = ts, .object = image};
}

PJ::Expected<PJ::sdk::ObjectRecord> emitCameraInfo(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  PJ::sdk::CameraInfo info;
  info.frame_id = "cam";
  info.width = 2;
  info.height = 2;
  info.timestamp_ns = ts;
  info.K = {100.0, 0.0, 0.5, 0.0, 100.0, 0.5, 0.0, 0.0, 1.0};
  return PJ::sdk::ObjectRecord{.ts = ts, .object = info};
}

// `size` identity poses along +X: three arms each.
PJ::Expected<PJ::sdk::ObjectRecord> emitPoses(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  PJ::sdk::PosesInFrame poses;
  poses.timestamp_ns = ts;
  poses.frame_id = "map";
  for (std::uint8_t i = 0; i < sizeOf(payload); ++i) {
    PJ::sdk::Pose pose;
    pose.position = {static_cast<double>(i), 0.0, 0.0};
    poses.poses.push_back(pose);
  }
  return PJ::sdk::ObjectRecord{.ts = ts, .object = poses};
}

// A row of `size` uint8 voxels.
PJ::Expected<PJ::sdk::ObjectRecord> emitVoxels(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  PJ::sdk::VoxelGrid grid;
  grid.timestamp_ns = ts;
  grid.frame_id = "map";
  grid.cell_size = {1.0, 1.0, 1.0};
  grid.column_count = sizeOf(payload);
  grid.row_count = 1;
  grid.slice_count = 1;
  grid.cell_stride = 1;
  grid.row_stride = grid.column_count;
  grid.slice_stride = grid.column_count;
  grid.fields = {PJ::sdk::PointField{"occupancy", 0, PJ::sdk::PointField::Datatype::kUint8, 1}};
  grid.data = PJ::Span<const std::uint8_t>(zeros().data(), grid.column_count);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = grid};
}

// A free `size` x `size` occupancy grid.
PJ::Expected<PJ::sdk::ObjectRecord> emitOccupancy(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  PJ::sdk::OccupancyGrid grid;
  grid.timestamp_ns = ts;
  grid.frame_id = "map";
  grid.resolution = 1.0;
  grid.width = sizeOf(payload);
  grid.height = sizeOf(payload);
  grid.data = PJ::Span<const std::uint8_t>(zeros().data(), static_cast<std::size_t>(grid.width) * grid.height);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = grid};
}

// A topic in dataset 1 carrying its builtin type (how the depth layer finds CameraInfo).
PJ::ObjectTopicId registerTyped(PJ::SessionManager& session, const std::string& name, BT type) {
  const auto id = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = 1,
          .topic_name = name,
          .metadata_json = R"({"builtin_object_type":")" + std::string(PJ::sdk::name(type)) + "\"}",
      });
  EXPECT_TRUE(id.has_value());
  return *id;
}

// One topic with a big sample at t=100 and a small one at t=200.
template <typename CreateFn>
PJ::ObjectTopicId addTopic(
    PJ::SessionManager& session, const std::string& name, BT type, CreateFn create, std::uint8_t big,
    std::uint8_t small) {
  const PJ::ObjectTopicId id = registerTyped(session, name, type);
  EXPECT_TRUE(session.objectStore().pushOwned(id, 100, std::vector<std::uint8_t>{big}).has_value());
  EXPECT_TRUE(session.objectStore().pushOwned(id, 200, std::vector<std::uint8_t>{small}).has_value());
  session.registerObjectTopicParser(id, makeBoundHandle("mock/" + name, create));
  return id;
}

pj::scene3d::Scene3DLayerContext contextFor(PJ::SessionManager& session) {
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  return ctx;
}

// Rejects the big sample at t=100, moves to the small one at t=200, and returns
// the layer's size after the next frame. `settle` lets an async layer land its
// sample; `size` reads what the layer would draw.
template <typename Layer, typename Size, typename Settle>
std::uint64_t sizeAfterMovingToASampleThatFits(Layer& layer, std::uint64_t budget, Size size, Settle settle) {
  layer.setTrackerTime(PJ::fromRaw(100));
  layer.prepareForRender(budget);
  settle();
  EXPECT_EQ(size(), 0U) << "the sample over the budget was not dropped";
  EXPECT_FALSE(layer.statusWarning().isEmpty()) << "the rejection left no warning";

  layer.setTrackerTime(PJ::fromRaw(200));
  layer.prepareForRender(budget);
  settle();
  layer.prepareForRender(budget);  // an async sample is admitted on the frame after it lands
  return size();
}

constexpr auto kSync = [] {};

TEST(LayerBudgetLatch, PointCloudDrawsALaterSampleThatFits) {
  PJ::SessionManager session;
  const auto topic = addTopic(
      session, "/cloud", BT::kPointCloud,
      []() noexcept -> void* { return new CountingObjectParser("mock//cloud", BT::kPointCloud, nullptr, &emitCloud); },
      10, 2);
  pj::scene3d::PointCloudLayer layer(topic, u"cloud"_s, BT::kPointCloud);
  ASSERT_TRUE(layer.attach(contextFor(session)));
  EXPECT_EQ(sizeAfterMovingToASampleThatFits(layer, 5, [&] { return std::uint64_t{layer.pointCount()}; }, kSync), 2U);
  EXPECT_TRUE(layer.statusWarning().isEmpty()) << layer.statusWarning().toStdString();
}

TEST(LayerBudgetLatch, DepthCloudDrawsALaterSampleThatFits) {
  PJ::SessionManager session;
  const auto depth = addTopic(
      session, "/cam/depth/image", BT::kImage,
      []() noexcept -> void* {
        return new CountingObjectParser("mock//cam/depth/image", BT::kImage, nullptr, &emitDepth);
      },
      4, 1);
  addTopic(
      session, "/cam/depth/camera_info", BT::kCameraInfo,
      []() noexcept -> void* {
        return new CountingObjectParser("mock//cam/depth/camera_info", BT::kCameraInfo, nullptr, &emitCameraInfo);
      },
      0, 0);
  pj::scene3d::DepthCloudLayer layer(depth, u"depth"_s, BT::kImage);
  ASSERT_TRUE(layer.attach(contextFor(session)));
  const auto settle = [&] {
    ASSERT_TRUE(pumpUntil([&] { return !layer.decodeInFlightForTest(); }, std::chrono::seconds(10)));
  };
  settle();
  EXPECT_EQ(sizeAfterMovingToASampleThatFits(layer, 2, [&] { return std::uint64_t{layer.pointCount()}; }, settle), 1U);
}

TEST(LayerBudgetLatch, PosesDrawALaterSampleThatFits) {
  PJ::SessionManager session;
  const auto topic = addTopic(
      session, "/poses", BT::kPosesInFrame,
      []() noexcept -> void* {
        return new CountingObjectParser("mock//poses", BT::kPosesInFrame, nullptr, &emitPoses);
      },
      10, 1);
  pj::scene3d::PosesInFrameLayer layer(topic, u"poses"_s);
  ASSERT_TRUE(layer.attach(contextFor(session)));
  EXPECT_EQ(
      sizeAfterMovingToASampleThatFits(
          layer, 10, [&] { return std::uint64_t{layer.instancesForTest().size()}; }, kSync),
      3U);
}

TEST(LayerBudgetLatch, VoxelsDrawALaterSampleThatFits) {
  PJ::SessionManager session;
  const auto topic = addTopic(
      session, "/voxels", BT::kVoxelGrid,
      []() noexcept -> void* { return new CountingObjectParser("mock//voxels", BT::kVoxelGrid, nullptr, &emitVoxels); },
      20, 4);
  pj::scene3d::VoxelGridLayer layer(topic, u"voxels"_s);
  ASSERT_TRUE(layer.attach(contextFor(session)));
  EXPECT_EQ(
      sizeAfterMovingToASampleThatFits(
          layer, 10, [&] { return layer.hasVolume() ? layer.voxelCount() : std::uint64_t{0}; }, kSync),
      4U);
}

TEST(LayerBudgetLatch, OccupancyGridDrawsALaterSampleThatFits) {
  PJ::SessionManager session;
  const auto topic = addTopic(
      session, "/map", BT::kOccupancyGrid,
      []() noexcept -> void* {
        return new CountingObjectParser("mock//map", BT::kOccupancyGrid, nullptr, &emitOccupancy);
      },
      8, 2);
  pj::scene3d::OccupancyGridLayer layer(topic, u"map"_s);
  ASSERT_TRUE(layer.attach(contextFor(session)));
  EXPECT_EQ(
      sizeAfterMovingToASampleThatFits(
          layer, 10, [&] { return std::uint64_t{layer.grid().width} * layer.grid().height; }, kSync),
      4U);
}

}  // namespace
