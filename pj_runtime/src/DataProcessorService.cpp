// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DataProcessorService.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_base/type_tree.hpp"
#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/derived_engine.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/processor_siso_adapter.hpp"
#include "pj_datastore/sample.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/lua_mimo_transform.h"
#include "pj_scripting/lua_siso_transform.h"
#include "pj_scripting/python_engine.h"
#include "pj_scripting/script_engine.h"

namespace PJ {

namespace {
std::vector<std::string> inputFieldPaths(const DataEngine& engine, const TopicStorage& storage);
std::string normalizedFieldPath(std::string path);
}  // namespace

DataProcessorService::DataProcessorService(DataEngine& engine)
    : engine_(engine), derived_(std::make_unique<DerivedEngine>(engine)) {}

DataProcessorService::DataProcessorService(SessionManager& session)
    : engine_(session.dataEngine()), session_(&session), derived_(std::make_unique<DerivedEngine>(engine_)) {}

DataProcessorService::~DataProcessorService() = default;

Expected<DataProcessorService::FilterHandle> DataProcessorService::applyFilter(
    TopicId input_topic_id, DatasetId dataset_id, std::string_view processor_id, std::string output_name,
    std::size_t input_column_index) {
  // Resolve the id to a Luau filter class through the installed catalogue.
  std::unique_ptr<proc::DataProcessor> processor;
  if (filter_catalogue_ && filter_catalogue_->find(processor_id) != nullptr) {
    if (auto made = filter_catalogue_->makeProcessor(processor_id, "{}"); made.has_value()) {
      processor = std::move(made).value();
    }
  }
  if (!processor) {
    return PJ::unexpected("DataProcessorService: unknown processor id '" + std::string(processor_id) + "'");
  }
  return applyFilter(input_topic_id, dataset_id, std::move(processor), std::move(output_name), input_column_index);
}

Expected<DataProcessorService::FilterHandle> DataProcessorService::applyFilter(
    TopicId input_topic_id, DatasetId dataset_id, std::unique_ptr<proc::DataProcessor> processor,
    std::string output_name, std::size_t input_column_index) {
  if (!processor) {
    return PJ::unexpected("DataProcessorService: null processor");
  }

  // Shared so the host could later keep its own handle (preview/reconfigure);
  // the engine owns the adapter, the adapter shares the processor.
  std::shared_ptr<proc::DataProcessor> shared = std::move(processor);
  auto node = derived_->addSisoTransform(
      input_topic_id, output_name, dataset_id, std::make_unique<proc::ProcessorSisoAdapter>(shared),
      input_column_index);
  if (!node) {
    return PJ::unexpected(node.error());
  }

  // Eager run over the already-committed input (file path: a single full pass).
  derived_->onSourceCommitted(std::vector<TopicId>{input_topic_id});
  if (Status scheduled = derived_->scheduleAll(); !scheduled.has_value()) {
    // Roll back the just-registered node + output topic. Leaving them reserves the output
    // name forever (no recipe is stored yet, so clearAllFilters can't reach it). Capture the
    // outputs BEFORE removeNode — afterwards outputTopics(*node) returns {}.
    const std::vector<TopicId> outs = derived_->outputTopics(*node);
    (void)derived_->removeNode(*node);
    for (TopicId out : outs) {
      engine_.retireTopic(out);
    }
    return PJ::unexpected(scheduled.error());
  }

  const std::vector<TopicId> outputs = derived_->outputTopics(*node);
  if (outputs.empty()) {
    (void)derived_->removeNode(*node);
    return PJ::unexpected("DataProcessorService: processor produced no output topic");
  }

  // Record the recipe so the editor can re-open on this output and edit it.
  FilterRecipe recipe;
  recipe.node_id = *node;
  recipe.output_topic_id = outputs.front();
  recipe.input_topic_id = input_topic_id;
  recipe.input_column_index = input_column_index;
  {
    const auto engine_lock = engine_.lockEngine();
    if (const TopicStorage* input = engine_.getTopicStorage(input_topic_id)) {
      const std::vector<std::string> paths = inputFieldPaths(engine_, *input);
      if (input_column_index < paths.size()) {
        recipe.input_field_path = paths[input_column_index];
      }
    }
  }
  recipe.dataset_id = dataset_id;
  recipe.processor_id = shared->id();
  recipe.output_name = output_name;
  recipe.processor = shared;
  // Capture the filter's defining module source from the LIVE processor so a saved
  // layout embeds it as <source_fallback> (M7) and reopens on a machine without this
  // filter installed. Reading it off the processor (not re-deriving from the
  // catalogue) keeps it authoritative even for a filter restored from an embedded
  // source whose id is absent from this machine's catalogue — so re-saving keeps it.
  // Native C++ builtins are not LuaSisoTransforms (no source) and round-trip by id.
  if (const auto* lua = dynamic_cast<const scripting::LuaSisoTransform*>(shared.get())) {
    recipe.filter_source = lua->sourceText();
  }
  recipes_[recipe.output_topic_id] = std::move(recipe);

  return FilterHandle{*node, outputs.front(), dataset_id, std::move(output_name)};
}

Status DataProcessorService::removeFilter(NodeId node_id) {
  for (const auto& [output_topic_id, recipe] : recipes_) {
    if (recipe.node_id != node_id) {
      continue;
    }
    if (Status removed_dependents = removeProcessorsDependingOn({output_topic_id}); !removed_dependents.has_value()) {
      return removed_dependents;
    }
    return removeFilterOnly(node_id);
  }
  return PJ::unexpected("DataProcessorService: filter node " + std::to_string(node_id) + " not found");
}

Status DataProcessorService::removeFilterOnly(NodeId node_id) {
  for (auto it = recipes_.begin(); it != recipes_.end(); ++it) {
    if (it->second.node_id == node_id) {
      const TopicId output_topic_id = it->second.output_topic_id;
      if (Status removed = derived_->removeNode(node_id); !removed.has_value()) {
        return removed;
      }
      recipes_.erase(it);
      engine_.retireTopic(output_topic_id);
      return PJ::okStatus();
    }
  }
  return PJ::unexpected("DataProcessorService: filter node " + std::to_string(node_id) + " not found");
}

void DataProcessorService::clearAllFilters() {
  std::vector<NodeId> nodes;
  nodes.reserve(recipes_.size());
  for (const auto& [output_topic_id, recipe] : recipes_) {
    (void)output_topic_id;
    nodes.push_back(recipe.node_id);
  }
  for (const NodeId node_id : nodes) {
    const bool still_live = std::any_of(
        recipes_.begin(), recipes_.end(), [node_id](const auto& entry) { return entry.second.node_id == node_id; });
    if (still_live) {
      (void)removeFilter(node_id);
    }
  }
}

bool DataProcessorService::hasHistoryExemptTransforms() const {
  return std::any_of(transform_recipes_.begin(), transform_recipes_.end(), [](const auto& entry) {
    return entry.second.history_exempt;
  });
}

void DataProcessorService::clearTransforms(RestoreIntent intent, const std::unordered_set<std::string>& keep_keys) {
  const bool keep_history_exempt = intent == RestoreIntent::kHistory;
  std::vector<std::string> keys;
  keys.reserve(transform_recipes_.size());
  for (const auto& [key, recipe] : transform_recipes_) {
    if (!(keep_history_exempt && recipe.history_exempt) && keep_keys.count(key) == 0) {
      keys.push_back(key);
    }
  }
  for (const std::string& key : keys) {
    const bool still_live = transform_recipes_.find(key) != transform_recipes_.end();
    if (still_live) {
      (void)removeTransform(key);
    }
  }
}

std::unordered_set<std::string> DataProcessorService::unchangedTransformKeys(
    const std::vector<TransformRecipe>& wanted, RestoreIntent intent) const {
  const auto same_binding = [](const TransformInputBinding& lhs, const TransformInputBinding& rhs) {
    return lhs.dataset_id == rhs.dataset_id && lhs.dataset_source == rhs.dataset_source &&
           lhs.topic_name == rhs.topic_name && lhs.field_path == rhs.field_path && lhs.column_index == rhs.column_index;
  };
  const auto same_persisted = [&same_binding](const TransformRecipe& live, const TransformRecipe& snapshot) {
    return live.owner_plugin == snapshot.owner_plugin && live.user_id == snapshot.user_id &&
           live.history_exempt == snapshot.history_exempt && live.inputs == snapshot.inputs &&
           live.outputs == snapshot.outputs && live.script == snapshot.script &&
           live.params_json == snapshot.params_json && live.backend == snapshot.backend &&
           live.api_version == snapshot.api_version && live.backend_version == snapshot.backend_version &&
           !snapshot.input_bindings.empty() &&
           std::equal(
               live.input_bindings.begin(), live.input_bindings.end(), snapshot.input_bindings.begin(),
               snapshot.input_bindings.end(), same_binding);
  };
  std::unordered_set<std::string> unchanged;
  for (const auto& [key, live] : transform_recipes_) {
    if (live.ephemeral || (intent == RestoreIntent::kHistory && live.history_exempt)) {
      continue;
    }
    const auto snapshot =
        std::find_if(wanted.begin(), wanted.end(), [&key](const auto& candidate) { return candidate.key == key; });
    if (snapshot != wanted.end() && same_persisted(live, *snapshot)) {
      unchanged.insert(key);
    }
  }
  return unchanged;
}

std::vector<TopicId> DataProcessorService::advanceOnCommit(const std::vector<TopicId>& changed_inputs) {
  // Run whenever there is ANY eager node — per-curve filters OR named transforms
  // (createTransform). Streaming a transform-only session still needs this path,
  // so the early-out must consider transform_recipes_ too, not just recipes_.
  if (changed_inputs.empty() || (recipes_.empty() && transform_recipes_.empty())) {
    return {};
  }
  // Mark dependent nodes dirty for the just-committed inputs, then run them.
  // scheduleAll only re-runs dirty nodes (incremental watermark), so a stateful
  // node folds just the new tail and its accumulator persists across commits.
  derived_->onSourceCommitted(changed_inputs);
  // Best-effort: a processor/script failure must not abort ingest. The node
  // surfaces its own failed state; we still notify whatever advanced.
  (void)derived_->scheduleAll();

  // Re-notify every filter AND transform output (covers chained nodes too); plots re-read.
  std::vector<TopicId> outputs;
  outputs.reserve(recipes_.size() + transform_recipes_.size());
  for (const auto& [out_tid, recipe] : recipes_) {
    (void)recipe;
    outputs.push_back(out_tid);
  }
  for (const auto& [key, recipe] : transform_recipes_) {
    (void)key;
    for (const TopicId out_tid : recipe.output_topic_ids) {
      outputs.push_back(out_tid);
    }
  }
  return outputs;
}

namespace {
// Index of the single leaf column whose normalized path equals `normalized_wanted`;
// `ambiguous` distinguishes several-matches from none (callers word their errors).
std::optional<std::size_t> uniqueColumnForFieldPath(
    const std::vector<std::string>& paths, const std::string& normalized_wanted, bool& ambiguous) {
  ambiguous = false;
  std::optional<std::size_t> match;
  for (std::size_t index = 0; index < paths.size(); ++index) {
    if (normalizedFieldPath(paths[index]) != normalized_wanted) {
      continue;
    }
    if (match.has_value()) {
      ambiguous = true;
      return std::nullopt;
    }
    match = index;
  }
  return match;
}
}  // namespace

Expected<std::vector<TopicId>> DataProcessorService::rebindAndRecomputeForReplacedSources(
    const std::vector<TopicId>& replaced_inputs) {
  std::vector<TopicId> affected_outputs;
  if (replaced_inputs.empty() || (recipes_.empty() && transform_recipes_.empty())) {
    return affected_outputs;
  }

  const std::unordered_set<TopicId> replaced(replaced_inputs.begin(), replaced_inputs.end());
  struct BindingChange {
    NodeId node_id = 0;
    DerivedEngine::InputBindingState prior;
    DerivedEngine::InputBindingState desired;
    std::optional<TopicId> filter_output;
    std::string transform_key;
  };
  std::vector<BindingChange> binding_changes;

  // Resolve the entire binding set before mutating any node. One missing field
  // must not leave an earlier node rebound against the new schema. One lock
  // scope makes the multi-step read atomic (inner acquisitions are recursive
  // owner re-locks).
  const auto resolution_lock = engine_.lockEngine();
  const auto stage_if_changed = [this, &binding_changes](
                                    NodeId node_id, std::vector<std::size_t> columns,
                                    std::optional<TopicId> filter_output, std::string transform_key) -> Status {
    auto prior = derived_->inputBindingState(node_id);
    if (!prior.has_value()) {
      return PJ::unexpected(prior.error());
    }
    if (prior->columns == columns) {
      return PJ::okStatus();
    }
    auto desired = derived_->resolvedInputBindingState(node_id, columns);
    if (!desired.has_value()) {
      return PJ::unexpected(desired.error());
    }
    binding_changes.push_back(
        BindingChange{
            .node_id = node_id,
            .prior = std::move(*prior),
            .desired = std::move(*desired),
            .filter_output = filter_output,
            .transform_key = std::move(transform_key),
        });
    return PJ::okStatus();
  };

  std::unordered_map<TopicId, std::vector<std::string>> paths_by_topic;
  for (const auto& [output_topic_id, recipe] : recipes_) {
    if (replaced.count(recipe.input_topic_id) == 0 || recipe.input_field_path.empty()) {
      continue;
    }
    auto paths = paths_by_topic.find(recipe.input_topic_id);
    if (paths == paths_by_topic.end()) {
      const TopicStorage* storage = engine_.getTopicStorage(recipe.input_topic_id);
      if (storage == nullptr) {
        return PJ::unexpected("DataProcessorService: filter input disappeared during reload");
      }
      paths = paths_by_topic.emplace(recipe.input_topic_id, inputFieldPaths(engine_, *storage)).first;
    }
    bool ambiguous = false;
    const std::optional<std::size_t> rebound_column =
        uniqueColumnForFieldPath(paths->second, normalizedFieldPath(recipe.input_field_path), ambiguous);
    if (ambiguous) {
      return PJ::unexpected("DataProcessorService: filter input field became ambiguous during reload");
    }
    if (!rebound_column.has_value()) {
      return PJ::unexpected("DataProcessorService: filter input field disappeared during reload");
    }
    if (Status staged = stage_if_changed(recipe.node_id, {*rebound_column}, output_topic_id, {}); !staged.has_value()) {
      return PJ::unexpected(staged.error());
    }
  }

  for (const auto& [key, recipe] : transform_recipes_) {
    const bool touches_replaced = std::any_of(
        recipe.input_topic_ids.begin(), recipe.input_topic_ids.end(),
        [&replaced](TopicId input) { return replaced.count(input) != 0; });
    if (!touches_replaced) {
      continue;
    }
    if (recipe.input_bindings.size() != recipe.input_topic_ids.size()) {
      return PJ::unexpected("pj.data_processors: live transform lost its exact input bindings");
    }
    std::vector<std::size_t> columns;
    columns.reserve(recipe.input_bindings.size());
    for (std::size_t index = 0; index < recipe.input_bindings.size(); ++index) {
      auto rebound = resolveInputBinding(recipe.input_bindings[index]);
      if (!rebound.has_value()) {
        return PJ::unexpected(rebound.error());
      }
      if (rebound->topic_id != recipe.input_topic_ids[index]) {
        return PJ::unexpected("pj.data_processors: transform input topic changed during in-place reload");
      }
      columns.push_back(rebound->column);
    }
    if (Status staged = stage_if_changed(recipe.node_id, std::move(columns), std::nullopt, key); !staged.has_value()) {
      return PJ::unexpected(staged.error());
    }
  }

  std::size_t applied_bindings = 0;
  const auto rollback_bindings = [&]() {
    std::vector<NodeId> restored_nodes;
    while (applied_bindings > 0) {
      const BindingChange& change = binding_changes[--applied_bindings];
      (void)derived_->restoreInputBindingState(change.node_id, change.prior);
      restored_nodes.push_back(change.node_id);
    }
    if (!restored_nodes.empty()) {
      (void)derived_->recomputeBatch(PJ::Span<const NodeId>(restored_nodes));
    }
  };
  for (const BindingChange& change : binding_changes) {
    if (Status rebound = derived_->restoreInputBindingState(change.node_id, change.desired); !rebound.has_value()) {
      rollback_bindings();
      return PJ::unexpected(rebound.error());
    }
    ++applied_bindings;
  }

  std::unordered_set<TopicId> affected(replaced_inputs.begin(), replaced_inputs.end());
  std::unordered_set<NodeId> done;
  forEachDependentProcessor(
      affected, /*include_ephemeral_transforms=*/true,
      [&done, &affected_outputs](TopicId output_topic_id, const FilterRecipe& recipe) {
        done.insert(recipe.node_id);
        affected_outputs.push_back(output_topic_id);
      },
      [&done, &affected_outputs](const std::string&, const TransformRecipe& recipe) {
        done.insert(recipe.node_id);
        affected_outputs.insert(affected_outputs.end(), recipe.output_topic_ids.begin(), recipe.output_topic_ids.end());
      });

  if (!done.empty()) {
    const std::vector<NodeId> affected_nodes(done.begin(), done.end());
    if (Status recomputed = derived_->recomputeBatch(PJ::Span<const NodeId>(affected_nodes)); !recomputed.has_value()) {
      rollback_bindings();
      return PJ::unexpected(recomputed.error());
    }
  }
  for (const BindingChange& change : binding_changes) {
    if (change.filter_output.has_value()) {
      recipes_.at(*change.filter_output).input_column_index = change.desired.columns.front();
      continue;
    }
    auto transform = transform_recipes_.find(change.transform_key);
    if (transform == transform_recipes_.end()) {
      rollback_bindings();
      return PJ::unexpected("pj.data_processors: transform disappeared during reload transaction");
    }
    for (std::size_t index = 0; index < change.desired.columns.size(); ++index) {
      transform->second.input_bindings[index].column_index = change.desired.columns[index];
    }
    if (change.desired.columns.size() == 1) {
      transform->second.input_column_index = change.desired.columns.front();
    }
  }
  return affected_outputs;
}

const DataProcessorService::FilterRecipe* DataProcessorService::filterConfig(TopicId output_topic_id) const {
  auto it = recipes_.find(output_topic_id);
  return it == recipes_.end() ? nullptr : &it->second;
}

std::vector<DataProcessorService::FilterRecipe> DataProcessorService::recipes() const {
  std::vector<FilterRecipe> out;
  out.reserve(recipes_.size());
  for (const auto& [out_tid, recipe] : recipes_) {
    (void)out_tid;
    out.push_back(recipe);
  }
  return out;
}

Status DataProcessorService::updateFilter(NodeId node_id, std::shared_ptr<proc::DataProcessor> processor) {
  if (!processor) {
    return PJ::unexpected("DataProcessorService: null processor");
  }
  if (Status replaced =
          derived_->replaceSisoTransform(node_id, std::make_unique<proc::ProcessorSisoAdapter>(processor));
      !replaced.has_value()) {
    return replaced;
  }
  for (auto& [out_tid, recipe] : recipes_) {
    if (recipe.node_id == node_id) {
      recipe.processor = processor;
      recipe.processor_id = processor->id();
      // Re-capture the defining source (like applyFilter) so a layout re-saved after
      // an in-place edit embeds the NEW filter's <source_fallback>, not the old one.
      if (const auto* lua = dynamic_cast<const scripting::LuaSisoTransform*>(processor.get())) {
        recipe.filter_source = lua->sourceText();
      } else {
        recipe.filter_source.clear();
      }
      break;
    }
  }
  return PJ::okStatus();
}

std::vector<std::pair<std::string, std::string>> DataProcessorService::availableFilters() const {
  std::vector<std::pair<std::string, std::string>> out;
  if (filter_catalogue_) {
    for (const scripting::CatalogueEntry& e : filter_catalogue_->entries()) {
      out.emplace_back(e.cls.id, e.cls.name);
    }
  }
  return out;
}

void DataProcessorService::setFilterCatalogue(std::shared_ptr<scripting::FilterCatalogue> catalogue) {
  filter_catalogue_ = std::move(catalogue);
}

std::unique_ptr<proc::DataProcessor> DataProcessorService::makeRestoredProcessor(
    const std::string& id, const std::string& params_json, const std::string& source_fallback) const {
  // 1. Live catalogue by id (the common case — bundled filters always resolve here).
  if (filter_catalogue_ && filter_catalogue_->find(id) != nullptr) {
    if (auto made = filter_catalogue_->makeProcessor(id, params_json); made.has_value()) {
      return std::move(made).value();
    }
  }
  // 2. The layout's embedded source — self-sufficient, so it must restore even with
  //    NO catalogue installed (a layout that carries its own filter source). Compile
  //    it through the installed catalogue's engine when present, else a transient one
  //    (the built LuaSisoTransform shares ownership of the engine, so it outlives the
  //    temporary catalogue).
  if (!source_fallback.empty()) {
    auto made = filter_catalogue_ ? filter_catalogue_->makeProcessorFromSource(source_fallback, id, params_json)
                                  : scripting::FilterCatalogue(scripting::makeLuauEngine())
                                        .makeProcessorFromSource(source_fallback, id, params_json);
    if (made.has_value()) {
      return std::move(made).value();
    }
  }
  // Unresolved: the caller logs a skip-with-diagnostic.
  return nullptr;
}

// --- Plugin-created transforms (pj.data_processors.v1 host side) ---

namespace {
// Infer the backend from the payload's leading bytes / first-line directive
// (data-only ABI: nothing executable crosses, so the host picks the backend from
// the bytes). WASM/Python are recognized but diagnosed as unavailable in this build.
Expected<std::string> inferTransformBackend(const std::string& script) {
  if (script.size() >= 4 && script.compare(0, 4, "\0asm", 4) == 0) {
    return PJ::unexpected("pj.data_processors: WASM backend is reserved for a future host build");
  }
  const std::size_t nl = script.find('\n');
  const std::string first = script.substr(0, nl == std::string::npos ? script.size() : nl);
  if (first.find("pj-script: luau") != std::string::npos || first.find("pj-script: lua") != std::string::npos) {
    return std::string("luau");
  }
  if (first.find("pj-script: python") != std::string::npos) {
    return std::string("python");
  }
  return PJ::unexpected(
      "pj.data_processors: unrecognized script backend directive (expected '-- pj-script: <lang>' on line 1)");
}

std::vector<std::string> inputFieldPaths(const DataEngine& engine, const TopicStorage& storage) {
  if (storage.descriptor().schema_id != 0) {
    if (const TypeTreeNode* root = engine.typeRegistry().lookup(storage.descriptor().schema_id)) {
      return flattenFieldPaths(*root);
    }
  }
  std::vector<std::string> paths;
  if (!storage.columnDescriptors().empty()) {
    paths.reserve(storage.columnDescriptors().size());
    for (const auto& column : storage.columnDescriptors()) {
      paths.push_back(column.field_path);
    }
    return paths;
  }
  if (!storage.sealedChunks().empty()) {
    const auto& columns = storage.sealedChunks().front().columns;
    paths.reserve(columns.size());
    for (const auto& column : columns) {
      paths.push_back(column.descriptor != nullptr ? column.descriptor->field_path : std::string{});
    }
  }
  return paths;
}

std::string normalizedFieldPath(std::string path) {
  std::replace(path.begin(), path.end(), '/', '.');
  return path;
}
}  // namespace

std::string DataProcessorService::makeTransformKey(std::string_view plugin_id, std::string_view id) {
  std::string key;
  key.reserve(plugin_id.size() + 1 + id.size());
  key.append(plugin_id);
  key.push_back('/');
  key.append(id);
  return key;
}

std::optional<std::pair<TopicId, DatasetId>> DataProcessorService::resolveInputTopic(const std::string& name) const {
  const auto engine_lock = engine_.lockEngine();
  std::vector<std::pair<TopicId, DatasetId>> matches;
  for (const DatasetId ds : engine_.listDatasets()) {
    for (const TopicId tid : engine_.listTopics(ds)) {
      const TopicStorage* storage = engine_.getTopicStorage(tid);
      if (storage != nullptr && storage->descriptor().name == name) {
        matches.emplace_back(tid, ds);
      }
    }
  }
  return matches.size() == 1 ? std::optional{matches.front()} : std::nullopt;
}

std::optional<DataProcessorService::ResolvedInput> DataProcessorService::resolveInputField(
    const std::string& name) const {
  const auto engine_lock = engine_.lockEngine();
  // Exact whole-topic name → the topic's first leaf column.
  if (const auto topic = resolveInputTopic(name)) {
    return ResolvedInput{topic->first, topic->second, 0};
  }
  // Otherwise treat the name as "<topic>/<field-path>": find the LONGEST live topic
  // whose name prefixes `name` (topic names can themselves contain '/'), then map the
  // trailing field path to its flattened-leaf column index.
  std::vector<ResolvedInput> matches;
  std::size_t best_topic_len = 0;
  for (const DatasetId ds : engine_.listDatasets()) {
    for (const TopicId tid : engine_.listTopics(ds)) {
      const TopicStorage* storage = engine_.getTopicStorage(tid);
      if (storage == nullptr) {
        continue;
      }
      const std::string& tn = storage->descriptor().name;
      if (name.size() > tn.size() + 1 && name.compare(0, tn.size(), tn) == 0 && name[tn.size()] == '/' &&
          tn.size() >= best_topic_len) {
        const std::string wanted = normalizedFieldPath(name.substr(tn.size() + 1));
        const std::vector<std::string> paths = inputFieldPaths(engine_, *storage);
        bool ambiguous = false;
        const std::optional<std::size_t> column = uniqueColumnForFieldPath(paths, wanted, ambiguous);
        if (!column.has_value()) {
          continue;  // no unique field on this topic — keep scanning candidates
        }
        if (tn.size() > best_topic_len) {
          matches.clear();
          best_topic_len = tn.size();
        }
        matches.push_back(ResolvedInput{tid, ds, *column});
      }
    }
  }
  return matches.size() == 1 ? std::optional{matches.front()} : std::nullopt;
}

Expected<DataProcessorService::ResolvedInput> DataProcessorService::resolveInputBinding(
    const TransformInputBinding& binding) const {
  if (binding.topic_name.empty()) {
    return PJ::unexpected("pj.data_processors: qualified transform input is missing its topic name");
  }
  if (session_ == nullptr) {
    return PJ::unexpected("pj.data_processors: qualified transform input requires session identity resolution");
  }

  const DatasetIdentityResolution dataset =
      session_->resolveDatasetIdentity(binding.dataset_id, QString::fromStdString(binding.dataset_source), QString());
  if (!dataset.id.has_value()) {
    return PJ::unexpected(
        dataset.ambiguous ? "pj.data_processors: transform input dataset source is ambiguous: " + binding.dataset_source
                          : "pj.data_processors: transform input dataset source not found: " + binding.dataset_source);
  }

  const auto engine_lock = engine_.lockEngine();
  std::vector<TopicId> topic_matches;
  for (const TopicId topic_id : engine_.listTopics(*dataset.id)) {
    const TopicStorage* storage = engine_.getTopicStorage(topic_id);
    if (storage != nullptr && storage->descriptor().name == binding.topic_name) {
      topic_matches.push_back(topic_id);
    }
  }
  if (topic_matches.size() != 1) {
    return PJ::unexpected(
        topic_matches.empty()
            ? "pj.data_processors: transform input topic not found in qualified dataset: " + binding.topic_name
            : "pj.data_processors: transform input topic is ambiguous in qualified dataset: " + binding.topic_name);
  }
  const TopicId topic_id = topic_matches.front();
  const TopicStorage* storage = engine_.getTopicStorage(topic_id);
  if (storage == nullptr) {
    return PJ::unexpected("pj.data_processors: qualified transform input disappeared during restore");
  }

  std::size_t column = binding.column_index;
  const std::vector<std::string> field_paths = inputFieldPaths(engine_, *storage);
  if (!binding.field_path.empty()) {
    bool ambiguous = false;
    const std::optional<std::size_t> field_column =
        uniqueColumnForFieldPath(field_paths, normalizedFieldPath(binding.field_path), ambiguous);
    if (ambiguous) {
      return PJ::unexpected(
          "pj.data_processors: transform input field is ambiguous in qualified topic: " + binding.field_path);
    }
    if (!field_column.has_value()) {
      return PJ::unexpected(
          "pj.data_processors: transform input field not found in qualified topic: " + binding.field_path);
    }
    column = *field_column;
  } else if (!field_paths.empty() && column >= field_paths.size()) {
    return PJ::unexpected(
        "pj.data_processors: transform input column is out of range for qualified topic: " + std::to_string(column));
  }
  return ResolvedInput{topic_id, *dataset.id, column};
}

bool DataProcessorService::outputNameInUse(const std::string& name, const std::string& except_key) const {
  // Another transform already outputs this name?
  for (const auto& [key, recipe] : transform_recipes_) {
    if (key == except_key) {
      continue;
    }
    if (std::find(recipe.outputs.begin(), recipe.outputs.end(), name) != recipe.outputs.end()) {
      return true;
    }
  }
  // A live engine topic (source / filter output / other node) has this name, and it
  // is not this transform's own current output (so an in-place replace is allowed)?
  std::unordered_set<TopicId> own_outputs;
  if (const auto it = transform_recipes_.find(except_key); it != transform_recipes_.end()) {
    own_outputs.insert(it->second.output_topic_ids.begin(), it->second.output_topic_ids.end());
  }
  for (const DatasetId ds : engine_.listDatasets()) {
    for (const TopicId tid : engine_.listTopics(ds)) {
      if (own_outputs.count(tid) > 0) {
        continue;
      }
      const TopicStorage* storage = engine_.getTopicStorage(tid);
      if (storage != nullptr && storage->descriptor().name == name) {
        return true;
      }
    }
  }
  return false;
}

Status DataProcessorService::validateScript(
    std::string_view script, std::string_view language, std::string_view params_json) {
  // Luau and Python backends are available; anything else is rejected.
  const std::string lang = language.empty() ? "luau" : std::string(language);
  if (lang != "luau" && lang != "python") {
    return PJ::unexpected("unsupported script language '" + lang + "' (only 'luau' or 'python' are available)");
  }
  const std::string src(script);
  const std::string params(params_json.empty() ? "{}" : std::string(params_json));
  // Compile through the matching backend. Python uses a transient catalogue (the
  // embedded interpreter is process-global); Luau reuses the installed catalogue.
  Expected<std::unique_ptr<proc::DataProcessor>> compiled =
      (lang == "python")  ? scripting::FilterCatalogue(scripting::makePythonEngine())
                                .makeProcessorFromSource(src, "__validate__", params)
      : filter_catalogue_ ? filter_catalogue_->makeProcessorFromSource(src, "__validate__", params)
                          : scripting::FilterCatalogue(scripting::makeLuauEngine())
                                .makeProcessorFromSource(src, "__validate__", params);
  if (!compiled.has_value()) {
    return PJ::unexpected(compiled.error());  // syntax / module-load error
  }
  // Compilation-only: do NOT run a synthetic test point. A SISO test sample carries
  // only (t, value), so a valid MULTI-INPUT function (using v1..vN) would see those
  // as nil and falsely report "no output". Runtime/empty-output is instead caught by
  // the live preview, which runs the real node with the real inputs.
  return PJ::okStatus();
}

namespace {

// A grouped MIMO output is requested EXPLICITLY as a single structured `outputs`
// entry "<topic>:<field1>,<field2>,...,<fieldN>": the host then materializes ONE
// topic named <topic> with those N named columns (column k = the script's k-th
// result), instead of N separate scalar topics. Returns {topic, [fields]} when the
// single output entry has that shape (non-empty topic + >= 1 non-empty field);
// otherwise {"", {}} and each output stays its own scalar topic. This keeps the
// intent unambiguous (unlike inferring a shared prefix) and entirely host-side —
// the plugin opts in by naming, e.g. outputs = {"rpy:roll,pitch,yaw"}.
std::pair<std::string, std::vector<std::string>> parseGroupedOutput(const std::vector<std::string>& outputs) {
  if (outputs.size() != 1) {
    return {"", {}};
  }
  const std::string& spec = outputs.front();
  const auto colon = spec.find(':');
  if (colon == std::string::npos || colon == 0) {
    return {"", {}};
  }
  const std::string topic = spec.substr(0, colon);
  std::vector<std::string> fields;
  std::size_t start = colon + 1;
  while (start <= spec.size()) {
    const std::size_t comma = spec.find(',', start);
    const std::size_t end = (comma == std::string::npos) ? spec.size() : comma;
    std::string field = spec.substr(start, end - start);
    const auto b = field.find_first_not_of(" \t");
    const auto e = field.find_last_not_of(" \t");
    if (b != std::string::npos) {
      fields.push_back(field.substr(b, e - b + 1));
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  if (fields.empty()) {
    return {"", {}};
  }
  return {topic, std::move(fields)};
}

}  // namespace

Expected<DataProcessorService::TransformRecipe> DataProcessorService::installTransform(TransformRecipe recipe) {
  // 1. Shape: N inputs -> M outputs (SISO is the 1->1 case). Field-level binding
  //    (a column within a multi-column input) is still a follow-up: inputs resolve
  //    to whole scalar topics here.
  if (recipe.inputs.empty() || recipe.outputs.empty()) {
    return PJ::unexpected("pj.data_processors: transform requires at least one input and one output");
  }
  for (const std::string& out_name : recipe.outputs) {
    if (out_name.empty()) {
      return PJ::unexpected("pj.data_processors: transform output name must be non-empty");
    }
  }
  const auto existing = transform_recipes_.find(recipe.key);
  if (existing != transform_recipes_.end() && existing->second.ephemeral != recipe.ephemeral) {
    return PJ::unexpected(
        "pj.data_processors: an existing transform cannot change between persistent and ephemeral lifetime");
  }
  // 2. Backend (also the missing/unavailable-backend diagnostic on restore).
  auto backend = inferTransformBackend(recipe.script);
  if (!backend.has_value()) {
    return PJ::unexpected(backend.error());
  }
  recipe.backend = std::move(backend).value();
  // 3. Output-name collision, per output (excludes this transform's own outputs, so a replace is allowed).
  for (const std::string& out_name : recipe.outputs) {
    if (outputNameInUse(out_name, recipe.key)) {
      return PJ::unexpected("pj.data_processors: output topic name already in use: " + out_name);
    }
  }
  // 4. Resolve every input. Persisted bindings use the shared session identity
  // resolver; legacy and plugin calls remain name-only until canonicalized.
  if (!recipe.input_bindings.empty() && recipe.input_bindings.size() != recipe.inputs.size()) {
    return PJ::unexpected("pj.data_processors: transform input binding count does not match input count");
  }
  const bool had_persisted_bindings = !recipe.input_bindings.empty();
  std::vector<TopicId> input_topic_ids;
  std::vector<std::size_t> input_columns;  // leaf column per input (field-level binding)
  input_topic_ids.reserve(recipe.inputs.size());
  input_columns.reserve(recipe.inputs.size());
  for (std::size_t index = 0; index < recipe.inputs.size(); ++index) {
    const std::string& in_name = recipe.inputs[index];
    std::optional<ResolvedInput> resolved;
    const TransformInputBinding* persisted = had_persisted_bindings ? &recipe.input_bindings[index] : nullptr;
    const bool qualified = persisted != nullptr && (persisted->dataset_id != 0 || !persisted->dataset_source.empty());
    if (qualified) {
      auto exact = resolveInputBinding(*persisted);
      if (!exact.has_value()) {
        return PJ::unexpected(exact.error());
      }
      resolved = std::move(*exact);
    } else {
      resolved = resolveInputField(in_name);
      if (resolved.has_value() && persisted != nullptr) {
        TransformInputBinding local = *persisted;
        const auto engine_lock = engine_.lockEngine();
        const TopicStorage* storage = engine_.getTopicStorage(resolved->topic_id);
        const DatasetInfo* dataset = engine_.getDataset(resolved->dataset_id);
        if (storage == nullptr || dataset == nullptr) {
          return PJ::unexpected("pj.data_processors: legacy transform input disappeared during restore");
        }
        local.dataset_id = resolved->dataset_id;
        local.dataset_source = dataset->source_name;
        local.topic_name = storage->descriptor().name;
        auto rebound = resolveInputBinding(local);
        if (!rebound.has_value()) {
          return PJ::unexpected(rebound.error());
        }
        resolved = std::move(*rebound);
      }
    }
    if (!resolved.has_value()) {
      return PJ::unexpected("pj.data_processors: input topic/field not found: " + in_name);
    }
    if (input_topic_ids.empty()) {
      recipe.dataset_id = resolved->dataset_id;
    }
    input_topic_ids.push_back(resolved->topic_id);
    input_columns.push_back(resolved->column);
  }
  if (!had_persisted_bindings && recipe.inputs.size() == 1 && recipe.input_column_index != 0) {
    input_columns.front() = recipe.input_column_index;
  }

  std::vector<TransformInputBinding> canonical_bindings;
  canonical_bindings.reserve(input_topic_ids.size());
  {
    const auto engine_lock = engine_.lockEngine();
    for (std::size_t index = 0; index < input_topic_ids.size(); ++index) {
      const TopicStorage* storage = engine_.getTopicStorage(input_topic_ids[index]);
      const DatasetInfo* dataset = engine_.getDataset(storage != nullptr ? storage->descriptor().dataset_id : 0);
      if (storage == nullptr || dataset == nullptr) {
        return PJ::unexpected("pj.data_processors: resolved transform input disappeared before installation");
      }
      const std::vector<std::string> paths = inputFieldPaths(engine_, *storage);
      std::string field_path;
      if (input_columns[index] < paths.size()) {
        field_path = paths[input_columns[index]];
      }
      canonical_bindings.push_back(
          TransformInputBinding{
              .dataset_id = dataset->id,
              .dataset_source = dataset->source_name,
              .topic_name = storage->descriptor().name,
              .field_path = std::move(field_path),
              .column_index = input_columns[index],
          });
    }
  }
  recipe.input_bindings = std::move(canonical_bindings);
  recipe.input_topic_ids = input_topic_ids;
  if (recipe.inputs.size() == 1) {
    recipe.input_column_index = input_columns.front();
  }
  // Grouped output: a single structured "topic:f1,f2,..." entry collapses into ONE
  // multi-column topic (parsed host-side, explicit — no prefix guessing). A grouped
  // output is MIMO even when it is the only output entry, so it must not be mistaken
  // for SISO, and its arity (M) is the count of declared fields.
  const auto [mimo_group, mimo_fields] = parseGroupedOutput(recipe.outputs);
  const bool grouped = !mimo_group.empty();
  const bool is_siso = recipe.inputs.size() == 1 && recipe.outputs.size() == 1 && !grouped;
  const std::size_t mimo_num_outputs = grouped ? mimo_fields.size() : recipe.outputs.size();
  // 5. Compile BEFORE tearing down any old node, so a bad script never destroys a
  //    working transform. Use the catalogue's engine when installed, else a transient
  //    one (the built transform shares ownership of the engine, so it outlives it).
  std::shared_ptr<proc::DataProcessor> siso_op;          // set iff is_siso
  std::unique_ptr<scripting::LuaMimoTransform> mimo_op;  // set iff !is_siso
  // Python transforms compile through a transient Python catalogue (the embedded
  // interpreter is process-global). Luau reuses the installed catalogue.
  const bool is_python = recipe.backend == "python";
  if (is_siso) {
    auto compiled = is_python ? scripting::FilterCatalogue(scripting::makePythonEngine())
                                    .makeProcessorFromSource(recipe.script, recipe.user_id, recipe.params_json)
                    : filter_catalogue_
                        ? filter_catalogue_->makeProcessorFromSource(recipe.script, recipe.user_id, recipe.params_json)
                        : scripting::FilterCatalogue(scripting::makeLuauEngine())
                              .makeProcessorFromSource(recipe.script, recipe.user_id, recipe.params_json);
    if (!compiled.has_value()) {
      return PJ::unexpected(compiled.error());
    }
    siso_op = std::move(compiled).value();
  } else {
    auto compiled =
        is_python ? scripting::FilterCatalogue(scripting::makePythonEngine())
                        .makeMimoFromSource(recipe.script, recipe.user_id, recipe.params_json, mimo_num_outputs)
        : filter_catalogue_
            ? filter_catalogue_->makeMimoFromSource(recipe.script, recipe.user_id, recipe.params_json, mimo_num_outputs)
            : scripting::FilterCatalogue(scripting::makeLuauEngine())
                  .makeMimoFromSource(recipe.script, recipe.user_id, recipe.params_json, mimo_num_outputs);
    if (!compiled.has_value()) {
      return PJ::unexpected(compiled.error());
    }
    mimo_op = std::move(compiled).value();
    if (mimo_op->failed()) {  // a create()/compile error puts the instance in a sticky failed state
      return PJ::unexpected(mimo_op->error());
    }
  }
  // 6. Existing-key edits may exchange only the implementation. Keeping the
  // exact graph identity preserves plot keys and downstream edges.
  if (existing != transform_recipes_.end()) {
    const TransformRecipe& old = existing->second;
    std::vector<std::size_t> old_input_columns;
    old_input_columns.reserve(old.input_bindings.size());
    for (const TransformInputBinding& binding : old.input_bindings) {
      old_input_columns.push_back(binding.column_index);
    }
    const auto [old_mimo_group, old_mimo_fields] = parseGroupedOutput(old.outputs);
    (void)old_mimo_fields;
    const bool old_is_siso = old.inputs.size() == 1 && old.outputs.size() == 1 && old_mimo_group.empty();
    const bool topology_compatible = old.input_topic_ids == input_topic_ids && old_input_columns == input_columns &&
                                     old.dataset_id == recipe.dataset_id && old.outputs == recipe.outputs &&
                                     old_is_siso == is_siso;
    if (!topology_compatible) {
      return PJ::unexpected(
          "pj.data_processors: structural replacement is not supported; remove the transform before changing "
          "its inputs or outputs");
    }
    Status replaced = is_siso ? derived_->replaceSisoTransform(
                                    old.node_id, std::make_unique<proc::ProcessorSisoAdapter>(std::move(siso_op)))
                              : derived_->replaceMimoTransform(old.node_id, std::move(mimo_op));
    if (!replaced.has_value()) {
      return PJ::unexpected(replaced.error());
    }
    recipe.node_id = old.node_id;
    recipe.output_topic_ids = old.output_topic_ids;
    existing->second = recipe;
    return recipe;
  }
  // 7. Install the eager node and run it over the committed input(s).
  const std::size_t siso_column = input_columns.front();
  // A grouped MIMO output materializes ONE topic (mimo_group) with mimo_fields as named
  // columns; otherwise each recipe.output is its own scalar topic.
  const std::vector<std::string>& mimo_output_names = grouped ? mimo_fields : recipe.outputs;
  auto node = is_siso ? derived_->addSisoTransform(
                            input_topic_ids.front(), recipe.outputs.front(), recipe.dataset_id,
                            std::make_unique<proc::ProcessorSisoAdapter>(siso_op), siso_column)
                      : derived_->addMimoTransform(
                            input_topic_ids, mimo_output_names, recipe.dataset_id, std::move(mimo_op), input_columns,
                            mimo_group);
  if (!node.has_value()) {
    return PJ::unexpected(node.error());
  }
  derived_->onSourceCommitted(input_topic_ids);
  if (const Status scheduled = derived_->scheduleAll(); !scheduled.has_value()) {
    // Roll back the half-installed node + its output topic (capture outputs first).
    const std::vector<TopicId> outs = derived_->outputTopics(*node);
    (void)derived_->removeNode(*node);
    for (const TopicId out : outs) {
      engine_.retireTopic(out);
    }
    return PJ::unexpected(scheduled.error());
  }
  const std::vector<TopicId> outputs = derived_->outputTopics(*node);
  if (outputs.empty()) {
    (void)derived_->removeNode(*node);
    return PJ::unexpected("pj.data_processors: transform produced no output topic");
  }
  recipe.node_id = *node;
  recipe.output_topic_ids = outputs;
  transform_recipes_[recipe.key] = recipe;
  return recipe;
}

Expected<DataProcessorService::TransformRecipe> DataProcessorService::upsertTransform(
    std::string_view plugin_id, std::string_view id, std::vector<std::string> inputs, std::vector<std::string> outputs,
    std::string_view script, std::string_view params_json, bool ephemeral, std::size_t input_column_index,
    bool history_exempt) {
  TransformRecipe recipe;
  recipe.key = makeTransformKey(plugin_id, id);
  recipe.owner_plugin = std::string(plugin_id);
  recipe.user_id = std::string(id);
  recipe.inputs = std::move(inputs);
  recipe.outputs = std::move(outputs);
  recipe.script = std::string(script);
  recipe.params_json = std::string(params_json);
  recipe.ephemeral = ephemeral;
  recipe.input_column_index = input_column_index;
  recipe.history_exempt = history_exempt;
  return installTransform(std::move(recipe));
}

Status DataProcessorService::removeTransform(std::string_view namespaced_key) {
  const auto it = transform_recipes_.find(std::string(namespaced_key));
  if (it == transform_recipes_.end()) {
    return PJ::unexpected("pj.data_processors: unknown transform '" + std::string(namespaced_key) + "'");
  }
  if (Status removed_dependents = removeProcessorsDependingOn(it->second.output_topic_ids);
      !removed_dependents.has_value()) {
    return removed_dependents;
  }
  return removeTransformOnly(namespaced_key);
}

Status DataProcessorService::removeTransformOnly(std::string_view namespaced_key) {
  const auto it = transform_recipes_.find(std::string(namespaced_key));
  if (it == transform_recipes_.end()) {
    return PJ::unexpected("pj.data_processors: unknown transform '" + std::string(namespaced_key) + "'");
  }
  const NodeId node = it->second.node_id;
  const std::vector<TopicId> outputs = it->second.output_topic_ids;
  if (Status removed = derived_->removeNode(node); !removed.has_value()) {
    return removed;
  }
  transform_recipes_.erase(it);
  // Retire the materialized output so the catalog drops it (removeNode alone leaves a zombie).
  for (const TopicId out : outputs) {
    engine_.retireTopic(out);
  }
  return PJ::okStatus();
}

void DataProcessorService::forEachDependentProcessor(
    std::unordered_set<TopicId>& affected, bool include_ephemeral_transforms,
    const std::function<void(TopicId, const FilterRecipe&)>& on_filter,
    const std::function<void(const std::string&, const TransformRecipe&)>& on_transform) const {
  std::unordered_set<NodeId> seen_filters;
  std::unordered_set<std::string> seen_transforms;
  bool grew = true;
  while (grew) {
    grew = false;
    for (const auto& [output_topic_id, recipe] : recipes_) {
      if (seen_filters.count(recipe.node_id) != 0 || affected.count(recipe.input_topic_id) == 0) {
        continue;
      }
      seen_filters.insert(recipe.node_id);
      affected.insert(output_topic_id);
      on_filter(output_topic_id, recipe);
      grew = true;
    }
    for (const auto& [key, recipe] : transform_recipes_) {
      if (seen_transforms.count(key) != 0 || (!include_ephemeral_transforms && recipe.ephemeral)) {
        continue;
      }
      const bool depends = std::any_of(
          recipe.input_topic_ids.begin(), recipe.input_topic_ids.end(),
          [&affected](TopicId input) { return affected.count(input) != 0; });
      if (!depends) {
        continue;
      }
      seen_transforms.insert(key);
      affected.insert(recipe.output_topic_ids.begin(), recipe.output_topic_ids.end());
      on_transform(key, recipe);
      grew = true;
    }
  }
}

std::vector<TopicId> DataProcessorService::dependentProcessorOutputs(const std::vector<TopicId>& input_topics) const {
  std::unordered_set<TopicId> affected(input_topics.begin(), input_topics.end());
  std::vector<TopicId> outputs;
  forEachDependentProcessor(
      affected, /*include_ephemeral_transforms=*/true,
      [&outputs](TopicId output_topic_id, const FilterRecipe&) { outputs.push_back(output_topic_id); },
      [&outputs](const std::string&, const TransformRecipe& recipe) {
        outputs.insert(outputs.end(), recipe.output_topic_ids.begin(), recipe.output_topic_ids.end());
      });
  return outputs;
}

std::vector<NodeId> DataProcessorService::liveNonExemptNodeIds(const std::unordered_set<std::string>& keep_keys) const {
  std::vector<NodeId> nodes;
  nodes.reserve(recipes_.size() + transform_recipes_.size());
  for (const auto& [output_topic_id, recipe] : recipes_) {
    (void)output_topic_id;
    nodes.push_back(recipe.node_id);  // a FilterRecipe is never exempt
  }
  for (const auto& [key, recipe] : transform_recipes_) {
    if (!recipe.history_exempt && keep_keys.count(key) == 0) {
      nodes.push_back(recipe.node_id);
    }
  }
  return nodes;
}

std::vector<std::string> DataProcessorService::liveNonExemptOutputNames(
    const std::unordered_set<std::string>& keep_keys) const {
  // Names come from the materialized topics, not the recipe declarations: a grouped
  // output "rpy:a,b" materializes ONE topic named "rpy", which is what a marker
  // generator's input key ("rpy/a") is matched against.
  std::vector<std::string> outputs;
  outputs.reserve(recipes_.size() + transform_recipes_.size());
  const auto engine_lock = engine_.lockEngine();
  const auto push_topic_name = [this, &outputs](TopicId topic_id) {
    if (const TopicStorage* storage = engine_.getTopicStorage(topic_id); storage != nullptr) {
      outputs.push_back(storage->descriptor().name);
    }
  };
  for (const auto& [output_topic_id, recipe] : recipes_) {
    (void)recipe;
    push_topic_name(output_topic_id);
  }
  for (const auto& [key, recipe] : transform_recipes_) {
    if (!recipe.history_exempt && keep_keys.count(key) == 0) {
      std::for_each(recipe.output_topic_ids.begin(), recipe.output_topic_ids.end(), push_topic_name);
    }
  }
  std::sort(outputs.begin(), outputs.end());
  outputs.erase(std::unique(outputs.begin(), outputs.end()), outputs.end());
  return outputs;
}

std::vector<std::string> DataProcessorService::exemptDependentsOf(const std::vector<NodeId>& nodes) const {
  const std::unordered_set<NodeId> node_set(nodes.begin(), nodes.end());
  std::unordered_set<TopicId> affected;
  for (const auto& [output_topic_id, recipe] : recipes_) {
    if (node_set.count(recipe.node_id) != 0) {
      affected.insert(output_topic_id);
    }
  }
  for (const auto& [key, recipe] : transform_recipes_) {
    (void)key;
    if (node_set.count(recipe.node_id) != 0) {
      affected.insert(recipe.output_topic_ids.begin(), recipe.output_topic_ids.end());
    }
  }
  std::vector<std::string> exempt_output_names;
  forEachDependentProcessor(
      affected, /*include_ephemeral_transforms=*/true, [](TopicId, const FilterRecipe&) {},
      [&exempt_output_names](const std::string&, const TransformRecipe& recipe) {
        if (recipe.history_exempt) {
          exempt_output_names.insert(exempt_output_names.end(), recipe.outputs.begin(), recipe.outputs.end());
        }
      });
  std::sort(exempt_output_names.begin(), exempt_output_names.end());
  exempt_output_names.erase(
      std::unique(exempt_output_names.begin(), exempt_output_names.end()), exempt_output_names.end());
  return exempt_output_names;
}

Status DataProcessorService::removeProcessorsDependingOn(const std::vector<TopicId>& input_topics) {
  struct Dependent {
    enum class Kind { kFilter, kTransform } kind;
    NodeId filter_node = 0;
    std::string transform_key;
  };

  std::unordered_set<TopicId> affected(input_topics.begin(), input_topics.end());
  std::vector<Dependent> dependents;
  forEachDependentProcessor(
      affected, /*include_ephemeral_transforms=*/true,
      [&dependents](TopicId, const FilterRecipe& recipe) {
        dependents.push_back(Dependent{.kind = Dependent::Kind::kFilter, .filter_node = recipe.node_id});
      },
      [&dependents](const std::string& key, const TransformRecipe&) {
        dependents.push_back(Dependent{.kind = Dependent::Kind::kTransform, .transform_key = key});
      });

  // The visitor discovers parents before children (a processor is visited only
  // after one of its inputs joined the affected set), so reverse order removes
  // leaf-first — no dependent ever outlives one of its inputs' producers.
  for (auto it = dependents.rbegin(); it != dependents.rend(); ++it) {
    Status removed = it->kind == Dependent::Kind::kFilter ? removeFilterOnly(it->filter_node)
                                                          : removeTransformOnly(it->transform_key);
    if (!removed.has_value()) {
      return removed;
    }
  }
  return PJ::okStatus();
}

void DataProcessorService::clearTransformsForPlugin(std::string_view plugin_id) {
  std::vector<std::string> keys;
  for (const auto& [key, recipe] : transform_recipes_) {
    if (recipe.owner_plugin == plugin_id) {
      keys.push_back(key);
    }
  }
  for (const auto& key : keys) {
    (void)removeTransform(key);
  }
}

void DataProcessorService::clearAllTransforms() {
  clearTransforms(RestoreIntent::kReplace);
}

void DataProcessorService::clearTransformsForDataset(DatasetId dataset_id) {
  std::vector<std::string> keys;
  for (const auto& [key, recipe] : transform_recipes_) {
    if (recipe.dataset_id == dataset_id) {
      keys.push_back(key);
    }
  }
  for (const auto& key : keys) {
    (void)removeTransform(key);
  }
}

std::vector<std::string> DataProcessorService::transformIdsForPlugin(std::string_view plugin_id) const {
  std::vector<std::string> ids;
  for (const auto& [key, recipe] : transform_recipes_) {
    (void)key;
    if (recipe.owner_plugin == plugin_id) {
      ids.push_back(recipe.user_id);
    }
  }
  return ids;
}

std::optional<std::string> DataProcessorService::transformRecipeJson(std::string_view namespaced_key) const {
  const auto it = transform_recipes_.find(std::string(namespaced_key));
  if (it == transform_recipes_.end()) {
    return std::nullopt;
  }
  const TransformRecipe& recipe = it->second;
  nlohmann::json doc;
  doc["inputs"] = recipe.inputs;
  doc["outputs"] = recipe.outputs;
  doc["backend"] = recipe.backend;
  doc["history_exempt"] = recipe.history_exempt;
  // params is structured; embed it as an object (fall back to {} on malformed input).
  nlohmann::json params = nlohmann::json::parse(recipe.params_json, nullptr, /*allow_exceptions=*/false);
  doc["params"] = params.is_discarded() ? nlohmann::json::object() : params;
  return doc.dump();
}

std::vector<DataProcessorService::TransformRecipe> DataProcessorService::transformRecipes() const {
  std::vector<TransformRecipe> out;
  out.reserve(transform_recipes_.size());
  for (const auto& [key, recipe] : transform_recipes_) {
    (void)key;
    if (recipe.ephemeral) {
      continue;  // preview node — never persisted
    }
    out.push_back(recipe);
  }
  return out;
}

std::vector<std::pair<DatasetId, std::string>> DataProcessorService::sourceTopicsForOutput(
    TopicId output_topic_id) const {
  const auto engine_lock = engine_.lockEngine();
  std::vector<std::pair<DatasetId, std::string>> sources;
  std::unordered_set<TopicId> visited;
  std::function<void(TopicId)> visit = [&](TopicId topic_id) {
    if (!visited.insert(topic_id).second) {
      return;
    }
    if (const auto filter = recipes_.find(topic_id); filter != recipes_.end()) {
      visit(filter->second.input_topic_id);
      return;
    }
    for (const auto& [key, recipe] : transform_recipes_) {
      (void)key;
      if (std::find(recipe.output_topic_ids.begin(), recipe.output_topic_ids.end(), topic_id) ==
          recipe.output_topic_ids.end()) {
        continue;
      }
      for (const TopicId input_topic_id : recipe.input_topic_ids) {
        visit(input_topic_id);
      }
      return;
    }
    if (const TopicStorage* storage = engine_.getTopicStorage(topic_id); storage != nullptr) {
      const DatasetId dataset_id = storage->descriptor().dataset_id;
      const std::vector<TopicId> live_topics = engine_.listTopics(dataset_id);
      if (std::find(live_topics.begin(), live_topics.end(), topic_id) != live_topics.end()) {
        sources.emplace_back(dataset_id, storage->descriptor().name);
      }
    }
  };
  const bool is_filter_output = recipes_.find(output_topic_id) != recipes_.end();
  const bool is_transform_output =
      std::any_of(transform_recipes_.begin(), transform_recipes_.end(), [output_topic_id](const auto& entry) {
        const auto& outputs = entry.second.output_topic_ids;
        return std::find(outputs.begin(), outputs.end(), output_topic_id) != outputs.end();
      });
  if (is_filter_output || is_transform_output) {
    visit(output_topic_id);
  }
  std::sort(sources.begin(), sources.end());
  sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
  return sources;
}

std::unordered_set<TopicId> DataProcessorService::ephemeralOutputTopics() const {
  std::unordered_set<TopicId> out;
  for (const auto& [key, recipe] : transform_recipes_) {
    (void)key;
    if (recipe.ephemeral) {
      out.insert(recipe.output_topic_ids.begin(), recipe.output_topic_ids.end());
    }
  }
  return out;
}

std::unordered_set<TopicId> DataProcessorService::processorOutputTopics() const {
  std::unordered_set<TopicId> outputs;
  outputs.reserve(recipes_.size() + transform_recipes_.size());
  for (const auto& [output_topic_id, recipe] : recipes_) {
    (void)recipe;
    outputs.insert(output_topic_id);
  }
  for (const auto& [key, recipe] : transform_recipes_) {
    (void)key;
    outputs.insert(recipe.output_topic_ids.begin(), recipe.output_topic_ids.end());
  }
  return outputs;
}

std::vector<DataProcessorService::TransformRecipe> DataProcessorService::transformsDependingOn(
    const std::vector<TopicId>& removed_topics) const {
  std::unordered_set<TopicId> affected(removed_topics.begin(), removed_topics.end());
  std::vector<TransformRecipe> result;
  forEachDependentProcessor(
      affected, /*include_ephemeral_transforms=*/false, [](TopicId, const FilterRecipe&) {},
      [&result](const std::string&, const TransformRecipe& recipe) { result.push_back(recipe); });
  return result;
}

Expected<DataProcessorService::TransformRecipe> DataProcessorService::restoreTransform(const TransformRecipe& recipe) {
  TransformRecipe restored = recipe;  // copy; installTransform re-derives the runtime fields
  if (restored.key.empty()) {
    restored.key = makeTransformKey(restored.owner_plugin, restored.user_id);
  }
  return installTransform(std::move(restored));
}

}  // namespace PJ
