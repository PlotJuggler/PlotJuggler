// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QObject>
#include <QString>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_widgets/layers/depth_cloud_layer.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"
#include "pj_scene3d_widgets/layers/robot_model_layer.h"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
#include "pj_scene3d_widgets/layers/voxel_grid_layer.h"
#include "pj_scene_common/scene_layer.h"

using namespace Qt::StringLiterals;

namespace {

struct MutationSignals {
  explicit MutationSignals(PJ::ISceneLayer& layer) {
    configuration_connection =
        QObject::connect(&layer, &PJ::ISceneLayer::configurationChanged, [this] { ++configuration; });
    repaint_connection = QObject::connect(&layer, &PJ::ISceneLayer::repaintRequested, [this] { ++repaint; });
    info_connection = QObject::connect(&layer, &PJ::ISceneLayer::infoChanged, [this] { ++info; });
    visibility_connection =
        QObject::connect(&layer, &PJ::ISceneLayer::visibilityChanged, [this](bool) { ++visibility; });
  }

  ~MutationSignals() {
    QObject::disconnect(configuration_connection);
    QObject::disconnect(repaint_connection);
    QObject::disconnect(info_connection);
    QObject::disconnect(visibility_connection);
  }

  void reset() {
    configuration = 0;
    repaint = 0;
    info = 0;
    visibility = 0;
  }

  int configuration = 0;
  int repaint = 0;
  int info = 0;
  int visibility = 0;
  QMetaObject::Connection configuration_connection;
  QMetaObject::Connection repaint_connection;
  QMetaObject::Connection info_connection;
  QMetaObject::Connection visibility_connection;
};

template <typename Layer>
QString savedState(const Layer& layer) {
  QDomDocument doc;
  doc.appendChild(layer.xmlSaveState(doc));
  return doc.toString(-1);
}

template <typename Layer, typename Mutator>
void expectRejectedWithoutMutation(
    Layer& layer, const QDomElement& valid, MutationSignals& observed, const char* description, Mutator&& mutate) {
  SCOPED_TRACE(description);
  ASSERT_TRUE(layer.xmlLoadState(valid));
  const QString before = savedState(layer);
  observed.reset();

  QDomDocument invalid_doc;
  QDomElement invalid = invalid_doc.importNode(valid, true).toElement();
  invalid_doc.appendChild(invalid);
  std::forward<Mutator>(mutate)(invalid_doc, invalid);

  EXPECT_FALSE(layer.xmlLoadState(invalid));
  EXPECT_EQ(savedState(layer), before);
  EXPECT_EQ(observed.configuration, 0);
  EXPECT_EQ(observed.repaint, 0);
  EXPECT_EQ(observed.info, 0);
  EXPECT_EQ(observed.visibility, 0);
}

template <typename Layer, typename Mutator>
void expectAcceptedWithUnknownAttribute(
    Layer& layer, const QDomElement& valid, MutationSignals& observed, const char* description, Mutator&& mutate) {
  SCOPED_TRACE(description);
  ASSERT_TRUE(layer.xmlLoadState(valid));
  const QString before = savedState(layer);
  observed.reset();

  QDomDocument extended_doc;
  QDomElement extended = extended_doc.importNode(valid, true).toElement();
  extended_doc.appendChild(extended);
  std::forward<Mutator>(mutate)(extended_doc, extended);

  EXPECT_TRUE(layer.xmlLoadState(extended));
  EXPECT_EQ(savedState(layer), before);
  // Refresh signals (info/visibility/repaint) fire on EVERY successful restore
  // by design — a freshly restored layer must seed the dock UI. Only the
  // change-guarded configuration signal proves the unknown attribute mutated
  // nothing.
  EXPECT_EQ(observed.configuration, 0);
}

template <typename Layer>
void expectCommonLeafFailures(Layer& layer, const QDomElement& valid, MutationSignals& observed) {
  expectAcceptedWithUnknownAttribute(
      layer, valid, observed, "unknown attribute",
      [](QDomDocument&, QDomElement& element) { element.setAttribute(u"misspelled_setting"_s, u"1"_s); });
  expectRejectedWithoutMutation(layer, valid, observed, "unknown child", [](QDomDocument& doc, QDomElement& element) {
    element.appendChild(doc.createElement(u"unknown"_s));
  });
  expectRejectedWithoutMutation(
      layer, valid, observed, "duplicate payload child",
      [](QDomDocument& doc, QDomElement& element) { element.appendChild(doc.createElement(element.tagName())); });
}

TEST(Scene3DLayerXmlValidation, PointCloudRejectsMalformedPayloadTransactionally) {
  pj::scene3d::PointCloudLayer layer(PJ::ObjectTopicId{1}, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  MutationSignals observed(layer);
  QDomDocument doc;
  QDomElement valid = doc.createElement(u"pointcloud"_s);
  valid.setAttribute(u"shape"_s, u"cube"_s);
  valid.setAttribute(u"size_meters"_s, u"0.5"_s);
  valid.setAttribute(u"size_pixels"_s, u"4"_s);
  valid.setAttribute(u"color_type"_s, u"solid"_s);
  valid.setAttribute(u"color_choice_explicit"_s, u"true"_s);
  valid.setAttribute(u"color_field"_s, u"intensity"_s);
  valid.setAttribute(u"solid_color"_s, u"#123456"_s);
  valid.setAttribute(u"colormap"_s, u"viridis"_s);
  valid.setAttribute(u"auto_range"_s, u"false"_s);
  valid.setAttribute(u"invert_lut"_s, u"true"_s);
  valid.setAttribute(u"outside_range_opacity"_s, u"0.4"_s);
  valid.setAttribute(u"outside_range_visible"_s, u"false"_s);
  valid.setAttribute(u"range_min"_s, u"-2"_s);
  valid.setAttribute(u"range_max"_s, u"3"_s);
  doc.appendChild(valid);

  expectCommonLeafFailures(layer, valid, observed);
  expectRejectedWithoutMutation(layer, valid, observed, "invalid enum", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"shape"_s, u"triangle"_s);
  });
  expectRejectedWithoutMutation(layer, valid, observed, "invalid boolean", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"invert_lut"_s, u"1"_s);
  });
  expectRejectedWithoutMutation(layer, valid, observed, "non-finite number", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"size_pixels"_s, u"nan"_s);
  });
  expectRejectedWithoutMutation(layer, valid, observed, "out-of-range size", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"size_meters"_s, u"0"_s);
  });
  expectRejectedWithoutMutation(
      layer, valid, observed, "inverted manual range", [](QDomDocument&, QDomElement& element) {
        element.setAttribute(u"range_min"_s, u"4"_s);
        element.setAttribute(u"range_max"_s, u"3"_s);
      });
  expectRejectedWithoutMutation(layer, valid, observed, "invalid color", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"solid_color"_s, u"not-a-color"_s);
  });
}

