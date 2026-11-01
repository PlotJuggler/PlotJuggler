// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// On-demand recipes in the layout protocol (docs/derived_objects/ARCHITECTURE.md §13): <on_demand> entries save,
// parse and restore through DerivedObjects, ordered by dependency; an unchanged recipe keeps its topic, entry and
// state; a changed dependency, an upstream's or a replaced transform output, runs its readers again; the sweep
// retires what no install reclaimed, once; findings come back at their pin. Driven through a real AppSession with
// the fake script run (`test::FakeRun`).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "on_demand_test_utils.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DerivedObjects.h"
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/ResultHistory.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "pj_scripting/python_object_script.h"
#ifdef PJ_PYTHON_TEST_PROBE
#include "python_test_probe.h"
#endif

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr Timestamp kSec = 1'000'000'000;
constexpr const char* kCloudMetadata = R"({"builtin_object_type":"kPointCloud"})";

scripting::OutputSpec cloudOutput(std::string name = "cropped") {
  return {std::move(name), scripting::OutputType::kObject, sdk::BuiltinObjectType::kPointCloud};
}

/// `owner/<id>` reading `inputs` with one cloud output `cropped`; the fake runs it.
RecipeGraph::Recipe recipe(std::string id, std::vector<std::string> inputs = {"cloud"}) {
  return test::onDemandRecipe(std::move(id), std::move(inputs), {cloudOutput()});
}

std::vector<uint8_t> canonicalCloud(size_t points, Timestamp stamp) {
  return serializeBuiltinObject(sdk::BuiltinObject(test::makeCloud(points, stamp))).value();
}

// A transform class whose id is `id`, multiplying its input by `factor`.
std::string scaleBy(const std::string& id, const std::string& factor) {
  return "-- pj-script: luau\nreturn { id=\"" + id + "\", name=\"" + id +
         "\", output=\"same\",\n"
         "  create = function(p) return { calculate = function(t, v) return v * " +
         factor + " end } end }\n";
}

/// An AppSession with the dataset `source_name` (at /data/run.mcap whatever its name) holding a cloud topic `cloud` (4
/// points at 5 s, 6 points at 10 s) and a five-sample `/imu` series; its on-demand engine runs the fake.
struct Fixture {
  explicit Fixture(std::string source_name = "run.mcap") {
    dataset = session.dataEngine().createDataset(DatasetDescriptor{.source_name = std::move(source_name)}).value();
    session.setDatasetSourcePath(dataset, u"/data/run.mcap"_s);
    cloud = session.objectStore().registerTopic({dataset, "cloud", kCloudMetadata}).value();
    pushCloud(5 * kSec, 4);
    pushCloud(10 * kSec, 6);
    DataWriter writer = session.dataEngine().createWriter();
    auto handle = writer.registerScalarSeries(dataset, "/imu", NumericType::kFloat64);
    EXPECT_TRUE(handle.has_value());
    for (int index = 0; index < 5; ++index) {
      writer.appendScalar(*handle, (index + 1) * kSec, static_cast<double>(index));
    }
    EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
    app.catalogModel().rebuildFromDatastore();
  }

  void pushCloud(Timestamp t, size_t points) {
    EXPECT_TRUE(session.objectStore().pushOwned(cloud, t, canonicalCloud(points, t)).has_value());
  }
  /// A second dataset `source_name` (at /data/<source_name>) carrying a cloud topic `cloud` too.
  DatasetId addOther(const std::string& source_name = "other.mcap") {
    const DatasetId other = session.dataEngine().createDataset(DatasetDescriptor{.source_name = source_name}).value();
    session.setDatasetSourcePath(other, u"/data/"_s + QString::fromStdString(source_name));
    const ObjectTopicId topic = session.objectStore().registerTopic({other, "cloud", kCloudMetadata}).value();
    EXPECT_TRUE(session.objectStore().pushOwned(topic, 5 * kSec, canonicalCloud(3, 5 * kSec)).has_value());
    app.catalogModel().rebuildFromDatastore();
    return other;
  }
  [[nodiscard]] DerivedObjects derived() {
    return DerivedObjects(session, app.catalogModel());
  }
  [[nodiscard]] OnDemandService& onDemand() {
    return session.onDemandService();
  }
  void install(const RecipeGraph::Recipe& wanted) {
    const auto installed = onDemand().install(wanted);
    ASSERT_TRUE(installed.has_value()) << installed.error();
    app.catalogModel().rebuildFromDatastore();
  }
  /// Requests `key` at `t` and waits for its result.
  void materialize(const std::string& key, Timestamp t) {
    onDemand().requestAt(key, t);
    settle();
  }
  /// Runs what is posted, then publishes everything submitted.
  void settle() {
    QCoreApplication::processEvents();
    session.enginePool().waitIdle();
    QCoreApplication::processEvents();
  }
  [[nodiscard]] std::optional<ObjectTopicId> topic(const std::string& name) const {
    return session.objectStore().findTopic(dataset, name);
  }
  [[nodiscard]] uint64_t revision(const std::string& key) const {
    const RecipeGraph::Node* node = session.onDemandService().graph().find(key);
    return node != nullptr ? node->revision : 0;
  }
  /// The live recipes saved as a snapshot in the scope `intent` restores.
  [[nodiscard]] QDomDocument snapshot(RestoreIntent intent) {
    QDomDocument doc;
    QDomElement root = doc.createElement(u"root"_s);
    doc.appendChild(root);
    root.appendChild(
        derived().save(doc, intent == RestoreIntent::kHistory ? SnapshotScope::kHistory : SnapshotScope::kFull));
    return doc;
  }
  [[nodiscard]] QByteArray savedBlock() {
    return snapshot(RestoreIntent::kReplace).toByteArray(2);
  }
  /// Parses and restores `doc`, failing the test on any diagnostic.
  void restoreCleanly(const QDomDocument& doc, RestoreIntent intent) {
    DerivedObjects coordinator = derived();
    const DerivedSnapshot parsed = coordinator.parse(doc.documentElement(), intent);
    ASSERT_FALSE(coordinator.refusal(parsed).has_value());
    const DerivedRestoreReport report = coordinator.restore(parsed);
    EXPECT_TRUE(report.restored_all);
    for (const DerivedDiagnostic& diagnostic : report.diagnostics) {
      ADD_FAILURE() << diagnostic.id << ": " << diagnostic.message.toStdString();
    }
  }

  test::FakeRun fake;  ///< outlives the session, whose pool joins the runs
  test::ScopedRun scoped_run{fake};
  QTemporaryDir dir;
  AppSession app{dir.path()};
  SessionManager& session = app.sessionManager();
  DatasetId dataset = 0;
  ObjectTopicId cloud;
};

QDomDocument parseXml(const QString& xml) {
  QDomDocument doc;
  EXPECT_TRUE(static_cast<bool>(doc.setContent(xml)));
  return doc;
}

/// `key` is installed with its first input bound to `upstream`'s output, and no input missing.
void expectReadsFrom(const RecipeGraph& graph, const std::string& key, const std::string& upstream) {
  const RecipeGraph::Node* reader = graph.find(key);
  ASSERT_NE(reader, nullptr) << key;
  EXPECT_EQ(reader->bindings.front().upstream, upstream);
  EXPECT_FALSE(graph.missingInput(key).has_value());
}

/// Removes the dataset a generic layout does not keep from every input of `doc`.
void removeInputDatasets(QDomDocument& doc) {
  const QDomNodeList inputs = doc.elementsByTagName(u"input"_s);
  for (int index = 0; index < inputs.size(); ++index) {
    QDomElement input = inputs.at(index).toElement();
    input.removeAttribute(u"dataset_id"_s);
    input.removeAttribute(u"dataset_source"_s);
  }
}

