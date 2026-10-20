// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// 3D layers on derived (on-demand) topics (docs/derived_objects/ARCHITECTURE.md §9, §17). A layer reads a derived
// topic from the store like any other: the newest entry at or before the dock's time. There is no readiness gate:
// the node's ring keeps the previous result while a newer one computes (D30) and holds an empty tombstone for a
// missing input or an error (D6). What the dock adds: the row warning (why it is empty), the row status (the lag),
// a refresh when a result lands at the dock's time, the inputs' clock range, and the finding rules (D26).
//
// The on-demand engine runs `test::FakeRun` (pj_runtime/tests/on_demand_test_utils.h): its cloud outputs pass the
// first cloud input through, its SceneEntities outputs hold `FakeRun::entities(query time)`.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QMouseEvent>
#include <QObject>
#include <QString>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "mock_parser_support.h"
#include "on_demand_test_utils.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/builtin/scene_entities.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_base/sdk/object_topic_metadata.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/ResultHistory.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TransformService.h"
#include "pj_scene3d_core/tf/object_placement.h"
#include "pj_scene3d_widgets/FixedFrameMemory.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_tf/tf_buffer.h"
using namespace Qt::StringLiterals;

namespace {

using PJ::kFindingTimestamp;
using PJ::Timestamp;
using Status = PJ::NodeScheduler::NodeState::Status;

constexpr Timestamp kSec = 1'000'000'000;

double displaySecondsOf(Timestamp ns) {
  return static_cast<double>(ns) * 1e-9;
}

PJ::ObjectTopicId registerCanonicalTopic(
    PJ::ObjectStore& store, PJ::DatasetId dataset, const std::string& name, PJ::sdk::BuiltinObjectType type) {
  const auto metadata = PJ::sdk::ObjectTopicMetadataBuilder().builtinObjectType(type).build();
  EXPECT_TRUE(metadata.has_value());
  const auto topic = store.registerTopic({dataset, name, *metadata});
  EXPECT_TRUE(topic.has_value());
  return topic.has_value() ? *topic : PJ::ObjectTopicId{};
}

PJ::sdk::SceneEntity sphereEntity(const std::string& id, Timestamp stamp = 0, std::string frame = "map") {
  PJ::sdk::SceneEntity entity;
  entity.timestamp = stamp;
  entity.frame_id = std::move(frame);
  entity.id = id;
  entity.spheres.push_back(PJ::sdk::SpherePrimitive{});
  return entity;
}

void pushEntities(
    PJ::ObjectStore& store, PJ::ObjectTopicId topic, Timestamp t, std::vector<PJ::sdk::SceneEntity> entities,
    bool delete_all = false) {
  PJ::sdk::SceneEntities batch;
  batch.entities = std::move(entities);
  if (delete_all) {
    batch.deletions.push_back({PJ::sdk::SceneEntityDeletion::Type::kAll, t, ""});
  }
  ASSERT_TRUE(store.pushOwned(topic, t, PJ::serializeSceneEntities(batch)).has_value());
}

// Captures the layers the base hands over: the one seam of a headless dock that sees the concrete layers.
class ProbeDock : public PJ::Scene3DDockWidget {
 public:
  std::vector<PJ::ISceneLayer*> synced_layers;
  int refreshes = 0;

 protected:
  void syncViewLayers(const std::vector<PJ::ISceneLayer*>& ordered_layers) override {
    synced_layers = ordered_layers;
    PJ::Scene3DDockWidget::syncViewLayers(ordered_layers);
  }
  void refreshView() override {
    ++refreshes;
    PJ::Scene3DDockWidget::refreshView();
  }
};

/// The fake run every on-demand service built while it lives uses.
struct ScopedFakeRun {
  explicit ScopedFakeRun(PJ::test::FakeRun& fake) {
    PJ::OnDemandService::setRunForTest(fake.function());
  }
  ~ScopedFakeRun() {
    PJ::OnDemandService::setRunForTest({});
  }
  ScopedFakeRun(const ScopedFakeRun&) = delete;
  ScopedFakeRun& operator=(const ScopedFakeRun&) = delete;
};

/// A dataset `log` with a canonical cloud topic `cloud` (entries at 5, 10 and 20 s), a dock on it, and recipes
/// `owner/<id>` reading the cloud with a point-cloud output `cropped` and a SceneEntities output `witness`.
struct Fixture {
  Fixture() {
    auto domain = session.dataEngine().createTimeDomain("log_domain");
    EXPECT_TRUE(domain.has_value());
    ds = session.dataEngine()
             .createDataset(PJ::DatasetDescriptor{.source_name = "log", .time_domain_id = *domain})
             .value();
    cloud = registerCanonicalTopic(store, ds, "cloud", PJ::sdk::BuiltinObjectType::kPointCloud);
    for (const Timestamp t : {5 * kSec, 10 * kSec, 20 * kSec}) {
      pushCloud(t);
    }
    fake.entities = [](Timestamp) { return std::vector<PJ::sdk::SceneEntity>{sphereEntity("witness")}; };
    dock.setSessionManager(&session);
    dock.setTransformService(&tf);
  }
  void pushCloud(Timestamp t) {
    const auto bytes = PJ::serializeBuiltinObject(PJ::sdk::BuiltinObject(PJ::test::makeCloud(4, t, "map"))).value();
    ASSERT_TRUE(store.pushOwned(cloud, t, bytes).has_value());
  }
  /// Installs `owner/<id>`, a finding when `pinned` is set; a non-empty `edit` appended to the script is a new
  /// revision of an installed recipe.
  void install(const std::string& id, std::optional<Timestamp> pinned = std::nullopt, const std::string& edit = {}) {
    PJ::RecipeGraph::Recipe recipe = PJ::test::onDemandRecipe(
        id, {"cloud"},
        {{"cropped", PJ::scripting::OutputType::kObject, PJ::sdk::BuiltinObjectType::kPointCloud},
         {"witness", PJ::scripting::OutputType::kObject, PJ::sdk::BuiltinObjectType::kSceneEntities}});
    recipe.pinned_t = pinned;
    recipe.script += edit;
    const auto installed = session.onDemandService().install(std::move(recipe));
    ASSERT_TRUE(installed.has_value()) << installed.error();
  }
  void request(const std::string& id, Timestamp t) {
    session.onDemandService().requestAt("owner/" + id, t);
  }
  /// Publishes every run submitted, then runs what the publications posted.
  void settle() {
    session.enginePool().waitIdle();
    QCoreApplication::processEvents();
  }
  [[nodiscard]] PJ::ObjectTopicId output(const std::string& id, const std::string& name) const {
    return store.findTopic(ds, "owner/" + id + "/" + name).value_or(PJ::ObjectTopicId{});
  }
  [[nodiscard]] Status status(const std::string& id) {
    return session.onDemandService().state("owner/" + id).value_or(PJ::NodeScheduler::NodeState{}).status;
  }
  template <class Layer>
  Layer* add(PJ::ObjectTopicId topic, PJ::sdk::BuiltinObjectType type) {
    EXPECT_TRUE(dock.addTopic(topic, type, u"derived"_s));
    return dynamic_cast<Layer*>(dock.layerFor(topic));
  }
  pj::scene3d::PointCloudLayer* addCloud(const std::string& id) {
    return add<pj::scene3d::PointCloudLayer>(output(id, "cropped"), PJ::sdk::BuiltinObjectType::kPointCloud);
  }
  pj::scene3d::SceneEntitiesLayer* addWitness(const std::string& id) {
    return add<pj::scene3d::SceneEntitiesLayer>(output(id, "witness"), PJ::sdk::BuiltinObjectType::kSceneEntities);
  }
  void tick(Timestamp t) {
    dock.onTrackerTime(displaySecondsOf(t));
  }

