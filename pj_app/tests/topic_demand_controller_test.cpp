// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QtGlobal>
#include <memory>
#include <string_view>
#include <utility>

#include "PendingDisplayBinder.h"
#include "TopicDemandController.h"
#include "pj_base/type_tree.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/StateTransitionsDockWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "pj_scene_common/layer_factory.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/script_engine.h"
using namespace Qt::StringLiterals;

namespace {

using PJ::DatasetId;
using PJ::ObjectTopicId;

// --- helpers mirroring pending_curve_binder_test.cpp -----------------------

DatasetId createDataset(PJ::AppSession& app_session) {
  auto dataset = app_session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "test"});
  if (!dataset.has_value()) {
    ADD_FAILURE() << dataset.error();
    return 0;
  }
  return *dataset;
}

// registerScalarSeries hardcodes its sole column's field_path to "value" (see
// DataWriter::registerScalarSeries), so a curve/pending-bind resolving this
// topic must ask for that field, not an empty one.
QString addScalarTopic(PJ::AppSession& app_session, DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = app_session.sessionManager().dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  if (!handle.has_value()) {
    ADD_FAILURE() << handle.error();
    return {};
  }
  writer.appendScalar(*handle, 100, 1.0);
  writer.appendScalar(*handle, 200, 2.0);
  if (app_session.sessionManager().commitChunks(writer.flushAll()).empty()) {
    ADD_FAILURE() << "commit produced no changed topics";
    return {};
  }
  app_session.catalogModel().rebuildFromDatastore();
  const auto key = PJ::resolveSeriesPath(
      app_session.catalogModel(),
      PJ::layout_xml::SeriesPath{
          QString::fromUtf8(topic_name.data(), static_cast<int>(topic_name.size())), u"value"_s});
  return key.value_or(QString());
}

// A topic whose sole scalar column has an EMPTY field_path — the shape a
// PendingDisplayBinder placeholder-drop pend (field="") can actually resolve
// against (see PendingDisplayBinder::addPendingCurve's doc: a placeholder row
// carries no field breakdown, so the pend only completes for a single-field
// topic named "" — this constructs exactly that, bypassing
// registerScalarSeries's hardcoded "value" column name).
QString addRawScalarTopic(PJ::AppSession& app_session, DatasetId dataset_id, const QString& topic_name) {
  PJ::DataWriter writer = app_session.sessionManager().dataEngine().createWriter();
  auto topic_id = writer.registerTopic(dataset_id, PJ::TopicDescriptor{.name = topic_name.toStdString()});
  if (!topic_id.has_value()) {
    ADD_FAILURE() << topic_id.error();
    return {};
  }
  if (auto field_id = writer.ensureColumn(*topic_id, "", PJ::PrimitiveType::kFloat64); !field_id.has_value()) {
    ADD_FAILURE() << field_id.error();
    return {};
  }
  if (auto status = writer.beginRow(*topic_id, 100); !status) {
    ADD_FAILURE() << status.error();
    return {};
  }
  writer.set(*topic_id, 0, 1.0);
  if (auto status = writer.finishRow(*topic_id); !status) {
    ADD_FAILURE() << status.error();
    return {};
  }
  if (app_session.sessionManager().commitChunks(writer.flushAll()).empty()) {
    ADD_FAILURE() << "commit produced no changed topics";
    return {};
  }
  app_session.catalogModel().rebuildFromDatastore();
  const auto key = PJ::resolveSeriesPath(app_session.catalogModel(), PJ::layout_xml::SeriesPath{topic_name, QString()});
  return key.value_or(QString());
}

void addObjectTopic(PJ::AppSession& app_session, DatasetId dataset_id, const QString& topic_name) {
  auto registered = app_session.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset_id,
          .topic_name = topic_name.toStdString(),
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(registered.has_value()) << registered.error();
  app_session.catalogModel().rebuildFromDatastore();
}

bool referencesTopic(PJ::TopicDemandTracker& tracker, DatasetId dataset_id, const QString& topic_name) {
  const auto active = tracker.activeTopics(dataset_id);
  return std::find(active.begin(), active.end(), topic_name) != active.end();
}

// Spins the event loop for `ms` so a preview's single-shot timeout QTimer can
// fire (the test binary does not link Qt6::Test / QTest::qWait).
void spinFor(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

std::shared_ptr<PJ::scripting::FilterCatalogue> catalogueFromSource(const char* src) {
  auto cat = std::make_shared<PJ::scripting::FilterCatalogue>(PJ::scripting::makeLuauEngine());
  EXPECT_TRUE(cat->addBundledSource(src, "bundled").has_value());
  return cat;
}
constexpr const char* kAbsoluteSrc = R"LUAU(
return { { id="absolute", name="Absolute", output="same",
  create = function(p) return { calculate = function(t, v) return math.abs(v) end } end } }
)LUAU";

// --- minimal SceneDockWidget stub -------------------------------------------

class StubSceneLayer : public PJ::ISceneLayer {
 public:
  StubSceneLayer(ObjectTopicId id, PJ::sdk::BuiltinObjectType type, QString name)
      : topic_id_(id), object_type_(type), name_(std::move(name)) {}

