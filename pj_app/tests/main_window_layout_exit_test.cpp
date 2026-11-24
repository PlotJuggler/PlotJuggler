// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// LayoutExitWatch, the --exit-after-layout exit-code decision: exactly one decision per watch, mapped from the
// settlement boundary (load failure -> kExitCodeLoadFailed, committed restore -> kExitCodeSuccess, no settlement
// within the deadline -> kExitCodeTimeout) and latched, so a timer firing after a settlement, or vice versa, does not
// re-decide. One MainWindow per binary; the cases share it in declaration order, and the timeout case is LAST: it
// leaves its import batch parked so suite teardown proves the close-time hard shutdown stays clean.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDeadlineTimer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <chrono>
#include <memory>
#include <vector>

#include "LayoutExitWatch.h"
#include "dataset_test_helpers.h"
#include "pj_runtime/CatalogModel.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

namespace {

using PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::pumpUntil;
namespace fake = pj_layout_import_gui;
using Peer = MainWindowLayoutImportTestPeer;

// Spins the loop for a fixed wall-clock window, to prove something does not happen (a suppressed late timer or
// settlement).
void pumpFor(int ms) {
  const QDeadlineTimer deadline(ms);
  while (!deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
}

class MainWindowLayoutExitTest : public ::testing::Test {
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
    // The timeout case left a parked import batch on purpose: this reset is the teardown under test (MainWindow's
    // dtor hard-shuts the running batch, children cancelled and joined) and must not crash.
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

// A failed load decides kExitCodeLoadFailed exactly once; the armed timeout expiring afterwards must not re-decide.
TEST_F(MainWindowLayoutExitTest, LoadFailureDecidesLoadFailedOnceAndSuppressesTheLateTimeout) {
  std::vector<int> codes;
  PJ::LayoutExitWatch watch(window(), std::chrono::milliseconds(300), [&codes](int code) { codes.push_back(code); });
  ASSERT_TRUE(codes.empty()) << "no decision before the load";

  Peer::loadLayout(window(), project_dir_->filePath(QStringLiteral("missing.pj4.xml")), /*interactive=*/false);

  ASSERT_EQ(codes.size(), 1u) << "the failure settlement must decide synchronously";
  EXPECT_EQ(codes.front(), PJ::LayoutExitWatch::kExitCodeLoadFailed);

  pumpFor(400);  // past the 300 ms deadline: the stopped timer must stay quiet
  EXPECT_EQ(codes.size(), 1u) << "a decision is final — the late timeout must not re-fire";
}

TEST_F(MainWindowLayoutExitTest, CommittedRestoreDecidesSuccess) {
  const PJ::DatasetId dataset = pj_test::createDataset(appSession(), "plain.mcap", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0u);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), dataset, "/plain", 1'000, 2'000), 0u);
  appSession().catalogModel().rebuildFromDatastore();
  const QString layout_path = project_dir_->filePath(QStringLiteral("plain.pj4.xml"));
  // The plain sync leg is enough (the settle suite covers the batch leg): only the settlement -> exit-code mapping
  // is under test.
  ASSERT_TRUE(fake::writeLayoutFile(layout_path, fake::buildGenericCurveLayoutDoc(QStringLiteral("/plain"))));

  std::vector<int> codes;
  PJ::LayoutExitWatch watch(window(), std::chrono::milliseconds(5000), [&codes](int code) { codes.push_back(code); });

  Peer::loadLayout(window(), layout_path, /*interactive=*/false);

  ASSERT_TRUE(pumpUntil([&codes]() { return !codes.empty(); }));
  ASSERT_EQ(codes.size(), 1u);
  EXPECT_EQ(codes.front(), PJ::LayoutExitWatch::kExitCodeSuccess);
}

// The watch hears only the load it was built before: another load's settle (here one that fails to open while the
// watched restore still waits for its data, and so supersedes nothing) decides nothing.
TEST_F(MainWindowLayoutExitTest, AnotherLoadsSettleDecidesNothing) {
  const QString source_path = project_dir_->filePath(QStringLiteral("data/watched.mcap"));
  const QString layout_path = project_dir_->filePath(QStringLiteral("watched.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          layout_path, QStringLiteral("/watched"), source_path,
          fake::makeDescriptor(QStringLiteral("trusted"), source_path, QStringLiteral("block"))));

  std::vector<int> codes;
  PJ::LayoutExitWatch watch(window(), std::chrono::milliseconds(10'000), [&codes](int code) { codes.push_back(code); });
  Peer::loadLayout(window(), layout_path, /*interactive=*/false);
  ASSERT_TRUE(Peer::progressiveInFlight(window()));
  Peer::loadLayout(window(), project_dir_->filePath(QStringLiteral("other-missing.pj4.xml")), /*interactive=*/false);
  EXPECT_TRUE(codes.empty()) << "another load's failure decided the exit code";
  ASSERT_TRUE(Peer::progressiveInFlight(window())) << "the failed open superseded the watched restore";

  const PJ::DatasetId dataset = pj_test::createDataset(appSession(), "watched.mcap", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0u);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), dataset, "/watched", 1'000, 2'000), 0u);
  appSession().catalogModel().rebuildFromDatastore();
  ASSERT_NE(fake::g_instance, nullptr);
  fake::g_instance->releaseStart();
  ASSERT_TRUE(pumpUntil([&codes]() { return !codes.empty(); }));
  EXPECT_EQ(codes, std::vector<int>{PJ::LayoutExitWatch::kExitCodeSuccess});
}

// A never-settling restore (parked import job) decides kExitCodeTimeout after the deadline; the batch is still active
// then, and left for the suite teardown to hard-cancel.
TEST_F(MainWindowLayoutExitTest, NeverSettlingRestoreDecidesTimeoutWhileTheBatchIsStillActive) {
  const QString source_path = project_dir_->filePath(QStringLiteral("data/parked.mcap"));
  const QString layout_path = project_dir_->filePath(QStringLiteral("parked.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          layout_path, QStringLiteral("/parked"), source_path,
          fake::makeDescriptor(QStringLiteral("trusted"), source_path, QStringLiteral("block"))));

  std::vector<int> codes;
  PJ::LayoutExitWatch watch(window(), std::chrono::milliseconds(1000), [&codes](int code) { codes.push_back(code); });

  Peer::loadLayout(window(), layout_path, /*interactive=*/false);
  ASSERT_TRUE(Peer::progressiveInFlight(window()));
  ASSERT_TRUE(Peer::batchActive(window()));
  EXPECT_TRUE(codes.empty()) << "no decision while the restore is still draining";

  ASSERT_TRUE(pumpUntil([&codes]() { return !codes.empty(); }));
  ASSERT_EQ(codes.size(), 1u);
  EXPECT_EQ(codes.front(), PJ::LayoutExitWatch::kExitCodeTimeout);
  EXPECT_TRUE(Peer::batchActive(window())) << "the batch never settled — timeout fired around it";
  EXPECT_TRUE(Peer::progressiveInFlight(window()));
  // Deliberately not released: TearDownTestSuite's window reset must hard-cancel the parked batch without crashing.
}

}  // namespace

PJ_MAIN_WINDOW_TEST_MAIN