  PJ::test::FakeRun fake;  ///< outlives the session, whose pool joins the runs
  ScopedFakeRun scoped{fake};
  PJ::SessionManager session;
  PJ::TransformService tf{session};
  PJ::ObjectStore& store = session.objectStore();
  PJ::DatasetId ds = 0;
  PJ::ObjectTopicId cloud;
  ProbeDock dock;
};

// ---- plain store topics (no node): the replay semantics the derived ones build on --------------------------------

// SCN-01: a canonical SceneEntities topic with no parser attaches and renders.
TEST(Scene3DDockDerivedTopics, ParserlessSceneEntitiesTopicAttaches) {
  Fixture fx;
  const auto topic = registerCanonicalTopic(fx.store, fx.ds, "entities", PJ::sdk::BuiltinObjectType::kSceneEntities);
  pushEntities(fx.store, topic, 100, {sphereEntity("a", 100), sphereEntity("b", 100)}, true);
  auto* layer = fx.add<pj::scene3d::SceneEntitiesLayer>(topic, PJ::sdk::BuiltinObjectType::kSceneEntities);
  ASSERT_NE(layer, nullptr);
  fx.tick(100);
  EXPECT_EQ(layer->currentEntities().size(), 2u);
}

// SCN-02: a same-stamp republish reopens the dock's per-tick gate and replaces the set.
TEST(Scene3DDockDerivedTopics, SameStampReplacementReplacesSet) {
  Fixture fx;
  const auto topic = registerCanonicalTopic(fx.store, fx.ds, "entities", PJ::sdk::BuiltinObjectType::kSceneEntities);
  pushEntities(fx.store, topic, 100, {sphereEntity("a", 100), sphereEntity("b", 100)}, true);
  auto* layer = fx.add<pj::scene3d::SceneEntitiesLayer>(topic, PJ::sdk::BuiltinObjectType::kSceneEntities);
  ASSERT_NE(layer, nullptr);
  fx.tick(100);
  ASSERT_EQ(layer->currentEntities().size(), 2u);
  pushEntities(fx.store, topic, 100, {sphereEntity("c", 100)}, true);
  fx.tick(100);
  EXPECT_EQ(layer->currentEntities().size(), 1u);
  EXPECT_TRUE(layer->currentEntities().contains("c"));
}

// SCN-03: a backward-inserted entry is what the store shows at or before an earlier cursor.
TEST(Scene3DDockDerivedTopics, BackwardSeekShowsSnapshotAtThatTime) {
  Fixture fx;
  const auto topic = registerCanonicalTopic(fx.store, fx.ds, "entities", PJ::sdk::BuiltinObjectType::kSceneEntities);
  pushEntities(fx.store, topic, 20, {sphereEntity("x", 20)}, true);
  pushEntities(fx.store, topic, 5, {sphereEntity("y", 5)}, true);
  auto* layer = fx.add<pj::scene3d::SceneEntitiesLayer>(topic, PJ::sdk::BuiltinObjectType::kSceneEntities);
  ASSERT_NE(layer, nullptr);
  fx.tick(5);
  EXPECT_TRUE(layer->currentEntities().contains("y"));
  fx.tick(20);
  EXPECT_TRUE(layer->currentEntities().contains("x"));
  fx.tick(12);
  EXPECT_TRUE(layer->currentEntities().contains("y"));
  EXPECT_EQ(layer->currentEntities().size(), 1u);
}

// SCN-09: a finite entity lifetime expires as the cursor moves, with no new sample.
TEST(Scene3DDockDerivedTopics, SceneEntityLifetimeAdvancesWithoutANewSample) {
  Fixture fx;
  const auto topic = registerCanonicalTopic(fx.store, fx.ds, "entities", PJ::sdk::BuiltinObjectType::kSceneEntities);
  auto entity = sphereEntity("expiring", 100);
  entity.lifetime_ns = 5;
  pushEntities(fx.store, topic, 100, {entity});
  auto* layer = fx.add<pj::scene3d::SceneEntitiesLayer>(topic, PJ::sdk::BuiltinObjectType::kSceneEntities);
  ASSERT_NE(layer, nullptr);
  fx.tick(100);
  ASSERT_EQ(layer->currentEntities().size(), 1u);
  fx.tick(106);
  EXPECT_TRUE(layer->currentEntities().empty());
}

// ---- derived topics ------------------------------------------------------------------------------------------

// SCN-04: evaluated with no scene open, then shown: the dock renders both outputs and follows a seek.
TEST(Scene3DDockDerivedTopics, OnDemandOutputsRenderThroughTheDock) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 20 * kSec);
  fx.settle();
  auto* cloud = fx.addCloud("crop");
  auto* witness = fx.addWitness("crop");
  ASSERT_NE(cloud, nullptr);
  ASSERT_NE(witness, nullptr);
  fx.tick(20 * kSec);
  cloud->renderAtForTest(20 * kSec);
  EXPECT_EQ(cloud->lastPushedStampForTest(), std::optional<int64_t>(20 * kSec));
  EXPECT_EQ(cloud->pointCount(), 4u);
  EXPECT_EQ(witness->currentEntities().size(), 1u);

  fx.request("crop", 5 * kSec);
  fx.settle();
  fx.tick(5 * kSec);
  cloud->renderAtForTest(5 * kSec);
  EXPECT_EQ(cloud->lastPushedStampForTest(), std::optional<int64_t>(5 * kSec));
  EXPECT_TRUE(cloud->statusWarning().isEmpty());
}

