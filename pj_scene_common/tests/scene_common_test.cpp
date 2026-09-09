// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QDomDocument>
#include <QString>
#include <QWidget>
#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/time.hpp"  // PJ::Timepoint, PJ::Range, fromRaw/toRaw
#include "pj_datastore/engine.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene_common/layer_factory.h"
#include "pj_scene_common/layer_params.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scene_common/scene_layer.h"
using namespace Qt::StringLiterals;

namespace {

struct FakeLayerConfig {
  std::pair<int64_t, int64_t> range{0, 0};
};

std::unordered_map<uint32_t, FakeLayerConfig> g_fake_layer_configs;
std::vector<uint32_t> g_detached_topics;
// Ordered lifecycle log ("sync:<n>" / "detach:<id>" / "destroy:<id>") for
// order-of-operations assertions (e.g. view re-pointed before layers die).
std::vector<std::string> g_layer_events;

PJ::ObjectTopicId topic(uint32_t id) {
  PJ::ObjectTopicId topic_id;
  topic_id.id = id;
  return topic_id;
}

std::vector<int64_t> infoIds(const std::vector<PJ::SceneLayerInfo>& infos) {
  std::vector<int64_t> ids;
  ids.reserve(infos.size());
  for (const auto& info : infos) {
    ids.push_back(static_cast<int64_t>(info.topic_id.id));
  }
  return ids;
}

class FakeLayer : public PJ::ISceneLayer {
 public:
  FakeLayer(PJ::ObjectTopicId topic_id, PJ::sdk::BuiltinObjectType object_type, const QString& display_name)
      : info_{topic_id, object_type, display_name, u"Fake"_s, true} {
    const auto it = g_fake_layer_configs.find(topic_id.id);
    if (it != g_fake_layer_configs.end()) {
      range_ = it->second.range;
    }
  }

  [[nodiscard]] PJ::SceneLayerInfo info() const override {
    return info_;
  }

  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override {
    return {PJ::fromRaw(range_.first), PJ::fromRaw(range_.second)};
  }

  bool attach(const PJ::SceneLayerContext& ctx) override {
    attached_ = true;
    attached_session_ = ctx.session;
    return true;
  }

  ~FakeLayer() override {
    g_layer_events.push_back("destroy:" + std::to_string(info_.topic_id.id));
  }

  void detach() override {
    detached_ = true;
    g_detached_topics.push_back(info_.topic_id.id);
    g_layer_events.push_back("detach:" + std::to_string(info_.topic_id.id));
  }

  void setTrackerTime(PJ::Timepoint time) override {
    tracker_times_ns_.push_back(PJ::toRaw(time));
  }

  void setVisible(bool visible) override {
    info_.visible = visible;
    emit visibilityChanged(visible);
  }

  // For the repaint-gate test: when set, renderKey() returns this fixed value
  // (simulating "rendered content unchanged across ticks"); when nullopt it falls
  // back to the base default (time-derived, "always changed") so every other test
  // keeps its existing repaint-on-each-tick behavior.
  void setFixedRenderKey(std::optional<uint64_t> key) {
    fixed_render_key_ = key;
  }

  [[nodiscard]] uint64_t renderKey(PJ::Timepoint time) const override {
    return fixed_render_key_.has_value() ? *fixed_render_key_ : PJ::ISceneLayer::renderKey(time);
  }

  QWidget* createConfigWidget(QWidget* parent) override {
    return new QWidget(parent);
  }

  QDomElement xmlSaveState(QDomDocument& doc) const override {
    QDomElement element = doc.createElement(u"fake"_s);
    element.setAttribute(u"payload"_s, payload_);
    return element;
  }

  bool xmlLoadState(const QDomElement& element) override {
    if (element.tagName() != "fake"_L1) {
      return false;
    }
    payload_ = element.attribute(u"payload"_s);
    return true;
  }

  void commitConfigurationChange() {
    emit configurationChanged();
  }

  void clearTrackerTimes() {
    tracker_times_ns_.clear();
  }

  [[nodiscard]] const std::vector<int64_t>& trackerTimesNs() const {
    return tracker_times_ns_;
  }

  [[nodiscard]] bool attached() const {
    return attached_;
  }

  [[nodiscard]] bool detached() const {
    return detached_;
  }

  [[nodiscard]] PJ::SessionManager* attachedSession() const {
    return attached_session_;
  }

  void setPayload(QString payload) {
    payload_ = std::move(payload);
  }

  [[nodiscard]] QString payload() const {
    return payload_;
  }

 private:
  PJ::SceneLayerInfo info_;
  std::pair<int64_t, int64_t> range_{0, 0};
  std::vector<int64_t> tracker_times_ns_;
  QString payload_;
  bool attached_ = false;
  bool detached_ = false;
  PJ::SessionManager* attached_session_ = nullptr;
  std::optional<uint64_t> fixed_render_key_;
};

class FakeSceneDock : public PJ::SceneDockWidget {
 public:
  FakeSceneDock() {
    layerFactory().registerType(
        PJ::sdk::BuiltinObjectType::kPointCloud,
        [](PJ::ObjectTopicId topic_id, PJ::sdk::BuiltinObjectType object_type, const QString& display_name) {
          return std::make_unique<FakeLayer>(topic_id, object_type, display_name);
        });
  }

  [[nodiscard]] const std::vector<int64_t>& lastSyncedIds() const {
    return last_synced_ids_;
  }

  [[nodiscard]] int refreshCount() const {
    return refresh_count_;
  }

  /// Makes the dock consume topics of this object type as scene-wide config
  /// (no layer created), exercising the ConsumedAsConfig path.
  void setConfigConsumeType(PJ::sdk::BuiltinObjectType type) {
    config_consume_type_ = type;
  }

  /// Drives the viewRenderKey() hook for the repaint-gate test (stands in for a 3D
  /// dock's TF-overlay fingerprint, which no layer owns).
  void setViewRenderKey(uint64_t key) {
    view_render_key_ = key;
  }

