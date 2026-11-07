// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/GridRunner.h"
#include "pj_runtime/InFlightBudget.h"
#include "pj_runtime/InputStager.h"
#include "pj_runtime/NodeScheduler.h"
#include "pj_runtime/RecipeGraph.h"
#include "pj_runtime/ResultHistory.h"
#include "pj_runtime/SeriesRunner.h"

namespace PJ {

/// The on-demand engine (docs/derived_objects/ARCHITECTURE.md §3, §8): owns the recipe graph, the result
/// history, the input stager, the node scheduler and the explicit evaluations' `GridRunner`, and keeps only the
/// orchestration that spans them: install, remove, and the requests and state queries it forwards to the scheduler. A
/// layout restore stays in `DerivedObjects`. GUI thread only; what runs on the pool is the scheduler's (see
/// `NodeScheduler.h`).
class OnDemandService {
 public:
  /// After an install that took a new revision of `key`, with the topics it retired (GUI thread). Inside a restore
  /// (from `clearExcept` to `sweepUnclaimed`) it gets none: the restore announces its topics once, after the sweep.
  using InstalledCallback = std::function<void(const std::string& key, const std::vector<ObjectTopicId>& retired)>;

  /// What the session wires in.
  struct Services {
    RecipeGraph::InputResolver resolver;
    RecipeGraph::QualifierSplitter qualifier;  ///< empty: only the bare `<key>/<output>` names a node's output
    InputStager::ScalarSampler scalars;
    InputStager::ParserLookup parsers;
    NodeScheduler::Hooks scheduling;  ///< an empty `run`: the one `setRunForTest` set, else `scripting::runOnDemand`
    /// The explicit evaluations' scalar change instants; their TF, epoch and run are `scheduling`'s.
    GridRunner::ScalarChangeLookup scalar_changes;
    ResultHistory::Limits limits;
    InstalledCallback installed;
    SeriesRunner::TopicsChanged series_changed;       ///< rows written to, or a series topic emptied or retired
    SeriesRunner::StateChanged series_state_changed;  ///< a series' state changed (its progress, for 11b)
  };

  /// Test seam: the run of every service built afterwards whose `Services` names none; empty restores
  /// `scripting::runOnDemand`. GUI thread.
  static void setRunForTest(NodeScheduler::RunFunction run);
  /// Test seam: the time budgets' clock of every service built afterwards (`GridRunner::Hooks::now`); empty restores
  /// steady_clock. A frozen clock keeps a slow host from ending a series chunk or an evaluation early. GUI thread.
  static void setClockForTest(std::function<std::chrono::steady_clock::time_point()> now);

  /// `store` must outlive the pool's running computes; `engine` (where the series live), the pool and the budget must
  /// outlive the service.
  OnDemandService(ObjectStore& store, DataEngine& engine, EnginePool& pool, InFlightBudget& budget, Services services);

  /// Installs `recipe` (`RecipeGraph::upsert`), registers its outputs (`ResultHistory::install`) and returns the
  /// topics a new revision retired, which the caller announces. A new revision supersedes the node and its
  /// readers: their runs are cancelled, the readers' results dropped (they were computed from the old meaning),
  /// and each one requested before is requested again at its cursor. An identical recipe changes nothing.
  /// Rejected, with nothing changed: whatever `upsert` rejects, and an output topic `ResultHistory::install` cannot
  /// register. The previous recipe stays at its revision, with its topics, its results and its run in flight.
  [[nodiscard]] Expected<std::vector<ObjectTopicId>> install(RecipeGraph::Recipe recipe);
  /// Cancels the node's run, retires its topics (returned, for the caller to announce) and removes it. Its
  /// readers stay installed and are requested again at their cursors, where they now miss the input. Nothing for
  /// an unknown key.
  std::vector<ObjectTopicId> remove(std::string_view key);

  /// `NodeScheduler::requestAt`. A finding's request (at its pin) also records what `followLiveEdge` compares, so a
  /// live tick runs it again only when an input changed after this request.
  void requestAt(std::string_view key, Timestamp t);

