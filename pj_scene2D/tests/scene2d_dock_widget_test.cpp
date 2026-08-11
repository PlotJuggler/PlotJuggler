// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QDomDocument>
#include <QDomElement>
#include <QMouseEvent>
#include <QPointF>
#include <QSet>
#include <QWheelEvent>
#include <QtGlobal>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_codec.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene2d_widgets/layers/depth_image_layer.h"
#include "pj_scene2d_widgets/layers/image_layer.h"
#include "pj_scene2d_widgets/media_viewer_widget.h"
#include "pj_widgets/ToggleSwitch.h"

using namespace Qt::StringLiterals;

namespace {

PJ::ObjectTopicId registerTopic(
    PJ::SessionManager& session, uint32_t dataset_id, const std::string& topic_name,
    const std::string& metadata_json = {}) {
  // Saved object identity resolves through the session's identity ladder, so the
  // dataset must exist in the engine exactly as it does in production. Registering
  // the same id twice is a harmless no-op (first writer wins).
  (void)session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "dataset_" + std::to_string(dataset_id), .time_domain_id = 0}, dataset_id);
  auto topic = session.objectStore().registerTopic(PJ::ObjectTopicDescriptor{dataset_id, topic_name, metadata_json});
  EXPECT_TRUE(topic.has_value());
  return topic.has_value() ? *topic : PJ::ObjectTopicId{};
}

std::vector<uint32_t> ids(const std::vector<PJ::ObjectTopicId>& topics) {
  std::vector<uint32_t> out;
  out.reserve(topics.size());
  for (const auto topic : topics) {
    out.push_back(topic.id);
  }
  return out;
}

std::vector<uint32_t> layerIds(const std::vector<PJ::SceneLayerInfo>& layers) {
  std::vector<uint32_t> out;
  out.reserve(layers.size());
  for (const auto& layer : layers) {
    out.push_back(layer.topic_id.id);
  }
  return out;
}

// Serialize a canonical sdk::Image — the bytes a no-parser image topic stores.
std::vector<uint8_t> makeImageBytes(
    uint32_t width, uint32_t height, const std::string& encoding, std::vector<uint8_t> pixels) {
  PJ::sdk::Image img;
  img.timestamp_ns = 100;
  img.width = width;
  img.height = height;
  img.encoding = encoding;
  img.data = PJ::Span<const uint8_t>(pixels.data(), pixels.size());
  return PJ::serializeImage(img);
}

std::atomic<int> g_first_annotation_parser_calls{0};
std::atomic<int> g_second_annotation_parser_calls{0};
std::weak_ptr<void> g_annotation_parser_keepalive;
std::atomic<long> g_annotation_parser_keepalive_refs{0};

class TaggedAnnotationParser final : public PJ::MessageParserPluginBase {
 public:
  TaggedAnnotationParser(std::atomic<int>* calls, uint32_t point_count) {
    registerSchemaHandler(
        "PJ.ImageAnnotations",
        PJ::sdk::SchemaHandler{
            .object_type = PJ::sdk::BuiltinObjectType::kImageAnnotations,
            .parse_scalars = nullptr,
            .parse_object = [calls, point_count](
                                PJ::Timestamp ts, PJ::sdk::PayloadView) -> PJ::Expected<PJ::sdk::ObjectRecord> {
              calls->fetch_add(1, std::memory_order_relaxed);
              g_annotation_parser_keepalive_refs.store(
                  g_annotation_parser_keepalive.use_count(), std::memory_order_relaxed);
              PJ::sdk::ImageAnnotations annotations;
              annotations.timestamp = ts;
              annotations.points.resize(point_count);
              return PJ::sdk::ObjectRecord{.ts = ts, .object = annotations};
            },
        });
  }
};

template <typename CreateFn>
std::unique_ptr<PJ::MessageParserHandle> makeBoundAnnotationParser(CreateFn create_fn) {
  static constexpr const char* kManifest =
      R"({"id":"scene2d-rebind-parser","name":"Scene2D Rebind Parser","version":"1.0.0","encoding":["mock"]})";
  auto handle =
      std::make_unique<PJ::MessageParserHandle>(PJ::MessageParserPluginBase::vtableWithCreate(create_fn, kManifest));
  EXPECT_TRUE(handle->valid());
  EXPECT_TRUE(handle->bindSchema("PJ.ImageAnnotations", {}).has_value());
  return handle;
}

// The layer family ("Depth" / "Image") the dock created for a topic.
QString familyFor(const std::vector<PJ::SceneLayerInfo>& layers, PJ::ObjectTopicId topic) {
  for (const auto& layer : layers) {
    if (layer.topic_id.id == topic.id) {
      return layer.family_name;
    }
  }
  return {};
}

class EventTestMediaViewer final : public PJ::MediaViewerWidget {
 public:
  using PJ::MediaViewerWidget::mouseDoubleClickEvent;
  using PJ::MediaViewerWidget::mouseMoveEvent;
  using PJ::MediaViewerWidget::mousePressEvent;
  using PJ::MediaViewerWidget::mouseReleaseEvent;
  using PJ::MediaViewerWidget::wheelEvent;
};

}  // namespace

