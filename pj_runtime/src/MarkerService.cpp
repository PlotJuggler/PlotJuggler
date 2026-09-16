// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/MarkerService.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

#include "pj_base/builtin/plot_markers.hpp"        // sdk::PlotMarker(s), markerObjectTopicName, kPreviewMarkerTopic
#include "pj_base/builtin/plot_markers_codec.hpp"  // serializePlotMarkers, deserializePlotMarkers
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/MarkerTopics.h"     // isMarkerObjectTopic, markerOwnerTopicName, kAllDatasetsMarkerDataset
#include "pj_scripting/marker_engine.h"  // scripting::SeriesView/SeriesProvider, runMarkerScript

namespace PJ {

namespace {

/// Materialize each input series key via the resolver into engine views, kept alive
/// by the returned map for the duration of the script run (the provider hands out
/// pointers into it).
std::unordered_map<std::string, scripting::SeriesView> materializeInputs(
    const MarkerService::SeriesResolver& resolver, DatasetId dataset_id, const std::vector<std::string>& inputs) {
  std::unordered_map<std::string, scripting::SeriesView> views;
  views.reserve(inputs.size());
  if (!resolver) {
    return views;
  }
  for (const std::string& key : inputs) {
    std::optional<MarkerService::ResolvedSeries> resolved = resolver(dataset_id, key);
    if (!resolved.has_value()) {
      continue;  // unknown key → absent; the script's series(key) sees no data
    }
    scripting::SeriesView view;
    view.timestamps = std::move(resolved->timestamps);
    view.values = std::move(resolved->values);
    views.emplace(key, std::move(view));
  }
  return views;
}

/// `declared` (parallel to `bare`, or empty) is the spelling the script wrote each
/// input against: a materialized series is enumerated once under it, and readable
/// under it first — a declared name that equals another input's bare key must not
/// silently read that other series — then under its bare key.
scripting::SeriesProvider buildProvider(
    const std::unordered_map<std::string, scripting::SeriesView>& views, const std::vector<std::string>& bare,
    const std::vector<std::string>& declared) {
  scripting::SeriesProvider provider;
  std::unordered_map<std::string, std::string> aliases;
  for (std::size_t i = 0; i < bare.size(); ++i) {
    const std::string& name = declared.empty() ? bare[i] : declared[i];
    if (!views.contains(bare[i]) ||
        std::find(provider.names.begin(), provider.names.end(), name) != provider.names.end()) {
      continue;
    }
    provider.names.push_back(name);
    aliases.emplace(name, bare[i]);
  }
  provider.get = [&views, aliases = std::move(aliases)](const std::string& name) -> const scripting::SeriesView* {
    const auto alias = aliases.find(name);
    const auto it = views.find(alias == aliases.end() ? name : alias->second);
    return it == views.end() ? nullptr : &it->second;
  };
  return provider;
}

/// Reject a non-"luau" script language (empty = default = luau). Shared by the
/// validate and upsert paths so the policy + message stay in one place — this is the
/// exact guard that grows when a second backend lands.
Status checkLanguage(std::string_view language) {
  if (!language.empty() && language != "luau") {
    return unexpected("unsupported script language '" + std::string(language) + "' (only 'luau')");
  }
  return okStatus();
}

/// Shift every marker's [t_start, t_end] window by `shift` — the rebase a marker set
/// needs when it moves onto another dataset's clock. kValueBand markers ignore
/// t_start/t_end, so shifting them is a harmless no-op for that kind.
void shiftMarkers(std::vector<sdk::PlotMarker>& markers, Timestamp shift) {
  for (sdk::PlotMarker& marker : markers) {
    marker.t_start += shift;
    marker.t_end += shift;
  }
}

/// A marker series key identifies either a topic itself or a field below it.
/// Requiring the slash boundary avoids treating similarly prefixed topics as
/// the same input (for example, `imu` and `imu_raw`).
bool seriesKeyBelongsToTopic(const std::string& key, const std::string& topic) {
  return key == topic ||
         (key.size() > topic.size() && key.compare(0, topic.size(), topic) == 0 && key[topic.size()] == '/');
}

}  // namespace

MarkerService::MarkerService(ObjectStore& object_store, SeriesResolver resolver)
    : object_store_(object_store), resolver_(std::move(resolver)) {}

void MarkerService::setResolver(SeriesResolver resolver) {
  resolver_ = std::move(resolver);
}

void MarkerService::setDatasetLister(DatasetLister lister) {
  dataset_lister_ = std::move(lister);
}

void MarkerService::setDisplayOffsetResolver(DisplayOffsetResolver resolver) {
  display_offset_resolver_ = std::move(resolver);
}

Timestamp MarkerService::displayOffsetOf(DatasetId dataset) const {
  return display_offset_resolver_ ? display_offset_resolver_(dataset) : Timestamp{0};
}

std::vector<std::string> MarkerService::reachNewDatasets() {
  std::vector<std::string> affected;
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (recipe.all_datasets) {
      republishShared(recipe, affected);
    }
  }
  return affected;
}

