// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/OnDemandService.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace PJ {
namespace {

NodeScheduler::RunFunction& runForTest() {
  static NodeScheduler::RunFunction run;
  return run;
}

NodeScheduler::Hooks withRun(NodeScheduler::Hooks hooks) {
  if (!hooks.run) {
    hooks.run = runForTest();
  }
  return hooks;
}

}  // namespace

void OnDemandService::setRunForTest(NodeScheduler::RunFunction run) {
  runForTest() = std::move(run);
}

OnDemandService::OnDemandService(ObjectStore& store, EnginePool& pool, InFlightBudget& budget, Services services)
    : graph_(std::move(services.resolver)),
      history_(store, services.limits),
      stager_(store, history_, std::move(services.scalars), std::move(services.parsers)),
      scheduler_(store, graph_, history_, stager_, pool, budget, withRun(std::move(services.scheduling))),
      installed_(std::move(services.installed)) {}

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
  auto retired = history_.install(node);
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
  const std::vector<std::string> readers = graph_.dependentsOf({key});
  for (const std::string& reader : readers) {
    history_.clear(reader);
  }
  scheduler_.supersede(key);
  for (const std::string& reader : readers) {
    scheduler_.supersede(reader);
  }
  if (installed_) {
    installed_(key, *retired);
  }
  return retired;
}

std::vector<ObjectTopicId> OnDemandService::remove(std::string_view key) {
  if (!graph_.remove(key)) {
    return {};
  }
  scheduler_.forget(key);
  std::vector<ObjectTopicId> retired = history_.forget(key);
  for (const std::string& reader : graph_.dependentsOf({std::string(key)})) {
    scheduler_.supersede(reader);
  }
  return retired;
}

void OnDemandService::setDemand(DatasetId dataset, std::vector<std::string> topic_names) {
  if (topic_names.empty()) {
    demand_.erase(dataset);
  } else {
    demand_[dataset] = {std::make_move_iterator(topic_names.begin()), std::make_move_iterator(topic_names.end())};
  }
}

bool OnDemandService::isDemanded(std::string_view key) const {
  const RecipeGraph::Node* node = graph_.find(key);
  if (node == nullptr || node->recipe.pinned_t) {
    return false;
  }
  const auto names = demand_.find(node->dataset);
  if (names == demand_.end()) {
    return false;
  }
  return std::ranges::any_of(node->recipe.outputs, [&](const scripting::OutputSpec& output) {
    return output.type == scripting::OutputType::kObject &&
           names->second.contains(std::string(key) + "/" + output.name);
  });
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

void OnDemandService::suspend(DatasetId dataset) {
  history_.clearDataset(dataset);
  scheduler_.hold(dataset);
}

}  // namespace PJ
