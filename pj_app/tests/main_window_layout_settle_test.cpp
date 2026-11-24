// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The --exit-after-layout settlement boundary: layoutRestoreSettled fires exactly once per loadLayoutFromPath
// terminal outcome:
//   * an open failure settles false synchronously (the failure record reaches the --dump-diagnostics file);
//   * a plain (no-batch) layout settles true on the sync apply leg;
//   * a materialize-import layout settles true only at the progressive drain, strictly after the batch finished and
//     every restore waiter cleared.
// Also covers what the same entry tells the user: the report naming stream records that cannot start, and warning
// boxes that show their text as text.
//
// Each ctest case runs in its own process (gtest_discover_tests) with its own MainWindow, so settlement observations
// cannot inherit process-global widget state from another case.

#include <gtest/gtest.h>

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QWidget>
#include <algorithm>
#include <memory>
#include <vector>

#include "DiagnosticDump.h"
#include "dataset_test_helpers.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/SessionManager.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"
#include "support/modal_answerer.h"
#include "ui/CurveListPanel.h"

namespace {

using namespace Qt::StringLiterals;
using PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::flushQueuedEvents;
using pj_app_test::pumpUntil;
namespace fake = pj_layout_import_gui;
using Peer = MainWindowLayoutImportTestPeer;

// The --dump-diagnostics file's JSON envelope ({"version", "records"}); empty if unreadable.
QJsonObject readDumpEnvelope(const QString& dump_path) {
  QFile file(dump_path);
  if (!file.open(QIODevice::ReadOnly)) {
    ADD_FAILURE() << "cannot open " << dump_path.toStdString();
    return {};
  }
  return QJsonDocument::fromJson(file.readAll()).object();
}

class MainWindowLayoutSettleTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    extensions_dir_ = std::make_unique<QTemporaryDir>();
    project_dir_ = std::make_unique<QTemporaryDir>();
    ASSERT_TRUE(extensions_dir_->isValid());
    ASSERT_TRUE(project_dir_->isValid());
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_->path());
    ASSERT_TRUE(appSession().extensionCatalog().pluginCatalog().registerStaticToolbox(&fake::kFakeVtable));
  }

  static void TearDownTestSuite() {
    window_.reset();
    project_dir_.reset();
    extensions_dir_.reset();
  }

  [[nodiscard]] static PJ::MainWindow& window() {
    return *window_;
  }
  [[nodiscard]] static PJ::AppSession& appSession() {
    return Peer::session(*window_);
  }

  inline static std::unique_ptr<QTemporaryDir> extensions_dir_;
  inline static std::unique_ptr<QTemporaryDir> project_dir_;
  inline static std::unique_ptr<PJ::MainWindow> window_;
};

// A layout whose data file no plugin handles: FileLoader's failure reaches the diagnostics (and so
// --dump-diagnostics), not only the console.
TEST_F(MainWindowLayoutSettleTest, UnloadableLayoutSourceReachesTheDumpAsLoadFailed) {
  const QString data_path = project_dir_->filePath(QStringLiteral("data.pjbogus"));
  {
    QFile data(data_path);
    ASSERT_TRUE(data.open(QIODevice::WriteOnly));
    data.write("bogus");
  }
  const QString layout_path = project_dir_->filePath(QStringLiteral("unloadable.pj4.xml"));
  {
    QFile layout(layout_path);
    ASSERT_TRUE(layout.open(QIODevice::WriteOnly));
    layout.write(
        "<root binding=\"source\" format=\"PlotJuggler\" pj4_version=\"4\">"
        "<previouslyLoaded_Datafiles><fileInfo filename=\"data.pjbogus\" prefix=\"\"/>"
        "</previouslyLoaded_Datafiles></root>");
  }

  const QString dump_path = project_dir_->filePath(QStringLiteral("unloadable-diag.json"));
  PJ::DiagnosticDump dump(dump_path);
  dump.attachTo(window().diagnosticBridge());

  QObject scope;
  std::vector<bool> settles;
  QObject::connect(
      &window(), &PJ::MainWindow::layoutRestoreSettled, &scope, [&settles](bool ok) { settles.push_back(ok); });

  Peer::loadLayout(window(), layout_path, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&settles]() { return !settles.empty(); }));

  ASSERT_TRUE(dump.write());
  const QJsonArray records = readDumpEnvelope(dump_path).value(QStringLiteral("records")).toArray();
  bool saw_load_failed = false;
  for (const auto& value : records) {
    const QJsonObject record = value.toObject();
    if (record.value(QStringLiteral("id")).toString() == QStringLiteral("data-source-load-failed")) {
      saw_load_failed = true;
      EXPECT_EQ(record.value(QStringLiteral("source")).toString(), QStringLiteral("File Loader"));
      EXPECT_EQ(record.value(QStringLiteral("level")).toString(), QStringLiteral("error"));
      EXPECT_TRUE(record.value(QStringLiteral("message")).toString().contains(QStringLiteral("data.pjbogus")));
    }
  }
  EXPECT_TRUE(saw_load_failed) << "the dump must carry data-source-load-failed";
}

