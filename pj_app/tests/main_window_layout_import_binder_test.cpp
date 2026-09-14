// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// GUI suite for the T7 binder shell work (D1/D3/D6/D7/D8/D9): the title-bar
// ingest strip's DISPLAYED OWNER, the batch-scoped SessionManager
// observation, the batch job's keep-partial Stop route, the #498
// coexistence rules, and the D6 REAL-SURFACE progressive legs. Two fake
// disciplines coexist here:
//   * announce-then-block (Task 1) parks a job with a scripted counter
//     DatasetId; those scenarios drive SessionManager::begin/update/endIngest
//     directly (the alive-test out-of-band pattern) — deterministic and
//     sufficient for presentation/arbitration semantics.
//   * progressive / progressive-promoted (Task 3, the headline) drive the
//     REAL bound host services from the job worker — createDataSource ->
//     on_dataset(real id) -> createDatasetIngest/progressStart -> write-host
//     topic/field/record -> ONE progressUpdate tick -> park — so the strip,
//     the observers, and the mid-import curve binding are exercised through
//     the production headless-session -> SessionManager chain with zero
//     driven signals.
//
// One MainWindow per binary; the scenarios share it fold-test style and
// clean up their driven ingests, seams, and panels.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QWidget>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dataset_test_helpers.h"
#include "pj_datastore/reader.hpp"
#include "pj_runtime/CatalogModel.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

#ifndef PJ_MOCK_FILE_SOURCE_PLUGIN_PATH
#error "PJ_MOCK_FILE_SOURCE_PLUGIN_PATH must be defined"
#endif

