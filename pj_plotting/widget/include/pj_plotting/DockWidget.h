#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <DockWidget.h>

#include <QEvent>
#include <QPoint>
#include <QStringList>
#include <functional>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/IDataWidget.h"
#include "pj_widgets/VisualizationKind.h"

namespace PJ {

class CatalogModel;
class DockToolbar;
class PlotWidget;
class SessionManager;
class VisualizationPlaceholderWidget;

// ADS-backed dock hosting a single plot area. splitHorizontal /
// splitVertical create sibling DockWidgets inside the parent PlotDocker.
class DockWidget : public ads::CDockWidget, public IDataWidget {
  Q_OBJECT
 public:
  // One factory for both paths: layout restore calls it with a `kind` tag and a
  // null seed (then xmlLoadState repopulates); a catalog drop calls it with an
  // empty kind and a non-null seed (the factory classifies + populates the first
  // topic). A null return means "not an object widget for this input".
  using ObjectWidgetFactory =
      std::function<IDataWidget*(const QString& kind, const ObjectDropSeed* seed, QWidget* parent)>;

  explicit DockWidget(
      SessionManager* session = nullptr, CatalogModel* catalog = nullptr, ads::CDockManager* manager = nullptr,
      QWidget* parent = nullptr);
  explicit DockWidget(
      PlotWidget* plot, SessionManager* session = nullptr, CatalogModel* catalog = nullptr,
      ads::CDockManager* manager = nullptr, QWidget* parent = nullptr, bool create_plot_when_null = false);
  ~DockWidget() override;

  void setDataServices(SessionManager* session, CatalogModel* catalog);
  void setObjectWidgetFactory(ObjectWidgetFactory factory);
  PlotWidget* plotWidget();
  IDataWidget* objectWidget();
  PlotWidget* releasePlotWidget();
  void setPlotWidget(PlotWidget* plot);
  // Restore-path counterpart of setPlotWidget for object widgets
  // (Scene3DDockWidget, …). The drop flow keeps installing its own object
  // widgets via the factory; this hook lets layout restore install one
  // it just constructed and then call xmlLoadState on it.
  void setObjectWidget(IDataWidget* widget);
  IDataWidget* releaseObjectWidget();
  // Installs a host-built *empty* object widget into the placeholder dock (the
  // click-to-create path): same wiring as setObjectWidget plus the user-action
  // side effects (undoable change + focus) the drop path performs, and it arms
  // the streaming-seed gate so the widget's first dropped topic emits
  // firstObjectTopicAdded. A null `widget` (factory refused / unknown kind)
  // reverts to the placeholder rather than leaving a blank dock.
  void adoptObjectWidget(IDataWidget* widget);
  void setPlaceholderWidget();
  DockToolbar* toolBar();
  QString name() const;
  void setName(const QString& name);
  [[nodiscard]] QString stateId() const;
  void setStateId(QString id);
  // Materializes this dock's plot if it doesn't have one yet (a freshly
  // created dock holds an empty placeholder). Returns the existing plot
  // unchanged if already populated. The canonical way to turn a placeholder
  // into a live plot; used by every drop path that lands a curve on it.
  PlotWidget* ensurePlotWidget();

  // IDataWidget
  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double time) override;

 public slots:
  void onStylesheetChanged(QString theme);
  // Resets the dock to the empty placeholder (same as the toolbar's "Clear").
  // A slot so the shell (via QMetaObject::invokeMethod) can reset a dock whose
  // bound data was evicted.
  void clearToPlaceholder();
  DockWidget* splitHorizontal();
  DockWidget* splitVertical();
  DockWidget* splitHorizontal(PlotWidget* plot);
  DockWidget* splitVertical(PlotWidget* plot);