  void setSessionManager(PJ::SessionManager* session) override {
    PJ::SceneDockWidget::setSessionManager(session);
  }

  [[nodiscard]] std::optional<PJ::DatasetId> resolveSavedDataset(
      PJ::DatasetId saved_id, const QString& saved_source, const QString& topic_name) const {
    return resolveObjectDataset(saved_id, saved_source, QString(), topic_name);
  }

  [[nodiscard]] bool everHadContentForTest() const {
    return everHadContent();
  }

  [[nodiscard]] PendingRestoreSnapshot capturePendingForTest() const {
    return capturePendingRestoreState();
  }

  void restorePendingForTest(const PendingRestoreSnapshot& snapshot) {
    restorePendingRestoreState(snapshot);
  }

 protected:
  QWidget* createSceneView() override {
    return new QWidget();
  }

  bool handleSceneConfigTopic(
      PJ::ObjectTopicId /*topic_id*/, PJ::sdk::BuiltinObjectType object_type, const QString& /*title*/) override {
    return object_type == config_consume_type_;
  }

  std::unique_ptr<PJ::SceneLayerContext> makeContext() override {
    auto context = std::make_unique<PJ::SceneLayerContext>();
    context->session = sessionManager();
    return context;
  }

  [[nodiscard]] bool acceptsObjectType(PJ::sdk::BuiltinObjectType object_type) const override {
    return object_type == PJ::sdk::BuiltinObjectType::kPointCloud;
  }

  void syncViewLayers(const std::vector<PJ::ISceneLayer*>& ordered_layers) override {
    g_layer_events.push_back("sync:" + std::to_string(ordered_layers.size()));
    last_synced_ids_.clear();
    last_synced_ids_.reserve(ordered_layers.size());
    for (const PJ::ISceneLayer* layer : ordered_layers) {
      last_synced_ids_.push_back(static_cast<int64_t>(layer->info().topic_id.id));
    }
  }

  void refreshView() override {
    ++refresh_count_;
  }

  [[nodiscard]] uint64_t viewRenderKey(PJ::Timepoint /*time*/) const override {
    return view_render_key_;
  }

 private:
  std::vector<int64_t> last_synced_ids_;
  int refresh_count_ = 0;
  PJ::sdk::BuiltinObjectType config_consume_type_ = PJ::sdk::BuiltinObjectType::kNone;
  uint64_t view_render_key_ = 0;
};

}  // namespace

TEST(LayerFactoryTest, RegisterSupportsCreateAndUnsupportedReturnsNull) {
  PJ::LayerFactory factory;
  EXPECT_FALSE(factory.supports(PJ::sdk::BuiltinObjectType::kPointCloud));
  EXPECT_EQ(factory.create(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud"_s), nullptr);

  factory.registerType(
      PJ::sdk::BuiltinObjectType::kPointCloud,
      [](PJ::ObjectTopicId topic_id, PJ::sdk::BuiltinObjectType object_type, const QString& display_name) {
        return std::make_unique<FakeLayer>(topic_id, object_type, display_name);
      });

  EXPECT_TRUE(factory.supports(PJ::sdk::BuiltinObjectType::kPointCloud));
  auto layer = factory.create(topic(7), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud"_s);
  ASSERT_NE(layer, nullptr);
  EXPECT_EQ(layer->info().topic_id, topic(7));
  EXPECT_EQ(layer->info().display_name, u"cloud"_s);
  EXPECT_EQ(factory.create(topic(8), PJ::sdk::BuiltinObjectType::kImage, u"image"_s), nullptr);
}

TEST(SceneDockWidgetTest, AddTopicEmitsLayerAddedAndReportsDrawOrder) {
  g_fake_layer_configs.clear();
  FakeSceneDock dock;
  int added_count = 0;
  QObject::connect(&dock, &PJ::SceneDockWidget::layerAdded, &dock, [&](PJ::ObjectTopicId) { ++added_count; });

  EXPECT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_a"_s));
  EXPECT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_b"_s));

  EXPECT_EQ(added_count, 2);
  EXPECT_EQ(infoIds(dock.layers()), (std::vector<int64_t>{1, 2}));
  EXPECT_EQ(dock.lastSyncedIds(), (std::vector<int64_t>{1, 2}));
  auto* layer = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  ASSERT_NE(layer, nullptr);
  EXPECT_TRUE(layer->attached());
  EXPECT_EQ(layer->info().display_name, u"cloud_a"_s);
}

TEST(SceneDockWidgetTest, AddingTheSameTopicTwiceIsRejected) {
  g_fake_layer_configs.clear();
  FakeSceneDock dock;
  int added_count = 0;
  QObject::connect(&dock, &PJ::SceneDockWidget::layerAdded, &dock, [&](PJ::ObjectTopicId) { ++added_count; });

  EXPECT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_a"_s));
  // Re-adding the same topic must be rejected: no duplicate layer, no second
  // layerAdded signal. Drag-drop and XML restore both route through addTopic, so
  // this guards against the same topic appearing twice in one scene dock.
  EXPECT_FALSE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_a"_s));

  EXPECT_EQ(added_count, 1);
  EXPECT_EQ(infoIds(dock.layers()), (std::vector<int64_t>{1}));
}

TEST(SceneDockWidgetTest, RemoveTopicDetachesAndEmitsLayerRemoved) {
  g_fake_layer_configs.clear();
  g_detached_topics.clear();
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_a"_s));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_b"_s));

  int removed_count = 0;
  QObject::connect(&dock, &PJ::SceneDockWidget::layerRemoved, &dock, [&](PJ::ObjectTopicId) { ++removed_count; });
  dock.removeTopic(topic(1));

  EXPECT_EQ(g_detached_topics, (std::vector<uint32_t>{1}));
  EXPECT_EQ(removed_count, 1);
  EXPECT_EQ(dock.layerFor(topic(1)), nullptr);
  EXPECT_EQ(infoIds(dock.layers()), (std::vector<int64_t>{2}));
  EXPECT_EQ(dock.lastSyncedIds(), (std::vector<int64_t>{2}));
}

TEST(SceneDockWidgetTest, ReorderLayersUpdatesPublicOrderAndSyncViewOrder) {
  g_fake_layer_configs.clear();
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_a"_s));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_b"_s));
  ASSERT_TRUE(dock.addTopic(topic(3), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_c"_s));

  dock.reorderLayers({topic(3), topic(1), topic(2)});

  EXPECT_EQ(infoIds(dock.layers()), (std::vector<int64_t>{3, 1, 2}));
  EXPECT_EQ(dock.lastSyncedIds(), (std::vector<int64_t>{3, 1, 2}));
}

TEST(SceneDockWidgetTest, TrackerTimeClampsToLayerUnionAndForwardsToVisibleLayersOnly) {
  g_fake_layer_configs.clear();
  g_fake_layer_configs[1] = FakeLayerConfig{std::pair<int64_t, int64_t>{100, 200}};
  g_fake_layer_configs[2] = FakeLayerConfig{std::pair<int64_t, int64_t>{300, 400}};
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_a"_s));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_b"_s));
  auto* layer_a = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  auto* layer_b = dynamic_cast<FakeLayer*>(dock.layerFor(topic(2)));
  ASSERT_NE(layer_a, nullptr);
  ASSERT_NE(layer_b, nullptr);
  layer_a->clearTrackerTimes();
  layer_b->clearTrackerTimes();

  dock.setLayerVisible(topic(2), false);
  dock.onTrackerTime(50.0 / 1000000000.0);

  EXPECT_EQ(layer_a->trackerTimesNs(), (std::vector<int64_t>{100}));
  EXPECT_TRUE(layer_b->trackerTimesNs().empty());

  layer_a->clearTrackerTimes();
  layer_b->clearTrackerTimes();
  dock.setLayerVisible(topic(1), false);
  dock.setLayerVisible(topic(2), true);
  // Un-hiding topic(2) re-delivers the last tracker time (100ns) clamped to its
  // own range [300,400] -> 300, the M.25 catch-up. The subsequent tick clamps
  // 500ns to the union's upper bound (400) and reaches only the visible layer.
  dock.onTrackerTime(500.0 / 1000000000.0);

  EXPECT_TRUE(layer_a->trackerTimesNs().empty());
  EXPECT_EQ(layer_b->trackerTimesNs(), (std::vector<int64_t>{300, 400}));
}

