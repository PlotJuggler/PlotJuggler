// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The four fold rules a toolbox panel with work in flight obeys, plus the
// auto-fold on import start and the completion-close suppression that keeps a
// folded tab alive. Every rule is keyed on one domain-neutral predicate — "this
// panel has work in flight" — so the fixture drives it from a bool rather than
// standing up a live parser-ingest context (which needs a real parser plugin).

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QDomDocument>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWidget>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "MainWindow.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "support/gui_test_env.h"

using namespace Qt::StringLiterals;

namespace PJ {

// Reaches the private takeover/pin seams so the assertions run against the real
// routing a toolbox takes, and installs the three test seams that stand in for
// the modal confirmation and for a plugin host's work-in-flight query/cancel.
class ToolboxPanelFoldTestPeer {
 public:
  // Mirror of the private WrappedToolboxPanel, which only a friend can name.
  struct Wrapped {
    QWidget* container = nullptr;
    std::function<void()> enter_pinned_chrome;
  };

  static Wrapped wrapToolboxPanel(
      MainWindow& window, QWidget* content, const QString& title, const std::function<void()>& on_close,
      const std::function<void()>& on_migrate, std::function<bool()> has_work_in_flight,
      const std::function<void()>& on_fold_busy = {}) {
    const MainWindow::WrappedToolboxPanel wrapped =
        window.wrapToolboxPanel(content, title, on_close, on_migrate, std::move(has_work_in_flight), on_fold_busy);
    return {.container = wrapped.container, .enter_pinned_chrome = wrapped.enter_pinned_chrome};
  }

  // `interactive` maps to the two policies that reach commitRestoredLayout in
  // production: kPrompt (layout open) / kRetainAndDiagnose (batch drain, D5).
  [[nodiscard]] static bool restorePinnedToolboxes(MainWindow& window, const QDomElement& root, bool interactive) {
    return window.restorePinnedToolboxes(
        root,
        interactive ? MainWindow::MissingCurvePolicy::kPrompt : MainWindow::MissingCurvePolicy::kRetainAndDiagnose);
  }

  [[nodiscard]] static bool presentPanel(MainWindow& window, QWidget* panel) {
    return window.presentPanel(panel);
  }

  static QWidget* releaseCentralPanel(MainWindow& window) {
    return window.releaseCentralPanel();
  }

  static void dismissTakeoverPanel(MainWindow& window) {
    window.dismissTakeoverPanel();
  }

  static void setTakeoverFold(
      MainWindow& window, const void* owner, std::function<void(bool)> fold, std::function<bool()> has_work_in_flight) {
    window.setTakeoverFold(owner, std::move(fold), std::move(has_work_in_flight));
  }

  static void foldTakeoverPanelIfOwnedBy(MainWindow& window, const void* owner, bool transient) {
    window.foldTakeoverPanelIfOwnedBy(owner, transient);
  }

  static void pinToolboxPanel(
      MainWindow& window, QWidget* container, const QString& plugin_id, const QString& title,
      std::function<QString()> save_config, ToolboxRuntimeHost* host, bool transient) {
    window.pinToolboxPanel(container, plugin_id, title, /*engine=*/nullptr, std::move(save_config), host, transient);
  }

  /// Returns whether the engine may tear itself down (false = the host declined
  /// and the panel must keep exchanging widget data).
  [[nodiscard]] static bool emitPinnedCloseRequest(MainWindow& window, QWidget* container, const std::string& reason) {
    return window.onPinnedPanelCloseRequested(container, reason);
  }

  [[nodiscard]] static bool isPinned(const MainWindow& window, const QString& plugin_id) {
    return window.pinned_toolboxes_.contains(plugin_id);
  }

  [[nodiscard]] static QString savedPinnedToolboxesXml(const MainWindow& window) {
    QDomDocument doc;
    doc.appendChild(window.savePinnedToolboxes(doc));
    return doc.toString();
  }

  static void closeAllPinnedToolboxTabs(MainWindow& window) {
    window.closeAllPinnedToolboxTabs();
  }