/// The diagnostics of `report` with id "on-demand-restore-failed".
std::vector<QString> onDemandFailures(const DerivedRestoreReport& report) {
  std::vector<QString> messages;
  for (const DerivedDiagnostic& diagnostic : report.diagnostics) {
    if (std::string_view(diagnostic.id) == "on-demand-restore-failed") {
      messages.push_back(diagnostic.message);
    }
  }
  return messages;
}

// Every attribute the <on_demand> format carries (owner, id, exemption, language, query clock, pin, inputs with
// their dataset, typed outputs, params, script) survives save -> restore into a fresh session -> save. A producer
// is written before its reader, whatever their keys sort as; a preview is not written.
TEST(DerivedObjectsTest, OnDemandRoundTripKeepsTheLayoutFormat) {
  QByteArray first_save;
  {
    Fixture source;
    RecipeGraph::Recipe producer = recipe("zz_crop", {"cloud", "/imu/value"});
    producer.params_json = R"({"k":1})";
    producer.outputs.push_back({"count", scripting::OutputType::kNumber, {}});
    source.install(producer);
    RecipeGraph::Recipe reader = recipe("aa_reader", {"owner/zz_crop/cropped"});
    reader.query_time = false;
    source.install(reader);
    RecipeGraph::Recipe finding = recipe("finding");
    finding.owner_plugin = "assistant";
    finding.pinned_t = 5 * kSec;
    finding.history_exempt = true;
    source.install(finding);
    RecipeGraph::Recipe preview = recipe("preview");
    preview.ephemeral = true;
    source.install(preview);
    first_save = source.savedBlock();
    EXPECT_EQ(source.snapshot(RestoreIntent::kHistory).toByteArray().count("<on_demand "), 2)
        << "a history snapshot omits the exempt finding";
  }
  EXPECT_EQ(first_save.count("<on_demand "), 3) << "the preview is not persisted";
  EXPECT_FALSE(first_save.contains("infer_outputs"));
  EXPECT_FALSE(first_save.contains("label"));
  EXPECT_TRUE(first_save.contains(R"(pinned_t_ns="5000000000")"));
  EXPECT_TRUE(first_save.contains(R"(query_time="0")"));
  EXPECT_TRUE(first_save.contains(R"(history_exempt="1")"));
  EXPECT_TRUE(first_save.contains(R"(type="kPointCloud")"));
  EXPECT_TRUE(first_save.contains(R"(type="number")"));
  EXPECT_TRUE(first_save.contains(R"(dataset_source="run.mcap")"));
  EXPECT_LT(first_save.indexOf(R"(id="zz_crop")"), first_save.indexOf(R"(id="aa_reader")"))
      << "a producer is saved before its reader";

  Fixture target;
  QDomDocument doc;
  ASSERT_TRUE(static_cast<bool>(doc.setContent(first_save)));
  target.restoreCleanly(doc, RestoreIntent::kReplace);
  const RecipeGraph::Node* reader = target.onDemand().graph().find("owner/aa_reader");
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->bindings.front().upstream, "owner/zz_crop");
  EXPECT_EQ(target.savedBlock(), first_save);
}

// LAY-09: a reader written before its producer still restores, bound to it.
TEST(DerivedObjectsTest, OnDemandRestoreOrdersByDependency) {
  Fixture fixture;
  const QDomDocument doc = parseXml(
      uR"(<root><data_processors>
  <on_demand owner_plugin="owner" id="reader" language="luau" query_time="1">
    <input name="owner/crop/cropped" dataset_source="run.mcap" dataset_path="/data/run.mcap"/>
    <output name="cropped" type="kPointCloud"/>
    <script><![CDATA[local inputs = ...
return {}
]]></script>
  </on_demand>
  <on_demand owner_plugin="owner" id="crop" language="luau" query_time="1">
    <input name="cloud" dataset_source="run.mcap" dataset_path="/data/run.mcap"/>
    <output name="cropped" type="kPointCloud"/>
    <script><![CDATA[local inputs = ...
return {}
]]></script>
  </on_demand>
</data_processors></root>)"_s);

  const DerivedSnapshot parsed = fixture.derived().parse(doc.documentElement(), RestoreIntent::kReplace);
  ASSERT_EQ(parsed.on_demand.size(), 2U);
  EXPECT_EQ(parsed.on_demand[0].recipe.key(), "owner/crop") << "producers first";
  EXPECT_EQ(parsed.on_demand[1].recipe.key(), "owner/reader");
  fixture.restoreCleanly(doc, RestoreIntent::kReplace);
  const RecipeGraph::Node* reader = fixture.onDemand().graph().find("owner/reader");
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->bindings.front().upstream, "owner/crop");
  EXPECT_EQ(reader->dataset, fixture.dataset);
}

// A chain whose reader names its producer's output with the dataset qualifier (what the Transform Editor writes) binds
// in the session on that dataset only, and survives save -> restore -> save: the producer is written first whatever
// the keys sort as, and the restored reader is bound to it again.
TEST(DerivedObjectsTest, AQualifiedUpstreamInputRoundTrips) {
  QByteArray first_save;
  {
    Fixture source;
    source.install(recipe("zz_crop"));
    source.addOther();
    const auto elsewhere = source.onDemand().install(recipe("elsewhere", {"other.mcap:owner/zz_crop/cropped"}));
    ASSERT_FALSE(elsewhere.has_value()) << "the qualifier names a dataset the producer is not on";
    EXPECT_NE(elsewhere.error().find("names another dataset"), std::string::npos) << elsewhere.error();
    source.install(recipe("aa_reader", {"run.mcap:owner/zz_crop/cropped"}));
    const RecipeGraph::Node* reader = source.onDemand().graph().find("owner/aa_reader");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->bindings.front().upstream, "owner/zz_crop");
    EXPECT_EQ(reader->dataset, source.dataset);
    first_save = source.savedBlock();
  }
  EXPECT_TRUE(first_save.contains(R"(name="run.mcap:owner/zz_crop/cropped")"));
  EXPECT_TRUE(first_save.contains(R"(upstream="owner/zz_crop")"));
  EXPECT_LT(first_save.indexOf(R"(id="zz_crop")"), first_save.indexOf(R"(id="aa_reader")"))
      << "a producer is saved before its reader";

  Fixture target;
  QDomDocument doc;
  ASSERT_TRUE(static_cast<bool>(doc.setContent(first_save)));
  const DerivedSnapshot parsed = target.derived().parse(doc.documentElement(), RestoreIntent::kReplace);
  ASSERT_EQ(parsed.on_demand.size(), 2U);
  EXPECT_TRUE(parsed.on_demand[1].parse_error.isEmpty()) << parsed.on_demand[1].parse_error.toStdString();
  target.restoreCleanly(doc, RestoreIntent::kReplace);
  expectReadsFrom(target.onDemand().graph(), "owner/aa_reader", "owner/zz_crop");
  EXPECT_EQ(target.savedBlock(), first_save);
}

// A layout listing a qualified reader before its producer restores the producer first: the recorded upstream names
// the edge the qualified input does not spell bare.
TEST(DerivedObjectsTest, AQualifiedReaderIsRestoredAfterItsProducer) {
  Fixture fixture;
  const QDomDocument doc = parseXml(
      uR"(<root><data_processors>
  <on_demand owner_plugin="owner" id="reader" language="luau" query_time="1">
    <input name="run.mcap:owner/crop/cropped" dataset_source="run.mcap" upstream="owner/crop" type="kPointCloud"/>
    <output name="cropped" type="kPointCloud"/>
    <script><![CDATA[local inputs = ...
return {}
]]></script>
  </on_demand>
  <on_demand owner_plugin="owner" id="crop" language="luau" query_time="1">
    <input name="cloud" dataset_source="run.mcap"/>
    <output name="cropped" type="kPointCloud"/>
    <script><![CDATA[local inputs = ...
return {}
]]></script>
  </on_demand>
</data_processors></root>)"_s);

  const DerivedSnapshot parsed = fixture.derived().parse(doc.documentElement(), RestoreIntent::kReplace);
  ASSERT_EQ(parsed.on_demand.size(), 2U);
  EXPECT_EQ(parsed.on_demand[0].recipe.key(), "owner/crop") << "producers first";
  EXPECT_EQ(parsed.on_demand[1].recipe.key(), "owner/reader");
  EXPECT_TRUE(parsed.on_demand[1].parse_error.isEmpty()) << parsed.on_demand[1].parse_error.toStdString();
  fixture.restoreCleanly(doc, RestoreIntent::kReplace);
  expectReadsFrom(fixture.onDemand().graph(), "owner/reader", "owner/crop");
}