// SCN-05 (inverted by D30), GAP-05, GAP-37: a 90 ms function behind the cursor keeps showing its newest earlier
// result, never a blank layer, and the row says how far behind it is; no warning (computing is not a fault).
TEST(Scene3DDockDerivedTopics, PendingDerivedTopicKeepsThePreviousResultAndShowsTheLag) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 5 * kSec);
  fx.settle();
  auto* cloud = fx.addCloud("crop");
  ASSERT_NE(cloud, nullptr);
  fx.fake.delay = std::chrono::milliseconds(90);
  fx.request("crop", 20 * kSec);
  ASSERT_EQ(fx.status("crop"), Status::kPending);
  fx.tick(20 * kSec);
  cloud->renderAtForTest(20 * kSec);
  EXPECT_EQ(cloud->lastPushedStampForTest(), std::optional<int64_t>(5 * kSec));
  EXPECT_EQ(cloud->pointCount(), 4u);
  EXPECT_TRUE(cloud->statusWarning().isEmpty());
  EXPECT_EQ(fx.dock.layerStatus(fx.output("crop", "cropped")), u"behind 15 s"_s);
  EXPECT_EQ(fx.dock.derivedStatus(), u"computing (behind 15 s)"_s);
  fx.settle();
}

// D30: once the cursor stops, the display converges to the current request's result and the lag goes.
TEST(Scene3DDockDerivedTopics, LagDisappearsWhenTheParkedCursorConverges) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 5 * kSec);
  fx.settle();
  auto* cloud = fx.addCloud("crop");
  ASSERT_NE(cloud, nullptr);
  fx.fake.gated = true;
  fx.request("crop", 20 * kSec);
  fx.tick(20 * kSec);
  ASSERT_EQ(fx.dock.layerStatus(fx.output("crop", "cropped")), u"behind 15 s"_s);
  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.status("crop"), Status::kReady);
  EXPECT_TRUE(fx.dock.layerStatus(fx.output("crop", "cropped")).isEmpty());
  EXPECT_TRUE(fx.dock.derivedStatus().isEmpty());
  cloud->renderAtForTest(20 * kSec);
  EXPECT_EQ(cloud->lastPushedStampForTest(), std::optional<int64_t>(20 * kSec));
}

// The lag is the scheduler's (the shown result against the current request's), never the cursor minus a stamp: a
// sparse input's current result at 5 s is not "behind" a cursor at 9 s.
TEST(Scene3DDockDerivedTopics, SparseInputCurrentResultHasNoLagIndicator) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 9 * kSec);
  fx.settle();
  auto* cloud = fx.addCloud("crop");
  ASSERT_NE(cloud, nullptr);
  fx.tick(9 * kSec);
  EXPECT_EQ(fx.status("crop"), Status::kReady);
  EXPECT_TRUE(fx.dock.layerStatus(fx.output("crop", "cropped")).isEmpty());
  EXPECT_TRUE(fx.dock.derivedStatus().isEmpty());
}

// Before any result is known, the row says it computes.
TEST(Scene3DDockDerivedTopics, AFirstRequestShowsComputingWithoutALag) {
  Fixture fx;
  fx.install("crop");
  auto* cloud = fx.addCloud("crop");
  ASSERT_NE(cloud, nullptr);
  fx.fake.gated = true;
  fx.request("crop", 20 * kSec);
  EXPECT_EQ(fx.dock.layerStatus(fx.output("crop", "cropped")), u"computing…"_s);
  EXPECT_EQ(fx.dock.derivedStatus(), u"computing…"_s);
  fx.fake.allow(1);
  fx.settle();
  EXPECT_TRUE(fx.dock.derivedStatus().isEmpty());
}

// SCN-06: a result landing at the dock's time repaints it without a tracker tick.
TEST(Scene3DDockDerivedTopics, PublishedSignalRefreshesTheDockAtItsTrackerTime) {
  Fixture fx;
  fx.install("crop");
  auto* witness = fx.addWitness("crop");
  ASSERT_NE(witness, nullptr);
  fx.fake.gated = true;
  fx.request("crop", 20 * kSec);
  fx.tick(20 * kSec);
  EXPECT_TRUE(witness->currentEntities().empty());
  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(witness->currentEntities().size(), 1u);
}

