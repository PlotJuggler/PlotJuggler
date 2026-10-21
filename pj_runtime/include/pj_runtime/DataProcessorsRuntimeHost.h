#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/plugin_data_api.h"

namespace PJ {

class DataProcessorService;
class ServiceRegistryBuilder;

/// Host-side bridge that exposes the unified `pj.data_processors.v1` SDK service to
/// ONE plugin and delegates to a `DataProcessorService`. This bridge serves only
/// kind="transform" (per-sample numeric series materialized into the DataEngine via
/// the IncrementalEngine) — kind="markers" is routed to a separate bridge
/// (`MarkersRuntimeHost`), so anything else is rejected. `create_data_processor_v2`
/// takes no time flags, typed outputs or `dataset_id` (each refused naming the kind),
/// then runs the legacy create; the evaluation slots are absent (the router refuses a
/// submit on this kind).
///
/// DATA-ONLY: it marshals the C ABI (only strings cross) into transform
/// upsert/remove/list/config calls; the host owns all execution. Per-plugin: every
/// ABI `id` is namespaced under `plugin_id`, so a plugin can only see or touch its
/// own transforms.
///
/// All vtable slots are `[main-thread]` (the ABI contract), so — unlike
/// `ToolboxRuntimeHost` — there is no worker-thread marshalling and no Qt
/// dependency; the bridge forwards directly and is headless-testable.
///
/// LIFETIME — the bridge is INDEPENDENT of the transforms it creates. Destroying
/// it (the ordinary case on plugin DSO unload) tears down only this object; the
/// transform nodes KEEP RUNNING because the host owns the script + VM. Transforms
/// are torn down only by an explicit `remove`, `clearAllTransforms` (session close)
/// or, for a preview this bridge created, the end of its plugin instance:
/// `DerivedRuntimeHosts` (the owner of this bridge) removes its `previews()`.
/// Not movable: the vtable fat pointer stores `this`.
class DataProcessorsRuntimeHost {
 public:
  /// @param service  the host service the bridge forwards to (must outlive this).
  /// @param plugin_id  the calling plugin's stable manifest id (the upsert namespace).
  DataProcessorsRuntimeHost(DataProcessorService& service, std::string plugin_id);

  DataProcessorsRuntimeHost(const DataProcessorsRuntimeHost&) = delete;
  DataProcessorsRuntimeHost& operator=(const DataProcessorsRuntimeHost&) = delete;
  DataProcessorsRuntimeHost(DataProcessorsRuntimeHost&&) = delete;
  DataProcessorsRuntimeHost& operator=(DataProcessorsRuntimeHost&&) = delete;

  /// Register the `pj.data_processors.v1` service into the plugin's registry
  /// (beside `pj.toolbox_write.v1`). It is this host's only service, so a
  /// rejection fails the Status rather than leaving the plugin unable to create
  /// any processor.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  /// The raw C-ABI fat pointer (for direct wiring / tests).
  [[nodiscard]] PJ_data_processors_host_t raw() const noexcept {
    return raw_;
  }

  /// Whether this plugin has a transform `local_id`, previews included: the router's
  /// ownership probe. Reads only.
  [[nodiscard]] bool owns(std::string_view local_id) const;

  /// The namespaced keys of the previews (ephemeral transforms) created through this
  /// bridge and not removed through it. One may since have gone another way (a
  /// cascade): `DataProcessorService::removeEphemerals` skips it.
  [[nodiscard]] const std::unordered_set<std::string>& previews() const noexcept {
    return previews_;
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
  static bool onConfig(
      void* ctx, PJ_string_view_t id, PJ_string_view_t* out_recipe_json, PJ_error_t* out_error) noexcept;
  static bool onValidate(
      void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script,
      PJ_string_view_t params_json, PJ_error_t* out_error) noexcept;
  /// Always decides: an unknown kind or language is refused (false), a script then VALID or INVALID.
  static bool onCheck(
      void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script,
      PJ_string_view_t params_json, uint32_t* out_status, PJ_error_t* out_error) noexcept;
  static bool onCreateV2(
      void* ctx, const PJ_data_processor_request_t* request, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
      uint64_t* out_topics_count, PJ_error_t* out_error) noexcept;

  DataProcessorService& service_;
  std::string plugin_id_;
  PJ_data_processors_host_vtable_t vtable_;
  PJ_data_processors_host_t raw_;
  // Backing storage for borrowed returns (views stay valid until the next call on
  // this vtable, per the ABI contract).
  std::vector<std::string> list_storage_;
  std::string config_storage_;
  std::vector<std::string> create_topics_storage_;  ///< backs onCreate's borrowed out_topics until the next call
  std::unordered_set<std::string> previews_;        ///< see previews()
};

}  // namespace PJ
