#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <DockManager.h>

#include <QDomDocument>
#include <QDomElement>
#include <QList>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <functional>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
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

// One tab's worth of plots. Owns an ads::CDockManager and the tree of
// DockWidgets splittable within it. Always keeps at least one DockWidget
// alive so the user never sees an empty tab.
class PlotDocker : public ads::CDockManager {
  Q_OBJECT
 public:
  // One factory for both paths. Layout restore calls it with the saved XML tag
  // as `kind` and a null seed; a catalog drop calls it with an empty kind and a
  // non-null seed. Null return = "not an object widget" (restore falls back to
  // the plot path).
  using ObjectWidgetFactory =
      std::function<IDataWidget*(const QString& kind, const ObjectDropSeed* seed, QWidget* parent)>;

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
  [[nodiscard]] QString stateId() const;
  void setStateId(QString id);
  /// A history-exempt tab persists in layout files but is omitted from undo and
  /// redo snapshots. History restores preserve its live page unchanged.
  [[nodiscard]] bool isHistoryExempt() const;
  void setHistoryExempt(bool history_exempt);
  /// Whether a snapshot at `scope` writes this tab: a history-exempt one only
  /// in a full layout file.
  [[nodiscard]] bool isSerializable(SnapshotScope scope) const;
  /// Stable owner identity persisted with a plugin-composed tab. Setting a
  /// non-empty owner marks the tab as composed by a plugin: a permanent corner
  /// watermark the user reads as "this view is not mine".
  [[nodiscard]] QString ownerPlugin() const;
  [[nodiscard]] QString ownerTabId() const;
  void setOwnerMetadata(QString plugin_id, QString tab_id);
  /// The watermark text for the owner while it is loaded (the host passes the
  /// plugin's display name); empty means the owner is not present in the
  /// toolbox catalog, and the watermark says so instead.
  [[nodiscard]] bool isOwnerPluginAvailable() const;
  void setOwnerBadge(QString badge);
  [[nodiscard]] QDomElement xmlSaveState(QDomDocument& doc) const;
  bool xmlLoadState(const QDomElement& tab_element);

  int plotCount() const;
  DockWidget* plotAt(int index);
  // The dock that currently holds focus in this tab (the one driving the right
  // config panel), or nullptr if none. Tracked from focusedDockWidgetChanged.
  [[nodiscard]] DockWidget* focusedDock() const;
  // Focus `dock` so its settings show. If it is already the focused dock, ADS
  // suppresses its focus-changed signal — so re-announce dockFocused directly,
  // which is what a drop populating an already-focused placeholder needs.
  void focusDock(DockWidget* dock);

  // Maximize `dock` so it fills the whole tab, or restore the layout if a dock
  // is already maximized (the `dock` argument is then ignored — any maximized
  // dock exits). Entering hides every other *currently open* dock via ADS'
  // native CDockWidget::toggleView(false), which collapses the emptied splitter
  // branches so the survivor reflows to fill; a raw CDockAreaWidget::setVisible
  // (false) does not, stranding the survivor in nested layouts. Exiting restores
  // exactly the docks this hid, leaving any independently-closed docks untouched.
  void toggleFullscreen(DockWidget* dock);
  // The dock currently maximized via toggleFullscreen, or nullptr if none.
  // Out-of-line so the header need not see a complete DockWidget for the
  // QPointer-to-pointer conversion (matches focusedDock()).
  [[nodiscard]] DockWidget* fullscreenDock() const;

 public slots:
  void onStylesheetChanged(QString theme);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 signals:
  void dockAdded(DockWidget* dock);
  void plotWidgetAdded(PlotWidget* plot);
  // Re-emit of ads::CDockManager::focusedDockWidgetChanged narrowed to
  // PJ::DockWidget so listeners don't pull in ADS types. Receivers
  // inspect dock->plotWidget()/dock->objectWidget() to know the kind.
  // nullptr when ADS focus moves to no widget.
  void dockFocused(DockWidget* dock);
  void undoableChange();
  // Re-emit of DockWidget::objectFamilyRequested, so the shell (the only
  // scene-kind-aware module) can build + adopt an empty object widget when a
  // placeholder 2D/3D icon is clicked.
  void objectFamilyRequested(DockWidget* dock, VisualizationKind family);
  // Re-emit of DockWidget::firstObjectTopicAdded — an empty click-created object
  // dock received its first topic, so the shell can seed streaming playback.
  void firstObjectTopicAdded();
  // Re-emit of DockWidget::placeholderTopicDropped — see that signal's doc.
  void placeholderTopicDropped(
      DockWidget* dock, DatasetId dataset_id, QString topic_name, sdk::BuiltinObjectType object_type);

 private:
  void ensureAtLeastOneWidget();
  DockWidget* addDockWithPlot(PlotWidget* plot, ads::DockWidgetArea area, ads::CDockAreaWidget* relative_to = nullptr);
  void watchPlotForHover(PlotWidget* plot);
  void watchSplitters();
  void onSplitterMoved(int position, int index);
  // Move focus to a surviving dock after the focused one was removed, so its
  // settings stay visible. Prefers the previously focused dock; otherwise the
  // first remaining dock (which, after ensureAtLeastOneWidget, may be a fresh
  // placeholder when the last real widget was closed).
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
  bool restoring_state_ = false;
  // Coalesces per-mouse-move splitterMoved events into one history snapshot
  // after the drag goes quiet.
  QTimer splitter_undo_debounce_;
  PlotFocusOverlay* focus_overlay_ = nullptr;
  // One-deep focus history, maintained from focusedDockWidgetChanged. Used to
  // restore focus to the previously active dock when the current one closes.
  QPointer<DockWidget> focused_dock_;
  QPointer<DockWidget> previous_dock_;

  // Fullscreen (maximize-one-dock) state. `fullscreen_dock_` is the maximized
  // dock (nullptr when not fullscreen); `hidden_by_fullscreen_` is the exact set
  // of docks the maximize hid, so exiting restores only those — not docks the
  // user closed independently. QPointers guard against a dock dying mid-state.
  QPointer<DockWidget> fullscreen_dock_;
  QList<QPointer<ads::CDockWidget>> hidden_by_fullscreen_;
};

}  // namespace PJ
