// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A marker generator and a transform input name their dataset the way a plotted
// curve does: the layout file carries the dataset's path relative to the layout
// (stampDatasetSourcePaths), a generic layout carries no dataset identity at all
// (removeDatasetQualifiersForGenericLayout), and restore resolves what is left with
// the session's one identity resolver. A bundled URDF file follows the layout folder
// too. These tests drive the real save and load entry points, moving the dataset's
// file between save and load.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDomDocument>
#include <QDomNodeList>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "FileLoader.h"
#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_codec.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/point_cloud_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/StateTransitionsController.h"
#include "pj_plotting/StateTransitionsDockWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/VisualizationKind.h"
#include "support/filepath_echo_source.h"
#include "support/loader_test_support.h"
#ifdef PJ_WITH_SCENE3D
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/layers/robot_model_layer.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#endif
#ifdef PJ_WITH_SCENE2D
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#endif

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowLayoutDatasetPathsTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }

  [[nodiscard]] static bool restoreDataProcessors(MainWindow& window, const QDomElement& root) {
    return window.restoreDataProcessors(root);
  }

  static void saveLayout(MainWindow& window, const QString& path, bool include_data_source) {
    window.saveLayoutToPath(path, include_data_source);
  }

  static void loadLayout(MainWindow& window, const QString& path) {
    window.loadLayoutFromPath(path, MainWindow::LayoutLoadInteractivity::kAutomated);
  }

  static void removeDatasetData(MainWindow& window, DatasetId dataset_id) {
    window.removeDatasetData(dataset_id);
  }

  static void reloadDataset(MainWindow& window, DatasetId dataset_id) {
    window.onReloadDatasetRequested(dataset_id);
  }

  [[nodiscard]] static FileLoader& fileLoader(MainWindow& window) {
    return *window.file_loader_;
  }
  /// The undo/redo snapshot: never crosses the file boundary, so it keeps absolute paths.
  [[nodiscard]] static QByteArray historySnapshot(MainWindow& window) {
    return window.captureWorkspace(SnapshotScope::kHistory).xml;
  }
};

}  // namespace PJ

namespace {

using Peer = PJ::MainWindowLayoutDatasetPathsTestPeer;

constexpr const char* kTopic = "sensor";
constexpr const char* kSeriesKey = "sensor/value";
constexpr const char* kOutputSeriesKey = "sensor/negated/value";
constexpr const char* kGeneratorId = "toolbox-anomaly-detector/rule/sensor";
constexpr const char* kOutputTopic = "sensor/negated";

constexpr const char* kNegate = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";

// A dataset-bound generator element, shaped as DerivedObjects::save writes it.
// `label` tells apart generators of one id that differ only in their script.
QDomElement appendGenerator(
    QDomDocument& doc, QDomElement& processors, PJ::DatasetId dataset_id, const QString& source_name,
    const QString& input_key = QString::fromLatin1(kSeriesKey), const QString& label = u"sample"_s) {
  QDomElement gen = doc.createElement(u"generator"_s);
  gen.setAttribute(u"id"_s, QString::fromLatin1(kGeneratorId));
  gen.setAttribute(u"language"_s, u"luau"_s);
  gen.setAttribute(u"all_datasets"_s, u"0"_s);
  gen.setAttribute(u"dataset_id"_s, QString::number(dataset_id));
  gen.setAttribute(u"dataset_source"_s, source_name);
  processors.appendChild(gen);

  QDomElement input = doc.createElement(u"input"_s);
  input.setAttribute(u"name"_s, input_key);
  gen.appendChild(input);
  QDomElement output = doc.createElement(u"output"_s);
  output.setAttribute(u"name"_s, QString::fromLatin1(kSeriesKey));
  gen.appendChild(output);
  QDomElement script = doc.createElement(u"script"_s);
  script.appendChild(doc.createCDATASection(
      u"local s = series(\"%1\")\ncreateMarker(s:at(0).t, s:at(0).v, {label=\"%2\"})\n"_s.arg(input_key, label)));
  gen.appendChild(script);
  return gen;
}

// The shape a generic layout leaves: the generator names no dataset.
void stripDatasetAttributes(QDomElement& gen) {
  for (const QString& name : {u"dataset_id"_s, u"dataset_source"_s, u"dataset_path"_s}) {
    gen.removeAttribute(name);
  }
}

// A transform element over `kSeriesKey`, shaped as DerivedObjects::save writes it.
void appendTransform(QDomDocument& doc, QDomElement& processors, PJ::DatasetId dataset_id, const QString& source_name) {
  QDomElement transform = doc.createElement(u"transform"_s);
  transform.setAttribute(u"owner_plugin"_s, u"pluginA"_s);
  transform.setAttribute(u"id"_s, u"negate"_s);
  transform.setAttribute(u"backend"_s, u"luau"_s);
  transform.setAttribute(u"api_version"_s, u"1"_s);
  processors.appendChild(transform);

  QDomElement input = doc.createElement(u"input"_s);
  input.setAttribute(u"name"_s, QString::fromLatin1(kSeriesKey));
  input.setAttribute(u"dataset_id"_s, QString::number(dataset_id));
  input.setAttribute(u"dataset_source"_s, source_name);
  input.setAttribute(u"topic"_s, QString::fromLatin1(kTopic));
  input.setAttribute(u"field"_s, u"value"_s);
  input.setAttribute(u"column"_s, u"0"_s);
  transform.appendChild(input);
  QDomElement output = doc.createElement(u"output"_s);
  output.setAttribute(u"name"_s, QString::fromLatin1(kOutputTopic));
  transform.appendChild(output);
  QDomElement params = doc.createElement(u"params"_s);
  params.appendChild(doc.createCDATASection(u"{}"_s));
  transform.appendChild(params);
  QDomElement script = doc.createElement(u"script"_s);
  script.appendChild(doc.createCDATASection(QString::fromLatin1(kNegate)));
  transform.appendChild(script);
}

enum class Processor { kGenerator, kTransform };

QDomDocument newLayout(QDomElement& processors) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  processors = doc.createElement(u"data_processors"_s);
  root.appendChild(processors);
  return doc;
}

QDomDocument processorLayout(Processor which, PJ::DatasetId dataset_id, const QString& source_name) {
  QDomElement processors;
  QDomDocument doc = newLayout(processors);
  if (which == Processor::kGenerator) {
    appendGenerator(doc, processors, dataset_id, source_name);
  } else {
    appendTransform(doc, processors, dataset_id, source_name);
  }
  return doc;
}

QDomDocument readXml(const QString& path) {
  QDomDocument doc;
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << qPrintable(path);
  EXPECT_TRUE(static_cast<bool>(doc.setContent(&file))) << qPrintable(path);
  return doc;
}

void writeXml(const QString& path, const QDomDocument& doc) {
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate)) << qPrintable(path);
  file.write(doc.toByteArray(2));
}

QDomElement firstElement(const QDomDocument& doc, const QString& tag) {
  return doc.elementsByTagName(tag).at(0).toElement();
}

// A temp tree with a "saved" and a "moved" layout folder, each holding the data
// subfolders the tests register dataset paths under. The data files themselves are
// never created: an existing source would make the layout load reload it.
struct Folders {
  QTemporaryDir root;
  QString saved;
  QString moved;

  [[nodiscard]] bool create() {
    if (!root.isValid()) {
      return false;
    }
    saved = root.filePath(u"saved"_s);
    moved = root.filePath(u"moved"_s);
    for (const QString& folder : {saved, moved}) {
      for (const QString& sub : {u"data/one"_s, u"data/two"_s}) {
        if (!QDir().mkpath(folder + u'/' + sub)) {
          return false;
        }
      }
    }
    return true;
  }
};

// Eight float32 xyz points at t = 1 s, as a writer would publish them.
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
  cloud.timestamp_ns = 1'000'000'000LL;
  return PJ::serializePointCloud(cloud);
}

