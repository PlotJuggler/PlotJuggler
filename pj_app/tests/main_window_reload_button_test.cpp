// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A reload disables the left panel's Reload button until its request resolves. The button must come
// back on every way the request can end, including the exits the legacy fileLoaded / fileLoadFailed
// signals stay silent on: a rejected config dialog, a queued cancel.

#include <gtest/gtest.h>

#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QPushButton>
#include <QTemporaryDir>
#include <memory>

#include "FileLoader.h"
#include "MainWindow.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/SessionManager.h"
#include "support/loader_test_support.h"

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowReloadButtonTestPeer {
 public:
  [[nodiscard]] static SessionManager& sessionManager(MainWindow& window) {
    return window.session_->sessionManager();
  }

  [[nodiscard]] static FileLoader& fileLoader(MainWindow& window) {
    return *window.file_loader_;
  }
};

}  // namespace PJ

namespace {

using Peer = PJ::MainWindowReloadButtonTestPeer;
using pj_app_test::flushQueuedEvents;
using pj_app_test::pumpUntil;

// The test plugin has a config dialog, so a reload with no recorded config opens it.
class ReloadButtonTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_.isValid());
    ASSERT_TRUE(data_.isValid());
    const QString plugin = QString::fromUtf8(PJ_DIALOG_PROBE_SOURCE_PLUGIN_PATH);
    ASSERT_TRUE(QFile::copy(plugin, extensions_.filePath(QFileInfo(plugin).fileName())));
    window_ = std::make_unique<PJ::MainWindow>(extensions_.path());
  }

  [[nodiscard]] QPushButton* reloadButton() const {
    return window_->findChild<QPushButton*>(u"buttonReloadData"_s);
  }

  [[nodiscard]] PJ::FileLoader& loader() const {
    return Peer::fileLoader(*window_);
  }

  [[nodiscard]] QString newFile(const QString& name) const {
    const QString path = data_.filePath(name);
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    return path;
  }

  [[nodiscard]] QDialog* visibleDialog() const {
    for (QDialog* dialog : window_->findChildren<QDialog*>()) {
      if (dialog->isVisible()) {
        return dialog;
      }
    }
    return nullptr;
  }

  // Deliver every terminal the loader has scheduled.
  void settle() const {
    ASSERT_TRUE(pumpUntil([this] { return !loader().isBusy(); }));
    flushQueuedEvents();
  }

  // Loads `path` without prompting, then forgets the recorded plugin config: the state of a source whose
  // plugin saved none, so the next reload takes the interactive dialog path.
  void loadAndForgetItsConfig(const QString& path) {
    ASSERT_NE(reloadButton(), nullptr);
    ASSERT_FALSE(reloadButton()->isEnabled()) << "nothing to reload yet";

    PJ::LoadHints hints;
    hints.expected_plugin_id = u"Dialog Probe Source"_s;
    hints.dialog_policy = PJ::DialogPolicy::kNever;
    hints.require_expected_plugin = true;
    ASSERT_TRUE(loader().loadFile(path, window_.get(), hints));
    ASSERT_TRUE(pumpUntil([this] { return reloadButton()->isEnabled(); })) << "a loaded file must be reloadable";
    ASSERT_NO_FATAL_FAILURE(settle());

    PJ::SessionManager& session = Peer::sessionManager(*window_);
    const auto source = session.lastLoadedSource();
    ASSERT_TRUE(source.has_value());
    session.recordLoadedSource(source->path, source->prefix, source->plugin_id, QString(), source->plugin_manifest_id);
  }

  QTemporaryDir extensions_;
  QTemporaryDir data_;
  std::unique_ptr<PJ::MainWindow> window_;
};

TEST_F(ReloadButtonTest, ComesBackWhenTheReloadsConfigDialogIsRejected) {
  ASSERT_NO_FATAL_FAILURE(loadAndForgetItsConfig(newFile(u"run.dlgprobe"_s)));

  reloadButton()->click();
  EXPECT_FALSE(reloadButton()->isEnabled()) << "a reload in flight must not be re-entered";
  ASSERT_TRUE(pumpUntil([this] { return visibleDialog() != nullptr; }, 5000)) << "the config dialog never opened";

  visibleDialog()->reject();
  ASSERT_NO_FATAL_FAILURE(settle());

  EXPECT_TRUE(reloadButton()->isEnabled()) << "a rejected dialog ends the reload without fileLoaded or fileLoadFailed";
}

TEST_F(ReloadButtonTest, ComesBackWhenAQueuedReloadIsCancelled) {
  ASSERT_NO_FATAL_FAILURE(loadAndForgetItsConfig(newFile(u"run.dlgprobe"_s)));

  // A second load parks on its own dialog; the reload queues behind it.
  const quint64 blocker = loader().loadFileTicketed(
      PJ::LoadInput::fromNativePath(newFile(u"blocker.dlgprobe"_s)), window_.get(), PJ::LoadHints{});
  ASSERT_NE(blocker, 0U);
  ASSERT_TRUE(pumpUntil([this] { return visibleDialog() != nullptr; }, 5000)) << "the blocking dialog never opened";

  reloadButton()->click();
  EXPECT_FALSE(reloadButton()->isEnabled());

  // Tickets are consecutive: the reload is the request right after the blocker.
  ASSERT_TRUE(loader().cancelLoad(blocker + 1));
  flushQueuedEvents();

  EXPECT_TRUE(reloadButton()->isEnabled()) << "a cancelled queued reload ends without fileLoaded or fileLoadFailed";

  visibleDialog()->reject();
  ASSERT_NO_FATAL_FAILURE(settle());
}

// A source the loader refuses outright never gets a ticket, so no loadFinished follows it.
TEST_F(ReloadButtonTest, ComesBackWhenTheReloadIsRejectedOutright) {
  ASSERT_NO_FATAL_FAILURE(loadAndForgetItsConfig(newFile(u"run.dlgprobe"_s)));
  const auto source = Peer::sessionManager(*window_).lastLoadedSource();
  ASSERT_TRUE(source.has_value());
  // "/" has no file name, which the loader refuses before queueing anything.
  Peer::sessionManager(*window_).recordLoadedSource(
      u"/"_s, source->prefix, source->plugin_id, source->plugin_config_json, source->plugin_manifest_id);

  reloadButton()->click();

  EXPECT_TRUE(reloadButton()->isEnabled()) << "a rejected request has no loadFinished to bring the button back";
}

}  // namespace