// M.25: a hidden layer receives no tracker ticks (onTrackerTime skips it), so
// it would otherwise repaint the geometry it held at the moment of hiding. On
// un-hide the dock must re-deliver the current playhead, clamped to that layer's
// own range exactly as registerLayer seeds it, so the layer catches up.
TEST(SceneDockWidgetTest, UnhideRedeliversLastTrackerTimeClampedToLayerRange) {
  g_fake_layer_configs.clear();
  // layer_a spans the whole window; layer_b's range is a strict subset. This lets
  // the un-hide clamp (per-LAYER) differ from onTrackerTime's clamp (per-UNION).
  g_fake_layer_configs[1] = FakeLayerConfig{std::pair<int64_t, int64_t>{100, 500}};
  g_fake_layer_configs[2] = FakeLayerConfig{std::pair<int64_t, int64_t>{300, 400}};
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_a"_s));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_b"_s));
  auto* layer_a = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  auto* layer_b = dynamic_cast<FakeLayer*>(dock.layerFor(topic(2)));
  ASSERT_NE(layer_a, nullptr);
  ASSERT_NE(layer_b, nullptr);

  // Hide topic(2), then scrub past its range while it is hidden: it must NOT
  // receive that tick (it is the stale-geometry condition the redelivery fixes).
  dock.setLayerVisible(topic(2), false);
  layer_a->clearTrackerTimes();
  layer_b->clearTrackerTimes();
  // Scrub above the union max; onTrackerTime clamps to the UNION ([100,500]) ->
  // 500 reaches the still-visible layer_a, and the hidden layer_b gets nothing.
  // (Clamping to a round bound also sidesteps the seconds<->ns double rounding.)
  dock.onTrackerTime(900.0 / 1'000'000'000.0);
  EXPECT_EQ(layer_a->trackerTimesNs(), (std::vector<int64_t>{500}));
  EXPECT_TRUE(layer_b->trackerTimesNs().empty());

  // Un-hiding catches up: the playhead (last_tracker_ == 500) is clamped to
  // layer_b's OWN range [300,400] -> 400 and re-delivered so it leaves hide-time
  // geometry. The 400 (not 500) proves the redelivery uses the per-layer clamp.
  layer_b->clearTrackerTimes();
  dock.setLayerVisible(topic(2), true);
  EXPECT_EQ(layer_b->trackerTimesNs(), (std::vector<int64_t>{400}));

  // Idempotent: re-hiding then un-hiding with no intervening tick re-delivers the
  // same time once (no double-deliver), and a no-op setVisible(true) does nothing.
  layer_b->clearTrackerTimes();
  dock.setLayerVisible(topic(2), true);  // already visible: no redelivery
  EXPECT_TRUE(layer_b->trackerTimesNs().empty());
}

// M.25 edge: a layer hidden before the first tracker tick still catches up on
// un-hide by being seeded with its own first frame (last_tracker_ is nullopt, so
// the redelivery falls back to the layer's range minimum, like registerLayer).
TEST(SceneDockWidgetTest, UnhideBeforeAnyTrackerTickSeedsLayerFirstFrame) {
  g_fake_layer_configs.clear();
  g_fake_layer_configs[1] = FakeLayerConfig{std::pair<int64_t, int64_t>{700, 900}};
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud"_s));
  auto* layer = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  ASSERT_NE(layer, nullptr);

  dock.setLayerVisible(topic(1), false);
  layer->clearTrackerTimes();
  dock.setLayerVisible(topic(1), true);
  EXPECT_EQ(layer->trackerTimesNs(), (std::vector<int64_t>{700}));  // range minimum, no tick yet
}

