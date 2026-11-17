// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A non-interactive batch restore opens no dialog (reload, trust or missing-curve prompt). Its unresolved curve
// intents are retained and reported through a diagnostic, so a later binding pass can still resolve them.

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

  // The import succeeds (eager) but never produces the curve's topic: the drain is left with an unresolved intent.
  const QString source_path = project_dir.filePath(QStringLiteral("data/run.mcap"));
  const QString layout_path = project_dir.filePath(QStringLiteral("import.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          layout_path, QStringLiteral("/left"), source_path,
          fake::makeDescriptor(QStringLiteral("trusted"), source_path, QStringLiteral("eager"))));

  MainWindowLayoutImportTestPeer::loadLayout(window, layout_path, /*interactive=*/false);
  EXPECT_TRUE(MainWindowLayoutImportTestPeer::progressiveInFlight(window));
  ASSERT_TRUE(pumpUntil([&window]() { return !MainWindowLayoutImportTestPeer::progressiveInFlight(window); }));

  // A modal prompt would have blocked the pump above (exec() never returns offscreen); probe anyway.
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    EXPECT_FALSE(widget->isModal() && widget->isVisible())
        << "a non-interactive restore opened a dialog: " << widget->metaObject()->className();
  }

  // The unresolved intent is retained and named in a diagnostic (kRetainAndDiagnose on the batch drain path).
  EXPECT_FALSE(MainWindowLayoutImportTestPeer::binderEmpty(window))
      << "unresolved intents must be retained for later binding, never cleared";
  // Diagnostics reach the recorder through a queued emit, still pending when the restore reports itself settled.
  EXPECT_TRUE(pumpUntil([&diagnostic_ids]() {
    return diagnostic_ids.contains(QStringLiteral("layout-import-unresolved-curves"));
  })) << "ids seen: "
      << diagnostic_ids.join(QStringLiteral(", ")).toStdString();
  EXPECT_EQ(MainWindowLayoutImportTestPeer::batch(window), nullptr) << "the settled restore must retire the batch";
  EXPECT_EQ(MainWindowLayoutImportTestPeer::totalCurveCount(window), 0);

  // Sync leg (same window): the provider is absent and the saved path is missing, so every source fails during
  // prepare() and the load falls through to the sync apply leg with the catalog still empty. The kAutomated load must
  // report a diagnostic instead of the catalog-empty warning.
  diagnostic_ids.clear();
  const QString sync_source = project_dir.filePath(QStringLiteral("data/gone.mcap"));
  const QString sync_layout = project_dir.filePath(QStringLiteral("sync.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          sync_layout, QStringLiteral("/left"), sync_source,
          fake::makeDescriptor(QStringLiteral("trusted"), sync_source, QStringLiteral("eager")),
          QStringLiteral("no-such-provider")));

  // A modal opened during the synchronous load would block it; this probe fires inside the modal's nested event loop
  // and closes it, so the test fails instead of hanging.
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
