// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Every layer's Settings-panel config widget must use the same vertical row
// pitch (Space::Snug) as the built-in Grid / Transforms-and-RobotModel grids in
// Scene3DConfigPanel, so switching the selected topic doesn't visibly loosen or
// tighten the panel. PosesInFrameLayer and SceneEntitiesLayer already followed
// this convention; the rest had drifted to the looser Space::Comfortable.

#include <gtest/gtest.h>

#include <QApplication>
#include <QFormLayout>
#include <QWidget>
#include <cstdint>
#include <memory>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_scene3d_widgets/layers/depth_cloud_layer.h"
#include "pj_scene3d_widgets/layers/grid_map_layer.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"
#include "pj_scene3d_widgets/layers/robot_model_layer.h"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
#include "pj_scene3d_widgets/layers/voxel_grid_layer.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace {

PJ::ObjectTopicId topic(uint32_t id) {
  PJ::ObjectTopicId topic_id;
  topic_id.id = id;
  return topic_id;
}

// The layers under test each add exactly one top-level QFormLayout directly
// onto the container returned by createConfigWidget.
QFormLayout* topForm(QWidget* container) {
  return container->findChild<QFormLayout*>();
}

int snugPx() {
  return PJ::theme::space(PJ::theme::Space::Snug);
}

TEST(LayerConfigWidgetRowSpacing, PointCloud) {
  pj::scene3d::PointCloudLayer layer(topic(1), u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  std::unique_ptr<QWidget> widget(layer.createConfigWidget(nullptr));
  QFormLayout* form = topForm(widget.get());
  ASSERT_NE(form, nullptr);
  EXPECT_EQ(form->verticalSpacing(), snugPx());
}

TEST(LayerConfigWidgetRowSpacing, DepthCloud) {
  pj::scene3d::DepthCloudLayer layer(topic(2), u"depth"_s, PJ::sdk::BuiltinObjectType::kImage);
  std::unique_ptr<QWidget> widget(layer.createConfigWidget(nullptr));
  QFormLayout* form = topForm(widget.get());
  ASSERT_NE(form, nullptr);
  EXPECT_EQ(form->verticalSpacing(), snugPx());
}

TEST(LayerConfigWidgetRowSpacing, OccupancyGrid) {
  pj::scene3d::OccupancyGridLayer layer(topic(3), u"map"_s);
  std::unique_ptr<QWidget> widget(layer.createConfigWidget(nullptr));
  QFormLayout* form = topForm(widget.get());
  ASSERT_NE(form, nullptr);
  EXPECT_EQ(form->verticalSpacing(), snugPx());
}

TEST(LayerConfigWidgetRowSpacing, RobotModel) {
  pj::scene3d::RobotModelLayer layer(topic(4), u"robot"_s);
  std::unique_ptr<QWidget> widget(layer.createConfigWidget(nullptr));
  QFormLayout* form = topForm(widget.get());
  ASSERT_NE(form, nullptr);
  EXPECT_EQ(form->verticalSpacing(), snugPx());
}

TEST(LayerConfigWidgetRowSpacing, VoxelGrid) {
  pj::scene3d::VoxelGridLayer layer(topic(5), u"voxels"_s);
  std::unique_ptr<QWidget> widget(layer.createConfigWidget(nullptr));
  QFormLayout* form = topForm(widget.get());
  ASSERT_NE(form, nullptr);
  EXPECT_EQ(form->verticalSpacing(), snugPx());
}

TEST(LayerConfigWidgetRowSpacing, GridMap) {
  pj::scene3d::GridMapLayer layer(topic(8), u"grid"_s);
  std::unique_ptr<QWidget> widget(layer.createConfigWidget(nullptr));
  QFormLayout* form = topForm(widget.get());
  ASSERT_NE(form, nullptr);
  EXPECT_EQ(form->verticalSpacing(), snugPx());
}

TEST(LayerConfigWidgetRowSpacing, PosesInFrame) {
  pj::scene3d::PosesInFrameLayer layer(topic(6), u"poses"_s);
  std::unique_ptr<QWidget> widget(layer.createConfigWidget(nullptr));
  QFormLayout* form = topForm(widget.get());
  ASSERT_NE(form, nullptr);
  EXPECT_EQ(form->verticalSpacing(), snugPx());
}

TEST(LayerConfigWidgetRowSpacing, SceneEntities) {
  pj::scene3d::SceneEntitiesLayer layer(topic(7), u"markers"_s);
  std::unique_ptr<QWidget> widget(layer.createConfigWidget(nullptr));
  QFormLayout* form = topForm(widget.get());
  ASSERT_NE(form, nullptr);
  EXPECT_EQ(form->verticalSpacing(), snugPx());
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
