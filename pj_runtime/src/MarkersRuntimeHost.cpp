// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/MarkersRuntimeHost.h"

#include <algorithm>
#include <exception>
#include <nlohmann/json.hpp>
#include <utility>

#include "data_processor_flags.hpp"
#include "pj_base/sdk/dataset_qualified_name.hpp"  // sdk::qualifiedSeriesName / splitDatasetQualifier
#include "pj_base/sdk/plugin_data_api.hpp"         // sdk::toStringView / toAbiString / fillError
#include "pj_base/sdk/service_traits.hpp"          // sdk::DataProcessorsHostService
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
    std::function<Expected<DatasetId>(std::vector<std::string>& inputs, std::vector<std::string>& outputs)>
        active_dataset,
    std::function<std::optional<std::string>(DatasetId)> source_name_of)
    : service_(service),
      plugin_id_(std::move(plugin_id)),
      active_dataset_(std::move(active_dataset)),
      source_name_of_(std::move(source_name_of)) {
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

std::vector<std::string> MarkersRuntimeHost::loadedSourceNames() const {
  std::vector<std::string> names;
  if (!source_name_of_) {
    return names;
  }
  for (const DatasetId dataset : service_.loadedDatasets()) {
    names.push_back(source_name_of_(dataset).value_or(std::string{}));
  }
  return names;
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
    recipe.outputs.reserve(output_count);
    for (uint64_t i = 0; i < output_count; ++i) {
      recipe.outputs.emplace_back(sdk::toStringView(outputs[i]));
    }
    recipe.script = std::string(sdk::toStringView(script));  // binary-safe (may carry NULs)
    recipe.params_json = std::string(sdk::toStringView(params_json));
    recipe.ephemeral = (flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) != 0;
    recipe.history_exempt = (flags & PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT) != 0;
    // params_json scope: {"scope":"all"} publishes a global marker across EVERY
    // dataset, each evaluated over its own series, so the keys are literal there;
    // absent/other → one dataset (markers only).
    if (!recipe.params_json.empty()) {
      const nlohmann::json params = nlohmann::json::parse(recipe.params_json, nullptr, false);
      recipe.all_datasets =
          !params.is_discarded() && params.is_object() && params.value("scope", std::string{}) == "all";
    }
    // A qualifier that names a loaded source contradicts scope=all (it would become
    // a key that resolves nowhere and a silently empty marker set); a colon whose
    // prefix is no loaded source is just part of the key.
    if (recipe.all_datasets) {
      const std::vector<std::string> source_names = self->loadedSourceNames();
      for (const std::string& input : recipe.inputs) {
        if (sdk::splitDatasetQualifier(input, source_names).qualified) {
          sdk::fillError(
              out_error, 1, kDomain, "dataset qualifiers cannot be combined with scope=all: '" + input + "'");
          return false;
        }
      }
    }
    // Pick the target dataset from the declared keys (the wire carries no dataset
    // field; a key may carry the "dataset_source:topic/field" qualifier, which the
    // callback consumes and strips). A resolution error fails the create — landing
    // a generator on a dataset the caller did not name is worse than refusing.
    if (!recipe.all_datasets && self->active_dataset_) {
      const std::vector<std::string> declared = recipe.inputs;
      Expected<DatasetId> dataset = self->active_dataset_(recipe.inputs, recipe.outputs);
      if (!dataset.has_value()) {
        sdk::fillError(out_error, 1, kDomain, dataset.error());
        return false;
      }
      recipe.dataset_id = *dataset;
      if (declared != recipe.inputs) {
        recipe.declared_inputs = declared;
      }
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
    for (const std::string& id : self->service_.generatorIds()) {
      if (id.size() > prefix.size() && id.compare(0, prefix.size(), prefix) == 0) {
        self->list_storage_.push_back(id.substr(prefix.size()));  // strip namespace → plugin-local id
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
    const MarkerService::GeneratorRecipe* first = self->service_.firstBinding(self->makeKey(sdk::toStringView(id)));
    if (first != nullptr && !first->ephemeral) {
      const MarkerService::GeneratorRecipe& recipe = *first;
      nlohmann::json j;
      j["kind"] = "markers";
      j["language"] = recipe.language;
      // Echo each input as declared (the SDK promises a re-editable recipe), qualified
      // with the bound dataset's source only once the bare name stopped resolving on
      // its own AND the qualified form resolves back to that dataset — checked by the
      // very resolver a resubmit would run. Neither round-tripping is the
      // unrepresentable case: the declared spelling is kept.
      const std::optional<std::string> source =
          (recipe.all_datasets || !self->source_name_of_) ? std::nullopt : self->source_name_of_(recipe.dataset_id);
      std::vector<std::string> inputs = recipe.declared_inputs.empty() ? recipe.inputs : recipe.declared_inputs;
      if (source.has_value() && !source->empty() && self->active_dataset_) {
        const auto resolves_to = [self](const std::string& name) -> std::optional<DatasetId> {
          std::vector<std::string> probe{name};
          std::vector<std::string> no_outputs;
          const Expected<DatasetId> dataset = self->active_dataset_(probe, no_outputs);
          return dataset.has_value() ? std::optional{*dataset} : std::nullopt;
        };
        for (std::size_t i = 0; i < inputs.size(); ++i) {
          const std::string qualified = sdk::qualifiedSeriesName(*source, recipe.inputs[i]);
          if (!resolves_to(recipe.inputs[i]).has_value() && resolves_to(qualified) == recipe.dataset_id) {
            inputs[i] = qualified;
          }
        }
      }
      j["inputs"] = inputs;
      j["outputs"] = recipe.outputs;
      j["history_exempt"] = recipe.history_exempt;
      nlohmann::json params = recipe.params_json.empty() ? nlohmann::json::object()
                                                         : nlohmann::json::parse(recipe.params_json, nullptr, false);
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
