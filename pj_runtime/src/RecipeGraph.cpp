// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/RecipeGraph.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
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
  return spec.name + ":" + scripting::outputTypeName(spec);
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

/// `RecipeGraph::missingInput`, not walking again the upstreams in `providing`, whose own inputs this walk already
/// found provided, so a shared ancestor is walked once. Its views name bindings of `graph`, which the walk does not
/// change.
std::optional<std::string> missingFrom(
    const RecipeGraph& graph, std::string_view key, std::unordered_set<std::string_view>& providing) {
  const RecipeGraph::Node* node = graph.find(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  for (const RecipeGraph::Binding& binding : node->bindings) {
    if (binding.upstream.empty()) {
      continue;
    }
    const RecipeGraph::Node* producer = graph.find(binding.upstream);
    if (producer == nullptr) {
      return "on-demand: upstream '" + binding.upstream + "' removed";
    }
    const OutputSpec* spec = objectOutput(producer->recipe, binding.name);
    if (spec == nullptr || spec->object_type != binding.object_type || producer->dataset != node->dataset) {
      return "on-demand: upstream '" + binding.upstream + "' no longer provides '" + binding.name + "'";
    }
    if (providing.contains(binding.upstream)) {
      continue;
    }
    if (std::optional<std::string> reason = missingFrom(graph, binding.upstream, providing)) {
      return reason;
    }
    providing.insert(binding.upstream);
  }
  return std::nullopt;
}

/// Whether `node` has a topic input rooted at a name in `topics` (`seriesKeyBelongsToTopic`).
bool readsSeriesOf(const RecipeGraph::Node& node, const std::vector<std::string>& topics) {
  return std::ranges::any_of(node.bindings, [&topics](const RecipeGraph::Binding& b) {
    return b.upstream.empty() &&
           std::ranges::any_of(topics, [&b](const std::string& name) { return seriesKeyBelongsToTopic(b.name, name); });
  });
}

/// A restore never clears or keeps a preview, and history never touches an exempt node.
bool inAuthority(const RecipeGraph::Node& node, RestoreIntent intent) {
  return !node.recipe.ephemeral && !(intent == RestoreIntent::kHistory && node.recipe.history_exempt);
}

/// Whether `json` nests deeper than `scripting::kMaxParamsDepth`, found without building the value: the parse is
/// iterative, while copying or dumping a deeper value would recurse past the GUI thread's stack. Text that is not
/// JSON nests nothing.
bool nestedTooDeep(std::string_view json) {
  using Event = nlohmann::json::parse_event_t;
  bool deep = false;
  (void)nlohmann::json::parse(
      json,
      [&deep](int depth, Event event, const nlohmann::json&) {
        deep = deep ||
               ((event == Event::object_start || event == Event::array_start) && depth >= scripting::kMaxParamsDepth);
        return !deep;
      },
      /*allow_exceptions=*/false);
  return deep;
}

/// The checks that need no graph; returns the compiled script: `installed`'s when it has the same language and source.
Expected<std::shared_ptr<const scripting::CompiledOnDemandScript>> validate(
    const RecipeGraph::Recipe& recipe, const RecipeGraph::Node* installed) {
  if (recipe.owner_plugin.empty() || recipe.user_id.empty()) {
    return unexpected("a recipe needs an owner and an id");
  }
  if (!recipe.ephemeral && recipe.user_id.starts_with('~')) {
    return unexpected("a persistent recipe's id may not start with '~', the previews' namespace");
  }
  if (nestedTooDeep(recipe.params_json)) {
    return unexpected("params_json is nested deeper than " + std::to_string(scripting::kMaxParamsDepth) + " levels");
  }
  if (recipe.inputs.empty()) {
    return unexpected("a recipe needs at least one input");
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
  if (installed != nullptr && installed->recipe.language == recipe.language &&
      installed->recipe.script == recipe.script) {
    return installed->compiled;
  }
  // Python compiles at its first run, on the Python lane: an upsert never waits for the GIL.
  Expected<scripting::CompiledOnDemandScript> compiled = scripting::compileOnDemand(recipe.language, recipe.script);
  if (!compiled) {
    return unexpected(std::move(compiled.error()));
  }
  return std::make_shared<const scripting::CompiledOnDemandScript>(std::move(*compiled));
}

}  // namespace

RecipeGraph::RecipeGraph(InputResolver resolver, QualifierSplitter qualifier)
    : resolver_(std::move(resolver)), qualifier_(std::move(qualifier)) {}

Expected<RecipeGraph::Node> RecipeGraph::bind(Recipe recipe) const {
  const auto reject = [](const std::string& why) { return unexpected("on-demand: " + why); };
  if (recipe.params_json.empty()) {
    recipe.params_json = "{}";  // what a layout parse reads back, so a save round-trips byte for byte
  }
  const std::string key = recipe.key();
  Expected<std::shared_ptr<const scripting::CompiledOnDemandScript>> compiled = validate(recipe, find(key));
  if (!compiled.has_value()) {
    return reject(compiled.error());
  }

  // Each input's candidate datasets: another node's output resolves in that node's dataset. A recorded output of an
  // absent node has none (its candidates stay empty): it constrains nothing and binds as a missing input.
  const std::vector<std::string> readers = dependentsOf({key});
  std::vector<Binding> bindings(recipe.inputs.size());
  std::vector<std::vector<InputCandidate>> candidates(recipe.inputs.size());
  for (std::size_t i = 0; i < recipe.inputs.size(); ++i) {
    const std::string& input = recipe.inputs[i];
    const auto recorded = recipe.recorded_upstreams.find(input);
    UpstreamOutput named = upstreamOutput(input);
    // A qualifier no loaded source explains hides the producer: the recorded upstream names it.
    if (named.producer == nullptr && recorded != recipe.recorded_upstreams.end()) {
      named = upstreamOutput(recorded->second.upstream + "/" + recorded->second.name);
    }
    const std::string* producer = named.producer;
    const bool names_new_output = std::ranges::any_of(
        recipe.outputs, [&](const OutputSpec& output) { return named.output_name == key + "/" + output.name; });
    if (names_new_output || (producer != nullptr && *producer == key)) {
      return reject("input '" + input + "' is this recipe's own output");
    }
    if (producer == nullptr) {
      candidates[i] = resolver_(input);
      const bool on_recipe_dataset =
          !recipe.dataset.has_value() || std::ranges::any_of(candidates[i], [&recipe](const InputCandidate& candidate) {
            return candidate.dataset == *recipe.dataset;
          });
      if (const auto topic = recipe.recorded_topics.find(input);
          (candidates[i].empty() || !on_recipe_dataset) && topic != recipe.recorded_topics.end()) {
        candidates[i] = resolver_(topic->second);
      }
      if (candidates[i].empty()) {
        if (recorded == recipe.recorded_upstreams.end() || recorded->second.upstream == key) {
          return reject("unknown input '" + input + "'");
        }
        bindings[i] = recorded->second;
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
    // A recorded producer was read when it was saved: the old recording's name in the qualifier does not refuse it.
    const bool recorded_producer =
        recorded != recipe.recorded_upstreams.end() && recorded->second.upstream == *producer;
    if (named.qualifier_datasets.has_value() && !recorded_producer &&
        std::ranges::find(*named.qualifier_datasets, upstream.dataset) == named.qualifier_datasets->end()) {
      return reject("input '" + input + "' names another dataset than the one '" + *producer + "' runs on");
    }
    const std::string output = named.output_name.substr(producer->size() + 1);
    bindings[i].upstream = *producer;
    candidates[i] = {{upstream.dataset, objectOutput(upstream.recipe, output)->object_type, output}};
  }

  // A recipe naming its dataset binds there only.
  if (recipe.dataset.has_value()) {
    for (std::size_t i = 0; i < recipe.inputs.size(); ++i) {
      if (candidates[i].empty()) {
        continue;
      }
      std::erase_if(candidates[i], [&recipe](const InputCandidate& c) { return c.dataset != *recipe.dataset; });
      if (candidates[i].empty()) {
        return reject(
            "input '" + recipe.inputs[i] + "' is not on dataset " + std::to_string(*recipe.dataset) +
            ", which the recipe binds");
      }
    }
  }

  // D7: exactly one dataset binds every input.
  std::optional<std::vector<DatasetId>> common;
  for (const auto& input_candidates : candidates) {
    if (input_candidates.empty()) {
      continue;
    }
    if (!common.has_value()) {
      common.emplace();
      for (const InputCandidate& candidate : input_candidates) {
        common->push_back(candidate.dataset);
      }
      continue;
    }
    std::erase_if(*common, [&input_candidates](DatasetId dataset) {
      return std::ranges::none_of(
          input_candidates, [dataset](const InputCandidate& c) { return c.dataset == dataset; });
    });
  }
  if (!common.has_value()) {
    if (!recipe.dataset.has_value()) {
      return reject("no input resolves, and the recipe names no dataset");
    }
    common = std::vector<DatasetId>{*recipe.dataset};
  }
  if (common->empty()) {
    return reject("inputs do not all resolve on one dataset (a recipe reads one dataset)");
  }
  if (common->size() > 1) {
    return reject("inputs resolve on more than one dataset (ambiguous); qualify them with the dataset's name");
  }
  const DatasetId dataset = common->front();
  for (std::size_t i = 0; i < recipe.inputs.size(); ++i) {
    if (candidates[i].empty()) {
      continue;
    }
    const auto chosen = std::ranges::find(candidates[i], dataset, &InputCandidate::dataset);
    bindings[i].object_type = chosen->object_type;
    bindings[i].name = chosen->topic;
    if (chosen->object_type != sdk::BuiltinObjectType::kNone && sdk::describe(chosen->object_type) == nullptr) {
      return reject(
          "input '" + recipe.inputs[i] + "' has type " + std::string(sdk::name(chosen->object_type)) +
          ", which scripts cannot read yet");
    }
  }

  return Node{std::move(recipe), 0, dataset, std::move(bindings), std::move(*compiled)};
}

Status RecipeGraph::upsert(Recipe recipe) {
  const auto reject = [](const std::string& why) { return unexpected("on-demand: " + why); };
  Expected<Node> bound = bind(std::move(recipe));
  if (!bound.has_value()) {
    return unexpected(std::move(bound.error()));
  }
  const Recipe& installed = bound->recipe;
  if (installed.outputs.empty()) {
    return reject("a recipe needs at least one output");
  }
  const std::string key = installed.key();
  const Node* existing = find(key);
  if (existing != nullptr && existing->recipe.owner_plugin != installed.owner_plugin) {
    return reject("the key '" + key + "' is held by another owner's recipe");
  }
  if (existing != nullptr && existing->recipe.history_exempt != installed.history_exempt) {
    return reject("history_exempt cannot change on the installed recipe '" + key + "': remove it and create it again");
  }
  if (installed.ephemeral) {
    // A reader keeps naming a removed upstream, so this also covers a reinstall after a removal.
    for (const auto& [reader, node] : nodes_) {
      if (!node.recipe.ephemeral &&
          std::ranges::any_of(node.bindings, [&key](const Binding& b) { return b.upstream == key; })) {
        return reject("persistent recipe '" + reader + "' reads '" + key + "', which cannot become a preview");
      }
    }
  }
  if (existing != nullptr && sameRecipe(existing->recipe, installed) &&
      existing->recipe.ephemeral == installed.ephemeral && existing->dataset == bound->dataset &&
      existing->bindings == bound->bindings) {
    return okStatus();  // the same meaning: the same revision
  }
  if (installed.pinned_t.has_value() && (existing == nullptr || !existing->recipe.pinned_t.has_value())) {
    const auto pins = std::ranges::count_if(nodes_, [&installed](const auto& entry) {
      return entry.second.recipe.owner_plugin == installed.owner_plugin && entry.second.recipe.pinned_t.has_value();
    });
    if (static_cast<std::size_t>(pins) >= kMaxPinnedPerOwner) {
      return reject(
          "owner '" + installed.owner_plugin + "' reached the cap of " + std::to_string(kMaxPinnedPerOwner) +
          " pinned findings");
    }
  }
  bound->revision = next_revision_++;
  nodes_.insert_or_assign(key, std::move(*bound));
  return okStatus();
}

void RecipeGraph::reinstate(Node node) {
  std::string key = node.recipe.key();
  nodes_.insert_or_assign(std::move(key), std::move(node));
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
  std::unordered_set<std::string_view> providing;
  return missingFrom(*this, key, providing);
}

std::vector<std::string> RecipeGraph::readersOfSeries(const std::vector<std::string>& topics) const {
  std::vector<std::string> keys;
  for (const auto& [key, node] : nodes_) {
    if (readsSeriesOf(node, topics)) {
      keys.push_back(key);
    }
  }
  return keys;
}

std::vector<std::string> RecipeGraph::keysOf(DatasetId dataset) const {
  std::vector<std::string> keys;
  for (const auto& [key, node] : nodes_) {
    if (node.dataset == dataset) {
      keys.push_back(key);
    }
  }
  return keys;
}

std::vector<std::string> RecipeGraph::keys() const {
  std::vector<std::string> keys;
  keys.reserve(nodes_.size());
  for (const auto& entry : nodes_) {
    keys.push_back(entry.first);
  }
  return keys;
}

std::vector<std::string> RecipeGraph::dependentsOf(const std::vector<std::string>& keys) const {
  std::unordered_map<std::string_view, std::vector<const std::string*>> readers;  // upstream -> reader keys
  for (const auto& [key, node] : nodes_) {
    for (const Binding& binding : node.bindings) {
      if (!binding.upstream.empty()) {
        readers[binding.upstream].push_back(&key);
      }
    }
  }
  std::unordered_set<std::string> visited(keys.begin(), keys.end());
  std::vector<std::string> pending = keys;
  std::vector<std::string> dependents;
  while (!pending.empty()) {
    const std::string current = std::move(pending.back());
    pending.pop_back();
    const auto found = readers.find(current);
    if (found == readers.end()) {
      continue;
    }
    for (const std::string* key : found->second) {
      if (visited.insert(*key).second) {
        dependents.push_back(*key);
        pending.push_back(*key);
      }
    }
  }
  std::ranges::sort(dependents);
  return dependents;
}

std::unordered_set<std::string> RecipeGraph::unchangedRecipeKeys(
    const std::vector<Recipe>& wanted, RestoreIntent intent, const std::vector<std::string>& replaced_outputs) const {
  std::unordered_map<std::string, const Recipe*> wanted_by_key;
  wanted_by_key.reserve(wanted.size());
  for (const Recipe& recipe : wanted) {
    wanted_by_key.emplace(recipe.key(), &recipe);
  }
  const auto wanted_entry = [&wanted_by_key](const std::string& key) -> const Recipe* {
    const auto found = wanted_by_key.find(key);
    return found == wanted_by_key.end() ? nullptr : found->second;
  };
  std::unordered_set<std::string> unchanged;
  for (const auto& [key, node] : nodes_) {
    const Recipe* snapshot = wanted_entry(key);
    if (inAuthority(node, intent) && snapshot != nullptr && sameRecipe(node.recipe, *snapshot) &&
        (!snapshot->dataset.has_value() || *snapshot->dataset == node.dataset) &&
        !readsSeriesOf(node, replaced_outputs)) {
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

std::vector<std::string> RecipeGraph::clearRecipes(RestoreIntent intent, const std::unordered_set<std::string>& keep) {
  std::vector<std::string> removed;
  std::erase_if(nodes_, [&](const auto& entry) {
    const bool clear = inAuthority(entry.second, intent) && !keep.contains(entry.first);
    if (clear) {
      removed.push_back(entry.first);
    }
    return clear;
  });
  return removed;
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

RecipeGraph::UpstreamOutput RecipeGraph::upstreamOutput(std::string_view input) const {
  if (const std::string* producer = producerOf(input); producer != nullptr || !qualifier_) {
    return {producer, std::string(input), std::nullopt};
  }
  std::optional<Qualifier> qualifier = qualifier_(input);
  if (!qualifier.has_value()) {
    return {nullptr, std::string(input), std::nullopt};
  }
  const std::string* producer = producerOf(qualifier->bare);
  return {producer, std::move(qualifier->bare), std::move(qualifier->datasets)};
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
