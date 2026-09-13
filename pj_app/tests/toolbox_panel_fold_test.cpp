// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The four fold rules a toolbox panel with work in flight obeys, plus the
// auto-fold on import start and the completion-close suppression that keeps a
// folded tab alive. Every rule is keyed on one domain-neutral predicate — "this
// panel has work in flight" — so the fixture drives it from a bool rather than
// standing up a live parser-ingest context (which needs a real parser plugin).

#include <gtest/gtest.h>
#include <pj_widgets/Dialog.h>

#include <QAbstractButton>
#include <QApplication>
#include <QBoxLayout>
#include <QByteArray>
#include <QDomDocument>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>
#include <memory>
#include <pj_plugins/host_qt/widget_binding.hpp>
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
    std::function<void()> enter_docked_chrome;
  };

  // Positional hooks, packed into MainWindow::ToolboxChromeHooks for the call.
  static Wrapped wrapToolboxPanel(
      MainWindow& window, QWidget* content, const QString& title, std::function<void()> on_close,
      std::function<void()> on_migrate, std::function<void()> on_float = {},
      std::function<bool()> has_work_in_flight = {}, std::function<void()> on_fold_busy = {},
      const QString& persist_key = {}, std::function<void()> on_dock = {}) {
    const MainWindow::WrappedToolboxPanel wrapped = window.wrapToolboxPanel(
        content, title,
        MainWindow::ToolboxChromeHooks{
            .on_close = std::move(on_close),
            .on_migrate = std::move(on_migrate),
            .on_float = std::move(on_float),
            .on_dock = std::move(on_dock),
            .on_fold_busy = std::move(on_fold_busy),
            .has_work_in_flight = std::move(has_work_in_flight)},
        persist_key);
    return {
        .container = wrapped.container,
        .enter_pinned_chrome = wrapped.enter_pinned_chrome,
        .enter_docked_chrome = wrapped.enter_docked_chrome};
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

  static QWidget* releaseToolboxPanel(MainWindow& window, const QString& id, QWidget* container, QString& title) {
    return window.releaseToolboxPanel(id, container, title);
  }

  static bool dockToolboxPanel(
      MainWindow& window, QWidget* container, const void* owner, std::function<void(bool)> fold,
      std::function<bool()> busy, const std::function<void()>& chrome) {
    return window.dockToolboxPanel(container, nullptr, owner, std::move(fold), std::move(busy), chrome);
  }

  static QWidget* currentPanel(const MainWindow& window) {
    return window.current_panel_;
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

  // Registers a floating-toolbox entry directly (the production insert lives
  // inside launchToolbox's migrate_to_float, which needs a live plugin
  // session); `floating_window` stands in for the floating PJ::Dialog and is
  // deleted by closeAllFloatingToolboxWindows.
  static void insertFloatingToolbox(
      MainWindow& window, const QString& plugin_id, QWidget* floating_window, std::function<QString()> save_config,
      ToolboxRuntimeHost* host, const QString& label) {
    window.floating_toolboxes_.insert(
        plugin_id, MainWindow::FloatingToolbox{
                       .window = floating_window,
                       .container = floating_window,
                       .engine = nullptr,
                       .save_config = std::move(save_config),
                       .host = host,
                       .label = label,
                       .on_close = {}});
  }

  [[nodiscard]] static bool isFloating(const MainWindow& window, const QString& plugin_id) {
    return window.floating_toolboxes_.contains(plugin_id);
  }

  static void closeAllFloatingToolboxWindows(MainWindow& window) {
    window.closeAllFloatingToolboxWindows();
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

// A `QSplitter::saveState()` blob for a two-pane horizontal splitter with the
// given pane widths and a 1-px handle — the exact shape makeSideDrawerSplitter
// builds — so a test can plant a persisted width without going through a real
// drag. Sized so the scratch splitter's total width matches `first + second +
// 1` (the handle), which is what a caller must also give the real splitter
// for restoreState to reproduce these sizes verbatim rather than rescaling.
QByteArray twoPaneSplitterStateBlob(int first, int second) {
  QSplitter scratch(Qt::Horizontal);
  scratch.setHandleWidth(1);
  scratch.addWidget(new QWidget(&scratch));
  scratch.addWidget(new QWidget(&scratch));
  scratch.resize(first + second + scratch.handleWidth(), 400);
  scratch.setSizes({first, second});
  return scratch.saveState();
}

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
    PJ::ToolboxPanelFoldTestPeer::closeAllFloatingToolboxWindows(mainWindow());
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

  struct DrawerContent {
    QWidget* content = nullptr;
    QWidget* drawer = nullptr;
  };

  // A content root with one tagged pjToolboxSideDrawer child — the shape
  // every drawer-splitter test starts from. `minimum_width` <= 0 leaves the
  // drawer's minimum at Qt's default (the hoist test, which never resizes).
  [[nodiscard]] static DrawerContent makeDrawerContent(int minimum_width = 0) {
    auto* content = new QWidget;
    auto* content_layout = new QVBoxLayout(content);
    auto* drawer = new QWidget(content);
    drawer->setObjectName(u"conversationsDrawer"_s);
    drawer->setProperty("pjToolboxSideDrawer", true);
    if (minimum_width > 0) {
      drawer->setMinimumWidth(minimum_width);
    }
    content_layout->addWidget(drawer);
    return {.content = content, .drawer = drawer};
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
        /*on_float=*/[this]() { floated_ = true; },
        /*has_work_in_flight=*/[this]() { return work_in_flight_; },
        /*on_fold_busy=*/
        [this]() {
          busy_folded_ = true;
          foldIntoTab(/*transient=*/true);
        },
        /*persist_key=*/{},
        /*on_dock=*/[this]() { dockPanel(); });
    container_ = wrapped_.container;
  }

  void dockPanel() {
    QString title;
    QWidget* released =
        PJ::ToolboxPanelFoldTestPeer::releaseToolboxPanel(mainWindow(), pluginId(), panelContainer(), title);
    ASSERT_EQ(released, panelContainer());
    ASSERT_TRUE(
        PJ::ToolboxPanelFoldTestPeer::dockToolboxPanel(
            mainWindow(), released, ownerToken(),
            [this](bool transient) {
              wrapped_.enter_pinned_chrome();
              migrated_ = true;
              foldIntoTab(transient);
            },
            [this]() { return work_in_flight_; }, wrapped_.enter_docked_chrome));
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

  [[nodiscard]] bool floated() const {
    return floated_;
  }

  void enterPinnedChrome() {
    wrapped_.enter_pinned_chrome();
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
  bool floated_ = false;
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

// The float button is takeover chrome with an offer behind it: with no
// on_float handler (the marketplace panel, whose migrate path has no plugin
// engine) it is not shown at all.
TEST_F(ToolboxPanelFoldTest, FloatButtonHiddenWithoutFloatHandler) {
  const auto wrapped = PJ::ToolboxPanelFoldTestPeer::wrapToolboxPanel(
      mainWindow(), new QWidget, u"No Float"_s, /*on_close=*/{}, /*on_migrate=*/{}, /*on_float=*/{});

  auto* float_button = wrapped.container->findChild<QAbstractButton*>(u"buttonMigrateFloat"_s);
  ASSERT_NE(float_button, nullptr);
  EXPECT_TRUE(float_button->isHidden());
  delete wrapped.container;
}

// A widget tagged pjToolboxSideDrawer leaves the content for a splitter pane
// beside the banner+content body — outer's only child is that splitter — and
// the content root records it as hoisted so the binding still finds it.
TEST_F(ToolboxPanelFoldTest, TaggedSideDrawerIsHoistedBesideTheBanner) {
  const auto [content, drawer] = makeDrawerContent();

  const auto wrapped = PJ::ToolboxPanelFoldTestPeer::wrapToolboxPanel(
      mainWindow(), content, u"Drawer"_s, /*on_close=*/{}, /*on_migrate=*/{}, /*on_float=*/{});

  auto* outer = qobject_cast<QBoxLayout*>(wrapped.container->layout());
  ASSERT_NE(outer, nullptr);
  ASSERT_EQ(outer->count(), 1) << "the splitter is outer's sole child when a drawer is in play";
  auto* splitter = qobject_cast<QSplitter*>(outer->itemAt(0)->widget());
  ASSERT_NE(splitter, nullptr);
  EXPECT_FALSE(splitter->childrenCollapsible());
  ASSERT_EQ(splitter->count(), 2);
  EXPECT_EQ(splitter->widget(0), drawer) << "the drawer leads the splitter";
  EXPECT_FALSE(drawer->isAncestorOf(wrapped.container->findChild<QWidget*>(u"toolboxBanner"_s)));
  EXPECT_FALSE(content->isAncestorOf(drawer)) << "it is no longer under the content root";

  const QVariantList hoisted = content->property(PJ::kHoistedWidgetsProperty).toList();
  ASSERT_EQ(hoisted.size(), 1);
  EXPECT_EQ(hoisted.front().value<QObject*>(), drawer) << "the binding's by-name lookups reach it through this";
  delete wrapped.container;
}

// childrenCollapsible(false) plus the drawer's own (plugin-authored) minimum
// stop a drag from shrinking it away, with no minimum hardcoded in the host.
TEST_F(ToolboxPanelFoldTest, SideDrawerCannotBeDraggedBelowItsMinimumWidth) {
  const auto [content, drawer] = makeDrawerContent(80);

  const auto wrapped = PJ::ToolboxPanelFoldTestPeer::wrapToolboxPanel(
      mainWindow(), content, u"Drawer"_s, /*on_close=*/{}, /*on_migrate=*/{}, /*on_float=*/{});
  auto* outer = qobject_cast<QBoxLayout*>(wrapped.container->layout());
  ASSERT_NE(outer, nullptr);
  wrapped.container->resize(500, 400);
  outer->activate();

  auto* splitter = qobject_cast<QSplitter*>(outer->itemAt(0)->widget());
  ASSERT_NE(splitter, nullptr);
  splitter->setSizes({0, 500});  // attempt to drag the drawer away entirely
  EXPECT_GE(splitter->sizes().front(), 80) << "Qt must clamp to the drawer's own minimumWidth";
  delete wrapped.container;
}

// A saved width is restored once the splitter reaches the total width the
// blob was captured at, but only when the blob still parses and matches the
// splitter's pane count — a stale/mismatched one (an old plugin build, a
// hand-edited settings file) is silently ignored and the splitter keeps its
// natural split instead.
TEST_F(ToolboxPanelFoldTest, SavedDrawerWidthIsRestoredWhenItFits) {
  const QString key = u"ToolboxDrawerWidth/%1"_s.arg(pluginId());
  QSettings().setValue(key, twoPaneSplitterStateBlob(150, 449));  // 150 + 449 + 1px handle = 600

  const auto [content, drawer] = makeDrawerContent(80);

  const auto wrapped = PJ::ToolboxPanelFoldTestPeer::wrapToolboxPanel(
      mainWindow(), content, u"Drawer"_s, /*on_close=*/{}, /*on_migrate=*/{}, /*on_float=*/{},
      /*has_work_in_flight=*/{}, /*on_fold_busy=*/{}, /*persist_key=*/pluginId());
  auto* outer = qobject_cast<QBoxLayout*>(wrapped.container->layout());
  ASSERT_NE(outer, nullptr);
  // restoreState() runs at construction, before the container has a real
  // size — show it so it reaches the 600px width the blob was captured at,
  // the same way it will in production once presentPanel() (or a
  // floating/pinned tab) puts it on screen.
  wrapped.container->resize(600, 400);
  wrapped.container->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(wrapped.container));

  auto* splitter = qobject_cast<QSplitter*>(outer->itemAt(0)->widget());
  ASSERT_NE(splitter, nullptr);
  EXPECT_EQ(splitter->sizes().front(), 150);
  delete wrapped.container;
  QSettings().remove(key);
}

// The other half of the round trip, and the one that was silently broken: a
// width the user drags has to reach QSettings. It is saved from
// QSplitter::splitterMoved -- an earlier version persisted from a
// QEvent::Destroy filter installed on the splitter itself, which never fires
// reliably for the object being destroyed, so every resize was lost on exit.
TEST_F(ToolboxPanelFoldTest, DraggingTheDrawerPersistsItsWidth) {
  const QString key = u"ToolboxDrawerWidth/%1"_s.arg(pluginId());
  QSettings().remove(key);

  const auto [content, drawer] = makeDrawerContent(80);

  const auto wrapped = PJ::ToolboxPanelFoldTestPeer::wrapToolboxPanel(
      mainWindow(), content, u"Drawer"_s, /*on_close=*/{}, /*on_migrate=*/{}, /*on_float=*/{},
      /*has_work_in_flight=*/{}, /*on_fold_busy=*/{}, /*persist_key=*/pluginId());
  auto* outer = qobject_cast<QBoxLayout*>(wrapped.container->layout());
  ASSERT_NE(outer, nullptr);
  wrapped.container->resize(600, 400);
  wrapped.container->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(wrapped.container));

  auto* splitter = qobject_cast<QSplitter*>(outer->itemAt(0)->widget());
  ASSERT_NE(splitter, nullptr);
  EXPECT_FALSE(QSettings().contains(key)) << "nothing is written until the user actually moves the handle";

  // What a drag ends with: new sizes, then the signal the splitter emits. The
  // write is debounced (splitterMoved fires per mouse-move), so the value only
  // lands once the handle has come to rest.
  splitter->setSizes({220, 380});
  emit splitter->splitterMoved(220, 1);
  EXPECT_FALSE(QSettings().contains(key)) << "an in-flight drag must not write on every step";
  QTRY_VERIFY(QSettings().contains(key));
  EXPECT_FALSE(QSettings().value(key).toByteArray().isEmpty()) << "a real QSplitter::saveState() blob, not a bare int";

  delete wrapped.container;
  QSettings().remove(key);
}

TEST_F(ToolboxPanelFoldTest, AbsurdSavedDrawerWidthIsIgnored) {
  const QString key = u"ToolboxDrawerWidth/%1"_s.arg(pluginId());
  QSettings().setValue(
      key, QByteArrayLiteral("not a splitter state blob"));  // corrupt/mismatched, restoreState rejects it

  const auto [content, drawer] = makeDrawerContent(80);

  const auto wrapped = PJ::ToolboxPanelFoldTestPeer::wrapToolboxPanel(
      mainWindow(), content, u"Drawer"_s, /*on_close=*/{}, /*on_migrate=*/{}, /*on_float=*/{},
      /*has_work_in_flight=*/{}, /*on_fold_busy=*/{}, /*persist_key=*/pluginId());
  auto* outer = qobject_cast<QBoxLayout*>(wrapped.container->layout());
  ASSERT_NE(outer, nullptr);
  wrapped.container->resize(600, 400);
  wrapped.container->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(wrapped.container));

  auto* splitter = qobject_cast<QSplitter*>(outer->itemAt(0)->widget());
  ASSERT_NE(splitter, nullptr);
  EXPECT_LT(splitter->sizes().front(), 600) << "a corrupt blob must not crash or be applied";
  delete wrapped.container;
  QSettings().remove(key);
}

// The gap the review flagged: the tab presentation persists a width under
// ToolboxDrawerWidth/<key>; PJ::Dialog::setSideWidget — the floating
// presentation's mechanism — must open at that same width when given the
// same key, since both dock through makeSideDrawerSplitter.
TEST_F(ToolboxPanelFoldTest, TabSavedWidthCarriesToFloatingPresentation) {
  const QString key = u"ToolboxDrawerWidth/%1"_s.arg(pluginId());
  QSettings().remove(key);

  // Tab presentation: drag the drawer to a width and let the debounced save land.
  const auto [tab_content, tab_drawer] = makeDrawerContent(80);
  const auto wrapped = PJ::ToolboxPanelFoldTestPeer::wrapToolboxPanel(
      mainWindow(), tab_content, u"Drawer"_s, /*on_close=*/{}, /*on_migrate=*/{}, /*on_float=*/{},
      /*has_work_in_flight=*/{}, /*on_fold_busy=*/{}, /*persist_key=*/pluginId());
  auto* tab_outer = qobject_cast<QBoxLayout*>(wrapped.container->layout());
  ASSERT_NE(tab_outer, nullptr);
  wrapped.container->resize(600, 400);
  wrapped.container->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(wrapped.container));
  auto* tab_splitter = qobject_cast<QSplitter*>(tab_outer->itemAt(0)->widget());
  ASSERT_NE(tab_splitter, nullptr);
  tab_splitter->setSizes({220, 380});
  emit tab_splitter->splitterMoved(220, 1);
  QTRY_VERIFY(QSettings().contains(key));
  delete wrapped.container;

  // Floating presentation: a fresh Dialog docking a drawer under the SAME key
  // (rootLayout's 1px margins on each side put its splitter at the same 600px
  // width the tab used, so restoreState reproduces the size verbatim).
  PJ::Dialog dialog;
  auto* float_drawer = new QWidget;
  float_drawer->setMinimumWidth(80);
  dialog.setSideWidget(float_drawer, key);
  dialog.resize(602, 400);
  dialog.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&dialog));

  auto* float_splitter = dialog.findChild<QSplitter*>();
  ASSERT_NE(float_splitter, nullptr);
  EXPECT_EQ(float_splitter->sizes().front(), 220);

  QSettings().remove(key);
}

