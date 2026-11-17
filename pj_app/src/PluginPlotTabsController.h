#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <functional>
#include <string>
#include <vector>

#include "PluginSceneViewsController.h"
#include "pj_base/expected.hpp"
#include "pj_runtime/PlotTabsRuntimeHost.h"
#include "pj_runtime/ViewportRuntimeHost.h"

namespace PJ {

class CatalogModel;
class ExtensionCatalogService;
class PlotDocker;
class PlotWidget;
class SessionManager;
class TabbedPlotWidget;

/// The shell half of `pj.plot_tabs.v1` and `pj.viewport.v1`: the plot and scene ("3d"/"2d") tabs a plugin
/// composes, one id namespace per plugin across kinds. Every operation is scoped to `plugin_id`: a tab it did not
/// compose is indistinguishable from one that does not exist, so a plugin can neither reach nor reveal the user's
/// tabs. The docker's `stateId` carries the host-namespaced `tab_id`, which keeps two plugins' ids apart.
/// An owned tab is outside undo/redo but saved in a full layout file. Main thread only.
class PluginPlotTabsController {
 public:
  /// `make_dock` builds the empty widget for "scene3d" / "scene2d", or null when this build has none: a scene
  /// tab kind is served exactly when it returns a widget.
  PluginPlotTabsController(
      TabbedPlotWidget& tabs, CatalogModel& catalog, SessionManager& session, ExtensionCatalogService& extensions,
      PluginSceneViewsController::MakeDock make_dock);

  /// Once per tab a plugin creates (not re-created under the same kind, attached or focused), after setup, so the
  /// shell can reveal it when the plugin's panel covers the tab area.
  using OwnedTabCreatedHook = std::function<void(const QString& plugin_id, PlotDocker* docker)>;
  void setOwnedTabCreatedHook(OwnedTabCreatedHook hook) {
    owned_tab_created_ = std::move(hook);
  }

  /// Capturing `plugin_id` here IS the boundary: the plugin never names itself across the wire.
  [[nodiscard]] PlotTabsRuntimeHost::Callbacks plotTabsCallbacks(const QString& plugin_id);
  [[nodiscard]] ViewportRuntimeHost::Callbacks viewportCallbacks(const QString& plugin_id);

  /// `createTabOfKind(..., "plot", ...)`.
  Status createTab(const QString& plugin_id, const QString& tab_id, const QString& title);
  /// `kind` is "plot", "3d" or "2d". Upsert by id: the same kind clears a plot tab's
  /// curves but only retitles a scene tab (its topics stay); another kind closes the
  /// old tab and creates an empty one. A scene kind this build cannot show is refused
  /// naming it, before anything is replaced.
  Status createTabOfKind(const QString& plugin_id, const QString& tab_id, const QString& kind, const QString& title);
  Status closeTab(const QString& plugin_id, const QString& tab_id);
  /// Ids in tab order, so the caller sees them the way the user does.
  [[nodiscard]] Expected<std::vector<std::string>> listTabIds(const QString& plugin_id) const;
  /// The ABI's JSON, read back from the live plot, never echoed from the request.
  [[nodiscard]] Expected<std::string> tabConfig(const QString& plugin_id, const QString& tab_id) const;
  /// Drawing a curve already drawn is success, not an error.
  Status addCurve(
      const QString& plugin_id, const QString& tab_id, const QString& topic, const QString& field,
      const QString& dataset_source);
  /// Removing a curve that was never drawn is an error: a mistake worth seeing.
  Status removeCurve(
      const QString& plugin_id, const QString& tab_id, const QString& topic, const QString& field,
      const QString& dataset_source);
  /// On a plot tab removes every curve; on a scene tab detaches every topic.
  Status clearTab(const QString& plugin_id, const QString& tab_id);
  /// Scene tabs only; on a plot tab, an error naming add_curve / remove_curve.
  Status attachTopic(
      const QString& plugin_id, const QString& tab_id, const QString& topic, const QString& dataset_source);
  Status detachTopic(
      const QString& plugin_id, const QString& tab_id, const QString& topic, const QString& dataset_source);
  Status focusTab(const QString& plugin_id, const QString& tab_id);
  /// Null when `plugin_id` owns no such tab. The single ownership check every operation goes through.
  [[nodiscard]] PlotDocker* ownedTab(const QString& plugin_id, const QString& tab_id) const;

  /// Sets the X window of every time plot `plugin_id` owns (or only in `tab_id`) to [t0_s, t1_s], display-axis
  /// seconds, keeping Y and ignoring Link-X. Owning no such tab and owning only tabs with nothing to zoom are
  /// distinct errors: the caller's remedy differs.
  Status zoomToTimeRange(const QString& plugin_id, double t0_s, double t1_s, const QString& tab_id = {});
  /// Reset every plot `plugin_id` owns, or only those in its tab `tab_id` when given, to fit its data.
  Status zoomOut(const QString& plugin_id, const QString& tab_id = {});

  /// Re-stamps every owned tab's watermark (owner name, or "not loaded"). Call after the catalog changes and
  /// after a layout load re-creates tabs.
  void refreshAvailability();

  /// The scene half, shared with the views plugin panels embed (MainWindow::makeEmbeddedViewHost).
  [[nodiscard]] PluginSceneViewsController& sceneViews() {
    return scene_views_;
  }

 private:
  /// Null when the workspace refuses a new tab.
  [[nodiscard]] PlotDocker* newOwnedTab(const QString& plugin_id, const QString& tab_id, const QString& title);
  static void clearPlotCurves(PlotDocker* docker);
  void forEachPlotOwnedBy(
      const QString& plugin_id, const QString& tab_id, const std::function<void(PlotWidget*)>& operation) const;

  TabbedPlotWidget& tabs_;
  CatalogModel& catalog_;
  ExtensionCatalogService& extensions_;
  PluginSceneViewsController scene_views_;
  OwnedTabCreatedHook owned_tab_created_;
};

}  // namespace PJ