 signals:
  void undoableChange();
  void plotWidgetCreated(PlotWidget* plot);
  // A placeholder scene icon (2D/3D) was clicked. pj_plotting stays agnostic to
  // scene families, so it asks the shell (which alone maps families to concrete
  // "scene2d"/"scene3d" kinds) to build the empty widget and adopt it back via
  // adoptObjectWidget(). `dock` is the dock to populate.
  void objectFamilyRequested(DockWidget* dock, VisualizationKind family);
  // An empty (click-created) object widget just received its FIRST topic in
  // place. The shell uses this to seed live-streaming playback, which the
  // placeholder→drop path otherwise does at creation time. Fires once per
  // click-create cycle (the gate is re-armed if the dock is cleared and re-adopted).
  void firstObjectTopicAdded();
  // A dropped catalog key named an ADVERTISED (not-yet-subscribed) placeholder
  // rather than resolved data — a scalar placeholder dropped on this dock's
  // chrome (materializing a plot lazily, like a real scalar drop) or an object
  // placeholder dropped onto an ALREADY-mounted object dock. This dock does not
  // fabricate a curve/layer for these (no storage id exists yet); the shell
  // (TopicDemandController) resolves demand + a pending bind/retry.
  // `object_type` is kNone for the scalar case. Not emitted for an object
  // placeholder with no already-mounted object widget — materializing a fresh
  // dock straight from a placeholder is out of scope; build the empty dock via
  // objectFamilyRequested first, then drop.
  void placeholderTopicDropped(
      DockWidget* dock, DatasetId dataset_id, QString topic_name, sdk::BuiltinObjectType object_type);

 private slots:
  void onCatalogItemsDropped(const QStringList& keys);
  // Builds a plot and creates an XY (scatter) curve from a right-dragged pair.
  void onCatalogItemsXyRequested(const QStringList& keys);
  // Copies the current object widget XML state to the widget clipboard.
  void copyObjectWidgetToClipboard();
  // Replaces the current object widget state from same-family clipboard XML.
  void pasteObjectWidgetFromClipboard();
  // Initializes the placeholder from compatible widget XML in the clipboard.
  void pastePlaceholderWidgetFromClipboard();
  // Refreshes the placeholder Paste action enabled state from the clipboard.
  void updatePlaceholderPasteAction();
  // Routes a placeholder icon click: Plot is handled here (this dock owns plots);
  // scene families are forwarded via objectFamilyRequested for the shell to build.
  void onVisualizationRequested(VisualizationKind kind);

 private:
  bool eventFilter(QObject* watched, QEvent* event) override;
  DockWidget* splitInto(ads::DockWidgetArea area, PlotWidget* plot);
  // A discrete-only curve drop (no plottable key) on an empty tile materializes
  // the state-transitions strip via the factory and seeds it through
  // IDataWidget::tryAcceptSeriesKeys.
  void maybeCreateStateTransitionsFromDrop(const QStringList& keys);
  void clearCurrentContent(bool delete_content);
  void installObjectContextMenuFilter(QWidget* root);
  void removeObjectContextMenuFilter(QWidget* root);
  void showObjectContextMenu(const QPoint& global_pos);
  // Returns the current object widget's XML root tag.
  [[nodiscard]] QString objectWidgetClipboardTag() const;
  // True when clipboard XML matches the current object widget family.
  [[nodiscard]] bool canPasteObjectWidgetFromClipboard() const;
  // True when clipboard XML can initialize this placeholder as a supported widget.
  [[nodiscard]] bool canPastePlaceholderWidgetFromClipboard() const;
  // Make this dock the focused one after it receives a drop, so its settings
  // become visible immediately (no-op if the manager has no focus controller).
  void focusSelf();

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  ObjectWidgetFactory object_widget_factory_;
  QWidget* content_widget_ = nullptr;
  VisualizationPlaceholderWidget* placeholder_widget_ = nullptr;
  PlotWidget* plot_widget_ = nullptr;
  IDataWidget* object_widget_ = nullptr;
  // True between adoptObjectWidget() (empty click-created object widget) and its
  // first absorbed topic; gates the one-shot firstObjectTopicAdded emission.
  // Reset by clearCurrentContent so a replaced/cleared widget never carries it.
  // Deliberately NOT set on the factory/drop path (onCatalogItemsDropped), which
  // already seeds streaming at creation — arming it there would double-fire.
  bool object_widget_awaiting_first_topic_ = false;
  // Re-entrancy guard for the ContextMenu case of eventFilter: while the dock
  // forwards the event to the content widget's own handler (first shot — e.g.
  // the 3D view's frame-gizmo menu), the filter must pass it through instead of
  // intercepting it again.
  bool forwarding_context_menu_ = false;
  DockToolbar* toolbar_ = nullptr;
  QString state_id_;
};

}  // namespace PJ