  // The restore protocol (docs/derived_objects/ARCHITECTURE.md §13): clear, install each wanted recipe, sweep.

  /// Starts a restore: removes every node in the restore's authority except `keep` (`RecipeGraph::clearRecipes`)
  /// and cancels and forgets their runs; returns whether any was removed. Their topics and results stay and their
  /// readers are not superseded yet: the restore installs the wanted recipes next, and one reinstalled under the same
  /// output names keeps its topic ids. End with `sweepUnclaimed`.
  bool clearExcept(RestoreIntent intent, const std::unordered_set<std::string>& keep);
  /// Retires the topics of the nodes `clearExcept` removed that no install reclaimed, and requests their remaining
  /// readers (a preview, an exempt node) again at their cursors, where they now miss the input. Returns the retired
  /// topics, for the caller to announce once.
  std::vector<ObjectTopicId> sweepUnclaimed();
  /// Drops the results of `keys` and of their readers, cancels their runs and requests each one requested before
  /// again at its cursor: what they read changed under the same recipe (a processor output a restore re-created,
  /// read by a node outside the restore's authority). An unknown key is skipped.
  void rerun(const std::vector<std::string>& keys);

  [[nodiscard]] std::optional<NodeScheduler::NodeState> state(std::string_view key) const {
    return scheduler_.state(key);
  }
  /// The node whose output `topic` is; nullopt for any other topic.
  [[nodiscard]] std::optional<std::string> keyOfTopic(ObjectTopicId topic) const {
    return history_.keyOf(topic);
  }
  /// `state` of the node whose output `topic` is.
  [[nodiscard]] std::optional<NodeScheduler::NodeState> stateOfTopic(ObjectTopicId topic) const {
    const std::optional<std::string> key = keyOfTopic(topic);
    return key ? state(*key) : std::nullopt;
  }

  /// Replaces the names of `dataset`'s topics something displays (the demand tracker's active set). A node that is
  /// not pinned is demanded while one of its object output topics is among them; its upstreams are the scheduler's.
  void setDemand(DatasetId dataset, std::vector<std::string> topic_names);
  [[nodiscard]] bool isDemanded(std::string_view key) const;
  /// Every demanded key, sorted by dataset, then key.
  [[nodiscard]] std::vector<std::string> demandedKeys() const;
  /// The demanded keys of `dataset`, sorted.
  [[nodiscard]] std::vector<std::string> demandedKeys(DatasetId dataset) const;
  /// `NodeScheduler::withdraw`: nothing demands the node any more. Its upstreams follow when nothing else needs them
  /// (no demand of their own, no other reader with a request), recursively; a finding is never withdrawn this way.
  void withdraw(std::string_view key);

  /// The newest stamp among the raw inputs `key` reads, its upstreams' included (`InputStager::newestInputStamp`): the
  /// time the docks show a live dataset at. nullopt for an unknown key, or when no raw input has a sample.
  [[nodiscard]] std::optional<Timestamp> liveEdgeOf(std::string_view key) const;
  /// A live tick of `dataset`, after its writes (ARCHITECTURE §16): requests each demanded node at its live edge and
  /// each finding at its pin, only when what the request would read differs from the node's last one: the time, the
  /// epoch, the revisions and raw input identities of the node and of every upstream it reads and, off a pin, the TF
  /// revision. So an unchanged tick requests nothing, a same-stamp replacement is seen even behind an upstream, a
  /// TF-only tick retries what read the TF (under the session's snapshot brake), and a finding runs again only when an
  /// input arrives at or before its pin; one with a raw input missing there (evicted, or not come yet) is left alone.
  void followLiveEdge(DatasetId dataset);
  /// `followLiveEdge` with the dataset's demanded nodes' last live requests forgotten, so each is requested at its live
  /// edge though nothing changed: when the transport starts following the edge again after the cursor moved them
  /// elsewhere (their results there may have left the ring).
  void restartLiveEdge(DatasetId dataset);
  /// The raw inputs (bindings with no upstream) the displayed nodes of `dataset` read (findings included: an input
  /// can still arrive before a pin), their upstreams' included.
  [[nodiscard]] std::vector<RecipeGraph::Binding> displayedRawInputs(DatasetId dataset) const;

