// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Layouts saved by a release-4.1 build (pj_app/tests/fixtures/layouts/release-4.1)
// restore through the real load paths: the derived recipes come back bound to their
// datasets, their outputs reach the catalog and the plots, the markers are drawn, and
// saving again writes the same <data_processors> block. So do the on-demand recipes of
// layouts saved by the prototype (pj_app/tests/fixtures/layouts/prototype).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "LayoutXml.h"
#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "on_demand_test_utils.h"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/point_cloud_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/ResultHistory.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowRelease41LayoutTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }
  static void loadLayout(MainWindow& window, const QString& path) {
    window.loadLayoutFromPath(path, MainWindow::LayoutLoadInteractivity::kAutomated);
  }
  static void saveLayout(MainWindow& window, const QString& path, bool include_data_source) {
    window.saveLayoutToPath(path, include_data_source);
  }
  [[nodiscard]] static bool applyHistorySnapshot(MainWindow& window, QDomDocument& doc) {
    return window.restoreWorkspaceState(
               doc, MainWindow::MissingCurvePolicy::kExact, /*rollback_to=*/nullptr, RestoreIntent::kHistory) ==
           MainWindow::RestoreResult::kApplied;
  }
};

}  // namespace PJ

namespace {

using Peer = PJ::MainWindowRelease41LayoutTestPeer;

constexpr PJ::Timestamp kSecond = 1'000'000'000LL;
constexpr const char* kFakeRoot = "/pj-fixtures";

const std::set<std::string> kTransformKeys{"fixture-assistant/t3", "fixture-toolbox/t1", "fixture-toolbox/t2"};
const std::set<std::string> kFilterOutputs{"imu_abs", "t1_abs"};
const std::set<std::string> kGeneratorIds{
    "fixture-assistant/rule/odom", "fixture-toolbox/rule/a-sensor", "fixture-toolbox/rule/all",
    "fixture-toolbox/rule/declared", "fixture-toolbox/rule/t1"};

QByteArray readFile(const QString& path) {
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
  return file.readAll();
}

void writeFile(const QString& path, const QByteArray& bytes) {
  EXPECT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  ASSERT_EQ(file.write(bytes), bytes.size());
}

// A float32 xyz cloud of eight points at t = 1 s, as the writer published it.
std::vector<uint8_t> smallCloud() {
  std::vector<float> xyz;
  for (int index = 0; index < 8; ++index) {
    xyz.insert(
        xyz.end(),
        {static_cast<float>(index & 1), static_cast<float>((index >> 1) & 1), static_cast<float>((index >> 2) & 1)});
  }
  using DT = PJ::sdk::PointField::Datatype;
  PJ::sdk::PointCloud cloud;
  cloud.frame_id = "map";
  cloud.fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
  cloud.point_step = 12;
  cloud.width = static_cast<uint32_t>(xyz.size() / 3);
  cloud.height = 1;
  cloud.row_step = cloud.point_step * cloud.width;
  cloud.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(xyz.data()), xyz.size() * sizeof(float));
  cloud.timestamp_ns = kSecond;
  return PJ::serializePointCloud(cloud);
}

PJ::TopicId addScalarTopic(PJ::AppSession& app, PJ::DatasetId dataset, const std::string& topic, double scale) {
  PJ::DataWriter writer = app.sessionManager().dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset, topic, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle.has_value());
  for (int index = 0; index < 5; ++index) {
    writer.appendScalar(*handle, (index + 1) * kSecond, scale * (index - 2));
  }
  EXPECT_FALSE(app.sessionManager().commitChunks(writer.flushAll()).empty());
  return handle->topic_id;
}