  [[nodiscard]] PJ::SceneLayerInfo info() const override {
    return PJ::SceneLayerInfo{topic_id_, object_type_, name_, u"stub"_s, true};
  }
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override {
    return PJ::Range<PJ::Timepoint>{PJ::Timepoint::max(), PJ::Timepoint::min()};
  }
  bool attach(const PJ::SceneLayerContext& /*ctx*/) override {
    return true;
  }
  void detach() override {}
  void setTrackerTime(PJ::Timepoint /*time*/) override {}
  void setVisible(bool /*visible*/) override {}
  QWidget* createConfigWidget(QWidget* /*parent*/) override {
    return nullptr;
  }

 private:
  ObjectTopicId topic_id_;
  PJ::sdk::BuiltinObjectType object_type_;
  QString name_;
};

class StubSceneDockWidget : public PJ::SceneDockWidget {
 public:
  explicit StubSceneDockWidget(QWidget* parent = nullptr) : PJ::SceneDockWidget(parent) {
    layerFactory().registerType(
        PJ::sdk::BuiltinObjectType::kPointCloud,
        [](ObjectTopicId id, PJ::sdk::BuiltinObjectType type, const QString& name) {
          return std::make_unique<StubSceneLayer>(id, type, name);
        });
  }

 protected:
  QWidget* createSceneView() override {
    return new QWidget();
  }
  std::unique_ptr<PJ::SceneLayerContext> makeContext() override {
    return std::make_unique<PJ::SceneLayerContext>();
  }
  [[nodiscard]] bool acceptsObjectType(PJ::sdk::BuiltinObjectType object_type) const override {
    return object_type == PJ::sdk::BuiltinObjectType::kPointCloud;
  }
  void syncViewLayers(const std::vector<PJ::ISceneLayer*>& /*ordered_layers*/) override {}
};

void wireSceneDock(StubSceneDockWidget& dock, PJ::AppSession& app_session) {
  dock.setSessionManager(&app_session.sessionManager());
}

// A committed single-column STRING topic; returns its catalog key. The
// discrete counterpart of addScalarTopic for the state-strip demand tests.
QString addStringTopic(PJ::AppSession& app_session, DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = app_session.sessionManager().dataEngine().createWriter();
  auto schema_or =
      writer.registerSchema(std::string(topic_name), PJ::makePrimitive("state", PJ::PrimitiveType::kString));
  if (!schema_or.has_value()) {
    ADD_FAILURE() << schema_or.error();
    return {};
  }
  PJ::TopicDescriptor descriptor;
  descriptor.name = std::string(topic_name);
  descriptor.schema_id = *schema_or;
  auto topic_or = writer.registerTopic(dataset_id, descriptor);
  if (!topic_or.has_value() || !writer.bindTopicWriter(*topic_or).has_value()) {
    ADD_FAILURE() << "string topic registration failed";
    return {};
  }
  EXPECT_TRUE(writer.beginRow(*topic_or, 100).has_value());
  writer.set(*topic_or, 0, std::string_view{"IDLE"});
  EXPECT_TRUE(writer.finishRow(*topic_or).has_value());
  if (app_session.sessionManager().commitChunks(writer.flushAll()).empty()) {
    ADD_FAILURE() << "commit produced no changed topics";
    return {};
  }
  auto& catalog = app_session.catalogModel();
  catalog.rebuildFromDatastore();
  for (const PJ::CatalogItem& item : catalog.items()) {
    if (catalog.isDiscreteKey(item.key) && item.dataset_id == dataset_id) {
      return item.key;
    }
  }
  return {};
}

// --- fixture -----------------------------------------------------------------

class TopicDemandControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    app_session_ = std::make_unique<PJ::AppSession>();
    dataset_id_ = createDataset(*app_session_);
    ASSERT_NE(dataset_id_, 0U);
    binder_ =
        std::make_unique<PJ::PendingDisplayBinder>(app_session_->catalogModel(), &app_session_->topicDemandTracker());
    controller_ = std::make_unique<PJ::TopicDemandController>(
        app_session_->catalogModel(), app_session_->topicDemandTracker(),
        app_session_->sessionManager().dataProcessorService(), *binder_, app_session_->sessionManager().dataEngine());
  }

  std::unique_ptr<PJ::AppSession> app_session_;
  DatasetId dataset_id_ = 0;
  std::unique_ptr<PJ::PendingDisplayBinder> binder_;
  std::unique_ptr<PJ::TopicDemandController> controller_;
};

TEST_F(TopicDemandControllerTest, PlotCurveAddRemoveTracksDemand) {
  const QString key = addScalarTopic(*app_session_, dataset_id_, "/speed");
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget plot(&app_session_->sessionManager(), &app_session_->catalogModel());
  controller_->registerPlot(&plot);
  EXPECT_FALSE(referencesTopic(app_session_->topicDemandTracker(), dataset_id_, u"/speed"_s));

  ASSERT_NE(plot.addCurve(key), nullptr);
  EXPECT_TRUE(referencesTopic(app_session_->topicDemandTracker(), dataset_id_, u"/speed"_s));

  plot.removeAllCurves();
  EXPECT_FALSE(referencesTopic(app_session_->topicDemandTracker(), dataset_id_, u"/speed"_s));
}

