// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string_view>
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
  /// What the session wires in.
  struct Services {
    RecipeGraph::InputResolver resolver;
    InputStager::ScalarSampler scalars;
    InputStager::ParserLookup parsers;
    NodeScheduler::Hooks scheduling;
    ResultHistory::Limits limits;
  };

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

  [[nodiscard]] const ResultHistory& history() const {
    return history_;
  }

 private:
  RecipeGraph graph_;
  ResultHistory history_;
  InputStager stager_;
  NodeScheduler scheduler_;  ///< last: destroyed first, cancelling its runs while the rest is alive
};

}  // namespace PJ