TEST(MediaViewerWidget, ViewStateSetterValidatesAndLeavesStateUntouchedOnFailure) {
  PJ::MediaViewerWidget viewer;
  const PJ::MediaViewState wanted{.zoom = 3.5f, .pan_x = 0.25f, .pan_y = -0.5f};
  ASSERT_TRUE(viewer.setViewState(wanted));
  EXPECT_EQ(viewer.viewState(), wanted);

  for (const PJ::MediaViewState invalid : {
           PJ::MediaViewState{.zoom = 0.99f, .pan_x = 0.0f, .pan_y = 0.0f},
           PJ::MediaViewState{.zoom = 20.01f, .pan_x = 0.0f, .pan_y = 0.0f},
           PJ::MediaViewState{.zoom = std::numeric_limits<float>::quiet_NaN(), .pan_x = 0.0f, .pan_y = 0.0f},
           PJ::MediaViewState{.zoom = 2.0f, .pan_x = std::numeric_limits<float>::infinity(), .pan_y = 0.0f},
           PJ::MediaViewState{.zoom = 2.0f, .pan_x = 0.0f, .pan_y = -std::numeric_limits<float>::infinity()},
       }) {
    EXPECT_FALSE(viewer.setViewState(invalid));
    EXPECT_EQ(viewer.viewState(), wanted);
  }
}

TEST(MediaViewerWidget, EmitsOneCommittedSignalPerWheelPanAndDoubleClickInteraction) {
  EventTestMediaViewer viewer;
  viewer.resize(200, 200);
  int committed = 0;
  QObject::connect(&viewer, &PJ::MediaViewerWidget::viewInteractionCommitted, [&committed]() { ++committed; });

  QWheelEvent wheel(
      QPointF(100.0, 100.0), QPointF(100.0, 100.0), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
      Qt::NoScrollPhase, false);
  viewer.wheelEvent(&wheel);
  EXPECT_EQ(committed, 1);
  EXPECT_GT(viewer.viewState().zoom, 1.0f);

  QMouseEvent press(
      QEvent::MouseButtonPress, QPointF(100.0, 100.0), QPointF(100.0, 100.0), QPointF(100.0, 100.0), Qt::LeftButton,
      Qt::LeftButton, Qt::NoModifier);
  viewer.mousePressEvent(&press);
  QMouseEvent move(
      QEvent::MouseMove, QPointF(120.0, 110.0), QPointF(120.0, 110.0), QPointF(120.0, 110.0), Qt::NoButton,
      Qt::LeftButton, Qt::NoModifier);
  viewer.mouseMoveEvent(&move);
  EXPECT_EQ(committed, 1) << "pan must commit on release, not on every mouse move";
  QMouseEvent release(
      QEvent::MouseButtonRelease, QPointF(120.0, 110.0), QPointF(120.0, 110.0), QPointF(120.0, 110.0), Qt::LeftButton,
      Qt::NoButton, Qt::NoModifier);
  viewer.mouseReleaseEvent(&release);
  EXPECT_EQ(committed, 2);

  QMouseEvent double_click(
      QEvent::MouseButtonDblClick, QPointF(100.0, 100.0), QPointF(100.0, 100.0), QPointF(100.0, 100.0), Qt::LeftButton,
      Qt::LeftButton, Qt::NoModifier);
  viewer.mouseDoubleClickEvent(&double_click);
  EXPECT_EQ(committed, 3);
  EXPECT_EQ(viewer.viewState(), PJ::MediaViewState{});

  viewer.mouseDoubleClickEvent(&double_click);
  EXPECT_EQ(committed, 3) << "resetting an already-default view is not a workspace mutation";
}

TEST(Scene2DDockWidget, XmlRoundTripPreservesViewStateAndLoadDoesNotNotifyWorkspace) {
  QDomDocument input_doc(u"scene2d"_s);
  QDomElement root = input_doc.createElement(u"scene2d"_s);
  root.setAttribute(u"version"_s, u"1"_s);
  QDomElement view = input_doc.createElement(u"view"_s);
  view.setAttribute(u"zoom"_s, u"4.25"_s);
  view.setAttribute(u"pan_x"_s, u"0.375"_s);
  view.setAttribute(u"pan_y"_s, u"-0.625"_s);
  root.appendChild(view);
  input_doc.appendChild(root);

  PJ::Scene2DDockWidget dock;
  int workspace_changes = 0;
  QObject::connect(&dock, &PJ::SceneDockWidget::workspaceChanged, [&workspace_changes]() { ++workspace_changes; });
  ASSERT_TRUE(dock.xmlLoadState(root));
  EXPECT_EQ(workspace_changes, 0);

  QDomDocument output_doc(u"scene2d"_s);
  const QDomElement saved = dock.xmlSaveState(output_doc);
  const QDomElement saved_view = saved.firstChildElement(u"view"_s);
  ASSERT_FALSE(saved_view.isNull());
  EXPECT_FLOAT_EQ(saved_view.attribute(u"zoom"_s).toFloat(), 4.25f);
  EXPECT_FLOAT_EQ(saved_view.attribute(u"pan_x"_s).toFloat(), 0.375f);
  EXPECT_FLOAT_EQ(saved_view.attribute(u"pan_y"_s).toFloat(), -0.625f);

  QDomElement invalid_root = root.cloneNode(/*deep=*/true).toElement();
  invalid_root.firstChildElement(u"view"_s).setAttribute(u"zoom"_s, u"nan"_s);
  EXPECT_FALSE(dock.xmlLoadState(invalid_root));
  const QDomElement after_rejection = dock.xmlSaveState(output_doc).firstChildElement(u"view"_s);
  EXPECT_FLOAT_EQ(after_rejection.attribute(u"zoom"_s).toFloat(), 4.25f);
  EXPECT_EQ(workspace_changes, 0);
}