// PJ::Dialog::setSideWidget's own round trip — the mechanism the toolbox
// tab<->floating-window migration relies on (Dialog.cpp mirrors
// wrapToolboxPanel's splitter). Docking then undocking hands the drawer back
// alive and intact, and dialogBody is restored straight into rootLayout
// cleanly, exactly as if setSideWidget had never been called.
TEST_F(ToolboxPanelFoldTest, DialogSideWidgetRoundTripRestoresOriginalLayout) {
  PJ::Dialog dialog;
  auto* content_child = new QLabel(u"content"_s, dialog.contentWidget());
  dialog.contentLayout()->addWidget(content_child);

  auto* drawer = new QWidget;
  drawer->setObjectName(u"drawer"_s);
  dialog.setSideWidget(drawer);
  EXPECT_EQ(dialog.sideWidget(), drawer);
  EXPECT_TRUE(dialog.isAncestorOf(drawer)) << "reparented under the dialog's own splitter";

  dialog.setSideWidget(nullptr);
  EXPECT_EQ(dialog.sideWidget(), nullptr);
  EXPECT_EQ(drawer->parentWidget(), nullptr) << "handed back intact, not deleted";
  // dialogBody went back into rootLayout cleanly — the content is still live.
  EXPECT_TRUE(dialog.isAncestorOf(content_child));
  EXPECT_EQ(content_child->text(), u"content"_s);

  delete drawer;
}

