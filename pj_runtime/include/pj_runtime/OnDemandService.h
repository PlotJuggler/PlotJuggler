// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

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
#include "pj_runtime/InFlightBudget.h"
#include "pj_runtime/InputStager.h"
#include "pj_runtime/NodeScheduler.h"
#include "pj_runtime/RecipeGraph.h"
#include "pj_runtime/ResultHistory.h"

namespace PJ {

/// The on-demand engine (docs/derived_objects/ARCHITECTURE.md §3, §8): owns the recipe graph, the result
/// history, the input stager and the node scheduler, and keeps only the orchestration that spans them: install,
/// remove, and the requests and state queries it forwards to the scheduler. A layout restore stays in
/// `DerivedObjects`. GUI thread only; what runs on the pool is the scheduler's (see `NodeScheduler.h`).
class OnDemandService {
 public:
  /// After an install that took a new revision of `key`, with the topics it retired (GUI thread).
  using InstalledCallback = std::function<void(const std::string& key, const std::vector<ObjectTopicId>& retired)>;

  /// What the session wires in.
  struct Services {
    RecipeGraph::InputResolver resolver;
    InputStager::ScalarSampler scalars;
    InputStager::ParserLookup parsers;
    NodeScheduler::Hooks scheduling;  ///< an empty `run`: the one `setRunForTest` set, else the Luau run
    ResultHistory::Limits limits;
    InstalledCallback installed;
  };

  /// Test seam: the run of every service built afterwards whose `Services` names none; empty restores the Luau
  /// run. GUI thread.
  static void setRunForTest(NodeScheduler::RunFunction run);

  /// `store` must outlive the pool's running computes; the pool and the budget must outlive the service.
  OnDemandService(ObjectStore& store, EnginePool& pool, InFlightBudget& budget, Services services);

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

  void requestAt(std::string_view key, Timestamp t) {
    scheduler_.requestAt(key, t);
  }
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

  /// Before a change of `dataset`'s storage that changes what its inputs mean, with the caller moving its epoch: drops
  /// the results of its nodes, cancels their runs and holds their requests (`NodeScheduler::hold`). Pair with `resume`.
  void suspend(DatasetId dataset);
  /// Ends one `suspend`: the last serves every node of the dataset requested before, at its cursor.
  void resume(DatasetId dataset) {
    scheduler_.release(dataset);
  }

  [[nodiscard]] const RecipeGraph& graph() const {
    return graph_;
  }
  [[nodiscard]] const ResultHistory& history() const {
    return history_;
  }

 private:
  RecipeGraph graph_;
  ResultHistory history_;
  InputStager stager_;
  NodeScheduler scheduler_;  ///< after the graph, the history and the stager: cancels its runs while they are alive
  InstalledCallback installed_;
  std::map<DatasetId, std::unordered_set<std::string>> demand_;
};

}  // namespace PJ
