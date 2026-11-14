// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// An interactive stream start creates its dataset before the plugin's dialog
// opens. Whatever ends the start without launching the session (the user
// cancelling, a plugin contract violation, a refused configuration, a failed
// start) must take that dataset back out of both engines and the catalog, so a
// later start of the same source is named as the first one would be.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <memory>

#include "StreamingSourceManager.h"
#include "pj_datastore/engine.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "support/loader_test_support.h"

#ifndef PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH
#error "PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH must be defined"
#endif

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr auto kPluginName = "Mock Layout Stream";
constexpr auto kDialogConfigEnv = "PJ_LAYOUT_STREAM_DIALOG_CONFIG";
constexpr auto kNullDialogEnv = "PJ_LAYOUT_STREAM_NULL_DIALOG";

class StreamingDialogExitTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(settings_dir_.isValid());
    // Settings of this process only: every pj_app_gui_tests process on the machine
    // (parallel ctest, other checkouts) clears the shared test file when it starts.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    const QString plugin = QString::fromUtf8(PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH);
    ASSERT_TRUE(QFile::copy(plugin, extensions_dir_.filePath(QFileInfo(plugin).fileName())));

    app_session_ = std::make_unique<AppSession>(extensions_dir_.path());
    manager_ = std::make_unique<StreamingSourceManager>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), app_session_->catalogModel(),
        app_session_->topicDemandTracker(), /*dialog_parent=*/nullptr);
    manager_->onSourceChanged(QString::fromLatin1(kPluginName));
    QObject::connect(manager_.get(), &StreamingSourceManager::streamStarted, [this](DatasetId) { ++started_; });
    QObject::connect(manager_.get(), &StreamingSourceManager::setupError, [this](const QString& message) {
      setup_errors_.push_back(message);
    });
    QObject::connect(manager_.get(), &StreamingSourceManager::streamError, [this](DatasetId, const QString& message) {
      stream_errors_.push_back(message);
    });
  }

  void TearDown() override {
    manager_->stopAllAndWait(QString{});
    manager_.reset();
    app_session_.reset();
    qunsetenv(kDialogConfigEnv);
    qunsetenv(kNullDialogEnv);
  }

  // Settles the next plugin dialog the manager opens, once it is on screen. Gives
  // up after 10 s, so a case that never opens one leaves no timer polling.
  static void settleNextDialog(bool accept) {
    auto* poll = new QTimer(qApp);
    poll->setInterval(5);
    QTimer::singleShot(10'000, poll, &QObject::deleteLater);
    QObject::connect(poll, &QTimer::timeout, poll, [poll, accept] {
      for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* dialog = qobject_cast<QDialog*>(widget);
        if (dialog != nullptr && dialog->isModal() && dialog->isVisible()) {
          poll->deleteLater();
          accept ? dialog->accept() : dialog->reject();
          return;
        }
      }
    });
    poll->start();
  }

  // Presses Start and waits until the start has settled: it does not wait for its dialog itself.
  void startAndSettle() {
    manager_->onStartRequested();
    ASSERT_TRUE(pj_app_test::pumpUntil([this] { return !manager_->hasPendingStart(); }));
  }

  // Both engines, the catalog and the manager hold nothing of the abandoned start.
  void expectNothingLeftBehind() {
    EXPECT_TRUE(app_session_->sessionManager().dataEngine().listDatasets().empty()) << "primary engine";
    EXPECT_TRUE(manager_->secondaryEngineForTests().listDatasets().empty()) << "secondary engine";
    EXPECT_TRUE(app_session_->catalogModel().datasets().empty()) << "catalog";
    EXPECT_FALSE(manager_->hasActiveSession());
    EXPECT_EQ(started_, 0);
  }

  // Starts the source again, accepts its dialog and waits for its first sample:
  // the dataset must carry the plain stream name, with no ordinal for an
  // abandoned predecessor.
  void expectRestartKeepsTheBaseName() {
    qunsetenv(kDialogConfigEnv);
    qunsetenv(kNullDialogEnv);
    settleNextDialog(/*accept=*/true);
    startAndSettle();
    ASSERT_EQ(started_, 1);
    const CatalogModel& catalog = app_session_->catalogModel();
    ASSERT_TRUE(pj_app_test::pumpUntil([&] { return !catalog.datasets().empty(); }));
    ASSERT_EQ(catalog.datasets().size(), 1U);
    EXPECT_EQ(
        catalog.datasets().front().second.toStdString(),
        u"[stream] %1"_s.arg(QString::fromLatin1(kPluginName)).toStdString());
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir settings_dir_;
  std::unique_ptr<AppSession> app_session_;
  std::unique_ptr<StreamingSourceManager> manager_;
  int started_ = 0;
  QStringList setup_errors_;
  QStringList stream_errors_;
};

