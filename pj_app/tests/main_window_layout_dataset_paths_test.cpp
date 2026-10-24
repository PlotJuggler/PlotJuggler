// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A marker generator and a transform input name their dataset the way a plotted
// curve does: the layout file carries the dataset's path relative to the layout
// (stampDatasetSourcePaths), a generic layout carries no dataset identity at all
// (removeDatasetQualifiersForGenericLayout), and restore resolves what is left with
// the session's one identity resolver. These tests drive the real save and load
// entry points, moving the dataset's file between save and load.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <optional>
#include <string>
#include <vector>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/SessionManager.h"

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

  [[nodiscard]] PJ::DatasetId addDataset(const char* source_name, const QString& path) {
    const PJ::DatasetId dataset = pj_test::createDataset(app(), source_name);
    if (dataset == 0 || pj_test::addScalarTopic(app(), dataset, kTopic) == 0) {
      return 0;
    }
    app().sessionManager().setDatasetSourcePath(dataset, path);
    return dataset;
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

  // Replaces the loaded data with a differently named recording of the same shape.
  [[nodiscard]] PJ::DatasetId swapForOtherRecording(PJ::DatasetId old_dataset, const QString& path) {
    Peer::removeDatasetData(window, old_dataset);
    return addDataset("other.csv", path);
  }
};

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

}  // namespace