// SCN-07 (replaced by D30): pending ticks keep the shown entities and decode nothing new.
TEST(Scene3DDockDerivedTopics, PendingDoesNotResetTheReplay) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 5 * kSec);
  fx.settle();
  auto* witness = fx.addWitness("crop");
  ASSERT_NE(witness, nullptr);
  fx.tick(5 * kSec);
  ASSERT_EQ(witness->currentEntities().size(), 1u);
  fx.fake.gated = true;
  fx.request("crop", 20 * kSec);
  const uint64_t decodes = witness->decodeStartsForTest();
  for (const Timestamp t : {20 * kSec, 21 * kSec, 22 * kSec}) {
    fx.tick(t);
    EXPECT_EQ(witness->currentEntities().size(), 1u);
  }
  EXPECT_EQ(witness->decodeStartsForTest(), decodes) << "an unchanged entry is never decoded again";
  fx.fake.allow(1);
  fx.settle();
}

// D27: each result of a derived SceneEntities topic is a complete snapshot: an entity absent from the next one goes.
TEST(Scene3DDockDerivedTopics, DerivedSnapshotDropsAnEntityAbsentFromTheNextResult) {
  Fixture fx;
  fx.fake.entities = [](Timestamp t) {
    std::vector<PJ::sdk::SceneEntity> entities{sphereEntity("a")};
    if (t < 10 * kSec) {
      entities.push_back(sphereEntity("b"));
    }
    return entities;
  };
  fx.install("crop");
  fx.request("crop", 5 * kSec);
  fx.settle();
  fx.request("crop", 10 * kSec);
  fx.settle();
  auto* witness = fx.addWitness("crop");
  ASSERT_NE(witness, nullptr);
  fx.tick(5 * kSec);
  EXPECT_EQ(witness->currentEntities().size(), 2u);
  fx.tick(10 * kSec);
  EXPECT_EQ(witness->currentEntities().size(), 1u);
  EXPECT_TRUE(witness->currentEntities().contains("a"));
  fx.tick(5 * kSec);
  EXPECT_EQ(witness->currentEntities().size(), 2u);
}

// SCN-10 (changed by N7: an error from the script): an error hides the geometry with a warning set by the state
// change itself, and the next good result brings it back and clears the warning.
TEST(Scene3DDockDerivedTopics, ScriptErrorHidesGeometryWithAWarningAndRecovers) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 5 * kSec);
  fx.settle();
  auto* cloud = fx.addCloud("crop");
  auto* witness = fx.addWitness("crop");
  ASSERT_NE(cloud, nullptr);
  ASSERT_NE(witness, nullptr);
  fx.tick(5 * kSec);
  ASSERT_EQ(witness->currentEntities().size(), 1u);

  fx.fake.fail = "boom";
  fx.request("crop", 10 * kSec);
  fx.settle();
  ASSERT_EQ(fx.status("crop"), Status::kError);
  EXPECT_EQ(cloud->statusWarning(), u"error: boom"_s);
  EXPECT_EQ(witness->statusWarning(), u"error: boom"_s);
  EXPECT_TRUE(fx.dock.orphanState(fx.output("crop", "witness")).is_orphan);
  int warning_changes = 0;
  QObject::connect(cloud, &pj::scene3d::Scene3DLayer::statusWarningChanged, [&] { ++warning_changes; });
  fx.tick(10 * kSec);
  cloud->renderAtForTest(10 * kSec);
  EXPECT_EQ(warning_changes, 0) << "rendering never recomputes the derived warning";
  EXPECT_EQ(cloud->pointCount(), 0u);
  EXPECT_TRUE(witness->currentEntities().empty());

  fx.fake.fail.clear();
  fx.request("crop", 20 * kSec);
  fx.settle();
  fx.tick(20 * kSec);
  cloud->renderAtForTest(20 * kSec);
  EXPECT_TRUE(cloud->statusWarning().isEmpty());
  EXPECT_TRUE(witness->statusWarning().isEmpty());
  EXPECT_EQ(cloud->pointCount(), 4u);
  EXPECT_EQ(witness->currentEntities().size(), 1u);
}

// D6: an edit drops the node's results, which the layers keep drawing at the unchanged cursor (an eviction); the
// new revision's failure writes the tombstone that clears them, although the ring shows nothing any more.
TEST(Scene3DDockDerivedTopics, AnEditThatFailsClearsTheKeptGeometry) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 10 * kSec);
  fx.settle();
  auto* cloud = fx.addCloud("crop");
  auto* witness = fx.addWitness("crop");
  ASSERT_NE(cloud, nullptr);
  ASSERT_NE(witness, nullptr);
  fx.tick(10 * kSec);
  cloud->renderAtForTest(10 * kSec);
  ASSERT_EQ(cloud->pointCount(), 4u);
  ASSERT_EQ(witness->currentEntities().size(), 1u);

  fx.fake.fail = "boom";
  fx.install("crop", std::nullopt, "-- edited\n");
  fx.settle();
  ASSERT_EQ(fx.status("crop"), Status::kError);
  cloud->renderAtForTest(10 * kSec);
  EXPECT_EQ(cloud->pointCount(), 0u) << "not the previous revision's cloud";
  EXPECT_TRUE(witness->currentEntities().empty());
  EXPECT_EQ(witness->statusWarning(), u"error: boom"_s);
}

// GAP-07, D6: an input lost under a layer that showed geometry clears it at once and says which input is missing.
TEST(Scene3DDockDerivedTopics, MissingInputClearsALayerThatHadGeometry) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 5 * kSec);
  fx.settle();
  auto* witness = fx.addWitness("crop");
  ASSERT_NE(witness, nullptr);
  fx.tick(5 * kSec);
  ASSERT_EQ(witness->currentEntities().size(), 1u);
  EXPECT_TRUE(witness->statusWarning().isEmpty());

  PJ::test::prune(fx.store, fx.cloud);
  fx.request("crop", 10 * kSec);
  fx.settle();
  ASSERT_EQ(fx.status("crop"), Status::kMissingInput);
  fx.tick(10 * kSec);
  EXPECT_TRUE(witness->currentEntities().empty());
  EXPECT_TRUE(witness->statusWarning().contains(u"missing input 'cloud'"_s)) << witness->statusWarning().toStdString();
}

