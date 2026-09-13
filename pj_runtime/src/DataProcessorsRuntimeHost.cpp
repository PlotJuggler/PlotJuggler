// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DataProcessorsRuntimeHost.h"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "data_processor_flags.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/ServiceRegistration.h"

namespace PJ {

namespace {
constexpr const char* kDomain = "data_processors";
constexpr int32_t kErrorRejected = 1;  // the service rejected the request (bad args / not found)
constexpr int32_t kErrorInternal = 2;  // an unexpected host-side exception at the boundary

/// Accept the `kind` discriminator; returns false on an unknown kind. Only
/// "transform" is implemented here ("markers" is routed by a different host); the
/// host rejects anything else (e.g. a stale plugin still sending a removed kind).
bool parseKind(std::string_view kind) {
  return kind == "transform";
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
  auto* self = static_cast<DataProcessorsRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (!parseKind(sdk::toStringView(kind))) {
      sdk::fillError(
          out_error, kErrorRejected, kDomain,
          "unknown data processor kind '" + std::string(sdk::toStringView(kind)) + "'");
      return false;
    }
    if ((flags & ~kKnownFlags) != 0) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "pj.data_processors: reserved flag bits set");
      return false;
    }
    std::vector<std::string> input_names;
    input_names.reserve(input_count);
    for (uint64_t i = 0; i < input_count; ++i) {
      input_names.emplace_back(sdk::toStringView(inputs[i]));
    }
    std::vector<std::string> output_names;
    output_names.reserve(output_count);
    for (uint64_t i = 0; i < output_count; ++i) {
      output_names.emplace_back(sdk::toStringView(outputs[i]));
    }
    const bool ephemeral = (flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) != 0;
    const bool history_exempt = (flags & PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT) != 0;
    auto result = self->service_.upsertTransform(
        self->plugin_id_, sdk::toStringView(id), std::move(input_names), std::move(output_names),
        sdk::toStringView(script), sdk::toStringView(params_json), ephemeral, /*input_column_index=*/0, history_exempt);
    if (!result.has_value()) {
      sdk::fillError(out_error, kErrorRejected, kDomain, result.error());
      return false;
    }
    // Resolved output topic names back to the plugin via the count-then-fill
    // convention. Owned snapshot in member storage; the returned views point into it
    // and stay valid until the next call on this vtable (the ABI contract).
    self->create_topics_storage_ = std::move(result->outputs);
    if (out_topics_count != nullptr) {
      *out_topics_count = self->create_topics_storage_.size();
    }
    if (out_topics != nullptr) {
      const uint64_t n = std::min<uint64_t>(out_topics_capacity, self->create_topics_storage_.size());
      for (uint64_t i = 0; i < n; ++i) {
        out_topics[i] = sdk::toAbiString(self->create_topics_storage_[i]);
      }
    }
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  } catch (...) {
    sdk::fillError(out_error, kErrorInternal, kDomain, "unknown error creating data processor");
    return false;
  }
}

bool DataProcessorsRuntimeHost::onRemove(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataProcessorsRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    const std::string key = DataProcessorService::makeTransformKey(self->plugin_id_, sdk::toStringView(id));
    auto status = self->service_.removeTransform(key);
    if (!status.has_value()) {
      sdk::fillError(out_error, kErrorRejected, kDomain, status.error());
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  } catch (...) {
    sdk::fillError(out_error, kErrorInternal, kDomain, "unknown error removing data processor");
    return false;
  }
}

bool DataProcessorsRuntimeHost::onList(
    void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataProcessorsRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    // Owned snapshot in member storage; the returned views point into it and stay
    // valid until the next call on this vtable (the ABI contract).
    self->list_storage_ = self->service_.transformIdsForPlugin(self->plugin_id_);
    if (out_count != nullptr) {
      *out_count = self->list_storage_.size();
    }
    const uint64_t fill = std::min<uint64_t>(capacity, self->list_storage_.size());
    for (uint64_t i = 0; i < fill; ++i) {
      out_ids[i] = sdk::toAbiString(self->list_storage_[i]);
    }
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  } catch (...) {
    sdk::fillError(out_error, kErrorInternal, kDomain, "unknown error listing data processors");
    return false;
  }
}

bool DataProcessorsRuntimeHost::onConfig(
    void* ctx, PJ_string_view_t id, PJ_string_view_t* out_recipe_json, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataProcessorsRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    const std::string key = DataProcessorService::makeTransformKey(self->plugin_id_, sdk::toStringView(id));
    auto recipe_json = self->service_.transformRecipeJson(key);
    if (!recipe_json.has_value()) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "unknown data processor id");
      return false;
    }
    self->config_storage_ = std::move(*recipe_json);
    if (out_recipe_json != nullptr) {
      *out_recipe_json = sdk::toAbiString(self->config_storage_);
    }
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  } catch (...) {
    sdk::fillError(out_error, kErrorInternal, kDomain, "unknown error reading data processor config");
    return false;
  }
}

bool DataProcessorsRuntimeHost::onValidate(
    void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script, PJ_string_view_t params_json,
    PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataProcessorsRuntimeHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (!parseKind(sdk::toStringView(kind))) {
      sdk::fillError(
          out_error, kErrorRejected, kDomain,
          "unknown data processor kind '" + std::string(sdk::toStringView(kind)) + "'");
      return false;
    }
    auto status = self->service_.validateScript(
        sdk::toStringView(script), sdk::toStringView(language), sdk::toStringView(params_json));
    if (!status.has_value()) {
      sdk::fillError(out_error, kErrorRejected, kDomain, status.error());
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  } catch (...) {
    sdk::fillError(out_error, kErrorInternal, kDomain, "unknown error validating script");
    return false;
  }
}

}  // namespace PJ