// A generic layout drops the dataset each <input> records and keeps the qualifier in its name.
TEST(DerivedObjectsTest, AQualifiedChainRestoresFromAGenericLayout) {
  QDomDocument doc;
  {
    Fixture source;
    source.install(recipe("crop", {"run.mcap:cloud"}));
    source.install(recipe("reader", {"run.mcap:owner/crop/cropped"}));
    doc = source.snapshot(RestoreIntent::kReplace);
  }
  removeInputDatasets(doc);

  Fixture target;
  target.restoreCleanly(doc, RestoreIntent::kReplace);
  expectReadsFrom(target.onDemand().graph(), "owner/reader", "owner/crop");
}

// A generic layout opened on another recording: the old qualifier stays the alias the script reads, and each input
// binds to what it was recorded reading.
TEST(DerivedObjectsTest, AQualifiedChainRestoresOnAnotherRecording) {
  QDomDocument doc;
  {
    Fixture source;
    source.install(recipe("crop", {"run.mcap:cloud"}));
    source.install(recipe("reader", {"run.mcap:owner/crop/cropped"}));
    doc = source.snapshot(RestoreIntent::kReplace);
  }
  removeInputDatasets(doc);

  Fixture target("next.mcap");
  target.restoreCleanly(doc, RestoreIntent::kReplace);
  const RecipeGraph& graph = target.onDemand().graph();
  expectReadsFrom(graph, "owner/reader", "owner/crop");
  const RecipeGraph::Node* crop = graph.find("owner/crop");
  ASSERT_NE(crop, nullptr);
  EXPECT_EQ(crop->recipe.inputs.front(), "run.mcap:cloud");
  EXPECT_EQ(crop->bindings.front().name, "cloud");
  EXPECT_EQ(crop->dataset, target.dataset);

  target.materialize("owner/crop", 5 * kSec);
  const std::optional<ObjectTopicId> output = target.topic("owner/crop/cropped");
  ASSERT_TRUE(output.has_value());
  EXPECT_TRUE(target.session.objectStore().latestAt(*output, 5 * kSec).has_value());
}

// The old recording is loaded too, without the topic: its name in the qualifier does not take the chain away.
TEST(DerivedObjectsTest, AStaleQualifierNamingALoadedDatasetKeepsTheChain) {
  QDomDocument doc;
  {
    Fixture source;
    source.install(recipe("crop", {"run.mcap:cloud"}));
    source.install(recipe("reader", {"run.mcap:owner/crop/cropped"}));
    doc = source.snapshot(RestoreIntent::kReplace);
  }
  removeInputDatasets(doc);

  Fixture target("next.mcap");
  ASSERT_TRUE(target.session.dataEngine().createDataset(DatasetDescriptor{.source_name = "run.mcap"}).has_value());
  target.app.catalogModel().rebuildFromDatastore();
  target.restoreCleanly(doc, RestoreIntent::kReplace);
  expectReadsFrom(target.onDemand().graph(), "owner/reader", "owner/crop");
  EXPECT_EQ(target.onDemand().graph().find("owner/crop")->dataset, target.dataset);
}

// The old recording, now loaded with that topic, must not take an undo away from the dataset the snapshot names.
TEST(DerivedObjectsTest, ASnapshotKeepsItsDatasetWhenTheOldRecordingLoadsLater) {
  QDomDocument doc;
  {
    Fixture source;
    source.install(recipe("crop", {"run.mcap:cloud"}));
    source.install(recipe("reader", {"run.mcap:owner/crop/cropped"}));
    doc = source.snapshot(RestoreIntent::kReplace);
  }
  removeInputDatasets(doc);

  Fixture target("next.mcap");
  target.restoreCleanly(doc, RestoreIntent::kReplace);
  const QDomDocument snapshot = target.snapshot(RestoreIntent::kHistory);
  target.addOther("run.mcap");
  ASSERT_FALSE(target.onDemand().remove("owner/reader").empty());
  ASSERT_FALSE(target.onDemand().remove("owner/crop").empty());

  target.restoreCleanly(snapshot, RestoreIntent::kHistory);
  expectReadsFrom(target.onDemand().graph(), "owner/reader", "owner/crop");
  EXPECT_EQ(target.onDemand().graph().find("owner/crop")->dataset, target.dataset);
}

// A merge keeps a recipe a generic layout bound by uniqueness on the anchor, whoever else carries its topic by then.
TEST(DerivedObjectsTest, AMergeKeepsAGenericRecipeOnTheAnchor) {
  QDomDocument doc;
  {
    Fixture source;
    source.install(recipe("crop", {"run.mcap:cloud"}));
    doc = source.snapshot(RestoreIntent::kReplace);
  }
  removeInputDatasets(doc);

  Fixture target("next.mcap");
  target.restoreCleanly(doc, RestoreIntent::kReplace);
  const DatasetId third = target.addOther("third.mcap");
  target.addOther("fourth.mcap");
  ASSERT_TRUE(target.session.mergeDatasets(target.dataset, {{third, 0}}).has_value());
  target.settle();

  const RecipeGraph::Node* crop = target.onDemand().graph().find("owner/crop");
  ASSERT_NE(crop, nullptr);
  EXPECT_EQ(crop->dataset, target.dataset);
}

// Saving again on that recording keeps what the input reads, so the layout survives a second move.
TEST(DerivedObjectsTest, AQualifiedInputKeepsItsTopicAcrossASecondRecording) {
  QDomDocument doc;
  {
    Fixture source;
    source.install(recipe("crop", {"run.mcap:cloud"}));
    doc = source.snapshot(RestoreIntent::kReplace);
  }
  removeInputDatasets(doc);
  {
    Fixture second("second.mcap");
    second.restoreCleanly(doc, RestoreIntent::kReplace);
    doc = second.snapshot(RestoreIntent::kReplace);
  }
  removeInputDatasets(doc);

  Fixture third("third.mcap");
  third.restoreCleanly(doc, RestoreIntent::kReplace);
  EXPECT_NE(third.onDemand().graph().find("owner/crop"), nullptr);
}

// After a merge the input's qualifier names the consumed dataset, not the source saved beside it.
TEST(DerivedObjectsTest, AGenericLayoutSavedAfterAMergeRestoresOnAnotherRecording) {
  QDomDocument doc;
  {
    Fixture source;
    const DatasetId other = source.addOther();
    source.install(recipe("crop", {"other.mcap:cloud"}));
    source.install(recipe("reader", {"other.mcap:owner/crop/cropped"}));
    ASSERT_TRUE(source.session.mergeDatasets(source.dataset, {{other, 0}}).has_value());
    source.settle();
    doc = source.snapshot(RestoreIntent::kReplace);
  }
  removeInputDatasets(doc);

  Fixture target("next.mcap");
  target.restoreCleanly(doc, RestoreIntent::kReplace);
  const RecipeGraph::Node* crop = target.onDemand().graph().find("owner/crop");
  ASSERT_NE(crop, nullptr);
  EXPECT_EQ(crop->bindings.front().name, "cloud");
  expectReadsFrom(target.onDemand().graph(), "owner/reader", "owner/crop");
}