TEST_F(TopicDemandControllerTest, XyCurveReferencesBothSources) {
  const QString x_key = addScalarTopic(*app_session_, dataset_id_, "/pose_x");
  const QString y_key = addScalarTopic(*app_session_, dataset_id_, "/pose_y");
  ASSERT_FALSE(x_key.isEmpty());
  ASSERT_FALSE(y_key.isEmpty());

  PJ::PlotWidget plot(&app_session_->sessionManager(), &app_session_->catalogModel());
  controller_->registerPlot(&plot);
  plot.setModeXY(true);
  ASSERT_NE(plot.addCurveXY(x_key, y_key, u"xy"_s), nullptr);

  auto& tracker = app_session_->topicDemandTracker();
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/pose_x"_s));
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/pose_y"_s));

  plot.removeAllCurves();
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/pose_x"_s));
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/pose_y"_s));
}

TEST_F(TopicDemandControllerTest, DerivedFilterOutputReferencesSourceNotOutput) {
  const QString source_key = addScalarTopic(*app_session_, dataset_id_, "/speed");
  ASSERT_FALSE(source_key.isEmpty());

  auto& processors = app_session_->sessionManager().dataProcessorService();
  processors.setFilterCatalogue(catalogueFromSource(kAbsoluteSrc));
  const auto source_descriptor = app_session_->catalogModel().curveDescriptor(source_key);
  ASSERT_TRUE(source_descriptor.has_value());
  const auto filter_result =
      processors.applyFilter(source_descriptor->topic_id, dataset_id_, "absolute", "speed[Absolute]");
  ASSERT_TRUE(filter_result.has_value()) << filter_result.error();
  app_session_->catalogModel().rebuildFromDatastore();

  const auto output_key =
      PJ::resolveSeriesPath(app_session_->catalogModel(), PJ::layout_xml::SeriesPath{u"speed[Absolute]"_s, u"value"_s});
  ASSERT_TRUE(output_key.has_value());

  PJ::PlotWidget plot(&app_session_->sessionManager(), &app_session_->catalogModel());
  controller_->registerPlot(&plot);
  ASSERT_NE(plot.addCurve(*output_key), nullptr);

  auto& tracker = app_session_->topicDemandTracker();
  // The displayed curve is the derived OUTPUT; demand must land on its SOURCE
  // ("/speed"), not on the output's own (unsubscribable) name.
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/speed"_s));
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"speed[Absolute]"_s));
}

TEST_F(TopicDemandControllerTest, PlotDestructionReleasesReferences) {
  const QString key = addScalarTopic(*app_session_, dataset_id_, "/speed");
  ASSERT_FALSE(key.isEmpty());

  auto plot = std::make_unique<PJ::PlotWidget>(&app_session_->sessionManager(), &app_session_->catalogModel());
  controller_->registerPlot(plot.get());
  ASSERT_NE(plot->addCurve(key), nullptr);
  EXPECT_TRUE(referencesTopic(app_session_->topicDemandTracker(), dataset_id_, u"/speed"_s));

  plot.reset();
  EXPECT_FALSE(referencesTopic(app_session_->topicDemandTracker(), dataset_id_, u"/speed"_s));
}

TEST_F(TopicDemandControllerTest, PlaceholderPlotDropStagesPendingBindThatCompletesOnRealTopic) {
  auto& catalog = app_session_->catalogModel();
  catalog.setAdvertisedTopics(dataset_id_, {PJ::AdvertisedTopic{u"/speed"_s, PJ::sdk::BuiltinObjectType::kNone}});

  PJ::PlotWidget plot(&app_session_->sessionManager(), &app_session_->catalogModel());
  controller_->registerPlot(&plot);

  controller_->handlePlaceholderPlotDrop(&plot, dataset_id_, u"/speed"_s);
  auto& tracker = app_session_->topicDemandTracker();
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/speed"_s));
  EXPECT_TRUE(plot.curveList().empty());  // no fabricated curve — still pending

  // Real data arrives, superseding the placeholder. The pend was staged with
  // field="" (handlePlaceholderPlotDrop's convention), so the real topic must
  // resolve against an EMPTY field path to complete — addRawScalarTopic (unlike
  // addScalarTopic's "value"-named column) matches that shape.
  const QString key = addRawScalarTopic(*app_session_, dataset_id_, u"/speed"_s);
  ASSERT_FALSE(key.isEmpty());
  EXPECT_EQ(binder_->flush(QSet<QString>{u"/speed"_s}), 1);

  EXPECT_EQ(plot.curveList().size(), 1U);
  // Exactly one reference survives the pending->real handoff (never zero, never two).
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/speed"_s));
}