// A chrome action with chromeActionSlot="leading" gets its banner proxy BEFORE
// the title, and the title centres itself; without the slot the proxy stays
// after the title as before.
TEST_F(ToolboxPanelFoldTest, LeadingChromeActionProxyPrecedesTheTitle) {
  auto* content = new QWidget;
  auto* content_layout = new QVBoxLayout(content);
  auto* menu = new QPushButton(content);
  menu->setObjectName(u"menuButton"_s);
  menu->setProperty("pjToolboxChromeAction", true);
  menu->setProperty("chromeActionSlot", u"leading"_s);
  content_layout->addWidget(menu);
  auto* help = new QPushButton(content);
  help->setObjectName(u"helpButton"_s);
  help->setProperty("pjToolboxChromeAction", true);
  content_layout->addWidget(help);

  const auto wrapped = PJ::ToolboxPanelFoldTestPeer::wrapToolboxPanel(
      mainWindow(), content, u"Leading"_s, /*on_close=*/{}, /*on_migrate=*/{}, /*on_float=*/{});

  auto* banner = wrapped.container->findChild<QWidget*>(u"toolboxBanner"_s);
  ASSERT_NE(banner, nullptr);
  auto* row = qobject_cast<QBoxLayout*>(banner->layout());
  ASSERT_NE(row, nullptr);
  auto* title = banner->findChild<QLabel*>(u"toolboxBannerTitle"_s);
  ASSERT_NE(title, nullptr);
  const int title_at = row->indexOf(title);
  ASSERT_GT(title_at, 0);
  EXPECT_NE(row->itemAt(0)->widget(), nullptr) << "a proxy button leads the row";
  EXPECT_EQ(row->itemAt(0)->widget()->toolTip(), menu->toolTip());
  EXPECT_EQ(title->alignment() & Qt::AlignHCenter, Qt::AlignHCenter);
  EXPECT_TRUE(menu->isHidden()) << "the original stays in the content, hidden";
  EXPECT_TRUE(help->isHidden());
  // Two proxies in total, one on each side of the title.
  int before = 0;
  int after = 0;
  for (int i = 0; i < row->count(); ++i) {
    auto* w = qobject_cast<QAbstractButton*>(row->itemAt(i)->widget());
    if (w == nullptr || w->objectName().startsWith(u"buttonMigrate"_s) || w->objectName() == u"buttonClose"_s) {
      continue;
    }
    (i < title_at ? before : after) += 1;
  }
  EXPECT_EQ(before, 1);
  EXPECT_EQ(after, 1);
  delete wrapped.container;
}

