// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/PlotTabsRuntimeHost.h"

#include <algorithm>
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
// Rejected: bad argument / unsupported / callback refusal. Internal: an exception at the boundary.
constexpr std::string_view kDomain{"plot_tabs"};

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
  return guarded<PlotTabsRuntimeHost>(ctx, out_error, kDomain, [&](PlotTabsRuntimeHost& self) {
    if (isEmpty(id)) {
      return reject(out_error, kDomain, "tab id must not be empty");
    }
    if (!self.callbacks_.create_tab) {
      return reject(out_error, kDomain, "this host does not support creating tabs");
    }
    return finish(out_error, kDomain, self.callbacks_.create_tab(sdk::toStringView(id), sdk::toStringView(title)));
  });
}

bool PlotTabsRuntimeHost::onCloseTab(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept {
  return guarded<PlotTabsRuntimeHost>(ctx, out_error, kDomain, [&](PlotTabsRuntimeHost& self) {
    if (isEmpty(id)) {
      return reject(out_error, kDomain, "tab id must not be empty");
    }
    if (!self.callbacks_.close_tab) {
      return reject(out_error, kDomain, "this host does not support closing tabs");
    }
    return finish(out_error, kDomain, self.callbacks_.close_tab(sdk::toStringView(id)));
  });
}

bool PlotTabsRuntimeHost::onListTabIds(
    void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* out_error) noexcept {
  return guarded<PlotTabsRuntimeHost>(ctx, out_error, kDomain, [&](PlotTabsRuntimeHost& self) {
    if (out_count == nullptr) {
      return reject(out_error, kDomain, "out_count must not be null");
    }
    if (!self.callbacks_.list_tab_ids) {
      return reject(out_error, kDomain, "this host does not support listing tabs");
    }
    auto ids = self.callbacks_.list_tab_ids();
    if (!ids) {
      return reject(out_error, kDomain, ids.error());
    }
    self.id_storage_ = std::move(ids).value();
    self.id_views_.clear();
    self.id_views_.reserve(self.id_storage_.size());
    for (const auto& id : self.id_storage_) {
      self.id_views_.push_back(sdk::toAbiString(id));
    }
    const auto total = static_cast<uint64_t>(self.id_views_.size());
    if (out_ids == nullptr || capacity == 0) {
      *out_count = total;
      return true;
    }
    const uint64_t filled = std::min(capacity, total);
    for (uint64_t index = 0; index < filled; ++index) {
      out_ids[index] = self.id_views_[index];
    }
    *out_count = filled;
    return true;
  });
}

bool PlotTabsRuntimeHost::onTabConfig(
    void* ctx, PJ_string_view_t id, PJ_string_view_t* out_config_json, PJ_error_t* out_error) noexcept {
  return guarded<PlotTabsRuntimeHost>(ctx, out_error, kDomain, [&](PlotTabsRuntimeHost& self) {
    if (isEmpty(id)) {
      return reject(out_error, kDomain, "tab id must not be empty");
    }
    if (out_config_json == nullptr) {
      return reject(out_error, kDomain, "out_config_json must not be null");
    }
    if (!self.callbacks_.tab_config) {
      return reject(out_error, kDomain, "this host does not support reading tab config");
    }
    auto config = self.callbacks_.tab_config(sdk::toStringView(id));
    if (!config) {
      return reject(out_error, kDomain, config.error());
    }
    self.config_storage_ = std::move(config).value();
    *out_config_json = sdk::toAbiString(self.config_storage_);
    return true;
  });
}

bool PlotTabsRuntimeHost::onAddCurve(
    void* ctx, PJ_string_view_t id, PJ_string_view_t topic, PJ_string_view_t field, PJ_string_view_t dataset_source,
    PJ_error_t* out_error) noexcept {
  return guarded<PlotTabsRuntimeHost>(ctx, out_error, kDomain, [&](PlotTabsRuntimeHost& self) {
    if (isEmpty(id)) {
      return reject(out_error, kDomain, "tab id must not be empty");
    }
    if (isEmpty(topic) || isEmpty(field)) {
      return reject(out_error, kDomain, "topic and field must not be empty");
    }
    if (!self.callbacks_.add_curve) {
      return reject(out_error, kDomain, "this host does not support adding curves");
    }
    return finish(
        out_error, kDomain,
        self.callbacks_.add_curve(
            sdk::toStringView(id), sdk::toStringView(topic), sdk::toStringView(field),
            sdk::toStringView(dataset_source)));
  });
}

bool PlotTabsRuntimeHost::onRemoveCurve(
    void* ctx, PJ_string_view_t id, PJ_string_view_t topic, PJ_string_view_t field, PJ_string_view_t dataset_source,
    PJ_error_t* out_error) noexcept {
  return guarded<PlotTabsRuntimeHost>(ctx, out_error, kDomain, [&](PlotTabsRuntimeHost& self) {
    if (isEmpty(id)) {
      return reject(out_error, kDomain, "tab id must not be empty");
    }
    if (isEmpty(topic) || isEmpty(field)) {
      return reject(out_error, kDomain, "topic and field must not be empty");
    }
    if (!self.callbacks_.remove_curve) {
      return reject(out_error, kDomain, "this host does not support removing curves");
    }
    return finish(
        out_error, kDomain,
        self.callbacks_.remove_curve(
            sdk::toStringView(id), sdk::toStringView(topic), sdk::toStringView(field),
            sdk::toStringView(dataset_source)));
  });
}

bool PlotTabsRuntimeHost::onClearTab(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept {
  return guarded<PlotTabsRuntimeHost>(ctx, out_error, kDomain, [&](PlotTabsRuntimeHost& self) {
    if (isEmpty(id)) {
      return reject(out_error, kDomain, "tab id must not be empty");
    }
    if (!self.callbacks_.clear_tab) {
      return reject(out_error, kDomain, "this host does not support clearing tabs");
    }
    return finish(out_error, kDomain, self.callbacks_.clear_tab(sdk::toStringView(id)));
  });
}

}  // namespace PJ
