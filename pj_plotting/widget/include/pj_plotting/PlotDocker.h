#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <DockManager.h>

#include <QDomDocument>
#include <QDomElement>
#include <QList>
#include <QPointer>
#include <QSplitter>
#include <QString>
#include <QTimer>
#include <functional>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plotting/CustomWidgetSupport.h"
#include "pj_runtime/HistoryScope.h"
#include "pj_widgets/VisualizationKind.h"

namespace PJ {

class CatalogModel;
class DockWidget;
class IDataWidget;
struct ObjectDropSeed;
class PlotFocusOverlay;
class PlotWidget;
class SessionManager;

// One tab's worth of plots: owns an ads::CDockManager and the tree of splittable DockWidgets, and always keeps at
// least one DockWidget so a tab is never empty.
class PlotDocker : public ads::CDockManager {
  Q_OBJECT
 public:
  // One factory for both paths: layout restore passes the saved XML tag as `kind` and a null seed; a catalog drop
  // passes an empty kind and a non-null seed. A null return means "not an object widget" (restore falls back to the
  // plot path).
  using ObjectWidgetFactory =
      std::function<IDataWidget*(const QString& kind, const ObjectDropSeed* seed, QWidget* parent)>;
  // Splitter -> its sizes() at an earlier moment (the fullscreen snapshot).
  using SplitterSizes = QList<std::pair<QPointer<QSplitter>, QList<int>>>;

  explicit PlotDocker(
      QString name, SessionManager* session = nullptr, CatalogModel* catalog = nullptr, QWidget* parent = nullptr);
  ~PlotDocker() override;

  QString name() const {
    return name_;
  }
  void setName(QString name) {
    name_ = std::move(name);
  }
  void setDataServices(SessionManager* session, CatalogModel* catalog);
  void setObjectWidgetFactory(ObjectWidgetFactory factory);
  /// Supplies the sidebar visibility query to existing and new docks.
  void setConfigurationPanelVisibilityQuery(std::function<bool()> query);
  /// Supplies the Custom Widget support to existing and new docks.
  void setCustomWidgetSupport(CustomWidgetSupport support);
  [[nodiscard]] QString stateId() const;
  void setStateId(QString id);
  /// A history-exempt tab persists in layout files but is omitted from undo and redo snapshots; history restores keep
  /// its live page unchanged.
  [[nodiscard]] bool isHistoryExempt() const;
  void setHistoryExempt(bool history_exempt);
  /// Whether a snapshot at `scope` writes this tab (a history-exempt one only in a full layout file).
  [[nodiscard]] bool isSerializable(SnapshotScope scope) const;
  /// Stable owner identity persisted with a plugin-composed tab. A non-empty owner marks the tab as composed by a
  /// plugin: a permanent corner watermark tells the user the view is not theirs.
  [[nodiscard]] QString ownerPlugin() const;
  [[nodiscard]] QString ownerTabId() const;
  void setOwnerMetadata(QString plugin_id, QString tab_id);
  /// The watermark text for the owner while it is loaded (the host passes the plugin's display name); empty means the
  /// owner is not in the toolbox catalog, and the watermark says so.
  [[nodiscard]] bool isOwnerPluginAvailable() const;
  void setOwnerBadge(QString badge);
  [[nodiscard]] QDomElement xmlSaveState(QDomDocument& doc) const;
  bool xmlLoadState(const QDomElement& tab_element);

  int plotCount() const;
  DockWidget* plotAt(int index);
  // The dock holding focus in this tab (the one driving the right config panel), or nullptr; tracked from
  // focusedDockWidgetChanged.
  [[nodiscard]] DockWidget* focusedDock() const;
  // Focuses `dock` so its settings show. If it is already focused ADS suppresses its focus-changed signal, so
  // dockFocused is re-announced directly (a drop populating an already-focused placeholder needs it).
  void focusDock(DockWidget* dock);