TEST(SceneDockWidgetTest, XmlSaveLoadRoundTripsLayersOrderVisibilityAndPayload) {
  g_fake_layer_configs.clear();
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  auto topic_a = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/cloud_a",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(topic_a.has_value()) << topic_a.error();
  auto topic_b = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/cloud_b",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(topic_b.has_value()) << topic_b.error();

  FakeSceneDock source;
  source.setSessionManager(&session);
  ASSERT_TRUE(source.addTopic(*topic_a, PJ::sdk::BuiltinObjectType::kPointCloud, u"Cloud A"_s));
  ASSERT_TRUE(source.addTopic(*topic_b, PJ::sdk::BuiltinObjectType::kPointCloud, u"Cloud B"_s));
  auto* source_a = dynamic_cast<FakeLayer*>(source.layerFor(*topic_a));
  auto* source_b = dynamic_cast<FakeLayer*>(source.layerFor(*topic_b));
  ASSERT_NE(source_a, nullptr);
  ASSERT_NE(source_b, nullptr);
  source_a->setPayload(u"alpha"_s);
  source_b->setPayload(u"beta"_s);
  source.setLayerVisible(*topic_a, false);
  source.reorderLayers({*topic_b, *topic_a});

  QDomDocument doc(u"scene_common"_s);
  const QDomElement root = source.xmlSaveState(doc);
  doc.appendChild(root);

  FakeSceneDock restored;
  restored.setSessionManager(&session);
  ASSERT_TRUE(restored.xmlLoadState(doc.documentElement()));

  const auto infos = restored.layers();
  ASSERT_EQ(infos.size(), 2U);
  EXPECT_EQ(
      infoIds(infos), (std::vector<int64_t>{static_cast<int64_t>(topic_b->id), static_cast<int64_t>(topic_a->id)}));
  EXPECT_EQ(infos[0].display_name, u"Cloud B"_s);
  EXPECT_TRUE(infos[0].visible);
  EXPECT_EQ(infos[1].display_name, u"Cloud A"_s);
  EXPECT_FALSE(infos[1].visible);

  auto* restored_a = dynamic_cast<FakeLayer*>(restored.layerFor(*topic_a));
  auto* restored_b = dynamic_cast<FakeLayer*>(restored.layerFor(*topic_b));
  ASSERT_NE(restored_a, nullptr);
  ASSERT_NE(restored_b, nullptr);
  EXPECT_EQ(restored_a->payload(), u"alpha"_s);
  EXPECT_EQ(restored_b->payload(), u"beta"_s);
  EXPECT_EQ(restored.lastSyncedIds(), infoIds(infos));
}

TEST(SceneDockWidgetTest, AmbiguousObjectDatasetIdentityNeverUsesFirstSourceMatch) {
  PJ::SessionManager session;
  const auto dataset_a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  const auto dataset_b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  ASSERT_TRUE(dataset_a.has_value()) << dataset_a.error();
  ASSERT_TRUE(dataset_b.has_value()) << dataset_b.error();
  ASSERT_TRUE(session.objectStore()
                  .registerTopic(
                      PJ::ObjectTopicDescriptor{
                          .dataset_id = *dataset_a,
                          .topic_name = "/shared",
                          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
                      })
                  .has_value());
  ASSERT_TRUE(session.objectStore()
                  .registerTopic(
                      PJ::ObjectTopicDescriptor{
                          .dataset_id = *dataset_b,
                          .topic_name = "/shared",
                          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
                      })
                  .has_value());
  FakeSceneDock dock;
  dock.setSessionManager(&session);

  EXPECT_FALSE(dock.resolveSavedDataset(999999U, u"same.mcap"_s, u"/shared"_s).has_value())
      << "an ambiguous portable identity must stay unresolved instead of binding the first dataset";
}

TEST(SceneDockWidgetTest, DeferredLayerReturnsToItsSavedDrawOrder) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto topic_a = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/cloud_a",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  const auto topic_b = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/cloud_b",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(topic_a.has_value());
  ASSERT_TRUE(topic_b.has_value());
  FakeSceneDock source;
  source.setSessionManager(&session);
  ASSERT_TRUE(source.addTopic(*topic_a, PJ::sdk::BuiltinObjectType::kPointCloud, u"A"_s));
  ASSERT_TRUE(source.addTopic(*topic_b, PJ::sdk::BuiltinObjectType::kPointCloud, u"B"_s));
  QDomDocument document;
  const QDomElement state = source.xmlSaveState(document);

  session.objectStore().removeTopic(*topic_a);
  FakeSceneDock restored;
  restored.setSessionManager(&session);
  ASSERT_TRUE(restored.xmlLoadState(state));
  EXPECT_EQ(infoIds(restored.layers()), (std::vector<int64_t>{static_cast<int64_t>(topic_b->id)}));

  const auto reminted_a = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/cloud_a",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(reminted_a.has_value());
  int workspace_changes = 0;
  QObject::connect(&restored, &PJ::SceneDockWidget::workspaceChanged, [&workspace_changes]() { ++workspace_changes; });
  EXPECT_EQ(restored.retryPendingRestores(QSet<QString>{u"/cloud_a"_s}), 1);
  EXPECT_EQ(workspace_changes, 0);
  EXPECT_EQ(
      infoIds(restored.layers()),
      (std::vector<int64_t>{static_cast<int64_t>(reminted_a->id), static_cast<int64_t>(topic_b->id)}));
}

TEST(SceneDockWidgetTest, RejectsCrossDatasetLayersUntilPerLayerTimeDomainsExist) {
  PJ::SessionManager session;
  const auto dataset_a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  const auto dataset_b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "b.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_TRUE(dataset_b.has_value());
  const auto topic_a = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset_a,
          .topic_name = "/cloud",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  const auto topic_b = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset_b,
          .topic_name = "/cloud",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(topic_a.has_value());
  ASSERT_TRUE(topic_b.has_value());
  FakeSceneDock dock;
  dock.setSessionManager(&session);
  EXPECT_TRUE(dock.addTopic(*topic_a, PJ::sdk::BuiltinObjectType::kPointCloud, u"A"_s));
  EXPECT_FALSE(dock.addTopic(*topic_b, PJ::sdk::BuiltinObjectType::kPointCloud, u"B"_s));
}

