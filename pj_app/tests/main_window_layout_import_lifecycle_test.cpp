// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Restore-lifecycle GUI scenarios for the Codex r1 findings (one MainWindow
// per binary, scenarios run sequentially on the same window):
//   S1 (F3)  mixed kAutomated layout — every materialize source fails
//            synchronously (batch discarded as kNoAsyncWork) while a plain
//            sibling keeps FileLoader busy: the progressive policy must come
//            from the LOAD's interactivity, not from batch survival — zero
//            dialogs, retain+diagnose at drain.
//   S2 (F6)  progressive apply failure (xmlLoadState rejects the doc) on a
//            kAutomated restore: diagnostic, never the modal warning.
//   S3 (F2)  a sync-path layout load superseding an in-flight progressive
//            restore must tear the WHOLE transaction down (flag, waiters,
//            batch) — not just the batch.
//   S4 (F4)  kAutomated failing plain source: no load-failure modal.
//   S5 (F4)  the same load kInteractive: the reload prompt and the failure
//            dialog still appear (pin).

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QPushButton>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include "pj_runtime/CatalogModel.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

#ifndef PJ_MOCK_FILE_SOURCE_PLUGIN_PATH
#error "PJ_MOCK_FILE_SOURCE_PLUGIN_PATH must be defined"
#endif

namespace {

using PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::pumpUntil;
namespace fake = pj_layout_import_gui;

// Scans for visible modal dialogs from inside their own nested event loop
// (timer-driven), records what it saw, and clicks/closes so a test FAILS on
// an unexpected dialog instead of hanging on its exec().
struct DialogProbe {
  QTimer timer;
  bool saw_any = false;
  int one_button_modals = 0;    // e.g. the load-failure warning (OK)
  int multi_button_modals = 0;  // e.g. the 3-way reload prompt
  // Per multi-button modal: which button to click, consumed front-first
  // (empty = the first button). Lets a scenario answer "Cancel" (index 2).
  QList<int> multi_click_sequence;

  DialogProbe() {
    timer.setInterval(30);
    QObject::connect(&timer, &QTimer::timeout, &timer, [this]() { scan(); });
  }
  void scan() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
      if (!widget->isModal() || !widget->isVisible()) {
        continue;
      }
      saw_any = true;
      const auto buttons = widget->findChildren<QPushButton*>();
      if (buttons.size() >= 2) {
        ++multi_button_modals;
        int index = 0;
        if (!multi_click_sequence.isEmpty()) {
          index = multi_click_sequence.takeFirst();
        }
        buttons.at(qBound(0, index, static_cast<int>(buttons.size()) - 1))->click();
      } else if (!buttons.isEmpty()) {
        ++one_button_modals;
        buttons.first()->click();  // single-OK warning
      } else {
        widget->close();
      }
    }
  }
  void reset() {
    saw_any = false;
    one_button_modals = 0;
    multi_button_modals = 0;
    multi_click_sequence.clear();
  }
};

// A plain (record-less) <fileInfo> for the mock source plugin, appended next
// to whatever buildImportLayoutDoc produced.
void appendPlainFileInfo(QDomDocument& doc, const QString& path, const QString& preset_json) {
  QDomElement wrapper = doc.documentElement().firstChildElement(QStringLiteral("previouslyLoaded_Datafiles"));
  QDomElement file_info = doc.createElement(QStringLiteral("fileInfo"));
  file_info.setAttribute(QStringLiteral("filename"), path);
  QDomElement plugin = doc.createElement(QStringLiteral("plugin"));
  plugin.setAttribute(QStringLiteral("ID"), QStringLiteral("Mock File Source"));
  plugin.setAttribute(QStringLiteral("manifest_id"), QStringLiteral("mock-file-source"));
  PJ::layout_xml::appendJsonAsCdata(doc, plugin, preset_json);
  file_info.appendChild(plugin);
  wrapper.appendChild(file_info);
}