TEST(Scene3DLayerXmlValidation, DepthCloudRejectsMalformedPayloadTransactionally) {
  pj::scene3d::DepthCloudLayer layer(PJ::ObjectTopicId{2}, u"depth"_s, PJ::sdk::BuiltinObjectType::kImage);
  MutationSignals observed(layer);
  QDomDocument doc;
  QDomElement valid = doc.createElement(u"depthcloud"_s);
  valid.setAttribute(u"colormap"_s, u"plasma"_s);
  valid.setAttribute(u"point_size_px"_s, u"5"_s);
  valid.setAttribute(u"min_depth"_s, u"1"_s);
  valid.setAttribute(u"max_depth"_s, u"8"_s);
  doc.appendChild(valid);

  expectCommonLeafFailures(layer, valid, observed);
  expectRejectedWithoutMutation(layer, valid, observed, "invalid enum", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"colormap"_s, u"rainbow"_s);
  });
  expectRejectedWithoutMutation(layer, valid, observed, "non-finite number", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"min_depth"_s, u"inf"_s);
  });
  expectRejectedWithoutMutation(
      layer, valid, observed, "out-of-range point size",
      [](QDomDocument&, QDomElement& element) { element.setAttribute(u"point_size_px"_s, u"0.5"_s); });
  expectRejectedWithoutMutation(
      layer, valid, observed, "inverted depth range", [](QDomDocument&, QDomElement& element) {
        element.setAttribute(u"min_depth"_s, u"9"_s);
        element.setAttribute(u"max_depth"_s, u"8"_s);
      });
}