TEST(Scene2DDockWidget, RejectsInvalidLayerPayloadBeforeReplacingCurrentState) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"scene2d"_s);
  QDomElement layer = doc.createElement(u"layer"_s);
  layer.setAttribute(u"dataset_id"_s, u"1"_s);
  layer.setAttribute(u"topic_name"_s, u"/camera/image"_s);
  layer.setAttribute(u"object_type"_s, u"kImage"_s);
  layer.setAttribute(u"layer_kind"_s, u"image"_s);
  QDomElement payload = doc.createElement(u"scene2d_layer"_s);
  payload.setAttribute(u"rectify_enabled"_s, u"not-a-bool"_s);
  layer.appendChild(payload);
  root.appendChild(layer);

  PJ::Scene2DDockWidget dock;
  QDomDocument valid_doc;
  QDomElement valid_root = valid_doc.createElement(u"scene2d"_s);
  QDomElement valid_view = valid_doc.createElement(u"view"_s);
  valid_view.setAttribute(u"zoom"_s, u"2"_s);
  valid_view.setAttribute(u"pan_x"_s, u"0.25"_s);
  valid_view.setAttribute(u"pan_y"_s, u"-0.25"_s);
  valid_root.appendChild(valid_view);
  ASSERT_TRUE(dock.xmlLoadState(valid_root));

  EXPECT_FALSE(dock.xmlLoadState(root));
  const QDomElement preserved = dock.xmlSaveState(valid_doc).firstChildElement(u"view"_s);
  EXPECT_FLOAT_EQ(preserved.attribute(u"zoom"_s).toFloat(), 2.0f);
  EXPECT_FLOAT_EQ(preserved.attribute(u"pan_x"_s).toFloat(), 0.25f);
  EXPECT_FLOAT_EQ(preserved.attribute(u"pan_y"_s).toFloat(), -0.25f);
}

TEST(Scene2DDockWidget, RejectsUnknownChildBeforeReplacingCurrentState) {
  QDomDocument valid_doc;
  QDomElement valid = valid_doc.createElement(u"scene2d"_s);
  QDomElement view = valid_doc.createElement(u"view"_s);
  view.setAttribute(u"zoom"_s, u"3"_s);
  valid.appendChild(view);
  valid_doc.appendChild(valid);

  PJ::Scene2DDockWidget dock;
  ASSERT_TRUE(dock.xmlLoadState(valid));

  QDomDocument malformed_doc;
  QDomElement malformed = malformed_doc.createElement(u"scene2d"_s);
  malformed.appendChild(malformed_doc.createElement(u"layre"_s));
  malformed_doc.appendChild(malformed);
  EXPECT_FALSE(dock.xmlLoadState(malformed));

  QDomDocument after_doc;
  const QDomElement after_view = dock.xmlSaveState(after_doc).firstChildElement(u"view"_s);
  EXPECT_FLOAT_EQ(after_view.attribute(u"zoom"_s).toFloat(), 3.0f);
}

TEST(Scene2DDockWidget, PropagatesBaseFailureInsteadOfAcceptingPartialScene) {
  PJ::SessionManager session;
  const auto topic = registerTopic(session, 1, "/camera/image");

  QDomDocument doc;
  QDomElement root = doc.createElement(u"scene2d"_s);
  const auto append_layer = [&doc, &root]() {
    QDomElement layer = doc.createElement(u"layer"_s);
    layer.setAttribute(u"dataset_id"_s, u"1"_s);
    layer.setAttribute(u"dataset_source"_s, u"dataset_1"_s);
    layer.setAttribute(u"topic_name"_s, u"/camera/image"_s);
    layer.setAttribute(u"object_type"_s, u"kImage"_s);
    layer.setAttribute(u"layer_kind"_s, u"image"_s);
    root.appendChild(layer);
  };
  append_layer();
  append_layer();  // the duplicate is permanently rejected after the first succeeds

  PJ::Scene2DDockWidget dock;
  dock.setSessionManager(&session);
  EXPECT_FALSE(dock.xmlLoadState(root));
  EXPECT_EQ(dock.layerFor(topic), nullptr)
      << "a rejected direct paste/load must roll back the partially installed first layer";
}

TEST(Scene2DDockWidget, GenericLayoutBindsMissingAndZeroQualifiersByUniqueCompatibleTopic) {
  static const std::string kImageMetadata = R"({"builtin_object_type":"kImage"})";
  static const std::string kAnnotationsMetadata = R"({"builtin_object_type":"kImageAnnotations"})";

  PJ::SessionManager session;
  const auto front = registerTopic(session, 1, "/camera/front", kImageMetadata);
  const auto rear = registerTopic(session, 1, "/camera/rear", kImageMetadata);
  // Same name, different object type: it is not a compatible candidate for
  // the saved kImage layer and must not make the generic identity ambiguous.
  const auto incompatible = registerTopic(session, 2, "/camera/front", kAnnotationsMetadata);

  PJ::Scene2DDockWidget source;
  source.setSessionManager(&session);
  ASSERT_TRUE(source.addTopic(front, PJ::sdk::BuiltinObjectType::kImage, u"Front"_s));
  ASSERT_TRUE(source.addTopic(rear, PJ::sdk::BuiltinObjectType::kImage, u"Rear"_s));

  QDomDocument doc;
  QDomElement generic = source.xmlSaveState(doc);
  QDomElement first = generic.firstChildElement(u"layer"_s);
  ASSERT_FALSE(first.isNull());
  QDomElement second = first.nextSiblingElement(u"layer"_s);
  ASSERT_FALSE(second.isNull());
  first.removeAttribute(u"dataset_id"_s);
  first.removeAttribute(u"dataset_source"_s);
  second.setAttribute(u"dataset_id"_s, u"0"_s);
  second.removeAttribute(u"dataset_source"_s);

  PJ::Scene2DDockWidget restored;
  restored.setSessionManager(&session);
  ASSERT_TRUE(restored.xmlLoadState(generic));
  EXPECT_NE(restored.layerFor(front), nullptr);
  EXPECT_NE(restored.layerFor(rear), nullptr);
  EXPECT_EQ(restored.layerFor(incompatible), nullptr);
  EXPECT_FALSE(first.hasAttribute(u"dataset_id"_s)) << "normalization must not rewrite the caller's generic document";
  EXPECT_EQ(second.attribute(u"dataset_id"_s), u"0"_s);
}