// A browser cannot block on a dialog, so no platform does: the start returns while its dialog is
// open, and launches when the user accepts.
TEST_F(StreamingDialogExitTest, TheStartDoesNotWaitForItsDialog) {
  manager_->onStartRequested();
  ASSERT_TRUE(manager_->hasPendingStart());
  EXPECT_EQ(started_, 0);

  settleNextDialog(/*accept=*/true);
  ASSERT_TRUE(pj_app_test::pumpUntil([this] { return started_ == 1; }));
  EXPECT_FALSE(manager_->hasPendingStart());
  EXPECT_TRUE(manager_->hasActiveSession());
}

// Whatever erases datasets or destroys sessions first withdraws a start still at its dialog.
TEST_F(StreamingDialogExitTest, StoppingEverythingWithdrawsAStartAtItsDialog) {
  manager_->onStartRequested();
  ASSERT_TRUE(manager_->hasPendingStart());

  manager_->stopAllAndWait(QString{});
  EXPECT_FALSE(manager_->hasPendingStart());
  expectNothingLeftBehind();
  EXPECT_TRUE(setup_errors_.isEmpty());
  EXPECT_TRUE(stream_errors_.isEmpty());
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    EXPECT_FALSE(widget->isVisible() && qobject_cast<QDialog*>(widget) != nullptr) << "the dialog outlived its start";
  }
}

// Erasing the dataset of a start still at its dialog withdraws that start: accepting the dialog
// later must not launch into a dataset that is gone.
TEST_F(StreamingDialogExitTest, StoppingItsDatasetWithdrawsAStartAtItsDialog) {
  manager_->onStartRequested();
  ASSERT_TRUE(manager_->hasPendingStart());
  const std::vector<DatasetId> datasets = app_session_->sessionManager().dataEngine().listDatasets();
  ASSERT_EQ(datasets.size(), 1U);

  EXPECT_FALSE(manager_->stopDatasetAndWait(datasets.front(), QString{})) << "nothing was streaming yet";
  EXPECT_FALSE(manager_->hasPendingStart());
  expectNothingLeftBehind();
}

TEST_F(StreamingDialogExitTest, CancellingTheDialogLeavesNoDataset) {
  settleNextDialog(/*accept=*/false);
  startAndSettle();

  expectNothingLeftBehind();
  EXPECT_TRUE(setup_errors_.isEmpty()) << "a cancel is not an error";
  EXPECT_TRUE(stream_errors_.isEmpty()) << "a cancel is not an error";
  expectRestartKeepsTheBaseName();
}

TEST_F(StreamingDialogExitTest, APluginContractViolationLeavesNoDataset) {
  qputenv(kNullDialogEnv, "1");
  startAndSettle();

  expectNothingLeftBehind();
  EXPECT_TRUE(setup_errors_.isEmpty());
  ASSERT_EQ(stream_errors_.size(), 1);
  EXPECT_TRUE(stream_errors_.front().contains(u"Plugin contract violation"_s)) << stream_errors_.front().toStdString();
  expectRestartKeepsTheBaseName();
}

TEST_F(StreamingDialogExitTest, ARefusedConfigurationLeavesNoDataset) {
  qputenv(kDialogConfigEnv, R"({"reject":true})");
  settleNextDialog(/*accept=*/true);
  startAndSettle();

  expectNothingLeftBehind();
  EXPECT_TRUE(setup_errors_.isEmpty());
  ASSERT_EQ(stream_errors_.size(), 1);
  EXPECT_TRUE(stream_errors_.front().contains(u"loadConfig (post-dialog) failed"_s))
      << stream_errors_.front().toStdString();
  expectRestartKeepsTheBaseName();
}

TEST_F(StreamingDialogExitTest, AFailedStartLeavesNoDataset) {
  qputenv(kDialogConfigEnv, R"({"fail_start":true})");
  settleNextDialog(/*accept=*/true);
  startAndSettle();

  expectNothingLeftBehind();
  EXPECT_TRUE(setup_errors_.isEmpty());
  ASSERT_EQ(stream_errors_.size(), 1);
  EXPECT_TRUE(stream_errors_.front().contains(u"start failed"_s)) << stream_errors_.front().toStdString();
  expectRestartKeepsTheBaseName();
}

}  // namespace
}  // namespace PJ
