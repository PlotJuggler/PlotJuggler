// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/PlotTabsRuntimeHost.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ServiceRegistration.h"

namespace PJ {

namespace {
constexpr const char* kDomain = "plot_tabs";
constexpr int32_t kErrorRejected = PJ_ERROR_CODE_REJECTED;  // bad argument / unsupported / callback rejection
constexpr int32_t kErrorInternal = PJ_ERROR_CODE_INTERNAL;  // unexpected host-side exception at the boundary

[[nodiscard]] bool isEmpty(PJ_string_view_t sv) {
  return sdk::toStringView(sv).empty();
}
}  // namespace

PlotTabsRuntimeHost::PlotTabsRuntimeHost(Callbacks callbacks)
    : callbacks_(std::move(callbacks)),
      vtable_{
          .protocol_version = 1,
          .struct_size = sizeof(PJ_plot_tab_host_vtable_t),
          .create_tab = &PlotTabsRuntimeHost::onCreateTab,
          .close_tab = &PlotTabsRuntimeHost::onCloseTab,
          .list_tab_ids = &PlotTabsRuntimeHost::onListTabIds,
          .tab_config = &PlotTabsRuntimeHost::onTabConfig,
          .add_curve = &PlotTabsRuntimeHost::onAddCurve,
          .remove_curve = &PlotTabsRuntimeHost::onRemoveCurve,
          .clear_tab = &PlotTabsRuntimeHost::onClearTab,
      },
      raw_{this, &vtable_} {}

Status PlotTabsRuntimeHost::registerServices(ServiceRegistryBuilder& registry) {
  return registerRequiredService<sdk::PlotTabHostService>(registry, raw_);
}

bool PlotTabsRuntimeHost::onCreateTab(
    void* ctx, PJ_string_view_t id, PJ_string_view_t title, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlotTabsRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (isEmpty(id)) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "tab id must not be empty");
      return false;
    }
    if (!self->callbacks_.create_tab) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "this host does not support creating tabs");
      return false;
    }
    if (auto status = self->callbacks_.create_tab(sdk::toStringView(id), sdk::toStringView(title)); !status) {
      sdk::fillError(out_error, kErrorRejected, kDomain, status.error());
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool PlotTabsRuntimeHost::onCloseTab(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlotTabsRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (isEmpty(id)) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "tab id must not be empty");
      return false;
    }
    if (!self->callbacks_.close_tab) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "this host does not support closing tabs");
      return false;
    }
    if (auto status = self->callbacks_.close_tab(sdk::toStringView(id)); !status) {
      sdk::fillError(out_error, kErrorRejected, kDomain, status.error());
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool PlotTabsRuntimeHost::onListTabIds(
    void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlotTabsRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (out_count == nullptr) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "out_count must not be null");
      return false;
    }
    if (!self->callbacks_.list_tab_ids) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "this host does not support listing tabs");
      return false;
    }
    auto ids = self->callbacks_.list_tab_ids();
    if (!ids) {
      sdk::fillError(out_error, kErrorRejected, kDomain, ids.error());
      return false;
    }
    self->id_storage_ = std::move(ids).value();
    self->id_views_.clear();
    self->id_views_.reserve(self->id_storage_.size());
    for (const auto& id : self->id_storage_) {
      self->id_views_.push_back(sdk::toAbiString(id));
    }
    const auto total = static_cast<uint64_t>(self->id_views_.size());
    if (out_ids == nullptr || capacity == 0) {
      *out_count = total;
      return true;
    }
    const uint64_t filled = std::min(capacity, total);
    for (uint64_t i = 0; i < filled; ++i) {
      out_ids[i] = self->id_views_[i];
    }
    *out_count = filled;
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool PlotTabsRuntimeHost::onTabConfig(
    void* ctx, PJ_string_view_t id, PJ_string_view_t* out_config_json, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlotTabsRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (isEmpty(id)) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "tab id must not be empty");
      return false;
    }
    if (out_config_json == nullptr) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "out_config_json must not be null");
      return false;
    }
    if (!self->callbacks_.tab_config) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "this host does not support reading tab config");
      return false;
    }
    auto config = self->callbacks_.tab_config(sdk::toStringView(id));
    if (!config) {
      sdk::fillError(out_error, kErrorRejected, kDomain, config.error());
      return false;
    }
    self->config_storage_ = std::move(config).value();
    *out_config_json = sdk::toAbiString(self->config_storage_);
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool PlotTabsRuntimeHost::onAddCurve(
    void* ctx, PJ_string_view_t id, PJ_string_view_t topic, PJ_string_view_t field, PJ_string_view_t dataset_source,
    PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlotTabsRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (isEmpty(id)) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "tab id must not be empty");
      return false;
    }
    if (isEmpty(topic) || isEmpty(field)) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "topic and field must not be empty");
      return false;
    }
    if (!self->callbacks_.add_curve) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "this host does not support adding curves");
      return false;
    }
    if (auto status = self->callbacks_.add_curve(
            sdk::toStringView(id), sdk::toStringView(topic), sdk::toStringView(field),
            sdk::toStringView(dataset_source));
        !status) {
      sdk::fillError(out_error, kErrorRejected, kDomain, status.error());
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool PlotTabsRuntimeHost::onRemoveCurve(
    void* ctx, PJ_string_view_t id, PJ_string_view_t topic, PJ_string_view_t field, PJ_string_view_t dataset_source,
    PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlotTabsRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (isEmpty(id)) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "tab id must not be empty");
      return false;
    }
    if (isEmpty(topic) || isEmpty(field)) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "topic and field must not be empty");
      return false;
    }
    if (!self->callbacks_.remove_curve) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "this host does not support removing curves");
      return false;
    }
    if (auto status = self->callbacks_.remove_curve(
            sdk::toStringView(id), sdk::toStringView(topic), sdk::toStringView(field),
            sdk::toStringView(dataset_source));
        !status) {
      sdk::fillError(out_error, kErrorRejected, kDomain, status.error());
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool PlotTabsRuntimeHost::onClearTab(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlotTabsRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (isEmpty(id)) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "tab id must not be empty");
      return false;
    }
    if (!self->callbacks_.clear_tab) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "this host does not support clearing tabs");
      return false;
    }
    if (auto status = self->callbacks_.clear_tab(sdk::toStringView(id)); !status) {
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