// Element `element` as one comparable string: tag, attributes sorted by name with
// every dataset id mapped through `saved_id_of`, text, then children in order.
QString canonical(const QDomElement& element, const std::map<QString, QString>& saved_id_of) {
  std::map<QString, QString> attributes;
  const QDomNamedNodeMap map = element.attributes();
  for (int index = 0; index < map.count(); ++index) {
    const QDomAttr attribute = map.item(index).toAttr();
    QString value = attribute.value();
    if (attribute.name().endsWith(u"dataset_id"_s)) {
      const auto found = saved_id_of.find(value);
      value = found != saved_id_of.end() ? found->second : u"?"_s + value;
    }
    attributes.emplace(attribute.name(), value);
  }
  QString out = u"<"_s + element.tagName();
  for (const auto& [name, value] : attributes) {
    out += u" "_s + name + u"=\""_s + value + u"\""_s;
  }
  out += u">"_s;
  for (QDomNode child = element.firstChild(); !child.isNull(); child = child.nextSibling()) {
    if (child.isElement()) {
      out += canonical(child.toElement(), saved_id_of);
    } else if (child.isCDATASection() || child.isText()) {
      out += child.nodeValue();
    }
  }
  return out + u"</"_s + element.tagName() + u">"_s;
}

// The <data_processors> entries of `bytes`, canonical and sorted: a restore may
// recreate entries of one kind in another order.
std::vector<QString> processorEntries(const QByteArray& bytes, const std::map<QString, QString>& saved_id_of) {
  QDomDocument doc;
  EXPECT_TRUE(static_cast<bool>(doc.setContent(bytes)));
  std::vector<QString> entries;
  const QDomElement block = doc.documentElement().firstChildElement(u"data_processors"_s);
  for (QDomElement entry = block.firstChildElement(); !entry.isNull(); entry = entry.nextSiblingElement()) {
    entries.push_back(canonical(entry, saved_id_of));
  }
  std::sort(entries.begin(), entries.end());
  return entries;
}