TEST(Scene2DDockWidget, SourceQualifiedLayoutUsesRemintedDatasetInsteadOfGenericMatching) {
  static const std::string kImageMetadata = R"({"builtin_object_type":"kImage"})";

  PJ::SessionManager source_session;
  auto source_dataset = source_session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "recording.mcap", .time_domain_id = 0}, 41);
  ASSERT_TRUE(source_dataset.has_value());
  auto source_topic = source_session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{*source_dataset, "/camera/image", kImageMetadata});
  ASSERT_TRUE(source_topic.has_value());

  PJ::Scene2DDockWidget source;
  source.setSessionManager(&source_session);
  ASSERT_TRUE(source.addTopic(*source_topic, PJ::sdk::BuiltinObjectType::kImage, u"Camera"_s));
  QDomDocument doc;
  const QDomElement saved = source.xmlSaveState(doc);

  PJ::SessionManager target_session;
  auto collision_dataset = target_session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "other.mcap", .time_domain_id = 0}, 41);
  auto reminted_dataset = target_session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "recording.mcap", .time_domain_id = 0}, 77);
  ASSERT_TRUE(collision_dataset.has_value());
  ASSERT_TRUE(reminted_dataset.has_value());
  auto collision_topic = target_session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{*collision_dataset, "/camera/image", kImageMetadata});
  auto reminted_topic = target_session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{*reminted_dataset, "/camera/image", kImageMetadata});
  ASSERT_TRUE(collision_topic.has_value());
  ASSERT_TRUE(reminted_topic.has_value());

  PJ::Scene2DDockWidget restored;
  restored.setSessionManager(&target_session);
  ASSERT_TRUE(restored.xmlLoadState(saved));
  EXPECT_EQ(restored.layerFor(*collision_topic), nullptr);
  EXPECT_NE(restored.layerFor(*reminted_topic), nullptr);
}

TEST(Scene2DDockWidget, GenericLayoutRejectsAmbiguousCompatibleTopicsWithoutReplacingState) {
  static const std::string kImageMetadata = R"({"builtin_object_type":"kImage"})";

  PJ::SessionManager session;
  const auto first_match = registerTopic(session, 1, "/camera/image", kImageMetadata);
  const auto kept = registerTopic(session, 1, "/camera/kept", kImageMetadata);
  const auto second_match = registerTopic(session, 2, "/camera/image", kImageMetadata);

  PJ::Scene2DDockWidget dock;
  dock.setSessionManager(&session);
  ASSERT_TRUE(dock.addTopic(kept, PJ::sdk::BuiltinObjectType::kImage, u"Kept"_s));

  QDomDocument doc;
  QDomElement generic = doc.createElement(u"scene2d"_s);
  QDomElement layer = doc.createElement(u"layer"_s);
  layer.setAttribute(u"topic_name"_s, u"/camera/image"_s);
  layer.setAttribute(u"object_type"_s, u"kImage"_s);
  layer.setAttribute(u"layer_kind"_s, u"image"_s);
  generic.appendChild(layer);

  EXPECT_FALSE(dock.xmlLoadState(generic));
  EXPECT_NE(dock.layerFor(kept), nullptr);
  EXPECT_EQ(dock.layerFor(first_match), nullptr);
  EXPECT_EQ(dock.layerFor(second_match), nullptr);
}

TEST(Scene2DDockWidget, CommittedViewerInteractionNotifiesWorkspaceExactlyOnce) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"scene2d"_s);

  PJ::Scene2DDockWidget dock;
  ASSERT_TRUE(dock.xmlLoadState(root));  // forces the lazily-created real viewer
  auto* viewer = dock.findChild<PJ::MediaViewerWidget*>(u"scene2dMediaViewer"_s);
  ASSERT_NE(viewer, nullptr);
  viewer->resize(200, 200);

  int workspace_changes = 0;
  QObject::connect(&dock, &PJ::SceneDockWidget::workspaceChanged, [&workspace_changes]() { ++workspace_changes; });
  QWheelEvent wheel(
      QPointF(100.0, 100.0), QPointF(100.0, 100.0), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
      Qt::NoScrollPhase, false);
  QApplication::sendEvent(viewer, &wheel);
  EXPECT_EQ(workspace_changes, 1);
}