// After a merge an <input> records the anchor's source while its name keeps the consumed dataset's.
TEST(DerivedObjectsTest, AHistorySnapshotAfterAMergeKeepsAQualifiedReader) {
  Fixture fixture;
  const DatasetId other = fixture.addOther();
  fixture.install(recipe("crop", {"other.mcap:cloud"}));
  fixture.install(recipe("reader", {"other.mcap:owner/crop/cropped"}));
  ASSERT_TRUE(fixture.session.mergeDatasets(fixture.dataset, {{other, 0}}).has_value());
  fixture.settle();

  fixture.restoreCleanly(fixture.snapshot(RestoreIntent::kHistory), RestoreIntent::kHistory);
  expectReadsFrom(fixture.onDemand().graph(), "owner/reader", "owner/crop");
}

// LAY-10: a cycle, and a reader of one, are reported "cyclic"; the independent recipe beside them restores.
TEST(DerivedObjectsTest, CyclesAreDiagnosedWithoutDroppingIndependentRecipes) {
  Fixture fixture;
  const QDomDocument doc = parseXml(
      uR"(<root><data_processors>
  <on_demand owner_plugin="owner" id="a"><input name="owner/b/out"/>
    <output name="out" type="kPointCloud"/><script>return {}</script></on_demand>
  <on_demand owner_plugin="owner" id="independent"><input name="cloud"/>
    <output name="out" type="kPointCloud"/><script>return {}</script></on_demand>
  <on_demand owner_plugin="owner" id="b"><input name="owner/a/out"/>
    <output name="out" type="kPointCloud"/><script>return {}</script></on_demand>
  <on_demand owner_plugin="owner" id="blocked"><input name="owner/a/out"/>
    <output name="out" type="kPointCloud"/><script>return {}</script></on_demand>
</data_processors></root>)"_s);

  DerivedObjects derived = fixture.derived();
  const DerivedSnapshot parsed = derived.parse(doc.documentElement(), RestoreIntent::kReplace);
  ASSERT_EQ(parsed.on_demand.size(), 4U) << "every entry is kept, the cyclic ones with their error";
  EXPECT_EQ(parsed.on_demand.front().recipe.key(), "owner/independent");
  EXPECT_TRUE(parsed.on_demand.front().parse_error.isEmpty());
  const DerivedRestoreReport report = derived.restore(parsed);
  EXPECT_FALSE(report.restored_all);
  const std::vector<QString> failures = onDemandFailures(report);
  for (const QString& failure : failures) {
    EXPECT_TRUE(failure.contains(u"cyclic"_s)) << failure.toStdString();
  }
  ASSERT_EQ(failures.size(), 3U);
  EXPECT_NE(fixture.onDemand().graph().find("owner/independent"), nullptr);
  EXPECT_EQ(fixture.onDemand().graph().find("owner/a"), nullptr);
  EXPECT_EQ(fixture.onDemand().graph().find("owner/blocked"), nullptr);
}

// LAY-07: an unchanged recipe is not re-created by a restore under either intent: same revision, same topic id.
TEST(DerivedObjectsTest, UnchangedOnDemandRecipeKeepsItsTopicId) {
  for (const RestoreIntent intent : {RestoreIntent::kReplace, RestoreIntent::kHistory}) {
    SCOPED_TRACE(intent == RestoreIntent::kReplace ? "kReplace" : "kHistory");
    Fixture fixture;
    fixture.install(recipe("crop"));
    const std::optional<ObjectTopicId> before = fixture.topic("owner/crop/cropped");
    ASSERT_TRUE(before.has_value());
    const uint64_t revision = fixture.revision("owner/crop");

    fixture.restoreCleanly(fixture.snapshot(intent), intent);

    EXPECT_EQ(fixture.revision("owner/crop"), revision);
    EXPECT_EQ(fixture.topic("owner/crop/cropped"), before);
  }
}

// An unchanged restore costs nothing: no script run, no new entry (the displayed one keeps its uid), no ring bytes,
// the node's state as it was.
TEST(DerivedObjectsTest, UnchangedRecipeKeepsItsEntryAndRunsNothing) {
  for (const RestoreIntent intent : {RestoreIntent::kReplace, RestoreIntent::kHistory}) {
    SCOPED_TRACE(intent == RestoreIntent::kReplace ? "kReplace" : "kHistory");
    Fixture fixture;
    fixture.install(recipe("crop"));
    fixture.install(recipe("reader", {"owner/crop/cropped"}));
    fixture.materialize("owner/reader", 5 * kSec);
    const int runs = fixture.fake.started();
    ASSERT_EQ(runs, 2) << "the reader and its upstream ran once";
    const ObjectTopicId topic = *fixture.topic("owner/reader/cropped");
    const auto entry = fixture.session.objectStore().latestAt(topic, 5 * kSec);
    ASSERT_TRUE(entry.has_value());
    const std::size_t ring_bytes = fixture.onDemand().history().ringBytes();
    const auto state = fixture.onDemand().state("owner/reader");
    ASSERT_TRUE(state.has_value());

    fixture.restoreCleanly(fixture.snapshot(intent), intent);
    fixture.settle();

    EXPECT_EQ(fixture.fake.started(), runs) << "nothing ran";
    const auto after = fixture.session.objectStore().latestAt(topic, 5 * kSec);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->sequential_uid, entry->sequential_uid) << "the same entry, not a republished one";
    EXPECT_EQ(fixture.onDemand().history().ringBytes(), ring_bytes);
    EXPECT_EQ(fixture.onDemand().state("owner/reader")->status, state->status);
    EXPECT_EQ(fixture.onDemand().state("owner/reader")->requested_t, state->requested_t);
  }
}

// LAY-08: a layout written by the prototype carries `infer_outputs` and `label`; both are read and ignored, so the
// entry describes the live recipe unchanged, and a save does not write them back.
TEST(DerivedObjectsTest, InferOutputsAttributeIsIgnoredAndIsNotAChange) {
  Fixture fixture;
  fixture.install(recipe("crop"));
  const uint64_t revision = fixture.revision("owner/crop");
  QDomDocument doc = fixture.snapshot(RestoreIntent::kReplace);
  QDomElement entry = doc.documentElement().firstChildElement(u"data_processors"_s).firstChildElement(u"on_demand"_s);
  ASSERT_FALSE(entry.isNull());
  entry.setAttribute(u"infer_outputs"_s, u"1"_s);
  entry.setAttribute(u"label"_s, u"Crop box"_s);

  fixture.restoreCleanly(doc, RestoreIntent::kReplace);

  EXPECT_EQ(fixture.revision("owner/crop"), revision) << "not a change";
  const QByteArray saved = fixture.savedBlock();
  EXPECT_FALSE(saved.contains("infer_outputs"));
  EXPECT_FALSE(saved.contains("label"));
}

// LAY-01: the clear keeps the cleared nodes' topics until the sweep; a reinstall under the same output reclaims its
// id, a node no install reclaims has its topic retired, and the restore announces the change once. A restore that
// changes nothing announces nothing.
TEST(DerivedObjectsTest, SweepAfterRestoreRetiresUnclaimedTopicsOnce) {
  Fixture fixture;
  fixture.install(recipe("edited"));
  fixture.install(recipe("dropped"));
  const ObjectTopicId edited = *fixture.topic("owner/edited/cropped");
  const ObjectTopicId dropped = *fixture.topic("owner/dropped/cropped");
  QDomDocument doc = parseXml(
      uR"(<root><data_processors>
  <on_demand owner_plugin="owner" id="edited" language="luau" query_time="1">
    <input name="cloud" dataset_source="run.mcap"/>
    <output name="cropped" type="kPointCloud"/>
    <params><![CDATA[{"zmin":-1}]]></params>
    <script><![CDATA[local inputs = ...
return {}
]]></script>
  </on_demand>
  <on_demand owner_plugin="owner" id="added" language="luau" query_time="1">
    <input name="cloud" dataset_source="run.mcap"/>
    <output name="cropped" type="kPointCloud"/>
    <script><![CDATA[local inputs = ...
return {}
]]></script>
  </on_demand>
</data_processors></root>)"_s);
  QSignalSpy announced(&fixture.session, &SessionManager::objectTopicsChanged);

  fixture.restoreCleanly(doc, RestoreIntent::kReplace);

  EXPECT_EQ(announced.count(), 1);
  EXPECT_EQ(fixture.topic("owner/edited/cropped"), edited) << "reclaimed by the reinstall";
  EXPECT_NE(fixture.onDemand().graph().find("owner/edited")->recipe.params_json.find("zmin"), std::string::npos);
  EXPECT_FALSE(fixture.topic("owner/dropped/cropped").has_value()) << "unclaimed: retired";
  EXPECT_TRUE(fixture.session.objectStore().descriptor(dropped).topic_name.empty());
  EXPECT_TRUE(fixture.topic("owner/added/cropped").has_value());

  announced.clear();
  fixture.restoreCleanly(fixture.snapshot(RestoreIntent::kReplace), RestoreIntent::kReplace);
  EXPECT_EQ(announced.count(), 0) << "an unchanged restore announces nothing";
}