// An unreadable layout settles false synchronously and its layout-open-failed record reaches the dump file.
TEST_F(MainWindowLayoutSettleTest, FailedOpenSettlesFalseAndTheDumpCarriesTheFailure) {
  const QString dump_path = project_dir_->filePath(QStringLiteral("failure-diag.json"));
  PJ::DiagnosticDump dump(dump_path);
  dump.attachTo(window().diagnosticBridge());

  // Receiver context scoped to the test: the window is suite-static, so a plain connection would outlive this stack
  // frame on an early ASSERT abort and a later settlement would write through the dead `settles`.
  QObject scope;
  std::vector<bool> settles;
  QObject::connect(
      &window(), &PJ::MainWindow::layoutRestoreSettled, &scope, [&settles](bool ok) { settles.push_back(ok); });

  Peer::loadLayout(window(), project_dir_->filePath(QStringLiteral("does-not-exist.pj4.xml")), /*interactive=*/false);

  ASSERT_EQ(settles.size(), 1u) << "an open failure must settle synchronously, exactly once";
  EXPECT_FALSE(settles.front());

  // No pump between settlement and write, as in main.cpp (settle -> QCoreApplication::exit -> aboutToQuit -> write):
  // the queued bridge delivery is undrained, so write() must drain it for the failure id to reach the file.
  ASSERT_TRUE(dump.write());
  const QJsonObject envelope = readDumpEnvelope(dump_path);
  EXPECT_EQ(envelope.value(QStringLiteral("version")).toInt(), 1);
  const QJsonArray records = envelope.value(QStringLiteral("records")).toArray();
  bool saw_open_failed = false;
  for (const auto& value : records) {
    saw_open_failed = saw_open_failed ||
                      value.toObject().value(QStringLiteral("id")).toString() == QStringLiteral("layout-open-failed");
  }
  EXPECT_TRUE(saw_open_failed) << "the dump written on the failure path must carry layout-open-failed";
}

// A folder opens as no file does (a locked file on Windows too): its failure is layout-open-failed, never a parse
// failure, though the folder itself is readable.
TEST_F(MainWindowLayoutSettleTest, AFolderFailsToOpenNotToParse) {
  const QString dump_path = project_dir_->filePath(QStringLiteral("folder-diag.json"));
  PJ::DiagnosticDump dump(dump_path);
  dump.attachTo(window().diagnosticBridge());
  const QString folder = project_dir_->filePath(QStringLiteral("folder.pj4.xml"));
  ASSERT_TRUE(QDir().mkpath(folder));

  QObject scope;
  std::vector<bool> settles;
  QObject::connect(
      &window(), &PJ::MainWindow::layoutRestoreSettled, &scope, [&settles](bool ok) { settles.push_back(ok); });
  Peer::loadLayout(window(), folder, /*interactive=*/false);
  ASSERT_EQ(settles, std::vector<bool>{false});

  ASSERT_TRUE(dump.write());
  QStringList ids;
  for (const auto& value : readDumpEnvelope(dump_path).value(QStringLiteral("records")).toArray()) {
    ids << value.toObject().value(QStringLiteral("id")).toString();
  }
  EXPECT_TRUE(ids.contains(QStringLiteral("layout-open-failed"))) << ids.join(u", ").toStdString();
  EXPECT_FALSE(ids.contains(QStringLiteral("layout-parse-failed"))) << ids.join(u", ").toStdString();
}