std::vector<uint8_t> smallImage() {
  const std::vector<uint8_t> pixels(4 * 4 * 4, 128);
  PJ::sdk::Image image;
  image.timestamp_ns = 1'000'000'000LL;
  image.width = 4;
  image.height = 4;
  image.encoding = "rgba8";
  image.data = PJ::Span<const uint8_t>(pixels.data(), pixels.size());
  return PJ::serializeImage(image);
}

// The first descendant of `parent_tag` elements named `tag` whose `attr` equals `value`.
QDomElement findByAttribute(
    const QDomDocument& doc, const QString& parent_tag, const QString& tag, const QString& attr, const QString& value) {
  const QDomNodeList parents = doc.elementsByTagName(parent_tag);
  for (int p = 0; p < parents.size(); ++p) {
    const QDomNodeList kids = parents.at(p).toElement().elementsByTagName(tag);
    for (int k = 0; k < kids.size(); ++k) {
      if (kids.at(k).toElement().attribute(attr) == value) {
        return kids.at(k).toElement();
      }
    }
  }
  return {};
}

class LayoutDatasetPathsTest : public ::testing::Test {
 protected:
  QTemporaryDir extensions_dir;
  PJ::MainWindow window{extensions_dir.path()};
  Folders folders;
  std::vector<QString> reported_;  ///< "<diagnostic id>: <message>", in report order

  void SetUp() override {
    QObject::connect(
        window.diagnosticBridge(), &PJ::QtDiagnosticBridge::diagnosticReported, &window,
        [this](int /*level*/, const QString& /*source*/, const QString& id, const QString& message) {
          reported_.push_back(id + u": "_s + message);
        });
  }

  [[nodiscard]] PJ::AppSession& app() {
    return Peer::session(window);
  }

  /// The generator-restore-failed reports so far; the bridge delivers them queued.
  [[nodiscard]] std::vector<QString> generatorFailures() {
    QCoreApplication::processEvents();
    std::vector<QString> failures;
    for (const QString& report : reported_) {
      if (report.startsWith(u"generator-restore-failed: "_s)) {
        failures.push_back(report);
      }
    }
    return failures;
  }

  /// A dataset with no registered source path carrying one two-sample series under `topic`.
  [[nodiscard]] PJ::DatasetId addPathlessDataset(const char* source_name, const char* topic) {
    const PJ::DatasetId dataset = pj_test::createDataset(app(), source_name);
    if (dataset == 0 || pj_test::addScalarTopic(app(), dataset, topic) == 0) {
      return 0;
    }
    return dataset;
  }

#ifdef PJ_WITH_SCENE3D
  void checkBundledUrdfFollowsLayout(bool include_data_source);

  /// Every live robot layer, across all scene docks.
  [[nodiscard]] std::vector<pj::scene3d::RobotModelLayer*> robotLayers() {
    std::vector<pj::scene3d::RobotModelLayer*> robots;
    for (auto* scene : window.findChildren<PJ::Scene3DDockWidget*>()) {
      if (scene->sceneView() == nullptr) {
        continue;
      }
      for (auto* layer : scene->sceneView()->layers()) {
        if (auto* robot = dynamic_cast<pj::scene3d::RobotModelLayer*>(layer)) {
          robots.push_back(robot);
        }
      }
    }
    return robots;
  }
#endif

  [[nodiscard]] PJ::DatasetId addDataset(const char* source_name, const QString& path) {
    const PJ::DatasetId dataset = pj_test::createDataset(app(), source_name);
    if (dataset == 0 || pj_test::addScalarTopic(app(), dataset, kTopic) == 0) {
      return 0;
    }
    app().sessionManager().setDatasetSourcePath(dataset, path);
    return dataset;
  }

  struct SceneWorkspace {
    PJ::DatasetId dataset = 0;
    PJ::DatasetId outside = 0;
    PJ::ObjectTopicId points{};
    PJ::ObjectTopicId camera{};
  };