  static void setSeams(
      MainWindow& window, std::function<bool(QString)> confirm, std::function<bool(ToolboxRuntimeHost*)> busy,
      std::function<void(ToolboxRuntimeHost*)> stop) {
    window.confirm_running_job_ = std::move(confirm);
    window.host_work_in_flight_ = std::move(busy);
    window.stop_host_work_ = std::move(stop);
  }
};

}  // namespace PJ

namespace {

// One MainWindow per binary: the shell leaks process-global widget state across
// instances, so every case shares this one and cleans up after itself.
class ToolboxPanelFoldTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    extensions_dir_ = std::make_unique<QTemporaryDir>();
    ASSERT_TRUE(extensions_dir_->isValid());
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_->path());
    window_->resize(1600, 900);
    window_->show();
  }

  static void TearDownTestSuite() {
    window_.reset();
    extensions_dir_.reset();
  }

  void SetUp() override {
    PJ::ToolboxPanelFoldTestPeer::setSeams(
        mainWindow(),
        [this](QString /*label*/) {
          confirm_shown_ = true;
          return confirm_answer_;
        },
        [this](PJ::ToolboxRuntimeHost* host) { return host == ownHost() && work_in_flight_; },
        [this](PJ::ToolboxRuntimeHost* host) { stopped_hosts_.push_back(host); });
  }

  void TearDown() override {
    // Forced, so a still-"busy" panel cannot veto the cleanup.
    PJ::ToolboxPanelFoldTestPeer::closeAllPinnedToolboxTabs(mainWindow());
    if (QWidget* released = PJ::ToolboxPanelFoldTestPeer::releaseCentralPanel(mainWindow()); released != nullptr) {
      delete released;
    }
    delete container_.data();
    PJ::ToolboxPanelFoldTestPeer::setSeams(mainWindow(), {}, {}, {});
  }

  [[nodiscard]] static PJ::MainWindow& mainWindow() {
    return *window_;
  }

  // A distinct host identity. Both host interactions are injected, so this is
  // never dereferenced — only compared.
  [[nodiscard]] PJ::ToolboxRuntimeHost* ownHost() {
    return reinterpret_cast<PJ::ToolboxRuntimeHost*>(&own_host_token_);
  }

  // The launch-session stand-in the fold is keyed on.
  [[nodiscard]] const void* ownerToken() const {
    return static_cast<const void*>(&owner_token_);
  }

  // One id per case, so entries from earlier cases in this binary cannot alias.
  [[nodiscard]] static QString pluginId() {
    return QString::fromLatin1(::testing::UnitTest::GetInstance()->current_test_info()->name());
  }

  [[nodiscard]] static PJ::TabbedPlotWidget& tabbedWidget() {
    PJ::TabbedPlotWidget* tabs = window_->findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
    Q_ASSERT(tabs != nullptr);
    return *tabs;
  }

  // Wraps a dummy panel in the real toolbox banner, recording which of the two
  // dispositions the chrome picked.
  void wrapPanel() {
    wrapped_ = PJ::ToolboxPanelFoldTestPeer::wrapToolboxPanel(
        mainWindow(), new QWidget, panelTitle(),
        /*on_close=*/[this]() { closed_ = true; },
        /*on_migrate=*/
        [this]() {
          migrated_ = true;
          foldIntoTab(/*transient=*/false);
        },
        /*has_work_in_flight=*/[this]() { return work_in_flight_; },
        /*on_fold_busy=*/
        [this]() {
          busy_folded_ = true;
          foldIntoTab(/*transient=*/true);
        });
    container_ = wrapped_.container;
  }

  [[nodiscard]] QToolButton* bannerCloseButton() const {
    return container_.isNull() ? nullptr : container_->findChild<QToolButton*>(u"buttonClose"_s);
  }

  [[nodiscard]] QWidget* panelContainer() const {
    return container_.data();
  }

  // Pins the wrapped panel as a folded tab, the state rules 3-4 start from.
  void pinPanel() {
    wrapPanel();
    wrapped_.enter_pinned_chrome();
    PJ::ToolboxPanelFoldTestPeer::pinToolboxPanel(
        mainWindow(), wrapped_.container, pluginId(), panelTitle(), saveConfig(), ownHost(), /*transient=*/false);
  }

  // Presents the wrapped panel as the chart-area takeover and registers the
  // same fold closure launchToolbox does.
  void presentAsTakeover() {
    wrapPanel();
    ASSERT_TRUE(PJ::ToolboxPanelFoldTestPeer::presentPanel(mainWindow(), wrapped_.container));
    PJ::ToolboxPanelFoldTestPeer::setTakeoverFold(
        mainWindow(), ownerToken(),
        [this](bool transient) {
          wrapped_.enter_pinned_chrome();
          migrated_ = true;
          foldIntoTab(transient);
        },
        [this]() { return work_in_flight_; });
  }

  void emitIngestStarted() {
    PJ::ToolboxPanelFoldTestPeer::foldTakeoverPanelIfOwnedBy(mainWindow(), ownerToken(), /*transient=*/true);
  }

  void dismissTakeover() {
    PJ::ToolboxPanelFoldTestPeer::dismissTakeoverPanel(mainWindow());
  }

  /// Returns whether the host let the engine tear itself down.
  bool emitCloseRequest(const std::string& reason) {
    return PJ::ToolboxPanelFoldTestPeer::emitPinnedCloseRequest(mainWindow(), container_.data(), reason);
  }

  void setWorkInFlight(bool busy) {
    work_in_flight_ = busy;
  }

  void setConfirmAnswer(bool answer) {
    confirm_answer_ = answer;
  }

  [[nodiscard]] bool closed() const {
    return closed_;
  }

  [[nodiscard]] bool migrated() const {
    return migrated_;
  }

  [[nodiscard]] bool busyFolded() const {
    return busy_folded_;
  }

  [[nodiscard]] bool confirmShown() const {
    return confirm_shown_;
  }

  // The pinned entry owns the plugin session, so its removal IS the engine
  // teardown having run.
  [[nodiscard]] static bool engineClosed() {
    return !PJ::ToolboxPanelFoldTestPeer::isPinned(mainWindow(), pluginId());
  }

  [[nodiscard]] const std::vector<PJ::ToolboxRuntimeHost*>& stoppedHosts() const {
    return stopped_hosts_;
  }

  [[nodiscard]] static bool savedLayoutMentions(const QString& plugin_id) {
    return PJ::ToolboxPanelFoldTestPeer::savedPinnedToolboxesXml(mainWindow()).contains(plugin_id);
  }

 private:
  [[nodiscard]] static QString panelTitle() {
    return u"Test Panel"_s;
  }

  void foldIntoTab(bool transient) {
    QWidget* released = PJ::ToolboxPanelFoldTestPeer::releaseCentralPanel(mainWindow());
    if (released == nullptr) {
      return;
    }
    PJ::ToolboxPanelFoldTestPeer::pinToolboxPanel(
        mainWindow(), released, pluginId(), panelTitle(), saveConfig(), ownHost(), transient);
  }

  [[nodiscard]] static std::function<QString()> saveConfig() {
    return []() { return u"{}"_s; };
  }

  inline static std::unique_ptr<QTemporaryDir> extensions_dir_;
  inline static std::unique_ptr<PJ::MainWindow> window_;

  void* own_host_token_ = nullptr;
  void* owner_token_ = nullptr;
  PJ::ToolboxPanelFoldTestPeer::Wrapped wrapped_;
  QPointer<QWidget> container_;
  bool work_in_flight_ = false;
  bool confirm_answer_ = true;
  bool confirm_shown_ = false;
  bool closed_ = false;
  bool migrated_ = false;
  bool busy_folded_ = false;
  std::vector<PJ::ToolboxRuntimeHost*> stopped_hosts_;
};