namespace {

using PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::flushQueuedEvents;
using pj_app_test::pumpUntil;
namespace fake = pj_layout_import_gui;
using Peer = MainWindowLayoutImportTestPeer;
using StripOwnerKind = Peer::StripOwnerKind;

// Mirrors MainWindow's kIngestProgressResolution: ingest counts may be bytes
// far beyond int range, so the bar tracks permille of total.
constexpr int kProgressResolution = 1000;

// Minimal applyable layout with no data sources at all — the D7 "plain
// layout" (no <materialize> anywhere -> no batch, no observers).
[[nodiscard]] QDomDocument buildGenericLayoutDoc() {
  QDomDocument doc;
  QDomElement root = doc.createElement(QStringLiteral("root"));
  root.setAttribute(QStringLiteral("pj4_version"), QStringLiteral("4"));
  root.setAttribute(QStringLiteral("binding"), QStringLiteral("generic"));
  doc.appendChild(root);
  QDomElement tabbed = doc.createElement(QStringLiteral("tabbed_widget"));
  tabbed.setAttribute(QStringLiteral("parent"), QStringLiteral("main_window"));
  QDomElement tab = doc.createElement(QStringLiteral("Tab"));
  tab.setAttribute(QStringLiteral("id"), QStringLiteral("t1"));
  tab.setAttribute(QStringLiteral("containers"), QStringLiteral("1"));
  QDomElement container = doc.createElement(QStringLiteral("Container"));
  QDomElement dock_area = doc.createElement(QStringLiteral("DockArea"));
  dock_area.setAttribute(QStringLiteral("id"), QStringLiteral("a1"));
  dock_area.setAttribute(QStringLiteral("name"), QStringLiteral("View"));
  QDomElement plot = doc.createElement(QStringLiteral("plot"));
  plot.setAttribute(QStringLiteral("id"), QStringLiteral("plot1"));
  plot.setAttribute(QStringLiteral("mode"), QStringLiteral("TimeSeries"));
  dock_area.appendChild(plot);
  container.appendChild(dock_area);
  tab.appendChild(container);
  tabbed.appendChild(tab);
  root.appendChild(tabbed);
  root.appendChild(doc.createElement(QStringLiteral("previouslyLoaded_Datafiles")));
  return doc;
}

// A second materialize-bearing <fileInfo>, appended next to the one
// buildImportLayoutDoc produced (the multi-job batch shape).
void appendMaterializeFileInfo(QDomDocument& doc, const QString& path, const QString& descriptor) {
  QDomElement wrapper = doc.documentElement().firstChildElement(QStringLiteral("previouslyLoaded_Datafiles"));
  QDomElement file_info = doc.createElement(QStringLiteral("fileInfo"));
  file_info.setAttribute(QStringLiteral("filename"), path);
  QDomElement materialize = doc.createElement(QStringLiteral("materialize"));
  materialize.setAttribute(QStringLiteral("provider"), QString::fromUtf8(fake::kProviderId));
  materialize.setAttribute(QStringLiteral("identity"), QStringLiteral("id-2"));
  PJ::layout_xml::appendJsonAsCdata(doc, materialize, descriptor);
  file_info.appendChild(materialize);
  wrapper.appendChild(file_info);
}

// One MainWindow per binary: the shell leaks process-global widget state
// across instances, so every scenario shares this one and cleans up after
// itself (driven ingests ended, seams reset, panels released).
class MainWindowLayoutImportBinderTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    extensions_dir_ = std::make_unique<QTemporaryDir>();
    project_dir_ = std::make_unique<QTemporaryDir>();
    ASSERT_TRUE(extensions_dir_->isValid());
    ASSERT_TRUE(project_dir_->isValid());
    // Stage the mock source plugin BEFORE the window scans the extensions
    // dir: the D4 promotion leg promotes through the real FileLoader replace
    // transaction with this stock loader.
    const QString plugin_src = QString::fromUtf8(PJ_MOCK_FILE_SOURCE_PLUGIN_PATH);
    ASSERT_TRUE(QFile::copy(plugin_src, extensions_dir_->filePath(QFileInfo(plugin_src).fileName())))
        << "could not stage " << plugin_src.toStdString();
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_->path());
    ASSERT_FALSE(appSession().extensionCatalog().findSourcesForExtension(QStringLiteral(".mock")).empty())
        << "mock_file_source_plugin did not load from the staged extensions dir";
    ASSERT_TRUE(appSession().extensionCatalog().pluginCatalog().registerStaticToolbox(&fake::kFakeVtable));
    fake::recordDiagnosticIds(*window_, diagnostic_ids_);
  }

  static void TearDownTestSuite() {
    window_.reset();
    project_dir_.reset();
    extensions_dir_.reset();
  }

  void SetUp() override {
    diagnostic_ids_.clear();
    pj_fake_import::g_log.clear();
    ingest_tokens_.clear();
  }

  void TearDown() override {
    // Unconditional (an ASSERT abort must not leak seams/panels into the
    // next scenario). Busy is off before the forced close so no pinned
    // panel can veto it; the seam closures capture this fixture instance,
    // which outlives the reset below.
    panel_busy_ = false;
    Peer::teardownAllToolboxes(window());
    if (QWidget* released = Peer::releaseCentralPanel(window()); released != nullptr) {
      delete released;
    }
    Peer::setHostSeams(window(), {}, {}, {});
    // A scenario's final driven end has no production handler, so it can
    // leave a stale displayed owner AND an engaged strip (fired show timer /
    // pending timers) behind (see resetStripState) — reset the whole strip
    // surface so the scenarios stay order-independent under gtest shuffle.
    Peer::resetStripState(window());
  }

  [[nodiscard]] static PJ::MainWindow& window() {
    return *window_;
  }
  [[nodiscard]] static PJ::AppSession& appSession() {
    return Peer::session(*window_);
  }
  [[nodiscard]] static PJ::SessionManager& sessionManager() {
    return appSession().sessionManager();
  }

  // The ingest API is addressed by IngestToken, but every scenario below is
  // written in terms of the dataset the batch announced. These remember the
  // token minted for each dataset so the tests keep reading as "this dataset
  // progressed", exactly as the production callbacks do via their own per-
  // dataset token maps. Cleared per test by the fixture's SetUp.
  static PJ::IngestToken beginIngestFor(PJ::DatasetId dataset, const QString& label, quint64 total) {
    const PJ::IngestToken token = sessionManager().beginIngest(dataset, label, total, /*cancellable=*/false, {});
    ingest_tokens_[dataset] = token;
    return token;
  }
  static void updateIngestFor(PJ::DatasetId dataset, quint64 current, quint64 total) {
    sessionManager().updateIngest(ingest_tokens_.value(dataset), current, total);
  }
  static void endIngestFor(PJ::DatasetId dataset) {
    sessionManager().endIngest(ingest_tokens_.value(dataset), PJ::IngestOutcome::kUnknown);
    ingest_tokens_.remove(dataset);
  }

  static inline QHash<PJ::DatasetId, PJ::IngestToken> ingest_tokens_;

  // Missing-source materialize layout whose import job announces its dataset
  // and then PARKS on the resume gate; loads it kAutomated and waits for the
  // announced id (0 = the job never got there).
  [[nodiscard]] static PJ::DatasetId startParkedBatchJob(const QString& stem) {
    const QString source = project_dir_->filePath(QStringLiteral("data/%1.mcap").arg(stem));
    const QString layout = project_dir_->filePath(QStringLiteral("%1.pj4.xml").arg(stem));
    EXPECT_TRUE(
        fake::writeImportLayout(
            layout, QStringLiteral("/curve-%1").arg(stem), source,
            pj_fake_import::makeDescriptor(
                stem, QStringLiteral("trusted"), source, 0, QStringLiteral("announce-then-block"))));
    Peer::loadLayout(*window_, layout, /*interactive=*/false);
    EXPECT_TRUE(pumpUntil([]() {
      auto* batch = Peer::batch(*window_);
      return batch != nullptr && batch->activeImportDataset().has_value();
    }));
    auto* batch = Peer::batch(*window_);
    return batch == nullptr ? 0 : batch->activeImportDataset().value_or(0);
  }

  // Release the parked job and let the restore drain to its settle point.
  static void settleRestore() {
    ASSERT_NE(fake::g_instance, nullptr);
    fake::g_instance->releaseResume();
    ASSERT_TRUE(pumpUntil([]() { return !Peer::progressiveInFlight(*window_); }));
    flushQueuedEvents();
  }

  // Descriptor for the D6 real-surface progressive modes. `topic` is what
  // the job publishes through the real write host mid-job; `materialized`
  // overrides the fake's cache verdict independent of the filesystem (the
  // promotion leg pre-creates its artifact yet must still classify as a
  // MISS so the import job runs).
  [[nodiscard]] static QString makeProgressiveDescriptor(
      const QString& name, const QString& path, const QString& topic, bool promoted,
      std::optional<bool> materialized = std::nullopt) {
    QJsonObject obj;
    obj.insert(QStringLiteral("name"), name);
    obj.insert(QStringLiteral("trust"), QStringLiteral("trusted"));
    obj.insert(QStringLiteral("path"), path);
    obj.insert(
        QStringLiteral("import"), promoted ? QStringLiteral("progressive-promoted") : QStringLiteral("progressive"));
    obj.insert(QStringLiteral("topic"), topic);
    if (materialized.has_value()) {
      obj.insert(QStringLiteral("materialized"), *materialized);
    }
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
  }

  // Writes a layout whose single curve references `curve_topic` and whose
  // <materialize> descriptor is `descriptor`, then loads it kAutomated. The
  // progressive job announces its REAL dataset, publishes one record, ticks
  // once, and parks on the resume gate — settleRestore() releases it.
  static void loadProgressiveLayout(
      const QString& stem, const QString& curve_topic, const QString& source, const QString& descriptor) {
    const QString layout = project_dir_->filePath(QStringLiteral("%1.pj4.xml").arg(stem));
    ASSERT_TRUE(fake::writeImportLayout(layout, curve_topic, source, descriptor));
    Peer::loadLayout(*window_, layout, /*interactive=*/false);
  }

  // Rows of `dataset_id`'s single scalar topic (-1: not exactly one topic).
  [[nodiscard]] static std::int64_t singleTopicRowCount(PJ::DatasetId dataset_id) {
    const PJ::DataReader reader = sessionManager().createReader();
    const auto topics = reader.listTopics(dataset_id);
    if (topics.size() != 1u) {
      return -1;
    }
    const auto metadata = reader.getMetadata(topics.front());
    return metadata.has_value() ? static_cast<std::int64_t>(metadata->total_row_count) : -1;
  }

  // Seam-driven pinned-panel state for the D9 scenario (fixture members, not
  // locals: the closures must outlive an ASSERT abort until TearDown).
  bool panel_busy_ = false;
  bool confirm_shown_ = false;
  std::vector<PJ::ToolboxRuntimeHost*> stopped_hosts_;
  int host_token_ = 0;

  [[nodiscard]] PJ::ToolboxRuntimeHost* pinnedHostToken() {
    // Distinct identity only — both host interactions are seam-injected, so
    // this is never dereferenced.
    return reinterpret_cast<PJ::ToolboxRuntimeHost*>(&host_token_);
  }

  inline static std::unique_ptr<QTemporaryDir> extensions_dir_;
  inline static std::unique_ptr<QTemporaryDir> project_dir_;
  inline static std::unique_ptr<PJ::MainWindow> window_;
  inline static QStringList diagnostic_ids_;
};