// D25: one output marked unavailable leaves its sibling's geometry shown, and neither row warns (the node is ready).
TEST(Scene3DDockDerivedTopics, UnavailableSiblingDoesNotHideAvailableOutput) {
  Fixture fx;
  fx.fake.unavailable = {"witness"};
  fx.install("crop");
  fx.request("crop", 20 * kSec);
  fx.settle();
  auto* cloud = fx.addCloud("crop");
  auto* witness = fx.addWitness("crop");
  ASSERT_NE(cloud, nullptr);
  ASSERT_NE(witness, nullptr);
  fx.tick(20 * kSec);
  cloud->renderAtForTest(20 * kSec);
  EXPECT_EQ(cloud->pointCount(), 4u);
  EXPECT_TRUE(witness->currentEntities().empty());
  EXPECT_TRUE(cloud->statusWarning().isEmpty());
  EXPECT_TRUE(witness->statusWarning().isEmpty());
}

// D25: a valid empty result is not a fault: the layer draws nothing and warns about nothing.
TEST(Scene3DDockDerivedTopics, AValidEmptyResultShowsNothingWithoutAWarning) {
  Fixture fx;
  fx.fake.empty_clouds = true;
  fx.install("crop");
  fx.request("crop", 20 * kSec);
  fx.settle();
  auto* cloud = fx.addCloud("crop");
  ASSERT_NE(cloud, nullptr);
  fx.tick(20 * kSec);
  cloud->renderAtForTest(20 * kSec);
  EXPECT_EQ(cloud->pointCount(), 0u);
  EXPECT_TRUE(cloud->statusWarning().isEmpty()) << cloud->statusWarning().toStdString();
}

// GAP-15: a dock whose only layer is derived clamps the cursor to its inputs' range, not to what was computed.
TEST(Scene3DDockDerivedTopics, DerivedOnlyDockDoesNotClampTheCursorToTheComputedSpan) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 5 * kSec);
  fx.settle();
  fx.request("crop", 10 * kSec);
  fx.settle();
  ASSERT_NE(fx.addCloud("crop"), nullptr);
  fx.tick(15 * kSec);
  EXPECT_EQ(fx.dock.lastTrackerNsForTest(), std::optional<int64_t>(15 * kSec));
  fx.tick(30 * kSec);
  EXPECT_EQ(fx.dock.lastTrackerNsForTest(), std::optional<int64_t>(20 * kSec)) << "the inputs end at 20 s";
}

// GAP-14 (layer half): what a layer drew stays drawn when later publications evict the entry it read.
TEST(Scene3DDockDerivedTopics, AHeldCloudSurvivesPublicationAndEviction) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 5 * kSec);
  fx.settle();
  auto* cloud = fx.addCloud("crop");
  ASSERT_NE(cloud, nullptr);
  cloud->renderAtForTest(5 * kSec);
  ASSERT_EQ(cloud->pointCount(), 4u);
  const uint64_t revision = cloud->geometryRevision();
  PJ::test::prune(fx.store, fx.output("crop", "cropped"));
  fx.request("crop", 20 * kSec);
  fx.settle();
  EXPECT_EQ(cloud->pointCount(), 4u);
  EXPECT_EQ(cloud->geometryRevision(), revision);
  EXPECT_EQ(cloud->lastPushedStampForTest(), std::optional<int64_t>(5 * kSec));
}

// A seek before the shown entry's stamp with nothing at the new time clears the cloud (a future result never stays
// on screen); the shown entry evicted under an unchanged cursor keeps it.
TEST(Scene3DDockDerivedTopics, ASeekBeforeTheShownCloudClearsItAndAnEvictionKeepsIt) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 20 * kSec);
  fx.settle();
  auto* cloud = fx.addCloud("crop");
  ASSERT_NE(cloud, nullptr);
  cloud->renderAtForTest(20 * kSec);
  ASSERT_EQ(cloud->pointCount(), 4u);
  PJ::test::prune(fx.store, fx.output("crop", "cropped"));
  cloud->renderAtForTest(20 * kSec);
  EXPECT_EQ(cloud->pointCount(), 4u) << "an eviction at the shown time keeps the cloud";
  cloud->renderAtForTest(5 * kSec);
  EXPECT_EQ(cloud->pointCount(), 0u) << "nothing at 5 s: the 20 s cloud must not stay";
  EXPECT_EQ(cloud->lastPushedStampForTest(), std::nullopt);
}

// A parked SceneEntities result evicted from the ring (another consumer moved the window) stays shown at the
// unchanged cursor; a newly selected entry replaces it, and a seek before its stamp with nothing there clears it.
TEST(Scene3DDockDerivedTopics, AnEvictedEntitySnapshotStaysAtAnUnchangedCursor) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 10 * kSec);
  fx.settle();
  auto* witness = fx.addWitness("crop");
  ASSERT_NE(witness, nullptr);
  fx.tick(10 * kSec);
  ASSERT_EQ(witness->currentEntities().size(), 1u);
  ASSERT_TRUE(pj::scene3d::test::pumpUntil([&] { return !witness->geometry().empty(); }));  // the marker worker

  PJ::test::prune(fx.store, fx.output("crop", "witness"));
  fx.request("crop", 20 * kSec);  // publishes at 20 s: the dock refreshes at its unchanged 10 s
  fx.settle();
  EXPECT_EQ(witness->currentEntities().size(), 1u);
  EXPECT_FALSE(witness->geometry().empty());

  fx.tick(7 * kSec);
  fx.settle();
  EXPECT_TRUE(witness->currentEntities().empty()) << "nothing at 7 s: a real seek miss";
  EXPECT_TRUE(witness->geometry().empty());
  fx.tick(20 * kSec);
  fx.settle();
  EXPECT_EQ(witness->currentEntities().size(), 1u);
}

