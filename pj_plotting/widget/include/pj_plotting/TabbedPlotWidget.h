#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QSizePolicy>
#include <QString>
#include <QWidget>
#include <functional>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
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

// Custom tab strip + QStackedWidget. Each tab is a small QFrame
// containing the tab name label and a close button; clicking a frame
// switches the stack to its PlotDocker, double-clicking renames it.
// Same external API the previous QTabWidget-based version exposed.
// Besides PlotDocker tabs, a tab can host an arbitrary widget
// (addWidgetTab) — see that method for the lifecycle contract.
class TabbedPlotWidget : public QWidget {
  Q_OBJECT
 public:
  // Single object-widget factory used by both drop (seed != null) and layout
  // restore (kind tag, seed == null). See DockWidget::ObjectWidgetFactory.
  using ObjectWidgetFactory =
      std::function<IDataWidget*(const QString& kind, const ObjectDropSeed* seed, QWidget* parent)>;

  explicit TabbedPlotWidget(QWidget* parent = nullptr);
  explicit TabbedPlotWidget(QString name, QWidget* parent = nullptr);
  ~TabbedPlotWidget() override;

  PlotDocker* currentTab();
  PlotDocker* addTab(QString name);
  void setDataServices(SessionManager* session, CatalogModel* catalog);
  void setObjectWidgetFactory(ObjectWidgetFactory factory);

  // Hosts an arbitrary non-plot widget as a whole tab (a heterogeneous,
  // VSCode-style tab — e.g. a pinned toolbox panel). Takes ownership of
  // `content` (reparented into the tab stack) and selects the new tab.
  // `on_close` runs exactly once when the tab closes (its X button or
  // closeWidgetTab), BEFORE `content` is deleted, so the owner can tear
  // down whatever drives the widget. Widget tabs are never serialized and
  // survive xmlLoadState (plot tabs are rebuilt around them) — persistence
  // is the owner's concern, including the tab name: the in-place rename
  // edits only the strip label (read it back via widgetTabName), and is
  // not undoable. Adding a widget tab is not an undoable change either.
  void addWidgetTab(const QString& tab_name, QWidget* content, std::function<void()> on_close);
  // Selects the tab hosting `content` (added via addWidgetTab). No-op if absent.
  void focusWidgetTab(QWidget* content);
  // Closes the tab hosting `content` exactly like its X button: honours any
  // setWidgetTabPreClose veto, then runs its on_close, removes the tab and
  // deletes the widget. No-op if absent.
  void closeWidgetTab(QWidget* content);
  // Removes a widget tab without consulting its close veto or running teardown.
  // Returns the hidden, parentless content with its original size policy; the
  // caller takes ownership. Returns nullptr if content is not a widget tab.
  [[nodiscard]] QWidget* takeWidgetTab(QWidget* content);
  // Close a plot tab programmatically. Honours the same path as the tab's
  // close button, including the last-serializable-tab rule.
  void closeTab(PlotDocker* docker);
  // Gives a widget tab a veto over its own close: `pre_close` runs BEFORE any
  // teardown and returning false abandons the close entirely (nothing is run,
  // removed or deleted), which is how a pinned panel with work in flight asks
  // the user to confirm first. Optional — a tab without one always closes.
  // `pre_close` may run a modal event loop, and may itself close this or other
  // tabs; doing so simply ends the close it was consulted for. Replaces any
  // previously set callback; no-op if `content` is not a widget tab.
  // closeWidgetTabForced() bypasses it — use that for shutdown and layout
  // replacement, which must not be refusable.
  void setWidgetTabPreClose(QWidget* content, std::function<bool()> pre_close);
  // Like closeWidgetTab, but ignores any setWidgetTabPreClose veto.
  void closeWidgetTabForced(QWidget* content);
  // Current strip label of the widget tab hosting `content` (empty if
  // absent) — the sole store of a user rename, so owners persisting the
  // tab must read it at save time.
  [[nodiscard]] QString widgetTabName(QWidget* content) const;
  // Renames the widget tab hosting `content` (no-op if absent). Not undoable.
  void setWidgetTabName(QWidget* content, const QString& name);

  // Docker-tab enumeration: counts/indexes ONLY PlotDocker tabs, skipping
  // widget tabs, so `for (i < dockerCount()) dockerAt(i)` never yields null
  // holes. tabCount() is the total including widget tabs.
  [[nodiscard]] int dockerCount() const;
  PlotDocker* dockerAt(int index);
  [[nodiscard]] int tabCount() const;
  // Visits every PlotDocker tab in tab order, in one pass over the tab vector
  // (dockerCount()/dockerAt() are each a linear scan, so an index loop over
  // them is quadratic). `operation` must not add or close tabs.
  void forEachDocker(const std::function<void(PlotDocker*)>& operation) const;
  // First PlotDocker tab satisfying `predicate`, or nullptr.
  [[nodiscard]] PlotDocker* findDocker(const std::function<bool(PlotDocker*)>& predicate) const;

