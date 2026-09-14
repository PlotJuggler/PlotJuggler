// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DataProcessorsRuntimeHost.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "RuntimeHostSlot.h"
#include "data_processor_flags.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/ServiceRegistration.h"

namespace PJ {

namespace {
using host_slot::finish;
using host_slot::guarded;
using host_slot::reject;
constexpr std::string_view kDomain{"data_processors"};

/// Accept the `kind` discriminator; returns false on an unknown kind. Only
/// "transform" is implemented here ("markers" is routed by a different host); the
/// host rejects anything else (e.g. a stale plugin still sending a removed kind).
bool parseKind(std::string_view kind) {
  return kind == "transform";
}

bool rejectKind(PJ_error_t* out_error, PJ_string_view_t kind) {
  return reject(out_error, kDomain, "unknown data processor kind '" + std::string(sdk::toStringView(kind)) + "'");
}
}  // namespace

DataProcessorsRuntimeHost::DataProcessorsRuntimeHost(DataProcessorService& service, std::string plugin_id)
    : service_(service),
      plugin_id_(std::move(plugin_id)),
      vtable_{
          .protocol_version = 1,
          .struct_size = sizeof(PJ_data_processors_host_vtable_t),
          .create_data_processor = &DataProcessorsRuntimeHost::onCreate,
          .remove_data_processor = &DataProcessorsRuntimeHost::onRemove,
          .list_data_processor_ids = &DataProcessorsRuntimeHost::onList,
          .data_processor_config = &DataProcessorsRuntimeHost::onConfig,
          .validate_data_processor_script = &DataProcessorsRuntimeHost::onValidate,
      },
      raw_{this, &vtable_} {}

Status DataProcessorsRuntimeHost::registerServices(ServiceRegistryBuilder& registry) {
  return registerRequiredService<sdk::DataProcessorsHostService>(registry, raw_);
}

bool DataProcessorsRuntimeHost::onCreate(
    void* ctx, PJ_string_view_t id, PJ_string_view_t kind, PJ_string_view_t /*language*/,
    const PJ_string_view_t* inputs, uint64_t input_count, const PJ_string_view_t* outputs, uint64_t output_count,
    PJ_string_view_t script, PJ_string_view_t params_json, uint32_t flags, PJ_string_view_t* out_topics,
    uint64_t out_topics_capacity, uint64_t* out_topics_count, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsRuntimeHost>(ctx, out_error, kDomain, [&](DataProcessorsRuntimeHost& self) {
    if (!parseKind(sdk::toStringView(kind))) {
      return rejectKind(out_error, kind);
    }
    if ((flags & ~kKnownFlags) != 0) {
      return reject(out_error, kDomain, "pj.data_processors: reserved flag bits set");
    }
    std::vector<std::string> input_names;
    input_names.reserve(input_count);
    for (uint64_t index = 0; index < input_count; ++index) {
      input_names.emplace_back(sdk::toStringView(inputs[index]));
    }
    std::vector<std::string> output_names;
    output_names.reserve(output_count);
    for (uint64_t index = 0; index < output_count; ++index) {
      output_names.emplace_back(sdk::toStringView(outputs[index]));
    }
    const bool ephemeral = (flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) != 0;
    const bool history_exempt = (flags & PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT) != 0;
    auto result = self.service_.upsertTransform(
        self.plugin_id_, sdk::toStringView(id), std::move(input_names), std::move(output_names),
        sdk::toStringView(script), sdk::toStringView(params_json), ephemeral, /*input_column_index=*/0, history_exempt);
    if (!result.has_value()) {
      return reject(out_error, kDomain, result.error());
    }
    // Resolved output topic names back to the plugin via the count-then-fill
    // convention. Owned snapshot in member storage; the returned views point into it
    // and stay valid until the next call on this vtable (the ABI contract).
    self.create_topics_storage_ = std::move(result->outputs);
    if (out_topics_count != nullptr) {
      *out_topics_count = self.create_topics_storage_.size();
    }
    if (out_topics != nullptr) {
      const uint64_t filled = std::min<uint64_t>(out_topics_capacity, self.create_topics_storage_.size());
      for (uint64_t index = 0; index < filled; ++index) {
        out_topics[index] = sdk::toAbiString(self.create_topics_storage_[index]);
      }
    }
    return true;
  });
}

bool DataProcessorsRuntimeHost::onRemove(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsRuntimeHost>(ctx, out_error, kDomain, [&](DataProcessorsRuntimeHost& self) {
    const std::string key = DataProcessorService::makeTransformKey(self.plugin_id_, sdk::toStringView(id));
    return finish(out_error, kDomain, self.service_.removeTransform(key));
  });
}

bool DataProcessorsRuntimeHost::onList(
    void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsRuntimeHost>(ctx, out_error, kDomain, [&](DataProcessorsRuntimeHost& self) {
    // Owned snapshot in member storage; the returned views point into it and stay
    // valid until the next call on this vtable (the ABI contract).
    self.list_storage_ = self.service_.transformIdsForPlugin(self.plugin_id_);
    if (out_count != nullptr) {
      *out_count = self.list_storage_.size();
    }
    const uint64_t filled = std::min<uint64_t>(capacity, self.list_storage_.size());
    for (uint64_t index = 0; index < filled; ++index) {
      out_ids[index] = sdk::toAbiString(self.list_storage_[index]);
    }
    return true;
  });
}

bool DataProcessorsRuntimeHost::onConfig(
    void* ctx, PJ_string_view_t id, PJ_string_view_t* out_recipe_json, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsRuntimeHost>(ctx, out_error, kDomain, [&](DataProcessorsRuntimeHost& self) {
    const std::string key = DataProcessorService::makeTransformKey(self.plugin_id_, sdk::toStringView(id));
    auto recipe_json = self.service_.transformRecipeJson(key);
    if (!recipe_json.has_value()) {
      return reject(out_error, kDomain, "unknown data processor id");
    }
    self.config_storage_ = std::move(*recipe_json);
    if (out_recipe_json != nullptr) {
      *out_recipe_json = sdk::toAbiString(self.config_storage_);
    }
    return true;
  });
}

bool DataProcessorsRuntimeHost::onValidate(
    void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script, PJ_string_view_t params_json,
    PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsRuntimeHost>(ctx, out_error, kDomain, [&](DataProcessorsRuntimeHost& self) {
    if (!parseKind(sdk::toStringView(kind))) {
      return rejectKind(out_error, kind);
    }
    return finish(
        out_error, kDomain,
        self.service_.validateScript(
            sdk::toStringView(script), sdk::toStringView(language), sdk::toStringView(params_json)));
  });
}

}  // namespace PJ