// An evicted snapshot's entities stay with its markers on a seek back that still lands at or after its stamp: the
// model state is kept, not rebuilt from an empty ring.
TEST(Scene3DDockDerivedTopics, AnEvictedSnapshotKeepsItsEntitiesOnASeekBackWithinIt) {
  Fixture fx;
  fx.install("crop");
  fx.request("crop", 10 * kSec);
  fx.settle();
  auto* witness = fx.addWitness("crop");
  ASSERT_NE(witness, nullptr);
  fx.tick(10 * kSec);
  fx.tick(15 * kSec);
  ASSERT_EQ(witness->currentEntities().size(), 1u);
  ASSERT_TRUE(pj::scene3d::test::pumpUntil([&] { return !witness->geometry().empty(); }));  // the marker worker

  PJ::test::prune(fx.store, fx.output("crop", "witness"));
  fx.tick(12 * kSec);
  EXPECT_EQ(witness->currentEntities().size(), 1u);
  EXPECT_FALSE(witness->geometry().empty());
}

// ---- findings (D26) ------------------------------------------------------------------------------------------

// GAP-01: a finding is visible at any instant, before its stamp included.
TEST(Scene3DDockDerivedTopics, AFindingIsVisibleWhenScrubbingBeforeItsStamp) {
  Fixture fx;
  fx.install("pin", 12 * kSec);
  fx.request("pin", 12 * kSec);
  fx.settle();
  ASSERT_EQ(PJ::test::timestampsOf(fx.store, fx.output("pin", "witness")), std::vector<Timestamp>{kFindingTimestamp});
  auto* witness = fx.addWitness("pin");
  ASSERT_NE(witness, nullptr);
  fx.tick(5 * kSec);
  EXPECT_EQ(witness->currentEntities().size(), 1u);
  EXPECT_EQ(fx.dock.layerStatus(fx.output("pin", "witness")), u"computed at 12 s"_s);
}

// GAP-01: a finding ignores a finite entity lifetime (its entry sits at kFindingTimestamp).
TEST(Scene3DDockDerivedTopics, FindingIgnoresEntityLifetime) {
  Fixture fx;
  fx.fake.entities = [](Timestamp) {
    auto entity = sphereEntity("short");
    entity.lifetime_ns = 1;
    return std::vector<PJ::sdk::SceneEntity>{entity};
  };
  fx.install("pin", 12 * kSec);
  fx.request("pin", 12 * kSec);
  fx.settle();
  auto* witness = fx.addWitness("pin");
  ASSERT_NE(witness, nullptr);
  for (const Timestamp t : {5 * kSec, 12 * kSec, 20 * kSec}) {
    fx.tick(t);
    EXPECT_EQ(witness->currentEntities().size(), 1u) << "at " << t;
  }
}

