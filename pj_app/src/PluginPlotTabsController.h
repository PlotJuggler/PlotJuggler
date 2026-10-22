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

/// The shell half of `pj.plot_tabs.v1` and `pj.viewport.v1`: the plot and scene
/// ("3d"/"2d") tabs a plugin composes in the workspace, one id namespace per plugin
/// across kinds. Every operation is scoped to `plugin_id` — a tab it did not compose
/// is indistinguishable from one that does not exist, so a plugin can neither reach
/// nor reveal the user's own tabs. `tab_id` is the plugin's own name for a tab; the
/// docker's `stateId` carries the host-namespaced form, which is what keeps two
/// plugins' ids from colliding.
/// An owned tab is history-exempt (outside undo/redo) but durable in a full
/// layout file. Plain object; every call is [main-thread].
class PluginPlotTabsController {
 public:
  /// `make_dock` builds the seeded-empty object widget for a scene widget kind
  /// ("scene3d" / "scene2d"), or null when this build has none: a scene tab kind
  /// is served exactly when it returns a widget.
  PluginPlotTabsController(
      TabbedPlotWidget& tabs, CatalogModel& catalog, SessionManager& session, ExtensionCatalogService& extensions,
      PluginSceneViewsController::MakeDock make_dock);

  /// Called once per tab a plugin CREATES (never for one it re-creates under the
  /// same kind, attaches to or focuses), after the tab is set up, so the shell can
  /// reveal it when the plugin's own panel covers the tab area.
  using OwnedTabCreatedHook = std::function<void(const QString& plugin_id, PlotDocker* docker)>;
  void setOwnedTabCreatedHook(OwnedTabCreatedHook hook) {
    owned_tab_created_ = std::move(hook);
  }

  /// The bridge callbacks for one plugin. Capturing `plugin_id` here IS the
  /// boundary: the plugin never names itself across the wire.
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
  /// What the tab actually holds, as the JSON the ABI specifies — read back from
  /// the live plot, never echoed from what the caller asked for.
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
  /// Bring a tab of either kind to the front.
  Status focusTab(const QString& plugin_id, const QString& tab_id);
  /// The docker `plugin_id` composed under `tab_id`, or null when it owns no
  /// such tab. The single ownership check every operation goes through.
  [[nodiscard]] PlotDocker* ownedTab(const QString& plugin_id, const QString& tab_id) const;

  /// Set the visible X window of every time plot `plugin_id` owns to [t0_s, t1_s]
  /// (display-axis seconds), keeping each plot's Y range and ignoring the
  /// toolbar's Link-X toggle. Owning no tab and owning only tabs with nothing to
  /// zoom are distinct errors: the caller's remedy differs.
  Status zoomToTimeRange(const QString& plugin_id, double t0_s, double t1_s);
  /// Reset every plot `plugin_id` owns to fit its data.
  Status zoomOut(const QString& plugin_id);

  /// Re-stamp every owned tab's watermark from the extension catalog: the owning
  /// toolbox's display name while loaded, and the "not loaded" form otherwise.
  /// Call after the catalog changes and after a layout load re-creates tabs.
  void refreshAvailability();

  /// The scene half, shared with the views plugin panels embed (MainWindow::makeEmbeddedViewHost).
  [[nodiscard]] PluginSceneViewsController& sceneViews() {
    return scene_views_;
  }

 private:
  /// A fresh owned tab: namespaced stateId, owner metadata, history exemption and
  /// badge. Null when the workspace refuses a new tab.
  [[nodiscard]] PlotDocker* newOwnedTab(const QString& plugin_id, const QString& tab_id, const QString& title);
  static void clearPlotCurves(PlotDocker* docker);
  void forEachPlotOwnedBy(const QString& plugin_id, const std::function<void(PlotWidget*)>& operation) const;

  TabbedPlotWidget& tabs_;
  CatalogModel& catalog_;
  ExtensionCatalogService& extensions_;
  PluginSceneViewsController scene_views_;
  OwnedTabCreatedHook owned_tab_created_;
};

}  // namespace PJ
