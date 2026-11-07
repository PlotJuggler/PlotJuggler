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
#include "pj_base/sdk/data_processors_host.hpp"
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
          .create_data_processor_v2 = &DataProcessorsRuntimeHost::onCreateV2,
          .check_data_processor_script = &DataProcessorsRuntimeHost::onCheck,
      },
      raw_{this, &vtable_} {}

bool DataProcessorsRuntimeHost::owns(std::string_view local_id) const {
  return service_.isTransformLive(
      DataProcessorService::makeTransformKey(plugin_id_, local_id), /*include_ephemeral=*/true);
}

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
    // A persistent create returns after its first slice and replays the rest of the
    // history from the event loop; an ephemeral preview stays synchronous (its caller
    // reads the whole output right back). See upsertTransform.
    auto result = self.service_.upsertTransform(
        self.plugin_id_, sdk::toStringView(id), std::move(input_names), std::move(output_names),
        sdk::toStringView(script), sdk::toStringView(params_json), ephemeral, /*input_column_index=*/0, history_exempt);
    if (!result.has_value()) {
      return reject(out_error, kDomain, result.error());
    }
    if (ephemeral) {
      self.previews_.insert(result->key);
    }
    // Resolved output topic names back to the plugin via the count-then-fill
    // convention. Owned snapshot in member storage; the returned views point into it
    // and stay valid until the next call on this vtable (the ABI contract).
    self.create_topics_storage_ = std::move(result->outputs);
    host_slot::fillViews(self.create_topics_storage_, out_topics, out_topics_capacity, out_topics_count);
    return true;
  });
}

bool DataProcessorsRuntimeHost::onCreateV2(
    void* ctx, const PJ_data_processor_request_t* request, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
    uint64_t* out_topics_count, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsRuntimeHost>(ctx, out_error, kDomain, [&](DataProcessorsRuntimeHost&) {
    const auto decoded = sdk::readDataProcessorRequest(request, sdk::DataProcessorSlot::kCreateV2);
    if (!decoded) {
      return reject(out_error, kDomain, decoded.error());
    }
    if (const std::string_view language = sdk::toStringView(decoded->language); language != "luau") {
      return reject(
          out_error, kDomain, "kind 'transform' runs language 'luau' only, not '" + std::string(language) + "'");
    }
    if (decoded->time_flags != 0) {
      return reject(out_error, kDomain, "kind 'transform' takes no time_flags");
    }
    const auto outputs = untypedOutputNames(*decoded, "transform");
    if (!outputs) {
      return reject(out_error, kDomain, outputs.error());
    }
    return onCreate(
        ctx, decoded->id, decoded->kind, decoded->language, decoded->inputs, decoded->input_count, outputs->data(),
        outputs->size(), decoded->script, decoded->params_json, decoded->flags, out_topics, out_topics_capacity,
        out_topics_count, out_error);
  });
}

bool DataProcessorsRuntimeHost::onRemove(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsRuntimeHost>(ctx, out_error, kDomain, [&](DataProcessorsRuntimeHost& self) {
    const std::string key = DataProcessorService::makeTransformKey(self.plugin_id_, sdk::toStringView(id));
    const Status removed = self.service_.removeTransform(key);
    if (removed.has_value()) {
      self.previews_.erase(key);
    }
    return finish(out_error, kDomain, removed);
  });
}

bool DataProcessorsRuntimeHost::onList(
    void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsRuntimeHost>(ctx, out_error, kDomain, [&](DataProcessorsRuntimeHost& self) {
    // Owned snapshot in member storage; the returned views point into it and stay
    // valid until the next call on this vtable (the ABI contract). Previews are not
    // listed (the contract); `isTransformLive` answers for the persisted ones only.
    self.list_storage_.clear();
    for (std::string& id : self.service_.transformIdsForPlugin(self.plugin_id_)) {
      if (self.service_.isTransformLive(DataProcessorService::makeTransformKey(self.plugin_id_, id))) {
        self.list_storage_.push_back(std::move(id));
      }
    }
    host_slot::fillViews(self.list_storage_, out_ids, capacity, out_count);
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

bool DataProcessorsRuntimeHost::onCheck(
    void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script, PJ_string_view_t params_json,
    uint32_t* out_status, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsRuntimeHost>(ctx, out_error, kDomain, [&](DataProcessorsRuntimeHost&) {
    if (!parseKind(sdk::toStringView(kind))) {
      return rejectKind(out_error, kind);
    }
    if (Status usable = DataProcessorService::checkLanguage(sdk::toStringView(language)); !usable) {
      return reject(out_error, kDomain, usable.error());
    }
    return host_slot::checkByValidate(
        &DataProcessorsRuntimeHost::onValidate, ctx, kind, language, script, params_json, out_status, out_error);
  });
}

}  // namespace PJ