TEST(SceneDockWidgetTest, QualifiedRestoreRejectsKnownLiveObjectTypeMismatch) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_TRUE(session.objectStore()
                  .registerTopic(
                      PJ::ObjectTopicDescriptor{
                          .dataset_id = *dataset,
                          .topic_name = "/same_name",
                          .metadata_json = R"({"builtin_object_type":"kImage"})",
                      })
                  .has_value());
  QDomDocument document;
  QDomElement state = document.createElement(u"scene"_s);
  QDomElement layer = document.createElement(u"layer"_s);
  layer.setAttribute(u"dataset_id"_s, QString::number(*dataset));
  layer.setAttribute(u"dataset_source"_s, u"drive.mcap"_s);
  layer.setAttribute(u"topic_name"_s, u"/same_name"_s);
  layer.setAttribute(u"object_type"_s, u"kPointCloud"_s);
  state.appendChild(layer);
  FakeSceneDock restored;
  restored.setSessionManager(&session);
  EXPECT_FALSE(restored.xmlLoadState(state));
  EXPECT_TRUE(restored.workspaceRestoreFailed());
  EXPECT_TRUE(restored.layers().empty());
}

TEST(SceneDockWidgetTest, MalformedLayerPayloadFailsWholeRestore) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  const auto cloud = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/cloud",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(cloud.has_value());
  FakeSceneDock source;
  source.setSessionManager(&session);
  ASSERT_TRUE(source.addTopic(*cloud, PJ::sdk::BuiltinObjectType::kPointCloud, u"Cloud"_s));
  QDomDocument document;
  QDomElement state = source.xmlSaveState(document);
  state.firstChildElement(u"layer"_s).firstChildElement().setTagName(u"broken"_s);
  FakeSceneDock restored;
  restored.setSessionManager(&session);
  EXPECT_FALSE(restored.xmlLoadState(state));
  EXPECT_TRUE(restored.layers().empty());
}

TEST(SceneDockWidgetTest, DeferredMalformedPayloadIsConsumedButExposesStickyFailure) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  QDomDocument document;
  QDomElement state = document.createElement(u"scene"_s);
  QDomElement layer = document.createElement(u"layer"_s);
  layer.setAttribute(u"dataset_id"_s, QString::number(*dataset));
  layer.setAttribute(u"dataset_source"_s, u"drive.mcap"_s);
  layer.setAttribute(u"topic_name"_s, u"/late_cloud"_s);
  layer.setAttribute(u"object_type"_s, u"kPointCloud"_s);
  layer.appendChild(document.createElement(u"broken"_s));
  state.appendChild(layer);
  FakeSceneDock restored;
  restored.setSessionManager(&session);
  ASSERT_TRUE(restored.xmlLoadState(state));
  ASSERT_TRUE(session.objectStore()
                  .registerTopic(
                      PJ::ObjectTopicDescriptor{
                          .dataset_id = *dataset,
                          .topic_name = "/late_cloud",
                          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
                      })
                  .has_value());
  EXPECT_EQ(restored.retryPendingRestores(QSet<QString>{u"/late_cloud"_s}), 0);
  EXPECT_TRUE(restored.workspaceRestoreFailed());
  EXPECT_TRUE(restored.unresolvedPendingRestores().isEmpty());
}

TEST(SceneDockWidgetTest, AdvertisedPendingIntentRoundTripsAndMaterializesLater) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  FakeSceneDock source;
  source.setSessionManager(&session);
  int changes = 0;
  QObject::connect(&source, &PJ::SceneDockWidget::workspaceChanged, [&changes]() { ++changes; });
  ASSERT_TRUE(
      source.deferTopicIntent(*dataset, u"/late_cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud, u"Late cloud"_s));
  EXPECT_EQ(changes, 1);
  EXPECT_TRUE(source.unresolvedBlockingPendingRestores().isEmpty());

  QDomDocument document;
  const QDomElement saved = source.xmlSaveState(document);
  EXPECT_EQ(saved.firstChildElement(u"layer"_s).attribute(u"pending_intent"_s), u"true"_s);
  FakeSceneDock restored;
  restored.setSessionManager(&session);
  ASSERT_TRUE(restored.xmlLoadState(saved));
  EXPECT_EQ(restored.unresolvedPendingRestores(), QStringList{u"/late_cloud"_s});
  const auto late = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/late_cloud",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(late.has_value());
  EXPECT_EQ(restored.retryPendingRestores(QSet<QString>{u"/late_cloud"_s}), 1);
  ASSERT_EQ(restored.layers().size(), 1U);
  EXPECT_EQ(restored.layers().front().topic_id, *late);
}

TEST(SceneDockWidgetTest, DeferredIntentRejectsUnsupportedAndSecondDatasetBeforeRecordingState) {
  PJ::SessionManager session;
  const auto dataset_a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a"});
  const auto dataset_b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "b"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_TRUE(dataset_b.has_value());
  FakeSceneDock dock;
  dock.setSessionManager(&session);
  EXPECT_FALSE(dock.deferTopicIntent(*dataset_a, u"/image"_s, PJ::sdk::BuiltinObjectType::kImage, u"Image"_s));
  EXPECT_TRUE(dock.deferTopicIntent(*dataset_a, u"/cloud_a"_s, PJ::sdk::BuiltinObjectType::kPointCloud, u"A"_s));
  EXPECT_FALSE(dock.deferTopicIntent(*dataset_b, u"/cloud_b"_s, PJ::sdk::BuiltinObjectType::kPointCloud, u"B"_s));
  ASSERT_EQ(dock.pendingRestoreDemands().size(), 1U);
}

TEST(SceneDockWidgetTest, PendingIntentKeepsDockAliveAfterLastLiveLayerIsEvicted) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive"});
  ASSERT_TRUE(dataset.has_value());
  const auto live = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/live",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(live.has_value());
  FakeSceneDock dock;
  dock.setSessionManager(&session);
  ASSERT_TRUE(dock.addTopic(*live, PJ::sdk::BuiltinObjectType::kPointCloud, u"Live"_s));
  ASSERT_TRUE(dock.deferTopicIntent(*dataset, u"/late"_s, PJ::sdk::BuiltinObjectType::kPointCloud, u"Late"_s));
  session.evictObjectTopics({*live});
  EXPECT_TRUE(dock.revalidateObjects());
  EXPECT_TRUE(dock.layers().empty());
  EXPECT_EQ(dock.unresolvedPendingRestores(), QStringList{u"/late"_s});
}