std::vector<DatasetId> MarkerService::loadedDatasets() const {
  return dataset_lister_ ? dataset_lister_() : std::vector<DatasetId>{};
}

Status MarkerService::validateScript(
    GeneratorKind /*kind*/, std::string_view language, const std::string& script) const {
  if (Status s = checkLanguage(language); !s.has_value()) {
    return s;
  }
  std::string err;
  if (!scripting::validateScript(script, &err)) {
    return unexpected(err);
  }
  return okStatus();
}

// ---- kind=markers ----------------------------------------------------------

Expected<std::optional<sdk::PlotMarkers>> MarkerService::evaluateMarkers(
    const GeneratorRecipe& recipe, DatasetId dataset_id) {
  std::unordered_map<std::string, scripting::SeriesView> views =
      materializeInputs(resolver_, dataset_id, recipe.inputs);
  if (recipe.all_datasets && !recipe.inputs.empty() && views.empty()) {
    return std::optional<sdk::PlotMarkers>{};  // not one of this rule's datasets
  }
  scripting::SeriesProvider provider = buildProvider(views, recipe.inputs, recipe.declared_inputs);

  std::string err;
  sdk::PlotMarkers set;
  set.markers = scripting::runMarkerScript(recipe.script, provider, &err);
  if (!err.empty()) {
    return unexpected(err);
  }
  return std::optional<sdk::PlotMarkers>{std::move(set)};
}

Expected<std::vector<std::string>> MarkerService::runMarkers(const GeneratorRecipe& recipe) {
  Expected<std::optional<sdk::PlotMarkers>> set =
      recipe.all_datasets ? computeShared(recipe) : evaluateMarkers(recipe, recipe.dataset_id);
  if (!set.has_value()) {
    return unexpected(set.error());
  }
  if (!set->has_value()) {
    return unexpected("none of the inputs exist in any loaded dataset");
  }
  const PublishTarget target = publishTarget(recipe);
  if (Status pushed = publishMarkerSet(target.first, target.second, **set); !pushed.has_value()) {
    return unexpected(pushed.error());
  }
  return std::vector<std::string>{target.second};
}

Expected<std::optional<sdk::PlotMarkers>> MarkerService::computeShared(const GeneratorRecipe& recipe) {
  // Any script error aborts before publishing: a partial union would silently
  // misrepresent the rule on every dataset it is drawn on.
  sdk::PlotMarkers shared;
  bool contributed = false;
  for (const DatasetId dataset_id : loadedDatasets()) {
    Expected<std::optional<sdk::PlotMarkers>> run = evaluateMarkers(recipe, dataset_id);
    if (!run.has_value()) {
      return unexpected(run.error());
    }
    if (!run->has_value()) {
      continue;
    }
    contributed = true;
    shiftMarkers((*run)->markers, -displayOffsetOf(dataset_id));
    shared.markers.insert(shared.markers.end(), (*run)->markers.begin(), (*run)->markers.end());
  }
  if (!contributed) {
    return std::optional<sdk::PlotMarkers>{};
  }
  return std::optional<sdk::PlotMarkers>{std::move(shared)};
}

void MarkerService::republishShared(const GeneratorRecipe& recipe, std::vector<std::string>& affected) {
  Expected<std::optional<sdk::PlotMarkers>> shared = computeShared(recipe);
  if (!shared.has_value()) {
    return;
  }
  const PublishTarget target = publishTarget(recipe);
  if (shared->has_value()) {
    if (!publishMarkerSet(target.first, target.second, **shared).has_value()) {
      return;
    }
  } else {
    publishEmptyMarkers(target.first, target.second);
  }
  affected.push_back(target.second);
}