TEST(DepthImageLayer, XmlRoundTripPreservesColormapInvertAndRange) {
  PJ::DepthImageLayer layer(PJ::ObjectTopicId{1}, PJ::sdk::BuiltinObjectType::kImage, u"depth"_s);

  QDomDocument doc;
  QDomElement in = doc.createElement(u"scene2d_layer"_s);
  in.setAttribute(u"colormap"_s, u"plasma"_s);
  in.setAttribute(u"invert"_s, u"true"_s);
  in.setAttribute(u"near_m"_s, u"1.5"_s);
  in.setAttribute(u"far_m"_s, u"7"_s);
  ASSERT_TRUE(layer.xmlLoadState(in));

  // Saving after loading must reproduce every depth-display attribute verbatim.
  const QDomElement out = layer.xmlSaveState(doc);
  EXPECT_EQ(out.attribute(u"colormap"_s), u"plasma"_s);
  EXPECT_EQ(out.attribute(u"invert"_s), u"true"_s);
  EXPECT_FLOAT_EQ(out.attribute(u"near_m"_s).toFloat(), 1.5f);
  EXPECT_FLOAT_EQ(out.attribute(u"far_m"_s).toFloat(), 7.0f);
}

TEST(ImageLayer, XmlRoundTripPreservesRectifyEnabled) {
  // The rectify override is the user-facing half of the feature: turn it off, save the
  // layout, reopen -> the override must survive. Pin the save/load symmetry so a
  // future mis-typed key or flipped comparison can't silently drop it.
  PJ::ImageLayer layer(PJ::ObjectTopicId{1}, PJ::sdk::BuiltinObjectType::kImage, u"image"_s);

  QDomDocument doc;
  QDomElement in = doc.createElement(u"scene2d_layer"_s);
  in.setAttribute(u"rectify_enabled"_s, u"false"_s);
  ASSERT_TRUE(layer.xmlLoadState(in));

  const QDomElement out = layer.xmlSaveState(doc);
  EXPECT_EQ(out.attribute(u"rectify_enabled"_s), u"false"_s);
}

TEST(ImageLayer, XmlLoadDefaultsRectifyOnWhenAttributeAbsent) {
  // A layout saved before this toggle existed has no rectify_enabled attribute; it
  // must default to ON, preserving the historical always-on rectification behaviour.
  PJ::ImageLayer layer(PJ::ObjectTopicId{1}, PJ::sdk::BuiltinObjectType::kImage, u"image"_s);

  QDomDocument doc;
  QDomElement in = doc.createElement(u"scene2d_layer"_s);  // no rectify_enabled attribute
  ASSERT_TRUE(layer.xmlLoadState(in));

  const QDomElement out = layer.xmlSaveState(doc);
  EXPECT_EQ(out.attribute(u"rectify_enabled"_s), u"true"_s);
}

TEST(ImageLayer, UserConfigurationChangeSignalsOnceAndXmlLoadStaysSilent) {
  PJ::ImageLayer layer(PJ::ObjectTopicId{1}, PJ::sdk::BuiltinObjectType::kImage, u"image"_s);
  std::unique_ptr<QWidget> config(layer.createConfigWidget(nullptr));
  auto* rectify = config->findChild<PJ::ToggleSwitch*>();
  ASSERT_NE(rectify, nullptr);

  int changes = 0;
  QObject::connect(&layer, &PJ::ISceneLayer::configurationChanged, [&changes]() { ++changes; });
  // ToggleSwitch emits only after its animation settles. Snap the visual state,
  // then deliver that settled user intent synchronously for a deterministic test.
  rectify->setChecked(false, /*animate=*/false);
  rectify->toggled(false);
  EXPECT_EQ(changes, 1);
  rectify->toggled(false);
  EXPECT_EQ(changes, 1) << "setting the same XML-visible value is a no-op";

  QDomDocument doc;
  QDomElement saved = doc.createElement(u"scene2d_layer"_s);
  saved.setAttribute(u"rectify_enabled"_s, u"true"_s);
  ASSERT_TRUE(layer.xmlLoadState(saved));
  EXPECT_EQ(changes, 1) << "state restoration must not look like a user edit";

  EXPECT_FALSE(layer.xmlLoadState(doc.createElement(u"wrong_layer_kind"_s)));
  EXPECT_EQ(changes, 1);
}

TEST(DepthImageLayer, UserConfigurationChangeSignalsOnceAndXmlLoadStaysSilent) {
  PJ::DepthImageLayer layer(PJ::ObjectTopicId{1}, PJ::sdk::BuiltinObjectType::kImage, u"depth"_s);
  std::unique_ptr<QWidget> config(layer.createConfigWidget(nullptr));
  auto* colormap = config->findChild<QComboBox*>();
  ASSERT_NE(colormap, nullptr);

  int changes = 0;
  QObject::connect(&layer, &PJ::ISceneLayer::configurationChanged, [&changes]() { ++changes; });
  const int viridis = colormap->findData(static_cast<int>(PJ::Colormap::kViridis));
  ASSERT_GE(viridis, 0);
  colormap->setCurrentIndex(viridis);
  EXPECT_EQ(changes, 1);
  colormap->setCurrentIndex(viridis);
  EXPECT_EQ(changes, 1) << "setting the same XML-visible value is a no-op";

  QDomDocument doc;
  QDomElement saved = doc.createElement(u"scene2d_layer"_s);
  saved.setAttribute(u"colormap"_s, u"plasma"_s);
  saved.setAttribute(u"invert"_s, u"true"_s);
  saved.setAttribute(u"near_m"_s, u"1.25"_s);
  saved.setAttribute(u"far_m"_s, u"8.5"_s);
  saved.setAttribute(u"opacity"_s, u"0.75"_s);
  ASSERT_TRUE(layer.xmlLoadState(saved));
  EXPECT_EQ(changes, 1) << "state restoration must not look like a user edit";

  EXPECT_FALSE(layer.xmlLoadState(doc.createElement(u"wrong_layer_kind"_s)));
  EXPECT_EQ(changes, 1);
}