// Clicking float runs the handler and hides the whole banner — the floating
// window's own title bar is the one header. Docking back as a tab
// (enter_pinned_chrome) re-shows the banner in pinned chrome.
TEST_F(ToolboxPanelFoldTest, FloatButtonHidesBannerAndPinnedChromeRestoresIt) {
  wrapPanel();
  ASSERT_TRUE(PJ::ToolboxPanelFoldTestPeer::presentPanel(mainWindow(), panelContainer()));

  auto* banner = panelContainer()->findChild<QWidget*>(u"toolboxBanner"_s);
  ASSERT_NE(banner, nullptr);
  auto* float_button = panelContainer()->findChild<QAbstractButton*>(u"buttonMigrateFloat"_s);
  ASSERT_NE(float_button, nullptr);
  EXPECT_FALSE(float_button->isHidden());
  float_button->click();

  EXPECT_TRUE(floated());
  EXPECT_TRUE(banner->isHidden());

  enterPinnedChrome();
  EXPECT_FALSE(banner->isHidden());
  EXPECT_FALSE(float_button->isHidden());
  auto* migrate = panelContainer()->findChild<QAbstractButton*>(u"buttonMigrateTab"_s);
  ASSERT_NE(migrate, nullptr);
  EXPECT_TRUE(migrate->isHidden());
  EXPECT_FALSE(bannerCloseButton()->isHidden());
}

