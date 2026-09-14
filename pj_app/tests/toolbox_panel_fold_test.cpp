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
#include <QStackedWidget>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <functional>
#include <memory>
#include <pj_plugins/host_qt/widget_binding.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "MainWindow.h"
#include "ToolboxPresenter.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
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
    std::function<void(bool pinned)> apply_chrome;
  };

  // Positional hooks, packed into MainWindow::ToolboxChromeHooks for the call.
  static Wrapped wrapToolboxPanel(
      MainWindow& window, QWidget* content, const QString& title, std::function<void()> on_close,
      std::function<void()> on_migrate, std::function<void()> on_float = {}, const QString& persist_key = {},
      std::function<void()> on_dock = {}) {
    MainWindow::WrappedToolboxPanel wrapped = window.wrapToolboxPanel(
        content, title,
        MainWindow::ToolboxChromeHooks{
            .on_close = std::move(on_close),
            .on_migrate = std::move(on_migrate),
            .on_float = std::move(on_float),
            .on_dock = std::move(on_dock)},
        persist_key);
    return {.container = wrapped.container, .apply_chrome = std::move(wrapped.apply_chrome)};
  }

  // The production chrome hooks: every gesture looks the presenter up by id.
  static void withToolbox(MainWindow& window, const QString& plugin_id, void (ToolboxPresenter::*gesture)()) {
    window.withToolbox(plugin_id, [gesture](ToolboxPresenter& live) { (live.*gesture)(); });
  }
  static void userPin(MainWindow& window, const QString& plugin_id) {
    window.withToolbox(plugin_id, [](ToolboxPresenter& live) { live.moveToTab(/*transient=*/false); });
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

  // Releases through the registered presenter; nullptr (presentation intact)
  // when nothing is registered under `id` for this container.
  static QWidget* releaseToolboxPanel(MainWindow& window, const QString& id, QWidget* container, QString& title) {
    ToolboxPresenter* presenter = window.findToolbox(id);
    if (presenter == nullptr || presenter->container() != container) {
      return nullptr;
    }
    QWidget* released = presenter->release();
    if (released != nullptr) {
      title = presenter->title();
    }
    return released;
  }

  static QWidget* currentPanel(const MainWindow& window) {
    return window.current_panel_;
  }

  static void dismissTakeoverPanel(MainWindow& window) {
    window.dismissTakeoverPanel();
  }

  // The production marketplace launch: a banner-wrapped panel with no
  // presenter, whose close policy lives in its own chrome hooks.
  static void openMarketplace(MainWindow& window) {
    window.onOpenMarketplace();
  }
  [[nodiscard]] static QWidget* marketplaceContainer(const MainWindow& window) {
    return window.marketplace_container_.data();
  }

  // The production launch entry, driven at its "one live instance" head; a
  // plugin id absent from the catalog returns before any session is built.
  static void launchToolbox(MainWindow& window, const QString& plugin_id) {
    window.launchToolbox(plugin_id);
  }

  static void foldTakeoverPanelIfOwnedBy(MainWindow& window, const void* owner, bool transient) {
    window.foldTakeoverPanelIfOwnedBy(owner, transient);
  }

  // The identity an ingest started by this toolbox reports (null when none is
  // registered).
  [[nodiscard]] static const void* ownerOf(const MainWindow& window, const QString& plugin_id) {
    const ToolboxPresenter* presenter = window.findToolbox(plugin_id);
    return presenter != nullptr ? presenter->sessionOwner() : nullptr;
  }

  // Registers an engine-less presenter (no session, no chrome hooks) for
  // `container`: the bare shape every presentation case starts from.
  static ToolboxPresenter& registerBare(
      MainWindow& window, const QString& plugin_id, QWidget* container, const QString& title,
      std::function<QString()> save_config, ToolboxRuntimeHost* host,
      std::function<void(bool pinned)> apply_chrome = {}) {
    return window.registerToolbox(
        plugin_id, ToolboxPresenter::Spec{
                       .container = container,
                       .engine = nullptr,
                       .session = {},
                       .host = host,
                       .title = title,
                       .save_config = std::move(save_config),
                       .apply_chrome = std::move(apply_chrome)});
  }

  // Pins through the presenter registered under `plugin_id`, registering a
  // bare one for the container first if none exists yet.
  static void moveToTab(
      MainWindow& window, QWidget* container, const QString& plugin_id, const QString& title,
      std::function<QString()> save_config, ToolboxRuntimeHost* host, bool transient) {
    ToolboxPresenter* presenter = window.findToolbox(plugin_id);
    if (presenter == nullptr) {
      presenter = &registerBare(window, plugin_id, container, title, std::move(save_config), host);
    }
    presenter->moveToTab(transient);
  }

  /// The engine's close routing: returns whether the engine may tear itself
  /// down (false = the host declined and the panel must keep exchanging widget
  /// data).
  [[nodiscard]] static bool emitPinnedCloseRequest(MainWindow& window, QWidget* container, const std::string& reason) {
    ToolboxPresenter* presenter = window.toolboxForContainer(container);
    return presenter == nullptr || presenter->onEngineCloseRequested(reason);
  }

  // Docks through the presenter registered under `plugin_id` (registering a
  // bare one for the container first if none exists yet).
  [[nodiscard]] static bool dockThroughPresenter(
      MainWindow& window, QWidget* container, const QString& plugin_id, const QString& title,
      ToolboxRuntimeHost* host) {
    ToolboxPresenter* presenter = window.findToolbox(plugin_id);
    if (presenter == nullptr) {
      presenter = &registerBare(window, plugin_id, container, title, []() { return u"{}"_s; }, host);
    }
    return presenter->moveToDock();
  }

  [[nodiscard]] static bool isTakeover(const MainWindow& window, const QString& plugin_id) {
    const ToolboxPresenter* presenter = window.findToolbox(plugin_id);
    return presenter != nullptr && presenter->presentation() == ToolboxPresenter::Presentation::kTakeover;
  }

  [[nodiscard]] static bool isPinned(const MainWindow& window, const QString& plugin_id) {
    const ToolboxPresenter* presenter = window.findToolbox(plugin_id);
    return presenter != nullptr && presenter->presentation() == ToolboxPresenter::Presentation::kTab;
  }

  // A host-chosen fold (excluded from layout save) rather than a user pin.
  [[nodiscard]] static bool isTransient(const MainWindow& window, const QString& plugin_id) {
    const ToolboxPresenter* presenter = window.findToolbox(plugin_id);
    return presenter != nullptr && presenter->transient();
  }

  [[nodiscard]] static bool isLive(const MainWindow& window, const QString& plugin_id) {
    return window.findToolbox(plugin_id) != nullptr;
  }

  [[nodiscard]] static QString savedPinnedToolboxesXml(const MainWindow& window) {
    QDomDocument doc;
    doc.appendChild(window.savePinnedToolboxes(doc));
    return doc.toString();
  }

  // Every live toolbox, synchronously (what TearDown sweeps).
  static void teardownAllToolboxes(MainWindow& window) {
    window.teardownAllToolboxes();
  }

  // Registers a presenter already in the floating presentation (the production
  // path needs a live plugin session to float); `floating_window` stands in
  // for the floating PJ::Dialog and is deleted by the presenter's teardown.
  static void insertFloatingToolbox(
      MainWindow& window, const QString& plugin_id, QWidget* floating_window, std::function<QString()> save_config,
      ToolboxRuntimeHost* host, const QString& label) {
    ToolboxPresenter& presenter = registerBare(window, plugin_id, floating_window, label, std::move(save_config), host);
    presenter.presentation_ = ToolboxPresenter::Presentation::kFloating;
    presenter.floating_window_ = floating_window;
  }

  [[nodiscard]] static bool isFloating(const MainWindow& window, const QString& plugin_id) {
    const ToolboxPresenter* presenter = window.findToolbox(plugin_id);
    return presenter != nullptr && presenter->presentation() == ToolboxPresenter::Presentation::kFloating;
  }

  // The service a toolbox's plugin session writes into; its QObject identity
  // is what a teardown-order probe watches.
  [[nodiscard]] static SessionManager* sessionManager(const MainWindow& window) {
    return &window.session_->sessionManager();
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
          if (during_confirm_) {
            during_confirm_();
          }
          return confirm_answer_;
        },
        [this](PJ::ToolboxRuntimeHost* host) { return busy_hosts_.contains(host); },
        [this](PJ::ToolboxRuntimeHost* host) { stopped_hosts_.push_back(host); });
  }

  void TearDown() override {
    if (window_ == nullptr) {
      return;  // the case destroyed the window itself
    }
    // Forced, so a still-"busy" panel cannot veto the cleanup.
    PJ::ToolboxPanelFoldTestPeer::teardownAllToolboxes(mainWindow());
    if (QWidget* released = PJ::ToolboxPanelFoldTestPeer::releaseCentralPanel(mainWindow()); released != nullptr) {
      delete released;
    }
    delete container_.data();
    PJ::ToolboxPanelFoldTestPeer::setSeams(mainWindow(), {}, {}, {});
  }

  [[nodiscard]] static PJ::MainWindow& mainWindow() {
    return *window_;
  }

  // Destroys the shared window inside a case: the destructor IS the subject
  // of the teardown-order cases. Each case runs in its own process, so no
  // later case misses the window.
  static void destroyMainWindow() {
    window_.reset();
  }

  // Distinct host identities. Both host interactions are injected, so these
  // are never dereferenced — only compared.
  [[nodiscard]] PJ::ToolboxRuntimeHost* ownHost() {
    return reinterpret_cast<PJ::ToolboxRuntimeHost*>(&own_host_token_);
  }
  [[nodiscard]] PJ::ToolboxRuntimeHost* otherHost() {
    return reinterpret_cast<PJ::ToolboxRuntimeHost*>(&other_host_token_);
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

  // Wraps a dummy panel in the real toolbox banner with launchToolbox's own
  // chrome hooks, and registers the engine-less presenter they drive (host =
  // ownHost(), so the seams decide its work-in-flight answer).
  void wrapPanel() {
    using Peer = PJ::ToolboxPanelFoldTestPeer;
    const QString id = pluginId();
    wrapped_ = Peer::wrapToolboxPanel(
        mainWindow(), new QWidget, panelTitle(),
        /*on_close=*/[id]() { Peer::withToolbox(mainWindow(), id, &PJ::ToolboxPresenter::requestClose); },
        /*on_migrate=*/[id]() { Peer::userPin(mainWindow(), id); },
        /*on_float=*/[id]() { Peer::withToolbox(mainWindow(), id, &PJ::ToolboxPresenter::moveToFloating); },
        /*persist_key=*/{},
        /*on_dock=*/[id]() { Peer::withToolbox(mainWindow(), id, &PJ::ToolboxPresenter::moveToDockOrTab); });
    container_ = wrapped_.container;
    Peer::registerBare(
        mainWindow(), id, wrapped_.container, panelTitle(), saveConfig(), ownHost(), wrapped_.apply_chrome);
  }

  void dockPanel() {
    ASSERT_TRUE(
        PJ::ToolboxPanelFoldTestPeer::dockThroughPresenter(
            mainWindow(), panelContainer(), pluginId(), panelTitle(), ownHost()));
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
    PJ::ToolboxPanelFoldTestPeer::moveToTab(
        mainWindow(), wrapped_.container, pluginId(), panelTitle(), saveConfig(), ownHost(), /*transient=*/false);
  }

  // Presents the wrapped panel as the chart-area takeover through its presenter.
  void presentAsTakeover() {
    wrapPanel();
    dockPanel();
  }

  // What the toolbox's own ingest-start callback does.
  void emitIngestStarted() {
    PJ::ToolboxPanelFoldTestPeer::foldTakeoverPanelIfOwnedBy(
        mainWindow(), PJ::ToolboxPanelFoldTestPeer::ownerOf(mainWindow(), pluginId()), /*transient=*/true);
  }

  void dismissTakeover() {
    PJ::ToolboxPanelFoldTestPeer::dismissTakeoverPanel(mainWindow());
  }

  /// Returns whether the host let the engine tear itself down.
  bool emitCloseRequest(const std::string& reason) {
    return PJ::ToolboxPanelFoldTestPeer::emitPinnedCloseRequest(mainWindow(), container_.data(), reason);
  }

  void setWorkInFlight(bool busy) {
    if (busy) {
      busy_hosts_.insert(ownHost());
    } else {
      busy_hosts_.erase(ownHost());
    }
  }

  void setOtherHostBusy() {
    busy_hosts_.insert(otherHost());
  }

  void setConfirmAnswer(bool answer) {
    confirm_answer_ = answer;
  }

  // What the modal confirmation's event loop lets happen before it answers.
  void setDuringConfirm(std::function<void()> during_confirm) {
    during_confirm_ = std::move(during_confirm);
  }

  // Torn down: the registry entry (which owns the plugin session) is gone.
  [[nodiscard]] static bool closed() {
    return !PJ::ToolboxPanelFoldTestPeer::isLive(mainWindow(), pluginId());
  }

  // In a tab, whichever disposition put it there.
  [[nodiscard]] static bool foldedIntoTab() {
    return PJ::ToolboxPanelFoldTestPeer::isPinned(mainWindow(), pluginId());
  }

  // The user's pin (persisted) vs the host-chosen fold (transient).
  [[nodiscard]] static bool userPinned() {
    return foldedIntoTab() && !PJ::ToolboxPanelFoldTestPeer::isTransient(mainWindow(), pluginId());
  }

  [[nodiscard]] static bool busyFolded() {
    return foldedIntoTab() && PJ::ToolboxPanelFoldTestPeer::isTransient(mainWindow(), pluginId());
  }

  [[nodiscard]] static bool floated() {
    return PJ::ToolboxPanelFoldTestPeer::isFloating(mainWindow(), pluginId());
  }

  void enterPinnedChrome() {
    wrapped_.apply_chrome(/*pinned=*/true);
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

  [[nodiscard]] static std::function<QString()> saveConfig() {
    return []() { return u"{}"_s; };
  }

  inline static std::unique_ptr<QTemporaryDir> extensions_dir_;
  inline static std::unique_ptr<PJ::MainWindow> window_;

  void* own_host_token_ = nullptr;
  void* other_host_token_ = nullptr;
  PJ::ToolboxPanelFoldTestPeer::Wrapped wrapped_;
  QPointer<QWidget> container_;
  std::set<PJ::ToolboxRuntimeHost*> busy_hosts_;
  bool confirm_answer_ = true;
  bool confirm_shown_ = false;
  std::function<void()> during_confirm_;
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
  EXPECT_FALSE(userPinned());
  EXPECT_FALSE(closed());
}

// Rule 1 — nothing running, so the X keeps its plain teardown.
TEST_F(ToolboxPanelFoldTest, BannerCloseTearsDownWhenIdle) {
  wrapPanel();
  setWorkInFlight(false);

  bannerCloseButton()->click();

  EXPECT_TRUE(closed());
  EXPECT_FALSE(foldedIntoTab());
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

  EXPECT_TRUE(foldedIntoTab());
  EXPECT_FALSE(closed());
}

// Launching any other panel goes through dismissTakeoverPanel, which must not
// become a second silent cancel.
TEST_F(ToolboxPanelFoldTest, DismissTakeoverFoldsWhileWorkInFlight) {
  presentAsTakeover();
  setWorkInFlight(true);

  dismissTakeover();

  EXPECT_TRUE(foldedIntoTab());
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

// The marketplace has no presenter: after "Move to a tab" its banner X must
// still close the tab it now lives in, not the (empty) central area.
TEST_F(ToolboxPanelFoldTest, MarketplaceTabBannerCloseClosesTheTab) {
  using Peer = PJ::ToolboxPanelFoldTestPeer;
  Peer::openMarketplace(mainWindow());
  QPointer<QWidget> container = Peer::marketplaceContainer(mainWindow());
  ASSERT_FALSE(container.isNull());
  ASSERT_EQ(Peer::currentPanel(mainWindow()), container.data());

  container->findChild<QToolButton*>(u"buttonMigrateTab"_s)->click();
  ASSERT_EQ(Peer::currentPanel(mainWindow()), nullptr);
  ASSERT_FALSE(tabbedWidget().widgetTabName(container).isEmpty());

  container->findChild<QToolButton*>(u"buttonClose"_s)->click();

  EXPECT_TRUE(container.isNull() || tabbedWidget().widgetTabName(container).isEmpty());
  if (!container.isNull()) {
    tabbedWidget().closeWidgetTabForced(container);
  }
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
      /*persist_key=*/pluginId());
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
      /*persist_key=*/pluginId());
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
      /*persist_key=*/pluginId());
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
      /*persist_key=*/pluginId());
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
// window's own title bar is the one header. Pinned chrome re-shows the banner.
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
  EXPECT_TRUE(foldedIntoTab());
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
  ASSERT_TRUE(
      PJ::ToolboxPanelFoldTestPeer::dockThroughPresenter(mainWindow(), other, u"other"_s, u"Other"_s, otherHost()));
  setOtherHostBusy();
  dockPanel();
  EXPECT_TRUE(PJ::ToolboxPanelFoldTestPeer::isPinned(mainWindow(), u"other"_s));
  EXPECT_TRUE(PJ::ToolboxPanelFoldTestPeer::isTransient(mainWindow(), u"other"_s)) << "a host-chosen fold";
  EXPECT_EQ(PJ::ToolboxPanelFoldTestPeer::currentPanel(mainWindow()), panelContainer());
}