  /// A source-bound session: `dataset` under saved/data/one holds the scene topics and `sensor`;
  /// `far` lives outside the layout folder. Docks: a 3D scene (cloud + TF), a 2D scene (camera) and a
  /// State Transitions strip over both datasets.
  [[nodiscard]] SceneWorkspace buildSceneWorkspace() {
    SceneWorkspace ws;
    ws.dataset = addDataset("run.mcap", folders.saved + u"/data/one/run.mcap"_s);
    EXPECT_NE(ws.dataset, 0U);
    auto& store = app().sessionManager().objectStore();
    const auto register_topic = [&](const char* name, const char* metadata) {
      const auto id = store.registerTopic(
          PJ::ObjectTopicDescriptor{.dataset_id = ws.dataset, .topic_name = name, .metadata_json = metadata});
      EXPECT_TRUE(id.has_value()) << name;
      return id.has_value() ? *id : PJ::ObjectTopicId{};
    };
    ws.points = register_topic("/points", R"({"builtin_object_type":"kPointCloud"})");
    const PJ::ObjectTopicId tf = register_topic("/tf", R"({"builtin_object_type":"kFrameTransforms"})");
    ws.camera = register_topic("/camera", R"({"builtin_object_type":"kImage","image_codec":"pj_image_v1"})");
    EXPECT_TRUE(store.pushOwned(ws.points, 1'000'000'000LL, smallCloud()).has_value());
    EXPECT_TRUE(store.pushOwned(ws.camera, 1'000'000'000LL, smallImage()).has_value());
    app().catalogModel().rebuildFromDatastore();

    ws.outside = pj_test::createDataset(app(), "far.mcap");
    EXPECT_NE(pj_test::addScalarTopic(app(), ws.outside, "far"), 0U);
    EXPECT_TRUE(QDir().mkpath(folders.root.filePath(u"elsewhere"_s)));
    app().sessionManager().setDatasetSourcePath(ws.outside, folders.root.filePath(u"elsewhere/far.mcap"_s));

#ifdef PJ_WITH_SCENE3D
    EXPECT_NE(newObjectDock(u"scene3d"_s, PJ::VisualizationKind::kScene3D), nullptr);
    auto* scene3d = widgetIn<PJ::Scene3DDockWidget>(u"scene3d"_s);
    EXPECT_NE(scene3d, nullptr);
    if (scene3d != nullptr) {
      EXPECT_TRUE(scene3d->addTopic(ws.points, PJ::sdk::BuiltinObjectType::kPointCloud, u"points"_s));
      EXPECT_TRUE(scene3d->addTopic(tf, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
    }
#endif
#ifdef PJ_WITH_SCENE2D
    EXPECT_NE(newObjectDock(u"scene2d"_s, PJ::VisualizationKind::kScene2D), nullptr);
    auto* scene2d = widgetIn<PJ::Scene2DDockWidget>(u"scene2d"_s);
    EXPECT_NE(scene2d, nullptr);
    if (scene2d != nullptr) {
      EXPECT_TRUE(scene2d->addTopic(ws.camera, PJ::sdk::BuiltinObjectType::kImage, u"camera"_s));
    }
#endif
    EXPECT_NE(newObjectDock(u"states"_s, PJ::VisualizationKind::kStateTransitions), nullptr);
    auto* states = widgetIn<PJ::StateTransitionsDockWidget>(u"states"_s);
    EXPECT_NE(states, nullptr);
    if (states != nullptr) {
      EXPECT_TRUE(states->tryAcceptSeriesKeys({curveKey(ws.dataset, u"sensor"_s), curveKey(ws.outside, u"far"_s)}));
    }
    return ws;
  }

  [[nodiscard]] bool restore(Processor which, PJ::DatasetId dataset_id, const QString& source_name) {
    const QDomDocument doc = processorLayout(which, dataset_id, source_name);
    return Peer::restoreDataProcessors(window, doc.documentElement());
  }

  [[nodiscard]] std::vector<PJ::BatchEngine::GeneratorRecipe> generators() {
    return app().sessionManager().batchEngine().recipes();
  }

  [[nodiscard]] std::vector<PJ::DataProcessorService::TransformRecipe> transforms() {
    return app().sessionManager().dataProcessorService().transformRecipes();
  }

  [[nodiscard]] QString curveKey(PJ::DatasetId dataset, const QString& topic) {
    const auto curves = app().catalogModel().curves();
    const auto it = std::find_if(curves.begin(), curves.end(), [&](const PJ::CurveDescriptor& curve) {
      return curve.dataset_id == dataset && curve.topic_name == topic;
    });
    return it != curves.end() ? it->name : QString();
  }

  [[nodiscard]] PJ::TabbedPlotWidget* tabs() const {
    return window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  }

  /// A new tab whose first dock became an object-family dock of `kind`.
  [[nodiscard]] PJ::PlotDocker* newObjectDock(const QString& tab, PJ::VisualizationKind kind) {
    PJ::PlotDocker* docker = tabs() != nullptr ? tabs()->addTab(tab) : nullptr;
    PJ::DockWidget* dock = docker != nullptr ? docker->plotAt(0) : nullptr;
    if (dock == nullptr) {
      return nullptr;
    }
    emit docker->objectFamilyRequested(dock, kind);
    return docker;
  }

  template <typename W>
  [[nodiscard]] W* widgetIn(const QString& tab) const {
    PJ::PlotDocker* docker =
        tabs() != nullptr ? tabs()->findDocker([&tab](PJ::PlotDocker* d) { return d->name() == tab; }) : nullptr;
    if (docker == nullptr || docker->plotCount() == 0 || docker->plotAt(0)->objectWidget() == nullptr) {
      return nullptr;
    }
    return qobject_cast<W*>(docker->plotAt(0)->objectWidget()->widget());
  }

  void loadAndSettle(const QString& path) {
    QSignalSpy settled(&window, &PJ::MainWindow::layoutRestoreSettled);
    Peer::loadLayout(window, path);
    EXPECT_TRUE(pj_app_test::pumpUntil([&] { return !settled.isEmpty(); }, 10'000));
  }

  // Replaces the loaded data with a differently named recording of the same shape.
  [[nodiscard]] PJ::DatasetId swapForOtherRecording(PJ::DatasetId old_dataset, const QString& path) {
    Peer::removeDatasetData(window, old_dataset);
    return addDataset("other.csv", path);
  }
};

// A layout saved from a real file load, then replayed after its folder moved or
// was copied. The echo source records what the host hands it, so the tests see
// the path the plugin actually opened.
class LayoutRelocatedSourceTest : public ::testing::Test {
 protected:
  QTemporaryDir extensions_dir;
  QTemporaryDir root_dir;
  PJ::MainWindow window{extensions_dir.path()};

  void SetUp() override {
    ASSERT_TRUE(root_dir.isValid());
    ASSERT_TRUE(pj_app_test::registerFilepathEcho(Peer::session(window).extensionCatalog().pluginCatalog()));
    root_ = QDir::cleanPath(root_dir.path());
  }

  [[nodiscard]] PJ::AppSession& app() {
    return Peer::session(window);
  }

  // Loads A/data/run.echo through the loader and saves A/layout.pj4.xml, then
  // drops the dataset so the replay has to load the file again.
  void saveLayoutFromLoadedFile(const QStringList& lines_a) {
    data_a_ = pj_app_test::writeEchoFile(root_ + u"/A/data/run.echo"_s, lines_a);
    PJ::LoadHints hints;
    hints.expected_plugin_id = u"Filepath Echo Source"_s;
    hints.preset_config_json = pj_app_test::compactJson({{u"filepath"_s, data_a_}});
    hints.dialog_policy = PJ::DialogPolicy::kPreferPreset;
    hints.require_expected_plugin = true;
    ASSERT_TRUE(pj_app_test::loadAndWait(Peer::fileLoader(window), data_a_, hints));
    Peer::saveLayout(window, root_ + u"/A/layout.pj4.xml"_s, /*include_data_source=*/true);
    for (const PJ::DatasetId id : app().sessionManager().createReader().listDatasets()) {
      Peer::removeDatasetData(window, id);
    }
    pj_app_test::resetFilepathEcho();
  }

  void loadLayoutAndSettle(const QString& layout_path) {
    QSignalSpy settled(&window, &PJ::MainWindow::layoutRestoreSettled);
    Peer::loadLayout(window, layout_path);
    ASSERT_TRUE(pj_app_test::pumpUntil([&settled] { return !settled.isEmpty(); }));
  }

  // The values of the topic of the dataset whose source path lies under `folder`.
  [[nodiscard]] std::vector<double> valuesLoadedFrom(const QString& folder) {
    const PJ::DataReader reader = app().sessionManager().createReader();
    for (const PJ::DatasetId id : reader.listDatasets()) {
      const QString source = app().sessionManager().datasetSourcePath(id);
      if (!source.startsWith(folder + u'/')) {
        continue;
      }
      return pj_app_test::singleTopicValues(reader, id);
    }
    return {};
  }

  [[nodiscard]] static QString layoutPluginConfigText(const QString& layout_path, QString* filepath_mode = nullptr) {
    const QDomDocument doc = readXml(layout_path);
    const QDomElement plugin = firstElement(doc, u"plugin"_s);
    if (filepath_mode != nullptr) {
      *filepath_mode = plugin.attribute(u"filepath_mode"_s);
    }
    return plugin.text();
  }

  QString root_;
  QString data_a_;
};

TEST_F(LayoutRelocatedSourceTest, ReplayAfterMovingTheFolderLoadsTheMovedFile) {
  saveLayoutFromLoadedFile({u"1"_s, u"2"_s, u"3"_s});
  ASSERT_TRUE(QDir().rename(root_ + u"/A"_s, root_ + u"/B"_s));

  loadLayoutAndSettle(root_ + u"/B/layout.pj4.xml"_s);

  const auto probe = pj_app_test::filepathEchoProbe();
  ASSERT_FALSE(probe.opened.empty());
  // Canonical on both sides: macOS's temp dir is /var, a symlink to /private/var.
  EXPECT_EQ(
      QFileInfo(QString::fromStdString(probe.opened.back())).canonicalFilePath(),
      QFileInfo(root_ + u"/B/data/run.echo"_s).canonicalFilePath());
  EXPECT_EQ(valuesLoadedFrom(QFileInfo(root_ + u"/B"_s).canonicalFilePath()), (std::vector<double>{1, 2, 3}));
  const QJsonObject last = pj_app_test::parseJsonObject(probe.configs.back());
  EXPECT_EQ(last.value(u"__pj_saved_filepath"_s).toString(), data_a_);
}

TEST_F(LayoutRelocatedSourceTest, ReplayOfACopiedFolderReadsTheCopyNotTheOriginal) {
  saveLayoutFromLoadedFile({u"1"_s, u"2"_s, u"3"_s});
  ASSERT_TRUE(QDir().mkpath(root_ + u"/B/data"_s));
  ASSERT_TRUE(QFile::copy(root_ + u"/A/layout.pj4.xml"_s, root_ + u"/B/layout.pj4.xml"_s));
  const QString data_b = pj_app_test::writeEchoFile(root_ + u"/B/data/run.echo"_s, {u"7"_s, u"8"_s});

  loadLayoutAndSettle(root_ + u"/B/layout.pj4.xml"_s);

  const auto probe = pj_app_test::filepathEchoProbe();
  ASSERT_FALSE(probe.opened.empty());
  EXPECT_EQ(
      QFileInfo(QString::fromStdString(probe.opened.back())).canonicalFilePath(),
      QFileInfo(data_b).canonicalFilePath());
  EXPECT_EQ(valuesLoadedFrom(QFileInfo(root_ + u"/B"_s).canonicalFilePath()), (std::vector<double>{7, 8}));
  EXPECT_EQ(QStringLiteral("1\n2\n3"), QString::fromUtf8(([&] {
              QFile file(data_a_);
              return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
            })()))
      << "the original folder's data must stay untouched";
}

// A replay that reuses a dataset the session already holds must record the file it
// reused: a later Reload replays the recorded config verbatim.
TEST_F(LayoutRelocatedSourceTest, ReloadAfterAReplayReusedADatasetOpensItsOwnFile) {
  saveLayoutFromLoadedFile({u"1"_s, u"2"_s, u"3"_s});
  ASSERT_TRUE(QDir().mkpath(root_ + u"/B/data"_s));
  ASSERT_TRUE(QFile::copy(root_ + u"/A/layout.pj4.xml"_s, root_ + u"/B/layout.pj4.xml"_s));
  const QString data_b = pj_app_test::writeEchoFile(root_ + u"/B/data/run.echo"_s, {u"7"_s, u"8"_s});
  const PJ::DatasetId dataset = pj_test::createDataset(app(), "run.echo");
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app(), dataset, "echo"), 0U);
  app().sessionManager().setDatasetSourcePath(dataset, data_b);

  loadLayoutAndSettle(root_ + u"/B/layout.pj4.xml"_s);
  ASSERT_TRUE(pj_app_test::filepathEchoProbe().opened.empty()) << "the replay must reuse the dataset, not load it";

  QSignalSpy reloaded(&Peer::fileLoader(window), &PJ::FileLoader::fileLoaded);
  Peer::reloadDataset(window, dataset);
  ASSERT_TRUE(pj_app_test::pumpUntil([&reloaded] { return !reloaded.isEmpty(); }));

  const auto probe = pj_app_test::filepathEchoProbe();
  ASSERT_FALSE(probe.opened.empty());
  EXPECT_EQ(
      QFileInfo(QString::fromStdString(probe.opened.back())).canonicalFilePath(),
      QFileInfo(data_b).canonicalFilePath());
}

TEST_F(LayoutRelocatedSourceTest, DesktopLayoutMarksThePluginFilepathAsSourceBound) {
  saveLayoutFromLoadedFile({u"1"_s});
  QString mode;
  (void)layoutPluginConfigText(root_ + u"/A/layout.pj4.xml"_s, &mode);
  EXPECT_EQ(mode, u"source"_s);
}

TEST_F(LayoutRelocatedSourceTest, ResavedLayoutCarriesNoSavedFilepath) {
  saveLayoutFromLoadedFile({u"1"_s, u"2"_s});
  ASSERT_TRUE(QDir().rename(root_ + u"/A"_s, root_ + u"/B"_s));
  loadLayoutAndSettle(root_ + u"/B/layout.pj4.xml"_s);

  Peer::saveLayout(window, root_ + u"/B/resaved.pj4.xml"_s, /*include_data_source=*/true);
  const QString config = layoutPluginConfigText(root_ + u"/B/resaved.pj4.xml"_s);
  EXPECT_FALSE(config.isEmpty());
  EXPECT_FALSE(config.contains(u"__pj_saved_filepath"_s)) << config.toStdString();
}

// Moving the layout together with its data keeps the generator on its dataset: the
// file stores the dataset path relative to the layout, like a curve's.
TEST_F(LayoutDatasetPathsTest, GeneratorFollowsItsDatasetWhenLayoutAndDataMove) {
  ASSERT_TRUE(folders.create());
  const PJ::DatasetId dataset = addDataset("run.csv", folders.saved + u"/data/one/run.csv"_s);
  ASSERT_NE(dataset, 0U);
  ASSERT_TRUE(restore(Processor::kGenerator, dataset, u"run.csv"_s));
  ASSERT_EQ(generators().size(), 1U);

  Peer::saveLayout(window, folders.saved + u"/layout.pj4.xml"_s, /*include_data_source=*/true);
  const QDomElement saved = firstElement(readXml(folders.saved + u"/layout.pj4.xml"_s), u"generator"_s);
  ASSERT_FALSE(saved.isNull());
  EXPECT_EQ(saved.attribute(u"dataset_path"_s).toStdString(), "data/one/run.csv")
      << "a source-bound layout stores the generator's dataset path relative to the layout";

  app().sessionManager().setDatasetSourcePath(dataset, folders.moved + u"/data/one/run.csv"_s);
  ASSERT_TRUE(QFile::copy(folders.saved + u"/layout.pj4.xml"_s, folders.moved + u"/layout.pj4.xml"_s));
  Peer::loadLayout(window, folders.moved + u"/layout.pj4.xml"_s);

  const auto restored = generators();
  ASSERT_EQ(restored.size(), 1U) << "the generator was dropped instead of following its dataset";
  EXPECT_EQ(restored.front().dataset_id, dataset);
}

// Only a layout carrying stream records is written as schema 5 (layout_xml::writeStreamingSources).
// Source-bound and generic saves of plots and files stay on 4, so an older build does not warn on them.
TEST_F(LayoutDatasetPathsTest, PlainSavesStayOnSchemaFour) {
  ASSERT_TRUE(folders.create());
  ASSERT_NE(addDataset("run.csv", folders.saved + u"/data/one/run.csv"_s), 0U);
  for (const bool include_data_source : {true, false}) {
    const QString path = folders.saved + (include_data_source ? u"/source.pj4.xml"_s : u"/generic.pj4.xml"_s);
    Peer::saveLayout(window, path, include_data_source);
    EXPECT_EQ(readXml(path).documentElement().attribute(u"pj4_version"_s).toStdString(), "4") << qPrintable(path);
  }
}

// Two same-named datasets: the generator's path, not its (swapped) id, picks the
// dataset it was saved on.
TEST_F(LayoutDatasetPathsTest, GeneratorResolvesByPathAmongSameNamedDatasets) {
  ASSERT_TRUE(folders.create());
  const PJ::DatasetId first = addDataset("run.csv", folders.saved + u"/data/one/run.csv"_s);
  const PJ::DatasetId second = addDataset("run.csv", folders.saved + u"/data/two/run.csv"_s);
  ASSERT_NE(first, 0U);
  ASSERT_NE(second, 0U);
  ASSERT_TRUE(restore(Processor::kGenerator, second, u"run.csv"_s));

  Peer::saveLayout(window, folders.saved + u"/layout.pj4.xml"_s, /*include_data_source=*/true);
  // The files reload in the opposite order: each id now stands for the other path.
  app().sessionManager().setDatasetSourcePath(first, folders.moved + u"/data/two/run.csv"_s);
  app().sessionManager().setDatasetSourcePath(second, folders.moved + u"/data/one/run.csv"_s);
  ASSERT_TRUE(QFile::copy(folders.saved + u"/layout.pj4.xml"_s, folders.moved + u"/layout.pj4.xml"_s));
  Peer::loadLayout(window, folders.moved + u"/layout.pj4.xml"_s);

  const auto restored = generators();
  ASSERT_EQ(restored.size(), 1U);
  EXPECT_EQ(restored.front().dataset_id, first) << "must follow the path data/two/run.csv, not the saved id";
}

// The session stores a dataset path canonically, so a layout reached through a symlinked
// folder (macOS reaches its temp dir as /var, a link to /private/var) must compare its own
// folder canonically too, or it stores an absolute path that breaks when the data moves.
TEST_F(LayoutDatasetPathsTest, GeneratorPathStaysLayoutRelativeThroughASymlinkedFolder) {
  ASSERT_TRUE(folders.create());
  const QString alias = folders.root.filePath(u"alias"_s);
  if (!QFile::link(folders.saved, alias) || !QFileInfo::exists(alias + u"/data/one"_s)) {
    GTEST_SKIP() << "no traversable directory symlink on this platform";
  }
  const PJ::DatasetId dataset = addDataset("run.csv", alias + u"/data/one/run.csv"_s);
  ASSERT_NE(dataset, 0U);
  ASSERT_TRUE(restore(Processor::kGenerator, dataset, u"run.csv"_s));

  Peer::saveLayout(window, alias + u"/layout.pj4.xml"_s, /*include_data_source=*/true);
  const QDomElement saved = firstElement(readXml(alias + u"/layout.pj4.xml"_s), u"generator"_s);
  ASSERT_FALSE(saved.isNull());
  EXPECT_EQ(saved.attribute(u"dataset_path"_s).toStdString(), "data/one/run.csv");

  Peer::loadLayout(window, alias + u"/layout.pj4.xml"_s);
  const auto restored = generators();
  ASSERT_EQ(restored.size(), 1U);
  EXPECT_EQ(restored.front().dataset_id, dataset);
}

// A generic layout names no dataset; the generator binds to the loaded recording
// that carries its inputs, as an unqualified curve does.
TEST_F(LayoutDatasetPathsTest, GenericLayoutCarriesNoGeneratorDatasetAndBindsToTheLoadedRecording) {
  ASSERT_TRUE(folders.create());
  const PJ::DatasetId dataset = addDataset("run.csv", folders.saved + u"/data/one/run.csv"_s);
  ASSERT_NE(dataset, 0U);
  ASSERT_TRUE(restore(Processor::kGenerator, dataset, u"run.csv"_s));

  Peer::saveLayout(window, folders.saved + u"/generic.pj4.xml"_s, /*include_data_source=*/false);
  const QDomElement saved = firstElement(readXml(folders.saved + u"/generic.pj4.xml"_s), u"generator"_s);
  ASSERT_FALSE(saved.isNull());
  EXPECT_FALSE(saved.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(saved.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(saved.hasAttribute(u"dataset_path"_s));

  const PJ::DatasetId other = swapForOtherRecording(dataset, folders.moved + u"/data/one/other.csv"_s);
  ASSERT_NE(other, 0U);
  Peer::loadLayout(window, folders.saved + u"/generic.pj4.xml"_s);

  const auto restored = generators();
  ASSERT_EQ(restored.size(), 1U) << "the generic layout's generator found no dataset";
  EXPECT_EQ(restored.front().dataset_id, other);
  EXPECT_TRUE(pj_test::publishedMarkers(app(), other, kSeriesKey, kGeneratorId).has_value());
}

// An unqualified generator never lands on a dataset by load order: two recordings
// carrying its input leave it unbound.
TEST_F(LayoutDatasetPathsTest, GenericLayoutGeneratorStaysUnboundWhenTwoRecordingsCarryItsInput) {
  ASSERT_TRUE(folders.create());
  const PJ::DatasetId dataset = addDataset("run.csv", folders.saved + u"/data/one/run.csv"_s);
  ASSERT_NE(dataset, 0U);
  ASSERT_TRUE(restore(Processor::kGenerator, dataset, u"run.csv"_s));
  Peer::saveLayout(window, folders.saved + u"/generic.pj4.xml"_s, /*include_data_source=*/false);

  ASSERT_NE(addDataset("other.csv", folders.saved + u"/data/two/other.csv"_s), 0U);
  Peer::loadLayout(window, folders.saved + u"/generic.pj4.xml"_s);

  EXPECT_TRUE(generators().empty());
  const std::vector<QString> failures = generatorFailures();
  ASSERT_EQ(failures.size(), 1U);
  EXPECT_TRUE(failures.front().contains(QString::fromLatin1(kGeneratorId))) << qPrintable(failures.front());
  EXPECT_TRUE(failures.front().contains(u"several loaded recordings"_s)) << qPrintable(failures.front());
  EXPECT_FALSE(failures.front().contains(u"qualify"_s)) << "a layout user has no input to qualify";
}

// A layout restores its transforms before its generators, so a generator that reads a
// transform's output must find that output when it picks (or reads) its dataset.
TEST_F(LayoutDatasetPathsTest, SourceBoundGeneratorReadingATransformOutputIsRestored) {
  const PJ::DatasetId dataset = addPathlessDataset("run.csv", kTopic);
  ASSERT_NE(dataset, 0U);
  QDomDocument both = processorLayout(Processor::kTransform, dataset, u"run.csv"_s);
  QDomElement processors = both.documentElement().firstChildElement(u"data_processors"_s);
  appendGenerator(both, processors, dataset, u"run.csv"_s, QString::fromLatin1(kOutputSeriesKey));

  EXPECT_TRUE(Peer::restoreDataProcessors(window, both.documentElement()));

  ASSERT_EQ(transforms().size(), 1U);
  const auto restored = generators();
  ASSERT_EQ(restored.size(), 1U) << "the generator over the transform's output was dropped";
  EXPECT_EQ(restored.front().dataset_id, dataset);
}

TEST_F(LayoutDatasetPathsTest, GenericLayoutBindsAGeneratorReadingATransformOutputToTheRecordingItRanOn) {
  const PJ::DatasetId other = addPathlessDataset("other.csv", kTopic);
  ASSERT_NE(other, 0U);
  QDomDocument both = processorLayout(Processor::kTransform, 0, QString());
  QDomElement processors = both.documentElement().firstChildElement(u"data_processors"_s);
  QDomElement transform_input = firstElement(both, u"transform"_s).firstChildElement(u"input"_s);
  transform_input.removeAttribute(u"dataset_id"_s);
  transform_input.removeAttribute(u"dataset_source"_s);
  QDomElement gen = appendGenerator(both, processors, 0, QString(), QString::fromLatin1(kOutputSeriesKey));
  stripDatasetAttributes(gen);

  EXPECT_TRUE(Peer::restoreDataProcessors(window, both.documentElement()));

  ASSERT_EQ(transforms().size(), 1U);
  const auto restored = generators();
  ASSERT_EQ(restored.size(), 1U) << "the generic layout's generator found no dataset for the transform output";
  EXPECT_EQ(restored.front().dataset_id, other);
  EXPECT_TRUE(generatorFailures().empty());
}

// One rule saved on two datasets with different scripts, reopened with only one dataset
// loaded: both elements resolve to it, and BatchEngine holds one binding per (id, dataset).
TEST_F(LayoutDatasetPathsTest, GenericLayoutBindingsOfOneRuleThatDifferAndShareADatasetAreReported) {
  const PJ::DatasetId dataset = addPathlessDataset("run.csv", kTopic);
  ASSERT_NE(dataset, 0U);
  QDomElement processors;
  QDomDocument doc = newLayout(processors);
  for (const QString& label : {u"on-first"_s, u"on-second"_s}) {
    QDomElement gen = appendGenerator(doc, processors, 0, QString(), QString::fromLatin1(kSeriesKey), label);
    stripDatasetAttributes(gen);
  }

  EXPECT_TRUE(Peer::restoreDataProcessors(window, doc.documentElement()));

  const auto restored = generators();
  ASSERT_EQ(restored.size(), 1U);
  EXPECT_EQ(restored.front().dataset_id, dataset);
  EXPECT_NE(restored.front().script.find("on-first"), std::string::npos) << "the first binding must not be replaced";
  const std::vector<QString> failures = generatorFailures();
  ASSERT_EQ(failures.size(), 1U) << "the dropped binding was not reported";
  EXPECT_TRUE(failures.front().contains(QString::fromLatin1(kGeneratorId))) << qPrintable(failures.front());
  EXPECT_TRUE(failures.front().contains(u"different"_s)) << qPrintable(failures.front());
}

TEST_F(LayoutDatasetPathsTest, GenericLayoutIdenticalBindingsOfOneRuleCoalesceSilently) {
  const PJ::DatasetId dataset = addPathlessDataset("run.csv", kTopic);
  ASSERT_NE(dataset, 0U);
  QDomElement processors;
  QDomDocument doc = newLayout(processors);
  for (int copy = 0; copy < 2; ++copy) {
    QDomElement gen = appendGenerator(doc, processors, 0, QString());
    stripDatasetAttributes(gen);
  }

  EXPECT_TRUE(Peer::restoreDataProcessors(window, doc.documentElement()));

  const auto restored = generators();
  ASSERT_EQ(restored.size(), 1U);
  EXPECT_EQ(restored.front().dataset_id, dataset);
  EXPECT_TRUE(generatorFailures().empty());
}

// A persisted input name is the literal series key. "b:/speed/value" is a series of
// dataset A even though another loaded dataset is called "b".
TEST_F(LayoutDatasetPathsTest, GenericLayoutGeneratorInputWithAColonIsReadLiterally) {
  const PJ::DatasetId carrier = addPathlessDataset("a", "b:/speed");
  const PJ::DatasetId namesake = addPathlessDataset("b", "/speed");
  ASSERT_NE(carrier, 0U);
  ASSERT_NE(namesake, 0U);
  QDomElement processors;
  QDomDocument doc = newLayout(processors);
  QDomElement gen = appendGenerator(doc, processors, 0, QString(), u"b:/speed/value"_s);
  stripDatasetAttributes(gen);

  EXPECT_TRUE(Peer::restoreDataProcessors(window, doc.documentElement()));

  const auto restored = generators();
  ASSERT_EQ(restored.size(), 1U) << "the literal key was read as dataset 'b' plus '/speed/value'";
  EXPECT_EQ(restored.front().dataset_id, carrier);
  EXPECT_EQ(restored.front().inputs, std::vector<std::string>{"b:/speed/value"});
}

// DerivedObjects::save writes dataset_source whenever the dataset has a source name, so an
// empty one is a record without information, not a dataset the generator names.
TEST_F(LayoutDatasetPathsTest, GeneratorWithAnEmptySourceNamesNoDatasetAndBindsByItsInputs) {
  const PJ::DatasetId dataset = addPathlessDataset("run.csv", kTopic);
  ASSERT_NE(dataset, 0U);
  QDomElement processors;
  QDomDocument doc = newLayout(processors);
  QDomElement gen = appendGenerator(doc, processors, 0, QString());
  gen.removeAttribute(u"dataset_id"_s);
  ASSERT_TRUE(gen.hasAttribute(u"dataset_source"_s));

  EXPECT_TRUE(Peer::restoreDataProcessors(window, doc.documentElement()));

  const auto restored = generators();
  ASSERT_EQ(restored.size(), 1U);
  EXPECT_EQ(restored.front().dataset_id, dataset);
}

// A source-bound layout saved from a dataset that has no file path loses the volatile id
// (removeUnvalidatedDatasetIds) and keeps the source label. That label alone picks the
// dataset, even though another loaded dataset carries the same series.
TEST_F(LayoutDatasetPathsTest, SourceOnlyGeneratorRebindsBySourceAmongRecordingsCarryingItsInput) {
  ASSERT_NE(addPathlessDataset("other.csv", kTopic), 0U);
  const PJ::DatasetId named = addPathlessDataset("run.csv", kTopic);
  ASSERT_NE(named, 0U);
  QDomElement processors;
  QDomDocument doc = newLayout(processors);
  QDomElement gen = appendGenerator(doc, processors, 0, u"run.csv"_s);
  gen.removeAttribute(u"dataset_id"_s);

  EXPECT_TRUE(Peer::restoreDataProcessors(window, doc.documentElement()));

  const auto restored = generators();
  ASSERT_EQ(restored.size(), 1U);
  EXPECT_EQ(restored.front().dataset_id, named);
  EXPECT_TRUE(generatorFailures().empty());
}

// A transform input stores its dataset path relative to the layout, and the path
// (not the swapped id) picks the dataset among same-named recordings.
TEST_F(LayoutDatasetPathsTest, TransformInputResolvesByPathAmongSameNamedDatasets) {
  ASSERT_TRUE(folders.create());
  const PJ::DatasetId first = addDataset("run.csv", folders.saved + u"/data/one/run.csv"_s);
  const PJ::DatasetId second = addDataset("run.csv", folders.saved + u"/data/two/run.csv"_s);
  ASSERT_NE(first, 0U);
  ASSERT_NE(second, 0U);
  ASSERT_TRUE(restore(Processor::kTransform, second, u"run.csv"_s));
  ASSERT_EQ(transforms().size(), 1U);

  Peer::saveLayout(window, folders.saved + u"/layout.pj4.xml"_s, /*include_data_source=*/true);
  const QDomElement saved_input =
      firstElement(readXml(folders.saved + u"/layout.pj4.xml"_s), u"transform"_s).firstChildElement(u"input"_s);
  ASSERT_FALSE(saved_input.isNull());
  EXPECT_EQ(saved_input.attribute(u"dataset_path"_s).toStdString(), "data/two/run.csv");

  app().sessionManager().setDatasetSourcePath(first, folders.moved + u"/data/two/run.csv"_s);
  app().sessionManager().setDatasetSourcePath(second, folders.moved + u"/data/one/run.csv"_s);
  ASSERT_TRUE(QFile::copy(folders.saved + u"/layout.pj4.xml"_s, folders.moved + u"/layout.pj4.xml"_s));
  Peer::loadLayout(window, folders.moved + u"/layout.pj4.xml"_s);

  const auto restored = transforms();
  ASSERT_EQ(restored.size(), 1U);
  ASSERT_EQ(restored.front().input_bindings.size(), 1U);
  EXPECT_EQ(restored.front().input_bindings.front().dataset_id, first)
      << "must follow the path data/two/run.csv, not the saved id";
}

TEST_F(LayoutDatasetPathsTest, GenericLayoutCarriesNoTransformDatasetAndBindsToTheLoadedRecording) {
  ASSERT_TRUE(folders.create());
  const PJ::DatasetId dataset = addDataset("run.csv", folders.saved + u"/data/one/run.csv"_s);
  ASSERT_NE(dataset, 0U);
  ASSERT_TRUE(restore(Processor::kTransform, dataset, u"run.csv"_s));

  Peer::saveLayout(window, folders.saved + u"/generic.pj4.xml"_s, /*include_data_source=*/false);
  const QDomElement saved_input =
      firstElement(readXml(folders.saved + u"/generic.pj4.xml"_s), u"transform"_s).firstChildElement(u"input"_s);
  ASSERT_FALSE(saved_input.isNull());
  EXPECT_FALSE(saved_input.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(saved_input.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(saved_input.hasAttribute(u"dataset_path"_s));

  const PJ::DatasetId other = swapForOtherRecording(dataset, folders.moved + u"/data/one/other.csv"_s);
  ASSERT_NE(other, 0U);
  Peer::loadLayout(window, folders.saved + u"/generic.pj4.xml"_s);

  const auto restored = transforms();
  ASSERT_EQ(restored.size(), 1U);
  ASSERT_EQ(restored.front().input_bindings.size(), 1U);
  EXPECT_EQ(restored.front().input_bindings.front().dataset_id, other);
}

// A layout written before the generator path was relative (absolute path) and
// before transform inputs carried one still restores onto the live dataset.
TEST_F(LayoutDatasetPathsTest, OldFormatLayoutStillRestoresGeneratorAndTransform) {
  ASSERT_TRUE(folders.create());
  const QString data_path = folders.saved + u"/data/one/run.csv"_s;
  const PJ::DatasetId dataset = addDataset("run.csv", data_path);
  ASSERT_NE(dataset, 0U);
  QDomDocument both = processorLayout(Processor::kGenerator, dataset, u"run.csv"_s);
  QDomElement processors = both.documentElement().firstChildElement(u"data_processors"_s);
  appendTransform(both, processors, dataset, u"run.csv"_s);
  ASSERT_TRUE(Peer::restoreDataProcessors(window, both.documentElement()));
  ASSERT_EQ(generators().size(), 1U);
  ASSERT_EQ(transforms().size(), 1U);

  const QString layout_path = folders.saved + u"/old.pj4.xml"_s;
  Peer::saveLayout(window, layout_path, /*include_data_source=*/true);
  QDomDocument old_layout = readXml(layout_path);
  QDomElement generator = firstElement(old_layout, u"generator"_s);
  ASSERT_FALSE(generator.isNull());
  generator.setAttribute(u"dataset_path"_s, QDir::cleanPath(data_path));
  QDomElement transform_input = firstElement(old_layout, u"transform"_s).firstChildElement(u"input"_s);
  ASSERT_FALSE(transform_input.isNull());
  transform_input.removeAttribute(u"dataset_path"_s);
  writeXml(layout_path, old_layout);

  Peer::loadLayout(window, layout_path);

  ASSERT_EQ(generators().size(), 1U);
  EXPECT_EQ(generators().front().dataset_id, dataset);
  ASSERT_EQ(transforms().size(), 1U);
  EXPECT_EQ(transforms().front().input_bindings.front().dataset_id, dataset);
}

#if defined(PJ_WITH_SCENE3D) && defined(PJ_WITH_SCENE2D)

// Scene layers, TF config topics and State Transitions rows write the session's absolute path
// themselves; a layout file makes it layout-relative when the data sits beneath the layout folder,
// so moving the folder with its data keeps them bound (a stale absolute path would block that).
TEST_F(LayoutDatasetPathsTest, SceneAndStateTransitionsPathsMoveWithTheLayoutFolder) {
  ASSERT_TRUE(folders.create());
  const SceneWorkspace ws = buildSceneWorkspace();
  ASSERT_NE(ws.dataset, 0U);

  Peer::saveLayout(window, folders.saved + u"/layout.pj4.xml"_s, /*include_data_source=*/true);
  const QDomDocument saved = readXml(folders.saved + u"/layout.pj4.xml"_s);

  const QString relative = u"data/one/run.mcap"_s;
  const QDomElement cloud = findByAttribute(saved, u"scene3d"_s, u"layer"_s, u"topic_name"_s, u"/points"_s);
  ASSERT_FALSE(cloud.isNull());
  EXPECT_EQ(cloud.attribute(u"dataset_path"_s), relative);
  EXPECT_TRUE(cloud.hasAttribute(u"dataset_id"_s)) << "scene ids must survive the file boundary";
  const QDomElement tf = findByAttribute(saved, u"scene3d"_s, u"config_topic"_s, u"topic_name"_s, u"/tf"_s);
  ASSERT_FALSE(tf.isNull());
  EXPECT_EQ(tf.attribute(u"dataset_path"_s), relative);
  const QDomElement camera = findByAttribute(saved, u"scene2d"_s, u"layer"_s, u"topic_name"_s, u"/camera"_s);
  ASSERT_FALSE(camera.isNull());
  EXPECT_EQ(camera.attribute(u"dataset_path"_s), relative);
  const QDomElement local_row = findByAttribute(saved, u"state_transitions"_s, u"series"_s, u"topic"_s, u"sensor"_s);
  ASSERT_FALSE(local_row.isNull());
  EXPECT_EQ(local_row.attribute(u"dataset_path"_s), relative);
  const QDomElement outside_row = findByAttribute(saved, u"state_transitions"_s, u"series"_s, u"topic"_s, u"far"_s);
  ASSERT_FALSE(outside_row.isNull());
  const QString outside_path = app().sessionManager().datasetSourcePath(ws.outside);
  EXPECT_EQ(outside_row.attribute(u"dataset_path"_s), outside_path);
  EXPECT_TRUE(QFileInfo(outside_path).isAbsolute());

  QDomDocument history;
  ASSERT_TRUE(static_cast<bool>(history.setContent(Peer::historySnapshot(window))));
  const QDomElement history_cloud = findByAttribute(history, u"scene3d"_s, u"layer"_s, u"topic_name"_s, u"/points"_s);
  ASSERT_FALSE(history_cloud.isNull());
  const QString absolute = app().sessionManager().datasetSourcePath(ws.dataset);
  EXPECT_EQ(history_cloud.attribute(u"dataset_path"_s), absolute);
  EXPECT_TRUE(QFileInfo(absolute).isAbsolute());

  app().sessionManager().setDatasetSourcePath(ws.dataset, folders.moved + u"/data/one/run.mcap"_s);
  ASSERT_TRUE(QFile::copy(folders.saved + u"/layout.pj4.xml"_s, folders.moved + u"/layout.pj4.xml"_s));
  reported_.clear();
  loadAndSettle(folders.moved + u"/layout.pj4.xml"_s);

  auto* scene3d = widgetIn<PJ::Scene3DDockWidget>(u"scene3d"_s);
  ASSERT_NE(scene3d, nullptr);
  EXPECT_NE(scene3d->layerFor(ws.points), nullptr) << "the cloud layer did not rebind";
  EXPECT_TRUE(scene3d->unresolvedPendingRestores().isEmpty())
      << qPrintable(scene3d->unresolvedPendingRestores().join(u", "_s));
  auto* scene2d = widgetIn<PJ::Scene2DDockWidget>(u"scene2d"_s);
  ASSERT_NE(scene2d, nullptr);
  EXPECT_NE(scene2d->layerFor(ws.camera), nullptr) << "the camera layer did not rebind";
  EXPECT_TRUE(scene2d->unresolvedPendingRestores().isEmpty());
  auto* states = widgetIn<PJ::StateTransitionsDockWidget>(u"states"_s);
  ASSERT_NE(states, nullptr);
  EXPECT_EQ(states->controller()->currentSeries().size(), 2U);
  for (const QString& report : reported_) {
    EXPECT_FALSE(report.startsWith(u"scene_restore_pending: "_s)) << qPrintable(report);
  }
}

#endif  // PJ_WITH_SCENE3D && PJ_WITH_SCENE2D

// A generic layout names no dataset for a State Transitions row either; it binds to the
// unique loaded recording that carries the topic.
TEST_F(LayoutDatasetPathsTest, GenericLayoutCarriesNoStateTransitionsDatasetAndBindsToTheLoadedRecording) {
  ASSERT_TRUE(folders.create());
  const PJ::DatasetId dataset = addDataset("run.csv", folders.saved + u"/data/one/run.csv"_s);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(newObjectDock(u"states"_s, PJ::VisualizationKind::kStateTransitions), nullptr);
  auto* states = widgetIn<PJ::StateTransitionsDockWidget>(u"states"_s);
  ASSERT_NE(states, nullptr);
  ASSERT_TRUE(states->tryAcceptSeriesKeys({curveKey(dataset, u"sensor"_s)}));

  Peer::saveLayout(window, folders.saved + u"/generic.pj4.xml"_s, /*include_data_source=*/false);
  const QDomDocument saved = readXml(folders.saved + u"/generic.pj4.xml"_s);
  const QDomElement row = firstElement(saved, u"series"_s);
  ASSERT_FALSE(row.isNull());
  EXPECT_FALSE(row.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(row.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(row.hasAttribute(u"dataset_path"_s));
  EXPECT_EQ(row.attribute(u"topic"_s), u"sensor"_s);

  const PJ::DatasetId other = swapForOtherRecording(dataset, folders.moved + u"/data/one/other.csv"_s);
  ASSERT_NE(other, 0U);
  loadAndSettle(folders.saved + u"/generic.pj4.xml"_s);

  states = widgetIn<PJ::StateTransitionsDockWidget>(u"states"_s);
  ASSERT_NE(states, nullptr);
  const auto series = states->controller()->currentSeries();
  ASSERT_EQ(series.size(), 1U);
  EXPECT_EQ(series.front().dataset_id, other);
  EXPECT_EQ(series.front().topic_name, u"sensor"_s);
}

#ifdef PJ_WITH_SCENE3D
// Saves a layout holding one URDF beside it and one elsewhere, copies the layout to a
// second folder (the first URDF still exists), loads the copy, and expects the bundled
// URDF to come from the copy's folder while the outside one stays where it was.
void LayoutDatasetPathsTest::checkBundledUrdfFollowsLayout(bool include_data_source) {
  ASSERT_TRUE(folders.create());
  const QString elsewhere = folders.root.filePath(u"elsewhere"_s);
  ASSERT_TRUE(QDir().mkpath(folders.saved + u"/robot"_s));
  ASSERT_TRUE(QDir().mkpath(folders.moved + u"/robot"_s));
  ASSERT_TRUE(QDir().mkpath(elsewhere));
  const auto write_urdf = [](const QString& file, const char* base) {
    QFile out(file);
    ASSERT_TRUE(out.open(QIODevice::WriteOnly));
    out.write(QByteArray("<robot name=\"r\"><link name=\"") + base + "\"/></robot>");
  };
  const QString saved_urdf = folders.saved + u"/robot/robot.urdf"_s;
  const QString moved_urdf = folders.moved + u"/robot/robot.urdf"_s;
  const QString outside_urdf = elsewhere + u"/outside.urdf"_s;
  write_urdf(saved_urdf, "saved_base");
  write_urdf(moved_urdf, "moved_base");
  write_urdf(outside_urdf, "outside_base");

  ASSERT_NE(addPathlessDataset("run.csv", kTopic), 0U);

  auto* tabs = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  ASSERT_NE(tabs, nullptr);
  PJ::PlotDocker* docker = tabs->addTab(u"robot"_s);
  ASSERT_NE(docker, nullptr);
  PJ::DockWidget* dock = docker->plotAt(0);
  ASSERT_NE(dock, nullptr);
  emit docker->objectFamilyRequested(dock, PJ::VisualizationKind::kScene3D);
  auto* scene = qobject_cast<PJ::Scene3DDockWidget*>(dock->objectWidget()->widget());
  ASSERT_NE(scene, nullptr);
  scene->addRobotModelLayer(saved_urdf);
  scene->addRobotModelLayer(outside_urdf);

  const QString saved_layout = folders.saved + u"/layout.pj4.xml"_s;
  Peer::saveLayout(window, saved_layout, include_data_source);

  const QDomDocument saved_doc = readXml(saved_layout);
  QStringList stored;
  const QDomNodeList models = saved_doc.elementsByTagName(u"robot_model"_s);
  for (int i = 0; i < models.size(); ++i) {
    stored.push_back(models.at(i).toElement().attribute(u"source_value"_s));
  }
  stored.sort();
  QStringList expected{outside_urdf, u"robot/robot.urdf"_s};
  expected.sort();
  EXPECT_EQ(stored, expected);
  const auto live = robotLayers();
  EXPECT_EQ(live.size(), 2U);
  for (const auto* robot : live) {
    EXPECT_TRUE(QDir::isAbsolutePath(robot->sourceValue())) << robot->sourceValue().toStdString();
  }

  const QString moved_layout = folders.moved + u"/layout.pj4.xml"_s;
  ASSERT_TRUE(QFile::copy(saved_layout, moved_layout));

  QPointer<PJ::PlotDocker> old(docker);
  Peer::loadLayout(window, moved_layout);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  ASSERT_TRUE(old.isNull());

  std::map<std::string, QString> source_by_link;
  for (auto* robot : robotLayers()) {
    ASSERT_NE(robot->robotModel(), nullptr) << robot->statusText().toStdString();
    ASSERT_FALSE(robot->robotModel()->links.empty());
    source_by_link[robot->robotModel()->links.front().name] = robot->sourceValue();
  }
  ASSERT_EQ(source_by_link.size(), 2U);
  EXPECT_EQ(QFileInfo(source_by_link["moved_base"]).canonicalFilePath(), QFileInfo(moved_urdf).canonicalFilePath());
  EXPECT_EQ(source_by_link["outside_base"], outside_urdf);
}

TEST_F(LayoutDatasetPathsTest, BundledUrdfFollowsLayoutFolderSourceBound) {
  checkBundledUrdfFollowsLayout(true);
}

TEST_F(LayoutDatasetPathsTest, BundledUrdfFollowsLayoutFolderGeneric) {
  checkBundledUrdfFollowsLayout(false);
}

// A package root beneath the layout is stored relative to it, so a re-saved layout's copy reads
// its own meshes; the root outside it stays where it was.
TEST_F(LayoutDatasetPathsTest, PackageRootFollowsACopiedLayoutFolder) {
  ASSERT_TRUE(folders.create());
  const QString saved_urdf = folders.saved + u"/robot.urdf"_s;
  QFile urdf(saved_urdf);
  ASSERT_TRUE(urdf.open(QIODevice::WriteOnly));
  urdf.write("<robot name=\"r\"><link name=\"base\"/></robot>");
  urdf.close();
  ASSERT_NE(addPathlessDataset("run.csv", kTopic), 0U);
  ASSERT_NE(newObjectDock(u"robot"_s, PJ::VisualizationKind::kScene3D), nullptr);
  auto* scene = widgetIn<PJ::Scene3DDockWidget>(u"robot"_s);
  ASSERT_NE(scene, nullptr);
  scene->addRobotModelLayer(saved_urdf);
  ASSERT_EQ(robotLayers().size(), 1U);
  // Relocation compares canonical paths, and only a folder that exists has one
  // when the temp dir is reached through a symlink (macOS /var -> /private/var).
  ASSERT_TRUE(QDir().mkpath(folders.saved + u"/my meshes/pkg"_s));
  const QString outside = QUrl::fromLocalFile(folders.root.filePath(u"elsewhere/pkg/"_s)).toString();
  robotLayers().front()->setPackageUrlRoots(
      {{u"inside"_s, QUrl::fromLocalFile(folders.saved + u"/my meshes/pkg/"_s)}, {u"outside"_s, QUrl(outside)}});

  const QString saved_layout = folders.saved + u"/layout.pj4.xml"_s;
  Peer::saveLayout(window, saved_layout, /*include_data_source=*/false);
  const auto roots_of = [](const QDomDocument& doc) {
    return QJsonDocument::fromJson(firstElement(doc, u"robot_model"_s).attribute(u"package_url_roots"_s).toUtf8())
        .object();
  };
  const QJsonObject stored = roots_of(readXml(saved_layout));
  EXPECT_EQ(stored.value(u"inside"_s).toString().toStdString(), "my%20meshes/pkg/");
  EXPECT_EQ(stored.value(u"outside"_s).toString(), outside);

  const QString moved_layout = folders.moved + u"/layout.pj4.xml"_s;
  ASSERT_TRUE(QFile::copy(saved_layout, moved_layout));
  ASSERT_TRUE(QFile::copy(saved_urdf, folders.moved + u"/robot.urdf"_s));
  Peer::loadLayout(window, moved_layout);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

  QDomDocument history;
  ASSERT_TRUE(static_cast<bool>(history.setContent(Peer::historySnapshot(window))));
  const QJsonObject live = roots_of(history);
  EXPECT_EQ(
      QUrl(live.value(u"inside"_s).toString()).toLocalFile(), QDir::cleanPath(folders.moved) + u"/my meshes/pkg/"_s);
  EXPECT_EQ(live.value(u"outside"_s).toString(), outside);
}
#endif  // PJ_WITH_SCENE3D

}  // namespace
