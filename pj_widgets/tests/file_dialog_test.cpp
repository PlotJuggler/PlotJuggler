// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <chrono>
#include <thread>

#include "pj_widgets/FileDialog.h"
using namespace Qt::StringLiterals;

namespace {

class FileDialogTest : public ::testing::Test {};

// The dialog is modal — we close it from a single-shot timer so the test
// doesn't hang. We verify the embedded extras checkbox state is reported
// back to the caller, which is the whole reason the overload exists.
TEST_F(FileDialogTest, GetSaveFileNameWithOptionsReportsCheckboxState) {
  PJ::FileDialog::ExtraOption opt{u"Save Data Source"_s, /*default_checked=*/true};
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  const QString save_path = QDir(temp.path()).filePath(u"_pj4_filedialog_test.pjl4"_s);

  // Fire after the modal dialog is up; find the checkbox by its label and
  // toggle it off, then accept by hitting the Save button.
  QTimer::singleShot(0, [save_path] {
    QWidget* active = QApplication::activeModalWidget();
    ASSERT_NE(active, nullptr);
    auto* checkbox = active->findChild<QCheckBox*>();
    ASSERT_NE(checkbox, nullptr);
    EXPECT_TRUE(checkbox->isChecked());
    checkbox->setChecked(false);
    auto* file_dialog = active->findChild<QFileDialog*>();
    ASSERT_NE(file_dialog, nullptr);
    file_dialog->selectFile(save_path);
    QMetaObject::invokeMethod(file_dialog, "accept", Qt::QueuedConnection);
  });

  const auto result = PJ::FileDialog::getSaveFileNameWithOptions(
      /*parent=*/nullptr, u"Save"_s, temp.path(), u"PJ4 Layout (*.pjl4)"_s, u"pjl4"_s, {opt});

  EXPECT_FALSE(result.path.isEmpty());
  ASSERT_EQ(result.option_states.size(), 1u);
  EXPECT_FALSE(result.option_states[0]);  // toggled off in the timer
}

TEST_F(FileDialogTest, GetSaveFileNameWithOptionsCancelReturnsEmpty) {
  PJ::FileDialog::ExtraOption opt{u"Save Data Source"_s, true};
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  QTimer::singleShot(0, [] {
    QWidget* active = QApplication::activeModalWidget();
    ASSERT_NE(active, nullptr);
    auto* file_dialog = active->findChild<QFileDialog*>();
    ASSERT_NE(file_dialog, nullptr);
    QMetaObject::invokeMethod(file_dialog, "reject", Qt::QueuedConnection);
  });

  const auto result = PJ::FileDialog::getSaveFileNameWithOptions(
      nullptr, u"Save"_s, temp.path(), u"PJ4 Layout (*.pjl4)"_s, u"pjl4"_s, {opt});

  EXPECT_TRUE(result.path.isEmpty());
  ASSERT_EQ(result.option_states.size(), 1u);
  EXPECT_TRUE(result.option_states[0]);  // defaults preserved on cancel
}

// selected_name is filled in inside `dir`, the suggestion a plugin's structured
// picker asks for. Compared canonically: macOS temp dirs sit behind a
// /var -> /private/var symlink.
TEST_F(FileDialogTest, GetSaveFileNameFillsInSelectedName) {
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  QString shown;
  QTimer::singleShot(0, [&shown] {
    QWidget* active = QApplication::activeModalWidget();
    ASSERT_NE(active, nullptr);
    auto* file_dialog = active->findChild<QFileDialog*>();
    ASSERT_NE(file_dialog, nullptr);
    shown = file_dialog->selectedFiles().value(0);
    QMetaObject::invokeMethod(file_dialog, "reject", Qt::QueuedConnection);
  });

  const QString path = PJ::FileDialog::getSaveFileName(
      nullptr, u"Save"_s, temp.path(), u"HTML (*.html)"_s, u"html"_s, u"FT0017-AV.html"_s);

  EXPECT_TRUE(path.isEmpty());
  const QFileInfo info(shown);
  EXPECT_EQ(info.dir().canonicalPath(), QDir(temp.path()).canonicalPath());
  EXPECT_EQ(info.fileName(), u"FT0017-AV.html"_s);
}

// Start-folder guard. Qt keeps its own file-dialog memory under QtProject; each
// case points file-based settings at a private root. On Windows that memory is
// in the registry, which QSettings::setPath does not redirect: there the cases
// write the user's real QtProject key, as any QFileDialog use does.
class FileDialogStartFolderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(settings_root_.isValid());
    default_settings_root_ = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settings_root_.path());
  }

  void TearDown() override {
    PJ::FileDialog::setStartFolderProbeForTest({});
    PJ::FileDialog::setStartFolderTimeoutForTest(-1);
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, default_settings_root_);
  }

  // Opens a modal open dialog on `dir`, runs `inspect` on its inner QFileDialog
  // and start-folder notice once it is up, then rejects it.
  template <typename Inspect>
  static void openAndInspect(const QString& dir, Inspect inspect) {
    QTimer::singleShot(0, [inspect] {
      QWidget* active = QApplication::activeModalWidget();
      ASSERT_NE(active, nullptr);
      auto* file_dialog = active->findChild<QFileDialog*>();
      ASSERT_NE(file_dialog, nullptr);
      auto* notice = active->findChild<QLabel*>(u"startFolderNotice"_s);
      ASSERT_NE(notice, nullptr);
      inspect(file_dialog, notice);
      QMetaObject::invokeMethod(file_dialog, "reject", Qt::QueuedConnection);
    });
    PJ::FileDialog::getOpenFileName(nullptr, u"Open"_s, dir, u"All (*)"_s);
  }

  QTemporaryDir settings_root_;
  QString default_settings_root_;
};

