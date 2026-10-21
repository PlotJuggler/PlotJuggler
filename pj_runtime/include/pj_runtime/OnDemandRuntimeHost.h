#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/plugin_data_api.h"
#include "pj_runtime/GridRunner.h"
#include "pj_runtime/RecipeGraph.h"

namespace PJ {

class ObjectStore;
class OnDemandService;

/// Serves `kind="on_demand"` of `pj.data_processors.v1` to ONE plugin instance over the session's `OnDemandService`
/// (docs/derived_objects/ARCHITECTURE.md §18): the typed create, list, config, remove and validate, and the
/// evaluations (submit, poll, release) run by its `GridRunner`. `DataProcessorsKindRouter` reaches it as its third
/// backend; every request arrives decoded and is decoded again here (`sdk::readDataProcessorRequest`).
///
/// Ids are namespaced per plugin: a persistent recipe is `<plugin>/<id>`, shared by the plugin's instances; a preview
/// `<plugin>/~preview/<n>/<id>`, with `n` unique to this bridge, so another instance neither sees nor ends it, and no
/// persistent recipe (hence no layout) can hold its key (`RecipeGraph::kPreviewIdPrefix`). A node is this plugin's
/// only when its recipe's owner is, since an id may contain '/' (plugin `a`'s `b/r` and plugin `a/b`'s `r` spell one
/// key). One id names one recipe in one instance: creating it with the other lifetime is refused until it is removed.
///
/// Create (v2 only; v1 is refused naming v2): typed outputs required; params at most `kMaxParamsBytes` and
/// `scripting::kMaxParamsDepth` levels; INSTANT pins the node (a finding, which the session requests at its pin);
/// WINDOW refused. A refused create leaves the previous recipe as it was. Submit takes an
/// installed node of this plugin (only its id, kind, flags and time fields) or a script evaluated as a preview,
/// installed nowhere; a script that does not bind, or a Luau one that does not compile, is refused at submit (Python
/// compiles at its first run, never on the calling thread: its compile error is the node's state, and an evaluation's
/// report). Validate and the three-way check compile Luau, and Python as `scripting::checkOnDemandScriptPython` does:
/// at most `kPythonValidateWaitMillis`, then unchecked, which the check reports and validate passes (it has no third
/// answer). Budget fields of 0 take
/// `GridRunner::Budget`'s defaults, and the runner clamps them to its host caps. A finished evaluation is FAILED when
/// it stopped on an error before any bundle, COMPLETED otherwise; both carry `GridRunner::toJson`'s report, written on
/// a worker (the handle stays PENDING until it lands) and stable until release. State 3 (reserved) is never emitted.
///
/// Handles are per bridge, increasing and never reused; at most `kMaxHandles` live and `kMaxReportBytes` reserved
/// (each handle reserves its clamped `max_report_bytes`) until released. Release cancels the job cooperatively and
/// returns its capacity at once.
///
/// At most `kMaxRunningEvaluations` of them are in the runner at once: a job holds an in-flight reservation for its
/// whole life, so 64 jobs in the budget's line would hold the playhead's request back behind all of them (D30). The
/// others wait PENDING in a FIFO here and go to the runner as the running one finishes or is released; one the runner
/// then refuses (an upstream removed meanwhile) is FAILED with the runner's error. A queued one released never runs. A
/// queued installed node is evaluated as it was at submit, so a new revision meanwhile fails it ("replaced").
/// ponytail: one per plugin instance; several plugins still add up in the line. Reserving per run in `GridRunner`
/// would lift both.
///
/// LIFETIME: holds no plugin pointer; recipes outlive it, its previews end with its owner (`previews()`, handed to
/// `SessionManager::removeEphemerals`). Destruction cancels every job, which drops its callback (it holds `this` and a
/// weak reference to its handle's slot), and drops the queue. `service` and `store` must outlive it, and it must
/// outlive every router holding its fat pointer (a router releases its handles when destroyed). Not movable: the vtable
/// stores `this`. Every slot `[main-thread]`.
class OnDemandRuntimeHost {
 public:
  static constexpr std::size_t kMaxHandles = 64;
  static constexpr std::size_t kMaxRunningEvaluations = 1;
  static constexpr std::size_t kMaxReportBytes = std::size_t{64} << 20;
  /// The largest params a request may carry, checked before they are copied: as large as a script may be.
  static constexpr std::size_t kMaxParamsBytes = std::size_t{1} << 20;

  /// @param on_topics_changed called after a create or a remove that may have registered or retired derived topics
  ///        (the catalog re-lists them); may be empty.
  OnDemandRuntimeHost(
      OnDemandService& service, const ObjectStore& store, std::string plugin_id,
      std::function<void()> on_topics_changed = {});
  ~OnDemandRuntimeHost();

