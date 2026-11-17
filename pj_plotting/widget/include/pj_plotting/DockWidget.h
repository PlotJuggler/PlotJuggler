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
#include "pj_plotting/CustomWidgetSupport.h"
#include "pj_runtime/IDataWidget.h"
#include "pj_widgets/VisualizationKind.h"

namespace PJ {

class CatalogModel;
class DockToolbar;
class PlotWidget;
class SessionManager;
class VisualizationPlaceholderWidget;

// ADS-backed dock hosting a single plot area. splitHorizontal / splitVertical create sibling DockWidgets inside the
// parent PlotDocker.
class DockWidget : public ads::CDockWidget, public IDataWidget {
  Q_OBJECT
 public:
  // One factory for both paths: layout restore passes a `kind` tag and a null seed (xmlLoadState then repopulates); a
  // catalog drop passes an empty kind and a non-null seed (the factory classifies and populates the first topic). A
  // null return means "not an object widget for this input".
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
  /// Queried whenever the context menu opens, for the host's live sidebar visibility.
  void setConfigurationPanelVisibilityQuery(std::function<bool()> query);
  PlotWidget* plotWidget();
  IDataWidget* objectWidget();
  PlotWidget* releasePlotWidget();
  void setPlotWidget(PlotWidget* plot);
  // Restore-path counterpart of setPlotWidget for object widgets (Scene3DDockWidget, ...): installs one the host just
  // built, then the caller runs xmlLoadState on it. The drop flow installs its own widgets through the factory.
  void setObjectWidget(IDataWidget* widget);
  IDataWidget* releaseObjectWidget();
  // Installs a host-built empty object widget into the placeholder dock (the click-to-create path): the wiring of
  // setObjectWidget plus the user-action side effects the drop path performs (undoable change, focus), and it arms the
  // streaming-seed gate so the widget's first dropped topic emits firstObjectTopicAdded. A null `widget` (factory
  // refused, unknown kind) reverts to the placeholder instead of leaving a blank dock.
  void adoptObjectWidget(IDataWidget* widget);
  void setPlaceholderWidget();
  // Retained, re-applied whenever the placeholder is recreated, and passed to docks split from this one. Available,
  // with no Custom-Widget-only type, by default.
  void setCustomWidgetSupport(CustomWidgetSupport support);
  DockToolbar* toolBar();
  QString name() const;
  void setName(const QString& name);
  [[nodiscard]] QString stateId() const;
  void setStateId(QString id);
  // Materializes this dock's plot if it has none (a new dock holds an empty placeholder); returns the existing plot
  // otherwise. The canonical placeholder-to-plot path, used by every drop that lands a curve on it.
  PlotWidget* ensurePlotWidget();

  // IDataWidget
  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double time) override;

 public slots:
  void onStylesheetChanged(QString theme);
  // Resets the dock to the empty placeholder (the toolbar's "Clear"). A slot so the shell can reset, through
  // QMetaObject::invokeMethod, a dock whose bound data was evicted.
  void clearToPlaceholder();
  DockWidget* splitHorizontal();
  DockWidget* splitVertical();
  DockWidget* splitHorizontal(PlotWidget* plot);
  DockWidget* splitVertical(PlotWidget* plot);

