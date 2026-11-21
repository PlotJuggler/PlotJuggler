#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QSizePolicy>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plotting/CustomWidgetSupport.h"
#include "pj_runtime/HistoryScope.h"
#include "pj_widgets/ChromeMetrics.h"

QT_BEGIN_NAMESPACE
class QHBoxLayout;
class QPushButton;
class QStackedWidget;
QT_END_NAMESPACE

namespace PJ {

class CatalogModel;
class IDataWidget;
struct ObjectDropSeed;
class PlotDocker;
class PlotTabFrame;
class SessionManager;

// Custom tab strip + QStackedWidget. Each tab is a QFrame holding the tab name label and a close button; clicking it
// shows its PlotDocker, double-clicking renames it, right-clicking a plot tab offers "Duplicate tab" (duplicateTab),
// dragging a plot tab reorders the strip (one undoableChange per drag that changes the plot tabs' order; saving writes
// the tabs in strip order). Widget and history-exempt tabs cannot be dragged.
// Besides PlotDocker tabs, a tab can host an arbitrary widget (addWidgetTab, which has the lifecycle contract).
class TabbedPlotWidget : public QWidget {
  Q_OBJECT
 public:
  // One object-widget factory for drops (seed != null) and layout restore (kind tag, seed == null); see
  // DockWidget::ObjectWidgetFactory.
  using ObjectWidgetFactory =
      std::function<IDataWidget*(const QString& kind, const ObjectDropSeed* seed, QWidget* parent)>;

  explicit TabbedPlotWidget(QWidget* parent = nullptr);
  explicit TabbedPlotWidget(QString name, QWidget* parent = nullptr);
  ~TabbedPlotWidget() override;

  PlotDocker* currentTab();
  /// An empty `name` gets `tab<N>`, N past every open tab's and every one handed out since the last layout load.
  PlotDocker* addTab(QString name);
  void setDataServices(SessionManager* session, CatalogModel* catalog);
  void setObjectWidgetFactory(ObjectWidgetFactory factory);
  /// Supplies the Custom Widget support to every tab's docks, existing and future.
  void setCustomWidgetSupport(CustomWidgetSupport support);

  // Hosts an arbitrary non-plot widget as a whole tab (a pinned toolbox panel). Takes ownership of `content`
  // (reparented into the tab stack) and selects the new tab. `on_close` runs exactly once when the tab closes (its X
  // button or closeWidgetTab), before `content` is deleted, so the owner can tear down whatever drives the widget.
  // Widget tabs are never serialized and survive xmlLoadState (plot tabs are rebuilt around them): persistence is the
  // owner's concern, the tab name included (the in-place rename edits only the strip label; read it back via
  // widgetTabName). Neither the rename nor adding a widget tab is undoable.
  void addWidgetTab(const QString& tab_name, QWidget* content, std::function<void()> on_close);
  // Selects the tab hosting `content` (added via addWidgetTab); no-op if absent.
  void focusWidgetTab(QWidget* content);
  // Closes the tab hosting `content` like its X button: honours any setWidgetTabPreClose veto, then runs on_close,
  // removes the tab and deletes the widget. No-op if absent.
  void closeWidgetTab(QWidget* content);
  // Removes a widget tab without its close veto or teardown and returns the hidden content with its original size
  // policy; the caller takes ownership. It stays a child of this widget until the caller reparents or deletes it (a
  // reparent re-polishes the whole tree against the app stylesheet, so it must happen once, at the destination).
  // nullptr if content is not a widget tab.
  [[nodiscard]] QWidget* takeWidgetTab(QWidget* content);
  // Inserts a copy of `source`'s plot tab right after it and selects it: same layout, curves and styles (through the
  // tab's XML round trip), named "<name> (copy)". The copy gets fresh tab, area and plot state ids, so it never aliases
  // the source in id-keyed lookups (undo restore, pending bindings), and is an ordinary user tab: history-exempt and
  // plugin-owner metadata are not copied. Emits undoableChange. nullptr if `source` is not a plot tab here or its state
  // fails to restore (the half-built copy is closed).
  PlotDocker* duplicateTab(PlotDocker* source);
  // Closes a plot tab like the tab's close button, the last-serializable-tab rule included.
  void closeTab(PlotDocker* docker);
  // Gives a widget tab a veto over its own close: `pre_close` runs before any teardown and returning false abandons the
  // close entirely (a pinned panel with work in flight asks the user first). Optional: a tab without one always closes.
  // `pre_close` may run a modal event loop and may close this or other tabs, which just ends the close it was consulted
  // for. Replaces any previous callback; no-op if `content` is not a widget tab. closeWidgetTabForced() bypasses it,
  // for shutdown and layout replacement, which must not be refusable.
  void setWidgetTabPreClose(QWidget* content, std::function<bool()> pre_close);
  // Like closeWidgetTab, ignoring any setWidgetTabPreClose veto.
  void closeWidgetTabForced(QWidget* content);
  // Strip label of the widget tab hosting `content` (empty if absent): the only store of a user rename, so an owner
  // persisting the tab reads it at save time.
  [[nodiscard]] QString widgetTabName(QWidget* content) const;
  // Renames the widget tab hosting `content` (no-op if absent). Not undoable.
  void setWidgetTabName(QWidget* content, const QString& name);