// (a) D1/D7/D8: the batch job's driven ingest displays under layout-batch
// ownership; observers exist exactly while the batch does.
TEST_F(MainWindowLayoutImportBinderTest, BatchIngestDisplaysOnStripAndObserversAreBatchScoped) {
  EXPECT_EQ(Peer::batchObserverCount(window()), 0) << "no batch yet -> no observers (D7)";

  const PJ::DatasetId dataset = startParkedBatchJob(QStringLiteral("a"));
  ASSERT_NE(dataset, 0u);
  EXPECT_EQ(Peer::batchObserverCount(window()), 3) << "the handover installs the three lifecycle observers";
  EXPECT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kNone) << "no ingest began yet";
  EXPECT_FALSE(Peer::stripEngaged(window()));

  beginIngestFor(dataset, QStringLiteral("Fetching a.mcap"), 100);
  EXPECT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kLayoutBatch);
  EXPECT_EQ(Peer::stripOwnerDataset(window()), dataset);
  EXPECT_TRUE(Peer::stripEngaged(window()));
  EXPECT_EQ(Peer::stripTitle(window()), QStringLiteral("Fetching a.mcap"));

  updateIngestFor(dataset, 50, 100);
  EXPECT_EQ(Peer::stripProgressMaximum(window()), kProgressResolution);
  EXPECT_EQ(Peer::stripProgressValue(window()), kProgressResolution / 2);

  endIngestFor(dataset);
  EXPECT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kNone) << "no survivor -> ownership cleared";
  EXPECT_EQ(Peer::batchObserverCount(window()), 3) << "the batch itself is still running";
  ASSERT_TRUE(pumpUntil([]() { return !Peer::stripEngaged(window()); })) << "the strip must linger-hide";

  settleRestore();
  EXPECT_EQ(Peer::batch(window()), nullptr);
  EXPECT_EQ(Peer::batchObserverCount(window()), 0) << "observers must die with the batch (D1/D7)";
}