// GAP-01: the dock's time-range clamp and live-edge fold skip a finding: its kFindingTimestamp neither lowers the
// clamp nor drives the dock to the far past.
TEST(Scene3DDockDerivedTopics, DockClampAndLiveEdgeFoldSkipFindings) {
  Fixture fx;
  fx.install("pin", 12 * kSec);
  fx.request("pin", 12 * kSec);
  fx.settle();
  ASSERT_NE(fx.addWitness("pin"), nullptr);
  fx.tick(12 * kSec);
  emit fx.session.samplesIngested({}, true);
  EXPECT_EQ(fx.dock.lastTrackerNsForTest(), std::optional<int64_t>(12 * kSec)) << "a finding alone has no live edge";

  ASSERT_TRUE(fx.dock.addTopic(fx.cloud, PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud"_s));
  fx.tick(2 * kSec);
  EXPECT_EQ(fx.dock.lastTrackerNsForTest(), std::optional<int64_t>(5 * kSec)) << "the raw layer starts at 5 s";
  emit fx.session.samplesIngested({}, true);
  EXPECT_EQ(fx.dock.lastTrackerNsForTest(), std::optional<int64_t>(20 * kSec));
}

// A finding stays where it was measured in a moving frame: its layer places it at the finding's real stamp, while
// a raw entity layer follows the frame at the cursor.
TEST(Scene3DDockDerivedTopics, FindingInAMovingFrameStaysWhereMeasured) {
  Fixture fx;
  const auto buffer = fx.tf.transformBuffer(fx.ds);
  ASSERT_NE(buffer, nullptr);
  for (const auto& [t, x] :
       std::vector<std::pair<Timestamp, double>>{{5 * kSec, 0.0}, {10 * kSec, 1.0}, {20 * kSec, 5.0}}) {
    const pj::tf::StampedTransform edge{
        PJ::fromRaw(t), "map", "base_link", pj::tf::Transform{{x, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}}};
    ASSERT_TRUE(buffer->setTransform(edge).has_value());
  }
  fx.fake.entities = [](Timestamp) { return std::vector<PJ::sdk::SceneEntity>{sphereEntity("seen", 0, "base_link")}; };
  fx.install("pin", 10 * kSec);  // its result's stamp: the cloud at 10 s
  fx.request("pin", 10 * kSec);
  fx.install("live");
  fx.request("live", 20 * kSec);
  fx.settle();
  auto* finding = fx.addWitness("pin");
  auto* live = fx.addWitness("live");
  ASSERT_NE(finding, nullptr);
  ASSERT_NE(live, nullptr);
  fx.tick(20 * kSec);
  EXPECT_EQ(finding->renderStamp(), std::optional<PJ::Timepoint>(PJ::fromRaw(10 * kSec)));
  EXPECT_EQ(live->renderStamp(), std::nullopt);
  const auto placed = pj::scene3d::placeObject(
      *buffer, "map", "base_link", finding->renderStamp(), PJ::fromRaw(20 * kSec), finding->placementHold());
  ASSERT_TRUE(placed.has_value());
  EXPECT_DOUBLE_EQ(placed->t.x, 1.0);
}

// A finding's "computed at" label is in display seconds: it follows a display-offset change.
TEST(Scene3DDockDerivedTopics, AFindingsLabelFollowsTheDisplayOffset) {
  Fixture fx;
  fx.install("pin", 12 * kSec);
  fx.request("pin", 12 * kSec);
  fx.settle();
  ASSERT_NE(fx.addWitness("pin"), nullptr);
  ASSERT_EQ(fx.dock.layerStatus(fx.output("pin", "witness")), u"computed at 12 s"_s);
  fx.session.setDisplayOffset(fx.ds, PJ::DisplayOffset{std::chrono::seconds(2)});
  EXPECT_EQ(fx.dock.layerStatus(fx.output("pin", "witness")), u"computed at 10 s"_s);
  fx.session.setUseTimeOffset(true);  // the global reference: the earliest raw sample, the cloud at 5 s
  EXPECT_EQ(fx.dock.layerStatus(fx.output("pin", "witness")), u"computed at 5 s"_s);
}

// Each entity frame keeps its own placement hold: placing one frame never overwrites another's, so a multi-frame
// finding stays put after the TF samples at its stamp are gone.
TEST(Scene3DDockDerivedTopics, EachEntityFrameKeepsItsOwnPlacementHold) {
  Fixture fx;
  fx.install("pin", 10 * kSec);
  fx.request("pin", 10 * kSec);
  fx.settle();
  auto* finding = fx.addWitness("pin");
  ASSERT_NE(finding, nullptr);
  pj::scene3d::PlacementHold* hold_a = finding->placementHoldFor("a");
  pj::scene3d::PlacementHold* hold_b = finding->placementHoldFor("b");
  ASSERT_NE(hold_a, hold_b);
  EXPECT_EQ(finding->placementHoldFor("a"), hold_a) << "stable per frame";

  const auto edge = [](const std::string& child, Timestamp t, double x) {
    return pj::tf::StampedTransform{
        PJ::fromRaw(t), "map", child, pj::tf::Transform{{x, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}}};
  };
  pj::tf::TransformBuffer measured;
  pj::tf::TransformBuffer later;  // the samples at 10 s are gone
  for (const auto& [child, x] : std::vector<std::pair<std::string, double>>{{"a", 1.0}, {"b", 2.0}}) {
    ASSERT_TRUE(measured.setTransform(edge(child, 10 * kSec, x)).has_value());
    ASSERT_TRUE(measured.setTransform(edge(child, 20 * kSec, 50.0)).has_value());
    ASSERT_TRUE(later.setTransform(edge(child, 15 * kSec, 50.0)).has_value());
    ASSERT_TRUE(later.setTransform(edge(child, 20 * kSec, 50.0)).has_value());
  }
  const auto stamp = std::optional<PJ::Timepoint>(PJ::fromRaw(10 * kSec));
  const PJ::Timepoint cursor = PJ::fromRaw(20 * kSec);
  ASSERT_TRUE(pj::scene3d::placeObject(measured, "map", "a", stamp, cursor, hold_a).has_value());
  ASSERT_TRUE(pj::scene3d::placeObject(measured, "map", "b", stamp, cursor, hold_b).has_value());
  const auto a = pj::scene3d::placeObject(later, "map", "a", stamp, cursor, finding->placementHoldFor("a"));
  const auto b = pj::scene3d::placeObject(later, "map", "b", stamp, cursor, finding->placementHoldFor("b"));
  ASSERT_TRUE(a.has_value() && b.has_value());
  EXPECT_DOUBLE_EQ(a->t.x, 1.0);
  EXPECT_DOUBLE_EQ(b->t.x, 2.0);
}

// ---- measurements -----------------------------------------------------------------------------------------------

// A 10 Hz publication stream refreshes the dock about once per publication: at most 20 repaint requests a second,
// under the 60 Hz gate (counted, not timed).
TEST(Scene3DDockDerivedTopics, APublicationStreamStaysUnderTheRepaintGate) {
  Fixture fx;
  constexpr int kPublications = 10;
  constexpr Timestamp kPeriod = kSec / 10;
  for (int index = 1; index <= kPublications; ++index) {
    fx.pushCloud(20 * kSec + index * kPeriod);
  }
  fx.install("crop");
  auto* witness = fx.addWitness("crop");
  ASSERT_NE(witness, nullptr);
  fx.tick(21 * kSec);  // parked at the stream's end: each result lands at or before it
  const int before = fx.dock.refreshes;
  for (int index = 1; index <= kPublications; ++index) {
    fx.request("crop", 20 * kSec + index * kPeriod);
    fx.settle();
    EXPECT_EQ(witness->currentEntities().size(), 1u);
  }
  // A result asks for at most two frames, its model fold and its marker expansion; Qt merges them into one paint.
  EXPECT_GE(fx.dock.refreshes - before, kPublications);
  EXPECT_LE(fx.dock.refreshes - before, 2 * kPublications);
}

// ---- initial framing (SCN-08) ---------------------------------------------------------------------------------

/// A cloud in frame "lidar" with points on the x axis at `xs`.
std::vector<uint8_t> lidarCloud(const std::vector<float>& xs, Timestamp stamp) {
  std::vector<uint8_t> bytes(xs.size() * PJ::test::kPointStride, 0);
  for (size_t index = 0; index < xs.size(); ++index) {
    std::memcpy(bytes.data() + index * PJ::test::kPointStride, &xs[index], sizeof(float));
  }
  const PJ::sdk::PointCloud cloud = PJ::test::cloudOver(PJ::sdk::makePayloadView(std::move(bytes)), stamp, "lidar");
  return PJ::serializeBuiltinObject(PJ::sdk::BuiltinObject(cloud)).value();
}

// The first content to arrive, at once or later, is fitted once: the fixed frame becomes its own frame (not
// remembered as the user's pick) and the camera frames it; later content does not refit.
TEST(Scene3DDockInitialFraming, FitsImmediateAndDelayedContentOnlyOnce) {
  for (const bool delayed : {false, true}) {
    Fixture fx;
    pj::scene3d::FixedFrameMemory memory{fx.session};
    fx.dock.setFixedFrameMemory(&memory);
    const auto topic = registerCanonicalTopic(fx.store, fx.ds, "fit", PJ::sdk::BuiltinObjectType::kPointCloud);
    if (!delayed) {
      ASSERT_TRUE(fx.store.pushOwned(topic, 0, lidarCloud({100.0F, 110.0F}, 0)).has_value());
    }
    fx.dock.frameInitialContent();
    auto* layer = fx.add<pj::scene3d::PointCloudLayer>(topic, PJ::sdk::BuiltinObjectType::kPointCloud);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(fx.dock.sceneView(), nullptr);
    if (delayed) {
      ASSERT_TRUE(fx.store.pushOwned(topic, 0, lidarCloud({100.0F, 110.0F}, 0)).has_value());
    }
    fx.dock.onTrackerTime(0);
    // The never-shown offscreen view does not paint: run the decode a paint would, with its repaintRequested.
    layer->renderAtForTest(0);
    ASSERT_TRUE(pj::scene3d::test::pumpUntil([&] { return !fx.dock.initialContentFramingPending(); }));
    EXPECT_EQ(fx.dock.currentFixedFrame(), u"lidar"_s) << "delayed=" << delayed;
    EXPECT_NEAR(fx.dock.sceneView()->camera().state().focal.x, 105.0F, 0.01F);
    EXPECT_GT(fx.dock.sceneView()->camera().state().radius, 5.0F);
    EXPECT_TRUE(memory.remembered(fx.ds).isEmpty()) << "the fit is not the user's pick";

    ASSERT_TRUE(fx.store.pushOwned(topic, 100, lidarCloud({200.0F, 210.0F}, 100)).has_value());
    fx.dock.onTrackerTime(displaySecondsOf(100));
    layer->renderAtForTest(100);
    QCoreApplication::processEvents();
    EXPECT_NEAR(fx.dock.sceneView()->camera().state().focal.x, 105.0F, 0.01F) << "fitted once";
  }
}

// The fit frames where the content is drawn: a cloud measured at 5 s in a frame that moved 100 m by the 20 s cursor
// is placed relative to the frame's pose at the cursor, and the camera frames that placement.
TEST(Scene3DDockInitialFraming, FitsThePlacedBoundsOfAStampedCloud) {
  Fixture fx;
  const auto buffer = fx.tf.transformBuffer(fx.ds);
  ASSERT_NE(buffer, nullptr);
  for (const auto& [t, x] : std::vector<std::pair<Timestamp, double>>{{5 * kSec, 0.0}, {20 * kSec, 100.0}}) {
    ASSERT_TRUE(
        buffer
            ->setTransform(
                pj::tf::StampedTransform{
                    PJ::fromRaw(t), "map", "lidar", pj::tf::Transform{{x, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}}})
            .has_value());
  }
  const auto topic = registerCanonicalTopic(fx.store, fx.ds, "fit", PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(fx.store.pushOwned(topic, 5 * kSec, lidarCloud({100.0F, 110.0F}, 5 * kSec)).has_value());
  fx.dock.frameInitialContent();
  auto* layer = fx.add<pj::scene3d::PointCloudLayer>(topic, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_NE(layer, nullptr);
  ASSERT_NE(fx.dock.sceneView(), nullptr);
  ASSERT_TRUE(fx.store.pushOwned(topic, 20 * kSec, lidarCloud({100.0F, 110.0F}, 5 * kSec)).has_value());
  fx.dock.onTrackerTime(displaySecondsOf(20 * kSec));
  layer->renderAtForTest(20 * kSec);
  ASSERT_TRUE(pj::scene3d::test::pumpUntil([&] { return !fx.dock.initialContentFramingPending(); }));
  EXPECT_EQ(fx.dock.currentFixedFrame(), u"lidar"_s);
  EXPECT_NEAR(fx.dock.sceneView()->camera().state().focal.x, 5.0F, 0.01F) << "the placed bounds, not the source's";
}

// User camera input, or a restored camera, before the content arrives cancels the fit: the camera stays.
TEST(Scene3DDockInitialFraming, UserInputAndRestoredCameraCancelPendingFit) {
  for (const bool restore : {false, true}) {
    Fixture fx;
    const auto topic = registerCanonicalTopic(fx.store, fx.ds, "fit", PJ::sdk::BuiltinObjectType::kPointCloud);
    fx.dock.frameInitialContent();
    auto* layer = fx.add<pj::scene3d::PointCloudLayer>(topic, PJ::sdk::BuiltinObjectType::kPointCloud);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(fx.dock.sceneView(), nullptr);
    const auto original = fx.dock.sceneView()->camera().state();
    if (restore) {
      QDomDocument document;
      const QDomElement saved = fx.dock.xmlSaveState(document);
      ASSERT_TRUE(fx.dock.xmlLoadState(saved));
      // The restore destroys the layers and rebuilds them from the XML: the old pointer dangles.
      layer = dynamic_cast<pj::scene3d::PointCloudLayer*>(fx.dock.layerFor(topic));
      ASSERT_NE(layer, nullptr);
    } else {
      QMouseEvent press(
          QEvent::MouseButtonPress, QPointF(10, 10), QPointF(10, 10), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(fx.dock.sceneView(), &press);
    }
    EXPECT_FALSE(fx.dock.initialContentFramingPending()) << "restore=" << restore;
    ASSERT_TRUE(fx.store.pushOwned(topic, 0, lidarCloud({100.0F, 110.0F}, 0)).has_value());
    fx.dock.onTrackerTime(0);
    layer->renderAtForTest(0);
    QCoreApplication::processEvents();
    EXPECT_EQ(fx.dock.sceneView()->camera().state().focal, original.focal);
    EXPECT_EQ(fx.dock.sceneView()->camera().state().radius, original.radius);
  }
}

}  // namespace