// Rule 2 — the banner X with a download running. Tearing the panel down would
// destroy the plugin instance, which is also the download's kill switch.
TEST_F(ToolboxPanelFoldTest, BannerCloseFoldsWhileWorkInFlight) {
  wrapPanel();
  setWorkInFlight(true);

  bannerCloseButton()->click();

  // The X takes the HOST-chosen fold disposition, never the migrate button's
  // user-pin path: a user who pressed close chose a pin even less than an
  // auto-fold did.
  EXPECT_TRUE(busyFolded());
  EXPECT_FALSE(migrated());
  EXPECT_FALSE(closed());
}

// Rule 1 — nothing running, so the X keeps its plain teardown.
TEST_F(ToolboxPanelFoldTest, BannerCloseTearsDownWhenIdle) {
  wrapPanel();
  setWorkInFlight(false);

  bannerCloseButton()->click();

  EXPECT_TRUE(closed());
  EXPECT_FALSE(migrated());
}

// Rule 4 — declining leaves both the tab and the transfer exactly as they were.
TEST_F(ToolboxPanelFoldTest, FoldedTabCloseDeclinedKeepsEngineAlive) {
  pinPanel();
  setWorkInFlight(true);
  setConfirmAnswer(false);

  tabbedWidget().closeWidgetTab(panelContainer());

  EXPECT_FALSE(engineClosed());
  EXPECT_TRUE(confirmShown());
  EXPECT_TRUE(stoppedHosts().empty());
}

