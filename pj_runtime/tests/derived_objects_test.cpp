// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Derived-recipe layout persistence without MainWindow: one restore orders the
// engines whatever the document order, save -> restore -> save keeps the
// <data_processors> block byte for byte, and a restore leaves a marker rule it
// does not change alone.

#include <gtest/gtest.h>

#include <QDomDocument>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "event_pump_test_utils.h"
#include "marker_test_utils.h"
#include "pj_base/builtin/plot_markers.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DerivedObjects.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/script_engine.h"

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr Timestamp kSecond = 1'000'000'000LL;

constexpr const char* kAbsoluteSrc = R"LUAU(
return { { id="absolute", name="Absolute", output="same",
  create = function(p) return { calculate = function(t, v) return math.abs(v) end } end } }
)LUAU";

// A transform class whose id is `id`, multiplying its input by 10.
std::string times10(const std::string& id) {
  return "-- pj-script: luau\nreturn { id=\"" + id + "\", name=\"" + id +
         "\", output=\"same\",\n"
         "  create = function(p) return { calculate = function(t, v) return v * 10.0 end } end }\n";
}

// One marker per sample of `series_name`, valued like the sample.
std::string markerPerSample(const std::string& series_name) {
  return "local s = series(\"" + series_name +
         "\")\n"
         "for i = 0, s:size() - 1 do createMarker(s:at(i).t, s:at(i).v, {label=\"x\"}) end\n";
}

// An AppSession (it installs the catalog-backed series resolver marker rules read
// through) holding one file dataset with a five-sample /imu series.
class Session {
 public:
  Session() : app_(extensions_dir_.path()) {
    SessionManager& session = app_.sessionManager();
    auto catalogue = std::make_shared<scripting::FilterCatalogue>(scripting::makeLuauEngine());
    EXPECT_TRUE(catalogue->addBundledSource(kAbsoluteSrc, "bundled").has_value());
    session.dataProcessorService().setFilterCatalogue(catalogue);
    dataset_ = *session.dataEngine().createDataset(DatasetDescriptor{.source_name = "run.mcap"});
    DataWriter writer = session.dataEngine().createWriter();
    auto handle = writer.registerScalarSeries(dataset_, "/imu", NumericType::kFloat64);
    EXPECT_TRUE(handle.has_value());
    imu_ = handle->topic_id;
    for (int index = 0; index < 5; ++index) {
      writer.appendScalar(*handle, (index + 1) * kSecond, static_cast<double>(index - 2));
    }
    EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
    session.setDatasetSourcePath(dataset_, u"/data/run.mcap"_s);
    app_.catalogModel().rebuildFromDatastore();
  }

  [[nodiscard]] DerivedObjects derived() {
    return DerivedObjects(app_.sessionManager(), app_.catalogModel());
  }
  [[nodiscard]] SessionManager& session() {
    return app_.sessionManager();
  }
  [[nodiscard]] DatasetId dataset() const {
    return dataset_;
  }
  [[nodiscard]] TopicId imu() const {
    return imu_;
  }

  // Lists the transform outputs created since the last rebuild (marker rules resolve
  // their inputs through the catalog).
  void rebuildCatalog() {
    app_.catalogModel().rebuildFromDatastore();
  }

  // Adds a one-sample `topic` to the dataset (a new topic, so /imu keeps its rows).
  void addTopic(std::string_view topic) {
    DataWriter writer = session().dataEngine().createWriter();
    auto handle = writer.registerScalarSeries(dataset_, topic, NumericType::kFloat64);
    ASSERT_TRUE(handle.has_value());
    writer.appendScalar(*handle, 6 * kSecond, 1.0);
    ASSERT_FALSE(session().commitChunks(writer.flushAll()).empty());
    app_.catalogModel().rebuildFromDatastore();
  }

  // The values of the markers published under `output` on this dataset.
  [[nodiscard]] std::vector<double> markerValues(const std::string& output) {
    std::vector<double> values;
    for (const sdk::PlotMarker& marker : test::readMarkerUnion(session().objectStore(), dataset_, output).markers) {
      values.push_back(marker.value_low);
    }
    return values;
  }

 private:
  QTemporaryDir extensions_dir_;
  AppSession app_;
  DatasetId dataset_ = 0;
  TopicId imu_ = 0;
};

