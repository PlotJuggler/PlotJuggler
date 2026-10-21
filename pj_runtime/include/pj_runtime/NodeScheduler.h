// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/InFlightBudget.h"
#include "pj_runtime/InputStager.h"
#include "pj_runtime/RecipeGraph.h"
#include "pj_runtime/ResultHistory.h"
#include "pj_scripting/on_demand_script.h"
#include "pj_tf/tf_buffer.h"

namespace PJ {

/// Runs the on-demand nodes on the pool under the result policy D30 (docs/derived_objects/ARCHITECTURE.md §4
/// invariant 1, §8, §11). Per node: one run in flight and one pending request, which holds only the latest
/// time. A time-only request never cancels the running run: it completes, is published, and then the pending
/// request starts. A run is cancelled, and its result dropped, only when its meaning changes: a new revision
/// (its own or an upstream's, `supersede`), a removal (`forget`), or a new epoch of its dataset (seen at the
/// next request, and again at publication).
///
/// A request stages the node's inputs on the GUI thread (cheap, so at every request), then serves it from the
/// history when it can: a cache hit or a result the ring holds computes nothing, republishes only a result the
/// ring no longer shows, and drops the tombstones it supersedes. A missing input runs nothing and clears what the
/// ring shows at the requested time (D6); a failed run that serves the current request clears what it shows at the
/// run's stamp, and every failed run is remembered by its key, so the same inputs fail again without a run, clearing
/// again (an input that failed to fetch or decode, only until the next request). After every run, a result that serves
/// the current request settles it; otherwise the current request is served again (the run was for another time,
/// or its inputs are gone), and a late result that does not serve it never takes the ring entry that does.
///
/// A node reading an upstream's output first requests the upstream at the same time and waits for it (one task
/// per node). Each publication of an upstream serves its readers again, which take the result it shows, so a
/// reader keeps updating while its upstream lags (D30) and converges with it; every settlement of an upstream
/// (published, served from the history, missing, failed) serves the readers waiting on it. Runs that do not fit
/// the in-flight budget wait in its line, first in, first out with the marker runs (`InFlightBudget`); a finishing
/// run serves that line first, then its readers, then its own next run.
///
/// THREADING: GUI thread only, except the run function. A run touches only its job (the staged inputs, the
/// shared bytecode and the TF snapshot taken at its submission) and the thread-safe `ObjectStore`, never the graph,
/// the history or the scheduler, and serializes its object outputs there. `store` must outlive the pool's
/// running computes; the graph, the history, the stager and the pool must outlive the scheduler, and the budget
/// the pool (a run still on the pool holds its reservation). A callback must not call back into the scheduler.
class NodeScheduler {
 public:
  /// What a layer shows about a node (REQUIREMENTS D6, D30).
  struct NodeState {
    enum class Status {
      kIdle,          ///< no request waits or has settled: the default
      kPending,       ///< the newest request is computing or waiting: the previous result stays shown
      kReady,         ///< the newest request's result is published
      kMissingInput,  ///< the newest request has an input with nothing at or before its time: no run, cleared
      kError,         ///< the newest request's run failed: cleared
    };
    Status status = Status::kIdle;
    std::string detail;         ///< why, for kMissingInput and kError
    Timestamp requested_t = 0;  ///< the newest request's time (a finding's pinned time)
    /// How far the result shown at `requested_t` is behind the one the newest request produces: the difference
    /// of their stamps (REQUIREMENTS D30), 0 once it is shown. nullopt when no available result is shown, or
    /// the target is not known yet (an upstream computes first), and in kIdle, kMissingInput and kError.
    std::optional<Timestamp> lag;

    bool operator==(const NodeState&) const = default;
  };