TEST(Scene2DDockWidget, RoutesDepthEncodedImageToDepthLayer) {
  PJ::SessionManager session;
  const auto depth = registerTopic(session, 1, "/camera/depth/image");
  const auto color = registerTopic(session, 1, "/camera/color/image");
  // One canonical sdk::Image sample each (no parser -> the dock resolves the first
  // sample via the canonical codec to read its encoding).
  ASSERT_TRUE(session.objectStore()
                  .pushOwned(depth, 100, makeImageBytes(2, 2, "16UC1", std::vector<uint8_t>(2 * 2 * 2, 0x10)))
                  .has_value());
  ASSERT_TRUE(session.objectStore()
                  .pushOwned(color, 100, makeImageBytes(2, 2, "rgb8", std::vector<uint8_t>(2 * 2 * 3, 0x20)))
                  .has_value());

  PJ::Scene2DDockWidget dock;
  dock.setSessionManager(&session);
  ASSERT_TRUE(dock.addTopic(depth, PJ::sdk::BuiltinObjectType::kImage, u"depth"_s));
  ASSERT_TRUE(dock.addTopic(color, PJ::sdk::BuiltinObjectType::kImage, u"color"_s));

  // A depth-encoded kImage routes to the colormap DepthImageLayer; a color kImage
  // (same type) stays on the plain ImageLayer.
  EXPECT_EQ(familyFor(dock.layers(), depth), u"Depth"_s);
  EXPECT_EQ(familyFor(dock.layers(), color), u"Image"_s);
}

TEST(Scene2DDockWidget, CompositeTracksVisibilityAndOrder) {
  PJ::SessionManager session;
  const auto image = registerTopic(session, 1, "/camera/image");
  const auto depth = registerTopic(session, 1, "/camera/depth");

  PJ::Scene2DDockWidget dock;
  dock.setSessionManager(&session);

  ASSERT_TRUE(dock.addTopic(image, PJ::sdk::BuiltinObjectType::kImage, u"image"_s));
  ASSERT_TRUE(dock.addTopic(depth, PJ::sdk::BuiltinObjectType::kDepthImage, u"depth"_s));

  EXPECT_EQ(dock.compositeLayerCountForTesting(), 2U);
  EXPECT_EQ(ids(dock.compositeTopicOrderForTesting()), (std::vector<uint32_t>{image.id, depth.id}));

  dock.setLayerVisible(depth, false);
  EXPECT_EQ(dock.compositeLayerCountForTesting(), 1U);
  EXPECT_EQ(ids(dock.compositeTopicOrderForTesting()), (std::vector<uint32_t>{image.id}));

  dock.setLayerVisible(depth, true);
  dock.reorderLayers({depth, image});
  EXPECT_EQ(dock.compositeLayerCountForTesting(), 2U);
  EXPECT_EQ(ids(dock.compositeTopicOrderForTesting()), (std::vector<uint32_t>{depth.id, image.id}));
  EXPECT_EQ(layerIds(dock.layers()), (std::vector<uint32_t>{depth.id, image.id}));
}

TEST(Scene2DDockWidget, DatasetReloadRebindsAnnotationParserAtAnUnchangedTrackerTime) {
  g_first_annotation_parser_calls.store(0, std::memory_order_relaxed);
  g_second_annotation_parser_calls.store(0, std::memory_order_relaxed);
  g_annotation_parser_keepalive.reset();
  g_annotation_parser_keepalive_refs.store(0, std::memory_order_relaxed);

  PJ::SessionManager session;
  const auto topic = registerTopic(session, 1, "/camera/annotations", R"({"builtin_object_type":"kImageAnnotations"})");
  ASSERT_TRUE(session.objectStore().pushOwned(topic, 100, std::vector<uint8_t>{0x01}).has_value());
  session.registerObjectTopicParser(topic, makeBoundAnnotationParser([]() noexcept -> void* {
                                      return new TaggedAnnotationParser(&g_first_annotation_parser_calls, 1);
                                    }));

  PJ::Scene2DDockWidget dock;
  dock.setSessionManager(&session);
  ASSERT_TRUE(dock.addTopic(topic, PJ::sdk::BuiltinObjectType::kImageAnnotations, u"annotations"_s));
  dock.onTrackerTime(100.0 / 1'000'000'000.0);
  ASSERT_GT(g_first_annotation_parser_calls.load(std::memory_order_relaxed), 0);

  // Keep the old parser mapped so a stale-pointer regression is deterministic:
  // it increments the first counter instead of becoming an allocator-dependent
  // use-after-free crash.
  const auto stale_guard = session.parserKeepaliveForObjectTopic(topic);
  ASSERT_NE(stale_guard, nullptr);
  const int first_calls_before_reload = g_first_annotation_parser_calls.load(std::memory_order_relaxed);

  PJ::DataEngine staged_engine;
  auto staged_dataset =
      staged_engine.createDataset(PJ::DatasetDescriptor{.source_name = "replacement", .time_domain_id = 0}, 2);
  ASSERT_TRUE(staged_dataset.has_value());
  PJ::ObjectStore staged_store;
  auto staged_topic = staged_store.registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *staged_dataset,
          .topic_name = "/camera/annotations",
          .metadata_json = R"({"builtin_object_type":"kImageAnnotations"})",
      });
  ASSERT_TRUE(staged_topic.has_value());
  ASSERT_TRUE(staged_store.pushOwned(*staged_topic, 100, std::vector<uint8_t>{0x02}).has_value());

  std::vector<std::pair<PJ::ObjectTopicId, std::unique_ptr<PJ::MessageParserHandle>>> staged_parsers;
  staged_parsers.emplace_back(*staged_topic, makeBoundAnnotationParser([]() noexcept -> void* {
    return new TaggedAnnotationParser(&g_second_annotation_parser_calls, 2);
  }));
  session.replaceDataset(staged_engine, staged_store, *staged_dataset, /*primary_id=*/1, std::move(staged_parsers));
  {
    const auto replacement_binding = session.parserBindingForObjectTopic(topic);
    ASSERT_TRUE(replacement_binding);
    g_annotation_parser_keepalive = replacement_binding.keepalive;
  }

  // The cursor and active sample timestamp are deliberately unchanged. The
  // dataset replacement hook must invalidate both the source's timestamp dedup
  // and the dock's render fingerprint before this production tracker seed.
  dock.onTrackerTime(100.0 / 1'000'000'000.0);

  EXPECT_EQ(g_first_annotation_parser_calls.load(std::memory_order_relaxed), first_calls_before_reload)
      << "the retained layer decoded through the replaced parser";
  EXPECT_GT(g_second_annotation_parser_calls.load(std::memory_order_relaxed), 0)
      << "the retained layer did not resolve the replacement parser";
  EXPECT_GE(g_annotation_parser_keepalive_refs.load(std::memory_order_relaxed), 2)
      << "the replacement parser binding was not held across parseObject";
}