// A plain layout (no batch, no pending loads) settles true on the sync leg, exactly once, with no batch constructed.
TEST_F(MainWindowLayoutSettleTest, PlainLayoutSettlesTrueOnTheSyncApplyLeg) {
  const PJ::DatasetId dataset = pj_test::createDataset(appSession(), "plain.mcap", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0u);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), dataset, "/plain", 1'000, 2'000), 0u);
  appSession().catalogModel().rebuildFromDatastore();

  const QString layout_path = project_dir_->filePath(QStringLiteral("plain.pj4.xml"));
  ASSERT_TRUE(fake::writeLayoutFile(layout_path, fake::buildGenericCurveLayoutDoc(QStringLiteral("/plain"))));

  // Receiver context scoped to the test (as in the failed-open test).
  QObject scope;
  std::vector<bool> settles;
  QObject::connect(
      &window(), &PJ::MainWindow::layoutRestoreSettled, &scope, [&settles](bool ok) { settles.push_back(ok); });

  Peer::loadLayout(window(), layout_path, /*interactive=*/false);

  ASSERT_EQ(settles.size(), 1u) << "the sync leg must settle synchronously, exactly once";
  EXPECT_TRUE(settles.front());
  EXPECT_FALSE(Peer::progressiveInFlight(window())) << "a plain layout with loaded data takes the sync leg";
  EXPECT_EQ(Peer::batch(window()), nullptr);

  flushQueuedEvents();
  EXPECT_EQ(settles.size(), 1u) << "no late duplicate settlement";
}