// ---- routing + lifecycle ---------------------------------------------------

Expected<std::vector<std::string>> MarkerService::runAndPublish(const GeneratorRecipe& recipe) {
  switch (recipe.kind) {
    case GeneratorKind::kMarkers:
      return runMarkers(recipe);
  }
  return unexpected("unknown generator kind");
}

void MarkerService::runAndAccumulate(const GeneratorRecipe& recipe, std::vector<std::string>& affected) {
  if (Expected<std::vector<std::string>> resolved = runAndPublish(recipe); resolved.has_value()) {
    for (std::string& topic : *resolved) {
      affected.push_back(std::move(topic));
    }
  }
}

Expected<std::vector<std::string>> MarkerService::upsertGenerator(GeneratorRecipe recipe) {
  if (Status s = checkLanguage(recipe.language); !s.has_value()) {
    return unexpected(s.error());
  }
  if (!recipe.declared_inputs.empty() && recipe.declared_inputs.size() != recipe.inputs.size()) {
    return unexpected("declared input count does not match input count");
  }
  if (recipe.kind == GeneratorKind::kMarkers) {
    // Auto-name an ephemeral preview if the caller left the output open; reject a
    // committed generator that targets the reserved preview namespace.
    if (recipe.ephemeral && recipe.outputs.empty()) {
      recipe.outputs = {std::string(sdk::kPreviewMarkerTopic) + recipe.id};
    }
    if (!recipe.ephemeral && !recipe.outputs.empty() && sdk::isPreviewMarkerTopic(recipe.outputs.front())) {
      return unexpected("output marker topic '__preview__/...' is reserved for previews");
    }
    if (recipe.outputs.empty()) {
      return unexpected("marker generator requires an output marker topic");
    }
  }

  // The owner separator is what keeps `<key>#<id>` unambiguous (MarkerTopics.h).
  const auto carries_separator = [](const std::string& text) {
    return text.find(kMarkerOwnerSeparator) != std::string::npos;
  };
  if (carries_separator(recipe.id) || std::any_of(recipe.outputs.begin(), recipe.outputs.end(), carries_separator)) {
    return unexpected("'#' is reserved as the marker owner separator (generator id and output topic)");
  }

  // Normalize the scope-encoded output topic host-side (see MarkerTopics.h): a
  // producer always addresses "__global__"; all_datasets rewrites that to the
  // ALL-DATASETS key, so the two scopes never share one family. A dataset-bound
  // recipe targeting the ALL-DATASETS key is rejected outright — silently
  // reinterpreting its scope would move the rule between UI rows with no signal.
  if (recipe.all_datasets && recipe.outputs.front() == sdk::kGlobalMarkerTopic) {
    recipe.outputs.front() = std::string(kAllDatasetsMarkerTopic);
  } else if (!recipe.all_datasets && recipe.outputs.front() == kAllDatasetsMarkerTopic) {
    return unexpected("output '__all__' is reserved for all-datasets generators");
  }

  // Flags are per-id (clearGenerators, removeGenerator and generatorIds read the
  // first binding), so a binding that would sit beside one with other flags is
  // refused. A scope change replaces the whole id below, so no sibling survives it.
  const auto existing = recipes_.find(recipe.id);
  const bool scope_changed = existing != recipes_.end() && existing->second.front().all_datasets != recipe.all_datasets;
  if (existing != recipes_.end() && !scope_changed) {
    for (const GeneratorRecipe& sibling : existing->second) {
      if (bindingDataset(sibling) != bindingDataset(recipe) &&
          (sibling.ephemeral != recipe.ephemeral || sibling.history_exempt != recipe.history_exempt)) {
        return unexpected(
            "generator '" + recipe.id + "' is already bound with different ephemeral/history_exempt flags");
      }
    }
  }

  Expected<std::vector<std::string>> resolved = runAndPublish(recipe);
  if (!resolved.has_value()) {
    return resolved;  // leave any prior published output + recipe untouched
  }

  // The previous revision's topic would otherwise keep drawing a set nothing ever
  // revisits: retire it unless this run just rewrote it (same dataset, same key).
  const PublishTarget fresh = publishTarget(recipe);
  if (scope_changed) {
    // The rule moved between Dataset and Global scope: every old output goes.
    retireOwnedTopics(recipe.id, existing->second.front().ephemeral, &fresh);
    recipes_.erase(existing);
  } else if (const GeneratorRecipe* replaced = findBinding(recipe)) {
    retireTarget(publishTarget(*replaced), replaced->ephemeral, &fresh);
  }
  if (GeneratorRecipe* replaced = findBinding(recipe)) {
    *replaced = std::move(recipe);
  } else {
    recipes_[recipe.id].push_back(std::move(recipe));
  }
  return resolved;
}

