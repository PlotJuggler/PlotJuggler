// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/MarkersRuntimeHost.h"

#include <algorithm>
#include <exception>
#include <nlohmann/json.hpp>
#include <utility>

#include "data_processor_flags.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"  // sdk::toStringView / toAbiString / fillError
#include "pj_base/sdk/service_traits.hpp"   // sdk::DataProcessorsHostService
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ServiceRegistration.h"

namespace PJ {

namespace {
constexpr const char* kDomain = "pj.data_processors.v1";

/// Parse the `kind` discriminator; returns false on an unknown kind. Only "markers"
/// is implemented here ("transform" is routed by a different host); the host rejects
/// anything else (e.g. a stale plugin still sending the removed "field" kind).
bool parseKind(std::string_view kind, GeneratorKind& out) {
  if (kind == "markers") {
    out = GeneratorKind::kMarkers;
    return true;
  }
  return false;
}
}  // namespace

MarkersRuntimeHost::MarkersRuntimeHost(
    MarkerService& service, std::string plugin_id,
    std::function<DatasetId(const std::vector<std::string>& inputs)> active_dataset)
    : service_(service), plugin_id_(std::move(plugin_id)), active_dataset_(std::move(active_dataset)) {
  vtable_ = PJ_data_processors_host_vtable_t{
      .protocol_version = 1,
      .struct_size = sizeof(PJ_data_processors_host_vtable_t),
      .create_data_processor = &MarkersRuntimeHost::onCreate,
      .remove_data_processor = &MarkersRuntimeHost::onRemove,
      .list_data_processor_ids = &MarkersRuntimeHost::onList,
      .data_processor_config = &MarkersRuntimeHost::onConfig,
      .validate_data_processor_script = &MarkersRuntimeHost::onValidate,
  };
  data_processors_ = PJ_data_processors_host_t{.ctx = this, .vtable = &vtable_};
}

Status MarkersRuntimeHost::registerServices(ServiceRegistryBuilder& registry) {
  return registerRequiredService<sdk::DataProcessorsHostService>(registry, data_processors_);
}

std::string MarkersRuntimeHost::makeKey(std::string_view local_id) const {
  std::string key = plugin_id_;
  key.push_back('/');
  key.append(local_id);
  return key;
}

bool MarkersRuntimeHost::onCreate(
    void* ctx, PJ_string_view_t id, PJ_string_view_t kind, PJ_string_view_t language, const PJ_string_view_t* inputs,
    uint64_t input_count, const PJ_string_view_t* outputs, uint64_t output_count, PJ_string_view_t script,
    PJ_string_view_t params_json, uint32_t flags, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
    uint64_t* out_topics_count, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<MarkersRuntimeHost*>(ctx);
  try {
    MarkerService::GeneratorRecipe recipe;
    recipe.id = self->makeKey(sdk::toStringView(id));
    if (!parseKind(sdk::toStringView(kind), recipe.kind)) {
      sdk::fillError(out_error, 1, kDomain, "unknown generator kind '" + std::string(sdk::toStringView(kind)) + "'");
      return false;
    }
    if ((flags & ~kKnownFlags) != 0) {
      sdk::fillError(out_error, 1, kDomain, "pj.data_processors: reserved flag bits set");
      return false;
    }
    recipe.language = std::string(sdk::toStringView(language));
    recipe.inputs.reserve(input_count);
    for (uint64_t i = 0; i < input_count; ++i) {
      recipe.inputs.emplace_back(sdk::toStringView(inputs[i]));
    }
    // Pick the target dataset from the declared inputs (the wire carries no dataset),
    // so a generator lands on the dataset that actually holds its series.
    recipe.dataset_id = self->active_dataset_ ? self->active_dataset_(recipe.inputs) : DatasetId{0};
    recipe.outputs.reserve(output_count);
    for (uint64_t i = 0; i < output_count; ++i) {
      recipe.outputs.emplace_back(sdk::toStringView(outputs[i]));
    }
    recipe.script = std::string(sdk::toStringView(script));  // binary-safe (may carry NULs)
    recipe.params_json = std::string(sdk::toStringView(params_json));
    recipe.ephemeral = (flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) != 0;
    recipe.history_exempt = (flags & PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT) != 0;
    // params_json scope: {"scope":"all"} publishes a global marker across EVERY
    // dataset; absent/other → the active dataset only (markers only).
    if (!recipe.params_json.empty()) {
      const nlohmann::json params = nlohmann::json::parse(recipe.params_json, nullptr, false);
      recipe.all_datasets =
          !params.is_discarded() && params.is_object() && params.value("scope", std::string{}) == "all";
    }
    if (!recipe.all_datasets && recipe.dataset_id == 0) {
      sdk::fillError(out_error, 1, kDomain, "no active dataset to attach the generator to");
      return false;
    }
    Expected<std::vector<std::string>> resolved = self->service_.upsertGenerator(std::move(recipe));
    if (!resolved.has_value()) {
      sdk::fillError(out_error, 1, kDomain, resolved.error());
      return false;
    }
    self->create_topics_storage_ = std::move(*resolved);
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
    sdk::fillError(out_error, 2, kDomain, e.what());
    return false;
  } catch (...) {
    sdk::fillError(out_error, 2, kDomain, "unknown error");
    return false;
  }
}

bool MarkersRuntimeHost::onRemove(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<MarkersRuntimeHost*>(ctx);
  try {
    if (Status s = self->service_.removeGenerator(self->makeKey(sdk::toStringView(id))); !s.has_value()) {
      sdk::fillError(out_error, 1, kDomain, s.error());
      return false;
    }
    return true;
  } catch (...) {
    sdk::fillError(out_error, 2, kDomain, "unknown error");
    return false;
  }
}

bool MarkersRuntimeHost::onList(
    void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<MarkersRuntimeHost*>(ctx);
  try {
    const std::string prefix = self->plugin_id_ + "/";
    self->list_storage_.clear();
    for (const MarkerService::GeneratorRecipe& r : self->service_.recipes()) {
      if (r.id.size() > prefix.size() && r.id.compare(0, prefix.size(), prefix) == 0) {
        self->list_storage_.push_back(r.id.substr(prefix.size()));  // strip namespace → plugin-local id
      }
    }
    if (out_count != nullptr) {
      *out_count = self->list_storage_.size();
    }
    const uint64_t n = std::min<uint64_t>(capacity, self->list_storage_.size());
    for (uint64_t i = 0; i < n; ++i) {
      out_ids[i] = sdk::toAbiString(self->list_storage_[i]);
    }
    return true;
  } catch (...) {
    sdk::fillError(out_error, 2, kDomain, "unknown error");
    return false;
  }
}

bool MarkersRuntimeHost::onConfig(
    void* ctx, PJ_string_view_t id, PJ_string_view_t* out_recipe_json, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<MarkersRuntimeHost*>(ctx);
  try {
    const std::string key = self->makeKey(sdk::toStringView(id));
    for (const MarkerService::GeneratorRecipe& r : self->service_.recipes()) {
      if (r.id != key) {
        continue;
      }
      nlohmann::json j;
      j["kind"] = "markers";
      j["language"] = r.language;
      j["inputs"] = r.inputs;
      j["outputs"] = r.outputs;
      j["history_exempt"] = r.history_exempt;
      nlohmann::json params =
          r.params_json.empty() ? nlohmann::json::object() : nlohmann::json::parse(r.params_json, nullptr, false);
      j["params"] = params.is_discarded() ? nlohmann::json::object() : params;
      self->config_storage_ = j.dump();
      if (out_recipe_json != nullptr) {
        *out_recipe_json = sdk::toAbiString(self->config_storage_);
      }
      return true;
    }
    sdk::fillError(out_error, 1, kDomain, "unknown generator id");
    return false;
  } catch (...) {
    sdk::fillError(out_error, 2, kDomain, "unknown error");
    return false;
  }
}

bool MarkersRuntimeHost::onValidate(
    void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script,
    PJ_string_view_t /*params_json*/, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<MarkersRuntimeHost*>(ctx);
  try {
    GeneratorKind parsed = GeneratorKind::kMarkers;
    if (!parseKind(sdk::toStringView(kind), parsed)) {
      sdk::fillError(out_error, 1, kDomain, "unknown generator kind '" + std::string(sdk::toStringView(kind)) + "'");
      return false;
    }
    if (Status s =
            self->service_.validateScript(parsed, sdk::toStringView(language), std::string(sdk::toStringView(script)));
        !s.has_value()) {
      sdk::fillError(out_error, 1, kDomain, s.error());
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, 2, kDomain, e.what());
    return false;
  } catch (...) {
    sdk::fillError(out_error, 2, kDomain, "unknown error");
    return false;
  }
}

}  // namespace PJ
