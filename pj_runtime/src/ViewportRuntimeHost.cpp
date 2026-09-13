// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ViewportRuntimeHost.h"

#include <cmath>
#include <exception>
#include <utility>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ServiceRegistration.h"

namespace PJ {

namespace {
constexpr const char* kDomain = "viewport";
constexpr int32_t kErrorRejected = PJ_ERROR_CODE_REJECTED;  // bad argument / nothing to zoom
constexpr int32_t kErrorInternal = PJ_ERROR_CODE_INTERNAL;  // unexpected host-side exception at the boundary
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
  auto* self = static_cast<ViewportRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (!std::isfinite(t0_s) || !std::isfinite(t1_s) || t0_s >= t1_s) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "zoom range must be finite with t0 < t1");
      return false;
    }
    if (!self->callbacks_.zoom_to_time_range) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "this host does not support zoom");
      return false;
    }
    if (auto status = self->callbacks_.zoom_to_time_range(t0_s, t1_s); !status) {
      sdk::fillError(out_error, kErrorRejected, kDomain, status.error());
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool ViewportRuntimeHost::onZoomReset(void* ctx, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<ViewportRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (!self->callbacks_.zoom_reset) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "this host does not support zoom");
      return false;
    }
    if (auto status = self->callbacks_.zoom_reset(); !status) {
      sdk::fillError(out_error, kErrorRejected, kDomain, status.error());
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

}  // namespace PJ