  /// The dataset's TF state, taken when a run is submitted (GUI thread); null when the dataset has no TF. While
  /// the dataset's TF ingest is active, return a snapshot no newer than the brake rate allows (ARCHITECTURE §19).
  using TfProvider = std::function<std::shared_ptr<const pj::tf::TransformSnapshot>(DatasetId)>;
  /// The revision the dataset's TF state has now, read at every request to match results that read the TF tree,
  /// with no snapshot taken (GUI thread).
  using TfRevisionProvider = std::function<uint64_t(DatasetId)>;
  /// The dataset's epoch, bumped when its storage is replaced (GUI thread).
  using EpochProvider = std::function<uint64_t(DatasetId)>;
  /// One script run, on a worker: `scripting::runOnDemand`'s signature.
  using RunFunction = std::function<Expected<scripting::ScriptBundle, scripting::ScriptError>(
      const scripting::CompiledOnDemandScript&, Span<const scripting::NamedValue>, std::string_view,
      Span<const scripting::OutputSpec>, const scripting::RunOptions&)>;
  /// Called on the GUI thread with the node's key.
  using KeyCallback = std::function<void(const std::string&)>;
  /// What a run decodes images through (`RunOptions::image_decoder`), read when the run is submitted (GUI thread);
  /// null: a script's `decode` reports "no image decoder".
  using ImageDecoderProvider = std::function<std::shared_ptr<const scripting::IImageDecoder>()>;
  /// What a run's `video:frame_at` decodes through (`RunOptions::video_source`), read when the run is submitted (GUI
  /// thread); null: it reports "no video frame source".
  using VideoSourceProvider = std::function<std::shared_ptr<scripting::IVideoFrameSource>()>;

  struct Hooks {
    TfProvider tf;                       ///< empty: no dataset has TF
    TfRevisionProvider tf_revision;      ///< empty: `tf`'s snapshot's revision, or 0
    EpochProvider epoch;                 ///< empty: every dataset stays at epoch 0
    RunFunction run;                     ///< empty: `scripting::runOnDemand`
    KeyCallback published;               ///< the history took a result of the node: the ring may show something new
    KeyCallback state_changed;           ///< the node's status, detail or lag changed
    ImageDecoderProvider image_decoder;  ///< empty: no run has a decoder
    VideoSourceProvider video_source;    ///< empty: no run has a video source
  };

  NodeScheduler(
      const ObjectStore& store, const RecipeGraph& graph, ResultHistory& history, const InputStager& stager,
      EnginePool& pool, InFlightBudget& budget, Hooks hooks);
  /// Cancels the runs in flight; their results are never published, and each returns its reservation once the
  /// pool lets it go.
  ~NodeScheduler();

  NodeScheduler(const NodeScheduler&) = delete;
  NodeScheduler& operator=(const NodeScheduler&) = delete;

  /// Requests `key` at `t` (a finding at its pinned time): its cursor moves there and its state follows this
  /// request. An unknown key is ignored.
  void requestAt(std::string_view key, Timestamp t);
  /// The node's meaning changed (a new revision of it or of an upstream): cancels its run and drops its pending
  /// request and its remembered failure; a node with a request (not withdrawn) is requested again at its cursor.
  void supersede(std::string_view key);
  /// The node is gone: cancels its run, whose result is then dropped, and forgets its state.
  void forget(std::string_view key);
  /// Nothing demands the node any more (DEM-03): drops its waiting request and settles it idle. A running run still
  /// publishes into the history (and settles the node ready when it was for its last request); nothing starts for the
  /// node until its next request. Kept, returning false, while a reader that still has a request reads it.
  bool withdraw(std::string_view key);
  /// The dataset's storage is about to change: none of its nodes is served until the matching `release` (a request only
  /// moves the cursor), and those with a request are pending. Their runs are cancelled: the caller moves the dataset's
  /// epoch with the change, so none could be published. Holds nest.
  void hold(DatasetId dataset);
  /// Ends one `hold`; the last serves every node of the dataset that has a request, at its cursor.
  void release(DatasetId dataset);

  /// nullopt for a node never requested, or forgotten.
  [[nodiscard]] std::optional<NodeState> state(std::string_view key) const;

 private:
  struct Job;
  /// What the newest request waits for.
  enum class Wait {
    kNone,      ///< nothing: settled, or its run is the newest request's
    kRun,       ///< the node's running run, for an older request
    kUpstream,  ///< an upstream's publication or settlement
    kRoom,      ///< room in the budget: `Slot::room` holds its place in the budget's line
  };
  /// Why a request is served.
  enum class Trigger {
    kRequest,  ///< a request, or the end of the node's run: waits for an upstream still computing
    kRetry,    ///< an upstream published or settled, or room came: takes what the upstreams show
  };
  /// One node's scheduling state.
  struct Slot {
    /// The last failed run: a request its key serves fails again without a run.
    struct Failure {
      ResultHistory::CacheKey key;  ///< with what the run observed
      std::string message;
      bool decode = false;  ///< an input failed to fetch or decode, not the script: the next request runs again
    };
    std::shared_ptr<Job> running;
    bool requested = false;  ///< has a request to serve: set by `requestAt`, cleared by `withdraw`
    Wait wait = Wait::kNone;
    InFlightBudget::Ticket room;  ///< its place in the budget's line while `wait` is kRoom
    Timestamp cursor = 0;         ///< the newest request's time
    NodeState::Status status = NodeState::Status::kIdle;
    std::string detail;
    std::optional<Timestamp> lag;
    std::optional<Failure> failure;
    /// The nodes that requested this one as an upstream; may name one gone since.
    std::vector<std::string> readers;
  };
  using Entry = std::pair<const std::string, Slot>;