TEST_F(ToolboxPanelFoldTest, HeaderDestinationsFollowDockedAndTabbedPresentation) {
  pinPanel();
  auto* dock = panelContainer()->findChild<QAbstractButton*>(u"buttonMigrateDock"_s);
  auto* tab = panelContainer()->findChild<QAbstractButton*>(u"buttonMigrateTab"_s);
  auto* floating = panelContainer()->findChild<QAbstractButton*>(u"buttonMigrateFloat"_s);
  ASSERT_NE(dock, nullptr);
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(floating, nullptr);
  EXPECT_FALSE(dock->isHidden());
  EXPECT_TRUE(tab->isHidden());
  EXPECT_FALSE(floating->isHidden());
  EXPECT_FALSE(bannerCloseButton()->isHidden());

  QPointer<QWidget> live = panelContainer();
  dock->click();
  EXPECT_FALSE(PJ::ToolboxPanelFoldTestPeer::isPinned(mainWindow(), pluginId()));
  EXPECT_EQ(PJ::ToolboxPanelFoldTestPeer::currentPanel(mainWindow()), live.data());
  EXPECT_TRUE(dock->isHidden());
  EXPECT_FALSE(tab->isHidden());
  EXPECT_FALSE(floating->isHidden());
  EXPECT_FALSE(bannerCloseButton()->isHidden());
  EXPECT_FALSE(closed());

  tab->click();
  EXPECT_TRUE(PJ::ToolboxPanelFoldTestPeer::isPinned(mainWindow(), pluginId()));
  EXPECT_EQ(panelContainer(), live.data());
  EXPECT_FALSE(dock->isHidden());
  EXPECT_TRUE(tab->isHidden());
}

