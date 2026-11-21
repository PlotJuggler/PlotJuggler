// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Several files loaded together ask once, before loading, how the loaders' configuration dialogs behave for the
// batch (BatchLoadDialog): show all, show the first of each format, or skip with the last-used settings. Only when a
// loader with a dialog gets two or more of the files.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <memory>
#include <optional>

#include "FileLoader.h"
#include "MainWindow.h"
#include "pj_widgets/DualOptionsWidget.h"
#include "support/loader_test_support.h"

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowBatchLoadTestPeer {
 public:
  [[nodiscard]] static FileLoader& fileLoader(MainWindow& window) {
    return *window.file_loader_;
  }
};

}  // namespace PJ

namespace {

using Peer = PJ::MainWindowBatchLoadTestPeer;
using PJ::BatchDialogMode;

// The probe plugin (.dlgprobe) has a configuration dialog and saves a config once it is accepted; its imports are
// recorded with the settings they ran with (imports()). The mock one (.mock) has no dialog.
class BatchLoadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_.isValid());
    ASSERT_TRUE(data_.isValid());
    qputenv("PJ_DIALOG_PROBE_IMPORT_FILE", data_.filePath(u"imports.log"_s).toUtf8());
    for (const QString& plugin :
         {QString::fromUtf8(PJ_DIALOG_PROBE_SOURCE_PLUGIN_PATH), QString::fromUtf8(PJ_MOCK_FILE_SOURCE_PLUGIN_PATH)}) {
      ASSERT_TRUE(QFile::copy(plugin, extensions_.filePath(QFileInfo(plugin).fileName())));
    }
    window_ = std::make_unique<PJ::MainWindow>(extensions_.path());
    QObject::connect(
        &Peer::fileLoader(*window_), &PJ::FileLoader::loadFinished, window_.get(),
        [this](quint64, PJ::LoadOutcome outcome) { outcomes_.push_back(outcome); });
    auto* timer = new QTimer(window_.get());
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, window_.get(), [this]() { answerDialog(); });
    timer->start();
  }

  QStringList newFiles(int count, bool with_dialog = true) {
    QStringList paths;
    for (int index = 0; index < count; ++index) {
      const QString name = u"run%1_%2"_s.arg(++file_serial_).arg(index);
      paths.push_back(
          with_dialog ? data_.filePath(name + u".dlgprobe"_s) : pj_app_test::makeMockFile(data_, name + u".mock"_s));
      if (with_dialog) {
        QFile file(paths.back());
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
      }
    }
    return paths;
  }

  // Loads `paths` as one batch and waits until every file has ended.
  void loadBatch(const QStringList& paths) {
    const std::size_t before = outcomes_.size();
    Peer::fileLoader(*window_).loadFiles(paths, window_.get());
    const std::size_t expected =
        batch_mode_.has_value() || paths.size() < 2 ? before + static_cast<std::size_t>(paths.size()) : before;
    ASSERT_TRUE(pj_app_test::pumpUntil([&] { return outcomes_.size() >= expected; }));
    pj_app_test::flushQueuedEvents();
  }

  // The settings each import ran with, in import order.
  [[nodiscard]] QStringList imports() const {
    QFile file(data_.filePath(u"imports.log"_s));
    // Text mode: the probe's std::ofstream writes \r\n line ends on Windows.
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      return {};
    }
    return QString::fromUtf8(file.readAll()).split(u'\n', Qt::SkipEmptyParts);
  }

  [[nodiscard]] int count(PJ::LoadOutcome outcome) const {
    return static_cast<int>(std::count(outcomes_.begin(), outcomes_.end(), outcome));
  }

  // The batch question takes batch_mode_ (Cancel when empty); a plugin dialog gets the next of dialog_names_ typed in
  // (when any is left) and is accepted, or rejected when reject_plugin_dialogs_. Answers are queued: the dialog runs a
  // nested loop, inside which this timer still fires.
  void answerDialog() {
    QDialog* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (dialog == nullptr) {
      for (QDialog* candidate : window_->findChildren<QDialog*>()) {
        if (candidate->isVisible()) {
          dialog = candidate;
        }
      }
    }
    if (dialog == nullptr || dialog->property("pjTestSeen").isValid()) {
      return;
    }
    dialog->setProperty("pjTestSeen", true);
    if (auto* mode = dialog->findChild<PJ::DualOptionsWidget*>(u"dialogMode"_s); mode != nullptr) {
      ++batch_questions_;
      if (!batch_mode_.has_value()) {
        QMetaObject::invokeMethod(dialog, &QDialog::reject, Qt::QueuedConnection);
        return;
      }
      mode->setSelectedIndex(static_cast<int>(*batch_mode_));
      QMetaObject::invokeMethod(dialog, &QDialog::accept, Qt::QueuedConnection);
      return;
    }
    ++plugin_dialogs_;
    if (reject_plugin_dialogs_) {
      QMetaObject::invokeMethod(dialog, &QDialog::reject, Qt::QueuedConnection);
      return;
    }
    if (!dialog_names_.isEmpty()) {
      dialog->findChild<QLineEdit*>(u"name_input"_s)->setText(dialog_names_.takeFirst());
    }
    QPushButton* ok = dialog->findChild<QDialogButtonBox*>(u"buttonBox"_s)->button(QDialogButtonBox::Ok);
    QTimer::singleShot(0, ok, [ok]() { ok->click(); });
  }

  QTemporaryDir extensions_;
  QTemporaryDir data_;
  std::unique_ptr<PJ::MainWindow> window_;
  std::vector<PJ::LoadOutcome> outcomes_;
  std::optional<BatchDialogMode> batch_mode_ = BatchDialogMode::kShowFirst;
  bool reject_plugin_dialogs_ = false;
  QStringList dialog_names_;
  int batch_questions_ = 0;
  int plugin_dialogs_ = 0;
  int file_serial_ = 0;
};

