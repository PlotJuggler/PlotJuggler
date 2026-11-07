// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/OnDemandService.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace PJ {
namespace {

NodeScheduler::RunFunction& runForTest() {
  static NodeScheduler::RunFunction run;
  return run;
}

std::function<std::chrono::steady_clock::time_point()>& clockForTest() {
  static std::function<std::chrono::steady_clock::time_point()> now;
  return now;
}

NodeScheduler::Hooks withRun(NodeScheduler::Hooks hooks) {
  if (!hooks.run) {
    hooks.run = runForTest();
  }
  return hooks;
}

/// Calls `visit` on `node` and on each upstream it reads, recursively, each once.
template <typename Visit>
void visitWithUpstreams(
    const RecipeGraph& graph, const RecipeGraph::Node& node, std::unordered_set<std::string_view>& visited,
    Visit& visit) {
  visit(node);
  for (const RecipeGraph::Binding& binding : node.bindings) {
    const RecipeGraph::Node* upstream = graph.find(binding.upstream);
    if (upstream != nullptr && visited.insert(binding.upstream).second) {
      visitWithUpstreams(graph, *upstream, visited, visit);
    }
  }
}

template <typename Visit>
void visitWithUpstreams(const RecipeGraph& graph, const RecipeGraph::Node& node, Visit visit) {
  std::unordered_set<std::string_view> visited;
  visitWithUpstreams(graph, node, visited, visit);
}

GridRunner::Hooks evaluationHooks(const OnDemandService::Services& services) {
  const NodeScheduler::Hooks scheduling = withRun(services.scheduling);
  GridRunner::Hooks hooks;
  hooks.scalar_changes = services.scalar_changes;
  hooks.tf = scheduling.tf;
  hooks.epoch = scheduling.epoch;
  hooks.run = scheduling.run;
  hooks.image_decoder = scheduling.image_decoder;
  hooks.video_source = scheduling.video_source;
  hooks.now = clockForTest();
  return hooks;
}

}  // namespace

void OnDemandService::setRunForTest(NodeScheduler::RunFunction run) {
  runForTest() = std::move(run);
}

void OnDemandService::setClockForTest(std::function<std::chrono::steady_clock::time_point()> now) {
  clockForTest() = std::move(now);
}

OnDemandService::OnDemandService(
    ObjectStore& store, DataEngine& engine, EnginePool& pool, InFlightBudget& budget, Services services)
    : pool_(pool),
      graph_(std::move(services.resolver), std::move(services.qualifier)),
      history_(store, services.limits),
      stager_(store, history_, std::move(services.scalars), std::move(services.parsers)),
      evaluations_(store, graph_, history_, stager_, pool, budget, evaluationHooks(services)),
      scheduler_(store, graph_, history_, stager_, pool, budget, withRun(services.scheduling)),
      series_(
          engine, graph_, evaluations_,
          {std::move(services.series_changed), std::move(services.series_state_changed),
           [this](const RecipeGraph::Node& node) { return seriesLiveEdge(node); }}),
      installed_(std::move(services.installed)),
      epoch_(std::move(services.scheduling.epoch)),
      tf_revision_(std::move(services.scheduling.tf_revision)) {}

Expected<std::vector<ObjectTopicId>> OnDemandService::install(RecipeGraph::Recipe recipe) {
  const std::string key = recipe.key();
  std::optional<RecipeGraph::Node> previous;
  if (const RecipeGraph::Node* before = graph_.find(key)) {
    previous = *before;
  }
  if (Status upserted = graph_.upsert(std::move(recipe)); !upserted) {
    return unexpected(upserted.error());
  }
  const RecipeGraph::Node& node = *graph_.find(key);
  if (previous && node.revision == previous->revision) {
    return std::vector<ObjectTopicId>{};
  }
  Status series = series_.check(node);
  Expected<std::vector<ObjectTopicId>> retired =
      series ? history_.install(node) : Expected<std::vector<ObjectTopicId>>(unexpected(std::move(series.error())));
  if (!retired) {
    // The history kept the previous revision, so the graph does too and nothing is superseded. A writer racing the
    // history's name check is its one exception: the history then refuses to publish until the next install.
    std::string error = std::move(retired.error());
    if (previous) {
      graph_.reinstate(std::move(*previous));
    } else {
      (void)remove(key);
    }
    return unexpected(std::move(error));
  }
  const auto names = demand_.find(node.dataset);
  series_.install(node, names != demand_.end() && names->second.contains(key));
  const std::vector<std::string> readers = graph_.dependentsOf({key});
  for (const std::string& reader : readers) {
    history_.clear(reader);
  }
  scheduler_.supersede(key);
  for (const std::string& reader : readers) {
    supersede(reader);
  }
  if (installed_) {
    installed_(key, restoring_ ? std::vector<ObjectTopicId>{} : *retired);
  }
  return retired;
}

