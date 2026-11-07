// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// GUI suite for the --exit-after-layout settlement boundary (stage-5 E5): the
// layoutRestoreSettled signal fires EXACTLY ONCE per loadLayoutFromPath
// terminal outcome —
//   * an open failure settles false synchronously (and composes with the
//     --dump-diagnostics collector: the failure record reaches the file);
//   * a plain (no-batch) layout settles true on the SYNC apply leg;
//   * a materialize-import layout settles true only at the progressive drain,
//     strictly after the batch finished and every restore waiter cleared —
//     never mid-import.
// It also covers what the same entry tells the user: the warning for stream
// records it does not start, and warning boxes that show their text as text.
//
// Each ctest case constructs its own MainWindow, so settlement observations
// cannot inherit process-global widget state from another scenario.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <memory>
#include <vector>

#include "DiagnosticDump.h"
#include "dataset_test_helpers.h"
#include "pj_runtime/CatalogModel.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

namespace {

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

// A layout whose data file no plugin handles: FileLoader's failure reaches the
// diagnostics (and so --dump-diagnostics), not only the console.
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

// (1) + (5): an unreadable layout settles false synchronously, and the
// --dump-diagnostics composition holds on the failure path — the
// layout-open-failed record reaches the JSON file.
TEST_F(MainWindowLayoutSettleTest, FailedOpenSettlesFalseAndTheDumpCarriesTheFailure) {
  const QString dump_path = project_dir_->filePath(QStringLiteral("failure-diag.json"));
  PJ::DiagnosticDump dump(dump_path);
  dump.attachTo(window().diagnosticBridge());

  // Scenario-scoped receiver context: the window is suite-static, so a plain
  // connection would outlive this stack frame on an early ASSERT abort and
  // the next scenario's settlement would write through the dead `settles`.
  QObject scope;
  std::vector<bool> settles;
  QObject::connect(
      &window(), &PJ::MainWindow::layoutRestoreSettled, &scope, [&settles](bool ok) { settles.push_back(ok); });

  Peer::loadLayout(window(), project_dir_->filePath(QStringLiteral("does-not-exist.pj4.xml")), /*interactive=*/false);

  ASSERT_EQ(settles.size(), 1u) << "an open failure must settle synchronously, exactly once";
  EXPECT_FALSE(settles.front());

  // Deliberately NO pump between the settlement and the write — main.cpp's
  // real shape (settle -> QCoreApplication::exit -> aboutToQuit -> write)
  // leaves the queued bridge delivery undrained; write() must drain it so
  // the failure id reaches the file (scripts assert ids, never message text).
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

// (2a): a plain layout (no batch, no pending loads) settles true on the sync
// leg, exactly once, with no batch ever constructed.
TEST_F(MainWindowLayoutSettleTest, PlainLayoutSettlesTrueOnTheSyncApplyLeg) {
  const PJ::DatasetId dataset = pj_test::createDataset(appSession(), "plain.mcap", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0u);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), dataset, "/plain", 1'000, 2'000), 0u);
  appSession().catalogModel().rebuildFromDatastore();

  const QString layout_path = project_dir_->filePath(QStringLiteral("plain.pj4.xml"));
  ASSERT_TRUE(fake::writeLayoutFile(layout_path, fake::buildGenericCurveLayoutDoc(QStringLiteral("/plain"))));

  // Scenario-scoped receiver context (see the first scenario's rationale).
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

// (2b): a materialize-import layout settles true ONLY at the progressive
// drain — after the batch finished and every restore waiter cleared. The
// mid-import window (job parked) must observe ZERO settlements: quitting on
// settlement can therefore never tear down a still-active batch.
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
  // Scenario-scoped receiver context (see the first scenario's rationale).
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

  // The import "arrives": inject the dataset+topic the job is fetching, then
  // release the blocked worker so the batch concludes and the drain settles.
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
// Scene3D resource references are resolved at ingress, before anything
// restores: a malformed robot package mapping fails the load, settles false
// with a diagnostic, and the layout's plot never reaches the workspace.
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

// The desktop loader does not start a layout's streaming sources: it says so
// once, naming how many, and still applies the rest of the layout.
TEST_F(MainWindowLayoutSettleTest, StreamRecordsWarnThatTheDesktopDoesNotStartThem) {
  const PJ::DatasetId dataset = pj_test::createDataset(appSession(), "streams.mcap", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0u);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), dataset, "/plain", 1'000, 2'000), 0u);
  appSession().catalogModel().rebuildFromDatastore();

  QDomDocument doc = fake::buildGenericCurveLayoutDoc(QStringLiteral("/plain"));
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
  EXPECT_TRUE(warnings.front().contains(QStringLiteral("2"))) << warnings.front().toStdString();
}

// Text a load interpolates into its warning box (here the path) reaches the
// box escaped, so markup it spells is shown, never rendered.
TEST_F(MainWindowLayoutSettleTest, WarningBoxShowsInterpolatedTextAsPlainText) {
  const QString path = project_dir_->filePath(QStringLiteral("<b>bold</b><a href=x>link</a>.pj4.xml"));
  QString shown;
  QTimer dismiss;
  dismiss.setInterval(10);
  QObject::connect(&dismiss, &QTimer::timeout, &dismiss, [&shown]() {
    QWidget* modal = QApplication::activeModalWidget();
    if (modal == nullptr || modal->objectName() != QStringLiteral("pjMessageBox")) {
      return;
    }
    if (const auto* body = modal->findChild<QLabel*>(QStringLiteral("pjMessageBoxBody"))) {
      shown = body->text();
    }
    if (auto* ok = modal->findChild<QPushButton*>(QStringLiteral("pjMessageBoxButton"))) {
      ok->click();
    }
  });
  dismiss.start();

  Peer::loadLayout(window(), path, /*interactive=*/true);

  dismiss.stop();
  EXPECT_FALSE(shown.contains(QStringLiteral("<b>"))) << shown.toStdString();
  EXPECT_TRUE(shown.contains(QStringLiteral("&lt;b&gt;bold&lt;/b&gt;&lt;a href=x&gt;link&lt;/a&gt;")))
      << shown.toStdString();
}

}  // namespace
