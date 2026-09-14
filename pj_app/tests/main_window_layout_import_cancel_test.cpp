// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// GUI scenario 3 (T6): cancelling a batch mid-import must roll the whole
// restore back — the batch removes any datasets it produced and
// exact-restores the pre-layout workspace (captured at start(), before the
// first mutation), and the shell unwinds the progressive scaffolding
// WITHOUT committing the half-applied layout.

#include <gtest/gtest.h>

#include <QApplication>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>

#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_runtime/CatalogModel.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

namespace {

using PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::pumpUntil;
namespace fake = pj_layout_import_gui;

TEST(MainWindowLayoutImportCancelTest, CancelMidImportRollsBackToPriorWorkspace) {
  QTemporaryDir extensions_dir;
  QTemporaryDir project_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(project_dir.isValid());

  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = MainWindowLayoutImportTestPeer::session(window);
  ASSERT_TRUE(app.extensionCatalog().pluginCatalog().registerStaticToolbox(&fake::kFakeVtable));

  QStringList diagnostic_ids;
  fake::recordDiagnosticIds(window, diagnostic_ids);

  // The prior workspace the rollback must reproduce.
  const int prior_plot_count = MainWindowLayoutImportTestPeer::plotCount(window);
  const int prior_curve_count = MainWindowLayoutImportTestPeer::totalCurveCount(window);

  const QString source_path = project_dir.filePath(QStringLiteral("data/run.mcap"));
  const QString layout_path = project_dir.filePath(QStringLiteral("import.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          layout_path, QStringLiteral("/left"), source_path,
          fake::makeDescriptor(QStringLiteral("trusted"), source_path, QStringLiteral("block"))));

  MainWindowLayoutImportTestPeer::loadLayout(window, layout_path, /*interactive=*/false);
  ASSERT_TRUE(MainWindowLayoutImportTestPeer::progressiveInFlight(window));
  ASSERT_TRUE(MainWindowLayoutImportTestPeer::batchActive(window));

  // Cancel while the import job is still blocked mid-flight.
  MainWindowLayoutImportTestPeer::batch(window)->cancel();
  ASSERT_TRUE(pumpUntil([&window]() { return MainWindowLayoutImportTestPeer::batch(window) == nullptr; }));

  // The restore unwound: nothing committed, the workspace is back to its
  // pre-layout shape, and the cancellation was reported.
  EXPECT_FALSE(MainWindowLayoutImportTestPeer::progressiveInFlight(window));
  EXPECT_EQ(MainWindowLayoutImportTestPeer::plotCount(window), prior_plot_count)
      << "the pre-layout workspace must be restored exactly";
  EXPECT_EQ(MainWindowLayoutImportTestPeer::totalCurveCount(window), prior_curve_count);
  EXPECT_TRUE(MainWindowLayoutImportTestPeer::binderEmpty(window))
      << "a cancelled restore leaves no retained intents behind";
  // Diagnostics reach the recorder through a queued emit, so the cancellation
  // report can still be in the event queue when the batch is already gone.
  EXPECT_TRUE(pumpUntil([&diagnostic_ids]() {
    return diagnostic_ids.contains(QStringLiteral("layout-import-cancelled"));
  })) << "ids seen: "
      << diagnostic_ids.join(QStringLiteral(", ")).toStdString();
}

}  // namespace

PJ_MAIN_WINDOW_TEST_MAIN