// Rule 4, confirmed — the cancel lands on THIS panel's host only, so another
// folded panel's import keeps running.
TEST_F(ToolboxPanelFoldTest, FoldedTabCloseConfirmedStopsThatHostOnly) {
  pinPanel();
  setWorkInFlight(true);
  setConfirmAnswer(true);

  tabbedWidget().closeWidgetTab(panelContainer());

  EXPECT_TRUE(engineClosed());
  EXPECT_EQ(stoppedHosts(), std::vector<PJ::ToolboxRuntimeHost*>{ownHost()});
}

// Rule 3 — an idle folded panel closes like any other tab, unprompted.
TEST_F(ToolboxPanelFoldTest, FoldedTabCloseWhenIdleNeedsNoConfirmation) {
  pinPanel();
  setWorkInFlight(false);

  tabbedWidget().closeWidgetTab(panelContainer());

  EXPECT_TRUE(engineClosed());
  EXPECT_FALSE(confirmShown());
}

// A starting import folds its own panel unconditionally: the rows it publishes
// mid-batch have to be reachable while the batch runs.
TEST_F(ToolboxPanelFoldTest, IngestStartFoldsTheTakeoverPanel) {
  presentAsTakeover();

  emitIngestStarted();

  EXPECT_TRUE(migrated());
  EXPECT_FALSE(closed());
}

// Launching any other panel goes through dismissTakeoverPanel, which must not
// become a second silent cancel.
TEST_F(ToolboxPanelFoldTest, DismissTakeoverFoldsWhileWorkInFlight) {
  presentAsTakeover();
  setWorkInFlight(true);

  dismissTakeover();

  EXPECT_TRUE(migrated());
  EXPECT_FALSE(engineClosed());
}

// A background fold is the host uncovering the charts, not a workspace the user
// arranged, so it must not be written into a saved layout.
TEST_F(ToolboxPanelFoldTest, BackgroundFoldIsExcludedFromLayoutSave) {
  presentAsTakeover();

  emitIngestStarted();

  ASSERT_FALSE(engineClosed());  // it really is pinned; the save is what excludes it
  EXPECT_FALSE(savedLayoutMentions(pluginId()));
}

// A folded panel outlives its own batch: the tab is the user's surface now and
// stays connected so another job can be queued into it.
TEST_F(ToolboxPanelFoldTest, CompletionCloseRequestIsIgnoredWhilePinned) {
  pinPanel();

  // Declining is what keeps the engine alive: a torn-down engine stops ticking
  // and rejects the plugin, leaving the still-visible tab an inert shell.
  EXPECT_FALSE(emitCloseRequest("import_complete"));

  EXPECT_FALSE(engineClosed());
}

// Every other reason is still honored, so a plugin's own Close button works.
TEST_F(ToolboxPanelFoldTest, UserCloseRequestStillClosesPinnedPanel) {
  pinPanel();

  EXPECT_TRUE(emitCloseRequest("user_back"));

  EXPECT_TRUE(engineClosed());
}