// The writer's session, recreated with B loaded before A so every saved dataset id
// names the other file: the restore must rebind by path, never trust the id.
class MainWindowRelease41LayoutTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(work_dir_.isValid());
    root_ = QDir(work_dir_.path()).absoluteFilePath(u"root"_s);
    const QString path_a = touch(root_ + u"/data/fixture-a.mcap"_s);
    const QString path_b = touch(root_ + u"/data/fixture-b.mcap"_s);
    root_ = QFileInfo(root_).canonicalFilePath();

    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_.path());
    PJ::AppSession& app = Peer::session(*window_);
    PJ::SessionManager& session = app.sessionManager();
    dataset_b_ = pj_test::createDataset(app, "fixture-b.mcap", /*own_time_domain=*/true);
    dataset_a_ = pj_test::createDataset(app, "fixture-a.mcap", /*own_time_domain=*/true);
    ASSERT_EQ(dataset_b_, 1U) << "the fixture saved A as 1 and B as 2; this session swaps them";
    ASSERT_EQ(dataset_a_, 2U);
    addScalarTopic(app, dataset_a_, "/sensor", 1.0);
    addScalarTopic(app, dataset_a_, "/imu", 0.5);
    addScalarTopic(app, dataset_b_, "/sensor", 2.0);
    addScalarTopic(app, dataset_b_, "/odom", 3.0);
    const auto points = session.objectStore().registerTopic(
        PJ::ObjectTopicDescriptor{
            .dataset_id = dataset_a_,
            .topic_name = "/points",
            .metadata_json = R"({"builtin_object_type":"kPointCloud"})"});
    ASSERT_TRUE(points.has_value());
    points_ = *points;
    ASSERT_TRUE(session.objectStore().pushOwned(points_, kSecond, smallCloud()).has_value());
    app.catalogModel().rebuildFromDatastore();
    session.setDatasetSourcePath(dataset_a_, path_a);
    session.setDatasetSourcePath(dataset_b_, path_b);
    session.recordLoadedSource(path_a, QString{}, u"MCAP Loader"_s, u"{}"_s, u"mcap-loader"_s);
    session.recordLoadedSource(path_b, QString{}, u"MCAP Loader"_s, u"{}"_s, u"mcap-loader"_s);
  }

  void TearDown() override {
    window_.reset();
  }

  [[nodiscard]] static QString touch(const QString& path) {
    EXPECT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    return QFileInfo(path).canonicalFilePath();
  }

  // The fixture `name` with its fake root replaced by this test's data root.
  [[nodiscard]] QByteArray relocatedFixture(const QString& name) const {
    QByteArray bytes = readFile(QDir(QStringLiteral(PJ_RELEASE41_LAYOUT_FIXTURES)).filePath(name));
    bytes.replace(QByteArray(kFakeRoot), root_.toUtf8());
    return bytes;
  }

  // Loads fixture `name` the way File > Load Layout does, and waits until the
  // restore settled and every marker rule finished its first run.
  void loadFixture(const QString& name) {
    const QString path = QDir(work_dir_.path()).filePath(u"layouts/"_s + name);
    writeFile(path, relocatedFixture(name));
    QSignalSpy settled(window_.get(), &PJ::MainWindow::layoutRestoreSettled);
    Peer::loadLayout(*window_, path);
    waitFor([&settled] { return !settled.isEmpty(); });
    ASSERT_EQ(settled.size(), 1);
    EXPECT_TRUE(settled.front().front().toBool());
    waitForMarkerRules();
  }

  static void waitFor(const std::function<bool()>& done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 10'000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
  }

  void waitForMarkerRules() {
    PJ::BatchEngine& engine = app().sessionManager().batchEngine();
    waitFor([&engine] {
      const auto recipes = engine.recipes();
      return std::none_of(
          recipes.begin(), recipes.end(), [&engine](const auto& recipe) { return engine.isRunning(recipe.id); });
    });
    QCoreApplication::processEvents();
  }

  [[nodiscard]] PJ::AppSession& app() const {
    return Peer::session(*window_);
  }

  [[nodiscard]] PJ::PlotWidget* plotInTab(const QString& tab_name) const {
    auto* tabs = window_->findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
    PJ::PlotDocker* docker =
        tabs != nullptr
            ? tabs->findDocker([&tab_name](PJ::PlotDocker* candidate) { return candidate->name() == tab_name; })
            : nullptr;
    return docker != nullptr && docker->plotCount() > 0 ? docker->plotAt(0)->plotWidget() : nullptr;
  }

  [[nodiscard]] PJ::Scene3DDockWidget* sceneInTab(const QString& tab_name) const {
    auto* tabs = window_->findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
    PJ::PlotDocker* docker =
        tabs != nullptr
            ? tabs->findDocker([&tab_name](PJ::PlotDocker* candidate) { return candidate->name() == tab_name; })
            : nullptr;
    if (docker == nullptr || docker->plotCount() == 0 || docker->plotAt(0)->objectWidget() == nullptr) {
      return nullptr;
    }
    return qobject_cast<PJ::Scene3DDockWidget*>(docker->plotAt(0)->objectWidget()->widget());
  }

  // (dataset, topic) of every curve bound on `plot`.
  [[nodiscard]] std::set<std::pair<PJ::DatasetId, QString>> boundCurves(const PJ::PlotWidget* plot) const {
    std::set<std::pair<PJ::DatasetId, QString>> curves;
    for (const auto& info : plot->curveList()) {
      if (const auto descriptor = app().catalogModel().curveDescriptor(info.source_name)) {
        curves.emplace(descriptor->dataset_id, descriptor->topic_name);
      }
    }
    return curves;
  }

  [[nodiscard]] bool catalogHas(PJ::DatasetId dataset, const QString& topic) const {
    const auto curves = app().catalogModel().curves();
    return std::any_of(curves.begin(), curves.end(), [&](const PJ::CurveDescriptor& curve) {
      return curve.dataset_id == dataset && curve.topic_name == topic;
    });
  }

  // What every fixture brings back: the recipe set, each bound to its dataset.
  void expectRecipesRestored() const {
    PJ::SessionManager& session = app().sessionManager();
    std::set<std::string> transform_keys;
    for (const auto& recipe : session.dataProcessorService().transformRecipes()) {
      transform_keys.insert(recipe.key);
      EXPECT_EQ(recipe.history_exempt, recipe.key == "fixture-assistant/t3") << recipe.key;
      ASSERT_EQ(recipe.input_bindings.size(), 1U) << recipe.key;
      EXPECT_EQ(
          recipe.input_bindings.front().dataset_id, recipe.key == "fixture-assistant/t3" ? dataset_b_ : dataset_a_)
          << recipe.key;
    }
    EXPECT_EQ(transform_keys, kTransformKeys);

    std::set<std::string> filter_outputs;
    for (const auto& recipe : session.dataProcessorService().recipes()) {
      filter_outputs.insert(recipe.output_name);
      EXPECT_EQ(recipe.dataset_id, dataset_a_) << recipe.output_name;
      EXPECT_EQ(recipe.processor_id, "absolute");
    }
    EXPECT_EQ(filter_outputs, kFilterOutputs);

    std::set<std::string> generator_ids;
    for (const auto& recipe : session.batchEngine().recipes()) {
      generator_ids.insert(recipe.id);
      EXPECT_FALSE(recipe.window.has_value()) << recipe.id << ": release-4.1 saved no window";
      EXPECT_EQ(recipe.history_exempt, recipe.id == "fixture-assistant/rule/odom") << recipe.id;
      EXPECT_EQ(recipe.all_datasets, recipe.id == "fixture-toolbox/rule/all") << recipe.id;
      if (!recipe.all_datasets) {
        EXPECT_EQ(recipe.dataset_id, recipe.id == "fixture-assistant/rule/odom" ? dataset_b_ : dataset_a_) << recipe.id;
      }
      if (recipe.id == "fixture-toolbox/rule/declared") {
        EXPECT_EQ(recipe.inputs, std::vector<std::string>{"/sensor/value"});
        EXPECT_EQ(recipe.declared_inputs, std::vector<std::string>{"fixture-a.mcap:/sensor/value"});
      }
      if (recipe.id == "fixture-toolbox/rule/a-sensor") {
        EXPECT_EQ(recipe.params_json, R"({"threshold":1.5})");
      }
    }
    EXPECT_EQ(generator_ids, kGeneratorIds);

    for (const QString& topic : {u"imu_abs"_s, u"t1_abs"_s, u"t1_scaled"_s, u"t2_negated"_s}) {
      EXPECT_TRUE(catalogHas(dataset_a_, topic)) << topic.toStdString();
    }
    EXPECT_TRUE(catalogHas(dataset_b_, u"t3_exempt"_s));
  }

  // Each rule published one marker per sample of its five-sample input.
  void expectMarkersDrawn() const {
    PJ::AppSession& session = app();
    EXPECT_EQ(
        pj_test::markerTimes(session, dataset_a_, "a_sensor_markers", "fixture-toolbox/rule/a-sensor").size(), 5U);
    EXPECT_EQ(pj_test::markerTimes(session, dataset_a_, "t1_markers", "fixture-toolbox/rule/t1").size(), 5U);
    EXPECT_EQ(
        pj_test::markerTimes(session, dataset_a_, "declared_markers", "fixture-toolbox/rule/declared").size(), 5U);
    EXPECT_EQ(pj_test::markerTimes(session, dataset_b_, "odom_markers", "fixture-assistant/rule/odom").size(), 5U);
    EXPECT_EQ(
        pj_test::markerTimes(
            session, PJ::kAllDatasetsMarkerDataset, PJ::kAllDatasetsMarkerTopic, "fixture-toolbox/rule/all")
            .size(),
        10U)
        << "five per dataset";
  }

  // Saving the restored session again writes the fixture's <data_processors> block,
  // modulo attribute order and the reminted dataset ids. A generic resave also strips
  // the dataset qualifiers release-4.1 left on its generic layout's generators.
  void expectResavedProcessorsMatch(const QString& fixture, bool include_data_source) const {
    const QString resaved = QDir(work_dir_.path()).filePath(u"layouts/resaved.pj4.xml"_s);
    Peer::saveLayout(*window_, resaved, include_data_source);
    const std::map<QString, QString> saved_id_of{
        {QString::number(dataset_a_), u"1"_s}, {QString::number(dataset_b_), u"2"_s}};
    const std::map<QString, QString> identity{{u"1"_s, u"1"_s}, {u"2"_s, u"2"_s}};
    QByteArray expected = relocatedFixture(fixture);
    if (!include_data_source) {
      QDomDocument doc;
      ASSERT_TRUE(static_cast<bool>(doc.setContent(expected)));
      PJ::layout_xml::removeDatasetQualifiersForGenericLayout(doc);
      expected = doc.toByteArray();
    }
    const std::vector<QString> saved_by_release41 = processorEntries(expected, identity);
    ASSERT_EQ(saved_by_release41.size(), 10U) << "2 filters, 3 transforms, 5 generators";
    EXPECT_EQ(processorEntries(readFile(resaved), saved_id_of), saved_by_release41);
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir work_dir_;
  QString root_;
  std::unique_ptr<PJ::MainWindow> window_;
  PJ::DatasetId dataset_a_ = 0;
  PJ::DatasetId dataset_b_ = 0;
  PJ::ObjectTopicId points_{};
};