TEST_F(ToolboxPanelFoldTest, DockingRestoresAutomaticFoldForTheSameLivePanel) {
  pinPanel();
  setWorkInFlight(true);
  dockPanel();
  QPointer<QWidget> live = panelContainer();
  emitIngestStarted();
  EXPECT_TRUE(migrated());
  EXPECT_TRUE(PJ::ToolboxPanelFoldTestPeer::isPinned(mainWindow(), pluginId()));
  EXPECT_EQ(panelContainer(), live.data());
  EXPECT_FALSE(closed());
  EXPECT_TRUE(stoppedHosts().empty());
}

TEST_F(ToolboxPanelFoldTest, DockingRestoresBusyCloseFoldInsteadOfCancelling) {
  pinPanel();
  setWorkInFlight(true);
  dockPanel();
  bannerCloseButton()->click();
  EXPECT_TRUE(busyFolded());
  EXPECT_TRUE(PJ::ToolboxPanelFoldTestPeer::isPinned(mainWindow(), pluginId()));
  EXPECT_FALSE(confirmShown());
  EXPECT_TRUE(stoppedHosts().empty());
}

TEST_F(ToolboxPanelFoldTest, PinnedHeaderCloseUsesTabBusyConfirmation) {
  pinPanel();
  setWorkInFlight(true);
  setConfirmAnswer(false);
  bannerCloseButton()->click();
  EXPECT_TRUE(confirmShown());
  EXPECT_FALSE(engineClosed());
  EXPECT_FALSE(busyFolded());
  setConfirmAnswer(true);
  bannerCloseButton()->click();
  EXPECT_TRUE(engineClosed());
  EXPECT_EQ(stoppedHosts(), std::vector<PJ::ToolboxRuntimeHost*>{ownHost()});
}