TEST(Scene2DDockWidget, XmlRoundTripRestoresLayerOrderAndVisibility) {
  PJ::SessionManager session;
  const auto image = registerTopic(session, 3, "/camera/image");
  const auto annotations = registerTopic(session, 3, "/camera/annotations");

  PJ::Scene2DDockWidget original;
  original.setSessionManager(&session);
  ASSERT_TRUE(original.addTopic(image, PJ::sdk::BuiltinObjectType::kImage, u"image"_s));
  ASSERT_TRUE(original.addTopic(annotations, PJ::sdk::BuiltinObjectType::kImageAnnotations, u"annotations"_s));

  original.reorderLayers({annotations, image});
  original.setLayerVisible(image, false);

  QDomDocument doc(u"scene2d"_s);
  const QDomElement saved = original.xmlSaveState(doc);
  doc.appendChild(saved);

  PJ::Scene2DDockWidget restored;
  restored.setSessionManager(&session);
  ASSERT_TRUE(restored.xmlLoadState(saved));

  EXPECT_EQ(layerIds(restored.layers()), (std::vector<uint32_t>{annotations.id, image.id}));
  EXPECT_EQ(restored.compositeLayerCountForTesting(), 1U);
  EXPECT_EQ(ids(restored.compositeTopicOrderForTesting()), (std::vector<uint32_t>{annotations.id}));

  const auto restored_layers = restored.layers();
  ASSERT_EQ(restored_layers.size(), 2U);
  EXPECT_TRUE(restored_layers[0].visible);
  EXPECT_FALSE(restored_layers[1].visible);
}

TEST(Scene2DDockWidget, XmlPersistsDepthConcreteKindWhenImageTopicHasNoSampleAtRestore) {
  PJ::SessionManager source_session;
  const auto source_topic = registerTopic(source_session, 12, "/camera/depth");
  ASSERT_TRUE(source_session.objectStore()
                  .pushOwned(source_topic, 100, makeImageBytes(2, 2, "16UC1", std::vector<uint8_t>(2 * 2 * 2, 0x10)))
                  .has_value());

  PJ::Scene2DDockWidget source;
  source.setSessionManager(&source_session);
  ASSERT_TRUE(source.addTopic(source_topic, PJ::sdk::BuiltinObjectType::kImage, u"depth"_s));
  ASSERT_EQ(familyFor(source.layers(), source_topic), u"Depth"_s);

  QDomDocument doc;
  const QDomElement saved = source.xmlSaveState(doc);
  const QDomElement layer_element = saved.firstChildElement(u"layer"_s);
  ASSERT_FALSE(layer_element.isNull());
  EXPECT_EQ(layer_element.attribute(u"layer_kind"_s), u"depth"_s);

  PJ::SessionManager target_session;
  ASSERT_TRUE(target_session.dataEngine()
                  .createDataset(PJ::DatasetDescriptor{.source_name = "dataset_12", .time_domain_id = 0}, 12)
                  .has_value());
  PJ::Scene2DDockWidget restored;
  restored.setSessionManager(&target_session);
  ASSERT_TRUE(restored.xmlLoadState(saved));
  EXPECT_TRUE(restored.layers().empty());  // topic is deferred until the catalog/store catches up

  const auto target_topic_result =
      target_session.objectStore().registerTopic(PJ::ObjectTopicDescriptor{12, "/camera/depth", {}});
  ASSERT_TRUE(target_topic_result.has_value());
  const auto target_topic = *target_topic_result;
  ASSERT_EQ(target_session.objectStore().entryCount(target_topic), 0U);
  EXPECT_EQ(restored.retryPendingRestores(QSet<QString>{u"/camera/depth"_s}), 1);
  EXPECT_EQ(familyFor(restored.layers(), target_topic), u"Depth"_s);
}

