// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/OnDemandService.h"

#include <string>
#include <utility>

namespace PJ {

OnDemandService::OnDemandService(ObjectStore& store, EnginePool& pool, InFlightBudget& budget, Services services)
    : graph_(std::move(services.resolver)),
      history_(store, services.limits),
      stager_(store, history_, std::move(services.scalars), std::move(services.parsers)),
      scheduler_(store, graph_, history_, stager_, pool, budget, std::move(services.scheduling)) {}

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

}  // namespace PJ
