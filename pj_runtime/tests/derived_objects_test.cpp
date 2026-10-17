// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Derived-recipe layout persistence without MainWindow: one restore orders the
// engines whatever the document order, and save -> restore -> save keeps the
// <data_processors> block byte for byte.

#include <gtest/gtest.h>

#include <QDomDocument>
#include <QTemporaryDir>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
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
  ASSERT_FALSE(derived.refusal(snapshot, RestoreIntent::kReplace).has_value());
  const DerivedRestoreReport report = derived.restore(std::move(snapshot), RestoreIntent::kReplace);
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
  const DerivedRestoreReport report =
      derived.restore(derived.parse(doc.documentElement(), RestoreIntent::kReplace), RestoreIntent::kReplace);
  EXPECT_TRUE(report.restored_all);
  EXPECT_TRUE(report.diagnostics.empty());

  EXPECT_EQ(savedBlock(target.derived()), first_save);
}

}  // namespace
}  // namespace PJ