  // Maximizes `dock` to fill the tab, or restores the layout if a dock is already maximized (the argument is then
  // ignored: any maximized dock exits). Entering hides every other open dock through ADS'
  // CDockWidget::toggleView(false), which collapses the emptied splitter branches so the survivor reflows; a raw
  // CDockAreaWidget::setVisible(false) does not and strands the survivor in nested layouts. Exiting restores exactly
  // the docks this hid, leaving independently closed docks alone. The maximized dock's toolbar switches with it
  // (expand/collapse glyph, hidden close and split buttons), whichever dock the call came from.
  void toggleFullscreen(DockWidget* dock);
  // Leaves fullscreen if a dock is maximized (no-op otherwise; also drops bookkeeping left by a maximized dock that no
  // longer exists). Call it before anything that changes the dock tree (a split, a layout load): the sibling docks are
  // hidden while maximized, so the tree would change under a fold that exit can no longer undo, and a save would store
  // the folded sizes. Re-entrant: safe from a slot that runs during an exit.
  void exitFullscreen();
  // The dock maximized through toggleFullscreen, or nullptr. Out of line so the header need not see a complete
  // DockWidget for the QPointer-to-pointer conversion (as focusedDock()).
  [[nodiscard]] DockWidget* fullscreenDock() const;

 public slots:
  void onStylesheetChanged(QString theme);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 signals:
  /// Forwards a dock's request to toggle its settings sidebar.
  void settingsPanelToggleRequested(DockWidget* dock);
  void dockAdded(DockWidget* dock);
  void plotWidgetAdded(PlotWidget* plot);
  // ads::CDockManager::focusedDockWidgetChanged re-emitted as PJ::DockWidget so listeners pull in no ADS types;
  // receivers inspect dock->plotWidget() / objectWidget() for the kind. nullptr when ADS focus moves to no widget.
  void dockFocused(DockWidget* dock);
  void undoableChange();
  // DockWidget::objectFamilyRequested re-emitted, so the shell (the only scene-kind-aware module) builds and adopts an
  // empty object widget when a placeholder 2D/3D icon is clicked.
  void objectFamilyRequested(DockWidget* dock, VisualizationKind family);
  // DockWidget::firstObjectTopicAdded re-emitted, with the dock: an empty click-created object dock received its first
  // topic, so the shell can seed streaming playback and frame the view.
  void firstObjectTopicAdded(DockWidget* dock);
  // DockWidget::placeholderTopicDropped re-emitted.
  void placeholderTopicDropped(
      DockWidget* dock, DatasetId dataset_id, QString topic_name, sdk::BuiltinObjectType object_type);
  // DockWidget::topicRefused re-emitted.
  void topicRefused(const QString& reason);

 private:
  void ensureAtLeastOneWidget();
  DockWidget* addDockWithPlot(PlotWidget* plot, ads::DockWidgetArea area, ads::CDockAreaWidget* relative_to = nullptr);
  void watchPlotForHover(PlotWidget* plot);
  void watchSplitters();
  void onSplitterMoved(int position, int index);
  // Moves focus to a surviving dock after the focused one was removed, so its settings stay visible: the previously
  // focused dock, else the first remaining (after ensureAtLeastOneWidget possibly a fresh placeholder).
  void refocusAfterRemoval(DockWidget* removed);
  void refreshOwnerWatermark();

  QString state_id_;
  bool history_exempt_ = false;
  QString owner_badge_;
  QString owner_plugin_;
  QString owner_tab_id_;
  QString name_;
  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  ObjectWidgetFactory object_widget_factory_;
  std::function<bool()> configuration_panel_visible_;
  CustomWidgetSupport custom_widget_support_;
  bool restoring_state_ = false;
  // Coalesces per-mouse-move splitterMoved events into one history snapshot once the drag goes quiet.
  QTimer splitter_undo_debounce_;
  PlotFocusOverlay* focus_overlay_ = nullptr;
  // One-deep focus history from focusedDockWidgetChanged, to restore focus to the previously active dock when the
  // current one closes.
  QPointer<DockWidget> focused_dock_;
  QPointer<DockWidget> previous_dock_;

  // Fullscreen state: `fullscreen_dock_` is the maximized dock (nullptr when not); `hidden_by_fullscreen_` is the exact
  // set of docks the maximize hid, so exiting restores only those, not docks the user closed. QPointers guard against a
  // dock dying mid-state.
  QPointer<DockWidget> fullscreen_dock_;
  QList<QPointer<ads::CDockWidget>> hidden_by_fullscreen_;
  // Every splitter's sizes() captured on entering fullscreen: the maximize folds hidden branches to 0 px, and
  // xmlSaveState writes these so a layout saved while maximized stores the split the user returns to.
  SplitterSizes pre_fullscreen_sizes_;
};

}  // namespace PJ