// A layout over data with /cosine and no /sine: one plot of /cosine plus a filter, a transform and an on-demand recipe
// whose inputs do not resolve.
QString writeDerivedInputMissingLayout(PJ::AppSession& session, const QString& dir) {
  const PJ::DatasetId dataset = pj_test::createDataset(session, "nosine.csv", /*own_time_domain=*/true);
  EXPECT_NE(dataset, 0u);
  EXPECT_NE(pj_test::addScalarTopic(session, dataset, "/cosine", 1'000, 2'000), 0u);
  session.catalogModel().rebuildFromDatastore();

  QDomDocument doc = fake::buildGenericCurveLayoutDoc(QStringLiteral("/cosine"));
  doc.elementsByTagName(QStringLiteral("curve")).at(0).toElement().setAttribute(QStringLiteral("field"), u"value"_s);
  QDomDocument processors;
  EXPECT_TRUE(processors.setContent(QStringLiteral(R"(<data_processors>
  <processor input_topic="/sine" input_field="" processor_id="absolute" output_name="sine[Absolute]"><![CDATA[{}]]></processor>
  <transform api_version="1" backend="luau" id="sine_x3" owner_plugin="owner">
    <input name="/sine"/><output name="sine_x3"/><params><![CDATA[{}]]></params>
    <script><![CDATA[-- pj-script: luau
return { id="sine_x3", name="sine_x3", output="same",
  create = function(p) return { calculate = function(t, v) return v * 3 end } end }
]]></script>
  </transform>
  <on_demand owner_plugin="owner" id="crop"><input name="cloud" dataset_path="/elsewhere/other.mcap"/>
    <output name="cropped" type="kPointCloud"/><script><![CDATA[return {}]]></script></on_demand>
</data_processors>)")));
  doc.documentElement().appendChild(doc.importNode(processors.documentElement(), true));
  const QString layout_path = QDir(dir).filePath(QStringLiteral("derived-input-missing.pj4.xml"));
  EXPECT_TRUE(fake::writeLayoutFile(layout_path, doc));
  return layout_path;
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
  return file.readAll();
}

// Answers the next message box that has a button labelled one of `labels`: records its body into `shown` and clicks the
// first of `labels` it has, recorded into `clicked`.
void answerNextMessageBox(
    pj_app_test::ModalAnswerer& answerer, const QStringList& labels, QString& shown, QString& clicked) {
  answerer.next(pj_app_test::messageBoxWithButton(labels), [labels, &shown, &clicked](QWidget* box) {
    shown = pj_app_test::messageBoxBody(box);
    clicked = pj_app_test::clickMessageBoxButton(box, labels);
  });
}

// A layout loaded over data that lacks a derived object's input still applies: the plot restores, and each derived
// kind (filter, transform, on-demand recipe) whose input does not resolve is skipped and reported, the way a missing
// curve is, instead of failing the whole layout.
TEST_F(MainWindowLayoutSettleTest, DerivedObjectWithMissingInputIsSkippedAndThePlotRestores) {
  const QString layout_path = writeDerivedInputMissingLayout(appSession(), project_dir_->path());

  QStringList reports;
  QObject::connect(
      window().diagnosticBridge(), &PJ::QtDiagnosticBridge::diagnosticReported, &window(),
      [&reports](int, const QString&, const QString& id, const QString& message) {
        reports.push_back(id + u": "_s + message);
      });
  QObject scope;
  std::vector<bool> settles;
  QObject::connect(
      &window(), &PJ::MainWindow::layoutRestoreSettled, &scope, [&settles](bool ok) { settles.push_back(ok); });

  Peer::loadLayout(window(), layout_path, /*interactive=*/false);
  flushQueuedEvents();

  const std::string all = reports.join(u"\n").toStdString();
  ASSERT_EQ(settles.size(), 1u);
  EXPECT_TRUE(settles.front()) << all;
  EXPECT_EQ(Peer::totalCurveCount(window()), 1) << "the plot of /cosine restores";
  const auto reported = [&reports](const QString& prefix) {
    return std::any_of(reports.begin(), reports.end(), [&prefix](const QString& r) { return r.startsWith(prefix); });
  };
  EXPECT_FALSE(reported(u"layout-apply-failed"_s)) << all;
  EXPECT_TRUE(reported(u"processor-restore-failed: Layout filter on '/sine/' has no matching data"_s)) << all;
  EXPECT_TRUE(reported(
      u"transform-restore-failed: Could not restore transform 'owner/sine_x3': "_s
      u"pj.data_processors: input topic/field not found: /sine"_s))
      << all;
  EXPECT_TRUE(reported(u"on-demand-restore-failed: Could not restore on-demand recipe 'owner/crop'"_s)) << all;
}

constexpr const char* kTimes2 = R"LUAU(-- pj-script: luau
return { id="sine_x3", name="sine_x3", output="same",
  create = function(p) return { calculate = function(t, v) return v * 2 end } end }
)LUAU";

// The layout file a save writes now.
QByteArray savedLayout(PJ::MainWindow& window, const QString& dir) {
  const QString saved_path = QDir(dir).filePath(QStringLiteral("saved.pj4.xml"));
  Peer::saveLayoutToPath(window, saved_path, /*include_data_source=*/false);
  return readFile(saved_path);
}

// What an unattended layout load could not restore is written back by the next save, unchanged, so saving over the
// file never loses a hand-written recipe; the next layout load forgets it.
TEST_F(MainWindowLayoutSettleTest, SaveKeepsTheDerivedObjectsTheLoadCouldNotRestore) {
  const QString layout_path = writeDerivedInputMissingLayout(appSession(), project_dir_->path());
  Peer::loadLayout(window(), layout_path, /*interactive=*/false);
  flushQueuedEvents();

  QByteArray saved = savedLayout(window(), project_dir_->path());
  EXPECT_TRUE(saved.contains(R"(output_name="sine[Absolute]")")) << saved.toStdString();
  EXPECT_TRUE(saved.contains(R"(owner_plugin="owner")")) << saved.toStdString();
  EXPECT_TRUE(saved.contains("v * 3")) << "the script is kept as it was";
  EXPECT_TRUE(saved.contains(R"(id="crop")")) << saved.toStdString();

  const QString plain_path = project_dir_->filePath(QStringLiteral("plain.pj4.xml"));
  ASSERT_TRUE(fake::writeLayoutFile(plain_path, fake::buildGenericCurveLayoutDoc(QStringLiteral("/cosine"))));
  Peer::loadLayout(window(), plain_path, /*interactive=*/false);
  flushQueuedEvents();
  saved = savedLayout(window(), project_dir_->path());
  EXPECT_FALSE(saved.contains(R"(id="crop")")) << saved.toStdString();
  EXPECT_FALSE(saved.contains("v * 3")) << saved.toStdString();
}

// A live recipe of a kept entry's identity replaces it for good: deleting the live one before any save does not bring
// the old entry back.
TEST_F(MainWindowLayoutSettleTest, ALiveRecipeRetiresTheKeptEntryForGood) {
  const QString layout_path = writeDerivedInputMissingLayout(appSession(), project_dir_->path());
  Peer::loadLayout(window(), layout_path, /*interactive=*/false);
  flushQueuedEvents();
  PJ::DataProcessorService& service = appSession().sessionManager().dataProcessorService();
  const auto live = service.upsertTransform("owner", "sine_x3", {"/cosine"}, {"sine_x3"}, kTimes2, "{}");
  ASSERT_TRUE(live.has_value()) << live.error();
  appSession().catalogModel().rebuildFromDatastore();
  flushQueuedEvents();
  ASSERT_TRUE(service.removeTransform("owner/sine_x3"));
  appSession().catalogModel().rebuildFromDatastore();

  const QByteArray saved = savedLayout(window(), project_dir_->path());
  EXPECT_EQ(saved.count("<transform "), 0) << saved.toStdString();
  EXPECT_TRUE(saved.contains(R"(id="crop")")) << "only the replaced entry is retired";
}

// A kept transform whose output name a live recipe now uses is not written back: the next load would refuse it
// ("output topic name already in use"), and a browser apply of the file would fail whole.
TEST_F(MainWindowLayoutSettleTest, AKeptTransformWhoseOutputIsTakenIsNotSaved) {
  const QString layout_path = writeDerivedInputMissingLayout(appSession(), project_dir_->path());
  Peer::loadLayout(window(), layout_path, /*interactive=*/false);
  flushQueuedEvents();
  const auto live = appSession().sessionManager().dataProcessorService().upsertTransform(
      "other", "sine_x3", {"/cosine"}, {"sine_x3"}, kTimes2, "{}");
  ASSERT_TRUE(live.has_value()) << live.error();
  appSession().catalogModel().rebuildFromDatastore();

  const QByteArray saved = savedLayout(window(), project_dir_->path());
  EXPECT_EQ(saved.count("<transform "), 1) << saved.toStdString();
  EXPECT_TRUE(saved.contains(R"(owner_plugin="other")")) << saved.toStdString();
}

// Removing every dataset, through Remove all or by trashing the last catalog row, forgets the kept entries.
TEST_F(MainWindowLayoutSettleTest, EmptyingTheCatalogForgetsTheKeptEntries) {
  const QString layout_path = writeDerivedInputMissingLayout(appSession(), project_dir_->path());
  Peer::loadLayout(window(), layout_path, /*interactive=*/false);
  flushQueuedEvents();
  ASSERT_TRUE(savedLayout(window(), project_dir_->path()).contains(R"(id="crop")"));
  QStringList keys;
  for (const auto& item : appSession().catalogModel().items()) {
    keys.push_back(item.key);
  }
  ASSERT_FALSE(keys.isEmpty());

  emit window().findChild<PJ::CurveListPanel*>(QStringLiteral("curveListPanel")) -> trashRequested(keys, false);
  flushQueuedEvents();
  ASSERT_TRUE(appSession().catalogModel().items().empty());
  const QByteArray saved = savedLayout(window(), project_dir_->path());
  EXPECT_FALSE(saved.contains(R"(id="crop")")) << saved.toStdString();
}

// An interactive load lists what it could not restore in the missing-data prompt, with the same Cancel choice as a
// missing curve; Cancel leaves the previous workspace.
TEST_F(MainWindowLayoutSettleTest, InteractiveLoadPromptsForTheSkippedDerivedObjects) {
  const QString layout_path = writeDerivedInputMissingLayout(appSession(), project_dir_->path());
  QObject scope;
  std::vector<bool> settles;
  QObject::connect(
      &window(), &PJ::MainWindow::layoutRestoreSettled, &scope, [&settles](bool ok) { settles.push_back(ok); });
  QString shown;
  QString clicked;
  pj_app_test::ModalAnswerer answer(10);
  answerNextMessageBox(answer, {QStringLiteral("Cancel")}, shown, clicked);

  Peer::loadLayout(window(), layout_path, /*interactive=*/true);
  flushQueuedEvents();

  EXPECT_TRUE(shown.contains(QStringLiteral("owner/sine_x3"))) << shown.toStdString();
  EXPECT_TRUE(shown.contains(QStringLiteral("owner/crop"))) << shown.toStdString();
  ASSERT_EQ(settles.size(), 1u);
  EXPECT_FALSE(settles.front());
  EXPECT_EQ(Peer::totalCurveCount(window()), 0) << "Cancel leaves the previous (empty) workspace";
}

// Accepting the prompt drops what it listed: the skipped derived objects are not kept, so a save does not write them
// back. With no curve missing, the prompt does not speak of curves.
TEST_F(MainWindowLayoutSettleTest, AcceptingThePromptDropsTheSkippedDerivedObjects) {
  const QString layout_path = writeDerivedInputMissingLayout(appSession(), project_dir_->path());
  QString shown;
  QString clicked;
  pj_app_test::ModalAnswerer answer(10);
  answerNextMessageBox(
      answer, {QStringLiteral("Continue without them"), QStringLiteral("Remove from plots")}, shown, clicked);

  Peer::loadLayout(window(), layout_path, /*interactive=*/true);
  flushQueuedEvents();

  EXPECT_EQ(clicked, QStringLiteral("Continue without them"));
  EXPECT_FALSE(shown.contains(QStringLiteral("curve"))) << shown.toStdString();
  EXPECT_EQ(Peer::totalCurveCount(window()), 1);
  const QByteArray saved = savedLayout(window(), project_dir_->path());
  EXPECT_FALSE(saved.contains("v * 3")) << saved.toStdString();
  EXPECT_FALSE(saved.contains(R"(id="crop")")) << saved.toStdString();
}

// A materialize-import layout settles true only at the progressive drain, after the batch finished and every
// restore waiter cleared; no settlement while the job is parked, so quitting on settlement never tears down an
// active batch.
TEST_F(MainWindowLayoutSettleTest, BatchLayoutSettlesTrueOnlyAtBatchFinish) {
  const QString source_path = project_dir_->filePath(QStringLiteral("data/run.mcap"));
  const QString layout_path = project_dir_->filePath(QStringLiteral("import.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          layout_path, QStringLiteral("/left"), source_path,
          fake::makeDescriptor(QStringLiteral("trusted"), source_path, QStringLiteral("block"))));

  struct AtSettle {
    bool success = false;
    bool batch_active = true;
    int waiters = -1;
    bool in_flight = true;
  };
  // Receiver context scoped to the test (as in the failed-open test).
  QObject scope;
  std::vector<AtSettle> settles;
  QObject::connect(&window(), &PJ::MainWindow::layoutRestoreSettled, &scope, [&settles](bool ok) {
    settles.push_back(
        AtSettle{
            .success = ok,
            .batch_active = Peer::batchActive(window()),
            .waiters = Peer::restoreWaiterCount(window()),
            .in_flight = Peer::progressiveInFlight(window()),
        });
  });

  Peer::loadLayout(window(), layout_path, /*interactive=*/false);

  ASSERT_TRUE(Peer::progressiveInFlight(window()));
  ASSERT_TRUE(Peer::batchActive(window()));
  EXPECT_TRUE(settles.empty()) << "no settlement while the import batch is still active";
  flushQueuedEvents();
  EXPECT_TRUE(settles.empty()) << "still parked: the settlement must wait for the batch waiter";

  // Inject the dataset+topic the job is fetching, then release the blocked worker so the batch concludes.
  const PJ::DatasetId dataset = pj_test::createDataset(appSession(), "run.mcap", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0u);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), dataset, "/left", 1'000, 2'000), 0u);
  appSession().catalogModel().rebuildFromDatastore();
  ASSERT_NE(fake::g_instance, nullptr);
  fake::g_instance->releaseStart();

  ASSERT_TRUE(pumpUntil([&settles]() { return !settles.empty(); }));
  ASSERT_EQ(settles.size(), 1u);
  EXPECT_TRUE(settles.front().success);
  EXPECT_FALSE(settles.front().batch_active) << "settlement must come strictly after the batch finished";
  EXPECT_EQ(settles.front().waiters, 0) << "every restore waiter must have cleared before settlement";
  EXPECT_FALSE(settles.front().in_flight);

  flushQueuedEvents();
  EXPECT_EQ(settles.size(), 1u) << "exactly one settlement per load";
  EXPECT_EQ(Peer::batch(window()), nullptr) << "the settled restore retires the batch";
}