// (c) D7: a layout with no <materialize> traverses zero binder code.
TEST_F(MainWindowLayoutImportBinderTest, PlainLayoutTraversesZeroBinderCode) {
  const QString layout = project_dir_->filePath(QStringLiteral("plain-c.pj4.xml"));
  ASSERT_TRUE(fake::writeLayoutFile(layout, buildGenericLayoutDoc()));

  Peer::loadLayout(window(), layout, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([]() { return !Peer::progressiveInFlight(window()); }));

  EXPECT_EQ(Peer::batch(window()), nullptr);
  EXPECT_EQ(Peer::batchObserverCount(window()), 0);
  EXPECT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kNone);
  EXPECT_FALSE(Peer::stripEngaged(window()));
}

// (b) D3: the strip's Stop with the batch job displayed cancels exactly the
// active job (keep-partial), and the batch continues to its next job — the
// restore then COMMITS (retain+diagnose), never the rollback unwind.
TEST_F(MainWindowLayoutImportBinderTest, StripStopRoutesToActiveJobKeepPartialAndBatchContinues) {
  const QString first_source = project_dir_->filePath(QStringLiteral("data/b-first.mcap"));
  const QString second_source = project_dir_->filePath(QStringLiteral("data/b-second.mcap"));
  QDomDocument doc = fake::buildImportLayoutDoc(
      QStringLiteral("/curve-b"), first_source,
      pj_fake_import::makeDescriptor(
          QStringLiteral("b-first"), QStringLiteral("trusted"), first_source, 0,
          QStringLiteral("announce-then-block")));
  appendMaterializeFileInfo(
      doc, second_source,
      pj_fake_import::makeDescriptor(QStringLiteral("b-second"), QStringLiteral("trusted"), second_source));
  const QString layout = project_dir_->filePath(QStringLiteral("b.pj4.xml"));
  ASSERT_TRUE(fake::writeLayoutFile(layout, doc));

  Peer::loadLayout(window(), layout, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([]() {
    auto* batch = Peer::batch(window());
    return batch != nullptr && batch->activeImportDataset().has_value();
  }));
  const PJ::DatasetId dataset = Peer::batch(window())->activeImportDataset().value();

  beginIngestFor(dataset, QStringLiteral("batch-b"), 0);
  ASSERT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kLayoutBatch);

  ASSERT_TRUE(Peer::clickStripStop(window()));
  // The ingest teardown that follows a cancelled job in production.
  endIngestFor(dataset);
  ASSERT_TRUE(pumpUntil([]() { return !Peer::progressiveInFlight(window()); }));
  flushQueuedEvents();

  EXPECT_EQ(pj_fake_import::g_log.snapshot(), (std::vector<std::string>{"b-first", "b-second"}))
      << "the batch must continue past the cancelled job";
  EXPECT_GE(diagnostic_ids_.count(QStringLiteral("layout-import-cancelled")), 1)
      << "the active job must reach its kCancelled terminal; ids seen: "
      << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();
  // The commit-drain pair: the cancelled-batch UNWIND (a rollback) clears the
  // binder and never runs the unresolved-curves finalization — seeing both
  // proves the keep-partial route, not batch->cancel().
  EXPECT_TRUE(diagnostic_ids_.contains(QStringLiteral("layout-import-unresolved-curves")))
      << "ids seen: " << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();
  EXPECT_FALSE(Peer::binderEmpty(window())) << "the retained intent proves the commit path (no rollback)";
  EXPECT_EQ(Peer::batch(window()), nullptr);
  EXPECT_EQ(Peer::batchObserverCount(window()), 0);
}