QDomDocument parseXml(const QString& xml) {
  QDomDocument doc;
  EXPECT_TRUE(static_cast<bool>(doc.setContent(xml)));
  return doc;
}

QByteArray savedBlock(DerivedObjects derived) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  root.appendChild(derived.save(doc, SnapshotScope::kFull));
  return doc.toByteArray(2);
}

// The live recipes saved as a snapshot document, in the scope `intent` restores.
QDomDocument snapshotOf(DerivedObjects derived, RestoreIntent intent) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  root.appendChild(
      derived.save(doc, intent == RestoreIntent::kHistory ? SnapshotScope::kHistory : SnapshotScope::kFull));
  return doc;
}

// Parses and restores `doc`, failing the test on any diagnostic.
void restoreCleanly(DerivedObjects derived, const QDomDocument& doc, RestoreIntent intent) {
  const DerivedRestoreReport report = derived.restore(derived.parse(doc.documentElement(), intent));
  EXPECT_TRUE(report.restored_all);
  for (const DerivedDiagnostic& diagnostic : report.diagnostics) {
    ADD_FAILURE() << diagnostic.id << ": " << diagnostic.message.toStdString();
  }
}

// A rule on "scaled/value", the output of the transform "tool/scale".
BatchEngine::GeneratorRecipe scaledRule(DatasetId dataset) {
  BatchEngine::GeneratorRecipe rule;
  rule.id = "tool/rule";
  rule.dataset_id = dataset;
  rule.inputs = {"scaled/value"};
  rule.outputs = {"scaled_markers"};
  rule.script = markerPerSample("scaled/value");
  return rule;
}

// A transform class whose id is `id`, multiplying its input by 100.
std::string times100(const std::string& id) {
  std::string script = times10(id);
  script.replace(script.find("v * 10.0"), 8, "v * 100.0");
  return script;
}

