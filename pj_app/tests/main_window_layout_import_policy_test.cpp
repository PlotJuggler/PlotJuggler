// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// GUI scenario 2 (T6, D5): a NON-INTERACTIVE batch restore must never open
// a dialog — not the reload prompt, not the trust prompt, not the
// missing-curve prompt at drain (which today fires unconditionally). Its
// unresolved curve intents are RETAINED (never cleared) and reported
// through a diagnostic, so a later binding pass can still resolve them.

#include <gtest/gtest.h>

#include <QApplication>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_runtime/CatalogModel.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

namespace {

using PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::pumpUntil;
namespace fake = pj_layout_import_gui;

TEST(MainWindowLayoutImportPolicyTest, NonInteractiveRestoreRetainsIntentsAndDiagnosesWithoutDialogs) {
  QTemporaryDir extensions_dir;
  QTemporaryDir project_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(project_dir.isValid());

  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = MainWindowLayoutImportTestPeer::session(window);
  ASSERT_TRUE(app.extensionCatalog().pluginCatalog().registerStaticToolbox(&fake::kFakeVtable));

  QStringList diagnostic_ids;
  fake::recordDiagnosticIds(window, diagnostic_ids);

  // The import succeeds (eager) but never produces the curve's topic: the
  // drain is left with an unresolved intent — the prompt trigger.
  const QString source_path = project_dir.filePath(QStringLiteral("data/run.mcap"));
  const QString layout_path = project_dir.filePath(QStringLiteral("import.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          layout_path, QStringLiteral("/left"), source_path,
          fake::makeDescriptor(QStringLiteral("trusted"), source_path, QStringLiteral("eager"))));

  MainWindowLayoutImportTestPeer::loadLayout(window, layout_path, /*interactive=*/false);
  EXPECT_TRUE(MainWindowLayoutImportTestPeer::progressiveInFlight(window));
  ASSERT_TRUE(pumpUntil([&window]() { return !MainWindowLayoutImportTestPeer::progressiveInFlight(window); }));

  // Zero dialogs: a modal prompt would have blocked the pump above (its
  // exec() never returns in an offscreen test); belt-and-braces, probe for
  // any visible modal widget.
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    EXPECT_FALSE(widget->isModal() && widget->isVisible())
        << "a non-interactive restore opened a dialog: " << widget->metaObject()->className();
  }

  // The unresolved intent is RETAINED — not cleared — and named in a
  // diagnostic (the kRetainAndDiagnose semantics on the batch drain path).
  EXPECT_FALSE(MainWindowLayoutImportTestPeer::binderEmpty(window))
      << "unresolved intents must be retained for later binding, never cleared";
  // Diagnostics reach the recorder through a queued emit: it can still be in the
  // event queue when the restore reports itself settled.
  EXPECT_TRUE(pumpUntil([&diagnostic_ids]() {
    return diagnostic_ids.contains(QStringLiteral("layout-import-unresolved-curves"));
  })) << "ids seen: "
      << diagnostic_ids.join(QStringLiteral(", ")).toStdString();
  EXPECT_EQ(MainWindowLayoutImportTestPeer::batch(window), nullptr) << "the settled restore must retire the batch";
  EXPECT_EQ(MainWindowLayoutImportTestPeer::totalCurveCount(window), 0);

  // --- SYNC-leg scenario (same window; the batch concludes synchronously) ---
  // §6.4 cross-machine degradation: the provider is absent AND the saved
  // path is missing, so every source fails during prepare() and the load
  // falls through to the SYNC apply leg with the catalog still empty. D5:
  // the kAutomated load must not open the catalog-empty warning (nor any
  // other dialog) — it reports a diagnostic instead.
  diagnostic_ids.clear();
  const QString sync_source = project_dir.filePath(QStringLiteral("data/gone.mcap"));
  const QString sync_layout = project_dir.filePath(QStringLiteral("sync.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          sync_layout, QStringLiteral("/left"), sync_source,
          fake::makeDescriptor(QStringLiteral("trusted"), sync_source, QStringLiteral("eager")),
          QStringLiteral("no-such-provider")));

  // A modal opened during the (synchronous) load would block it; this probe
  // fires inside the modal's nested event loop, records it, and closes it so
  // the test FAILS instead of hanging.
  bool dialog_seen = false;
  QTimer dialog_probe;
  dialog_probe.setInterval(50);
  QObject::connect(&dialog_probe, &QTimer::timeout, &window, [&dialog_seen]() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
      if (widget->isModal() && widget->isVisible()) {
        dialog_seen = true;
        widget->close();
      }
    }
  });
  dialog_probe.start();
  MainWindowLayoutImportTestPeer::loadLayout(window, sync_layout, /*interactive=*/false);
  dialog_probe.stop();

  EXPECT_FALSE(dialog_seen) << "a non-interactive SYNC-leg restore opened a dialog";
  EXPECT_FALSE(MainWindowLayoutImportTestPeer::progressiveInFlight(window))
      << "an all-failed batch must conclude synchronously";
  ASSERT_TRUE(pumpUntil([&diagnostic_ids]() {
    return diagnostic_ids.contains(QStringLiteral("layout-apply-no-data"));
  })) << "the catalog-empty abort must surface as a diagnostic; ids seen: "
      << diagnostic_ids.join(QStringLiteral(", ")).toStdString();
  EXPECT_TRUE(diagnostic_ids.contains(QStringLiteral("layout-import-provider-unavailable")))
      << "ids seen: " << diagnostic_ids.join(QStringLiteral(", ")).toStdString();
}

}  // namespace