// (d) D8 arbitration (rule documented at MainWindow's owner state): the
// newest begin takes the strip, an interactive ingest always outranks the
// background batch, Stop routes to the DISPLAYED owner only, and the ending
// displayed ingest hands the strip to the survivor with ITS label/progress.
TEST_F(MainWindowLayoutImportBinderTest, InteractiveIngestOutranksBatchAndStripSwitchesToSurvivor) {
  const PJ::DatasetId batch_dataset = startParkedBatchJob(QStringLiteral("d"));
  ASSERT_NE(batch_dataset, 0u);
  beginIngestFor(batch_dataset, QStringLiteral("batch-d"), 0);
  ASSERT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kLayoutBatch);

  const PJ::DatasetId interactive = pj_test::createDataset(appSession(), "inter-d");
  ASSERT_NE(interactive, 0u);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), interactive, "/inter-d"), 0u);
  beginIngestFor(interactive, QStringLiteral("inter-d"), 100);
  EXPECT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kInteractiveToolbox);
  EXPECT_EQ(Peer::stripOwnerDataset(window()), interactive);
  EXPECT_EQ(Peer::stripTitle(window()), QStringLiteral("inter-d"));

  // The batch may never displace a displayed interactive ingest (a repeated
  // begin restarts its entry — the arbitration must still refuse it).
  beginIngestFor(batch_dataset, QStringLiteral("batch-d"), 0);
  EXPECT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kInteractiveToolbox);
  EXPECT_EQ(Peer::stripOwnerDataset(window()), interactive);

  // Stop routes to the DISPLAYED owner only: the batch job must survive.
  ASSERT_TRUE(Peer::clickStripStop(window()));
  flushQueuedEvents();
  ASSERT_NE(Peer::batch(window()), nullptr);
  EXPECT_TRUE(Peer::batch(window())->activeImportDataset().has_value())
      << "Stop with an interactive owner displayed must not cancel the batch job";
  EXPECT_EQ(diagnostic_ids_.count(QStringLiteral("layout-import-cancelled")), 0);

  // Displayed progress belongs to the displayed owner.
  updateIngestFor(interactive, 30, 100);
  EXPECT_EQ(Peer::stripProgressMaximum(window()), kProgressResolution);
  EXPECT_EQ(Peer::stripProgressValue(window()), 300);

  // The displayed ingest ends -> the strip switches to the surviving batch
  // ingest with ITS label (never stale text).
  endIngestFor(interactive);
  EXPECT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kLayoutBatch);
  EXPECT_EQ(Peer::stripOwnerDataset(window()), batch_dataset);
  EXPECT_EQ(Peer::stripTitle(window()), QStringLiteral("batch-d"));

  endIngestFor(batch_dataset);
  EXPECT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kNone);
  settleRestore();
  EXPECT_EQ(Peer::batchObserverCount(window()), 0);
}

// (f) D1 teardown ordering: a superseding load mid-ingest disconnects the
// observers and clears strip ownership BEFORE the batch dies, and the strip
// is handed to the surviving interactive ingest. Runs before the D9
// scenario: its final driven END has no production route while no batch is
// active (real ends go through the interactive host callback), so the
// displayed owner it leaves behind is a test artifact — tolerable for D9's
// fold/pin assertions, but this scenario's owner assertions must not
// inherit one.
TEST_F(MainWindowLayoutImportBinderTest, SupersedeMidIngestClearsObserversAndReselectsSurvivor) {
  // The interactive ingest exists BEFORE the batch: with no observers and no
  // interactive host callback in the loop, nothing displays it yet.
  const PJ::DatasetId interactive = pj_test::createDataset(appSession(), "inter-f");
  ASSERT_NE(interactive, 0u);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), interactive, "/inter-f"), 0u);
  beginIngestFor(interactive, QStringLiteral("inter-f"), 50);
  EXPECT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kNone);

  const PJ::DatasetId batch_dataset = startParkedBatchJob(QStringLiteral("f"));
  ASSERT_NE(batch_dataset, 0u);
  beginIngestFor(batch_dataset, QStringLiteral("batch-f"), 0);
  ASSERT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kLayoutBatch);
  ASSERT_EQ(Peer::batchObserverCount(window()), 3);

  // A second layout load supersedes the restore MID-INGEST (batch hard
  // shutdown): observers off + ownership handed to the survivor before the
  // batch dies, and the loop keeps running safely afterwards.
  const QString generic = project_dir_->filePath(QStringLiteral("generic-f.pj4.xml"));
  ASSERT_TRUE(fake::writeLayoutFile(generic, buildGenericLayoutDoc()));
  Peer::loadLayout(window(), generic, /*interactive=*/false);

  EXPECT_EQ(Peer::batch(window()), nullptr);
  EXPECT_EQ(Peer::batchObserverCount(window()), 0);
  EXPECT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kInteractiveToolbox)
      << "the surviving interactive ingest must be reselected on the strip";
  EXPECT_EQ(Peer::stripOwnerDataset(window()), interactive);
  EXPECT_EQ(Peer::stripTitle(window()), QStringLiteral("inter-f"));
  flushQueuedEvents();  // no crash, no stale delivery

  // The dead batch's lifecycle entry ends late (its producer's teardown in
  // production): it must not disturb the reselected owner.
  endIngestFor(batch_dataset);
  EXPECT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kInteractiveToolbox);
  EXPECT_EQ(Peer::stripOwnerDataset(window()), interactive);

  endIngestFor(interactive);
}

