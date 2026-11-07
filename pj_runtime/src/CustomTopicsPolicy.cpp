// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/CustomTopicsPolicy.h"

#include <vector>

#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/RecipeGraph.h"

namespace PJ {
namespace {

bool isEditor(const QString& editor_id, const std::string& owner) {
  return ExtensionCatalogService::isEditorOwner(editor_id, QString::fromStdString(owner));
}

std::string joined(const std::vector<std::string>& names) {
  std::string text;
  for (const std::string& name : names) {
    text += (text.empty() ? "" : ", ") + name;
  }
  return text;
}

}  // namespace

std::optional<CustomTopicDeleteRefusal> transformDeleteRefusal(
    const DataProcessorService& processors, const RecipeGraph& graph, const BatchEngine& markers, TopicId output_topic,
    const QString& editor_id) {
  using Reason = CustomTopicDeleteRefusal::Reason;
  const std::optional<DataProcessorService::TransformRecipe> transform = processors.transformOwning(output_topic);
  if (!transform.has_value()) {
    return CustomTopicDeleteRefusal{};
  }
  if (!isEditor(editor_id, transform->owner_plugin)) {
    return CustomTopicDeleteRefusal{Reason::kReadOnly, transform->user_id, transform->owner_plugin, {}};
  }
  for (const auto& reader : processors.transformsDependingOn(transform->output_topic_ids)) {
    if (!reader.owner_plugin.empty() && !isEditor(editor_id, reader.owner_plugin)) {
      return CustomTopicDeleteRefusal{Reason::kForeignReader, transform->user_id, reader.owner_plugin, reader.user_id};
    }
  }
  const std::vector<std::string> removed = processors.removedOutputNames(transform->key);
  std::vector<std::string> exempt = graph.exemptDependentsOf(removed);
  const std::vector<std::string> exempt_markers = markers.exemptDependentsOf(removed);
  exempt.insert(exempt.end(), exempt_markers.begin(), exempt_markers.end());
  if (!exempt.empty()) {
    return CustomTopicDeleteRefusal{Reason::kExemptReader, transform->user_id, {}, joined(exempt)};
  }
  return std::nullopt;
}

std::optional<CustomTopicDeleteRefusal> onDemandDeleteRefusal(
    const RecipeGraph& graph, const std::string& key, const QString& editor_id) {
  using Reason = CustomTopicDeleteRefusal::Reason;
  const RecipeGraph::Node* node = graph.find(key);
  if (node == nullptr) {
    return CustomTopicDeleteRefusal{};
  }
  const RecipeGraph::Recipe& recipe = node->recipe;
  if (!isEditor(editor_id, recipe.owner_plugin)) {
    return CustomTopicDeleteRefusal{Reason::kReadOnly, recipe.user_id, recipe.owner_plugin, {}};
  }
  std::vector<std::string> outputs;
  outputs.reserve(recipe.outputs.size());
  for (const scripting::OutputSpec& output : recipe.outputs) {
    outputs.push_back(key + "/" + output.name);
  }
  if (const std::vector<std::string> exempt = graph.exemptDependentsOf(outputs); !exempt.empty()) {
    return CustomTopicDeleteRefusal{Reason::kExemptReader, recipe.user_id, {}, joined(exempt)};
  }
  return std::nullopt;
}

}  // namespace PJ
