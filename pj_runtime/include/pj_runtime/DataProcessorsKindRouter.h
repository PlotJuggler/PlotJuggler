#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/plugin_data_api.h"

namespace PJ {

class ServiceRegistryBuilder;

/// Registers the ONE `pj.data_processors.v1` service a plugin sees and routes each
/// call to the backend bridge that owns its `kind`: "markers" →
/// `MarkersRuntimeHost` (object engine / ObjectStore), everything else →
/// `DataProcessorsRuntimeHost` (transform engine / IncrementalEngine, which rejects
/// unknown kinds itself).
///
/// Why this exists: the service registry rejects duplicate names, so the two
/// per-kind bridges cannot both register — without the router the second
/// registration is silently dropped and that kind's toolboxes break with no
/// diagnostics.
///
/// Routing rules per slot:
///  - create/validate carry `kind` → routed directly.
///  - remove/config carry only `id` → tried on the transform backend first, then
///    the markers backend (ids are plugin-namespaced by each backend, so a miss is
///    a clean "unknown id" error, never a cross-plugin hit).
///  - list is the union of both backends (copied into router-owned storage, since
///    each backend's views are only valid until the next call on ITS vtable).
///
/// Lifetime: holds only the backends' fat pointers — both backends must outlive
/// the router. Not movable: the registered vtable fat pointer stores `this`.
class DataProcessorsKindRouter {
 public:
  /// @param markers     `MarkersRuntimeHost::raw()` — serves kind="markers".
  /// @param transforms  `DataProcessorsRuntimeHost::raw()` — serves kind="transform".
  DataProcessorsKindRouter(PJ_data_processors_host_t markers, PJ_data_processors_host_t transforms);

  DataProcessorsKindRouter(const DataProcessorsKindRouter&) = delete;
  DataProcessorsKindRouter& operator=(const DataProcessorsKindRouter&) = delete;
  DataProcessorsKindRouter(DataProcessorsKindRouter&&) = delete;
  DataProcessorsKindRouter& operator=(DataProcessorsKindRouter&&) = delete;

  /// Register the routed `pj.data_processors.v1` service into the plugin's registry.
  /// The name may be claimed only once — routing through this class is exactly how
  /// both backends share it — so a rejection would strand every toolbox of that
  /// kind on someone else's backend. The Status fails the caller instead.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  /// The raw C-ABI fat pointer (for direct wiring / tests).
  [[nodiscard]] PJ_data_processors_host_t raw() const noexcept {
    return raw_;
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

  /// The backend that owns `kind`: markers_ for "markers", transforms_ otherwise.
  [[nodiscard]] const PJ_data_processors_host_t& backendFor(PJ_string_view_t kind) const noexcept;

  /// The backend that currently owns `id`, or nullptr if neither does. Probes each
  /// backend's (non-mutating) config — the backends are the single source of truth,
  /// so routing kind-less remove/config this way never drifts from reality (a cached
  /// id→backend map would, since a layout restore re-creates nodes directly on a
  /// service, bypassing this router).
  [[nodiscard]] const PJ_data_processors_host_t* ownerOf(PJ_string_view_t id) const noexcept;

  PJ_data_processors_host_t markers_;
  PJ_data_processors_host_t transforms_;
  PJ_data_processors_host_vtable_t vtable_;
  PJ_data_processors_host_t raw_;
  std::vector<std::string> list_storage_;     ///< owns the union'd ids returned by onList
  std::vector<PJ_string_view_t> list_views_;  ///< views into list_storage_, valid until the next call
};

}  // namespace PJ