// LAY-11: history may not take away an output a protected (exempt) on-demand recipe reads. Removing the producer,
// or renaming the output it reads, is refused before anything changes; an unchanged snapshot, or one that edits
// the producer and keeps the output, is not.
TEST(DerivedObjectsTest, HistoryCannotRemoveProducerOfProtectedOnDemandConsumer) {
  Fixture fixture;
  fixture.install(recipe("crop"));
  RecipeGraph::Recipe finding = recipe("finding", {"owner/crop/cropped"});
  finding.owner_plugin = "assistant";
  finding.history_exempt = true;
  fixture.install(finding);
  DerivedObjects derived = fixture.derived();
  const QDomDocument unchanged = fixture.snapshot(RestoreIntent::kHistory);

  const std::optional<QString> refused = derived.refusal(
      derived.parse(parseXml(u"<root><data_processors/></root>"_s).documentElement(), RestoreIntent::kHistory));
  ASSERT_TRUE(refused.has_value());
  EXPECT_TRUE(refused->contains(u"protected"_s)) << refused->toStdString();
  EXPECT_TRUE(refused->contains(u"assistant/finding/cropped"_s)) << refused->toStdString();
  EXPECT_NE(fixture.onDemand().graph().find("owner/crop"), nullptr);

  QDomDocument renamed = unchanged.cloneNode(true).toDocument();
  QDomElement output = renamed.documentElement()
                           .firstChildElement(u"data_processors"_s)
                           .firstChildElement(u"on_demand"_s)
                           .firstChildElement(u"output"_s);
  output.setAttribute(u"name"_s, u"other"_s);
  EXPECT_TRUE(derived.refusal(derived.parse(renamed.documentElement(), RestoreIntent::kHistory)).has_value())
      << "the output it reads goes away";

  QDomDocument edited = unchanged.cloneNode(true).toDocument();
  QDomElement params = edited.createElement(u"params"_s);
  params.appendChild(edited.createCDATASection(u"{\"zmin\":2}"_s));
  edited.documentElement()
      .firstChildElement(u"data_processors"_s)
      .firstChildElement(u"on_demand"_s)
      .appendChild(params);
  EXPECT_FALSE(derived.refusal(derived.parse(edited.documentElement(), RestoreIntent::kHistory)).has_value())
      << "an edit keeps the output: the protected reader runs on the new result";
  EXPECT_FALSE(derived.refusal(derived.parse(unchanged.documentElement(), RestoreIntent::kHistory)).has_value());
}

// LAY-21: an unchanged recipe reading a transform output the restore re-creates runs again; one reading an
// output the restore leaves alone is kept.
TEST(DerivedObjectsTest, AnOnDemandReaderOfAReplacedTransformOutputRunsAgain) {
  Fixture fixture;
  DataProcessorService& processors = fixture.session.dataProcessorService();
  ASSERT_TRUE(
      processors.upsertTransform("tool", "scale", {"/imu"}, {"scaled"}, scaleBy("scale", "10.0"), "{}").has_value());
  fixture.app.catalogModel().rebuildFromDatastore();
  fixture.install(recipe("crop", {"cloud", "scaled/value"}));
  fixture.install(recipe("plain"));
  const uint64_t crop_revision = fixture.revision("owner/crop");
  const uint64_t plain_revision = fixture.revision("owner/plain");

  fixture.restoreCleanly(fixture.snapshot(RestoreIntent::kReplace), RestoreIntent::kReplace);
  EXPECT_EQ(fixture.revision("owner/crop"), crop_revision) << "the transform is unchanged: kept";

  // The same snapshot with the transform's script edited: the restore re-creates its output.
  QDomDocument doc = fixture.snapshot(RestoreIntent::kReplace);
  QDomElement script = doc.documentElement()
                           .firstChildElement(u"data_processors"_s)
                           .firstChildElement(u"transform"_s)
                           .firstChildElement(u"script"_s);
  script.removeChild(script.firstChild());
  script.appendChild(doc.createCDATASection(QString::fromStdString(scaleBy("scale", "100.0"))));
  fixture.restoreCleanly(doc, RestoreIntent::kReplace);

  EXPECT_NE(fixture.revision("owner/crop"), crop_revision) << "a reader of the re-created output runs again";
  EXPECT_EQ(fixture.revision("owner/plain"), plain_revision) << "a recipe not reading it is kept";
}

// PIN-07: a finding restored into a session is requested at its pin by the install, against the data there now
// (here a sample that arrived after the save, still before the pin); a kept finding keeps its entry and runs
// nothing.
TEST(DerivedObjectsTest, PinnedRecipeRematerializesAtItsPinOnRestore) {
  QDomDocument saved;
  {
    Fixture source;
    RecipeGraph::Recipe finding = recipe("finding");
    finding.pinned_t = 9 * kSec;
    source.install(finding);
    source.settle();
    ASSERT_EQ(source.fake.started(), 1) << "the install requests a finding at its pin";
    const ObjectTopicId topic = *source.topic("owner/finding/cropped");
    const auto entry = source.session.objectStore().latestAt(topic, kFindingTimestamp);
    ASSERT_TRUE(entry.has_value());

    source.restoreCleanly(source.snapshot(RestoreIntent::kReplace), RestoreIntent::kReplace);
    source.settle();
    EXPECT_EQ(source.fake.started(), 1) << "a kept finding runs nothing";
    EXPECT_EQ(source.session.objectStore().latestAt(topic, kFindingTimestamp)->sequential_uid, entry->sequential_uid);
    saved = source.snapshot(RestoreIntent::kReplace);
  }

  Fixture target;
  target.pushCloud(8 * kSec, 7);  // after the save, before the pin
  target.restoreCleanly(saved, RestoreIntent::kReplace);
  target.settle();

  ASSERT_EQ(target.fake.started(), 1);
  EXPECT_EQ(target.fake.records().front().query_time, 9 * kSec) << "at its pin";
  const ObjectTopicId topic = *target.topic("owner/finding/cropped");
  EXPECT_EQ(test::timestampsOf(target.session.objectStore(), topic), std::vector<Timestamp>{kFindingTimestamp});
  EXPECT_EQ(test::decodeAt<sdk::PointCloud>(target.session.objectStore(), topic, 0).width, 7U)
      << "computed from the sample at 8 s";
}