// Minimal applyable layout: one empty plot; optionally one plain fileInfo.
[[nodiscard]] QDomDocument buildShellDoc(
    const QString& binding, const QString& plain_path = {}, const QString& preset_json = QStringLiteral("{}")) {
  QDomDocument doc;
  QDomElement root = doc.createElement(QStringLiteral("root"));
  root.setAttribute(QStringLiteral("pj4_version"), QStringLiteral("4"));
  root.setAttribute(QStringLiteral("binding"), binding);
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
  QDomElement wrapper = doc.createElement(QStringLiteral("previouslyLoaded_Datafiles"));
  root.appendChild(wrapper);
  if (!plain_path.isEmpty()) {
    appendPlainFileInfo(doc, plain_path, preset_json);
  }
  return doc;
}

TEST(MainWindowLayoutImportLifecycleTest, RestoreLifecycleHonorsPolicyAndSupersession) {
  QTemporaryDir extensions_dir;
  QTemporaryDir project_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(project_dir.isValid());
  // Stage the mock source plugin so plain .mock replays load for real.
  const QString plugin_src = QString::fromUtf8(PJ_MOCK_FILE_SOURCE_PLUGIN_PATH);
  ASSERT_TRUE(QFile::copy(plugin_src, extensions_dir.filePath(QFileInfo(plugin_src).fileName())));

  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = MainWindowLayoutImportTestPeer::session(window);
  ASSERT_TRUE(app.extensionCatalog().pluginCatalog().registerStaticToolbox(&fake::kFakeVtable));

  QStringList diagnostic_ids;
  fake::recordDiagnosticIds(window, diagnostic_ids);
  DialogProbe probe;
  probe.timer.start();

  // ---- S1 (F3): mixed kAutomated — batch dies synchronously, plain load
  // keeps the restore progressive; the policy must still be non-interactive.
  const QString plain_path = pj_app_test::makeMockFile(project_dir, QStringLiteral("plain.mock"));
  const QString dead_source = project_dir.filePath(QStringLiteral("data/dead.mcap"));
  QDomDocument mixed = fake::buildImportLayoutDoc(
      QStringLiteral("/nonexistent"), dead_source,
      fake::makeDescriptor(QStringLiteral("trusted"), dead_source, QStringLiteral("eager")),
      QStringLiteral("no-such-provider"));
  appendPlainFileInfo(mixed, plain_path, QStringLiteral("{}"));
  const QString mixed_path = project_dir.filePath(QStringLiteral("mixed.pj4.xml"));
  ASSERT_TRUE(fake::writeLayoutFile(mixed_path, mixed));

  MainWindowLayoutImportTestPeer::loadLayout(window, mixed_path, /*interactive=*/false);
  EXPECT_TRUE(MainWindowLayoutImportTestPeer::progressiveInFlight(window))
      << "the plain sibling keeps the restore progressive";
  ASSERT_TRUE(pumpUntil([&window]() { return !MainWindowLayoutImportTestPeer::progressiveInFlight(window); }));
  EXPECT_FALSE(probe.saw_any) << "S1: a kAutomated mixed restore opened a dialog (policy inferred from batch survival)";
  EXPECT_FALSE(MainWindowLayoutImportTestPeer::binderEmpty(window))
      << "S1: the unresolved intent must be retained, not prompted away";
  // Diagnostics reach the recorder through a queued emit: it can still be in the
  // event queue when the restore reports itself settled.
  EXPECT_TRUE(pumpUntil([&diagnostic_ids]() {
    return diagnostic_ids.contains(QStringLiteral("layout-import-unresolved-curves"));
  })) << "S1 ids seen: "
      << diagnostic_ids.join(QStringLiteral(", ")).toStdString();

  // ---- S2 (F6): progressive apply failure on a kAutomated restore.
  probe.reset();
  diagnostic_ids.clear();
  const QString import_source = project_dir.filePath(QStringLiteral("data/import.mcap"));
  QDomDocument broken = fake::buildImportLayoutDoc(
      QStringLiteral("/left"), import_source,
      fake::makeDescriptor(QStringLiteral("trusted"), import_source, QStringLiteral("eager")));
  broken.documentElement().removeChild(broken.documentElement().firstChildElement(QStringLiteral("tabbed_widget")));
  const QString broken_path = project_dir.filePath(QStringLiteral("broken.pj4.xml"));
  ASSERT_TRUE(fake::writeLayoutFile(broken_path, broken));

  MainWindowLayoutImportTestPeer::loadLayout(window, broken_path, /*interactive=*/false);
  EXPECT_FALSE(probe.saw_any) << "S2: a kAutomated apply failure opened the modal warning";
  EXPECT_TRUE(pumpUntil(
      [&diagnostic_ids]() { return diagnostic_ids.contains(QStringLiteral("layout-progressive-apply-failed")); },
      /*timeout_ms=*/5000))
      << "S2: the apply failure must surface as a diagnostic; ids seen: "
      << diagnostic_ids.join(QStringLiteral(", ")).toStdString();
  EXPECT_FALSE(MainWindowLayoutImportTestPeer::progressiveInFlight(window));

  // ---- S3 (F2): a sync-path load supersedes an in-flight restore fully.
  probe.reset();
  const QString blocked_source = project_dir.filePath(QStringLiteral("data/blocked.mcap"));
  const QString blocked_path = project_dir.filePath(QStringLiteral("blocked.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          blocked_path, QStringLiteral("/left"), blocked_source,
          fake::makeDescriptor(QStringLiteral("trusted"), blocked_source, QStringLiteral("block"))));
  MainWindowLayoutImportTestPeer::loadLayout(window, blocked_path, /*interactive=*/false);
  ASSERT_TRUE(MainWindowLayoutImportTestPeer::progressiveInFlight(window));
  ASSERT_TRUE(MainWindowLayoutImportTestPeer::batchActive(window));
  ASSERT_GT(MainWindowLayoutImportTestPeer::restoreWaiterCount(window), 0);

  const QString generic_path = project_dir.filePath(QStringLiteral("generic.pj4.xml"));
  ASSERT_TRUE(fake::writeLayoutFile(generic_path, buildShellDoc(QStringLiteral("generic"))));
  MainWindowLayoutImportTestPeer::loadLayout(window, generic_path, /*interactive=*/false);

  EXPECT_FALSE(MainWindowLayoutImportTestPeer::progressiveInFlight(window))
      << "S3: the sync-path load must tear down the superseded progressive transaction";
  EXPECT_EQ(MainWindowLayoutImportTestPeer::restoreWaiterCount(window), 0)
      << "S3: stale waiters could commit the OLD document over the new workspace";
  EXPECT_EQ(MainWindowLayoutImportTestPeer::batch(window), nullptr);
  EXPECT_TRUE(MainWindowLayoutImportTestPeer::binderEmpty(window));
  EXPECT_FALSE(probe.saw_any) << "S3: the supersede sequence opened a dialog";

  // ---- S4 (F4): kAutomated failing plain source -> zero dialogs. The
  // failure mode is the loader prologue's fail() (no plugin serves the
  // extension) — the ONE path that raises the aggregated load-failure
  // dialog (reportLoadWarning); start-time plugin failures never do.
  probe.reset();
  diagnostic_ids.clear();
  const QString failing_path = pj_app_test::makeMockFile(project_dir, QStringLiteral("failing.zzz"));
  const QString failing_layout = project_dir.filePath(QStringLiteral("failing.pj4.xml"));
  ASSERT_TRUE(
      fake::writeLayoutFile(
          failing_layout, buildShellDoc(QStringLiteral("source"), failing_path, QStringLiteral("{}"))));

  MainWindowLayoutImportTestPeer::loadLayout(window, failing_layout, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&window]() { return !MainWindowLayoutImportTestPeer::progressiveInFlight(window); }));
  pj_app_test::flushQueuedEvents();
  probe.scan();  // catch a lingering non-modal-loop dialog too
  EXPECT_FALSE(probe.saw_any) << "S4: a kAutomated plain-source failure opened the load-failure dialog";

  // ---- S6 (Codex r2-3): pre-classification ENTRY failures — unreadable
  // path / malformed XML — honor the kAutomated zero-dialog contract too.
  probe.reset();
  diagnostic_ids.clear();
  const QString missing_layout = project_dir.filePath(QStringLiteral("does-not-exist.pj4.xml"));
  const QString malformed_layout = project_dir.filePath(QStringLiteral("malformed.pj4.xml"));
  {
    QFile malformed(malformed_layout);
    ASSERT_TRUE(malformed.open(QIODevice::WriteOnly));
    malformed.write("this is <<< not xml");
  }
  MainWindowLayoutImportTestPeer::loadLayout(window, missing_layout, /*interactive=*/false);
  MainWindowLayoutImportTestPeer::loadLayout(window, malformed_layout, /*interactive=*/false);
  probe.scan();
  EXPECT_FALSE(probe.saw_any) << "S6: a kAutomated entry failure opened a modal";
  EXPECT_TRUE(pumpUntil([&diagnostic_ids]() {
    return diagnostic_ids.contains(QStringLiteral("layout-open-failed")) &&
           diagnostic_ids.contains(QStringLiteral("layout-parse-failed"));
  })) << "S6: entry failures must surface as diagnostics; ids seen: "
      << diagnostic_ids.join(QStringLiteral(", ")).toStdString();
  // Interactive pin: both entry failures keep their modal warnings.
  probe.reset();
  MainWindowLayoutImportTestPeer::loadLayout(window, missing_layout, /*interactive=*/true);
  MainWindowLayoutImportTestPeer::loadLayout(window, malformed_layout, /*interactive=*/true);
  ASSERT_TRUE(pumpUntil([&probe]() { return probe.one_button_modals >= 2; }))
      << "S6: interactive entry failures must still warn (saw " << probe.one_button_modals << ")";

  // ---- S5 (F4 pin): the same failing load kInteractive keeps its dialogs.
  probe.reset();
  MainWindowLayoutImportTestPeer::loadLayout(window, failing_layout, /*interactive=*/true);
  ASSERT_TRUE(pumpUntil([&window]() { return !MainWindowLayoutImportTestPeer::progressiveInFlight(window); }));
  ASSERT_TRUE(pumpUntil([&probe]() { return probe.one_button_modals > 0; }))
      << "S5: the interactive load-failure dialog must still appear";
  EXPECT_GE(probe.multi_button_modals, 1) << "S5: the interactive reload prompt must still appear";

  // ---- S7 (regression review): cancelling the reload prompt of a SECOND
  // layout load must be a COMPLETE no-op — the in-flight restore A keeps
  // draining and completes exactly like an undisturbed run (base behavior).
  probe.reset();
  diagnostic_ids.clear();
  const QString a_source = project_dir.filePath(QStringLiteral("data/a-restore.mcap"));
  const QString a_layout = project_dir.filePath(QStringLiteral("a-restore.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          a_layout, QStringLiteral("/left"), a_source,
          fake::makeDescriptor(QStringLiteral("trusted"), a_source, QStringLiteral("block"))));
  MainWindowLayoutImportTestPeer::loadLayout(window, a_layout, /*interactive=*/false);
  ASSERT_TRUE(MainWindowLayoutImportTestPeer::progressiveInFlight(window));
  ASSERT_TRUE(MainWindowLayoutImportTestPeer::batchActive(window));
  ASSERT_EQ(MainWindowLayoutImportTestPeer::restoreWaiterCount(window), 2);

  const QString b_plain = pj_app_test::makeMockFile(project_dir, QStringLiteral("b-cancel.mock"));
  const QString b_layout = project_dir.filePath(QStringLiteral("b-cancel.pj4.xml"));
  ASSERT_TRUE(fake::writeLayoutFile(b_layout, buildShellDoc(QStringLiteral("source"), b_plain)));
  probe.multi_click_sequence = {2};  // answer "Cancel" on the 3-way reload prompt
  MainWindowLayoutImportTestPeer::loadLayout(window, b_layout, /*interactive=*/true);
  EXPECT_GE(probe.multi_button_modals, 1) << "S7: the reload prompt must have appeared";
  EXPECT_TRUE(MainWindowLayoutImportTestPeer::progressiveInFlight(window))
      << "S7: Cancel must leave restore A draining, not half-torn-down";
  EXPECT_TRUE(MainWindowLayoutImportTestPeer::batchActive(window)) << "S7: A's import must still be running";
  EXPECT_EQ(MainWindowLayoutImportTestPeer::restoreWaiterCount(window), 2) << "S7: A's drain gate must survive";

  // A completes identically to an undisturbed run: release the gated import,
  // drain runs finalization (kAutomated -> retain + diagnose), batch retires.
  ASSERT_NE(fake::g_instance, nullptr) << "S7: A's provider instance must still be alive";
  fake::g_instance->releaseStart();
  ASSERT_TRUE(pumpUntil([&window]() { return !MainWindowLayoutImportTestPeer::progressiveInFlight(window); }));
  EXPECT_TRUE(pumpUntil(
      [&diagnostic_ids]() { return diagnostic_ids.contains(QStringLiteral("layout-import-unresolved-curves")); },
      /*timeout_ms=*/5000))
      << "S7: A's drain must run its finalization; ids seen: "
      << diagnostic_ids.join(QStringLiteral(", ")).toStdString();
  EXPECT_FALSE(MainWindowLayoutImportTestPeer::binderEmpty(window)) << "S7: A's retained intent must survive";
  EXPECT_EQ(MainWindowLayoutImportTestPeer::batch(window), nullptr) << "S7: A settled and retired its batch";
  EXPECT_EQ(probe.one_button_modals, 0) << "S7: A's automated drain must stay dialog-free";

  // ---- S8 (regression review pin): trust-gate Cancel = "keep what I had" —
  // the NEW batch dies with nothing enqueued and the OLD restore drains on.
  probe.reset();
  const QString a2_source = project_dir.filePath(QStringLiteral("data/a2.mcap"));
  const QString a2_layout = project_dir.filePath(QStringLiteral("a2.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          a2_layout, QStringLiteral("/left"), a2_source,
          fake::makeDescriptor(QStringLiteral("trusted"), a2_source, QStringLiteral("block"))));
  MainWindowLayoutImportTestPeer::loadLayout(window, a2_layout, /*interactive=*/false);
  ASSERT_TRUE(MainWindowLayoutImportTestPeer::progressiveInFlight(window));
  ASSERT_TRUE(MainWindowLayoutImportTestPeer::batchActive(window));

  const QString b2_source = project_dir.filePath(QStringLiteral("data/b2.mcap"));
  const QString b2_layout = project_dir.filePath(QStringLiteral("b2.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          b2_layout, QStringLiteral("/right"), b2_source,
          fake::makeDescriptor(QStringLiteral("confirm"), b2_source, QStringLiteral("eager"))));
  probe.multi_click_sequence = {0, 2};  // "Reload source file" -> "Cancel layout load"
  MainWindowLayoutImportTestPeer::loadLayout(window, b2_layout, /*interactive=*/true);
  EXPECT_GE(probe.multi_button_modals, 2) << "S8: reload + trust prompts must both have appeared";
  EXPECT_TRUE(MainWindowLayoutImportTestPeer::progressiveInFlight(window))
      << "S8: trust-gate Cancel must leave restore A2 draining";
  EXPECT_TRUE(MainWindowLayoutImportTestPeer::batchActive(window)) << "S8: A2's import must still be running";
  EXPECT_EQ(MainWindowLayoutImportTestPeer::restoreWaiterCount(window), 2) << "S8: A2's drain gate must survive";

  // Cleanup: a final automated load supersedes A2 (hard shutdown joins the
  // gated job via destroy, which releases its start gate).
  MainWindowLayoutImportTestPeer::loadLayout(window, generic_path, /*interactive=*/false);
  EXPECT_FALSE(MainWindowLayoutImportTestPeer::progressiveInFlight(window));
  EXPECT_EQ(MainWindowLayoutImportTestPeer::batch(window), nullptr);
}

}  // namespace

PJ_MAIN_WINDOW_TEST_MAIN