TEST_F(BatchLoadTest, OneFileAsksNoBatchQuestion) {
  ASSERT_NO_FATAL_FAILURE(loadBatch(newFiles(1)));
  EXPECT_EQ(batch_questions_, 0);
  EXPECT_EQ(plugin_dialogs_, 1);
  EXPECT_EQ(count(PJ::LoadOutcome::kLoaded), 1);
}

TEST_F(BatchLoadTest, NoQuestionWhenNoDialogWouldRepeat) {
  ASSERT_NO_FATAL_FAILURE(loadBatch(newFiles(3, /*with_dialog=*/false)));
  EXPECT_EQ(batch_questions_, 0) << "no loader has a dialog";
  QStringList mixed = newFiles(1);
  mixed += newFiles(1, /*with_dialog=*/false);
  ASSERT_NO_FATAL_FAILURE(loadBatch(mixed));
  EXPECT_EQ(batch_questions_, 0) << "the one loader with a dialog gets one file";
  EXPECT_EQ(plugin_dialogs_, 1);
  EXPECT_EQ(count(PJ::LoadOutcome::kLoaded), 5);
}

TEST_F(BatchLoadTest, ShowFirstAsksOnlyTheFirstFileOfTheFormat) {
  ASSERT_NO_FATAL_FAILURE(loadBatch(newFiles(3)));
  EXPECT_EQ(batch_questions_, 1);
  EXPECT_EQ(plugin_dialogs_, 1);
  EXPECT_EQ(count(PJ::LoadOutcome::kLoaded), 3);

  // Settings saved by the previous batch do not count: this batch's first file still asks.
  ASSERT_NO_FATAL_FAILURE(loadBatch(newFiles(2)));
  EXPECT_EQ(plugin_dialogs_, 2);
  EXPECT_EQ(count(PJ::LoadOutcome::kLoaded), 5);
}

TEST_F(BatchLoadTest, ShowFirstLoadsEveryFileWithTheSettingsItsFirstDialogAccepted) {
  dialog_names_ = {u"first"_s};
  ASSERT_NO_FATAL_FAILURE(loadBatch(newFiles(3)));
  EXPECT_EQ(plugin_dialogs_, 1);
  EXPECT_EQ(imports(), (QStringList{u"first"_s, u"first"_s, u"first"_s}));
}

// A first dialog that does not end with saved settings (here the plugin refuses what was accepted) leaves "show
// first" asking: the next file must not load with settings an earlier session saved.
TEST_F(BatchLoadTest, AFirstDialogThatFailsLeavesShowFirstAsking) {
  dialog_names_ = {u"stale"_s};
  ASSERT_NO_FATAL_FAILURE(loadBatch(newFiles(1)));
  ASSERT_EQ(imports(), QStringList{u"stale"_s});

  dialog_names_ = {u"reject"_s, u"fresh"_s};
  ASSERT_NO_FATAL_FAILURE(loadBatch(newFiles(3)));
  EXPECT_EQ(plugin_dialogs_, 3) << "the failed dialog, then the next file's";
  EXPECT_EQ(count(PJ::LoadOutcome::kFailed), 1);
  EXPECT_EQ(imports(), (QStringList{u"stale"_s, u"fresh"_s, u"fresh"_s}));
}

TEST_F(BatchLoadTest, ShowAllAsksEveryFile) {
  batch_mode_ = BatchDialogMode::kShowAll;
  ASSERT_NO_FATAL_FAILURE(loadBatch(newFiles(3)));
  EXPECT_EQ(plugin_dialogs_, 3);
  EXPECT_EQ(count(PJ::LoadOutcome::kLoaded), 3);
}

TEST_F(BatchLoadTest, SkipAsksOnlyAFormatNeverConfigured) {
  batch_mode_ = BatchDialogMode::kSkip;
  ASSERT_NO_FATAL_FAILURE(loadBatch(newFiles(3)));
  EXPECT_EQ(plugin_dialogs_, 1) << "no saved settings yet: the first file asks, the others reuse what it saved";
  EXPECT_EQ(count(PJ::LoadOutcome::kLoaded), 3);

  ASSERT_NO_FATAL_FAILURE(loadBatch(newFiles(2)));
  EXPECT_EQ(plugin_dialogs_, 1) << "settings saved: no dialog at all";
  EXPECT_EQ(count(PJ::LoadOutcome::kLoaded), 5);
}

TEST_F(BatchLoadTest, CancellingAFilesDialogCancelsTheRestOfItsFormat) {
  batch_mode_ = BatchDialogMode::kShowAll;
  reject_plugin_dialogs_ = true;
  ASSERT_NO_FATAL_FAILURE(loadBatch(newFiles(3)));
  EXPECT_EQ(plugin_dialogs_, 1);
  EXPECT_EQ(count(PJ::LoadOutcome::kCancelled), 3);
}

TEST_F(BatchLoadTest, CancellingTheBatchQuestionLoadsNothing) {
  batch_mode_.reset();
  ASSERT_NO_FATAL_FAILURE(loadBatch(newFiles(3)));
  EXPECT_EQ(batch_questions_, 1);
  EXPECT_FALSE(Peer::fileLoader(*window_).isBusy()) << "a file was queued after Cancel";
  EXPECT_EQ(plugin_dialogs_, 0);
  EXPECT_TRUE(outcomes_.empty());
}

}  // namespace