TEST_F(TopicDemandControllerTest, PendingBindAttachesDemandWhenTopicIsAdvertisedLater) {
  auto& tracker = app_session_->topicDemandTracker();

  // Layout-restore shape: the pend is staged BEFORE any dataset names the topic
  // (the stream has not connected yet), so no demand reference can exist.
  PJ::PlotWidget plot(&app_session_->sessionManager(), &app_session_->catalogModel());
  binder_->addPendingCurve(&plot, PJ::layout_xml::SeriesPath{u"/lidar"_s, u"range"_s});
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/lidar"_s));

  // The stream connects and advertises the topic (placeholder only, no data yet).
  // The still-pending entry must attach its demand reference NOW — without it the
  // topic never subscribes, no data ever arrives, and the pend waits forever.
  app_session_->catalogModel().setAdvertisedTopics(
      dataset_id_, {PJ::AdvertisedTopic{u"/lidar"_s, PJ::sdk::BuiltinObjectType::kNone}});
  EXPECT_EQ(binder_->flush(QSet<QString>{u"/lidar"_s}), 0);  // cannot bind yet — no data
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/lidar"_s));
}

ObjectTopicId findObjectTopicId(const PJ::CatalogModel& catalog, const QString& topic_name) {
  for (const auto& catalog_item : catalog.items()) {
    if (const auto* object_topic = PJ::asObjectTopic(catalog_item);
        object_topic != nullptr && catalog_item.topic_name == topic_name) {
      return object_topic->object_topic_id;
    }
  }
  return ObjectTopicId{};
}

TEST_F(TopicDemandControllerTest, SceneDockLayerAddRemoveTracksDemand) {
  addObjectTopic(*app_session_, dataset_id_, u"/points"_s);

  StubSceneDockWidget dock;
  controller_->registerSceneDock(&dock);

  const ObjectTopicId topic_id = findObjectTopicId(app_session_->catalogModel(), u"/points"_s);
  ASSERT_NE(topic_id.id, 0U);

  ASSERT_TRUE(dock.addTopic(topic_id, PJ::sdk::BuiltinObjectType::kPointCloud, u"/points"_s));
  auto& tracker = app_session_->topicDemandTracker();
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/points"_s));

  dock.removeTopic(topic_id);
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/points"_s));
}

