#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/plugin_data_api.h"
#include "pj_base/types.hpp"
#include "pj_runtime/MarkerService.h"

namespace PJ {

class ServiceRegistryBuilder;

/// Host-side bridge that exposes the unified `pj.data_processors.v1` SDK service to a
/// plugin and delegates to `MarkerService` (the object engine; this bridge serves only
/// kind="markers" — kind="transform" is routed to the DerivedEngine bridge, owned by
/// the transform-editor work).
/// Implements the C ABI vtable (trampolines recover `this` from ctx), namespaces
/// every node id under the calling plugin (so list/remove/config only ever
/// see/affect THAT plugin's nodes), and stamps each submitted recipe with the
/// dataset the host says is active.
///
/// Lifetime: the bridge is independent of the nodes it created — destroying it
/// tears down only this object; the nodes keep living in `MarkerService` (the
/// host owns the script + runtime), so a plugin DSO unload never kills a running
/// node. `service` must outlive this host. Not movable: the vtable fat pointer
/// stores `this`.
///
/// NOTE: the class/file name is kept as `MarkersRuntimeHost` to bound churn; it
/// bridges the kind="markers" path of the unified data-processors service. A rename is
/// a cosmetic follow-up.
class MarkersRuntimeHost {
 public:
  /// `plugin_id` namespaces submitted ids. `active_dataset` supplies the dataset a
  /// newly submitted generator targets, given its declared input and output series
  /// keys — host policy (the wire carries no dataset field; a key MAY carry the
  /// host's "dataset_source:topic/field" qualifier, which the callback consumes:
  /// it picks the dataset and rewrites the key to its bare form in place; the
  /// recipe keeps the declared spelling so the script still reads it by that
  /// name). An error return fails the create with that message. A
  /// {"scope":"all"} generator does not consult `active_dataset`: its keys are
  /// literal in every dataset, so a key whose prefix names a loaded source (per
  /// `source_name_of` over the service's dataset lister) is rejected as a
  /// contradiction. `source_name_of` (dataset → raw source name) also lets the
  /// config read-back qualify an input whose bare name became ambiguous, when
  /// the qualified form resolves back to the bound dataset; without it inputs
  /// echo as declared. Both `service` and whatever the callbacks read must
  /// outlive this.
  MarkersRuntimeHost(
      MarkerService& service, std::string plugin_id,
      std::function<Expected<DatasetId>(std::vector<std::string>& inputs, std::vector<std::string>& outputs)>
          active_dataset,
      std::function<std::optional<std::string>(DatasetId)> source_name_of = {});

  MarkersRuntimeHost(const MarkersRuntimeHost&) = delete;
  MarkersRuntimeHost& operator=(const MarkersRuntimeHost&) = delete;

  /// Register `DataProcessorsHostService` into the builder used to bind the plugin.
  /// It is this host's only service, so a rejection leaves nothing to bind against:
  /// the Status fails the caller rather than producing a mute host.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  /// The fat pointer this registers — exposed for direct binding in tests.
  [[nodiscard]] PJ_data_processors_host_t raw() const noexcept {
    return data_processors_;
  }

 private:
  static bool onCreate(
      void* ctx, PJ_string_view_t id, PJ_string_view_t kind, PJ_string_view_t language, const PJ_string_view_t* inputs,
      uint64_t input_count, const PJ_string_view_t* outputs, uint64_t output_count, PJ_string_view_t script,
      PJ_string_view_t params_json, uint32_t flags, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
      uint64_t* out_topics_count, PJ_error_t* out_error) noexcept;
  static bool onRemove(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept;
  static bool onList(
      void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* out_error) noexcept;
  /// A plugin-local id may be bound to several datasets; this reports its
  /// first-applied binding (`MarkerService::firstBinding`).
  static bool onConfig(
      void* ctx, PJ_string_view_t id, PJ_string_view_t* out_recipe_json, PJ_error_t* out_error) noexcept;
  static bool onValidate(
      void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script,
      PJ_string_view_t params_json, PJ_error_t* out_error) noexcept;

  /// Namespaced storage key for a plugin-local id: `plugin_id_ + "/" + local`.
  [[nodiscard]] std::string makeKey(std::string_view local_id) const;
  /// Raw source names of the service's loaded datasets (the qualifier vocabulary);
  /// empty without `source_name_of`.
  [[nodiscard]] std::vector<std::string> loadedSourceNames() const;

  MarkerService& service_;
  std::string plugin_id_;
  std::function<Expected<DatasetId>(std::vector<std::string>& inputs, std::vector<std::string>& outputs)>
      active_dataset_;
  std::function<std::optional<std::string>(DatasetId)> source_name_of_;
  PJ_data_processors_host_vtable_t vtable_;
  PJ_data_processors_host_t data_processors_;
  std::vector<std::string> list_storage_;           ///< backs onList's borrowed views until the next call
  std::string config_storage_;                      ///< backs onConfig's borrowed view until the next call
  std::vector<std::string> create_topics_storage_;  ///< backs onCreate's borrowed out_topics until the next call
};

}  // namespace PJ