  /// Before a change of `dataset`'s storage that changes what its inputs mean, with the caller moving its epoch: drops
  /// the results of its nodes, cancels their runs and holds their requests (`NodeScheduler::hold`), and empties its
  /// series (`SeriesRunner::suspend`). Pair with `resume`.
  void suspend(DatasetId dataset);
  /// Ends one `suspend`: the last serves every node of the dataset requested before, at its cursor, and replays its
  /// wanted series.
  void resume(DatasetId dataset);

  [[nodiscard]] const RecipeGraph& graph() const {
    return graph_;
  }
  [[nodiscard]] const ResultHistory& history() const {
    return history_;
  }
  /// The explicit evaluations (D19): a node of `graph()` or one it binds (`RecipeGraph::bind`).
  [[nodiscard]] GridRunner& evaluations() {
    return evaluations_;
  }
  /// The series of the nodes that have one (ARCHITECTURE §15): their demand, requests, stops and state. The service
  /// keeps them in step with installs, removals, restores, the demand set, dataset holds and live ticks.
  [[nodiscard]] SeriesRunner& series() {
    return series_;
  }
  [[nodiscard]] const SeriesRunner& series() const {
    return series_;
  }
  /// The pool its runs share: what a consumer of `evaluations()` moves its own heavy GUI-thread work to.
  [[nodiscard]] EnginePool& pool() {
    return pool_;
  }

 private:
  /// What a live tick last requested a node with.
  struct LiveRequest {
    Timestamp t = 0;
    uint64_t epoch = 0;
    uint64_t tf_revision = 0;  ///< 0 for a finding
    /// Per node, the node first and then each upstream it reads: its revision, then its raw inputs' identities at
    /// `t` (`InputStager::appendRawIdentities`), so a change behind an upstream is seen before the upstream ran.
    std::vector<uint64_t> inputs;
    bool complete = true;  ///< every raw input had something at or before `t`

    bool operator==(const LiveRequest&) const = default;
  };

  /// What a request of `node` at `t` would read now.
  [[nodiscard]] LiveRequest liveRequestOf(const RecipeGraph::Node& node, Timestamp t) const;
  /// `SeriesRunner::LiveEdge`: the oldest of the newest samples of the raw inputs of `node` and of its upstreams.
  [[nodiscard]] std::optional<Timestamp> seriesLiveEdge(const RecipeGraph::Node& node) const;
  /// One of the node's object outputs is displayed (the demand set), whether or not it is pinned.
  [[nodiscard]] bool displayed(const RecipeGraph::Node& node) const;
  /// What `reader` computed no longer holds: the scheduler serves it again and its series, if it has one, replays.
  void supersede(std::string_view reader);

  EnginePool& pool_;
  RecipeGraph graph_;
  ResultHistory history_;
  InputStager stager_;
  GridRunner evaluations_;   ///< after the components it reads
  NodeScheduler scheduler_;  ///< after the graph, the history and the stager: cancels its runs while they are alive
  SeriesRunner series_;      ///< after the evaluations it submits its chunks to
  InstalledCallback installed_;
  NodeScheduler::EpochProvider epoch_;             ///< the scheduler's: `followLiveEdge` stages under it
  NodeScheduler::TfRevisionProvider tf_revision_;  ///< the scheduler's; empty: 0
  std::map<DatasetId, std::unordered_set<std::string>> demand_;
  std::map<std::string, LiveRequest, std::less<>> live_;
  std::vector<std::string> cleared_;  ///< what `clearExcept` removed, until `sweepUnclaimed`
  bool restoring_ = false;            ///< from `clearExcept` to `sweepUnclaimed`
};

}  // namespace PJ