// (e) D9: a headless batch job never traverses the panel-fold path, and the
// batch drain leaves a busy pinned interactive panel — and a concurrent
// interactive ingest — untouched (#498 coexistence).
TEST_F(MainWindowLayoutImportBinderTest, HeadlessBatchJobNeverFoldsPanelsNorDisplacesBusyPinnedPanel) {
  Peer::setHostSeams(
      window(),
      [this](QString /*label*/) {
        confirm_shown_ = true;
        return false;
      },
      [this](PJ::ToolboxRuntimeHost* host) { return host == pinnedHostToken() && panel_busy_; },
      [this](PJ::ToolboxRuntimeHost* host) { stopped_hosts_.push_back(host); });

  // A busy pinned panel (the :8950 restore guard's subject)...
  Peer::moveToTab(
      window(), new QWidget, QStringLiteral("binder-e-pinned"), QStringLiteral("Pinned"), pinnedHostToken());
  ASSERT_TRUE(Peer::isPinned(window(), QStringLiteral("binder-e-pinned")));
  panel_busy_ = true;

  // ...plus a toolbox in the chart-area takeover (the interactive ingest-start
  // fold's only target) and a concurrent driven interactive ingest.
  const QString takeover_id = QStringLiteral("binder-e-takeover");
  ASSERT_TRUE(Peer::dockBare(window(), new QWidget, takeover_id, QStringLiteral("Takeover"), nullptr));
  const PJ::DatasetId interactive = pj_test::createDataset(appSession(), "inter-e");
  ASSERT_NE(interactive, 0u);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), interactive, "/inter-e"), 0u);
  beginIngestFor(interactive, QStringLiteral("inter-e"), 0);

  const PJ::DatasetId batch_dataset = startParkedBatchJob(QStringLiteral("e"));
  ASSERT_NE(batch_dataset, 0u);
  EXPECT_TRUE(Peer::isTakeover(window(), takeover_id)) << "a batch job start must not fold any panel";
  EXPECT_TRUE(Peer::takeoverPanelPresent(window()));

  beginIngestFor(batch_dataset, QStringLiteral("batch-e"), 0);
  updateIngestFor(batch_dataset, 1, 2);
  EXPECT_TRUE(Peer::isTakeover(window(), takeover_id))
      << "a headless batch ingest must never traverse the panel-fold path (D9)";

  endIngestFor(batch_dataset);
  settleRestore();

  // The drain's restore (kRetainAndDiagnose) retained the busy pinned panel
  // unprompted and cancelled nothing.
  EXPECT_TRUE(diagnostic_ids_.contains(QStringLiteral("pinned-toolbox-busy")))
      << "ids seen: " << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();
  EXPECT_TRUE(Peer::isPinned(window(), QStringLiteral("binder-e-pinned")));
  EXPECT_TRUE(stopped_hosts_.empty()) << "the batch drain must not stop the pinned panel's job";
  EXPECT_FALSE(confirm_shown_) << "a batch drain must never raise the cancel-confirmation modal";
  EXPECT_TRUE(Peer::isTakeover(window(), takeover_id));
  EXPECT_TRUE(sessionManager().ingestActive(interactive)) << "the interactive ingest must ride through untouched";

  endIngestFor(interactive);
}

