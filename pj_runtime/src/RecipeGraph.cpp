// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/RecipeGraph.h"

#include <algorithm>
#include <utility>

#include "SeriesKey.h"
#include "TextCompare.h"
#include "pj_base/builtin/field_table_registry.hpp"

namespace PJ {
namespace {

using scripting::OutputSpec;
using scripting::OutputType;

/// `spec` as `parseOutputSpec` text, so the one parser validates a declaration built in code.
std::string declaration(const OutputSpec& spec) {
  switch (spec.type) {
    case OutputType::kNumber:
      return spec.name + ":number";
    case OutputType::kString:
      return spec.name + ":string";
    case OutputType::kObject:
      return spec.name + ":" + std::string(sdk::name(spec.object_type));
  }
  return spec.name;
}

/// Every field a layout carries, script and params ignoring line endings (the key is equal already).
bool sameRecipe(const RecipeGraph::Recipe& live, const RecipeGraph::Recipe& wanted) {
  return live.language == wanted.language && sameTextIgnoringLineEndings(live.script, wanted.script) &&
         sameTextIgnoringLineEndings(live.params_json, wanted.params_json) && live.inputs == wanted.inputs &&
         live.outputs == wanted.outputs && live.pinned_t == wanted.pinned_t &&
         live.history_exempt == wanted.history_exempt && live.query_time == wanted.query_time;
}

const OutputSpec* objectOutput(const RecipeGraph::Recipe& recipe, std::string_view name) {
  const auto found = std::ranges::find_if(recipe.outputs, [name](const OutputSpec& output) {
    return output.type == OutputType::kObject && output.name == name;
  });
  return found == recipe.outputs.end() ? nullptr : &*found;
}

/// A restore never clears or keeps a preview, and history never touches an exempt node.
bool inAuthority(const RecipeGraph::Node& node, RestoreIntent intent) {
  return !node.recipe.ephemeral && !(intent == RestoreIntent::kHistory && node.recipe.history_exempt);
}

/// The checks that need no graph; returns the compiled script.
Expected<scripting::CompiledScript> validate(const RecipeGraph::Recipe& recipe) {
  if (recipe.owner_plugin.empty() || recipe.user_id.empty()) {
    return unexpected("a recipe needs an owner and an id");
  }
  if (recipe.language != "luau") {
    return unexpected("unsupported language '" + recipe.language + "' (only 'luau')");
  }
  if (recipe.inputs.empty() || recipe.outputs.empty()) {
    return unexpected("a recipe needs at least one input and one output");
  }
  std::unordered_set<std::string> names;
  for (const OutputSpec& output : recipe.outputs) {
    const Expected<OutputSpec> parsed = scripting::parseOutputSpec(declaration(output));
    if (!parsed.has_value()) {
      return unexpected(parsed.error());
    }
    if (*parsed != output) {
      return unexpected("invalid output '" + declaration(output) + "'");
    }
    if (!names.insert(output.name).second) {
      return unexpected("duplicate output name '" + output.name + "'");
    }
  }
  for (const std::string& input : recipe.inputs) {
    if (input.empty()) {
      return unexpected("an input name must not be empty");
    }
    if (!names.insert(input).second) {
      return unexpected("input '" + input + "' is duplicated or names one of this recipe's outputs");
    }
  }
  return scripting::compileOnDemandScript(recipe.script);
}

}  // namespace

RecipeGraph::RecipeGraph(InputResolver resolver) : resolver_(std::move(resolver)) {}

Status RecipeGraph::upsert(Recipe recipe) {
  const auto reject = [](const std::string& why) { return unexpected("on-demand: " + why); };
  Expected<scripting::CompiledScript> compiled = validate(recipe);
  if (!compiled.has_value()) {
    return reject(compiled.error());
  }
  const std::string key = recipe.key();
  if (recipe.ephemeral) {
    // A reader keeps naming a removed upstream, so this also covers a reinstall after a removal.
    for (const auto& [reader, node] : nodes_) {
      if (!node.recipe.ephemeral &&
          std::ranges::any_of(node.bindings, [&key](const Binding& b) { return b.upstream == key; })) {
        return reject("persistent recipe '" + reader + "' reads '" + key + "', which cannot become a preview");
      }
    }
  }

  // Each input's candidate datasets: another node's output resolves in that node's dataset.
  const std::vector<std::string> readers = dependentsOf({key});
  std::vector<Binding> bindings(recipe.inputs.size());
  std::vector<std::vector<InputCandidate>> candidates(recipe.inputs.size());
  for (std::size_t i = 0; i < recipe.inputs.size(); ++i) {
    const std::string& input = recipe.inputs[i];
    const bool names_new_output =
        std::ranges::any_of(recipe.outputs, [&](const OutputSpec& output) { return input == key + "/" + output.name; });
    const std::string* producer = producerOf(input);
    if (names_new_output || (producer != nullptr && *producer == key)) {
      return reject("input '" + input + "' is this recipe's own output");
    }
    if (producer == nullptr) {
      candidates[i] = resolver_(input);
      if (candidates[i].empty()) {
        return reject("unknown input '" + input + "'");
      }
      continue;
    }
    if (std::ranges::binary_search(readers, *producer)) {
      return reject("input from '" + *producer + "' would create a cycle back to '" + key + "'");
    }
    const Node& upstream = nodes_.find(*producer)->second;
    if (!recipe.ephemeral && upstream.recipe.ephemeral) {
      return reject(
          "input '" + input + "' is an output of preview '" + *producer +
          "', which ends with its panel. Create that recipe as a persistent one first.");
    }
    const std::string output = input.substr(producer->size() + 1);
    bindings[i].upstream = *producer;
    candidates[i] = {{upstream.dataset, objectOutput(upstream.recipe, output)->object_type, output}};
  }

  // D7: exactly one dataset binds every input.
  std::vector<DatasetId> common;
  for (const InputCandidate& candidate : candidates.front()) {
    common.push_back(candidate.dataset);
  }
  for (const auto& input_candidates : candidates) {
    std::erase_if(common, [&input_candidates](DatasetId dataset) {
      return std::ranges::none_of(
          input_candidates, [dataset](const InputCandidate& c) { return c.dataset == dataset; });
    });
  }
  if (common.empty()) {
    return reject("inputs do not all resolve on one dataset (a recipe reads one dataset)");
  }
  if (common.size() > 1) {
    return reject("inputs resolve on more than one dataset (ambiguous); qualify them with the dataset's name");
  }
  const DatasetId dataset = common.front();
  for (std::size_t i = 0; i < recipe.inputs.size(); ++i) {
    const auto chosen = std::ranges::find(candidates[i], dataset, &InputCandidate::dataset);
    bindings[i].object_type = chosen->object_type;
    bindings[i].name = chosen->topic;
    if (chosen->object_type != sdk::BuiltinObjectType::kNone && sdk::describe(chosen->object_type) == nullptr) {
      return reject(
          "input '" + recipe.inputs[i] + "' has type " + std::string(sdk::name(chosen->object_type)) +
          ", which scripts cannot read yet");
    }
  }

  const Node* existing = find(key);
  if (existing != nullptr && sameRecipe(existing->recipe, recipe) && existing->recipe.ephemeral == recipe.ephemeral &&
      existing->dataset == dataset && existing->bindings == bindings) {
    return okStatus();  // the same meaning: the same revision
  }
  if (recipe.pinned_t.has_value() && (existing == nullptr || !existing->recipe.pinned_t.has_value())) {
    const auto pins = std::ranges::count_if(nodes_, [&recipe](const auto& entry) {
      return entry.second.recipe.owner_plugin == recipe.owner_plugin && entry.second.recipe.pinned_t.has_value();
    });
    if (static_cast<std::size_t>(pins) >= kMaxPinnedPerOwner) {
      return reject(
          "owner '" + recipe.owner_plugin + "' reached the cap of " + std::to_string(kMaxPinnedPerOwner) +
          " pinned findings");
    }
  }
  nodes_.insert_or_assign(
      key, Node{std::move(recipe), next_revision_++, dataset, std::move(bindings), std::move(*compiled)});
  return okStatus();
}

Status RecipeGraph::remove(std::string_view key) {
  const auto found = nodes_.find(key);
  if (found == nodes_.end()) {
    return unexpected("on-demand: unknown recipe '" + std::string(key) + "'");
  }
  nodes_.erase(found);
  return okStatus();
}

Status RecipeGraph::dismiss(std::string_view key) {
  const Node* node = find(key);
  if (node == nullptr || !node->recipe.pinned_t.has_value()) {
    return unexpected("on-demand: '" + std::string(key) + "' is not a pinned finding");
  }
  return remove(key);
}

bool RecipeGraph::dismissAll(std::string_view owner_plugin) {
  return std::erase_if(nodes_, [owner_plugin](const auto& entry) {
           return entry.second.recipe.owner_plugin == owner_plugin && entry.second.recipe.pinned_t.has_value();
         }) > 0;
}

bool RecipeGraph::removeEphemerals(const std::unordered_set<std::string>& keys) {
  return std::erase_if(nodes_, [&keys](const auto& entry) {
           return entry.second.recipe.ephemeral && keys.contains(entry.first);
         }) > 0;
}

const RecipeGraph::Node* RecipeGraph::find(std::string_view key) const {
  const auto found = nodes_.find(key);
  return found == nodes_.end() ? nullptr : &found->second;
}

std::optional<std::string> RecipeGraph::missingInput(std::string_view key) const {
  const Node* node = find(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  for (const Binding& binding : node->bindings) {
    if (binding.upstream.empty()) {
      continue;
    }
    const Node* producer = find(binding.upstream);
    if (producer == nullptr) {
      return "on-demand: upstream '" + binding.upstream + "' removed";
    }
    const OutputSpec* spec = objectOutput(producer->recipe, binding.name);
    if (spec == nullptr || spec->object_type != binding.object_type || producer->dataset != node->dataset) {
      return "on-demand: upstream '" + binding.upstream + "' no longer provides '" + binding.name + "'";
    }
    // ponytail: re-walks shared ancestors (exponential on deep diamonds); memoize if recipe graphs grow large.
    if (std::optional<std::string> reason = missingInput(binding.upstream)) {
      return reason;
    }
  }
  return std::nullopt;
}

std::vector<std::string> RecipeGraph::dependentsOf(const std::vector<std::string>& keys) const {
  std::unordered_set<std::string> visited(keys.begin(), keys.end());
  std::vector<std::string> pending = keys;
  std::vector<std::string> dependents;
  while (!pending.empty()) {
    const std::string current = std::move(pending.back());
    pending.pop_back();
    for (const auto& [key, node] : nodes_) {
      const bool reads =
          std::ranges::any_of(node.bindings, [&current](const Binding& b) { return b.upstream == current; });
      if (reads && visited.insert(key).second) {
        dependents.push_back(key);
        pending.push_back(key);
      }
    }
  }
  std::ranges::sort(dependents);
  return dependents;
}

std::unordered_set<std::string> RecipeGraph::unchangedRecipeKeys(
    const std::vector<Recipe>& wanted, RestoreIntent intent) const {
  const auto wanted_entry = [&wanted](const std::string& key) -> const Recipe* {
    const auto found = std::ranges::find_if(wanted, [&key](const Recipe& recipe) { return recipe.key() == key; });
    return found == wanted.end() ? nullptr : &*found;
  };
  std::unordered_set<std::string> unchanged;
  for (const auto& [key, node] : nodes_) {
    const Recipe* snapshot = wanted_entry(key);
    if (inAuthority(node, intent) && snapshot != nullptr && sameRecipe(node.recipe, *snapshot)) {
      unchanged.insert(key);
    }
  }
  // An upstream the restore clears, re-creates or creates makes its readers run again, transitively.
  const auto restored = [&](const std::string& upstream) {
    const Node* producer = find(upstream);
    return producer == nullptr ? wanted_entry(upstream) != nullptr
                               : inAuthority(*producer, intent) && !unchanged.contains(upstream);
  };
  while (std::erase_if(unchanged, [&](const std::string& key) {
           return std::ranges::any_of(
               find(key)->bindings, [&](const Binding& b) { return !b.upstream.empty() && restored(b.upstream); });
         }) > 0) {}
  return unchanged;
}

void RecipeGraph::clearRecipes(RestoreIntent intent, const std::unordered_set<std::string>& keep) {
  std::erase_if(
      nodes_, [&](const auto& entry) { return inAuthority(entry.second, intent) && !keep.contains(entry.first); });
}

bool RecipeGraph::hasHistoryExemptRecipes() const {
  return std::ranges::any_of(nodes_, [](const auto& entry) { return entry.second.recipe.history_exempt; });
}

std::vector<std::string> RecipeGraph::exemptDependentsOf(const std::vector<std::string>& removed) const {
  std::vector<std::string> hit;
  for (const auto& [key, node] : nodes_) {
    // A topic binding reads its bare topic, an upstream edge the bare `<upstream>/<output>` topic.
    if (std::ranges::any_of(node.bindings, [&removed](const Binding& b) {
          const std::string read = b.upstream.empty() ? b.name : b.upstream + "/" + b.name;
          return std::ranges::any_of(
              removed, [&read](const std::string& name) { return seriesKeyBelongsToTopic(read, name); });
        })) {
      hit.push_back(key);
    }
  }
  const std::vector<std::string> downstream = dependentsOf(hit);  // excludes `hit` itself
  hit.insert(hit.end(), downstream.begin(), downstream.end());
  std::vector<std::string> outputs;
  for (const std::string& key : hit) {
    const Recipe& recipe = find(key)->recipe;
    if (recipe.ephemeral || !recipe.history_exempt) {
      continue;
    }
    for (const OutputSpec& output : recipe.outputs) {
      outputs.push_back(key + "/" + output.name);
    }
  }
  std::ranges::sort(outputs);
  return outputs;
}

const std::string* RecipeGraph::producerOf(std::string_view input) const {
  for (const auto& [key, node] : nodes_) {
    if (input.size() > key.size() && input.starts_with(key) && input[key.size()] == '/' &&
        objectOutput(node.recipe, input.substr(key.size() + 1)) != nullptr) {
      return &key;
    }
  }
  return nullptr;
}

}  // namespace PJ