#ifdef PJ_WITH_SCENE3D
// Scene3D resource references are resolved at ingress, before anything restores: a malformed robot package mapping
// fails the load, settles false with a diagnostic, and the layout's plot never reaches the workspace.
TEST_F(MainWindowLayoutSettleTest, InvalidSceneResourcesFailBeforeTheWorkspaceChanges) {
  const PJ::DatasetId dataset = pj_test::createDataset(appSession(), "resources.mcap", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0u);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), dataset, "/plain", 1'000, 2'000), 0u);
  appSession().catalogModel().rebuildFromDatastore();

  QDomDocument doc = fake::buildGenericCurveLayoutDoc(QStringLiteral("/plain"));
  QDomElement robot = doc.createElement(QStringLiteral("robot_model"));
  robot.setAttribute(QStringLiteral("source_type"), QStringLiteral("topic"));
  robot.setAttribute(QStringLiteral("package_url_roots"), QStringLiteral(R"(["not", "an", "object"])"));
  doc.documentElement().appendChild(robot);
  const QString layout_path = project_dir_->filePath(QStringLiteral("bad-resources.pj4.xml"));
  ASSERT_TRUE(fake::writeLayoutFile(layout_path, doc));

  QObject scope;
  std::vector<bool> settles;
  QObject::connect(
      &window(), &PJ::MainWindow::layoutRestoreSettled, &scope, [&settles](bool ok) { settles.push_back(ok); });
  QStringList ids;
  QObject::connect(
      Peer::diagnosticBridge(window()), &PJ::QtDiagnosticBridge::diagnosticReported, &scope,
      [&ids](int /*level*/, const QString& /*source*/, const QString& id, const QString& /*message*/) {
        ids.push_back(id);
      });

  Peer::loadLayout(window(), layout_path, /*interactive=*/false);

  ASSERT_EQ(settles.size(), 1u);
  EXPECT_FALSE(settles.front());
  EXPECT_EQ(Peer::totalCurveCount(window()), 0) << "the rejected layout restored a curve";
  flushQueuedEvents();
  EXPECT_TRUE(ids.contains(QStringLiteral("layout-resources-invalid"))) << ids.join(u',').toStdString();
}
#endif