TEST_F(MainWindowRelease41LayoutTest, RestoresLayoutSavedByRelease41) {
  loadFixture(u"source_bound.pj4.xml"_s);
  PJ::SessionManager& session = app().sessionManager();

  expectRecipesRestored();
  EXPECT_EQ(session.sourceDisplayOffset(dataset_a_).value.count(), 2'500'000'000LL);
  EXPECT_EQ(session.sourceDisplayOffset(dataset_b_).value.count(), -1'250'000'000LL);
  expectMarkersDrawn();

  const PJ::PlotWidget* first = plotInTab(u"tab1"_s);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(
      boundCurves(first), (std::set<std::pair<PJ::DatasetId, QString>>{
                              {dataset_a_, u"/sensor"_s},
                              {dataset_a_, u"imu_abs"_s},
                              {dataset_a_, u"t1_scaled"_s},
                              {dataset_a_, u"t2_negated"_s}}));
  const PJ::PlotWidget* second = plotInTab(u"fixture-derived"_s);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(
      boundCurves(second), (std::set<std::pair<PJ::DatasetId, QString>>{
                               {dataset_a_, u"t1_abs"_s}, {dataset_b_, u"/sensor"_s}, {dataset_b_, u"t3_exempt"_s}}));
  EXPECT_FALSE(second->datasetMarkerScopeVisible(dataset_b_, PJ::MarkerScope::kAllDatasets));
  EXPECT_TRUE(second->datasetMarkerScopeVisible(dataset_b_, PJ::MarkerScope::kDataset));
  EXPECT_TRUE(second->datasetMarkerScopeVisible(dataset_a_, PJ::MarkerScope::kAllDatasets));

  const PJ::Scene3DDockWidget* scene = sceneInTab(u"fixture-scene"_s);
  ASSERT_NE(scene, nullptr);
  EXPECT_NE(scene->layerFor(points_), nullptr);

  expectResavedProcessorsMatch(u"source_bound.pj4.xml"_s, /*include_data_source=*/true);
}

TEST_F(MainWindowRelease41LayoutTest, RestoresGenericLayoutSavedByRelease41) {
  loadFixture(u"generic.pj4.xml"_s);

  // Generic: no saved offsets, and series bind only where topic+field is unique;
  // the generators still carry the absolute dataset paths release-4.1 wrote, and bind by them.
  expectRecipesRestored();
  expectMarkersDrawn();
  const PJ::PlotWidget* second = plotInTab(u"fixture-derived"_s);
  ASSERT_NE(second, nullptr);
  const auto curves = boundCurves(second);
  EXPECT_TRUE(curves.contains({dataset_a_, u"t1_abs"_s}));
  EXPECT_TRUE(curves.contains({dataset_b_, u"t3_exempt"_s}));

  expectResavedProcessorsMatch(u"generic.pj4.xml"_s, /*include_data_source=*/false);
}

TEST_F(MainWindowRelease41LayoutTest, RestoresHistorySnapshotSavedByRelease41) {
  loadFixture(u"source_bound.pj4.xml"_s);
  // Change the non-exempt set so the undo has something to put back.
  PJ::SessionManager& session = app().sessionManager();
  ASSERT_TRUE(session.dataProcessorService().removeTransform("fixture-toolbox/t2").has_value());
  ASSERT_TRUE(session.batchEngine().removeGenerator("fixture-toolbox/rule/a-sensor").has_value());
  app().catalogModel().rebuildFromDatastore();

  QDomDocument snapshot;
  ASSERT_TRUE(static_cast<bool>(snapshot.setContent(relocatedFixture(u"history_snapshot.xml"_s))));
  ASSERT_TRUE(Peer::applyHistorySnapshot(*window_, snapshot));
  waitForMarkerRules();

  // The snapshot names no exempt recipe; history keeps the live ones.
  expectRecipesRestored();
  expectMarkersDrawn();
}

// The prototype's session (prototype_fixture_writer.cpp.txt), recreated with a decoy dataset loaded first, so the
// saved dataset id 1 names the decoy: the restore must bind by path. The fake script run computes.
class MainWindowPrototypeLayoutTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(work_dir_.isValid());
    root_ = QDir(work_dir_.path()).absoluteFilePath(u"root"_s);
    const QString path = touch(root_ + u"/data/fixture-a.mcap"_s);
    root_ = QFileInfo(root_).canonicalFilePath();
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_.path());
    PJ::AppSession& app = Peer::session(*window_);
    PJ::SessionManager& session = app.sessionManager();
    ASSERT_EQ(pj_test::createDataset(app, "decoy.mcap", /*own_time_domain=*/true), 1U);
    dataset_ = pj_test::createDataset(app, "fixture-a.mcap", /*own_time_domain=*/true);
    addScalarTopic(app, dataset_, "threshold", 1.0);
    const auto cloud = session.objectStore().registerTopic(
        PJ::ObjectTopicDescriptor{
            .dataset_id = dataset_,
            .topic_name = "cloud",
            .metadata_json = R"({"builtin_object_type":"kPointCloud"})"});
    ASSERT_TRUE(cloud.has_value());
    const auto bytes = PJ::serializeBuiltinObject(PJ::sdk::BuiltinObject(PJ::test::makeCloud(2, 5 * kSecond)));
    ASSERT_TRUE(session.objectStore().pushOwned(*cloud, 5 * kSecond, *bytes).has_value());
    app.catalogModel().rebuildFromDatastore();
    session.setDatasetSourcePath(dataset_, path);
    session.recordLoadedSource(path, QString{}, u"MCAP Loader"_s, u"{}"_s, u"mcap-loader"_s);
  }
  void TearDown() override {
    window_.reset();
  }

  [[nodiscard]] static QString touch(const QString& path) {
    EXPECT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    return QFileInfo(path).canonicalFilePath();
  }
  [[nodiscard]] QByteArray relocatedFixture(const QString& name) const {
    QByteArray bytes = readFile(QDir(QStringLiteral(PJ_PROTOTYPE_LAYOUT_FIXTURES)).filePath(name));
    bytes.replace(QByteArray(kFakeRoot), root_.toUtf8());
    return bytes;
  }
  [[nodiscard]] PJ::AppSession& app() const {
    return Peer::session(*window_);
  }

  PJ::test::FakeRun fake_;  ///< outlives the window, whose session's pool joins the runs
  PJ::test::ScopedRun scoped_run_{fake_};
  QTemporaryDir extensions_dir_;
  QTemporaryDir work_dir_;
  QString root_;
  std::unique_ptr<PJ::MainWindow> window_;
  PJ::DatasetId dataset_ = 0;
};