  // Docker-tab enumeration counts and indexes only PlotDocker tabs, so `for (i < dockerCount()) dockerAt(i)` never
  // yields a null; tabCount() includes widget tabs.
  [[nodiscard]] int dockerCount() const;
  PlotDocker* dockerAt(int index);
  [[nodiscard]] int tabCount() const;
  // Visits every PlotDocker tab in tab order in one pass (dockerCount() / dockerAt() are each a linear scan, so an
  // index loop is quadratic). `operation` must not add or close tabs.
  void forEachDocker(const std::function<void(PlotDocker*)>& operation) const;
  // First PlotDocker tab satisfying `predicate`, or nullptr.
  [[nodiscard]] PlotDocker* findDocker(const std::function<bool(PlotDocker*)>& predicate) const;
  // Selects `docker`'s tab like a click on its frame; no-op if `docker` is null or hosts no tab here (a stale pointer).
  void focusTab(PlotDocker* docker);
  // Renames `docker` and its tab-strip label (PlotDocker::setName alone leaves the label stale). Pushes no undo step
  // (callers renaming user tabs emit their own); no-op if `docker` hosts no tab here.
  void renameTab(PlotDocker* docker, const QString& name);

  // Panel-toggle buttons: created here, relocated into the title bar by the MainWindow shell, which wires clicked() to
  // show or hide the outer panel and swaps the filled/outlined glyph.
  [[nodiscard]] QPushButton* leftPanelButton() const {
    return button_left_panel_;
  }
  [[nodiscard]] QPushButton* bottomPanelButton() const {
    return button_bottom_panel_;
  }
  [[nodiscard]] QPushButton* rightPanelButton() const {
    return button_right_panel_;
  }

  [[nodiscard]] QString name() const {
    return name_;
  }
  void setName(QString name) {
    name_ = std::move(name);
  }

  [[nodiscard]] QString stateId() const {
    return state_id_;
  }
  void setStateId(QString id) {
    if (!id.isEmpty()) {
      state_id_ = std::move(id);
    }
  }

  // Serializes and restores plot pages, always preserving widget and ephemeral pages. Full scope includes
  // history-exempt plot pages; history scope omits them and keeps their live objects through a restore. Restoring emits
  // no undoableChange.
  [[nodiscard]] QDomElement xmlSaveState(QDomDocument& doc, SnapshotScope scope = SnapshotScope::kFull) const;
  bool xmlLoadState(const QDomElement& tabbed_area, RestoreIntent intent = RestoreIntent::kReplace);

 public slots:
  void onStylesheetChanged(QString theme);
  // Rebinds the tab-strip chrome metrics (the [+] button, the three panel-toggle buttons, the strip height, every open
  // tab frame); connected to MainWindow::chromeMetricsChanged. layout_spacing is ignored: tab frames sit flush, and
  // gaps would expose strips of the tab-bar background.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 signals:
  void undoableChange();
  void tabAdded(PlotDocker* docker);
  // Fires after the active tab changes (click, programmatic switch, close, layout restore); `docker` is the active
  // PlotDocker or nullptr.
  void currentTabChanged(PlotDocker* docker);

 private slots:
  void onAddTabButtonPressed();
  void onTabFrameClicked(PlotTabFrame* frame);
  void onTabRenameRequested(PlotTabFrame* frame, const QString& new_name);
  void onTabCloseRequested(PlotTabFrame* frame);
  void onTabContextMenuRequested(PlotTabFrame* frame, const QPoint& pos);
  void onTabDragStarted(PlotTabFrame* frame);
  void onTabDragMoved(PlotTabFrame* frame, const QPoint& global_pos);
  void onTabDragFinished();