TEST(Scene2DDockWidget, RejectedClipboardReplayPreservesPriorPendingQueue) {
  PJ::SessionManager session;
  constexpr uint32_t kDataset = 24;
  ASSERT_TRUE(session.dataEngine()
                  .createDataset(PJ::DatasetDescriptor{.source_name = "dataset_24", .time_domain_id = 0}, kDataset)
                  .has_value());

  QDomDocument pending_doc;
  QDomElement pending_state = pending_doc.createElement(u"scene2d"_s);
  QDomElement pending_layer = pending_doc.createElement(u"layer"_s);
  pending_layer.setAttribute(u"dataset_id"_s, QString::number(kDataset));
  pending_layer.setAttribute(u"dataset_source"_s, u"dataset_24"_s);
  pending_layer.setAttribute(u"topic_name"_s, u"/late"_s);
  pending_layer.setAttribute(u"object_type"_s, u"kImage"_s);
  pending_layer.setAttribute(u"layer_kind"_s, u"image"_s);
  pending_state.appendChild(pending_layer);
  pending_doc.appendChild(pending_state);

  PJ::Scene2DDockWidget dock;
  dock.setSessionManager(&session);
  ASSERT_TRUE(dock.xmlLoadState(pending_state));
  ASSERT_EQ(dock.unresolvedPendingRestores(), QStringList{u"/late"_s});

  const PJ::ObjectTopicId duplicate_topic = registerTopic(session, kDataset, "/duplicate");
  QDomDocument duplicate_doc;
  QDomElement duplicate_state = duplicate_doc.createElement(u"scene2d"_s);
  for (int i = 0; i < 2; ++i) {
    QDomElement layer = duplicate_doc.createElement(u"layer"_s);
    layer.setAttribute(u"dataset_id"_s, QString::number(kDataset));
    layer.setAttribute(u"dataset_source"_s, u"dataset_24"_s);
    layer.setAttribute(u"topic_name"_s, u"/duplicate"_s);
    layer.setAttribute(u"object_type"_s, u"kImage"_s);
    layer.setAttribute(u"layer_kind"_s, u"image"_s);
    duplicate_state.appendChild(layer);
  }
  duplicate_doc.appendChild(duplicate_state);

  EXPECT_FALSE(dock.xmlLoadState(duplicate_state));
  EXPECT_TRUE(dock.layers().empty());
  EXPECT_EQ(dock.unresolvedPendingRestores(), QStringList{u"/late"_s});
  EXPECT_FALSE(dock.workspaceRestoreFailed());
  EXPECT_NE(duplicate_topic.id, 0U);
}

TEST(Scene2DDockWidget, EmptyPlaceholderActiveUntilFirstVisibleLayer) {
  PJ::SessionManager session;
  const auto image = registerTopic(session, 5, "/camera/image");

  PJ::Scene2DDockWidget dock;
  dock.setSessionManager(&session);

  // A fresh, empty dock shows the placeholder rather than a blank GPU surface.
  EXPECT_TRUE(dock.emptyPlaceholderActiveForTesting());

  // The first visible layer fronts the viewer instead.
  ASSERT_TRUE(dock.addTopic(image, PJ::sdk::BuiltinObjectType::kImage, u"image"_s));
  EXPECT_FALSE(dock.emptyPlaceholderActiveForTesting());

  // Hiding the only layer empties the composite, so the placeholder returns.
  dock.setLayerVisible(image, false);
  EXPECT_TRUE(dock.emptyPlaceholderActiveForTesting());

  // Showing it again brings the viewer back.
  dock.setLayerVisible(image, true);
  EXPECT_FALSE(dock.emptyPlaceholderActiveForTesting());

  // Removing the last layer also returns to the placeholder.
  dock.removeTopic(image);
  EXPECT_TRUE(dock.emptyPlaceholderActiveForTesting());
}

TEST(Scene2DDockWidget, RevalidateKeepsNeverPopulatedDockButResetsEvictedOne) {
  PJ::SessionManager session;

  PJ::Scene2DDockWidget dock;
  dock.setSessionManager(&session);

  // A click-created, never-populated dock must survive catalog churn (e.g. a
  // later dataset load), so the shell does NOT reset it to the placeholder.
  EXPECT_TRUE(dock.revalidateObjects());

  // Once it has held a topic and that topic is evicted, it reports empty so the
  // shell can reset it to the neutral placeholder — the original eviction behavior.
  const auto image = registerTopic(session, 9, "/camera/image");
  ASSERT_TRUE(dock.addTopic(image, PJ::sdk::BuiltinObjectType::kImage, u"image"_s));
  EXPECT_TRUE(dock.revalidateObjects());  // live layer remains
  session.objectStore().removeTopic(image);
  EXPECT_FALSE(dock.revalidateObjects());  // had content, now evicted -> reset
}

TEST(Scene2DDockWidget, RevalidateWithoutSessionKeepsNeverPopulatedDock) {
  // No session set (e.g. mid teardown / session swap): a never-populated dock
  // must still be kept, not reported empty and wiped to the placeholder.
  PJ::Scene2DDockWidget dock;
  EXPECT_TRUE(dock.revalidateObjects());
}

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
  ::testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