Status MarkerService::publishMarkerSet(
    DatasetId dataset_id, const std::string& object_topic_name, const sdk::PlotMarkers& set) {
  ObjectTopicId topic_id;
  if (const std::optional<ObjectTopicId> existing = object_store_.findTopic(dataset_id, object_topic_name)) {
    topic_id = *existing;
  } else {
    Expected<ObjectTopicId> registered = object_store_.registerTopic(
        ObjectTopicDescriptor{.dataset_id = dataset_id, .topic_name = object_topic_name, .metadata_json = {}});
    if (!registered.has_value()) {
      return unexpected(registered.error());
    }
    topic_id = *registered;
    object_store_.setRetentionBudget(
        topic_id, RetentionBudget{.time_window_ns = 0, .max_memory_bytes = 0, .max_entries = 1});
  }
  return object_store_.pushOwned(topic_id, Timestamp{0}, serializePlotMarkers(set));
}

void MarkerService::publishEmptyMarkers(DatasetId dataset_id, const std::string& object_topic_name) {
  // The tombstone must NOT register a topic that was never published — so this uses
  // findTopic directly rather than publishMarkerSet (which registers on a miss).
  const std::optional<ObjectTopicId> topic_id = object_store_.findTopic(dataset_id, object_topic_name);
  if (!topic_id.has_value()) {
    return;  // never published on this dataset → nothing to clear
  }
  (void)object_store_.pushOwned(*topic_id, Timestamp{0}, serializePlotMarkers(sdk::PlotMarkers{}));
}

sdk::PlotMarkers MarkerService::storedMarkers(ObjectTopicId topic_id, Timestamp shift) const {
  const std::optional<ResolvedObjectEntry> entry =
      object_store_.latestAt(topic_id, std::numeric_limits<Timestamp>::max());
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    return {};
  }
  Expected<sdk::PlotMarkers> decoded = deserializePlotMarkers(entry->payload.bytes.data(), entry->payload.bytes.size());
  if (!decoded.has_value()) {
    return {};
  }
  shiftMarkers(decoded->markers, shift + entry->payload_stamp_shift);
  return std::move(*decoded);
}

MarkerService::PublishTarget MarkerService::publishTarget(const GeneratorRecipe& recipe) {
  return PublishTarget{
      recipe.all_datasets ? kAllDatasetsMarkerDataset : recipe.dataset_id,
      markerOwnerTopicName(recipe.outputs.front(), recipe.id)};
}

void MarkerService::retireTarget(const PublishTarget& target, bool ephemeral, const PublishTarget* keep) {
  if (keep != nullptr && *keep == target) {
    return;
  }
  if (!ephemeral) {
    publishEmptyMarkers(target.first, target.second);
    return;
  }
  if (const std::optional<ObjectTopicId> topic_id = object_store_.findTopic(target.first, target.second)) {
    object_store_.removeTopic(*topic_id);
  }
}

void MarkerService::retireOwnedTopics(std::string_view id, bool ephemeral, const PublishTarget* keep) {
  std::string suffix(1, kMarkerOwnerSeparator);
  suffix.append(id);
  for (const ObjectTopicId topic_id : object_store_.listTopics()) {
    const ObjectTopicDescriptor descriptor = object_store_.descriptor(topic_id);
    if (isMarkerObjectTopic(descriptor.topic_name) && descriptor.topic_name.ends_with(suffix)) {
      retireTarget(PublishTarget{descriptor.dataset_id, descriptor.topic_name}, ephemeral, keep);
    }
  }
}

Status MarkerService::removeGenerator(std::string_view id) {
  const auto it = recipes_.find(std::string(id));
  if (it == recipes_.end()) {
    return unexpected("unknown generator id");
  }
  retireOwnedTopics(id, it->second.front().ephemeral);
  recipes_.erase(it);
  return okStatus();
}

