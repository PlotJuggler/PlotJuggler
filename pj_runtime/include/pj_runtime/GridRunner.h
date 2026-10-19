// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/sdk/evaluation_report.hpp"
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/InFlightBudget.h"
#include "pj_runtime/InputStager.h"
#include "pj_runtime/NodeScheduler.h"
#include "pj_runtime/RecipeGraph.h"
#include "pj_runtime/ResultHistory.h"

namespace PJ {

/// The per-instant loop behind explicit evaluations (REQUIREMENTS D19; docs/derived_objects/ARCHITECTURE.md §8,
/// §15): one node, installed or transient, over a window or at an instant, ending in one report of the shape
/// `poll_evaluation` returns (pj_base/plugin_data_api.h).
///
/// The grid is the window's start plus every instant inside the inclusive window at which a raw input of the node,
/// or of an upstream it reads, changes: sorted and unique. A window that starts before the newest of those raw inputs'
/// oldest retained samples reports the part before it as a leading gap (`gaps`) and starts its grid there; a window
/// inside the gap evaluates nothing. An instant is a window whose end is its start. A pinned upstream (a finding) is
/// read, never evaluated: it adds its real result stamp, and it is an input only from that stamp (decided
/// 2026-10-18). At each instant every upstream is evaluated once, before its readers, then the node: an installed
/// upstream from its history when a result there serves it (read only), any node from the job's own last result of it
/// when that serves, otherwise by a run. Readers read the job's upstream results, so no installed node's cursor,
/// state, ring or cache moves, and nothing is published (GAP-33).
///
/// A job holds one in-flight reservation, a default run's (`InFlightBudget::kRunBytes`), for its whole life: it waits
/// for it in the budget's line before anything runs, and its chain runs under it one run at a time, so it never waits
/// for room while holding room. Where the budget holds one run (`capacity() == kRunBytes`: a shared lane of one
/// thread), an evaluation so holds the playhead's and the marker runs back for its whole duration, suspending D30;
/// reserving per run would lift that. The job stops at the first instant with a missing input (a finding before its
/// stamp included) or a failed run (`stopped: "error"`, the instant not counted), and when an installed node it reads
/// or evaluates gets a new revision or is removed, or its dataset a new epoch ("evaluation inputs were replaced"). It
/// never reports `cancelled`: `cancel` drops the callback instead.
///
/// THREADING: GUI thread, except the runs, which touch only their job's staged inputs and bytecode and the thread-safe
/// store. `store` must outlive the pool's running computes; the graph, the history, the stager and the budget must
/// outlive the runner, though a job still waiting for room may outlive the budget (it then never runs). A job still
/// held after the runner is gone never calls back. An ended job (finished, cancelled, or whose publication the pool
/// discarded) holds no node result and no room, even while its caller keeps it; a cancelled one keeps its partial
/// report until it is dropped.
class GridRunner {
 public:
  /// More input change instants in one window are refused at submission, before the grid is allocated.
  static constexpr std::size_t kMaxGridInstants = std::size_t{1} << 20;

  /// A finished evaluation, in the SDK's report shape. `evaluated` is `bundles.size()`; `error` is set exactly when
  /// `stopped` is kError; an object output is only its status; `inferred_outputs` lists, in first-seen order, what
  /// the runs of a node that declares no output inferred, and is empty otherwise.
  using Report = sdk::EvaluationReport;

  /// What one scalar input contributes to a window.
  struct ScalarChanges {
    std::vector<Timestamp> instants;  ///< its sample times inside the window, ascending
    std::optional<Timestamp> first;   ///< its oldest retained sample: a later one makes a leading gap
    bool over_limit = false;          ///< more than the limit: `instants` stays empty
  };
  /// The samples of the scalar series `name` (bare) of `dataset` inside the inclusive [start, end]; past `limit` of
  /// them it sets `over_limit` without collecting any. GUI thread.
  using ScalarChangeLookup = std::function<ScalarChanges(
      DatasetId dataset, std::string_view name, Timestamp start, Timestamp end, std::size_t limit)>;

  /// What the session wires in.
  struct Hooks {
    ScalarChangeLookup scalar_changes;   ///< empty: a scalar input adds no instant and no gap
    NodeScheduler::TfProvider tf;        ///< read once, when the job gets its room; empty: no TF
    NodeScheduler::EpochProvider epoch;  ///< empty: every dataset stays at epoch 0
    NodeScheduler::RunFunction run;      ///< empty: `scripting::runOnDemandScript`
  };

  struct Job;
  struct Result;  ///< one node's result in a job
  /// Called once, on the GUI thread, unless the job is cancelled first.
  using Done = std::function<void(Report)>;

  GridRunner(
      const ObjectStore& store, const RecipeGraph& graph, const ResultHistory& history, const InputStager& stager,
      EnginePool& pool, InFlightBudget& budget, Hooks hooks);

  GridRunner(const GridRunner&) = delete;
  GridRunner& operator=(const GridRunner&) = delete;

  /// Starts evaluating `node` over [start, end], an instant when they are equal: a copy of an installed node
  /// (`installed`: a new revision of it fails the job) or a transient one (`RecipeGraph::bind`). The job first waits
  /// for its room; `done` is never called inside this call. Hold the job until `done`, or cancel it: dropped while
  /// waiting for room, it never runs. Rejected: an end before the start, an upstream removed or no longer providing
  /// what is read, and more than `kMaxGridInstants` input change instants.
  [[nodiscard]] Expected<std::shared_ptr<Job>> submit(
      RecipeGraph::Node node, bool installed, Timestamp start, Timestamp end, Done done);
  /// Ends `job` and drops its callback. A run in flight sees the cancellation inside the interpreter; the job's results
  /// and room go once the pool lets that run go. Nothing for a finished job.
  void cancel(const std::shared_ptr<Job>& job);

 private:
  /// GUI thread: evaluates the job's chain from where it stands until a run is submitted or the job ends.
  void advance(const std::shared_ptr<Job>& job);
  /// GUI thread, once per run: takes its result into the job's memo (and, for the node, the report), then advances.
  void collect(const std::shared_ptr<Job>& job, const EnginePool::Outcome& outcome);
  /// Submits the run of the chain node at hand on `staged`.
  void submitRun(const std::shared_ptr<Job>& job, InputStager::Staged staged);
  /// Moves on to the next chain node, or adds the node's bundle and moves on to the next instant.
  static void proceed(Job& job, const InputStager::Staged& staged);
  /// Ends the job: returns the room unless a run holds it, and calls `done`.
  void finish(const std::shared_ptr<Job>& job, sdk::EvaluationStop stopped, std::string error = {});
  /// An installed node the job reads or evaluates changed, or its dataset's epoch did.
  [[nodiscard]] bool replaced(const Job& job) const;

  const ObjectStore& store_;
  const RecipeGraph& graph_;
  const ResultHistory& history_;
  const InputStager& stager_;
  EnginePool& pool_;
  InFlightBudget& budget_;
  Hooks hooks_;
  /// Expires with the runner: a grant or a publication arriving later does nothing.
  std::shared_ptr<const int> lifetime_ = std::make_shared<const int>(0);
};

}  // namespace PJ