// OWN-01: a preview is outside every restore's authority: an empty snapshot clears its persistent sibling under
// both intents and leaves the preview, its topic and its result.
TEST(DerivedObjectsTest, PreviewSurvivesBothRestoreIntents) {
  for (const RestoreIntent intent : {RestoreIntent::kReplace, RestoreIntent::kHistory}) {
    SCOPED_TRACE(intent == RestoreIntent::kReplace ? "kReplace" : "kHistory");
    Fixture fixture;
    RecipeGraph::Recipe preview = recipe("preview");
    preview.ephemeral = true;
    fixture.install(preview);
    fixture.install(recipe("persistent"));
    fixture.materialize("owner/preview", 5 * kSec);
    const ObjectTopicId topic = *fixture.topic("owner/preview/cropped");
    const uint64_t revision = fixture.revision("owner/preview");

    fixture.restoreCleanly(parseXml(u"<root><data_processors/></root>"_s), intent);

    EXPECT_EQ(fixture.onDemand().graph().find("owner/persistent"), nullptr);
    EXPECT_FALSE(fixture.topic("owner/persistent/cropped").has_value());
    EXPECT_EQ(fixture.revision("owner/preview"), revision);
    EXPECT_EQ(fixture.topic("owner/preview/cropped"), topic);
    EXPECT_TRUE(fixture.session.objectStore().latestAt(topic, 5 * kSec).has_value()) << "its result stays";
  }
}

// A malformed entry is reported, naming the recipe and the reason, and skipped: a pin that is not an integer, a
// query clock that is neither 0 nor 1, an unknown output type, an input dataset id that is not a number, an input
// dataset path that is not loaded. The valid entry beside them restores.
TEST(DerivedObjectsTest, MalformedPinTimeAndWrongXmlTypesAreDiagnosed) {
  Fixture fixture;
  const auto entry = [](const QString& id, const QString& attributes, const QString& input_attributes,
                        const QString& type) {
    return uR"(<on_demand owner_plugin="owner" id="%1" %2><input name="cloud" %3/><output name="cropped" type="%4"/>
<script><![CDATA[local inputs = ...
return {}
]]></script></on_demand>)"_s.arg(id, attributes, input_attributes, type);
  };
  const QDomDocument doc = parseXml(
      u"<root><data_processors>"_s + entry(u"pin"_s, u"pinned_t_ns=\"soon\""_s, {}, u"kPointCloud"_s) +
      entry(u"clock"_s, u"query_time=\"yes\""_s, {}, u"kPointCloud"_s) + entry(u"type"_s, {}, {}, u"kBogus"_s) +
      entry(u"id"_s, {}, u"dataset_id=\"x\" dataset_path=\"/data/run.mcap\""_s, u"kPointCloud"_s) +
      entry(u"path"_s, {}, u"dataset_path=\"/elsewhere/other.mcap\""_s, u"kPointCloud"_s) +
      entry(u"valid"_s, u"pinned_t_ns=\"-5\""_s, {}, u"kPointCloud"_s) + u"</data_processors></root>"_s);

  DerivedObjects derived = fixture.derived();
  const DerivedRestoreReport report = derived.restore(derived.parse(doc.documentElement(), RestoreIntent::kReplace));

  EXPECT_FALSE(report.restored_all);
  const std::vector<QString> failures = onDemandFailures(report);
  ASSERT_EQ(failures.size(), 5U);
  const std::vector<std::pair<QString, QString>> expected{
      {u"owner/pin"_s, u"invalid pinned time"_s},
      {u"owner/clock"_s, u"invalid query_time"_s},
      {u"owner/type"_s, u"invalid output 'cropped'"_s},
      {u"owner/id"_s, u"invalid input dataset id"_s},
      {u"owner/path"_s, u"input dataset is not loaded"_s}};
  for (const auto& [key, reason] : expected) {
    EXPECT_TRUE(
        std::any_of(
            failures.begin(), failures.end(),
            [&](const QString& failure) {
              return failure.contains(u"'"_s + key + u"'"_s) && failure.contains(reason);
            }))
        << key.toStdString() << " reported with '" << reason.toStdString() << "'";
  }
  for (const char* key : {"owner/pin", "owner/clock", "owner/type", "owner/id", "owner/path"}) {
    EXPECT_EQ(fixture.onDemand().graph().find(key), nullptr) << key;
  }
  const RecipeGraph::Node* valid = fixture.onDemand().graph().find("owner/valid");
  ASSERT_NE(valid, nullptr);
  EXPECT_EQ(valid->recipe.pinned_t, -5) << "a negative raw time is a time";
}

/// `<root><data_processors>` holding one `<on_demand owner_plugin="owner">` entry per id, each reading `cloud` of the
/// dataset its input attributes name, with `test::onDemandRecipe`'s script.
QDomDocument onDemandEntries(const std::vector<QString>& ids, const QString& input_attributes) {
  QString entries;
  for (const QString& id : ids) {
    entries +=
        uR"(<on_demand owner_plugin="owner" id="%1"><input name="cloud" %2/>
<output name="cropped" type="kPointCloud"/><script><![CDATA[local inputs = ...
return {}
]]></script></on_demand>)"_s.arg(id, input_attributes);
  }
  return parseXml(u"<root><data_processors>"_s + entries + u"</data_processors></root>"_s);
}

// GAP-31: the dataset a saved input names binds the restored recipe there, also when another loaded dataset carries
// the same topics (by name alone, ambiguous); a live recipe bound to the other one is not unchanged: it moves.
TEST(DerivedObjectsTest, TheSavedInputDatasetBindsTheRestoredRecipe) {
  Fixture fixture;
  const DatasetId other = fixture.addOther();
  RecipeGraph::Recipe elsewhere = recipe("elsewhere");
  elsewhere.dataset = other;
  fixture.install(elsewhere);
  ASSERT_NE(fixture.onDemand().graph().find("owner/elsewhere"), nullptr);
  ASSERT_EQ(fixture.onDemand().graph().find("owner/elsewhere")->dataset, other);

  fixture.restoreCleanly(
      onDemandEntries({u"crop"_s, u"elsewhere"_s}, uR"(dataset_source="run.mcap" dataset_path="/data/run.mcap")"_s),
      RestoreIntent::kReplace);

  ASSERT_NE(fixture.onDemand().graph().find("owner/crop"), nullptr);
  EXPECT_EQ(fixture.onDemand().graph().find("owner/crop")->dataset, fixture.dataset);
  ASSERT_NE(fixture.onDemand().graph().find("owner/elsewhere"), nullptr);
  EXPECT_EQ(fixture.onDemand().graph().find("owner/elsewhere")->dataset, fixture.dataset) << "moved to the saved one";
}

// History may not move the producer of a protected (exempt) reader to another dataset: the reader would read an
// output no longer on its dataset. A snapshot binding it elsewhere takes its outputs away (LAY-11).
TEST(DerivedObjectsTest, HistoryCannotMoveTheProducerOfAProtectedReaderToAnotherDataset) {
  Fixture fixture;
  const DatasetId other = fixture.addOther();
  RecipeGraph::Recipe producer = recipe("crop");
  producer.dataset = other;
  fixture.install(producer);
  const QDomDocument on_other = fixture.snapshot(RestoreIntent::kHistory);
  producer.dataset = fixture.dataset;
  fixture.install(producer);
  RecipeGraph::Recipe finding = recipe("finding", {"owner/crop/cropped"});
  finding.owner_plugin = "assistant";
  finding.history_exempt = true;
  fixture.install(finding);
  DerivedObjects derived = fixture.derived();

  const std::optional<QString> refused =
      derived.refusal(derived.parse(on_other.documentElement(), RestoreIntent::kHistory));
  ASSERT_TRUE(refused.has_value());
  EXPECT_TRUE(refused->contains(u"assistant/finding/cropped"_s)) << refused->toStdString();
  EXPECT_FALSE(
      derived
          .refusal(derived.parse(fixture.snapshot(RestoreIntent::kHistory).documentElement(), RestoreIntent::kHistory))
          .has_value())
      << "on its dataset still: not refused";
}