// (g) THE HEADLINE PIN (D6): a retained curve intent binds WHILE the import
// job grows, through the REAL host surfaces only — the job worker creates
// the dataset via the bound write host, announces it, publishes one record,
// ticks the real ingest once, and parks. No SessionManager signal is driven
// by the test; the strip lights up and the curve binds through the
// production headless-session -> SessionManager -> CatalogModel chain. The
// drain-time half of this guarantee stays pinned by
// MissImportKeepsRestoreAliveAndBindsAtDrain (unchanged).
TEST_F(MainWindowLayoutImportBinderTest, ProgressiveImportBindsCurveMidJobThroughRealHostSurfaces) {
  const QString source = project_dir_->filePath(QStringLiteral("data/prog.mcap"));  // never exists: a MISS
  loadProgressiveLayout(
      QStringLiteral("prog"), QStringLiteral("/curve-prog"), source,
      makeProgressiveDescriptor(QStringLiteral("prog"), source, QStringLiteral("/curve-prog"), /*promoted=*/false));

  // The curve must bind MID-JOB: the job is parked on its gate, so a binder
  // that only completes at drain would never satisfy this wait.
  ASSERT_TRUE(pumpUntil([]() { return Peer::totalCurveCount(window()) == 1; }))
      << "the curve must bind while the import job is still running";

  // Mid-import invariants, all at this moment: restore alive, job alive.
  EXPECT_TRUE(Peer::progressiveInFlight(window()));
  auto* batch = Peer::batch(window());
  ASSERT_NE(batch, nullptr);
  ASSERT_TRUE(batch->activeImportDataset().has_value()) << "the job must not have terminated";
  const PJ::DatasetId dataset = *batch->activeImportDataset();
  EXPECT_TRUE(sessionManager().ingestActive(dataset)) << "the real ingest lifecycle entry must be open";
  EXPECT_TRUE(Peer::binderEmpty(window())) << "the bound intent must have been consumed";

  // The strip engaged through the real chain, batch as displayed owner.
  EXPECT_EQ(Peer::stripOwnerKind(window()), StripOwnerKind::kLayoutBatch);
  EXPECT_EQ(Peer::stripOwnerDataset(window()), dataset);
  EXPECT_TRUE(Peer::stripEngaged(window()));
  EXPECT_EQ(Peer::stripTitle(window()), QStringLiteral("prog")) << "the progressStart label reaches the strip";
  EXPECT_EQ(Peer::batchObserverCount(window()), 3);

  settleRestore();
  EXPECT_EQ(Peer::batch(window()), nullptr);
  EXPECT_EQ(Peer::totalCurveCount(window()), 1);
  EXPECT_GE(diagnostic_ids_.count(QStringLiteral("layout-import-eager-only")), 1)
      << "the non-promoted progressive job concludes EAGER_ONLY; ids seen: "
      << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();
  EXPECT_EQ(diagnostic_ids_.count(QStringLiteral("layout-import-job-failed")), 0)
      << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();
}

// (h) D2 ordering pin: on_dataset's GUI delivery (which installs the batch
// correlation) precedes the first ingestBegan delivery for that dataset —
// the consult-verified ABI/marshal order (same worker, same GUI queue,
// posted in order). Pinned so a future marshalling change fails here
// instead of silently breaking the exact-id filtering.
TEST_F(MainWindowLayoutImportBinderTest, OnDatasetCorrelationPrecedesFirstIngestBeganDelivery) {
  struct Probe {
    bool seen = false;
    bool correlated = false;
  };
  auto probe = std::make_shared<Probe>();
  QObject guard;  // scope-bound connection: auto-torn even on an ASSERT abort
  QObject::connect(
      &sessionManager(), &PJ::SessionManager::ingestBegan, &guard,
      [probe](PJ::IngestToken token, const QString& /*label*/, quint64 /*total*/) {
        if (probe->seen) {
          return;
        }
        const PJ::DatasetId dataset = token.dataset_id;
        probe->seen = true;
        auto* batch = Peer::batch(window());
        probe->correlated =
            batch != nullptr && batch->activeImportDataset().has_value() && *batch->activeImportDataset() == dataset;
      });

  const QString source = project_dir_->filePath(QStringLiteral("data/d2.mcap"));
  loadProgressiveLayout(
      QStringLiteral("d2"), QStringLiteral("/curve-d2"), source,
      makeProgressiveDescriptor(QStringLiteral("d2"), source, QStringLiteral("/curve-d2"), /*promoted=*/false));

  ASSERT_TRUE(pumpUntil([probe]() { return probe->seen; })) << "the real ingest never began";
  EXPECT_TRUE(probe->correlated)
      << "activeImportDataset() must already equal the id when its first ingestBegan is delivered (D2)";

  settleRestore();
}