  /// Serves the node's newest request, at its cursor: upstream checks, staging, the history, then a run, which
  /// takes `room` (granted by the budget's line) or reserves its own; `room` returns when nothing runs.
  void serve(
      Entry& entry, const RecipeGraph::Node& node, Trigger trigger,
      std::optional<InFlightBudget::Reservation> room = std::nullopt);
  void submit(
      Entry& entry, const RecipeGraph::Node& node, uint64_t epoch, InputStager::Staged staged,
      InFlightBudget::Reservation room);
  /// GUI thread, once per submitted run: publishes it unless its meaning changed, returns its room, serves its
  /// readers and its own current request (behind the budget's waiters when there are any), and serves the budget's
  /// line last.
  void finish(Job& job, const EnginePool::Outcome& outcome);
  /// Publishes a run's result, or records its failure and, when it serves the current request, clears what the ring
  /// shows at its stamp. True when the published result serves the current request.
  bool apply(Entry& entry, const RecipeGraph::Node& node, Job& job);
  /// The result at `stamp` serves the newest request: drops the tombstones it supersedes and settles ready.
  void settleServed(Entry& entry, Timestamp stamp);
  /// Runs nothing, clears what the ring shows at the cursor (D6) and settles the newest request as missing input.
  /// With `keep_finding`, a finding that shows a result keeps it and settles ready instead: its inputs left the window
  /// (an upstream's results followed its own cursor, retention), they did not change. A removed upstream passes false;
  /// a new revision or epoch has already dropped the finding.
  void settleMissing(
      Entry& entry, const RecipeGraph::Node& node, uint64_t epoch, std::string reason, bool keep_finding = true);
  /// Publishes every output unavailable at `stamp` when an available result is shown there, or when nothing is and
  /// the node's results were dropped, since a layer may still draw one (`ResultHistory::resultsDropped`) (D6).
  void clearShown(
      const std::string& key, const RecipeGraph::Node& node, Timestamp requested_t, Timestamp stamp, uint64_t epoch,
      const std::string& reason);
  /// How far what the ring shows at the cursor is behind `target`; nullopt when it shows nothing available.
  [[nodiscard]] std::optional<Timestamp> lagBehind(const Entry& entry, Timestamp target) const;
  /// Sets the state, reports a change and, for a settled request, serves the readers waiting on it.
  void setState(
      Entry& entry, NodeState::Status status, std::string detail = {}, std::optional<Timestamp> lag = std::nullopt);
  /// Keeps the slot in the budget's line exactly while it waits for room.
  void setWait(Entry& entry, Wait wait);
  /// Serves the readers of `entry` that wait for it (`waiting_only`), or every one with no run in flight or turn.
  void serveReaders(const Entry& entry, bool waiting_only);
  /// The budget's line granted `entry` its room: serves it with that room.
  void admit(Entry& entry, InFlightBudget::Reservation room);
  [[nodiscard]] uint64_t epochOf(DatasetId dataset) const;
  [[nodiscard]] uint64_t tfRevisionOf(DatasetId dataset) const;

  const ObjectStore& store_;
  const RecipeGraph& graph_;
  ResultHistory& history_;
  const InputStager& stager_;
  EnginePool& pool_;
  InFlightBudget& budget_;
  Hooks hooks_;
  std::map<std::string, Slot, std::less<>> slots_;
  std::map<DatasetId, int> held_;  ///< the datasets `hold` holds, with their nesting depth
  /// Expires with the scheduler: a publish arriving later does nothing.
  std::shared_ptr<const int> lifetime_ = std::make_shared<const int>(0);
};

}  // namespace PJ