// A reader whose producer was removed stays installed, missing its input. A layout saved then restores it so (not as
// an unknown input), an undo of the removal and its redo leave it so too, and it reads its producer again once that
// comes back.
TEST(DerivedObjectsTest, AReaderOfARemovedProducerRestoresMissingItsInput) {
  QDomDocument saved;
  {
    Fixture source;
    source.install(recipe("crop"));
    source.install(recipe("reader", {"owner/crop/cropped"}));
    const QDomDocument before = source.snapshot(RestoreIntent::kHistory);
    (void)source.onDemand().remove("owner/crop");
    ASSERT_TRUE(source.onDemand().graph().missingInput("owner/reader").has_value());
    const QDomDocument after = source.snapshot(RestoreIntent::kHistory);

    source.restoreCleanly(before, RestoreIntent::kHistory);  // undo
    ASSERT_NE(source.onDemand().graph().find("owner/crop"), nullptr);
    EXPECT_FALSE(source.onDemand().graph().missingInput("owner/reader").has_value());
    source.restoreCleanly(after, RestoreIntent::kHistory);  // redo
    EXPECT_EQ(source.onDemand().graph().find("owner/crop"), nullptr);
    ASSERT_NE(source.onDemand().graph().find("owner/reader"), nullptr);
    EXPECT_TRUE(source.onDemand().graph().missingInput("owner/reader").has_value());
    saved = source.snapshot(RestoreIntent::kReplace);
  }

  Fixture target;
  target.restoreCleanly(saved, RestoreIntent::kReplace);
  ASSERT_NE(target.onDemand().graph().find("owner/reader"), nullptr);
  EXPECT_EQ(target.onDemand().graph().missingInput("owner/reader"), "on-demand: upstream 'owner/crop' removed");
  target.install(recipe("crop"));
  EXPECT_FALSE(target.onDemand().graph().missingInput("owner/reader").has_value()) << "relinked by name";
}

// A reader whose producer is installed but no longer provides the output stays installed, missing it ("no longer
// provides"). A layout saved then, and the redo of the rename, restore it so, not as an unknown input.
TEST(DerivedObjectsTest, AReaderOfAnOutputItsProducerNoLongerProvidesRestoresMissingIt) {
  const RecipeGraph::Recipe renamed = test::onDemandRecipe("crop", {"cloud"}, {cloudOutput("renamed")});
  QDomDocument saved;
  {
    Fixture source;
    source.install(recipe("crop"));
    source.install(recipe("reader", {"owner/crop/cropped"}));
    const QDomDocument before = source.snapshot(RestoreIntent::kHistory);
    source.install(renamed);
    const std::optional<std::string> missing = source.onDemand().graph().missingInput("owner/reader");
    ASSERT_TRUE(missing.has_value());
    EXPECT_NE(missing->find("no longer provides"), std::string::npos) << *missing;
    const QDomDocument after = source.snapshot(RestoreIntent::kHistory);

    source.restoreCleanly(before, RestoreIntent::kHistory);  // undo
    EXPECT_FALSE(source.onDemand().graph().missingInput("owner/reader").has_value());
    source.restoreCleanly(after, RestoreIntent::kHistory);  // redo
    ASSERT_NE(source.onDemand().graph().find("owner/reader"), nullptr);
    EXPECT_TRUE(source.onDemand().graph().missingInput("owner/reader").has_value());
    saved = source.snapshot(RestoreIntent::kReplace);
  }

  Fixture target;
  target.restoreCleanly(saved, RestoreIntent::kReplace);
  ASSERT_NE(target.onDemand().graph().find("owner/reader"), nullptr);
  const std::optional<std::string> missing = target.onDemand().graph().missingInput("owner/reader");
  ASSERT_TRUE(missing.has_value());
  EXPECT_NE(missing->find("no longer provides"), std::string::npos) << *missing;
}

// A restore that fails part-way (here a slot throwing on the first install) must not leave the service believing it
// is still restoring: the next install announces the topics it retires.
TEST(DerivedObjectsTest, AFailedRestoreDoesNotSilenceLaterInstallAnnouncements) {
  Fixture fixture;
  fixture.install(recipe("a"));
  QDomDocument doc = fixture.snapshot(RestoreIntent::kReplace);
  doc.documentElement()
      .firstChildElement(u"data_processors"_s)
      .firstChildElement(u"on_demand"_s)
      .firstChildElement(u"output"_s)
      .setAttribute(u"name"_s, u"renamed"_s);
  DerivedObjects coordinator = fixture.derived();
  const DerivedSnapshot parsed = coordinator.parse(doc.documentElement(), RestoreIntent::kReplace);
  const QMetaObject::Connection thrower = QObject::connect(
      &fixture.session, &SessionManager::onDemandRecipeInstalled, [] { throw std::runtime_error("x"); });
  EXPECT_THROW((void)coordinator.restore(parsed), std::runtime_error);
  QObject::disconnect(thrower);

  QSignalSpy announced(&fixture.session, &SessionManager::objectTopicsChanged);
  fixture.install(recipe("a"));  // back to "cropped": retires "renamed"
  EXPECT_EQ(announced.count(), 1);
}

// A restore that re-creates a transform output leaves a preview reading it, and the previews reading that one, live
// (OWN-01), and runs them again at their cursors: their results were computed from the old series.
TEST(DerivedObjectsTest, APreviewReadingAReplacedTransformOutputRunsAgain) {
  for (const RestoreIntent intent : {RestoreIntent::kReplace, RestoreIntent::kHistory}) {
    SCOPED_TRACE(intent == RestoreIntent::kReplace ? "kReplace" : "kHistory");
    Fixture fixture;
    ASSERT_TRUE(fixture.session.dataProcessorService()
                    .upsertTransform("tool", "scale", {"/imu"}, {"scaled"}, scaleBy("scale", "10.0"), "{}")
                    .has_value());
    fixture.app.catalogModel().rebuildFromDatastore();
    RecipeGraph::Recipe preview = recipe("preview", {"cloud", "scaled/value"});
    preview.ephemeral = true;
    fixture.install(preview);
    RecipeGraph::Recipe downstream = recipe("downstream", {"owner/preview/cropped"});
    downstream.ephemeral = true;
    fixture.install(downstream);
    fixture.materialize("owner/downstream", 5 * kSec);
    const int runs = fixture.fake.started();
    ASSERT_EQ(runs, 2);
    const uint64_t revision = fixture.revision("owner/preview");

    QDomDocument doc = fixture.snapshot(intent);
    QDomElement script = doc.documentElement()
                             .firstChildElement(u"data_processors"_s)
                             .firstChildElement(u"transform"_s)
                             .firstChildElement(u"script"_s);
    script.removeChild(script.firstChild());
    script.appendChild(doc.createCDATASection(QString::fromStdString(scaleBy("scale", "100.0"))));
    fixture.restoreCleanly(doc, intent);
    fixture.settle();

    EXPECT_EQ(fixture.revision("owner/preview"), revision) << "the preview is not the restore's";
    // At least: under load the reader may also run once on the old upstream result before the new one lands (D30).
    EXPECT_GE(fixture.fake.started(), runs + 2) << "both previews ran again";
  }
}

// A scalar input naming a scalar topic exactly reads that topic, not a field of a shorter topic its name also spells
// (`sensor`'s field `value`), as a transform input resolves it.
TEST(DerivedObjectsTest, AScalarInputPrefersTheExactScalarTopic) {
  Fixture fixture;
  DataWriter writer = fixture.session.dataEngine().createWriter();
  const auto sensor = writer.registerScalarSeries(fixture.dataset, "sensor", NumericType::kFloat64);
  const auto exact = writer.registerScalarSeries(fixture.dataset, "sensor/value", NumericType::kFloat64);
  ASSERT_TRUE(sensor.has_value() && exact.has_value());
  writer.appendScalar(*sensor, kSec, 1.0);
  writer.appendScalar(*exact, kSec, 2.0);
  ASSERT_FALSE(fixture.session.commitChunks(writer.flushAll()).empty());
  fixture.app.catalogModel().rebuildFromDatastore();
  fixture.fake.fail_on_number = 1.0;  // reading `sensor`'s field fails the run

  fixture.install(recipe("crop", {"cloud", "sensor/value"}));
  fixture.materialize("owner/crop", 5 * kSec);

  const auto state = fixture.onDemand().state("owner/crop");
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->status, NodeScheduler::NodeState::Status::kReady) << state->detail;
}