// A source-bound layout whose stream plugins are not installed names every stream it could not start, once, and
// still applies the rest of the layout.
TEST_F(MainWindowLayoutSettleTest, StreamsThatCannotStartAreNamedAndTheLayoutStillApplies) {
  const PJ::DatasetId dataset = pj_test::createDataset(appSession(), "streams.mcap", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0u);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), dataset, "/plain", 1'000, 2'000), 0u);
  appSession().catalogModel().rebuildFromDatastore();

  QDomDocument doc = fake::buildGenericCurveLayoutDoc(QStringLiteral("/plain"));
  doc.documentElement().setAttribute(QStringLiteral("binding"), QStringLiteral("source"));
  QList<PJ::layout_xml::StreamingSourceRef> streams;
  for (const QString& id : {QStringLiteral("left"), QStringLiteral("right")}) {
    PJ::layout_xml::StreamingSourceRef stream;
    stream.id = id;
    stream.plugin_manifest_id = QStringLiteral("some-stream");
    stream.plugin_config_json = QStringLiteral("{}");
    streams.push_back(stream);
  }
  ASSERT_TRUE(PJ::layout_xml::writeStreamingSources(doc, streams, nullptr));
  const QString layout_path = project_dir_->filePath(QStringLiteral("streams.pj4.xml"));
  ASSERT_TRUE(fake::writeLayoutFile(layout_path, doc));

  QObject scope;
  std::vector<bool> settles;
  QObject::connect(
      &window(), &PJ::MainWindow::layoutRestoreSettled, &scope, [&settles](bool ok) { settles.push_back(ok); });
  QStringList warnings;
  QObject::connect(
      Peer::diagnosticBridge(window()), &PJ::QtDiagnosticBridge::diagnosticReported, &scope,
      [&warnings](int level, const QString& /*source*/, const QString& id, const QString& message) {
        if (id == QStringLiteral("layout-streams-not-started")) {
          EXPECT_EQ(level, static_cast<int>(PJ::DiagnosticLevel::kWarning));
          warnings.push_back(message);
        }
      });

  Peer::loadLayout(window(), layout_path, /*interactive=*/false);

  ASSERT_EQ(settles.size(), 1u);
  EXPECT_TRUE(settles.front());
  EXPECT_EQ(Peer::totalCurveCount(window()), 1) << "the plot still restores";
  flushQueuedEvents();
  ASSERT_EQ(warnings.size(), 1) << warnings.join(u'\n').toStdString();
  EXPECT_TRUE(warnings.front().contains(QStringLiteral("'left'"))) << warnings.front().toStdString();
  EXPECT_TRUE(warnings.front().contains(QStringLiteral("'right'"))) << warnings.front().toStdString();
}