void MarkerService::remapGeneratorsToAnchor(DatasetId anchor, const std::vector<DatasetId>& consumed) {
  const auto is_consumed = [&consumed](const GeneratorRecipe& recipe) {
    return !recipe.all_datasets && std::find(consumed.begin(), consumed.end(), recipe.dataset_id) != consumed.end();
  };
  for (auto& [id, bindings] : recipes_) {
    // Collision rule (see the header): the anchor's own binding wins. mergeMarkerTopics
    // runs before this call (SessionManager::mergeDatasets, (3b)) and already moved
    // every consumed output onto the anchor's topic of this id, so retiring the
    // consumed binding loses no published marker.
    const bool anchor_bound = std::any_of(bindings.begin(), bindings.end(), [&](const GeneratorRecipe& recipe) {
      return !recipe.all_datasets && recipe.dataset_id == anchor;
    });
    if (anchor_bound) {
      std::erase_if(bindings, is_consumed);
      continue;
    }
    // Several consumed bindings fold into ONE anchor binding: the first applied.
    const auto first = std::find_if(bindings.begin(), bindings.end(), is_consumed);
    if (first == bindings.end()) {
      continue;
    }
    first->dataset_id = anchor;
    bindings.erase(std::remove_if(std::next(first), bindings.end(), is_consumed), bindings.end());
  }
}

std::vector<std::string> MarkerService::clearGeneratorsForDataset(DatasetId dataset) {
  for (auto& [id, bindings] : recipes_) {
    std::erase_if(bindings, [dataset](const GeneratorRecipe& recipe) {
      return !recipe.all_datasets && recipe.dataset_id == dataset;
    });
  }
  std::erase_if(recipes_, [](const auto& entry) { return entry.second.empty(); });

  // The removed dataset may have contributed to a shared set: rebuild each from the
  // survivors. With no contributor left the rule draws nothing, but it stays live
  // so a contributor loaded later revives it.
  std::vector<std::string> affected;
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (recipe.all_datasets) {
      republishShared(recipe, affected);
    }
  }
  return affected;
}

std::vector<std::string> MarkerService::recomputeForChangedInputs(
    const std::vector<std::string>& changed, DatasetId scope) {
  std::vector<std::string> affected;
  for (const GeneratorRecipe& recipe : allRecipes()) {
    // Topic names are not unique across datasets, so the name test alone would re-run
    // a generator bound to a dataset that never changed.
    if (scope != kAnyDataset && !recipe.all_datasets && recipe.dataset_id != scope) {
      continue;
    }
    const bool rerun =
        changed.empty() || std::any_of(recipe.inputs.begin(), recipe.inputs.end(), [&changed](const std::string& in) {
          return std::any_of(
              changed.begin(), changed.end(), [&in](const std::string& prefix) { return in.rfind(prefix, 0) == 0; });
        });
    if (!rerun) {
      continue;
    }
    // An all_datasets recipe's set is shared, so a change on one contributing
    // dataset re-runs the whole union — `scope` only attributes the change.
    if (recipe.all_datasets) {
      republishShared(recipe, affected);
    } else {
      runAndAccumulate(recipe, affected);
    }
  }
  return affected;
}

std::vector<std::string> MarkerService::exemptDependentsOf(const std::vector<std::string>& removed_inputs) const {
  std::vector<std::string> outputs;
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (recipe.ephemeral || !recipe.history_exempt) {
      continue;
    }
    const bool depends = std::any_of(recipe.inputs.begin(), recipe.inputs.end(), [&](const std::string& input) {
      return std::any_of(removed_inputs.begin(), removed_inputs.end(), [&](const std::string& removed) {
        return !removed.empty() && seriesKeyBelongsToTopic(input, removed);
      });
    });
    if (!depends) {
      continue;
    }
    if (recipe.outputs.empty()) {
      outputs.push_back(recipe.id);
    } else {
      outputs.insert(outputs.end(), recipe.outputs.begin(), recipe.outputs.end());
    }
  }
  std::sort(outputs.begin(), outputs.end());
  outputs.erase(std::unique(outputs.begin(), outputs.end()), outputs.end());
  return outputs;
}

