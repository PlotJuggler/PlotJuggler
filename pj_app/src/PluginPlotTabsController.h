#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <functional>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_runtime/PlotTabsRuntimeHost.h"
#include "pj_runtime/ViewportRuntimeHost.h"

namespace PJ {

class CatalogModel;
class ExtensionCatalogService;
class PlotDocker;
class PlotWidget;
class TabbedPlotWidget;

/// The shell half of `pj.plot_tabs.v1` and `pj.viewport.v1`: the plot tabs a
/// plugin composes in the workspace. Every operation is scoped to `plugin_id` —
/// a tab it did not compose is indistinguishable from one that does not exist,
/// so a plugin can neither reach nor reveal the user's own tabs. `tab_id` is the
/// plugin's own name for a tab; the docker's `stateId` carries the
/// host-namespaced form, which is what keeps two plugins' ids from colliding.
/// An owned tab is history-exempt (outside undo/redo) but durable in a full
/// layout file. Plain object; every call is [main-thread].
class PluginPlotTabsController {
 public:
  PluginPlotTabsController(TabbedPlotWidget& tabs, CatalogModel& catalog, ExtensionCatalogService& extensions);

  /// The bridge callbacks for one plugin. Capturing `plugin_id` here IS the
  /// boundary: the plugin never names itself across the wire.
  [[nodiscard]] PlotTabsRuntimeHost::Callbacks plotTabsCallbacks(const QString& plugin_id);
  [[nodiscard]] ViewportRuntimeHost::Callbacks viewportCallbacks(const QString& plugin_id);

  /// Upsert: re-creating an id the plugin already used starts that tab over
  /// rather than accumulating duplicates it can no longer address.
  Status createTab(const QString& plugin_id, const QString& tab_id, const QString& title);
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
  Status clearTab(const QString& plugin_id, const QString& tab_id);
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

 private:
  [[nodiscard]] QString toolboxBadge(const QString& plugin_id) const;
  void forEachPlotOwnedBy(const QString& plugin_id, const std::function<void(PlotWidget*)>& operation) const;

  TabbedPlotWidget& tabs_;
  CatalogModel& catalog_;
  ExtensionCatalogService& extensions_;
};

}  // namespace PJ
