#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <functional>

#include "pj_base/expected.hpp"
#include "pj_base/plugin_data_api.h"

namespace PJ {

class ServiceRegistryBuilder;

/// Host-side bridge exposing the `pj.viewport.v1` SDK service: zoom the
/// caller's owned time-series plots to a display-seconds X window, or reset
/// them to fit; plots the plugin does not own are never touched.
/// The zoom itself is shell (MainWindow) business, so this is a pure callback
/// bridge — the `ToolboxRuntimeHost::Callbacks` idiom — keeping pj_runtime free
/// of Qt6::Widgets. All vtable slots are `[main-thread]` (the ABI contract);
/// the callbacks run on the calling thread, unmarshalled.
///
/// The bridge validates arguments (finite, t0 < t1) BEFORE invoking a callback,
/// so shell code never sees garbage ranges. Not movable: the vtable fat pointer
/// stores `this`.
class ViewportRuntimeHost {
 public:
  struct Callbacks {
    /// [main-thread] Apply [t0_s, t1_s] (display-axis seconds) as the visible X
    /// window of the caller's owned time-series plots. Error = nothing was zoomable.
    std::function<Status(double t0_s, double t1_s)> zoom_to_time_range;
    /// [main-thread] Reset the caller's owned plots to fit their data.
    std::function<Status()> zoom_reset;
  };

  explicit ViewportRuntimeHost(Callbacks callbacks);

  ViewportRuntimeHost(const ViewportRuntimeHost&) = delete;
  ViewportRuntimeHost& operator=(const ViewportRuntimeHost&) = delete;
  ViewportRuntimeHost(ViewportRuntimeHost&&) = delete;
  ViewportRuntimeHost& operator=(ViewportRuntimeHost&&) = delete;

  /// Register the `pj.viewport.v1` service into the plugin's registry.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  /// The raw C-ABI fat pointer (for direct wiring / tests).
  [[nodiscard]] PJ_viewport_host_t raw() const noexcept {
    return raw_;
  }

 private:
  static bool onZoomToTimeRange(void* ctx, double t0_s, double t1_s, PJ_error_t* out_error) noexcept;
  static bool onZoomReset(void* ctx, PJ_error_t* out_error) noexcept;

  Callbacks callbacks_;
  PJ_viewport_host_vtable_t vtable_;
  PJ_viewport_host_t raw_;
};

}  // namespace PJ