// (i) D4: a promoted progressive job runs the REAL promotion transaction —
// SourcePromotionHostView against the staged mock_file_source_plugin, the
// FileLoader replace pipeline, kSucceededPromoted only after the async
// result reports ok. The mid-import-bound curve must survive the replace:
// DatasetId AND TopicId stay stable through beginRefill, and the artifact's
// rows stand in place of the eager ones.
TEST_F(MainWindowLayoutImportBinderTest, PromotedProgressiveJobSurvivesRealPromotionTransaction) {
  // The artifact EXISTS up front (as if a download had produced it), but the
  // scripted verdict says not-materialized so the source still classifies as
  // a MISS and the import job runs.
  ASSERT_TRUE(QDir(project_dir_->path()).mkpath(QStringLiteral("data")));
  const QString source = project_dir_->filePath(QStringLiteral("data/promo.mock"));
  {
    QFile artifact(source);
    ASSERT_TRUE(artifact.open(QIODevice::WriteOnly));
  }
  loadProgressiveLayout(
      QStringLiteral("promo"), QStringLiteral("mock/file_data"), source,
      makeProgressiveDescriptor(
          QStringLiteral("promo"), source, QStringLiteral("mock/file_data"), /*promoted=*/true,
          /*materialized=*/false));

  ASSERT_TRUE(pumpUntil([]() { return Peer::totalCurveCount(window()) == 1; }))
      << "the curve must bind mid-import through the real surfaces";
  auto* batch = Peer::batch(window());
  ASSERT_NE(batch, nullptr);
  ASSERT_TRUE(batch->activeImportDataset().has_value());
  const PJ::DatasetId dataset = *batch->activeImportDataset();
  EXPECT_TRUE(Peer::progressiveInFlight(window()));
  const auto mid_import_topics = sessionManager().createReader().listTopics(dataset);
  ASSERT_EQ(mid_import_topics.size(), 1u) << "the eager job published exactly one topic";
  const PJ::TopicId topic_id = mid_import_topics.front();

  settleRestore();

  // The real promotion replaced the dataset in place: same DatasetId, same
  // TopicId, the artifact's 3 rows standing in for the eager one.
  const auto post_topics = sessionManager().createReader().listTopics(dataset);
  ASSERT_EQ(post_topics.size(), 1u);
  EXPECT_EQ(post_topics.front(), topic_id) << "TopicId must survive the replace transaction";
  EXPECT_EQ(singleTopicRowCount(dataset), 3) << "the artifact's rows must have replaced the eager row";
  EXPECT_EQ(Peer::totalCurveCount(window()), 1) << "the mid-import-bound curve must survive the replace";
  EXPECT_TRUE(Peer::binderEmpty(window()));

  // kSucceededPromoted, not eager-only: the promotion attached the source
  // record (provider identity HOST-DERIVED from the binding) and the
  // eager-only degradation diagnostic must be absent.
  const PJ::SourceRecord* record = sessionManager().sourceRecord(dataset);
  ASSERT_NE(record, nullptr) << "a successful promotion must attach the source record";
  EXPECT_EQ(record->provider_id, QString::fromUtf8(fake::kProviderId));
  EXPECT_EQ(record->source_identity, QStringLiteral("fake:%1").arg(source));
  EXPECT_EQ(diagnostic_ids_.count(QStringLiteral("layout-import-eager-only")), 0)
      << "a promoted outcome must not report the eager-only degradation; ids seen: "
      << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();
  EXPECT_EQ(diagnostic_ids_.count(QStringLiteral("layout-import-job-failed")), 0)
      << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();
  EXPECT_EQ(diagnostic_ids_.count(QStringLiteral("layout-import-cancelled")), 0);
}

// (j) Out-of-band pure binder regression net (explicitly NON-headline, the
// only scenario allowed to inject rows instead of driving the real write
// host): with an announce-then-block job parked, rows injected via the
// datastore helpers plus a driven SessionManager lifecycle must bind the
// retained intent mid-import — the binder logic alone, isolated from the
// D6 plugin-side machinery.
TEST_F(MainWindowLayoutImportBinderTest, OutOfBandRowsBindPendingCurveMidImport) {
  const PJ::DatasetId announced = startParkedBatchJob(QStringLiteral("oob"));
  ASSERT_NE(announced, 0u);
  EXPECT_EQ(Peer::totalCurveCount(window()), 0) << "no data yet -> the curve is pending";

  const PJ::DatasetId dataset = pj_test::createDataset(appSession(), "oob-data");
  ASSERT_NE(dataset, 0u);
  beginIngestFor(dataset, QStringLiteral("oob"), 2);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), dataset, "/curve-oob"), 0u);  // rebuild -> itemsAdded
  updateIngestFor(dataset, 1, 2);

  EXPECT_EQ(Peer::totalCurveCount(window()), 1) << "the intent must bind mid-import, not only at drain";
  EXPECT_TRUE(Peer::progressiveInFlight(window()));
  EXPECT_TRUE(Peer::binderEmpty(window()));

  endIngestFor(dataset);
  settleRestore();
  EXPECT_EQ(Peer::totalCurveCount(window()), 1);
  EXPECT_EQ(Peer::batch(window()), nullptr);
}

}  // namespace

PJ_MAIN_WINDOW_TEST_MAIN