  OnDemandRuntimeHost(const OnDemandRuntimeHost&) = delete;
  OnDemandRuntimeHost& operator=(const OnDemandRuntimeHost&) = delete;
  OnDemandRuntimeHost(OnDemandRuntimeHost&&) = delete;
  OnDemandRuntimeHost& operator=(OnDemandRuntimeHost&&) = delete;

  [[nodiscard]] PJ_data_processors_host_t raw() const noexcept {
    return raw_;
  }

  /// Whether this plugin has a recipe `local_id`, previews included: the router's ownership probe. Reads only.
  [[nodiscard]] bool owns(std::string_view local_id) const;

  /// The keys of the previews created through this bridge and not removed through it; one may since have gone
  /// another way (its dataset closed): `SessionManager::removeEphemerals` skips it.
  [[nodiscard]] const std::unordered_set<std::string>& previews() const noexcept {
    return previews_;
  }

  /// The live evaluation handles and the report bytes they reserve.
  [[nodiscard]] std::size_t liveHandles() const noexcept {
    return evaluations_.size();
  }
  [[nodiscard]] std::size_t reservedReportBytes() const noexcept {
    return report_bytes_reserved_;
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

  /// `<plugin>/<id>`, or this bridge's preview `<plugin>/~preview/<n>/<id>`.
  [[nodiscard]] std::string keyOf(std::string_view local_id, bool preview) const;
  /// The node at `key` when this plugin owns it; null otherwise.
  [[nodiscard]] const RecipeGraph::Node* ownNode(const std::string& key) const;
  /// This plugin's node `local_id` names: this bridge's own preview first, else the persistent one; null when none.
  [[nodiscard]] const RecipeGraph::Node* nodeOf(std::string_view local_id) const;
  /// The recipe fields of a decoded request: owner, id, language, inputs, typed outputs, script, params, dataset.
  [[nodiscard]] Expected<RecipeGraph::Recipe> recipeOf(const PJ_data_processor_request_t& request) const;
  /// `data_processor_config`'s JSON of `node`.
  [[nodiscard]] std::string recipeJson(const RecipeGraph::Node& node) const;

  /// What `GridRunner::submit` takes.
  struct Submission {
    RecipeGraph::Node node;
    bool installed = false;
    Timestamp start = 0;
    Timestamp end = 0;
    GridRunner::Budget limits;
  };
  /// One submitted evaluation, kept until release.
  struct Evaluation {
    uint32_t state = PJ_EVALUATION_STATE_PENDING;  ///< pending until its report is written
    std::string json;                              ///< the report once finished; what poll's view borrows
    std::shared_ptr<GridRunner::Job> job;          ///< set while it is in the runner
    std::optional<Submission> queued;              ///< set while it waits in `queue_`
    std::size_t reserved_bytes = 0;
  };

  /// The evaluations in the runner.
  [[nodiscard]] std::size_t running() const;
  /// Hands `evaluation` to the runner; its report arrives through `writeReport`, then the queue moves on.
  [[nodiscard]] Status start(const std::shared_ptr<Evaluation>& evaluation);
  /// Starts queued evaluations while fewer than `kMaxRunningEvaluations` run; one the runner refuses is FAILED with its
  /// error. Never re-entered from `start`: `GridRunner::submit` never calls back inside itself. GUI thread.
  void dispatch();

  /// Writes `report` into `slot` and makes it terminal: a report of up to 8 MiB takes tens of ms to serialize, so a
  /// worker of `pool` serializes (and frees) it and the GUI thread only moves the string in. A slot released meanwhile
  /// gets nothing. GUI thread.
  static void writeReport(EnginePool& pool, std::weak_ptr<Evaluation> slot, GridRunner::Report report);

  OnDemandService& service_;
  const ObjectStore& store_;
  std::string plugin_id_;
  std::string preview_namespace_;  ///< `~preview/<n>/`, the user-id prefix of this bridge's previews
  std::function<void()> on_topics_changed_;
  PJ_data_processors_host_vtable_t vtable_;
  PJ_data_processors_host_t raw_;
  // Backing storage of the borrowed returns, valid until the next call on this vtable.
  std::vector<std::string> list_storage_;
  std::string config_storage_;
  std::vector<std::string> create_topics_storage_;
  std::unordered_set<std::string> previews_;  ///< see previews()
  std::map<uint64_t, std::shared_ptr<Evaluation>> evaluations_;
  std::deque<std::weak_ptr<Evaluation>> queue_;  ///< waiting for the runner, oldest first; a released one expires
  std::size_t report_bytes_reserved_ = 0;
  uint64_t next_handle_ = 1;
};

}  // namespace PJ