  // Panel-toggle buttons. Created here but relocated by the MainWindow
  // shell into the title bar (it reparents them after construction).
  // The shell wires clicked() to show/hide the corresponding outer
  // panel widget and swaps the filled/outlined glyph.
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

  // Serializes/restores plot pages while always preserving widget and ephemeral
  // pages. Full scope includes history-exempt plot pages; history scope omits
  // them and preserves their live objects during restore. Restoration emits no
  // undoableChange.
  [[nodiscard]] QDomElement xmlSaveState(QDomDocument& doc, SnapshotScope scope = SnapshotScope::kFull) const;
  bool xmlLoadState(const QDomElement& tabbed_area, RestoreIntent intent = RestoreIntent::kReplace);

 public slots:
  void onStylesheetChanged(QString theme);
  // Rebinds the tab-strip Chrome metrics — the [+] tab button, the
  // three panel-toggle buttons, the strip height itself, and every
  // open tab frame. Connected to MainWindow::chromeMetricsChanged.
  // layout_spacing is ignored for now — tab frames sit flush with one
  // another by design, so introducing gaps between them would expose
  // strips of the tab-bar background.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 signals:
  void undoableChange();
  void tabAdded(PlotDocker* docker);
  // Fires after the active tab changes (user click, programmatic switch,
  // tab close, or layout restore). docker is the now-active PlotDocker or
  // nullptr if no tab is active.
  void currentTabChanged(PlotDocker* docker);

 private slots:
  void onAddTabButtonPressed();
  void onTabFrameClicked(PlotTabFrame* frame);
  void onTabRenameRequested(PlotTabFrame* frame, const QString& new_name);
  void onTabCloseRequested(PlotTabFrame* frame);

 private:
  struct TabEntry {
    PlotTabFrame* frame = nullptr;
    // Exactly one of docker / widget is set: docker for plot tabs, widget
    // (plus its owner-teardown on_close) for hosted widget tabs.
    PlotDocker* docker = nullptr;
    QWidget* widget = nullptr;
    std::function<void()> on_close;
    // Widget tabs only (see setWidgetTabPreClose): consulted first on a
    // vetoable close, and false there aborts it before anything is torn down.
    std::function<bool()> pre_close;
    // The hosted widget's size policy at addWidgetTab time — restored when
    // its tab is current; hidden widget pages are set to Ignored so they
    // never inflate the QStackedWidget's union size hint.
    QSizePolicy original_policy;
  };

  TabEntry* findEntry(PlotTabFrame* frame);
  TabEntry* findEntry(QWidget* content);
  // The entry hosting `content` as a WIDGET tab (nullptr for absent content
  // and for docker pages) — the shared predicate behind the widget-tab API.
  TabEntry* findWidgetEntry(QWidget* content);
  [[nodiscard]] const TabEntry* findWidgetEntry(QWidget* content) const;
  // Dockers an undo snapshot can reconstruct. Ephemeral and history-exempt
  // pages are outside history, so closing the last ordinary page creates a
  // fresh one even when either kind remains live.
  [[nodiscard]] int historySerializableDockerCount() const;
  // The one close path for both tab kinds. With honor_veto, a widget tab's
  // pre_close runs first and can abandon the close; plot tabs have none.
  void closeTab(PlotTabFrame* frame, bool honor_veto);
  // The stack page a tab entry shows (its docker or hosted widget).
  static QWidget* contentOf(const TabEntry& entry);
  void updateSelectionStyle();
  // Keeps hidden widget-tab pages from inflating the stack's union size
  // hint (Ignored while non-current, original policy restored when
  // current). Connected to QStackedWidget::currentChanged.
  void adaptWidgetTabPagePolicies();
  void selectEntry(const TabEntry& entry);
  PlotDocker* createDocker(const QString& tab_name);
  PlotTabFrame* createTabFrame(const QString& tab_name);

  QHBoxLayout* tabs_bar_layout_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  QWidget* tabs_inner_ = nullptr;
  QPushButton* button_add_tab_ = nullptr;
  QPushButton* button_left_panel_ = nullptr;
  QPushButton* button_bottom_panel_ = nullptr;
  QPushButton* button_right_panel_ = nullptr;
  // Tab-strip Chrome metrics — defaults match the kTabBar* constants
  // in the .cpp so first paint is unchanged until the host pushes the
  // saved Preferences values. layout_spacing is ignored: tab frames sit
  // flush by design.
  ChromeMetrics chrome_metrics_{20, 3, 0, 0};
  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  ObjectWidgetFactory object_widget_factory_;
  QString name_;
  QString state_id_;
  int tab_suffix_count_ = 0;
  bool restoring_state_ = false;
  std::vector<TabEntry> tabs_;
};

}  // namespace PJ