// Text a load interpolates into its warning box (here the path) reaches the box escaped: markup is shown, not rendered.
TEST_F(MainWindowLayoutSettleTest, WarningBoxShowsInterpolatedTextAsPlainText) {
  const QString path = project_dir_->filePath(QStringLiteral("<b>bold</b><a href=x>link</a>.pj4.xml"));
  QString shown;
  pj_app_test::ModalAnswerer dismiss(10);
  dismiss.every(
      [](QWidget* modal) { return pj_app_test::isMessageBox(modal); },
      [&shown](QWidget* box) {
        shown = pj_app_test::messageBoxBody(box);
        if (const auto buttons = pj_app_test::messageBoxButtons(box); !buttons.isEmpty()) {
          QMetaObject::invokeMethod(buttons.first(), &QAbstractButton::click, Qt::QueuedConnection);
        }
      });

  Peer::loadLayout(window(), path, /*interactive=*/true);

  dismiss.stop();
  EXPECT_FALSE(shown.contains(QStringLiteral("<b>"))) << shown.toStdString();
  EXPECT_TRUE(shown.contains(QStringLiteral("&lt;b&gt;bold&lt;/b&gt;&lt;a href=x&gt;link&lt;/a&gt;")))
      << shown.toStdString();
}

}  // namespace