TEST(Scene3DLayerXmlValidation, OccupancyGridRejectsMalformedPayloadTransactionally) {
  pj::scene3d::OccupancyGridLayer layer(PJ::ObjectTopicId{3}, u"map"_s);
  MutationSignals observed(layer);
  QDomDocument doc;
  QDomElement valid = doc.createElement(u"occupancy_grid"_s);
  valid.setAttribute(u"color_scheme"_s, u"costmap"_s);
  valid.setAttribute(u"opacity"_s, u"0.4"_s);
  doc.appendChild(valid);

  expectCommonLeafFailures(layer, valid, observed);
  expectRejectedWithoutMutation(layer, valid, observed, "invalid enum", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"color_scheme"_s, u"heatmap"_s);
  });
  expectRejectedWithoutMutation(layer, valid, observed, "non-finite number", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"opacity"_s, u"nan"_s);
  });
  expectRejectedWithoutMutation(
      layer, valid, observed, "out-of-range opacity",
      [](QDomDocument&, QDomElement& element) { element.setAttribute(u"opacity"_s, u"1.1"_s); });
}

TEST(Scene3DLayerXmlValidation, SceneEntitiesRejectsMalformedPayloadTransactionally) {
  pj::scene3d::SceneEntitiesLayer layer(PJ::ObjectTopicId{4}, u"markers"_s);
  MutationSignals observed(layer);
  QDomDocument doc;
  QDomElement valid = doc.createElement(u"markers"_s);
  valid.setAttribute(u"opacity"_s, u"0.3"_s);
  valid.setAttribute(u"color_override"_s, u"true"_s);
  valid.setAttribute(u"override_color"_s, u"#00ff00"_s);
  valid.setAttribute(u"wireframe"_s, u"true"_s);
  doc.appendChild(valid);

  expectCommonLeafFailures(layer, valid, observed);
  expectRejectedWithoutMutation(layer, valid, observed, "invalid boolean", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"wireframe"_s, u"yes"_s);
  });
  expectRejectedWithoutMutation(layer, valid, observed, "non-finite number", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"opacity"_s, u"nan"_s);
  });
  expectRejectedWithoutMutation(
      layer, valid, observed, "out-of-range opacity",
      [](QDomDocument&, QDomElement& element) { element.setAttribute(u"opacity"_s, u"-0.1"_s); });
  expectRejectedWithoutMutation(layer, valid, observed, "invalid color", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"override_color"_s, u"not-a-color"_s);
  });
}

TEST(Scene3DLayerXmlValidation, PosesInFrameRejectsMalformedPayloadTransactionally) {
  pj::scene3d::PosesInFrameLayer layer(PJ::ObjectTopicId{5}, u"poses"_s);
  MutationSignals observed(layer);
  QDomDocument doc;
  QDomElement valid = doc.createElement(u"poses_in_frame"_s);
  valid.setAttribute(u"gizmo_size"_s, u"0.5"_s);
  valid.setAttribute(u"gizmo_opacity"_s, u"0.4"_s);
  valid.setAttribute(u"x_arrow_only"_s, u"1"_s);
  valid.setAttribute(u"override_color"_s, u"1"_s);
  valid.setAttribute(u"override_color_value"_s, u"#334455"_s);
  doc.appendChild(valid);

  expectCommonLeafFailures(layer, valid, observed);
  expectRejectedWithoutMutation(layer, valid, observed, "invalid boolean", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"x_arrow_only"_s, u"true"_s);
  });
  expectRejectedWithoutMutation(layer, valid, observed, "non-finite number", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"gizmo_size"_s, u"nan"_s);
  });
  expectRejectedWithoutMutation(
      layer, valid, observed, "out-of-range opacity",
      [](QDomDocument&, QDomElement& element) { element.setAttribute(u"gizmo_opacity"_s, u"2"_s); });
  expectRejectedWithoutMutation(layer, valid, observed, "invalid color", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"override_color_value"_s, u"not-a-color"_s);
  });
  expectRejectedWithoutMutation(
      layer, valid, observed, "two owned trails", [](QDomDocument& payload_doc, QDomElement& element) {
        element.appendChild(payload_doc.createElement(u"trail"_s));
        element.appendChild(payload_doc.createElement(u"trail"_s));
      });

  // The one nested payload this layer owns. The browser build has no trail
  // renderer and instead accepts-and-ignores this element so a desktop-saved
  // layout still restores its poses (WasmPosesInFrameLayer::parseSettings), so
  // the tag name and the single-child rule are a CROSS-PLATFORM contract, not a
  // desktop detail. tests/wasm/scene3d_data.spec.js drives the browser half.
  QDomDocument trail_doc;
  QDomElement with_trail = trail_doc.importNode(valid, true).toElement();
  trail_doc.appendChild(with_trail);
  QDomElement trail = trail_doc.createElement(u"trail"_s);
  trail.setAttribute(u"source_kind"_s, u"pose_topic"_s);
  trail.setAttribute(u"past_color"_s, u"#123456"_s);
  trail.setAttribute(u"thickness"_s, u"3"_s);
  with_trail.appendChild(trail);
  ASSERT_TRUE(layer.xmlLoadState(with_trail));
  EXPECT_TRUE(layer.trailEnabled());
  ASSERT_NE(layer.trail(), nullptr);
  EXPECT_EQ(layer.trail()->pastColor().name(QColor::HexRgb), u"#123456"_s);
  // Round-tripping must reproduce exactly one <trail>, or the browser's
  // single-nested-child rule would start rejecting desktop layouts.
  QDomDocument saved_doc;
  const QDomElement saved = layer.xmlSaveState(saved_doc);
  EXPECT_EQ(saved.elementsByTagName(u"trail"_s).count(), 1);
}