// GAP-39: a source-bound layout the prototype saved (its reader written before its producer, a finding, an exempt
// recipe, `infer_outputs` and `label` attributes) restores through File > Load Layout: every recipe on the
// dataset its path names, the reader bound to its producer, the finding computed at its pin; saving again writes
// the same <on_demand> entries without the two attributes the format dropped, the reader's input naming its upstream.
TEST_F(MainWindowPrototypeLayoutTest, RestoresLayoutSavedByPrototype) {
  const QString path = QDir(work_dir_.path()).filePath(u"layouts/source_bound.pj4.xml"_s);
  writeFile(path, relocatedFixture(u"source_bound.pj4.xml"_s));
  QSignalSpy settled(window_.get(), &PJ::MainWindow::layoutRestoreSettled);
  Peer::loadLayout(*window_, path);
  QElapsedTimer timer;
  timer.start();
  while (settled.isEmpty() && timer.elapsed() < 10'000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  ASSERT_EQ(settled.size(), 1);
  EXPECT_TRUE(settled.front().front().toBool());
  PJ::SessionManager& session = app().sessionManager();
  session.enginePool().waitIdle();
  QCoreApplication::processEvents();

  const PJ::RecipeGraph& graph = session.onDemandService().graph();
  EXPECT_EQ(
      graph.keys(), (std::vector<std::string>{
                        "fixture-assistant/exempt", "fixture-toolbox/crop", "fixture-toolbox/downstream",
                        "fixture-toolbox/finding", "fixture-toolbox/inferred"}));
  for (const std::string& key : graph.keys()) {
    EXPECT_EQ(graph.find(key)->dataset, dataset_) << key << ": bound by path, not by the saved id";
    EXPECT_EQ(graph.find(key)->recipe.history_exempt, key == "fixture-assistant/exempt") << key;
  }
  const PJ::RecipeGraph::Node* downstream = graph.find("fixture-toolbox/downstream");
  ASSERT_NE(downstream, nullptr);
  EXPECT_EQ(downstream->bindings.front().upstream, "fixture-toolbox/crop");
  EXPECT_FALSE(downstream->recipe.query_time);
  EXPECT_EQ(graph.find("fixture-toolbox/crop")->recipe.params_json, R"({"zmin": -5})");
  const PJ::RecipeGraph::Node* finding = graph.find("fixture-toolbox/finding");
  ASSERT_NE(finding, nullptr);
  EXPECT_EQ(finding->recipe.pinned_t, 5 * kSecond);
  const auto finding_topic = session.objectStore().findTopic(dataset_, "fixture-toolbox/finding/cropped");
  ASSERT_TRUE(finding_topic.has_value());
  EXPECT_TRUE(session.objectStore().latestAt(*finding_topic, PJ::kFindingTimestamp).has_value())
      << "the finding is computed at its pin";

  const QString resaved = QDir(work_dir_.path()).filePath(u"layouts/resaved.pj4.xml"_s);
  Peer::saveLayout(*window_, resaved, /*include_data_source=*/true);
  QDomDocument expected;
  ASSERT_TRUE(static_cast<bool>(expected.setContent(relocatedFixture(u"source_bound.pj4.xml"_s))));
  const QDomNodeList entries = expected.elementsByTagName(u"on_demand"_s);
  for (int index = 0; index < entries.size(); ++index) {
    QDomElement entry = entries.at(index).toElement();
    entry.removeAttribute(u"infer_outputs"_s);
    entry.removeAttribute(u"label"_s);
    if (entry.attribute(u"id"_s) == u"downstream"_s) {  // an input reading another node's output records it
      QDomElement input = entry.firstChildElement(u"input"_s);
      input.setAttribute(u"upstream"_s, u"fixture-toolbox/crop"_s);
      input.setAttribute(u"type"_s, u"kPointCloud"_s);
    }
  }
  const std::vector<QString> saved_by_prototype = processorEntries(expected.toByteArray(), {{u"1"_s, u"1"_s}});
  ASSERT_EQ(saved_by_prototype.size(), 5U);
  EXPECT_EQ(processorEntries(readFile(resaved), {{QString::number(dataset_), u"1"_s}}), saved_by_prototype);
}

}  // namespace