std::vector<ObjectTopicId> OnDemandService::remove(std::string_view key) {
  if (!graph_.remove(key)) {
    return {};
  }
  scheduler_.forget(key);
  if (const auto live = live_.find(key); live != live_.end()) {
    live_.erase(live);
  }
  std::vector<ObjectTopicId> retired = history_.forget(key);
  series_.remove(key);
  for (const std::string& reader : graph_.dependentsOf({std::string(key)})) {
    supersede(reader);
  }
  return retired;
}

bool OnDemandService::clearExcept(RestoreIntent intent, const std::unordered_set<std::string>& keep) {
  restoring_ = true;
  const std::vector<std::string> removed = graph_.clearRecipes(intent, keep);
  for (const std::string& key : removed) {
    scheduler_.forget(key);
    live_.erase(key);
    series_.park(key);
  }
  cleared_.insert(cleared_.end(), removed.begin(), removed.end());
  return !removed.empty();
}

std::vector<ObjectTopicId> OnDemandService::sweepUnclaimed() {
  restoring_ = false;
  std::vector<std::string> unclaimed;
  std::vector<ObjectTopicId> retired;
  for (const std::string& key : std::exchange(cleared_, {})) {
    if (graph_.find(key) == nullptr) {
      std::ranges::move(history_.forget(key), std::back_inserter(retired));
      unclaimed.push_back(key);
    }
  }
  series_.sweepParked();
  for (const std::string& reader : graph_.dependentsOf(unclaimed)) {
    supersede(reader);
  }
  return retired;
}

void OnDemandService::rerun(const std::vector<std::string>& keys) {
  std::vector<std::string> affected = graph_.dependentsOf(keys);
  affected.insert(affected.end(), keys.begin(), keys.end());
  for (const std::string& key : affected) {
    history_.clear(key);
  }
  for (const std::string& key : affected) {
    supersede(key);
  }
}

void OnDemandService::supersede(std::string_view reader) {
  scheduler_.supersede(reader);
  series_.invalidate(reader);
}

void OnDemandService::setDemand(DatasetId dataset, std::vector<std::string> topic_names) {
  static const std::unordered_set<std::string> kNone;
  if (topic_names.empty()) {
    demand_.erase(dataset);
  } else {
    demand_[dataset] = {std::make_move_iterator(topic_names.begin()), std::make_move_iterator(topic_names.end())};
  }
  const auto names = demand_.find(dataset);
  series_.setDemand(dataset, names != demand_.end() ? names->second : kNone);
}

bool OnDemandService::isDemanded(std::string_view key) const {
  const RecipeGraph::Node* node = graph_.find(key);
  return node != nullptr && !node->recipe.pinned_t && displayed(*node);
}

bool OnDemandService::displayed(const RecipeGraph::Node& node) const {
  const auto names = demand_.find(node.dataset);
  if (names == demand_.end()) {
    return false;
  }
  const std::string key = node.recipe.key();
  return std::ranges::any_of(node.recipe.outputs, [&](const scripting::OutputSpec& output) {
    return output.type == scripting::OutputType::kObject && names->second.contains(key + "/" + output.name);
  });
}

void OnDemandService::requestAt(std::string_view key, Timestamp t) {
  scheduler_.requestAt(key, t);
  if (const RecipeGraph::Node* node = graph_.find(key); node != nullptr && node->recipe.pinned_t) {
    live_.insert_or_assign(std::string(key), liveRequestOf(*node, *node->recipe.pinned_t));
  }
}

std::vector<std::string> OnDemandService::demandedKeys() const {
  std::vector<std::string> keys;
  for (const auto& entry : demand_) {
    std::ranges::move(demandedKeys(entry.first), std::back_inserter(keys));
  }
  return keys;
}

std::vector<std::string> OnDemandService::demandedKeys(DatasetId dataset) const {
  std::vector<std::string> keys;
  if (demand_.contains(dataset)) {
    keys = graph_.keysOf(dataset);
    std::erase_if(keys, [this](const std::string& key) { return !isDemanded(key); });
  }
  return keys;
}

