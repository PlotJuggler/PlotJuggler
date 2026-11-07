// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <memory>
#include <vector>

#include "MainWindow.h"
#include "pj_runtime/CatalogModel.h"
#include "support/loader_test_support.h"
#include "support/preset_test_support.h"

namespace {

using PJ::PresetTestPeer;
using pj_app_test::pumpUntil;

// Every ctest case owns one shell and an isolated, real mock DataSource DSO.
class MainWindowStartupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_.isValid());
    ASSERT_TRUE(data_.isValid());
    const QString plugin = QString::fromUtf8(PJ_MOCK_FILE_SOURCE_PLUGIN_PATH);
    ASSERT_TRUE(QFile::copy(plugin, extensions_.filePath(QFileInfo(plugin).fileName())));
    window_ = std::make_unique<PJ::MainWindow>(extensions_.path());
    QObject::connect(window_.get(), &PJ::MainWindow::layoutRestoreSettled, window_.get(), [this](bool ok) {
      settles_.push_back(ok);
    });
  }

  QString layout(const QByteArray& body) {
    const QString path = data_.filePath(QStringLiteral("startup.pj4.xml"));
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(body), body.size());
    return path;
  }

  // Destroy the window before its connected result storage and temporary files.
  QTemporaryDir extensions_;
  QTemporaryDir data_;
  std::vector<bool> settles_;
  std::unique_ptr<PJ::MainWindow> window_;
};

TEST_F(MainWindowStartupTest, WaitsForEveryExplicitFileBeforeApplyingGenericLayout) {
  const QString first = pj_app_test::makeMockFile(data_, QStringLiteral("first.mock"));
  const QString second = pj_app_test::makeMockFile(data_, QStringLiteral("second.mock"));
  const QString path = layout(
      "<root pj4_version=\"4\" binding=\"generic\"><tabbed_widget parent=\"main_window\">"
      "<Tab id=\"t1\" containers=\"1\"><Container><DockArea id=\"a1\" name=\"View\">"
      "<plot id=\"plot1\" mode=\"TimeSeries\"/></DockArea></Container></Tab></tabbed_widget></root>");
  window_->loadAtStartup({first, second}, path, /*interactive=*/false);
  EXPECT_TRUE(settles_.empty());
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_EQ(PresetTestPeer::session(*window_).catalogModel().datasets().size(), 2u);
  pj_app_test::flushQueuedEvents();
  EXPECT_EQ(settles_.size(), 1u);
}

TEST_F(MainWindowStartupTest, ExplicitFileReplacesSavedSourcesAndRebindsQualifiedCurve) {
  const QString input = pj_app_test::makeMockFile(data_, QStringLiteral("replacement.mock"));
  const QString path = layout(
      "<root pj4_version=\"4\" binding=\"source\">"
      "<tabbed_widget parent=\"main_window\"><Tab id=\"t1\" containers=\"1\"><Container>"
      "<DockArea id=\"a1\" name=\"View\"><plot id=\"plot1\" mode=\"TimeSeries\">"
      "<curve topic=\"mock/file_data\" field=\"value\" dataset_id=\"1234\" dataset_path=\"old.mock\"/>"
      "</plot></DockArea></Container></Tab></tabbed_widget>"
      "<previouslyLoaded_Datafiles><fileInfo filename=\"old.mock\" prefix=\"\"/>"
      "</previouslyLoaded_Datafiles></root>");
  window_->loadAtStartup({input}, path, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_EQ(PresetTestPeer::curveCount(*window_), 1);
  const auto& datasets = PresetTestPeer::session(*window_).catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 1u);
  EXPECT_EQ(PresetTestPeer::fileLoader(*window_).sourcePathForDataset(datasets.front().first), input);
}

TEST_F(MainWindowStartupTest, FailedExplicitFileAbortsLayoutAndSettlesFalseOnce) {
  const QString good = pj_app_test::makeMockFile(data_, QStringLiteral("good.mock"));
  const QString bad = pj_app_test::makeMockFile(data_, QStringLiteral("bad.unsupported"));
  const QString path = layout("<root pj4_version=\"4\" binding=\"generic\"/>");
  window_->loadAtStartup({bad, good}, path, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(settles_, std::vector<bool>{false});
  EXPECT_EQ(PresetTestPeer::session(*window_).catalogModel().datasets().size(), 1u);
  pj_app_test::flushQueuedEvents();
  EXPECT_EQ(settles_.size(), 1u);
}

TEST_F(MainWindowStartupTest, DataOnlyLoadsWithoutRestoringALayout) {
  const QString input = pj_app_test::makeMockFile(data_, QStringLiteral("data.mock"));
  window_->loadAtStartup({input}, {}, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !PresetTestPeer::session(*window_).catalogModel().isEmpty(); }));
  pj_app_test::flushQueuedEvents();
  EXPECT_TRUE(settles_.empty());
}

}  // namespace