TEST(Scene3DLayerXmlValidation, VoxelGridRejectsUnknownStructureTransactionally) {
  pj::scene3d::VoxelGridLayer layer(PJ::ObjectTopicId{6}, u"voxels"_s);
  MutationSignals observed(layer);
  QDomDocument doc;
  QDomElement valid = doc.createElement(u"voxel_grid"_s);
  valid.setAttribute(u"field"_s, u"cost"_s);
  valid.setAttribute(u"draw_mode"_s, u"2"_s);
  valid.setAttribute(u"threshold"_s, u"0.25"_s);
  valid.setAttribute(u"auto_range"_s, u"0"_s);
  valid.setAttribute(u"range_lo"_s, u"-1"_s);
  valid.setAttribute(u"range_hi"_s, u"2"_s);
  valid.setAttribute(u"colormap"_s, u"1"_s);
  valid.setAttribute(u"opacity"_s, u"0.6"_s);
  doc.appendChild(valid);

  expectCommonLeafFailures(layer, valid, observed);
}

TEST(Scene3DLayerXmlValidation, RobotModelRejectsMalformedPayloadTransactionally) {
  pj::scene3d::RobotModelLayer layer(PJ::ObjectTopicId{7}, u"robot"_s);
  MutationSignals observed(layer);
  int status_changes = 0;
  const QMetaObject::Connection status_connection = QObject::connect(
      &layer, &pj::scene3d::RobotModelLayer::statusTextChanged,
      [&status_changes](const QString&) { ++status_changes; });

  QDomDocument doc;
  QDomElement valid = doc.createElement(u"robot_model"_s);
  valid.setAttribute(u"source_type"_s, u"file"_s);
  valid.setAttribute(u"source_value"_s, u"/tmp/missing-test-robot.urdf"_s);
  valid.setAttribute(u"frame_prefix"_s, u"robot/"_s);
  valid.setAttribute(u"display_mode"_s, u"collision"_s);
  valid.setAttribute(u"visible"_s, u"false"_s);
  valid.setAttribute(u"color"_s, u"#abcdef"_s);
  valid.setAttribute(u"ignore_collada_up_axis"_s, u"true"_s);
  doc.appendChild(valid);

  expectCommonLeafFailures(layer, valid, observed);
  expectRejectedWithoutMutation(layer, valid, observed, "invalid source enum", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"source_type"_s, u"socket"_s);
  });
  expectRejectedWithoutMutation(
      layer, valid, observed, "invalid display enum",
      [](QDomDocument&, QDomElement& element) { element.setAttribute(u"display_mode"_s, u"both"_s); });
  expectRejectedWithoutMutation(layer, valid, observed, "invalid boolean", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"visible"_s, u"1"_s);
  });
  expectRejectedWithoutMutation(layer, valid, observed, "invalid color", [](QDomDocument&, QDomElement& element) {
    element.setAttribute(u"color"_s, u"not-a-color"_s);
  });
  expectRejectedWithoutMutation(
      layer, valid, observed, "identity on non-topic source", [](QDomDocument&, QDomElement& element) {
        element.setAttribute(u"source_topic_name"_s, u"/robot_description"_s);
      });
  expectRejectedWithoutMutation(
      layer, valid, observed, "malformed dataset id", [](QDomDocument&, QDomElement& element) {
        element.setAttribute(u"source_type"_s, u"topic"_s);
        element.setAttribute(u"source_topic_name"_s, u"/robot_description"_s);
        element.setAttribute(u"source_dataset_id"_s, u"not-an-id"_s);
      });
  EXPECT_EQ(status_changes, 0);
  QObject::disconnect(status_connection);
}

}  // namespace