TEST(SceneDockWidgetTest, PendingSnapshotRestoresEverHadContentLatch) {
  FakeSceneDock dock;
  const auto empty_snapshot = dock.capturePendingForTest();
  ASSERT_TRUE(dock.addTopic(topic(77), PJ::sdk::BuiltinObjectType::kPointCloud, u"temporary"_s));
  EXPECT_TRUE(dock.everHadContentForTest());
  dock.restorePendingForTest(empty_snapshot);
  EXPECT_FALSE(dock.everHadContentForTest());
}

TEST(SceneDockWidgetTest, UnknownRootChildIsRejectedBeforeDestructiveClear) {
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(5), PJ::sdk::BuiltinObjectType::kPointCloud, u"keep"_s));
  QDomDocument document;
  QDomElement malformed = document.createElement(u"scene"_s);
  malformed.appendChild(document.createElement(u"layre"_s));
  EXPECT_FALSE(dock.xmlLoadState(malformed));
  ASSERT_EQ(dock.layers().size(), 1U);
}

TEST(SceneDockWidgetTest, RestoreSignalsAreSuppressedButCommittedConfigurationIsUndoable) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  const auto cloud = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/cloud",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(cloud.has_value());
  FakeSceneDock source;
  source.setSessionManager(&session);
  ASSERT_TRUE(source.addTopic(*cloud, PJ::sdk::BuiltinObjectType::kPointCloud, u"Cloud"_s));
  QDomDocument document;
  const QDomElement state = source.xmlSaveState(document);
  FakeSceneDock restored;
  restored.setSessionManager(&session);
  int changes = 0;
  QObject::connect(&restored, &PJ::SceneDockWidget::workspaceChanged, [&changes]() { ++changes; });
  ASSERT_TRUE(restored.xmlLoadState(state));
  EXPECT_EQ(changes, 0);
  auto* layer = dynamic_cast<FakeLayer*>(restored.layerFor(*cloud));
  ASSERT_NE(layer, nullptr);
  layer->commitConfigurationChange();
  EXPECT_EQ(changes, 1);
}

TEST(SceneDockWidgetTest, PersistentLayerMutationsEmitWorkspaceChangedOnce) {
  FakeSceneDock dock;
  int changes = 0;
  QObject::connect(&dock, &PJ::SceneDockWidget::workspaceChanged, [&changes]() { ++changes; });
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"A"_s));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, u"B"_s));
  EXPECT_EQ(changes, 2);
  dock.setLayerVisible(topic(1), false);
  EXPECT_EQ(changes, 3);
  dock.setLayerVisible(topic(1), false);
  EXPECT_EQ(changes, 3);
  dock.reorderLayers({topic(2), topic(1)});
  EXPECT_EQ(changes, 4);
  dock.reorderLayers({topic(2), topic(1)});
  EXPECT_EQ(changes, 4);
  dock.removeTopic(topic(1));
  EXPECT_EQ(changes, 5);
}

TEST(SceneDockWidgetTest, LayerAddedIsEmittedAfterViewIsReconciled) {
  g_fake_layer_configs.clear();
  FakeSceneDock dock;
  std::vector<int64_t> synced_at_emit;
  bool fired = false;
  // Mutate -> reconcile view -> notify: the view must already include the new
  // layer when observers (which may trigger a paint) react to layerAdded.
  QObject::connect(&dock, &PJ::SceneDockWidget::layerAdded, &dock, [&](PJ::ObjectTopicId) {
    synced_at_emit = dock.lastSyncedIds();
    fired = true;
  });

  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_a"_s));

  EXPECT_TRUE(fired);
  EXPECT_EQ(synced_at_emit, (std::vector<int64_t>{1}));
}

TEST(SceneDockWidgetTest, LayerRemovedIsEmittedAfterViewIsReconciled) {
  g_fake_layer_configs.clear();
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_a"_s));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_b"_s));

  std::vector<int64_t> synced_at_emit;
  bool fired = false;
  // The view must already exclude the removed layer when observers react, so a
  // paint-triggering slot never renders a composite bound to the detached layer.
  QObject::connect(&dock, &PJ::SceneDockWidget::layerRemoved, &dock, [&](PJ::ObjectTopicId) {
    synced_at_emit = dock.lastSyncedIds();
    fired = true;
  });

  dock.removeTopic(topic(1));

  EXPECT_TRUE(fired);
  EXPECT_EQ(synced_at_emit, (std::vector<int64_t>{2}));
}

// Catalog-removal cleanup (IObjectViewer): revalidateObjects() drops layers
// whose ObjectStore topic was evicted and reports whether any live layer
// remains, so the shell can reset an emptied dock to its placeholder.
TEST(SceneDockWidgetTest, RevalidateObjectsPrunesEvictedTopicsAndReportsEmpty) {
  g_fake_layer_configs.clear();
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  auto topic_a = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/cloud_a",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(topic_a.has_value()) << topic_a.error();
  auto topic_b = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/cloud_b",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(topic_b.has_value()) << topic_b.error();

  FakeSceneDock dock;
  dock.setSessionManager(&session);
  ASSERT_TRUE(dock.addTopic(*topic_a, PJ::sdk::BuiltinObjectType::kPointCloud, u"a"_s));
  ASSERT_TRUE(dock.addTopic(*topic_b, PJ::sdk::BuiltinObjectType::kPointCloud, u"b"_s));
  std::vector<uint32_t> removed_topics;
  QObject::connect(&dock, &PJ::SceneDockWidget::layerRemoved, [&removed_topics](PJ::ObjectTopicId topic_id) {
    removed_topics.push_back(topic_id.id);
  });

  // Nothing evicted: everything stays, dock reports non-empty.
  EXPECT_TRUE(dock.revalidateObjects());
  EXPECT_EQ(dock.layers().size(), 2U);

  // Evict one topic: its layer is pruned (with the usual removal notification),
  // the other survives, dock still reports non-empty.
  session.objectStore().removeTopic(*topic_a);
  EXPECT_TRUE(dock.revalidateObjects());
  ASSERT_EQ(dock.layers().size(), 1U);
  EXPECT_EQ(dock.layers().front().topic_id.id, topic_b->id);
  EXPECT_EQ(removed_topics, (std::vector<uint32_t>{topic_a->id}));

  // Evict the last topic: dock empties and reports false (shell resets it).
  session.objectStore().removeTopic(*topic_b);
  EXPECT_FALSE(dock.revalidateObjects());
  EXPECT_TRUE(dock.layers().empty());
}