void OnDemandService::withdraw(std::string_view key) {
  const RecipeGraph::Node* node = graph_.find(key);
  if (node == nullptr || !scheduler_.withdraw(key)) {
    return;
  }
  for (const RecipeGraph::Binding& binding : node->bindings) {
    const RecipeGraph::Node* upstream = graph_.find(binding.upstream);
    if (upstream != nullptr && !upstream->recipe.pinned_t && !isDemanded(binding.upstream)) {
      withdraw(binding.upstream);
    }
  }
}

std::optional<Timestamp> OnDemandService::liveEdgeOf(std::string_view key) const {
  const RecipeGraph::Node* node = graph_.find(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  std::optional<Timestamp> edge;
  visitWithUpstreams(graph_, *node, [&](const RecipeGraph::Node& current) {
    if (const std::optional<Timestamp> newest = stager_.newestInputStamp(current)) {
      edge = std::max(edge.value_or(*newest), *newest);
    }
  });
  return edge;
}

std::optional<Timestamp> OnDemandService::seriesLiveEdge(const RecipeGraph::Node& node) const {
  std::optional<Timestamp> edge = std::numeric_limits<Timestamp>::max();
  visitWithUpstreams(graph_, node, [&](const RecipeGraph::Node& current) {
    const std::optional<Timestamp> lagging = edge ? stager_.laggingInputStamp(current) : std::nullopt;
    edge = edge && lagging ? std::optional(std::min(*edge, *lagging)) : std::nullopt;
  });
  return edge;
}

OnDemandService::LiveRequest OnDemandService::liveRequestOf(const RecipeGraph::Node& node, Timestamp t) const {
  LiveRequest request;
  request.t = t;
  request.epoch = epoch_ ? epoch_(node.dataset) : 0;
  if (!node.recipe.pinned_t && tf_revision_) {
    request.tf_revision = tf_revision_(node.dataset);
  }
  visitWithUpstreams(graph_, node, [&](const RecipeGraph::Node& current) {
    request.inputs.push_back(current.revision);
    request.complete = stager_.appendRawIdentities(current, t, request.inputs) && request.complete;
  });
  return request;
}

void OnDemandService::followLiveEdge(DatasetId dataset) {
  for (const std::string& key : graph_.keysOf(dataset)) {
    const RecipeGraph::Node* found = graph_.find(key);
    if (found == nullptr) {
      continue;  // a callback of an earlier request removed it
    }
    const RecipeGraph::Node& node = *found;
    const std::optional<Timestamp> t = node.recipe.pinned_t ? node.recipe.pinned_t
                                       : isDemanded(key)    ? liveEdgeOf(key)
                                                            : std::nullopt;
    if (!t) {
      continue;
    }
    LiveRequest request = liveRequestOf(node, *t);
    if (node.recipe.pinned_t && !request.complete) {
      continue;  // an input of the finding left (evicted) or never came: nothing arrived to refresh it with
    }
    LiveRequest& last = live_[key];
    if (last == request) {
      continue;
    }
    last = std::move(request);
    scheduler_.requestAt(key, *t);
  }
  series_.appendLive(dataset);
}

void OnDemandService::restartLiveEdge(DatasetId dataset) {
  for (const std::string& key : demandedKeys(dataset)) {
    live_.erase(key);
  }
  followLiveEdge(dataset);
}

std::vector<RecipeGraph::Binding> OnDemandService::displayedRawInputs(DatasetId dataset) const {
  std::vector<RecipeGraph::Binding> inputs;
  for (const std::string& key : graph_.keysOf(dataset)) {
    const RecipeGraph::Node& root = *graph_.find(key);
    if (!displayed(root) && !series_.isDemanded(key)) {
      continue;
    }
    visitWithUpstreams(graph_, root, [&inputs](const RecipeGraph::Node& node) {
      for (const RecipeGraph::Binding& binding : node.bindings) {
        if (binding.upstream.empty() && std::ranges::find(inputs, binding) == inputs.end()) {
          inputs.push_back(binding);
        }
      }
    });
  }
  return inputs;
}

void OnDemandService::suspend(DatasetId dataset) {
  history_.clearDataset(dataset);
  scheduler_.hold(dataset);
  series_.suspend(dataset);
}

void OnDemandService::resume(DatasetId dataset) {
  scheduler_.release(dataset);
  series_.resume(dataset);
}

}  // namespace PJ