// The X-driven fold is host-chosen chrome management, not a workspace the user
// arranged: like the ingest auto-fold, it must stay out of the saved layout.
TEST_F(ToolboxPanelFoldTest, BusyBannerCloseFoldIsExcludedFromLayoutSave) {
  wrapPanel();
  ASSERT_TRUE(PJ::ToolboxPanelFoldTestPeer::presentPanel(mainWindow(), panelContainer()));
  setWorkInFlight(true);

  bannerCloseButton()->click();

  ASSERT_FALSE(engineClosed());  // folded into a pinned tab, not torn down
  EXPECT_FALSE(savedLayoutMentions(pluginId()));
}

// The contrast: the migrate button IS the user arranging their workspace, so
// that pin persists.
TEST_F(ToolboxPanelFoldTest, MigrateButtonPinIsIncludedInLayoutSave) {
  wrapPanel();
  ASSERT_TRUE(PJ::ToolboxPanelFoldTestPeer::presentPanel(mainWindow(), panelContainer()));

  auto* migrate = panelContainer()->findChild<QAbstractButton*>(u"buttonMigrateTab"_s);
  ASSERT_NE(migrate, nullptr);
  migrate->click();

  ASSERT_FALSE(engineClosed());
  EXPECT_TRUE(savedLayoutMentions(pluginId()));
}

// D5: a non-interactive restore (a batch drain lands minutes after the layout
// open) must never raise the cancel-confirmation modal. Busy pinned panels are
// retained unprompted — same resolution as a decline — with a diagnostic.
TEST_F(ToolboxPanelFoldTest, NonInteractiveRestoreKeepsBusyPinnedPanelsUnprompted) {
  pinPanel();
  setWorkInFlight(true);

  QDomDocument doc;
  const bool applied = PJ::ToolboxPanelFoldTestPeer::restorePinnedToolboxes(
      mainWindow(), doc.createElement(u"root"_s), /*interactive=*/false);

  EXPECT_FALSE(applied);
  EXPECT_FALSE(confirmShown());
  EXPECT_FALSE(engineClosed());
  EXPECT_TRUE(stoppedHosts().empty());
}

// The interactive leg keeps the confirmation; declining leaves the live set
// (and the transfer) untouched.
TEST_F(ToolboxPanelFoldTest, InteractiveRestoreConfirmDeclineKeepsBusyPinnedPanels) {
  pinPanel();
  setWorkInFlight(true);
  setConfirmAnswer(false);

  QDomDocument doc;
  const bool applied = PJ::ToolboxPanelFoldTestPeer::restorePinnedToolboxes(
      mainWindow(), doc.createElement(u"root"_s), /*interactive=*/true);

  EXPECT_FALSE(applied);
  EXPECT_TRUE(confirmShown());
  EXPECT_FALSE(engineClosed());
  EXPECT_TRUE(stoppedHosts().empty());
}

// Confirming cancels the busy panel's own job and replaces the pinned set.
TEST_F(ToolboxPanelFoldTest, InteractiveRestoreConfirmedReplacesBusyPinnedPanels) {
  pinPanel();
  setWorkInFlight(true);
  setConfirmAnswer(true);

  QDomDocument doc;
  const bool applied = PJ::ToolboxPanelFoldTestPeer::restorePinnedToolboxes(
      mainWindow(), doc.createElement(u"root"_s), /*interactive=*/true);

  EXPECT_TRUE(applied);
  EXPECT_TRUE(confirmShown());
  EXPECT_TRUE(engineClosed());
  EXPECT_EQ(stoppedHosts(), std::vector<PJ::ToolboxRuntimeHost*>{ownHost()});
}

}  // namespace

namespace {

// Preserve this suite's settings scope without changing other runner cases.
class SettingsEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    if (!pj_app_test::isTestSuiteSelected("ToolboxPanelFoldTest")) {
      return;
    }
    QCoreApplication::setOrganizationName(u"PlotJugglerTest"_s);
    QCoreApplication::setApplicationName(u"toolbox_panel_fold_test"_s);
    QSettings().clear();
  }
};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new SettingsEnvironment);

}  // namespace
