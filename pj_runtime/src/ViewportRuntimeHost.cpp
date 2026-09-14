// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ViewportRuntimeHost.h"

#include <cmath>
#include <string_view>
#include <utility>

#include "RuntimeHostSlot.h"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ServiceRegistration.h"

namespace PJ {

namespace {
using host_slot::finish;
using host_slot::guarded;
using host_slot::reject;
// Rejected: bad argument / nothing to zoom. Internal: an exception at the boundary.
constexpr std::string_view kDomain{"viewport"};
}  // namespace

ViewportRuntimeHost::ViewportRuntimeHost(Callbacks callbacks)
    : callbacks_(std::move(callbacks)),
      vtable_{
          .protocol_version = 1,
          .struct_size = sizeof(PJ_viewport_host_vtable_t),
          .zoom_to_time_range = &ViewportRuntimeHost::onZoomToTimeRange,
          .zoom_reset = &ViewportRuntimeHost::onZoomReset,
      },
      raw_{this, &vtable_} {}

Status ViewportRuntimeHost::registerServices(ServiceRegistryBuilder& registry) {
  return registerRequiredService<sdk::ViewportHostService>(registry, raw_);
}

bool ViewportRuntimeHost::onZoomToTimeRange(void* ctx, double t0_s, double t1_s, PJ_error_t* out_error) noexcept {
  return guarded<ViewportRuntimeHost>(ctx, out_error, kDomain, [&](ViewportRuntimeHost& self) {
    if (!std::isfinite(t0_s) || !std::isfinite(t1_s) || t0_s >= t1_s) {
      return reject(out_error, kDomain, "zoom range must be finite with t0 < t1");
    }
    if (!self.callbacks_.zoom_to_time_range) {
      return reject(out_error, kDomain, "this host does not support zoom");
    }
    return finish(out_error, kDomain, self.callbacks_.zoom_to_time_range(t0_s, t1_s));
  });
}

bool ViewportRuntimeHost::onZoomReset(void* ctx, PJ_error_t* out_error) noexcept {
  return guarded<ViewportRuntimeHost>(ctx, out_error, kDomain, [&](ViewportRuntimeHost& self) {
    if (!self.callbacks_.zoom_reset) {
      return reject(out_error, kDomain, "this host does not support zoom");
    }
    return finish(out_error, kDomain, self.callbacks_.zoom_reset());
  });
}

}  // namespace PJ