TEST_F(ToolboxPanelFoldTest, ReleaseToolboxPreservesRenameAndDoesNotStealAnotherTakeover) {
  pinPanel();
  tabbedWidget().setWidgetTabName(panelContainer(), u"Renamed"_s);
  auto* other = new QWidget;
  ASSERT_TRUE(PJ::ToolboxPanelFoldTestPeer::presentPanel(mainWindow(), other));
  QString title;
  QWidget* released =
      PJ::ToolboxPanelFoldTestPeer::releaseToolboxPanel(mainWindow(), pluginId(), panelContainer(), title);
  EXPECT_EQ(released, panelContainer());
  EXPECT_EQ(title, u"Renamed"_s);
  EXPECT_EQ(PJ::ToolboxPanelFoldTestPeer::currentPanel(mainWindow()), other);
  EXPECT_EQ(
      PJ::ToolboxPanelFoldTestPeer::releaseToolboxPanel(mainWindow(), pluginId(), panelContainer(), title), nullptr);
  EXPECT_EQ(PJ::ToolboxPanelFoldTestPeer::currentPanel(mainWindow()), other);
  EXPECT_FALSE(closed());
}

TEST_F(ToolboxPanelFoldTest, DockingFoldsAnExistingBusyTakeoverBeforeReplacingIt) {
  pinPanel();
  auto* other = new QWidget;
  ASSERT_TRUE(PJ::ToolboxPanelFoldTestPeer::presentPanel(mainWindow(), other));
  bool folded = false;
  PJ::ToolboxPanelFoldTestPeer::setTakeoverFold(
      mainWindow(), other,
      [&](bool transient) {
        EXPECT_TRUE(transient);
        QWidget* released = PJ::ToolboxPanelFoldTestPeer::releaseCentralPanel(mainWindow());
        EXPECT_EQ(released, other);
        PJ::ToolboxPanelFoldTestPeer::pinToolboxPanel(
            mainWindow(), released, u"other"_s, u"Other"_s, {}, nullptr, true);
        folded = true;
      },
      []() { return true; });
  dockPanel();
  EXPECT_TRUE(folded);
  EXPECT_TRUE(PJ::ToolboxPanelFoldTestPeer::isPinned(mainWindow(), u"other"_s));
  EXPECT_EQ(PJ::ToolboxPanelFoldTestPeer::currentPanel(mainWindow()), panelContainer());
}