bool MarkerService::hasHistoryExemptGenerators() const {
  const auto recipes = allRecipes();
  return std::any_of(recipes.begin(), recipes.end(), [](const GeneratorRecipe& r) { return r.history_exempt; });
}

void MarkerService::clearGenerators(RestoreIntent intent) {
  const bool keep_history_exempt = intent == RestoreIntent::kHistory;
  // ephemeral / history_exempt are per-id properties, so the first binding decides.
  std::vector<std::string> ids;
  for (const auto& [id, bindings] : recipes_) {
    const GeneratorRecipe& recipe = bindings.front();
    if (!recipe.ephemeral && !(keep_history_exempt && recipe.history_exempt)) {
      ids.push_back(id);
    }
  }
  for (const std::string& id : ids) {
    (void)removeGenerator(id);
  }
}

void MarkerService::clearAllGenerators() {
  clearGenerators(RestoreIntent::kReplace);
}

std::vector<std::string> MarkerService::recomputeForDataset(DatasetId dataset) {
  // Whole-dataset recompute == "everything changed, scoped to this dataset": an empty
  // `changed` re-runs every recipe bound here, plus every all_datasets recipe (its
  // shared set may have this dataset as a contributor).
  return recomputeForChangedInputs(/*changed=*/{}, dataset);
}

std::vector<std::string> MarkerService::mergeMarkerTopics(
    DatasetId anchor, const std::vector<DatasetMergeSource>& sources) {
  // Anchor topic names touched by this merge, republished once each in sorted order.
  std::set<std::string> touched;
  for (const DatasetMergeSource& source : sources) {
    for (const ObjectTopicId source_id : object_store_.listTopics(source.dataset_id)) {
      const std::string name = object_store_.descriptor(source_id).topic_name;
      if (!isMarkerObjectTopic(name)) {
        continue;
      }
      // The anchor's own set (unshifted) first, then the source's on the anchor clock.
      sdk::PlotMarkers merged;
      if (const std::optional<ObjectTopicId> anchor_id = object_store_.findTopic(anchor, name)) {
        merged = storedMarkers(*anchor_id, Timestamp{0});
      }
      sdk::PlotMarkers moved = storedMarkers(source_id, source.raw_shift_ns);
      merged.markers.insert(
          merged.markers.end(), std::make_move_iterator(moved.markers.begin()),
          std::make_move_iterator(moved.markers.end()));
      object_store_.removeTopic(source_id);
      if (publishMarkerSet(anchor, name, merged).has_value()) {
        touched.insert(name);
      }
    }
  }
  return {touched.begin(), touched.end()};
}

std::vector<MarkerService::GeneratorRecipe> MarkerService::recipes() const {
  std::vector<GeneratorRecipe> out;
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (!recipe.ephemeral) {
      out.push_back(recipe);
    }
  }
  return out;
}

std::vector<std::string> MarkerService::generatorIds() const {
  std::vector<std::string> ids;
  for (const auto& [id, bindings] : recipes_) {
    if (!bindings.front().ephemeral) {
      ids.push_back(id);
    }
  }
  return ids;
}

const MarkerService::GeneratorRecipe* MarkerService::firstBinding(std::string_view id) const {
  const auto it = recipes_.find(std::string(id));
  return it == recipes_.end() ? nullptr : &it->second.front();
}

std::vector<std::reference_wrapper<const MarkerService::GeneratorRecipe>> MarkerService::allRecipes() const {
  std::vector<std::reference_wrapper<const GeneratorRecipe>> out;
  for (const auto& [id, bindings] : recipes_) {
    out.insert(out.end(), bindings.begin(), bindings.end());
  }
  return out;
}

DatasetId MarkerService::bindingDataset(const GeneratorRecipe& recipe) {
  return (recipe.all_datasets || recipe.ephemeral) ? kAnyDataset : recipe.dataset_id;
}

MarkerService::GeneratorRecipe* MarkerService::findBinding(const GeneratorRecipe& recipe) {
  const auto it = recipes_.find(recipe.id);
  if (it == recipes_.end()) {
    return nullptr;
  }
  const auto match = std::find_if(it->second.begin(), it->second.end(), [&recipe](const GeneratorRecipe& binding) {
    return bindingDataset(binding) == bindingDataset(recipe);
  });
  return match == it->second.end() ? nullptr : &*match;
}

}  // namespace PJ
