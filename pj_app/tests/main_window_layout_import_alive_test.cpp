// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A materialize-miss import keeps the layout restore alive across the catalog-empty guard: a provider job is
// invisible to FileLoader's isBusy(), so without the batch an import-only layout would reach applyRestoredLayout on
// an empty catalog ("No data is loaded") and be lost. Once the import's data arrives, the drain binds the layout's
// curves and commits.

#include <gtest/gtest.h>

#include <QApplication>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "dataset_test_helpers.h"
#include "pj_runtime/CatalogModel.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

namespace {

using PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::pumpUntil;
namespace fake = pj_layout_import_gui;

TEST(MainWindowLayoutImportAliveTest, MissImportKeepsRestoreAliveAndBindsAtDrain) {
  QTemporaryDir extensions_dir;
  QTemporaryDir project_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(project_dir.isValid());

  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = MainWindowLayoutImportTestPeer::session(window);
  ASSERT_TRUE(app.extensionCatalog().pluginCatalog().registerStaticToolbox(&fake::kFakeVtable));

  // The saved source does not exist anywhere: a cache MISS by construction.
  const QString source_path = project_dir.filePath(QStringLiteral("data/run.mcap"));
  const QString layout_path = project_dir.filePath(QStringLiteral("import.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          layout_path, QStringLiteral("/left"), source_path,
          fake::makeDescriptor(QStringLiteral("trusted"), source_path, QStringLiteral("block"))));

  ASSERT_TRUE(app.catalogModel().isEmpty()) << "the scenario needs the catalog-empty guard armed";
  MainWindowLayoutImportTestPeer::loadLayout(window, layout_path, /*interactive=*/false);

  // The restore survived the guard: progressive restore in flight, the batch
  // (with its blocked import job) active, and the layout structure applied.
  EXPECT_TRUE(MainWindowLayoutImportTestPeer::progressiveInFlight(window))
      << "a pending import must keep the restore alive even though FileLoader is idle";
  ASSERT_TRUE(MainWindowLayoutImportTestPeer::batchActive(window));
  ASSERT_GE(MainWindowLayoutImportTestPeer::plotCount(window), 1) << "the layout structure must be applied";
  EXPECT_EQ(MainWindowLayoutImportTestPeer::totalCurveCount(window), 0) << "no data yet -> the curve is pending";

  // The provider's import "arrives": inject the dataset+topic the job is
  // fetching, then release the blocked worker so the job concludes.
  const PJ::DatasetId dataset = pj_test::createDataset(app, "run.mcap", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0u);
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/left", 1'000, 2'000), 0u);
  app.catalogModel().rebuildFromDatastore();
  ASSERT_NE(fake::g_instance, nullptr);
  fake::g_instance->releaseStart();

  ASSERT_TRUE(pumpUntil([&window]() { return !MainWindowLayoutImportTestPeer::progressiveInFlight(window); }));

  EXPECT_EQ(MainWindowLayoutImportTestPeer::totalCurveCount(window), 1)
      << "the layout's curve must bind once the import's topic exists";
  EXPECT_TRUE(MainWindowLayoutImportTestPeer::binderEmpty(window));
  EXPECT_EQ(MainWindowLayoutImportTestPeer::batch(window), nullptr) << "the settled restore must retire the batch";
}

}  // namespace

PJ_MAIN_WINDOW_TEST_MAIN