// A preview holding the key of a persistent recipe the snapshot wants is a conflict refused before anything changes,
// under both intents: the restore would skip the saved recipe, and the next save drop it.
TEST(DerivedObjectsTest, APreviewHoldingASavedRecipesKeyIsRefused) {
  for (const RestoreIntent intent : {RestoreIntent::kReplace, RestoreIntent::kHistory}) {
    SCOPED_TRACE(intent == RestoreIntent::kReplace ? "kReplace" : "kHistory");
    Fixture fixture;
    fixture.install(recipe("crop"));
    const QDomDocument saved = fixture.snapshot(intent);
    (void)fixture.onDemand().remove("owner/crop");
    RecipeGraph::Recipe preview = recipe("crop");
    preview.ephemeral = true;
    fixture.install(preview);
    DerivedObjects derived = fixture.derived();

    const std::optional<QString> refused = derived.refusal(derived.parse(saved.documentElement(), intent));
    ASSERT_TRUE(refused.has_value());
    EXPECT_TRUE(refused->contains(u"'owner/crop'"_s) && refused->contains(u"preview"_s)) << refused->toStdString();
    EXPECT_TRUE(fixture.onDemand().graph().find("owner/crop")->recipe.ephemeral) << "nothing changed";
  }
}

// A restore renaming the outputs of two recipes announces the topics it registered and retired once, after the
// sweep, not once per install.
TEST(DerivedObjectsTest, ARestoreRenamingOutputsAnnouncesTopicsOnce) {
  Fixture fixture;
  fixture.install(recipe("a"));
  fixture.install(recipe("b"));
  QDomDocument doc = fixture.snapshot(RestoreIntent::kReplace);
  for (QDomElement entry =
           doc.documentElement().firstChildElement(u"data_processors"_s).firstChildElement(u"on_demand"_s);
       !entry.isNull(); entry = entry.nextSiblingElement(u"on_demand"_s)) {
    entry.firstChildElement(u"output"_s).setAttribute(u"name"_s, u"renamed"_s);
  }
  QSignalSpy announced(&fixture.session, &SessionManager::objectTopicsChanged);

  fixture.restoreCleanly(doc, RestoreIntent::kReplace);

  EXPECT_EQ(announced.count(), 1);
  EXPECT_TRUE(fixture.topic("owner/a/renamed").has_value());
  EXPECT_FALSE(fixture.topic("owner/a/cropped").has_value());
}

// Empty params are the recipe's "{}": the first save already writes what the parse reads, so save -> restore -> save
// is byte for byte, and restoring the saved block keeps the recipe.
TEST(DerivedObjectsTest, EmptyParamsRoundTripByteForByte) {
  QByteArray first_save;
  {
    Fixture source;
    RecipeGraph::Recipe no_params = recipe("crop");
    no_params.params_json.clear();
    source.install(no_params);
    const uint64_t revision = source.revision("owner/crop");
    first_save = source.savedBlock();
    source.restoreCleanly(source.snapshot(RestoreIntent::kReplace), RestoreIntent::kReplace);
    EXPECT_EQ(source.revision("owner/crop"), revision) << "not a change";
  }
  Fixture target;
  QDomDocument doc;
  ASSERT_TRUE(static_cast<bool>(doc.setContent(first_save)));
  target.restoreCleanly(doc, RestoreIntent::kReplace);
  EXPECT_EQ(target.savedBlock(), first_save);
}

// A layout restore of many Python recipes waits for none of them, even while a script stuck in a builtin C loop holds
// the interpreter (a held GIL here): Python compiles at a recipe's first run, on the Python lane, so every recipe
// installs and the restore reports nothing. The time bound is lax; a compile that waited would take seconds.
TEST(DerivedObjectsTest, ARestoreOfManyPythonRecipesWaitsForNone) {
#ifndef PJ_PYTHON_TEST_PROBE
  GTEST_SKIP() << "this build has no Python";
#else
  constexpr int kRecipes = 32;
  QByteArray saved;
  {
    Fixture source;
    for (int index = 0; index < kRecipes; ++index) {
      RecipeGraph::Recipe python = recipe("py" + std::to_string(index));
      python.language = "python";
      python.script = "def evaluate(inputs, params):\n    return {'cropped': inputs['cloud']}\n";
      source.install(python);
    }
    saved = source.savedBlock();
  }
  ASSERT_TRUE(scripting::compileOnDemandScriptPython("def evaluate(inputs, params):\n    return 1\n").has_value());
  Fixture target;
  QDomDocument doc;
  ASSERT_TRUE(static_cast<bool>(doc.setContent(saved)));
  {
    const test::PythonGilHold hold;
    const auto begin = std::chrono::steady_clock::now();
    target.restoreCleanly(doc, RestoreIntent::kReplace);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(500)) << "no wait per recipe";
  }
  EXPECT_EQ(target.onDemand().graph().keys().size(), static_cast<size_t>(kRecipes));
#endif
}

// A Python compile error never fails a restore: the broken recipe installs with no diagnostic (nothing compiles
// Python on the GUI thread), and its compile error surfaces at the node's first demanded run, as its error state, with
// the message an eager compile gives.
TEST(DerivedObjectsTest, ARestoredBrokenPythonRecipeInstallsSilentlyAndErrorsWhenDemanded) {
  if (!scripting::pythonAvailable()) {
    GTEST_SKIP() << "this build has no Python";
  }
  constexpr const char* kBroken = "def evaluate(inputs, params:\n    return {}\n";
  QByteArray saved;
  {
    Fixture source;
    RecipeGraph::Recipe broken = recipe("broken");
    broken.language = "python";
    broken.script = kBroken;
    source.install(broken);
    saved = source.savedBlock();
  }
  Fixture target;
  target.fake.real = true;
  QDomDocument doc;
  ASSERT_TRUE(static_cast<bool>(doc.setContent(saved)));
  target.restoreCleanly(doc, RestoreIntent::kReplace);
  ASSERT_NE(target.onDemand().graph().find("owner/broken"), nullptr) << "installed";
  const std::optional<NodeScheduler::NodeState> before = target.onDemand().state("owner/broken");
  EXPECT_TRUE(!before || before->status != NodeScheduler::NodeState::Status::kError) << "no error before a run";

  target.materialize("owner/broken", 10 * kSec);
  const auto eager = scripting::compileOnDemandScriptPython(kBroken);
  ASSERT_FALSE(eager.has_value());
  const std::optional<NodeScheduler::NodeState> after = target.onDemand().state("owner/broken");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->status, NodeScheduler::NodeState::Status::kError);
  EXPECT_EQ(after->detail, eager.error());
}

// What a restore's comparison and ordering cost per recipe and per edge, for a chain of N recipes the snapshot
// describes unchanged (parse, refusal, restore: no install, no run). PJ_BENCH only.
TEST(DerivedObjectsBench, OnDemandRestoreComparisonPerRecipe) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  for (const int count : {50, 200, 800}) {
    Fixture fixture;
    fixture.install(recipe("n0"));
    for (int index = 1; index < count; ++index) {
      fixture.install(recipe("n" + std::to_string(index), {"owner/n" + std::to_string(index - 1) + "/cropped"}));
    }
    const QDomDocument doc = fixture.snapshot(RestoreIntent::kHistory);
    const double micros = test::medianMicros(5, [&] {
      DerivedObjects derived = fixture.derived();
      const DerivedSnapshot parsed = derived.parse(doc.documentElement(), RestoreIntent::kHistory);
      (void)derived.refusal(parsed);
      (void)derived.restore(parsed);
    });
    EXPECT_EQ(fixture.fake.started(), 0);
    std::printf(
        "[bench] on-demand unchanged restore: %d recipes, %d edges: %.0f us total, %.2f us per recipe\n", count,
        count - 1, micros, micros / count);
  }
}

}  // namespace
}  // namespace PJ
