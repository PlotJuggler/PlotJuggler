// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Integration test: installs can-bus-parser via ExtensionManager using the real registry.json.
//
// Requires network access. Not intended for CI — run manually to verify the full pipeline:
//   registry parse → ExtensionManager::install() → download → checksum → extract → register
//
// Results are written to tests/results/extensions/can-bus-parser/ (defined at build time
// via RESULTS_DIR compile definition).

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QSignalSpy>
#include <QString>
#include <QUrl>

#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension.hpp"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/registry_manager.hpp"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

bool waitForSignal(QSignalSpy& spy, int timeout_ms = 5000) {
  QDeadlineTimer deadline(timeout_ms);
  while (spy.isEmpty() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return !spy.isEmpty();
}

// ---------------------------------------------------------------------------
// Integration test — real network, real registry, real ExtensionManager
// ---------------------------------------------------------------------------

TEST(ExtensionManagerIntegrationTest, InstallCanBusParserUsingRegistry) {
  // ---------------------------------------------------------------------------
  // 1. Parse the local registry.json via RegistryManager (file:// URL)
  // ---------------------------------------------------------------------------
  RegistryManager registry;
  QSignalSpy registry_finished(&registry, &RegistryManager::fetchFinished);
  QSignalSpy registry_error(&registry, &RegistryManager::fetchError);

  // Defaults to the official registry; PJ_TEST_REGISTRY_URL points this manual
  // integration test at a local file:// copy or a staging registry instead.
  const QByteArray registry_override = qgetenv("PJ_TEST_REGISTRY_URL");
  registry.fetchRegistry(QUrl(
      registry_override.isEmpty()
          ? QStringLiteral("https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/main/registry.json")
          : QString::fromUtf8(registry_override)));

  ASSERT_TRUE(waitForSignal(registry_finished, 5000)) << "RegistryManager did not finish parsing";
  ASSERT_TRUE(registry_finished.first().at(0).toBool())
      << "Registry parse failed: "
      << (registry_error.isEmpty() ? "" : registry_error.first().at(0).toString().toStdString());

  // ---------------------------------------------------------------------------
  // 2. Look up can-bus-parser
  // ---------------------------------------------------------------------------
  const Extension ext = registry.findById(u"can-bus-parser"_s);
  ASSERT_FALSE(ext.id.isEmpty()) << "can-bus-parser not found in registry.json";

  // ---------------------------------------------------------------------------
  // 3. Prepare destination directories
  // ---------------------------------------------------------------------------
  const QString ext_dir = QStringLiteral(RESULTS_DIR) + "/extensions";
  const QString pending_dir = QStringLiteral(RESULTS_DIR) + "/.extension_staging";
  ASSERT_TRUE(QDir().mkpath(ext_dir)) << "Could not create extensions directory: " << ext_dir.toStdString();
  ASSERT_TRUE(QDir().mkpath(pending_dir)) << "Could not create pending directory: " << pending_dir.toStdString();

  // ---------------------------------------------------------------------------
  // 4. Install via ExtensionManager
  // ---------------------------------------------------------------------------
  DownloadManager dm;
  ExtensionManager mgr(&dm, ext_dir, pending_dir);

  QSignalSpy spy_started(&mgr, &ExtensionManager::installStarted);
  QSignalSpy spy_finished(&mgr, &ExtensionManager::installFinished);
  QSignalSpy spy_error(&mgr, &ExtensionManager::installError);
  QSignalSpy spy_progress(&mgr, &ExtensionManager::installProgress);

  mgr.install(ext);

  // installStarted must fire synchronously before the download begins.
  ASSERT_EQ(spy_started.count(), 1);
  EXPECT_EQ(spy_started.first().at(0).toString(), "can-bus-parser");

  // Real network download — allow up to 60 seconds.
  ASSERT_TRUE(waitForSignal(spy_finished, 60000)) << "installFinished not received within 60s — check network and URL: "
                                                  << ext.platforms.value(u"linux-x86_64"_s).url.toStdString();

  EXPECT_TRUE(spy_finished.first().at(1).toBool())
      << "Install failed: " << (spy_error.isEmpty() ? "" : spy_error.first().at(1).toString().toStdString());

  EXPECT_TRUE(spy_error.isEmpty()) << "Unexpected installError: "
                                   << (spy_error.isEmpty() ? "" : spy_error.first().at(1).toString().toStdString());

  EXPECT_GE(spy_progress.count(), 1) << "No installProgress signals emitted during download";

  // ---------------------------------------------------------------------------
  // 5. Verify final state
  // ---------------------------------------------------------------------------
  EXPECT_TRUE(mgr.isInstalled("can-bus-parser"));
  EXPECT_EQ(mgr.installedExtensions()["can-bus-parser"].version, ext.version);
  EXPECT_TRUE(QDir(ext_dir + "/can-bus-parser").exists());

  qDebug() << "Installed can-bus-parser to:" << ext_dir + "/can-bus-parser";
}

}  // namespace
}  // namespace PJ

// ---------------------------------------------------------------------------
// main — QCoreApplication is required for the Qt network event loop
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