TEST_F(ToolboxPanelFoldTest, PinnedChromeWithoutFloatingSupportKeepsFloatHidden) {
  const auto wrapped =
      PJ::ToolboxPanelFoldTestPeer::wrapToolboxPanel(mainWindow(), new QWidget, u"Panel"_s, []() {}, []() {});
  wrapped.enter_pinned_chrome();
  auto* float_button = wrapped.container->findChild<QAbstractButton*>(u"buttonMigrateFloat"_s);
  ASSERT_NE(float_button, nullptr);
  EXPECT_TRUE(float_button->isHidden());
  delete wrapped.container;
}

// A floating toolbox is part of the workspace: layout save writes it as a
// pinned tab (floating has no layout representation), so the layout restores
// it as a tab.
TEST_F(ToolboxPanelFoldTest, FloatingToolboxIsSavedAsPinnedTab) {
  PJ::ToolboxPanelFoldTestPeer::insertFloatingToolbox(
      mainWindow(), pluginId(), new QWidget, /*save_config=*/[]() { return u"{\"k\":1}"_s; }, ownHost(),
      u"Floating Panel"_s);

  EXPECT_TRUE(savedLayoutMentions(pluginId()));

  PJ::ToolboxPanelFoldTestPeer::closeAllFloatingToolboxWindows(mainWindow());
  EXPECT_FALSE(PJ::ToolboxPanelFoldTestPeer::isFloating(mainWindow(), pluginId()));
  EXPECT_FALSE(savedLayoutMentions(pluginId()));
}

// The layout's pinned set replaces the live one, floating windows included —
// they persist as tabs, so a layout without this toolbox restores to none of it.
TEST_F(ToolboxPanelFoldTest, RestoreReplacesFloatingWindows) {
  PJ::ToolboxPanelFoldTestPeer::insertFloatingToolbox(
      mainWindow(), pluginId(), new QWidget, /*save_config=*/{}, ownHost(), u"Floating Panel"_s);

  QDomDocument doc;
  ASSERT_TRUE(
      PJ::ToolboxPanelFoldTestPeer::restorePinnedToolboxes(
          mainWindow(), doc.createElement(u"root"_s), /*interactive=*/true));

  EXPECT_FALSE(PJ::ToolboxPanelFoldTestPeer::isFloating(mainWindow(), pluginId()));
}

// A busy floating panel joins the restore's one-question rule: declining the
// cancel confirmation keeps the live set — floating window included — intact.
TEST_F(ToolboxPanelFoldTest, RestoreDeclineKeepsBusyFloatingWindow) {
  PJ::ToolboxPanelFoldTestPeer::insertFloatingToolbox(
      mainWindow(), pluginId(), new QWidget, /*save_config=*/{}, ownHost(), u"Floating Panel"_s);
  setWorkInFlight(true);
  setConfirmAnswer(false);

  QDomDocument doc;
  EXPECT_FALSE(
      PJ::ToolboxPanelFoldTestPeer::restorePinnedToolboxes(
          mainWindow(), doc.createElement(u"root"_s), /*interactive=*/true));

  EXPECT_TRUE(confirmShown());
  EXPECT_TRUE(PJ::ToolboxPanelFoldTestPeer::isFloating(mainWindow(), pluginId()));
  setWorkInFlight(false);
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