TEST_F(FileDialogStartFolderTest, ReachableFolderIsUsedAndNoticeHidden) {
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  openAndInspect(temp.path(), [&temp](QFileDialog* file_dialog, QLabel* notice) {
    EXPECT_EQ(
        QFileInfo(file_dialog->directory().absolutePath()).canonicalFilePath(),
        QFileInfo(temp.path()).canonicalFilePath());
    EXPECT_TRUE(notice->isHidden());
  });
}

TEST_F(FileDialogStartFolderTest, SlowFolderFallsBackToHomeWithoutBlocking) {
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  PJ::FileDialog::setStartFolderProbeForTest([](const QString&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    return true;
  });
  PJ::FileDialog::setStartFolderTimeoutForTest(100);

  QElapsedTimer elapsed;
  elapsed.start();
  qint64 elapsed_at_open_ms = -1;
  openAndInspect(temp.path(), [&](QFileDialog* file_dialog, QLabel* notice) {
    elapsed_at_open_ms = elapsed.elapsed();
    EXPECT_EQ(file_dialog->directory().absolutePath(), QDir(QDir::homePath()).absolutePath());
    EXPECT_FALSE(notice->isHidden());
    EXPECT_TRUE(notice->text().contains(QDir::toNativeSeparators(temp.path())));
  });
  EXPECT_GE(elapsed_at_open_ms, 0);
  EXPECT_LT(elapsed_at_open_ms, 800);
}

TEST_F(FileDialogStartFolderTest, MissingFolderThatAnswersFastKeepsNoticeHidden) {
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  const QString missing = QDir(temp.path()).filePath(u"does_not_exist"_s);
  openAndInspect(missing, [](QFileDialog*, QLabel* notice) { EXPECT_TRUE(notice->isHidden()); });
}

TEST_F(FileDialogStartFolderTest, PersistedQtMemoryDropsLastVisitedAndDeadShortcuts) {
  QTemporaryDir alive;
  ASSERT_TRUE(alive.isValid());
  const QUrl alive_url = QUrl::fromLocalFile(alive.path());
  const QUrl dead_url = QUrl::fromLocalFile(u"/pj4_unreachable_share"_s);
  {
    QSettings qt_settings(QSettings::UserScope, u"QtProject"_s);
    qt_settings.setValue(u"FileDialog/lastVisited"_s, dead_url.toString());
    qt_settings.setValue(u"FileDialog/viewMode"_s, u"Detail"_s);
    qt_settings.setValue(u"FileDialog/history"_s, QStringList{alive_url.toString()});
    qt_settings.setValue(u"FileDialog/shortcuts"_s, QStringList{u"file:"_s, alive_url.toString(), dead_url.toString()});
  }
  // Only the dead share hangs; everything else answers at once.
  PJ::FileDialog::setStartFolderProbeForTest([](const QString& path) {
    if (path.contains(u"unreachable"_s)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    return true;
  });
  PJ::FileDialog::setStartFolderTimeoutForTest(100);

  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  // Qt rewrites the group when the dialog is destroyed, so look right after
  // construction.
  openAndInspect(temp.path(), [&](QFileDialog* file_dialog, QLabel* notice) {
    QSettings qt_settings(QSettings::UserScope, u"QtProject"_s);
    EXPECT_FALSE(qt_settings.childGroups().contains(u"FileDialog"_s));
    EXPECT_EQ(file_dialog->viewMode(), QFileDialog::Detail);
    EXPECT_TRUE(file_dialog->history().contains(alive.path()));  // Qt appends the folder it opened
    const QList<QUrl> shortcuts = file_dialog->sidebarUrls();
    EXPECT_TRUE(shortcuts.contains(alive_url));
    EXPECT_FALSE(shortcuts.contains(dead_url));
    EXPECT_TRUE(notice->isHidden());  // the start folder itself answered
  });
}

}  // namespace
