#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/plugin_data_api.h"

namespace PJ {

class ServiceRegistryBuilder;

/// Host-side bridge exposing the `pj.plot_tabs.v1` SDK service: create/close a
/// plugin-owned plotting tab, place and remove curves in it, and read back what
/// it holds. Composing the tab itself is shell (MainWindow) business, so this is
/// a pure callback bridge — the `ToolboxRuntimeHost::Callbacks` idiom — keeping
/// pj_runtime free of Qt6::Widgets. All vtable slots are `[main-thread]` (the
/// ABI contract); the callbacks run on the calling thread, unmarshalled.
///
/// Scoping a tab id to the plugin that created it is NOT this class's job — the
/// shell's lambdas capture the plugin identity and enforce it, exactly as
/// `ViewportRuntimeHost`'s lambdas do. This bridge only validates that an
/// argument is well-formed (non-empty id, non-empty topic/field) before handing
/// it to a callback; ownership rejection is a callback-level `Status` failure.
///
/// `list_tab_ids` and `tab_config` hand the caller borrowed `PJ_string_view_t`s
/// per the ABI contract: valid only until the next call on this vtable. The
/// bridge owns the backing storage for those borrows (see the private members)
/// and refills it on every call. Not movable: the vtable fat pointer stores
/// `this`.
class PlotTabsRuntimeHost {
 public:
  struct Callbacks {
    /// [main-thread] Create or replace a tab under `id`, holding one empty plot.
    std::function<Status(std::string_view id, std::string_view title)> create_tab;
    /// [main-thread] Close the tab under `id`.
    std::function<Status(std::string_view id)> close_tab;
    /// Ids of the tabs the shell considers this bridge's. Empty is success.
    std::function<Expected<std::vector<std::string>>()> list_tab_ids;
    /// JSON describing what the tab holds; see the ABI doc-comment on
    /// `PJ_plot_tab_host_vtable_t::tab_config`.
    std::function<Expected<std::string>(std::string_view id)> tab_config;
    std::function<Status(
        std::string_view id, std::string_view topic, std::string_view field, std::string_view dataset_source)>
        add_curve;
    std::function<Status(
        std::string_view id, std::string_view topic, std::string_view field, std::string_view dataset_source)>
        remove_curve;
    /// [main-thread] Remove every curve from the tab under `id`, keeping the tab.
    std::function<Status(std::string_view id)> clear_tab;
  };

  explicit PlotTabsRuntimeHost(Callbacks callbacks);

  PlotTabsRuntimeHost(const PlotTabsRuntimeHost&) = delete;
  PlotTabsRuntimeHost& operator=(const PlotTabsRuntimeHost&) = delete;
  PlotTabsRuntimeHost(PlotTabsRuntimeHost&&) = delete;
  PlotTabsRuntimeHost& operator=(PlotTabsRuntimeHost&&) = delete;

  /// Register the `pj.plot_tabs.v1` service into the plugin's registry.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  /// The raw C-ABI fat pointer (for direct wiring / tests).
  [[nodiscard]] PJ_plot_tab_host_t raw() const noexcept {
    return raw_;
  }

 private:
  static bool onCreateTab(void* ctx, PJ_string_view_t id, PJ_string_view_t title, PJ_error_t* out_error) noexcept;
  static bool onCloseTab(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept;
  static bool onListTabIds(
      void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* out_error) noexcept;
  static bool onTabConfig(
      void* ctx, PJ_string_view_t id, PJ_string_view_t* out_config_json, PJ_error_t* out_error) noexcept;
  static bool onAddCurve(
      void* ctx, PJ_string_view_t id, PJ_string_view_t topic, PJ_string_view_t field, PJ_string_view_t dataset_source,
      PJ_error_t* out_error) noexcept;
  static bool onRemoveCurve(
      void* ctx, PJ_string_view_t id, PJ_string_view_t topic, PJ_string_view_t field, PJ_string_view_t dataset_source,
      PJ_error_t* out_error) noexcept;
  static bool onClearTab(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept;

  Callbacks callbacks_;
  PJ_plot_tab_host_vtable_t vtable_;
  PJ_plot_tab_host_t raw_;

  /// Backing storage for `list_tab_ids`'s borrowed out-array, refilled on every
  /// call: the ABI promises the views stay valid only until the next call on
  /// this vtable, so `id_storage_` (the owned strings) must outlive `id_views_`
  /// (the `PJ_string_view_t`s pointing into it).
  std::vector<std::string> id_storage_;
  std::vector<PJ_string_view_t> id_views_;
  /// Backing storage for `tab_config`'s borrowed out-string, refilled on every
  /// call per the same borrow contract.
  std::string config_storage_;
};

}  // namespace PJ
