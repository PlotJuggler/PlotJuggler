// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The camera's scene bounds, as the dock pushes them to either renderer's view:
// the union of every layer's worldBounds() placed in the fixed frame, hidden
// layers included; layers without worldBounds() (poses) add nothing. Needs no
// GPU: the view is never shown.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <cstring>
#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/point_cloud_codec.hpp"
#include "pj_base/builtin/poses_in_frame_codec.hpp"
#include "pj_base/builtin/voxel_grid_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/layers/voxel_grid_layer.h"
#include "pj_scene3d_widgets/scene_view_widget.h"

using namespace Qt::StringLiterals;
using BT = PJ::sdk::BuiltinObjectType;

namespace {

constexpr const char* kFrame = "map";

std::vector<uint8_t> cloudBytes(const std::vector<glm::vec3>& points) {
  using DT = PJ::sdk::PointField::Datatype;
  PJ::sdk::PointCloud cloud;
  cloud.frame_id = kFrame;
  cloud.fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
  cloud.point_step = 12;
  cloud.width = static_cast<uint32_t>(points.size());
  cloud.height = 1;
  cloud.row_step = cloud.point_step * cloud.width;
  std::vector<uint8_t> bytes(cloud.row_step);
  std::memcpy(bytes.data(), points.data(), bytes.size());
  cloud.data = PJ::Span<const uint8_t>(bytes.data(), bytes.size());
  cloud.timestamp_ns = 100;
  return PJ::serializePointCloud(cloud);
}

std::vector<uint8_t> poseBytes(glm::vec3 position) {
  PJ::sdk::PosesInFrame poses;
  poses.frame_id = kFrame;
  poses.timestamp_ns = 100;
  PJ::sdk::Pose pose;
  pose.position = {position.x, position.y, position.z};
  pose.orientation.w = 1.0;
  poses.poses.push_back(pose);
  return PJ::serializePosesInFrame(poses);
}

// One uint8 voxel of `cell` metres, its lower corner at `origin`.
std::vector<uint8_t> voxelBytes(glm::vec3 origin, float cell) {
  static const std::vector<std::uint8_t> kValues{255};
  PJ::sdk::VoxelGrid grid;
  grid.frame_id = kFrame;
  grid.timestamp_ns = 100;
  grid.origin.position = {origin.x, origin.y, origin.z};
  grid.origin.orientation.w = 1.0;
  grid.cell_size = {cell, cell, cell};
  grid.column_count = 1;
  grid.row_count = 1;
  grid.slice_count = 1;
  grid.cell_stride = 1;
  grid.row_stride = 1;
  grid.slice_stride = 1;
  PJ::sdk::PointField field;
  field.name = "occupancy";
  field.datatype = PJ::sdk::PointField::Datatype::kUint8;
  field.count = 1;
  grid.fields = {field};
  grid.data = PJ::Span<const std::uint8_t>(kValues.data(), kValues.size());
  return PJ::serializeVoxelGrid(grid);
}

class DockFixture {
 public:
  DockFixture() {
    dataset_ = *session_.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "test", .time_domain_id = 0});
    dock_.setSessionManager(&session_);
  }
  PJ::ObjectTopicId add(const std::string& topic, BT type, std::vector<uint8_t> bytes) {
    const auto id = session_.objectStore().registerTopic(
        PJ::ObjectTopicDescriptor{
            .dataset_id = dataset_,
            .topic_name = topic,
            .metadata_json = R"({"builtin_object_type":")" + std::string(PJ::sdk::name(type)) + "\"}",
        });
    EXPECT_TRUE(id.has_value());
    EXPECT_TRUE(session_.objectStore().pushOwned(*id, 100, std::move(bytes)).has_value());
    EXPECT_TRUE(dock_.addTopic(*id, type, QString::fromStdString(topic)));
    return *id;
  }
  PJ::Scene3DDockWidget& dock() {
    return dock_;
  }

 private:
  PJ::SessionManager session_;
  PJ::DatasetId dataset_ = 0;
  PJ::Scene3DDockWidget dock_;
};

// Processes events until every listed layer holds decoded bounds (decodes may be async).
bool waitForBounds(PJ::Scene3DDockWidget& dock, const std::vector<PJ::ObjectTopicId>& topics) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 5000) {
    bool all = true;
    for (const auto topic : topics) {
      auto* layer = dynamic_cast<pj::scene3d::Scene3DLayer*>(dock.layerFor(topic));
      all = all && layer != nullptr && layer->worldBounds().has_value();
    }
    if (all) {
      return true;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
  return false;
}

}  // namespace

TEST(Scene3DDockCameraBounds, HiddenLayersCountPosesDoNot) {
  DockFixture fixture;
  const auto near_layer = fixture.add("/near", BT::kPointCloud, cloudBytes({{1.0F, 2.0F, 3.0F}, {2.0F, 3.0F, 4.0F}}));
  const auto far_layer = fixture.add("/far", BT::kPointCloud, cloudBytes({{100.0F, 2.0F, 3.0F}}));
  const auto voxels = fixture.add("/voxels", BT::kVoxelGrid, voxelBytes({1.0F, 2.0F, 49.0F}, 1.0F));
  fixture.add("/poses", BT::kPosesInFrame, poseBytes({-50.0F, -60.0F, -70.0F}));
  PJ::Scene3DDockWidget& dock = fixture.dock();
  dock.setFixedFrame(QString::fromLatin1(kFrame));
  dock.onTrackerTime(0.0);
  // A voxel grid decodes when painted; this view never paints.
  auto* voxel_layer = dynamic_cast<pj::scene3d::VoxelGridLayer*>(dock.layerFor(voxels));
  ASSERT_NE(voxel_layer, nullptr);
  voxel_layer->renderAtForTest(100);
  ASSERT_TRUE(waitForBounds(dock, {near_layer, far_layer, voxels}));
  dock.setLayerVisible(far_layer, false);
  dock.setLayerVisible(voxels, false);
  ASSERT_NE(dock.sceneView(), nullptr);

  const pj::scene3d::AABB bounds = dock.sceneView()->sceneBounds();
  ASSERT_TRUE(bounds.valid) << "the view has no camera bounds without a frame";
  EXPECT_FLOAT_EQ(bounds.min.x, 1.0F) << "the pose layer must not extend the bounds";
  EXPECT_FLOAT_EQ(bounds.min.y, 2.0F);
  EXPECT_FLOAT_EQ(bounds.min.z, 3.0F);
  EXPECT_FLOAT_EQ(bounds.max.x, 100.0F) << "the hidden cloud must still count";
  EXPECT_FLOAT_EQ(bounds.max.y, 3.0F);
  EXPECT_FLOAT_EQ(bounds.max.z, 50.0F) << "the hidden voxel grid must still count";
}