 private:
  struct TabEntry {
    PlotTabFrame* frame = nullptr;
    // Exactly one of docker / widget is set: docker for plot tabs, widget (plus its owner teardown on_close) for hosted
    // widget tabs.
    PlotDocker* docker = nullptr;
    QWidget* widget = nullptr;
    std::function<void()> on_close;
    // Widget tabs only (setWidgetTabPreClose): consulted first on a vetoable close; false aborts before any teardown.
    std::function<bool()> pre_close;
    // The hosted widget's size policy at addWidgetTab time, restored when its tab is current; hidden widget pages are
    // set to Ignored so they never inflate the QStackedWidget's union size hint.
    QSizePolicy original_policy;
  };

  TabEntry* findEntry(PlotTabFrame* frame);
  TabEntry* findEntry(QWidget* content);
  // The entry hosting `content` as a widget tab (nullptr for absent content and docker pages): the predicate behind the
  // widget-tab API.
  TabEntry* findWidgetEntry(QWidget* content);
  [[nodiscard]] const TabEntry* findWidgetEntry(QWidget* content) const;
  // Dockers an undo snapshot can reconstruct. Ephemeral and history-exempt pages are outside history, so closing the
  // last ordinary page creates a fresh one even while either kind remains.
  [[nodiscard]] int historySerializableDockerCount() const;
  // The one close path for both tab kinds; with honor_veto a widget tab's pre_close runs first and can abandon it.
  void closeTab(PlotTabFrame* frame, bool honor_veto);
  // The stack page a tab entry shows.
  static QWidget* contentOf(const TabEntry& entry);
  void updateSelectionStyle();
  // Keeps hidden widget-tab pages from inflating the stack's union size hint (Ignored while not current, original
  // policy when current). Connected to QStackedWidget::currentChanged.
  void adaptWidgetTabPagePolicies();
  void selectEntry(const TabEntry& entry);
  PlotDocker* createDocker(const QString& tab_name);
  PlotTabFrame* createTabFrame(const QString& tab_name);
  // Moves tab `from` to index `to` in both tabs_ and the strip layout, which hold the tab frames at the same indexes.
  void moveTabFrame(int from, int to);
  [[nodiscard]] int tabIndexOf(PlotTabFrame* frame) const;
  // A plot page undo snapshots rebuild (not a widget tab, not history-exempt), and those pages in strip order.
  static bool isHistoryDocker(const TabEntry& entry);
  // The page to show when `entry`'s goes away: the nearest tab on the strip on side `first`, else on the other side;
  // with `plot_pages_only`, the nearest history docker. nullptr when there is none.
  enum class Side : std::uint8_t { kLeft, kRight };
  [[nodiscard]] QWidget* neighbourOf(const TabEntry& entry, Side first, bool plot_pages_only) const;
  [[nodiscard]] std::vector<PlotDocker*> historyDockerOrder() const;

  QHBoxLayout* tabs_bar_layout_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  QWidget* tabs_inner_ = nullptr;
  QPushButton* button_add_tab_ = nullptr;
  QPushButton* button_left_panel_ = nullptr;
  QPushButton* button_bottom_panel_ = nullptr;
  QPushButton* button_right_panel_ = nullptr;
  // Tab-strip chrome metrics; the defaults match the kTabBar* constants in the .cpp so first paint is unchanged until
  // the host pushes the saved Preferences values. layout_spacing is ignored: tab frames sit flush.
  ChromeMetrics chrome_metrics_{20, 3, 0, 0};
  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  ObjectWidgetFactory object_widget_factory_;
  CustomWidgetSupport custom_widget_support_;
  QString name_;
  QString state_id_;
  /// The highest `tab<N>` suffix addTab has given or seen open. Lowered only by a layout load, so within one layout a
  /// closed tab's name is not handed out again.
  int tab_suffix_count_ = 0;
  // True while xmlLoadState / duplicateTab rebuild dockers: their undoableChange is not forwarded.
  bool restoring_state_ = false;
  std::vector<TabEntry> tabs_;
  // historyDockerOrder() when the running drag started; empty when no drag is under way or its tab may not move. The
  // pointers are only compared, never dereferenced (a page can close mid-drag).
  std::vector<PlotDocker*> drag_start_order_;
};

}  // namespace PJ