TEST_F(ToolboxPanelFoldTest, PinnedChromeWithoutFloatingSupportKeepsFloatHidden) {
  const auto wrapped =
      PJ::ToolboxPanelFoldTestPeer::wrapToolboxPanel(mainWindow(), new QWidget, u"Panel"_s, []() {}, []() {});
  wrapped.apply_chrome(/*pinned=*/true);
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

  PJ::ToolboxPanelFoldTestPeer::teardownAllToolboxes(mainWindow());
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

// The confirmation runs a modal event loop, during which a takeover the layout
// does not name can receive its ingest-start callback and fold into a tab. The
// replaced set is the live tab set at teardown time, so that fold joins it:
// its job is stopped and its tab goes down with the rest.
TEST_F(ToolboxPanelFoldTest, RestoreReplacesATakeoverThatFoldsDuringTheConfirmation) {
  using Peer = PJ::ToolboxPanelFoldTestPeer;
  pinPanel();
  setWorkInFlight(true);
  const QString other_id = pluginId() + u"_takeover"_s;
  const auto other = Peer::wrapToolboxPanel(mainWindow(), new QWidget, u"Takeover"_s, {}, {});
  ASSERT_TRUE(Peer::dockThroughPresenter(mainWindow(), other.container, other_id, u"Takeover"_s, otherHost()));
  ASSERT_TRUE(Peer::isTakeover(mainWindow(), other_id));
  setDuringConfirm([this, other_id]() {
    setOtherHostBusy();
    Peer::foldTakeoverPanelIfOwnedBy(mainWindow(), Peer::ownerOf(mainWindow(), other_id), /*transient=*/true);
  });

  QDomDocument doc;
  EXPECT_TRUE(Peer::restorePinnedToolboxes(mainWindow(), doc.createElement(u"root"_s), /*interactive=*/true));

  EXPECT_TRUE(confirmShown());
  EXPECT_TRUE(closed());
  EXPECT_FALSE(Peer::isLive(mainWindow(), other_id));
  EXPECT_EQ(std::count(stoppedHosts().begin(), stoppedHosts().end(), ownHost()), 1);
  EXPECT_EQ(std::count(stoppedHosts().begin(), stoppedHosts().end(), otherHost()), 1);
  setWorkInFlight(false);
}

// D5 again, for a busy toolbox that sits in the chart-area takeover while the
// layout being restored names it as a pinned tab (with a config, so the restore
// must relaunch it): a non-interactive restore must retain it unprompted, not
// fold it and then raise the tab's cancel confirmation on the relaunch.
TEST_F(ToolboxPanelFoldTest, NonInteractiveRestoreKeepsBusyTakeoverNamedByTheLayoutUnprompted) {
  wrapPanel();
  ASSERT_TRUE(
      PJ::ToolboxPanelFoldTestPeer::dockThroughPresenter(
          mainWindow(), panelContainer(), pluginId(), u"Docked"_s, ownHost()));
  ASSERT_TRUE(PJ::ToolboxPanelFoldTestPeer::isTakeover(mainWindow(), pluginId()));
  setWorkInFlight(true);
  setConfirmAnswer(false);

  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  QDomElement pinned = doc.createElement(u"pinned_toolboxes"_s);
  QDomElement toolbox = doc.createElement(u"toolbox"_s);
  toolbox.setAttribute(u"plugin_id"_s, pluginId());
  toolbox.appendChild(doc.createCDATASection(u"{\"k\":1}"_s));
  pinned.appendChild(toolbox);
  root.appendChild(pinned);
  doc.appendChild(root);

  const bool applied = PJ::ToolboxPanelFoldTestPeer::restorePinnedToolboxes(mainWindow(), root, /*interactive=*/false);

  EXPECT_FALSE(applied);
  EXPECT_FALSE(confirmShown());
  EXPECT_TRUE(PJ::ToolboxPanelFoldTestPeer::isTakeover(mainWindow(), pluginId()));
  EXPECT_TRUE(stoppedHosts().empty());
  setWorkInFlight(false);
}

// Relaunching the toolbox that sits busy in the chart-area takeover must not
// restart it (the teardown is its job's kill switch): the "one live instance"
// head folds it into a transient tab and focuses that tab.
TEST_F(ToolboxPanelFoldTest, RelaunchingABusyTakeoverFoldsItIntoATabAndFocusesIt) {
  wrapPanel();
  ASSERT_TRUE(
      PJ::ToolboxPanelFoldTestPeer::dockThroughPresenter(
          mainWindow(), panelContainer(), pluginId(), u"Docked"_s, ownHost()));
  ASSERT_TRUE(PJ::ToolboxPanelFoldTestPeer::isTakeover(mainWindow(), pluginId()));
  setWorkInFlight(true);
  // A regression that tears the panel down instead reaches the catalog lookup,
  // whose miss raises a modal: dismiss it so the failure is reported, not hung.
  QObject modal_guard;
  QTimer::singleShot(200, &modal_guard, []() {
    if (QWidget* modal = QApplication::activeModalWidget()) {
      modal->close();
    }
  });

  PJ::ToolboxPanelFoldTestPeer::launchToolbox(mainWindow(), pluginId());

  EXPECT_FALSE(PJ::ToolboxPanelFoldTestPeer::isTakeover(mainWindow(), pluginId()));
  EXPECT_TRUE(PJ::ToolboxPanelFoldTestPeer::isPinned(mainWindow(), pluginId()));
  EXPECT_FALSE(savedLayoutMentions(pluginId())) << "a host-chosen fold stays out of the layout";
  auto* stack = tabbedWidget().findChild<QStackedWidget*>(QString(), Qt::FindDirectChildrenOnly);
  ASSERT_NE(stack, nullptr);
  EXPECT_EQ(stack->currentWidget(), panelContainer()) << "the relaunch focuses the folded tab";
  EXPECT_TRUE(stoppedHosts().empty());
  setWorkInFlight(false);
}

// A plugin-initiated close the user then declines (busy tab, "keep
// downloading") must leave the engine running: returning true would let the
// engine close itself under a tab that stays on screen.
TEST_F(ToolboxPanelFoldTest, DeclinedPluginCloseRequestKeepsEngineRunning) {
  pinPanel();
  setWorkInFlight(true);
  setConfirmAnswer(false);

  EXPECT_FALSE(emitCloseRequest("user_back"));

  EXPECT_TRUE(confirmShown());
  EXPECT_FALSE(engineClosed());
  EXPECT_TRUE(stoppedHosts().empty());
  setWorkInFlight(false);
}

// A floating window is a QObject child of the MainWindow, so without an
// explicit teardown it dies in ~QObject's child sweep — after the AppSession
// (and the SessionManager its plugin session flushes into) is already gone.
// The probe stands in for the PanelSession the window's close closure holds.
TEST_F(ToolboxPanelFoldTest, DirectDestructionReleasesFloatingSessionWhileServicesAreAlive) {
  struct Probe {
    QPointer<PJ::SessionManager> services;
    bool* services_alive_at_release;
    ~Probe() {
      *services_alive_at_release = !services.isNull();
    }
  };
  static bool services_alive_at_release = false;
  auto probe = std::make_shared<Probe>(Probe{
      .services = PJ::ToolboxPanelFoldTestPeer::sessionManager(mainWindow()),
      .services_alive_at_release = &services_alive_at_release});
  auto* window = new QWidget(&mainWindow(), Qt::Window);
  // The production window's finished() closure owns the session; the
  // connection dies with the window, so the probe does too.
  QObject::connect(window, &QObject::destroyed, window, [probe]() {});
  PJ::ToolboxPanelFoldTestPeer::insertFloatingToolbox(
      mainWindow(), pluginId(), window, /*save_config=*/[probe]() { return u"{}"_s; }, ownHost(), u"Floating Panel"_s);
  probe.reset();

  destroyMainWindow();

  EXPECT_TRUE(services_alive_at_release)
      << "the floating toolbox's session was released after the SessionManager it writes into";
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