TEST_F(TopicDemandControllerTest, SceneDockPlaceholderDropCompletesOnRealTopicWithoutDoubleCounting) {
  // The drop always originates from an advertised placeholder — the pend's
  // demand reference attaches to the datasets naming the topic in the catalog.
  app_session_->catalogModel().setAdvertisedTopics(
      dataset_id_, {PJ::AdvertisedTopic{u"/points"_s, PJ::sdk::BuiltinObjectType::kPointCloud}});

  auto dock = std::make_unique<StubSceneDockWidget>();
  wireSceneDock(*dock, *app_session_);
  controller_->registerSceneDock(dock.get());

  controller_->handleSceneDockPlaceholderDrop(
      dock.get(), dataset_id_, u"/points"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  auto& tracker = app_session_->topicDemandTracker();
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/points"_s));

  // Real object topic arrives — the flush pass (MainWindow's permanent
  // itemsAdded wiring) retries addTopic and hands the reference off from the
  // placeholder hold to the real layerAdded-driven one.
  addObjectTopic(*app_session_, dataset_id_, u"/points"_s);
  EXPECT_EQ(dock->retryPendingRestores(QSet<QString>{u"/points"_s}), 1);
  EXPECT_EQ(binder_->flush(QSet<QString>{u"/points"_s}), 1);
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/points"_s));
  const ObjectTopicId topic_id = findObjectTopicId(app_session_->catalogModel(), u"/points"_s);
  ASSERT_NE(topic_id.id, 0U);
  EXPECT_NE(dock->layerFor(topic_id), nullptr);

  dock.reset();
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/points"_s));
}

TEST_F(TopicDemandControllerTest, StateStripSeriesAddRemoveTracksDemand) {
  const QString key = addStringTopic(*app_session_, dataset_id_, "/robot/state");
  ASSERT_FALSE(key.isEmpty());

  PJ::StateTransitionsDockWidget dock(
      &app_session_->sessionManager(), &app_session_->catalogModel(), &app_session_->playbackEngine());
  controller_->registerStateTransitionsDock(&dock);
  // Double registration is a no-op (mirrors registerPlot's guard).
  controller_->registerStateTransitionsDock(&dock);

  ASSERT_TRUE(dock.controller()->addSeries(key));
  auto& tracker = app_session_->topicDemandTracker();
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/robot/state"_s));

  const auto entries = dock.controller()->seriesEntries();
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_TRUE(dock.controller()->removeSeries(entries.front().row_id));
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/robot/state"_s));
}

TEST_F(TopicDemandControllerTest, StateStripDestructionReleasesReferences) {
  const QString key = addStringTopic(*app_session_, dataset_id_, "/robot/state");
  ASSERT_FALSE(key.isEmpty());

  auto dock = std::make_unique<PJ::StateTransitionsDockWidget>(
      &app_session_->sessionManager(), &app_session_->catalogModel(), &app_session_->playbackEngine());
  controller_->registerStateTransitionsDock(dock.get());
  ASSERT_TRUE(dock->controller()->addSeries(key));
  auto& tracker = app_session_->topicDemandTracker();
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/robot/state"_s));

  dock.reset();
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/robot/state"_s));
}

TEST_F(TopicDemandControllerTest, PlaceholderPlotDropBindsConventionalValueFieldTopic) {
  auto& catalog = app_session_->catalogModel();
  catalog.setAdvertisedTopics(dataset_id_, {PJ::AdvertisedTopic{u"/speed"_s, PJ::sdk::BuiltinObjectType::kNone}});

  PJ::PlotWidget plot(&app_session_->sessionManager(), &app_session_->catalogModel());
  controller_->registerPlot(&plot);
  controller_->handlePlaceholderPlotDrop(&plot, dataset_id_, u"/speed"_s);

  // Real data arrives with the CONVENTIONAL "value"-named sole column
  // (registerScalarSeries's shape — the overwhelmingly common case). The
  // empty-field pend must fall back to binding the topic's scalar field(s),
  // not wait forever for an exact empty-field descriptor match.
  const QString key = addScalarTopic(*app_session_, dataset_id_, "/speed");
  ASSERT_FALSE(key.isEmpty());
  EXPECT_EQ(binder_->flush(QSet<QString>{u"/speed"_s}), 1);
  EXPECT_EQ(plot.curveList().size(), 1U);
  EXPECT_TRUE(referencesTopic(app_session_->topicDemandTracker(), dataset_id_, u"/speed"_s));
}

TEST_F(TopicDemandControllerTest, SceneDockDestructionReleasesUnresolvedPlaceholderDropReference) {
  app_session_->catalogModel().setAdvertisedTopics(
      dataset_id_, {PJ::AdvertisedTopic{u"/points"_s, PJ::sdk::BuiltinObjectType::kPointCloud}});

  auto dock = std::make_unique<StubSceneDockWidget>();
  wireSceneDock(*dock, *app_session_);
  controller_->registerSceneDock(dock.get());

  controller_->handleSceneDockPlaceholderDrop(
      dock.get(), dataset_id_, u"/points"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  auto& tracker = app_session_->topicDemandTracker();
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/points"_s));

  // Dock dies while the drop is still pending (real topic never arrived) — the
  // placeholder hold must be released with it, or the topic stays subscribed.
  dock.reset();
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/points"_s));
}

TEST_F(TopicDemandControllerTest, SceneDropSurvivesStreamReconnectMintingFreshDatasetId) {
  // A scene placeholder drop staged against dataset A must complete when the
  // topic materializes under dataset B (stream reconnects mint fresh dataset
  // ids, re-advertising the same names) — mirroring the curve pend's
  // preferred-dataset-with-fallback resolution.
  auto& catalog = app_session_->catalogModel();
  catalog.setAdvertisedTopics(
      dataset_id_, {PJ::AdvertisedTopic{u"/points"_s, PJ::sdk::BuiltinObjectType::kPointCloud}});

  auto dock = std::make_unique<StubSceneDockWidget>();
  wireSceneDock(*dock, *app_session_);
  controller_->registerSceneDock(dock.get());
  controller_->handleSceneDockPlaceholderDrop(
      dock.get(), dataset_id_, u"/points"_s, PJ::sdk::BuiltinObjectType::kPointCloud);

  auto& tracker = app_session_->topicDemandTracker();
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/points"_s));

  // Reconnect: the fresh dataset advertises the same topic. The pend's demand
  // reference must attach to it (or the topic never subscribes there and the
  // pend deadlocks) — the flush refresh pass mirrors MainWindow's permanent
  // itemsAdded wiring.
  const DatasetId dataset_b = createDataset(*app_session_);
  ASSERT_NE(dataset_b, 0U);
  ASSERT_NE(dataset_b, dataset_id_);
  catalog.setAdvertisedTopics(dataset_b, {PJ::AdvertisedTopic{u"/points"_s, PJ::sdk::BuiltinObjectType::kPointCloud}});
  static_cast<void>(binder_->flush({}));
  EXPECT_TRUE(referencesTopic(tracker, dataset_b, u"/points"_s))
      << "pend reference must follow the topic to the reconnected dataset";
  ASSERT_TRUE(catalog.removeDataset(dataset_id_, /*tombstone=*/false));
  app_session_->sessionManager().removeDataset(dataset_id_);

  // The topic materializes under B only — the pend must still complete.
  addObjectTopic(*app_session_, dataset_b, u"/points"_s);
  EXPECT_EQ(dock->retryPendingRestores(QSet<QString>{u"/points"_s}), 1);
  static_cast<void>(binder_->flush(QSet<QString>{u"/points"_s}));
  const ObjectTopicId topic_id = findObjectTopicId(app_session_->catalogModel(), u"/points"_s);
  ASSERT_NE(topic_id.id, 0U);
  EXPECT_NE(dock->layerFor(topic_id), nullptr) << "drop staged against A must complete against B";

  dock.reset();
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/points"_s));
  EXPECT_FALSE(referencesTopic(tracker, dataset_b, u"/points"_s));
}

TEST_F(TopicDemandControllerTest, PlotDestructionReleasesUnresolvedPlaceholderDropReferenceEagerly) {
  // Curve twin of SceneDockDestructionReleases...: a plot destroyed while its
  // placeholder drop is still pending must release the demand reference AT
  // destruction — not lazily at some later flush, which never comes on a quiet
  // stream and leaves the topic subscribed forever.
  auto& catalog = app_session_->catalogModel();
  catalog.setAdvertisedTopics(dataset_id_, {PJ::AdvertisedTopic{u"/speed"_s, PJ::sdk::BuiltinObjectType::kNone}});

  auto plot = std::make_unique<PJ::PlotWidget>(&app_session_->sessionManager(), &app_session_->catalogModel());
  controller_->registerPlot(plot.get());
  controller_->handlePlaceholderPlotDrop(plot.get(), dataset_id_, u"/speed"_s);

  auto& tracker = app_session_->topicDemandTracker();
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/speed"_s));

  plot.reset();  // no flush after this — release must be eager
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/speed"_s));
}

TEST_F(TopicDemandControllerTest, AllPendingSceneDropsForOneTopicComplete) {
  app_session_->catalogModel().setAdvertisedTopics(
      dataset_id_, {PJ::AdvertisedTopic{u"/points"_s, PJ::sdk::BuiltinObjectType::kPointCloud}});

  auto dock_a = std::make_unique<StubSceneDockWidget>();
  auto dock_b = std::make_unique<StubSceneDockWidget>();
  wireSceneDock(*dock_a, *app_session_);
  wireSceneDock(*dock_b, *app_session_);
  controller_->registerSceneDock(dock_a.get());
  controller_->registerSceneDock(dock_b.get());

  controller_->handleSceneDockPlaceholderDrop(
      dock_a.get(), dataset_id_, u"/points"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  controller_->handleSceneDockPlaceholderDrop(
      dock_b.get(), dataset_id_, u"/points"_s, PJ::sdk::BuiltinObjectType::kPointCloud);

  addObjectTopic(*app_session_, dataset_id_, u"/points"_s);
  EXPECT_EQ(dock_a->retryPendingRestores(QSet<QString>{u"/points"_s}), 1);
  EXPECT_EQ(dock_b->retryPendingRestores(QSet<QString>{u"/points"_s}), 1);
  // BOTH pending drops complete on one flush pass (not just the first match).
  EXPECT_EQ(binder_->flush(QSet<QString>{u"/points"_s}), 2);
  const ObjectTopicId topic_id = findObjectTopicId(app_session_->catalogModel(), u"/points"_s);
  ASSERT_NE(topic_id.id, 0U);
  // BOTH pending drops complete (not just the first match) — each dock got its layer.
  EXPECT_NE(dock_a->layerFor(topic_id), nullptr);
  EXPECT_NE(dock_b->layerFor(topic_id), nullptr);

  auto& tracker = app_session_->topicDemandTracker();
  dock_a.reset();
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/points"_s)) << "dock B's layer still references the topic";
  dock_b.reset();
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/points"_s))
      << "no stuck pending reference may survive both docks";
}

// --- bounded field-preview subscriptions (census + double-click peek) --------

TEST_F(TopicDemandControllerTest, CensusPreviewsScalarPlaceholderAndReleasesOnPromotion) {
  auto& catalog = app_session_->catalogModel();
  auto& tracker = app_session_->topicDemandTracker();
  catalog.setPerTopicPauseCapable(dataset_id_, true);

  catalog.setAdvertisedTopics(dataset_id_, {PJ::AdvertisedTopic{u"/imu"_s, PJ::sdk::BuiltinObjectType::kNone}});
  // The census auto-previews the scalar placeholder: a demand reference is held.
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/imu"_s));

  // Real data lands (a real itemsAdded batch) → the preview releases on promotion.
  const QString key = addScalarTopic(*app_session_, dataset_id_, "/imu");
  ASSERT_FALSE(key.isEmpty());
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/imu"_s));

  // The topic is now in the previewed-this-session memory: re-advertising it adds
  // no fresh reference.
  catalog.setAdvertisedTopics(dataset_id_, {PJ::AdvertisedTopic{u"/imu"_s, PJ::sdk::BuiltinObjectType::kNone}});
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/imu"_s));
}

TEST_F(TopicDemandControllerTest, PreviewSampleIsDisownedWhenTopicGainsRealReference) {
  auto& catalog = app_session_->catalogModel();
  auto& tracker = app_session_->topicDemandTracker();
  catalog.setPerTopicPauseCapable(dataset_id_, true);

  // Census previews the placeholder; one real sample promotes it and releases.
  catalog.setAdvertisedTopics(dataset_id_, {PJ::AdvertisedTopic{u"/speed"_s, PJ::sdk::BuiltinObjectType::kNone}});
  ASSERT_TRUE(referencesTopic(tracker, dataset_id_, u"/speed"_s));
  const QString key = addScalarTopic(*app_session_, dataset_id_, "/speed");
  ASSERT_FALSE(key.isEmpty());
  ASSERT_FALSE(referencesTopic(tracker, dataset_id_, u"/speed"_s));

  // Pre-subscribe, the preview sample stays readable (Value column shows it).
  const PJ::DisplayOffset offset = app_session_->sessionManager().displayOffset(dataset_id_);
  const auto display_of = [&](PJ::Timestamp raw_ns) {
    return PJ::toAxisDouble(PJ::rawToDisplaySeconds(raw_ns, offset));
  };
  ASSERT_TRUE(catalog.scalarValueAt(key, display_of(200)).has_value());

  // A REAL display reference arrives (a plot drop). The fake-interest preview
  // sample must vanish from every read path — it would otherwise draw a bogus
  // flat segment from preview time to the first really-subscribed sample.
  tracker.addReference(dataset_id_, u"/speed"_s);
  EXPECT_FALSE(catalog.scalarValueAt(key, display_of(200)).has_value());

  // The discovered FIELDS survive the disown (columns stay registered).
  catalog.rebuildFromDatastore();
  EXPECT_TRUE(catalog.itemDescriptor(key).has_value());
}

TEST_F(TopicDemandControllerTest, RealHoldArrivingDuringPreviewCancelsTheDisown) {
  auto& catalog = app_session_->catalogModel();
  auto& tracker = app_session_->topicDemandTracker();
  catalog.setPerTopicPauseCapable(dataset_id_, true);

  // Census previews the placeholder...
  catalog.setAdvertisedTopics(dataset_id_, {PJ::AdvertisedTopic{u"/speed"_s, PJ::sdk::BuiltinObjectType::kNone}});
  ASSERT_TRUE(referencesTopic(tracker, dataset_id_, u"/speed"_s));

  // ...but a REAL hold (a pend completing, or the user forcing streaming)
  // arrives while the preview is still in flight: the data that follows is
  // really-requested history, not fake interest.
  tracker.addReference(dataset_id_, u"/speed"_s);

  const QString key = addScalarTopic(*app_session_, dataset_id_, "/speed");
  ASSERT_FALSE(key.isEmpty());
  const PJ::DisplayOffset offset = app_session_->sessionManager().displayOffset(dataset_id_);
  const auto display_of = [&](PJ::Timestamp raw_ns) {
    return PJ::toAxisDouble(PJ::rawToDisplaySeconds(raw_ns, offset));
  };
  ASSERT_TRUE(catalog.scalarValueAt(key, display_of(200)).has_value());

  // A later active-set emission naming the topic must NOT disown that history.
  tracker.addReference(dataset_id_, u"/other"_s);
  tracker.addReference(dataset_id_, u"/speed"_s);
  EXPECT_TRUE(catalog.scalarValueAt(key, display_of(200)).has_value())
      << "history collected under a real hold was disowned as if it were preview-only";
}

TEST_F(TopicDemandControllerTest, ReallySubscribedDataIsNeverDisowned) {
  auto& catalog = app_session_->catalogModel();
  auto& tracker = app_session_->topicDemandTracker();
  catalog.setPerTopicPauseCapable(dataset_id_, true);

  // Data that arrived WITHOUT a preview (a normal subscription) must keep its
  // history across reference churn — only preview-originated samples are fake.
  const QString key = addScalarTopic(*app_session_, dataset_id_, "/speed");
  ASSERT_FALSE(key.isEmpty());
  const PJ::DisplayOffset offset = app_session_->sessionManager().displayOffset(dataset_id_);
  const auto display_of = [&](PJ::Timestamp raw_ns) {
    return PJ::toAxisDouble(PJ::rawToDisplaySeconds(raw_ns, offset));
  };

  tracker.addReference(dataset_id_, u"/speed"_s);
  EXPECT_TRUE(catalog.scalarValueAt(key, display_of(200)).has_value());
  tracker.removeReference(dataset_id_, u"/speed"_s);
  tracker.addReference(dataset_id_, u"/speed"_s);
  EXPECT_TRUE(catalog.scalarValueAt(key, display_of(200)).has_value());
}

TEST_F(TopicDemandControllerTest, PreviewReleasesOnTimeoutWithoutPromotion) {
  auto& catalog = app_session_->catalogModel();
  auto& tracker = app_session_->topicDemandTracker();
  catalog.setPerTopicPauseCapable(dataset_id_, true);
  controller_->setPreviewTimeoutMsForTest(50);

  catalog.setAdvertisedTopics(dataset_id_, {PJ::AdvertisedTopic{u"/quiet"_s, PJ::sdk::BuiltinObjectType::kNone}});
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/quiet"_s));

  spinFor(200);  // past the injected timeout — nothing ever promoted it
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/quiet"_s));
}

TEST_F(TopicDemandControllerTest, CensusExcludesObjectClassifiedPlaceholder) {
  auto& catalog = app_session_->catalogModel();
  auto& tracker = app_session_->topicDemandTracker();
  catalog.setPerTopicPauseCapable(dataset_id_, true);

  // A pointcloud placeholder is bandwidth-heavy by construction — classification
  // is the size filter, so the census must not preview it.
  catalog.setAdvertisedTopics(
      dataset_id_, {PJ::AdvertisedTopic{u"/points"_s, PJ::sdk::BuiltinObjectType::kPointCloud}});
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/points"_s));
}

TEST_F(TopicDemandControllerTest, CensusSkipsNonCapableDataset) {
  auto& catalog = app_session_->catalogModel();
  auto& tracker = app_session_->topicDemandTracker();
  // Dataset left non-per-topic-pause-capable (a file / non-demand source).
  catalog.setAdvertisedTopics(dataset_id_, {PJ::AdvertisedTopic{u"/imu"_s, PJ::sdk::BuiltinObjectType::kNone}});
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/imu"_s));
}

TEST_F(TopicDemandControllerTest, CensusRunsOncePerTopicButManualPreviewBypassesMemory) {
  auto& catalog = app_session_->catalogModel();
  auto& tracker = app_session_->topicDemandTracker();
  controller_->setPreviewTimeoutMsForTest(50);

  // A real topic keeps the catalog non-empty, so clearAdvertisedTopics below only
  // removes the placeholder (it never empties the catalog and emits cleared(),
  // which would reset the census memory under test).
  ASSERT_FALSE(addScalarTopic(*app_session_, dataset_id_, "/anchor").isEmpty());
  catalog.setPerTopicPauseCapable(dataset_id_, true);

  catalog.setAdvertisedTopics(dataset_id_, {PJ::AdvertisedTopic{u"/quiet"_s, PJ::sdk::BuiltinObjectType::kNone}});
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/quiet"_s));

  spinFor(200);  // timeout → released and recorded in the census memory
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/quiet"_s));

  // Re-advertise the SAME topic (drop then re-add so it re-enters an itemsAdded
  // batch). The census must skip it.
  catalog.clearAdvertisedTopics(dataset_id_);
  catalog.setAdvertisedTopics(dataset_id_, {PJ::AdvertisedTopic{u"/quiet"_s, PJ::sdk::BuiltinObjectType::kNone}});
  EXPECT_FALSE(referencesTopic(tracker, dataset_id_, u"/quiet"_s))
      << "the census must not re-preview a topic it already previewed this session";

  // A manual peek bypasses the census memory and previews it again.
  controller_->requestFieldPreview(dataset_id_, u"/quiet"_s);
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/quiet"_s));
}

TEST_F(TopicDemandControllerTest, RequestFieldPreviewNoOpsWhenTopicAlreadyReferenced) {
  auto& catalog = app_session_->catalogModel();
  auto& tracker = app_session_->topicDemandTracker();

  // Advertise BEFORE making the dataset capable, so the census does not preview
  // the topic — isolating the active-set guard we want to exercise.
  catalog.setAdvertisedTopics(dataset_id_, {PJ::AdvertisedTopic{u"/speed"_s, PJ::sdk::BuiltinObjectType::kNone}});

  // A placeholder drop stages a pend that holds a real demand reference.
  PJ::PlotWidget plot(&app_session_->sessionManager(), &app_session_->catalogModel());
  controller_->registerPlot(&plot);
  controller_->handlePlaceholderPlotDrop(&plot, dataset_id_, u"/speed"_s);
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/speed"_s));

  catalog.setPerTopicPauseCapable(dataset_id_, true);
  // /speed is already in the active set (the pend reference) — a preview must not
  // touch it.
  controller_->requestFieldPreview(dataset_id_, u"/speed"_s);
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/speed"_s));

  // Prove no self-releasing preview snuck in: after a full timeout window the
  // pend's reference still holds.
  controller_->setPreviewTimeoutMsForTest(50);
  spinFor(200);
  EXPECT_TRUE(referencesTopic(tracker, dataset_id_, u"/speed"_s))
      << "requestFieldPreview must not add a preview that later releases the pend's reference";
}

TEST_F(TopicDemandControllerTest, CensusCapsConcurrentPreviewsThenRunsTheRest) {
  auto& catalog = app_session_->catalogModel();
  auto& tracker = app_session_->topicDemandTracker();
  catalog.setPerTopicPauseCapable(dataset_id_, true);
  controller_->setPreviewTimeoutMsForTest(50);

  std::vector<PJ::AdvertisedTopic> topics;
  for (int i = 0; i < 12; ++i) {
    topics.push_back(PJ::AdvertisedTopic{u"/scalar_%1"_s.arg(i), PJ::sdk::BuiltinObjectType::kNone});
  }
  catalog.setAdvertisedTopics(dataset_id_, topics);

  // Exactly the concurrency cap of previews hold a reference at once; the other 4
  // wait in the queue.
  EXPECT_EQ(tracker.activeTopics(dataset_id_).size(), 8U);

  // As the first wave times out, the queued previews start and eventually all
  // release.
  spinFor(300);
  EXPECT_EQ(tracker.activeTopics(dataset_id_).size(), 0U);
}

}  // namespace