// The generator comes first in the document, before the filter on the transform
// output it reads and before that transform; one restore still brings back all
// three, the rule reading the transform's output.
TEST(DerivedObjectsTest, DerivedRestoreOrdersAcrossEngines) {
  Session fixture;
  const QDomDocument doc = parseXml(
      uR"(<root><data_processors>
  <generator id="tool/rule" language="luau" all_datasets="0" dataset_source="run.mcap">
    <input name="scaled/value"/>
    <output name="scaled_markers"/>
    <script><![CDATA[)"_s +
      QString::fromStdString(markerPerSample("scaled/value")) +
      uR"(]]></script>
  </generator>
  <processor input_topic="scaled" input_field="value" input_dataset_source="run.mcap" processor_id="absolute" output_name="scaled_abs"><![CDATA[{}]]></processor>
  <transform owner_plugin="tool" id="scale" backend="luau" api_version="1">
    <input name="/imu"/>
    <output name="scaled"/>
    <script><![CDATA[)"_s +
      QString::fromStdString(times10("scale")) +
      uR"(]]></script>
  </transform>
</data_processors></root>)"_s);

  DerivedObjects derived = fixture.derived();
  DerivedSnapshot snapshot = derived.parse(doc.documentElement(), RestoreIntent::kReplace);
  EXPECT_TRUE(snapshot.diagnostics.empty());
  ASSERT_FALSE(derived.refusal(snapshot).has_value());
  const DerivedRestoreReport report = derived.restore(std::move(snapshot));
  // A layout load waits a bounded time for a rule; let a slow first run publish.
  test::pumpUntil([&] { return !fixture.session().batchEngine().isRunning("tool/rule"); }, 10'000);

  EXPECT_TRUE(report.restored_all);
  for (const DerivedDiagnostic& diagnostic : report.diagnostics) {
    ADD_FAILURE() << diagnostic.id << ": " << diagnostic.message.toStdString();
  }
  EXPECT_TRUE(fixture.session().dataProcessorService().isTransformLive("tool/scale"));
  EXPECT_TRUE(fixture.session().dataProcessorService().isFilterLive(fixture.dataset(), "scaled_abs"));
  const std::vector<BatchEngine::GeneratorRecipe> generators = fixture.session().batchEngine().recipes();
  ASSERT_EQ(generators.size(), 1U);
  EXPECT_EQ(generators.front().dataset_id, fixture.dataset());
  EXPECT_EQ(fixture.markerValues("scaled_markers"), (std::vector<double>{-20.0, -10.0, 0.0, 10.0, 20.0}))
      << "the rule ran over the transform's output, not before it existed";
}

// Every attribute the format carries (filter params and embedded source, transform
// bindings and params, a history-exempt transform, generator window, declared input,
// params and exemption) survives save -> restore into a fresh session -> save.
TEST(DerivedObjectsTest, RoundTripKeepsTheLayoutFormat) {
  QByteArray first_save;
  {
    Session source;
    SessionManager& session = source.session();
    DataProcessorService& processors = session.dataProcessorService();
    ASSERT_TRUE(processors.applyFilter(source.imu(), source.dataset(), "absolute", "imu_abs").has_value());
    ASSERT_TRUE(
        processors.upsertTransform("tool", "scale", {"/imu"}, {"scaled"}, times10("scale"), R"({"k":1})").has_value());
    ASSERT_TRUE(processors
                    .upsertTransform(
                        "assistant", "again", {"scaled"}, {"scaled_twice"}, times10("again"), "{}",
                        /*ephemeral=*/false, /*input_column_index=*/0, /*history_exempt=*/true)
                    .has_value());
    BatchEngine::GeneratorRecipe rule;
    rule.id = "tool/rule";
    rule.dataset_id = source.dataset();
    rule.inputs = {"/imu/value"};
    rule.declared_inputs = {"run.mcap:/imu/value"};
    rule.outputs = {"imu_markers"};
    rule.script = markerPerSample("run.mcap:/imu/value");
    rule.params_json = R"({"threshold":1.5})";
    rule.window = std::pair<Timestamp, Timestamp>{2 * kSecond, 4 * kSecond};
    rule.history_exempt = true;
    ASSERT_TRUE(session.batchEngine().upsertGenerator(rule).has_value());
    first_save = savedBlock(source.derived());
  }
  ASSERT_TRUE(first_save.contains("window_start_ns"));
  ASSERT_TRUE(first_save.contains("source_fallback"));

  Session target;
  DerivedObjects derived = target.derived();
  QDomDocument doc;
  ASSERT_TRUE(static_cast<bool>(doc.setContent(first_save)));
  const DerivedRestoreReport report = derived.restore(derived.parse(doc.documentElement(), RestoreIntent::kReplace));
  EXPECT_TRUE(report.restored_all);
  EXPECT_TRUE(report.diagnostics.empty());

  EXPECT_EQ(savedBlock(target.derived()), first_save);
}

// One marker per /imu sample, from the rule `tool/rule` on the fixture's dataset.
BatchEngine::GeneratorRecipe imuRule(DatasetId dataset) {
  BatchEngine::GeneratorRecipe rule;
  rule.id = "tool/rule";
  rule.dataset_id = dataset;
  rule.inputs = {"/imu/value"};
  rule.outputs = {"imu_markers"};
  rule.script = markerPerSample("/imu/value");
  return rule;
}

// `imuRule` that also reads /flag and fails once it exists, brought to its failed state:
// the first run's markers stay drawn.
BatchEngine::GeneratorRecipe ruleInError(Session& fixture) {
  BatchEngine& engine = fixture.session().batchEngine();
  BatchEngine::GeneratorRecipe rule = imuRule(fixture.dataset());
  rule.inputs.push_back("/flag/value");
  rule.script = "local flag = series(\"/flag/value\")\nif flag ~= nil then error(\"flagged\") end\n" + rule.script;
  EXPECT_TRUE(engine.upsertGenerator(rule).has_value());
  fixture.addTopic("/flag");
  engine.refresh(rule.id);
  test::pumpUntil([&] { return !engine.isRunning(rule.id); });
  EXPECT_FALSE(engine.errorOf(rule.id).empty()) << "the last run failed";
  return rule;
}

// A restore whose snapshot describes a live marker rule unchanged leaves it alone,
// under either intent: the same object topic and published entry (no tombstone, no
// republish), no run, and its coverage kept.
TEST(DerivedObjectsTest, UnchangedGeneratorKeepsItsTopicAndEntryAcrossRestore) {
  for (const RestoreIntent intent : {RestoreIntent::kReplace, RestoreIntent::kHistory}) {
    SCOPED_TRACE(intent == RestoreIntent::kReplace ? "kReplace" : "kHistory");
    Session fixture;
    BatchEngine& engine = fixture.session().batchEngine();
    const BatchEngine::GeneratorRecipe rule = imuRule(fixture.dataset());
    ASSERT_TRUE(engine.upsertGenerator(rule).has_value());
    const auto coverage = engine.coverageOf(rule.id);
    ASSERT_TRUE(coverage.has_value());
    const ObjectStore& store = fixture.session().objectStore();
    const std::optional<ObjectTopicId> topic =
        store.findTopic(fixture.dataset(), markerOwnerTopicName("imu_markers", rule.id));
    ASSERT_TRUE(topic.has_value());
    const std::optional<std::uint64_t> entry = test::ownerPublishUid(store, fixture.dataset(), "imu_markers", rule.id);
    ASSERT_TRUE(entry.has_value());

    std::atomic<int> computes{0};
    BatchEngine::setComputeThreadProbeForTest([&computes](QThread*) { ++computes; });
    restoreCleanly(fixture.derived(), snapshotOf(fixture.derived(), intent), intent);
    test::pumpFor(50);  // a run the restore started would publish here
    BatchEngine::setComputeThreadProbeForTest({});

    EXPECT_EQ(computes.load(), 0) << "an unchanged rule does not run";
    EXPECT_EQ(store.findTopic(fixture.dataset(), markerOwnerTopicName("imu_markers", rule.id)), topic);
    EXPECT_EQ(test::ownerPublishUid(store, fixture.dataset(), "imu_markers", rule.id), entry)
        << "neither tombstoned nor republished";
    EXPECT_EQ(fixture.markerValues("imu_markers"), (std::vector<double>{-2.0, -1.0, 0.0, 1.0, 2.0}));
    EXPECT_EQ(engine.coverageOf(rule.id), coverage);
  }
}

// A restore whose snapshot describes a rule unchanged keeps it as it was even when its
// last run failed: its error, its drawn markers and its coverage stay, and it does not
// run.
void expectRestoreKeepsARuleInError(RestoreIntent intent) {
  Session fixture;
  BatchEngine& engine = fixture.session().batchEngine();
  const BatchEngine::GeneratorRecipe rule = ruleInError(fixture);
  const std::string error = engine.errorOf(rule.id);
  const auto coverage = engine.coverageOf(rule.id);
  ASSERT_TRUE(coverage.has_value());
  const std::optional<std::uint64_t> entry =
      test::ownerPublishUid(fixture.session().objectStore(), fixture.dataset(), "imu_markers", rule.id);
  ASSERT_TRUE(entry.has_value());

  std::atomic<int> computes{0};
  BatchEngine::setComputeThreadProbeForTest([&computes](QThread*) { ++computes; });
  restoreCleanly(fixture.derived(), snapshotOf(fixture.derived(), intent), intent);
  test::pumpFor(50);
  BatchEngine::setComputeThreadProbeForTest({});

  EXPECT_EQ(computes.load(), 0);
  EXPECT_EQ(test::ownerPublishUid(fixture.session().objectStore(), fixture.dataset(), "imu_markers", rule.id), entry);
  EXPECT_EQ(fixture.markerValues("imu_markers"), (std::vector<double>{-2.0, -1.0, 0.0, 1.0, 2.0}));
  EXPECT_EQ(engine.errorOf(rule.id), error);
  EXPECT_EQ(engine.coverageOf(rule.id), coverage);
}

// An undo restores what the user saw, the error included.
TEST(DerivedObjectsTest, HistoryRestoreKeepsARuleInErrorAsItWas) {
  expectRestoreKeepsARuleInError(RestoreIntent::kHistory);
}

// A layout load (any kReplace restore, a workspace rollback included) describes the rule
// just as unchanged, so it neither deletes nor re-runs a rule in error.
TEST(DerivedObjectsTest, LayoutLoadKeepsARuleInErrorAsItWas) {
  expectRestoreKeepsARuleInError(RestoreIntent::kReplace);
}

// A rule the snapshot describes unchanged still runs again when the transform it
// reads changes: the old output is retired, so the kept markers would describe data
// that no longer exists.
TEST(DerivedObjectsTest, KeptGeneratorReadingAChangedTransformReruns) {
  Session fixture;
  DataProcessorService& processors = fixture.session().dataProcessorService();
  BatchEngine& engine = fixture.session().batchEngine();
  engine.setMode(BatchEngine::Mode::kManual);  // only the restore and refresh() run the rule
  ASSERT_TRUE(processors.upsertTransform("tool", "scale", {"/imu"}, {"scaled"}, times100("scale"), "{}").has_value());
  fixture.rebuildCatalog();
  ASSERT_TRUE(engine.upsertGenerator(scaledRule(fixture.dataset())).has_value());
  const QDomDocument snapshot = snapshotOf(fixture.derived(), RestoreIntent::kReplace);

  ASSERT_TRUE(processors.upsertTransform("tool", "scale", {"/imu"}, {"scaled"}, times10("scale"), "{}").has_value());
  engine.refresh("tool/rule");
  test::pumpUntil([&] { return !engine.isRunning("tool/rule"); });
  ASSERT_EQ(fixture.markerValues("scaled_markers"), (std::vector<double>{-20.0, -10.0, 0.0, 10.0, 20.0}));

  restoreCleanly(fixture.derived(), snapshot, RestoreIntent::kReplace);
  test::pumpUntil([&] { return !engine.isRunning("tool/rule"); });
  EXPECT_EQ(fixture.markerValues("scaled_markers"), (std::vector<double>{-200.0, -100.0, 0.0, 100.0, 200.0}));
}

// The same when the restore creates the transform the rule reads (it was removed
// after the rule last ran): the rule's markers came from an output that is gone.
TEST(DerivedObjectsTest, KeptGeneratorReadingARecreatedTransformReruns) {
  Session fixture;
  DataProcessorService& processors = fixture.session().dataProcessorService();
  BatchEngine& engine = fixture.session().batchEngine();
  engine.setMode(BatchEngine::Mode::kManual);
  ASSERT_TRUE(processors.upsertTransform("tool", "scale", {"/imu"}, {"scaled"}, times10("scale"), "{}").has_value());
  fixture.rebuildCatalog();
  ASSERT_TRUE(engine.upsertGenerator(scaledRule(fixture.dataset())).has_value());
  ASSERT_EQ(fixture.markerValues("scaled_markers"), (std::vector<double>{-20.0, -10.0, 0.0, 10.0, 20.0}));
  ASSERT_TRUE(processors.removeTransform("tool/scale").has_value());
  ASSERT_TRUE(processors.upsertTransform("tool", "scale", {"/imu"}, {"scaled"}, times100("scale"), "{}").has_value());
  const QDomDocument snapshot = snapshotOf(fixture.derived(), RestoreIntent::kReplace);
  ASSERT_TRUE(processors.removeTransform("tool/scale").has_value());
  fixture.rebuildCatalog();

  restoreCleanly(fixture.derived(), snapshot, RestoreIntent::kReplace);
  test::pumpUntil([&] { return !engine.isRunning("tool/rule"); });
  EXPECT_EQ(fixture.markerValues("scaled_markers"), (std::vector<double>{-200.0, -100.0, 0.0, 100.0, 200.0}));
}

// The same when the restore reinstalls a live key whose outputs were renamed: the
// rule reads a name the restore creates, while its markers came from an earlier
// output of that name.
TEST(DerivedObjectsTest, KeptGeneratorReadingAReinstalledTransformsNewOutputReruns) {
  Session fixture;
  DataProcessorService& processors = fixture.session().dataProcessorService();
  BatchEngine& engine = fixture.session().batchEngine();
  engine.setMode(BatchEngine::Mode::kManual);
  ASSERT_TRUE(processors.upsertTransform("tool", "scale", {"/imu"}, {"scaled"}, times10("scale"), "{}").has_value());
  fixture.rebuildCatalog();
  ASSERT_TRUE(engine.upsertGenerator(scaledRule(fixture.dataset())).has_value());
  const QDomDocument snapshot = snapshotOf(fixture.derived(), RestoreIntent::kReplace);
  ASSERT_TRUE(processors.removeTransform("tool/scale").has_value());
  ASSERT_TRUE(processors.upsertTransform("tool", "scale", {"/imu"}, {"scaled"}, times100("scale"), "{}").has_value());
  fixture.rebuildCatalog();
  engine.refresh("tool/rule");
  test::pumpUntil([&] { return !engine.isRunning("tool/rule"); });
  ASSERT_EQ(fixture.markerValues("scaled_markers"), (std::vector<double>{-200.0, -100.0, 0.0, 100.0, 200.0}));
  ASSERT_TRUE(processors.removeTransform("tool/scale").has_value());
  ASSERT_TRUE(processors.upsertTransform("tool", "scale", {"/imu"}, {"renamed"}, times10("scale"), "{}").has_value());
  fixture.rebuildCatalog();

  restoreCleanly(fixture.derived(), snapshot, RestoreIntent::kReplace);
  test::pumpUntil([&] { return !engine.isRunning("tool/rule"); });
  EXPECT_EQ(fixture.markerValues("scaled_markers"), (std::vector<double>{-20.0, -10.0, 0.0, 10.0, 20.0}));
}

// A snapshot binding the restore cannot resolve is reported, also when the id's
// other binding is unchanged and stays live.
TEST(DerivedObjectsTest, UnresolvedBindingOfAKeptGeneratorIsReported) {
  Session fixture;
  BatchEngine& engine = fixture.session().batchEngine();
  BatchEngine::GeneratorRecipe rule;
  rule.id = "tool/rule";
  rule.dataset_id = fixture.dataset();
  rule.inputs = {"/imu/value"};
  rule.outputs = {"imu_markers"};
  rule.script = markerPerSample("/imu/value");
  ASSERT_TRUE(engine.upsertGenerator(rule).has_value());
  QDomDocument doc = snapshotOf(fixture.derived(), RestoreIntent::kReplace);
  // The same rule bound to a dataset this session has not loaded.
  QDomElement block = doc.documentElement().firstChildElement(u"data_processors"_s);
  QDomElement unloaded = block.firstChildElement(u"generator"_s).cloneNode().toElement();
  unloaded.setAttribute(u"dataset_id"_s, u"999"_s);
  unloaded.setAttribute(u"dataset_source"_s, u"other.mcap"_s);
  unloaded.setAttribute(u"dataset_path"_s, u"/data/other.mcap"_s);
  block.appendChild(unloaded);

  DerivedObjects derived = fixture.derived();
  const DerivedRestoreReport report = derived.restore(derived.parse(doc.documentElement(), RestoreIntent::kReplace));

  ASSERT_EQ(report.diagnostics.size(), 1U);
  EXPECT_STREQ(report.diagnostics.front().id, "generator-restore-failed");
  EXPECT_TRUE(report.diagnostics.front().message.contains(u"tool/rule"_s))
      << report.diagnostics.front().message.toStdString();
  EXPECT_NE(engine.firstBinding(rule.id), nullptr) << "the binding that resolves stays live";
}

// A generic layout (the generator names no dataset) with a rule that reads no series
// binds to the one loaded dataset: every dataset carries an empty input set.
TEST(DerivedObjectsTest, GenericRuleWithoutInputsBindsToTheOnlyLoadedDataset) {
  Session fixture;
  BatchEngine& engine = fixture.session().batchEngine();
  BatchEngine::GeneratorRecipe rule;
  rule.id = "tool/rule";
  rule.dataset_id = fixture.dataset();
  rule.outputs = {"constant_markers"};
  rule.script = "createMarker(1.0, 2.0, {label=\"x\"})\n";
  ASSERT_TRUE(engine.upsertGenerator(rule).has_value());
  QDomDocument doc = snapshotOf(fixture.derived(), RestoreIntent::kReplace);
  QDomElement generator =
      doc.documentElement().firstChildElement(u"data_processors"_s).firstChildElement(u"generator"_s);
  for (const QString& name : {u"dataset_id"_s, u"dataset_source"_s, u"dataset_path"_s}) {
    generator.removeAttribute(name);
  }

  restoreCleanly(fixture.derived(), doc, RestoreIntent::kReplace);

  const BatchEngine::GeneratorRecipe* bound = engine.firstBinding(rule.id);
  ASSERT_NE(bound, nullptr);
  EXPECT_EQ(bound->dataset_id, fixture.dataset());
}

}  // namespace
}  // namespace PJ
