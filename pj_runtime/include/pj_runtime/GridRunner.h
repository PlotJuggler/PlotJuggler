// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
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
/// §15): one node, installed or transient, over a window or at an instant, under a budget, ending in one report of
/// the shape `poll_evaluation` returns (pj_base/plugin_data_api.h; `sdk::parseEvaluationReport` reads it).
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
/// A job holds one in-flight reservation, its memory budget, for its whole life: it waits for it in the budget's line
/// before anything runs, and its chain runs under it one run at a time, so it never waits for room while holding room.
/// Where the budget holds one default run (`capacity() == kRunBytes`: a shared lane of one thread), an evaluation on
/// the default memory budget so holds the playhead's and the marker runs back for its whole duration, suspending D30.
/// `OnDemandRuntimeHost` lets at most one job per plugin instance in, so each instance holds them back by at most one
/// evaluation; reserving per run would lift the rest. The job stops at the first instant with a missing input (a
/// finding before its stamp included) or a failed run (`stopped: "error"`, the instant not counted), when an installed
/// node it reads or evaluates gets a new revision or is removed, or its dataset a new epoch ("evaluation inputs were
/// replaced"), and on its budgets: the time and the evaluations between instants (the first instant always runs), the
/// report's size before each bundle joins it. It never reports `cancelled`: `cancel` drops the callback instead.
///
/// THREADING: GUI thread, except the runs, which touch only their job's staged inputs and bytecode and the thread-safe
/// store, and summarize their outputs there. `store` must outlive the pool's running computes; the graph, the
/// history, the stager and the budget must outlive the runner, though a job still waiting for room may outlive the
/// budget (it then never runs). A job still held after the runner is gone never calls back. An ended job (finished,
/// cancelled, or whose publication the pool discarded) holds no node result and no room, even while its caller keeps
/// it; a cancelled one keeps its partial report until it is dropped.
class GridRunner {
 public:
  /// An evaluation's budget, every field honoured as given (0 included) once clamped to `kHostCaps`; the plugin
  /// contract's "0 means the default" is its bridge's to apply.
  struct Budget {
    uint64_t max_millis = 1000;  ///< checked between instants: 0 still evaluates the first one
    /// The job's whole memory, split in the default ratio between each run's VM cap and one native ledger all its
    /// runs share (the objects the chain keeps for its readers count against the later runs); also the job's
    /// in-flight reservation. A run past it stops the job with `budget_bytes`; 0 runs nothing.
    uint64_t max_bytes = InFlightBudget::kRunBytes;
    uint64_t max_evaluations = 200;          ///< bundles; 0 runs nothing
    uint64_t max_report_bytes = 1ull << 20;  ///< the whole report, as `toJson` writes it
  };
  static constexpr Budget kHostCaps{5000, InFlightBudget::kRunBytes, 2000, 8ull << 20};
  /// More input change instants in one window are refused at submission, before the grid is allocated.
  static constexpr std::size_t kMaxGridInstants = std::size_t{1} << 20;
  /// The longest `Report::error`, in bytes.
  static constexpr std::size_t kMaxErrorBytes = 1024;
  /// A series chunk's time budget: the longest a playhead request waits behind it on a lane of one thread.
  static constexpr uint64_t kRowsMaxMillis = 100;

  /// A finished evaluation, in the SDK's report shape: `toJson` writes it, `sdk::parseEvaluationReport` reads it back.
  /// `evaluated` is `bundles.size()`; `error` is set exactly when `stopped` is kError, at most `kMaxErrorBytes` long
  /// (shorter only to keep the report within its budget); `inferred_outputs` lists, in first-seen order, what the runs
  /// of a node that declares no output inferred, and is empty otherwise.
  using Report = sdk::EvaluationReport;

  /// What one scalar input contributes to a window.
  struct ScalarChanges {
    std::vector<Timestamp> instants;  ///< its sample times inside the window, ascending
    std::optional<Timestamp> first;   ///< its oldest retained sample: a later one makes a leading gap
    bool over_limit = false;          ///< more than the limit: `instants` stays empty
    /// Why the lookup did not read the series' changes; `submit` refuses a window that reads such a series.
    enum class Unreadable : uint8_t {
      kNone,
      kString,      ///< a string series, whose changes the lookup does not read
      kNotASeries,  ///< neither a numeric nor a string column, or a column that does not exist
    };
    Unreadable unreadable = Unreadable::kNone;
  };
  /// The samples of the scalar series `name` (bare) of `dataset` inside the inclusive [start, end]; past `limit` of
  /// them it sets `over_limit` without collecting any. GUI thread.
  using ScalarChangeLookup = std::function<ScalarChanges(
      DatasetId dataset, std::string_view name, Timestamp start, Timestamp end, std::size_t limit)>;

  /// What the session wires in.
  struct Hooks {
    ScalarChangeLookup scalar_changes;                  ///< empty: a scalar input adds no instant and no gap
    NodeScheduler::TfProvider tf;                       ///< read once, when the job gets its room; empty: no TF
    NodeScheduler::EpochProvider epoch;                 ///< empty: every dataset stays at epoch 0
    NodeScheduler::RunFunction run;                     ///< empty: `scripting::runOnDemand`
    NodeScheduler::ImageDecoderProvider image_decoder;  ///< empty: no run has a decoder
    NodeScheduler::VideoSourceProvider video_source;    ///< empty: no run has a video source
    std::function<std::chrono::steady_clock::time_point()> now;  ///< the time budget's clock; empty: steady_clock
  };

