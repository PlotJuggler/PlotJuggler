#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/plugin_data_api.h"

namespace PJ {

class DataProcessorsRuntimeHost;
class MarkersRuntimeHost;
class OnDemandRuntimeHost;
class ServiceRegistryBuilder;

/// Registers the ONE `pj.data_processors.v1` service a plugin sees and routes each
/// call to the backend bridge that owns its `kind`: "markers" → `MarkersRuntimeHost`
/// (BatchEngine), "transform" → `DataProcessorsRuntimeHost` (IncrementalEngine),
/// "on_demand" → the optional third backend (`OnDemandRuntimeHost`). A kind with no bound backend is
/// rejected here, naming the kind.
///
/// Why this exists: the service registry rejects duplicate names, so per-kind bridges
/// cannot each register their own copy of this service.
///
/// Routing rules per slot:
///  - create/validate carry `kind` → routed directly; the v1 create refuses "on_demand"
///    (its outputs are typed: create_data_processor_v2 only).
///  - create_v2/submit carry it inside a typed request, decoded with the SDK decoder
///    (`readDataProcessorRequest`, `readEvaluationBudget`) before any field is read, so
///    a short request is refused before its `kind`. The backend gets the decoded copy.
///    Submit on a kind whose backend has no `submit_evaluation` is refused ("has no
///    on-demand evaluation").
///  - remove/config carry only `id` → the backend whose ownership probe holds it.
///  - list is the union of every bound backend (copied into router-owned storage, since
///    each backend's views are only valid until the next call on ITS vtable).
///  - poll/release take a router handle: submit renumbers the backend's handle into
///    one namespace shared by every router in the process (increasing, never reused;
///    submit is refused once it is exhausted), so a handle reaches only the backend
///    that issued it, and only through the router (one per plugin instance) that
///    returned it. Submit needs the backend's submit, poll and release slots, and the
///    destructor releases the handles still held.
///
/// Every refusal is `PJ_ERROR_CODE_REJECTED`; a null ctx still fills `out_error`.
/// Lifetime: holds only the backends' fat pointers and probes — every bound backend
/// must outlive the router. Not movable: the registered vtable fat pointer stores `this`.
class DataProcessorsKindRouter {
 public:
  /// Per backend, "holds this plugin-local id", previews included (a backend's `config`
  /// may hide its previews, so it cannot stand in). Must not mutate the backend.
  /// Mandatory for every BOUND backend; the probe of an unbound one is never called.
  struct OwnershipProbes {
    std::function<bool(std::string_view)> transforms;
    std::function<bool(std::string_view)> markers;
    std::function<bool(std::string_view)> on_demand;
  };

  /// The primitive: raw fat pointers plus their probes. A default-constructed fat
  /// pointer (`vtable == nullptr`) is an unbound backend: its kind is rejected and it is
  /// skipped by list and the ownership lookup.
  DataProcessorsKindRouter(
      PJ_data_processors_host_t markers, PJ_data_processors_host_t transforms, PJ_data_processors_host_t on_demand,
      OwnershipProbes owns);

  /// The production wiring, probing through each bridge's `owns`.
  DataProcessorsKindRouter(
      MarkersRuntimeHost& markers, DataProcessorsRuntimeHost& transforms, OnDemandRuntimeHost& on_demand);

  DataProcessorsKindRouter(const DataProcessorsKindRouter&) = delete;
  DataProcessorsKindRouter& operator=(const DataProcessorsKindRouter&) = delete;
  DataProcessorsKindRouter(DataProcessorsKindRouter&&) = delete;
  DataProcessorsKindRouter& operator=(DataProcessorsKindRouter&&) = delete;

  /// Releases every evaluation handle still held on its backend.
  ~DataProcessorsKindRouter();

  /// Test-only: set the process-wide next handle; returns the previous one.
  static uint64_t setNextHandleForTest(uint64_t next) noexcept;

  /// Register the routed `pj.data_processors.v1` service into the plugin's registry.
  /// The name may be claimed only once — routing through this class is exactly how
  /// the backends share it — so a rejection would strand every toolbox of that kind
  /// on someone else's backend. The Status fails the caller instead.
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
  /// The backend's own check, or its validate as VALID / INVALID when it has none.
  static bool onCheck(
      void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script,
      PJ_string_view_t params_json, uint32_t* out_status, PJ_error_t* out_error) noexcept;
  static bool onCreateV2(
      void* ctx, const PJ_data_processor_request_t* request, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
      uint64_t* out_topics_count, PJ_error_t* out_error) noexcept;
  static bool onSubmit(
      void* ctx, const PJ_data_processor_request_t* request, const PJ_evaluation_budget_t* budget, uint64_t* out_handle,
      PJ_error_t* out_error) noexcept;
  static bool onPoll(
      void* ctx, uint64_t handle, uint32_t* out_state, PJ_string_view_t* out_json, PJ_error_t* out_error) noexcept;
  static bool onRelease(void* ctx, uint64_t handle, PJ_error_t* out_error) noexcept;

  /// The bound backend that serves `kind`, or nullptr (unknown kind, or its backend unbound).
  [[nodiscard]] const PJ_data_processors_host_t* backendFor(PJ_string_view_t kind) const noexcept;

  /// The bound backend whose probe holds `id`, or nullptr. The backends are the single
  /// source of truth: a cached id→backend map would drift, since a layout restore
  /// re-creates nodes directly on a service, bypassing this router.
  [[nodiscard]] const PJ_data_processors_host_t* ownerOf(PJ_string_view_t id) const;

  /// One submitted evaluation: the backend that issued it and its handle there.
  struct HandleOwner {
    const PJ_data_processors_host_t* backend = nullptr;
    uint64_t backend_handle = 0;
  };

  PJ_data_processors_host_t markers_;
  PJ_data_processors_host_t transforms_;
  PJ_data_processors_host_t on_demand_;
  OwnershipProbes owns_;
  PJ_data_processors_host_vtable_t vtable_;
  PJ_data_processors_host_t raw_;
  std::vector<std::string> list_storage_;     ///< owns the union'd ids returned by onList
  std::vector<PJ_string_view_t> list_views_;  ///< views into list_storage_, valid until the next call
  std::map<uint64_t, HandleOwner> handles_;   ///< the live handles this router returned
};

}  // namespace PJ