 signals:
  /// Toggles the host's settings sidebar for this dock.
  void settingsPanelToggleRequested(DockWidget* dock);
  void undoableChange();
  void plotWidgetCreated(PlotWidget* plot);
  // A placeholder scene icon (2D/3D) was clicked. pj_plotting stays agnostic to scene families, so the shell (the only
  // place that maps families to concrete kinds) builds the empty widget and adopts it back through adoptObjectWidget().
  // `dock` is the dock to populate.
  void objectFamilyRequested(DockWidget* dock, VisualizationKind family);
  // An empty click-created object widget received its first topic in place; the shell seeds live-streaming playback,
  // which the placeholder-to-drop path does at creation. Fires once per click-create cycle (re-armed when the dock is
  // cleared and re-adopted).
  void firstObjectTopicAdded();
  // A dropped catalog key named an advertised (not yet subscribed) placeholder, not resolved data: a scalar placeholder
  // dropped on this dock's chrome (materializing a plot lazily, like a real scalar drop), or an object placeholder
  // dropped onto an already-mounted object dock. No curve or layer is fabricated (no storage id exists yet); the shell
  // (TopicDemandController) resolves demand and a pending bind or retry. `object_type` is kNone for the scalar case.
  // Not emitted for an object placeholder with no mounted object widget: build the empty dock through
  // objectFamilyRequested first, then drop.
  void placeholderTopicDropped(
      DockWidget* dock, DatasetId dataset_id, QString topic_name, sdk::BuiltinObjectType object_type);
  // A drop carried topics this dock cannot show (isPanelOnlyObjectType, or CustomWidgetSupport::shows_only without a
  // mounted widget), which were left out; `reason` is ready for a toast.
  void topicRefused(const QString& reason);

 private slots:
  void onCatalogItemsDropped(const QStringList& dropped_keys);
  // Builds a plot and an XY (scatter) curve from a right-dragged pair.
  void onCatalogItemsXyRequested(const QStringList& keys);
  // Copies the current object widget XML state to the widget clipboard.
  void copyObjectWidgetToClipboard();
  // Replaces the current object widget state from same-family clipboard XML.
  void pasteObjectWidgetFromClipboard();
  // Initializes the placeholder from compatible widget XML in the clipboard.
  void pastePlaceholderWidgetFromClipboard();
  // Refreshes the placeholder Paste action enabled state from the clipboard.
  void updatePlaceholderPasteAction();
  // Routes a placeholder icon click: Plot is handled here (this dock owns plots); scene families go to the shell
  // through objectFamilyRequested.
  void onVisualizationRequested(VisualizationKind kind);

 private:
  bool eventFilter(QObject* watched, QEvent* event) override;
  DockWidget* splitInto(ads::DockWidgetArea area, PlotWidget* plot);
  // A discrete-only curve drop (no plottable key) on an empty tile materializes the state-transitions strip through the
  // factory and seeds it through IDataWidget::tryAcceptSeriesKeys.
  void maybeCreateStateTransitionsFromDrop(const QStringList& keys);
  void clearCurrentContent(bool delete_content);
  void installObjectContextMenuFilter(QWidget* root);
  void removeObjectContextMenuFilter(QWidget* root);
  void showObjectContextMenu(const QPoint& global_pos);
  // The current object widget's XML root tag.
  [[nodiscard]] QString objectWidgetClipboardTag() const;
  // True when clipboard XML matches the current object widget family.
  [[nodiscard]] bool canPasteObjectWidgetFromClipboard() const;
  // True when clipboard XML can initialize this placeholder as a supported widget.
  [[nodiscard]] bool canPastePlaceholderWidgetFromClipboard() const;
  // Focuses this dock after a drop so its settings show at once (no-op without a focus controller).
  void focusSelf();

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  ObjectWidgetFactory object_widget_factory_;
  std::function<bool()> configuration_panel_visible_;
  QWidget* content_widget_ = nullptr;
  VisualizationPlaceholderWidget* placeholder_widget_ = nullptr;
  CustomWidgetSupport custom_widget_support_;
  PlotWidget* plot_widget_ = nullptr;
  IDataWidget* object_widget_ = nullptr;
  // True between adoptObjectWidget() (empty click-created object widget) and its first absorbed topic; gates the
  // one-shot firstObjectTopicAdded. Reset by clearCurrentContent so a replaced widget never carries it. Not set on the
  // factory/drop path (onCatalogItemsDropped), which already seeds streaming at creation: arming it there would fire
  // twice.
  bool object_widget_awaiting_first_topic_ = false;
  // Re-entrancy guard for the ContextMenu case of eventFilter: while the dock forwards the event to the content
  // widget's own handler (the 3D view's frame-gizmo menu), the filter must pass it through, not intercept it again.
  bool forwarding_context_menu_ = false;
  DockToolbar* toolbar_ = nullptr;
  QString state_id_;
};

}  // namespace PJ