// A latched / one-shot layer (first == last, e.g. a map pinned to the recording
// start) is valid from its stamp ONWARD: it lowers the clamp's minimum but must
// NOT cap the maximum, or its lone early stamp would drag the live playhead
// backwards (the old "TF doesn't render unless another topic is present" bug).
TEST(SceneDockWidgetTest, LatchedLayerDoesNotDragTrackerTimeBack) {
  g_fake_layer_configs.clear();
  g_fake_layer_configs[1] = FakeLayerConfig{std::pair<int64_t, int64_t>{1'000, 1'000}};  // latched
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"map"_s));
  auto* latched = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  ASSERT_NE(latched, nullptr);

  // Live playhead after the latched stamp: must pass through unclamped.
  latched->clearTrackerTimes();
  dock.onTrackerTime(5'000.0 / 1'000'000'000.0);
  EXPECT_EQ(latched->trackerTimesNs(), (std::vector<int64_t>{5'000}));

  // Before the latched stamp: raised to it (the slider minimum snaps onto it).
  latched->clearTrackerTimes();
  dock.onTrackerTime(500.0 / 1'000'000'000.0);
  EXPECT_EQ(latched->trackerTimesNs(), (std::vector<int64_t>{1'000}));

  // A spanning layer bounds the top; the latched layer still does not extend it.
  g_fake_layer_configs[2] = FakeLayerConfig{std::pair<int64_t, int64_t>{2'000, 3'000}};
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud"_s));
  latched->clearTrackerTimes();
  dock.onTrackerTime(10'000.0 / 1'000'000'000.0);
  EXPECT_EQ(latched->trackerTimesNs(), (std::vector<int64_t>{3'000}));
}

// A static layer (inverted/empty timeRange, e.g. a future URDF RobotModelLayer)
// must NOT contribute to the dock's clamp union: clampToLayerRange skips
// `range.max < range.min`, so a co-resident spanning layer alone bounds the
// playhead, and the static layer still receives every tracker tick.
TEST(SceneDockWidgetTest, StaticLayerWithInvertedRangeIsSkippedByClamp) {
  g_fake_layer_configs.clear();
  // Inverted range == "static / no data": max() as min field, min() as max field.
  g_fake_layer_configs[1] = FakeLayerConfig{
      std::pair<int64_t, int64_t>{std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::lowest()}};
  g_fake_layer_configs[2] = FakeLayerConfig{std::pair<int64_t, int64_t>{2'000, 3'000}};  // spanning
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"static"_s));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud"_s));
  auto* static_layer = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  ASSERT_NE(static_layer, nullptr);
  static_layer->clearTrackerTimes();

  // Below the spanning layer's range -> raised to its lower bound (the static
  // layer's inverted range neither lowers nor caps).
  dock.onTrackerTime(1'000.0 / 1'000'000'000.0);
  EXPECT_EQ(static_layer->trackerTimesNs(), (std::vector<int64_t>{2'000}));

  // Inside the spanning layer's range -> passes through; static layer still ticks.
  static_layer->clearTrackerTimes();
  dock.onTrackerTime(2'500.0 / 1'000'000'000.0);
  EXPECT_EQ(static_layer->trackerTimesNs(), (std::vector<int64_t>{2'500}));
}

// Repaint coalescing: when every visible layer's renderKey() is unchanged across
// consecutive tracker ticks (same active sample, same transform), the dock advances
// the layers and repaints ONCE, then skips — so a 60 Hz playhead over <10 Hz data
// does not drive a 60 Hz repaint. A render-key change reopens the gate immediately.
TEST(SceneDockWidgetTest, TrackerRepaintCoalescesWhenRenderKeyUnchanged) {
  g_fake_layer_configs.clear();
  g_fake_layer_configs[1] = FakeLayerConfig{std::pair<int64_t, int64_t>{0, 1'000'000}};  // wide: no clamp distortion
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud"_s));
  auto* layer = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  ASSERT_NE(layer, nullptr);

  // Pin a constant key => "rendered content identical across ticks".
  layer->setFixedRenderKey(uint64_t{42});
  layer->clearTrackerTimes();
  const int base_refresh = dock.refreshCount();

  // Three DISTINCT tracker times, all carrying the SAME render key.
  dock.onTrackerTime(100.0 / 1'000'000'000.0);
  dock.onTrackerTime(200.0 / 1'000'000'000.0);
  dock.onTrackerTime(300.0 / 1'000'000'000.0);

  // Painted once; the next two coalesced away (and the layer advanced exactly once).
  EXPECT_EQ(dock.refreshCount() - base_refresh, 1);
  EXPECT_EQ(layer->trackerTimesNs().size(), 1U);

  // Content changes -> the gate reopens and painting resumes on the next tick.
  layer->setFixedRenderKey(uint64_t{43});
  dock.onTrackerTime(400.0 / 1'000'000'000.0);
  EXPECT_EQ(dock.refreshCount() - base_refresh, 2);
  EXPECT_EQ(layer->trackerTimesNs().size(), 2U);
}

// View-owned content (a 3D dock's TF axis triads / connection lines pose the whole
// frame forest at the tracker time and belong to NO layer) is folded into the gate
// via viewRenderKey(). A change there must repaint even when every visible layer's
// key is unchanged — otherwise a moving TF tree freezes behind a static cloud.
TEST(SceneDockWidgetTest, ViewRenderKeyChangeRepaintsDespiteStableLayers) {
  g_fake_layer_configs.clear();
  g_fake_layer_configs[1] = FakeLayerConfig{std::pair<int64_t, int64_t>{0, 1'000'000}};
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud"_s));
  auto* layer = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  ASSERT_NE(layer, nullptr);

  layer->setFixedRenderKey(uint64_t{7});  // the only layer's content is pinned (static cloud)
  dock.setViewRenderKey(uint64_t{100});   // TF overlay at its initial pose
  layer->clearTrackerTimes();
  const int base_refresh = dock.refreshCount();

  dock.onTrackerTime(100.0 / 1'000'000'000.0);  // first paint
  dock.onTrackerTime(200.0 / 1'000'000'000.0);  // layer + view keys both unchanged -> coalesce
  EXPECT_EQ(dock.refreshCount() - base_refresh, 1);

  dock.setViewRenderKey(uint64_t{101});  // the TF tree moved (no layer changed)
  dock.onTrackerTime(300.0 / 1'000'000'000.0);
  EXPECT_EQ(dock.refreshCount() - base_refresh, 2) << "a view-key change must repaint despite a stable layer";
}

// Clearing all layers (the xmlLoadState restore path) must re-point the
// concrete view off the old layers (a sync with an empty order) BEFORE any of
// them is destroyed: a view holding raw layer pointers (e.g. the 3D
// SceneViewWidget's layer list) otherwise dereferences freed layers when it
// reconciles, and the layers' GL resources die without a release hook.
TEST(SceneDockWidgetTest, ClearLayersRepointsViewBeforeDestroyingLayers) {
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"a"_s));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, u"b"_s));
  g_layer_events.clear();

  QDomDocument doc;
  const QDomElement empty_scene = doc.createElement(u"scene"_s);
  ASSERT_TRUE(dock.xmlLoadState(empty_scene));

  const auto begin = g_layer_events.begin();
  const auto end = g_layer_events.end();
  const auto first_empty_sync = std::find(begin, end, std::string("sync:0"));
  const auto first_destroy =
      std::find_if(begin, end, [](const std::string& event) { return event.rfind("destroy:", 0) == 0; });
  ASSERT_NE(first_empty_sync, end) << "clearLayers() never re-pointed the view (no empty sync)";
  ASSERT_NE(first_destroy, end);
  EXPECT_LT(first_empty_sync - begin, first_destroy - begin)
      << "view was re-pointed only AFTER layers were destroyed (use-after-free window)";
  EXPECT_TRUE(dock.layers().empty());
}

TEST(SceneDockWidgetTest, TrackerTimeIgnoresNonFiniteValues) {
  g_fake_layer_configs.clear();
  g_fake_layer_configs[1] = FakeLayerConfig{std::pair<int64_t, int64_t>{100, 200}};
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud_a"_s));
  auto* layer = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  ASSERT_NE(layer, nullptr);
  layer->clearTrackerTimes();

  // NaN/inf carry no position; the dock must drop them rather than cast a
  // non-finite double to int64 (undefined behavior) and forward garbage.
  dock.onTrackerTime(std::numeric_limits<double>::quiet_NaN());
  dock.onTrackerTime(std::numeric_limits<double>::infinity());

  EXPECT_TRUE(layer->trackerTimesNs().empty());
}

TEST(SceneDockWidgetTest, ConfigTopicIsConsumedWithoutCreatingLayer) {
  g_fake_layer_configs.clear();
  FakeSceneDock dock;
  dock.setConfigConsumeType(PJ::sdk::BuiltinObjectType::kImage);
  int added_count = 0;
  QObject::connect(&dock, &PJ::SceneDockWidget::layerAdded, &dock, [&](PJ::ObjectTopicId) { ++added_count; });

  // A scene-wide config topic is accepted (addTopic == true) but spawns no layer
  // and no layerAdded signal: the two meanings the typed AddOutcome separates.
  EXPECT_TRUE(dock.addTopic(topic(5), PJ::sdk::BuiltinObjectType::kImage, u"config"_s));

  EXPECT_EQ(added_count, 0);
  EXPECT_EQ(dock.layerFor(topic(5)), nullptr);
  EXPECT_TRUE(dock.layers().empty());
}

// serializeLayerParams + applyLayerParams are the copy/paste backbone for the
// 3D config panel: copy serializes one layer's xmlSaveState blob, paste applies
// it to another. FakeLayer's "payload" attribute stands in for real per-layer
// params (shape, size, colormap, …).
TEST(LayerParamsTest, RoundTripCopiesParamsBetweenLayers) {
  FakeLayer source(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"src"_s);
  source.setPayload(u"alpha"_s);

  const QString xml = PJ::serializeLayerParams(source);
  ASSERT_FALSE(xml.isEmpty());
  EXPECT_TRUE(xml.contains("alpha"_L1));

  FakeLayer target(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, u"dst"_s);
  ASSERT_TRUE(target.payload().isEmpty());

  EXPECT_TRUE(PJ::applyLayerParams(target, xml));
  EXPECT_EQ(target.payload(), u"alpha"_s);
}

// Empty / malformed input must be rejected and leave the target untouched, so a
// paste from an empty clipboard or a corrupted blob never silently wipes params.
TEST(LayerParamsTest, ApplyRejectsEmptyAndMalformedXmlLeavingTargetUntouched) {
  FakeLayer target(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, u"dst"_s);
  target.setPayload(u"keep"_s);

  EXPECT_FALSE(PJ::applyLayerParams(target, QString()));
  EXPECT_FALSE(PJ::applyLayerParams(target, u"<broken"_s));
  EXPECT_EQ(target.payload(), u"keep"_s);
}