  /// A series chunk's outcome (`submitRows`, the column sink; ARCHITECTURE §15): a row per instant at which the node
  /// gave each of its number outputs a finite number, a gap per other instant (a missing input, a failed run, an
  /// unavailable or non-finite number).
  struct Rows {
    std::vector<Timestamp> times;      ///< the rows' instants, ascending
    std::vector<double> values;        ///< row-major: the number outputs in declaration order, per row
    std::optional<Timestamp> through;  ///< the last instant evaluated, a row or a gap; nullopt: none
    uint64_t gaps = 0;
    std::string first_gap;  ///< why the first gap has no row, at most `kMaxErrorBytes`
    /// Clock instants after `through` up to `until`, counted at the submission: exact for one object input, an upper
    /// bound when several share stamps.
    uint64_t left = 0;
    /// Clock instants at or before `after`, counted like `left`: with it and the chunk's own, the whole clock's.
    uint64_t before = 0;
    std::string error;  ///< why the chunk ended early: an input was replaced (a revision, a removal, an epoch)
    /// The arrivals this chunk's clock was cut from: an object input entry with a greater uid was not in it. The next
    /// chunk's `seen`.
    SequentialUID seen;
    /// The clock started again from its first instant: an entry arrived before `after` since `seen`. The rows
    /// before replace the series'.
    bool restarted = false;
  };
  /// Called once, on the GUI thread, unless the job is cancelled first; the job has given its room back by then.
  using RowsDone = std::function<void(Rows)>;

  struct Job;
  struct Result;  ///< one node's result in a job
  /// Called once, on the GUI thread, unless the job is cancelled first; the job has given its room back by then, and
  /// `done` may submit the next job.
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
  /// what is read, more than `kMaxGridInstants` input change instants, and a report budget below the smallest report.
  [[nodiscard]] Expected<std::shared_ptr<Job>> submit(
      RecipeGraph::Node node, bool installed, Timestamp start, Timestamp end, Budget budget, Done done);
  /// A series chunk: evaluates the installed `node` at the first `limit` instants after `after` (nullopt: from the
  /// first) and at or before `until` (nullopt: no end) of its object clock, the entry stamps of the raw object inputs
  /// of the node and of every upstream it reads and the real stamps of the findings it reads, sorted and unique: a
  /// scalar side input adds none (D10). As `submit` does (one reservation, the job's own upstream results, nothing
  /// published), except that an instant ends in a row or a gap and never stops the chunk, and that its time budget is
  /// `kRowsMaxMillis`, which ends a chunk early. `seen` is the previous chunk's `Rows::seen` (invalid: no check): an
  /// object input entry that arrived since then before `after` was in no clock, so the chunk starts from the first
  /// instant instead (`Rows::restarted`). One at `after` does not: a latched source republishing its stamp would replay
  /// the series at every tick. Null, with nothing submitted, when the clock has no instant in (`after`, `until`] and
  /// nothing arrived late. Rejected: an upstream removed or no longer providing.
  [[nodiscard]] Expected<std::shared_ptr<Job>> submitRows(
      RecipeGraph::Node node, std::optional<Timestamp> after, std::optional<Timestamp> until, SequentialUID seen,
      std::size_t limit, RowsDone done);
  /// Ends `job` and drops its callback. A run in flight sees the cancellation inside the interpreter; the job's results
  /// and room go once the pool lets that run go. Nothing for a finished job. True when a run still holds its pool
  /// thread and the job's room: `released` is then called, on the GUI thread, once the pool lets it go, unless the
  /// runner is gone or the pool discards the publication first.
  bool cancel(const std::shared_ptr<Job>& job, std::function<void()> released = {});
  /// Whether the series chunk `job` (`submitRows`) starts its clock again from the first instant (`Rows::restarted`),
  /// known at its submission.
  [[nodiscard]] static bool restarts(const Job& job);

  /// The report as `poll_evaluation` returns it; every nanosecond count a JSON integer.
  [[nodiscard]] static std::string toJson(const Report& report);

 private:
  /// A job over `node` and the upstreams it reads (each once, upstreams first; the findings among them, which are read,
  /// never evaluated, go to `findings`), guarded by the revisions of the installed ones and the dataset's epoch.
  [[nodiscard]] Expected<std::shared_ptr<Job>> chainJob(
      RecipeGraph::Node node, bool installed, std::vector<std::string>& findings) const;
  /// Puts the job in the budget's line; its room starts it.
  void waitForRoom(const std::shared_ptr<Job>& job);
  /// A series chunk's instant at hand has no row: counts the gap, keeps its first reason, moves to the next instant.
  static void skipInstant(Job& job, std::string_view reason);
  /// GUI thread: evaluates the job's chain from where it stands until a run is submitted or the job ends.
  void advance(const std::shared_ptr<Job>& job);
  /// GUI thread, once per run: takes its result into the job's memo (and, for the node, the report), then advances.
  void collect(const std::shared_ptr<Job>& job, const EnginePool::Outcome& outcome);
  /// Submits the run of the chain node at hand on `staged`.
  void submitRun(const std::shared_ptr<Job>& job, InputStager::Staged staged);
  /// Moves on to the next chain node, or adds the node's bundle and moves on to the next instant; false when that
  /// ended the job (the bundle did not fit: `budget_report`).
  [[nodiscard]] bool proceed(const std::shared_ptr<Job>& job, const InputStager::Staged& staged);
  /// Adds the node's bundle at the current instant; false, with nothing added, when the report would not fit.
  [[nodiscard]] static bool addBundle(Job& job, const InputStager::Staged& staged, const Result& result);
  /// Ends the job: shortens the error to fit, returns the room unless a run holds it, and calls `done`.
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
