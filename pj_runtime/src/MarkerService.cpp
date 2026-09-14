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
#include "pj_runtime/MarkerTopics.h"     // isMarkerObjectTopic
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
  const std::vector<DatasetId> loaded = loadedDatasets();
  for (const GeneratorRecipe& recipe : allRecipes()) {
    reachUnpublishedDatasets(recipe, loaded, affected);
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
  const std::string marker_topic = recipe.outputs.empty() ? std::string{} : recipe.outputs.front();
  const std::string object_topic_name = sdk::markerObjectTopicName(marker_topic);
  const std::vector<std::string> resolved{object_topic_name};

  if (!recipe.all_datasets) {
    Expected<std::optional<sdk::PlotMarkers>> run = evaluateMarkers(recipe, recipe.dataset_id);
    if (!run.has_value()) {
      return unexpected(run.error());
    }
    if (Status pushed = setPart(PublishTarget{recipe.dataset_id, object_topic_name}, recipe.id, std::move(**run));
        !pushed.has_value()) {
      return unexpected(pushed.error());
    }
    return resolved;
  }

  Expected<std::optional<sdk::PlotMarkers>> shared = computeShared(recipe);
  if (!shared.has_value()) {
    return unexpected(shared.error());
  }
  if (!shared->has_value()) {
    return unexpected("none of the inputs exist in any loaded dataset");
  }
  if (Status pushed = publishSharedEverywhere(recipe, std::move(**shared)); !pushed.has_value()) {
    return unexpected(pushed.error());
  }
  return resolved;
}

Status MarkerService::publishSharedEverywhere(const GeneratorRecipe& recipe, sdk::PlotMarkers shared) {
  for (const DatasetId dataset_id : targetDatasets(recipe)) {
    if (Status pushed = publishShared(recipe, shared, dataset_id); !pushed.has_value()) {
      return pushed;
    }
  }
  shared_sets_[recipe.id] = std::move(shared);
  return okStatus();
}

Expected<std::optional<sdk::PlotMarkers>> MarkerService::computeShared(const GeneratorRecipe& recipe) {
  // Any script error aborts before publishing: a partial union would silently
  // misrepresent the rule on every dataset it is drawn on.
  sdk::PlotMarkers shared;
  bool contributed = false;
  for (const DatasetId dataset_id : targetDatasets(recipe)) {
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

Status MarkerService::publishShared(const GeneratorRecipe& recipe, const sdk::PlotMarkers& shared, DatasetId dataset) {
  sdk::PlotMarkers part = shared;
  shiftMarkers(part.markers, displayOffsetOf(dataset));
  return setPart(
      PublishTarget{dataset, sdk::markerObjectTopicName(recipe.outputs.front())}, recipe.id, std::move(part));
}

std::vector<std::string> MarkerService::rebaseForDisplayOffset(DatasetId dataset) {
  std::vector<std::string> affected;
  for (const auto& [id, shared] : shared_sets_) {
    const GeneratorRecipe& recipe = recipes_.at(id).front();  // all_datasets: one binding
    if (publishShared(recipe, shared, dataset).has_value()) {
      affected.push_back(sdk::markerObjectTopicName(recipe.outputs.front()));
    }
  }
  return affected;
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

void MarkerService::reachUnpublishedDatasets(
    const GeneratorRecipe& recipe, const std::vector<DatasetId>& loaded, std::vector<std::string>& affected) {
  if (!recipe.all_datasets || recipe.outputs.empty()) {
    return;
  }
  const auto shared_it = shared_sets_.find(recipe.id);
  if (shared_it == shared_sets_.end()) {
    runAndAccumulate(recipe, affected);  // never contributed anywhere yet — try again
    return;
  }
  const std::string object_topic_name = sdk::markerObjectTopicName(recipe.outputs.front());
  for (const DatasetId dataset_id : targetDatasets(recipe, loaded)) {
    const auto parts_it = parts_.find(PublishTarget{dataset_id, object_topic_name});
    if (parts_it != parts_.end() && parts_it->second.contains(recipe.id)) {
      continue;  // already publishing there
    }
    if (publishShared(recipe, shared_it->second, dataset_id).has_value()) {
      affected.push_back(object_topic_name);
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

  // Normalize the scope-encoded output topic host-side (see MarkerTopics.h): a
  // producer always addresses "__global__"; all_datasets rewrites that to the
  // ALL-DATASETS key, so the two scopes never share one union. A dataset-bound
  // recipe targeting the ALL-DATASETS key is rejected outright — silently
  // reinterpreting its scope would move the rule between UI rows with no signal.
  if (!recipe.outputs.empty()) {
    if (recipe.all_datasets && recipe.outputs.front() == sdk::kGlobalMarkerTopic) {
      recipe.outputs.front() = std::string(kAllDatasetsMarkerTopic);
    } else if (!recipe.all_datasets && recipe.outputs.front() == kAllDatasetsMarkerTopic) {
      return unexpected("output '__all__' is reserved for all-datasets generators");
    }
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

  if (scope_changed) {
    // The rule moved between Dataset and Global scope: every old binding and its
    // output goes, only the freshly published targets stay.
    if (!recipe.all_datasets) {
      shared_sets_.erase(recipe.id);
    }
    dropParts(recipe.id, publishTargets(recipe), existing->second.front().ephemeral);
    recipes_.erase(existing);
  } else {
    dropStaleTargets(recipe);
  }
  if (GeneratorRecipe* replaced = findBinding(recipe)) {
    *replaced = std::move(recipe);
  } else {
    recipes_[recipe.id].push_back(std::move(recipe));
  }
  return resolved;
}

void MarkerService::dropStaleTargets(const GeneratorRecipe& recipe) {
  // A previous revision's part would otherwise stay stuck forever with a set
  // nothing ever revisits — no later recompute or remove names it again. The
  // binding THIS upsert replaces is deliberately excluded from `keep`: its old
  // targets are exactly what must stay droppable when the new revision no longer
  // covers them — only a sibling's targets (another dataset's binding) are a live,
  // unrelated binding that must survive.
  const auto it = recipes_.find(recipe.id);
  if (it == recipes_.end()) {
    return;
  }
  const bool prev_ephemeral = it->second.front().ephemeral;  // ephemeral is a per-id property, not per-dataset
  std::vector<PublishTarget> keep = publishTargets(recipe);
  for (const GeneratorRecipe& sibling : it->second) {
    if (bindingDataset(sibling) == bindingDataset(recipe)) {
      continue;
    }
    const std::vector<PublishTarget> sibling_targets = publishTargets(sibling);
    keep.insert(keep.end(), sibling_targets.begin(), sibling_targets.end());
  }
  dropParts(recipe.id, keep, prev_ephemeral);
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

std::vector<DatasetId> MarkerService::targetDatasets(const GeneratorRecipe& recipe) const {
  return recipe.all_datasets ? loadedDatasets() : std::vector<DatasetId>{recipe.dataset_id};
}

std::vector<DatasetId> MarkerService::targetDatasets(
    const GeneratorRecipe& recipe, const std::vector<DatasetId>& loaded) {
  return recipe.all_datasets ? loaded : std::vector<DatasetId>{recipe.dataset_id};
}

std::vector<MarkerService::PublishTarget> MarkerService::publishTargets(const GeneratorRecipe& recipe) const {
  std::vector<PublishTarget> targets;
  if (recipe.outputs.empty()) {
    return targets;
  }
  const std::string object_topic_name = sdk::markerObjectTopicName(recipe.outputs.front());
  for (const DatasetId dataset_id : targetDatasets(recipe)) {
    targets.emplace_back(dataset_id, object_topic_name);
  }
  return targets;
}

void MarkerService::adoptForeignBlob(const PublishTarget& target, PartsByOwner& owners) const {
  const std::optional<ObjectTopicId> topic_id = object_store_.findTopic(target.first, target.second);
  if (!topic_id.has_value()) {
    return;  // nothing published there yet
  }
  if (std::optional<sdk::PlotMarkers> decoded = decodeStoredMarkers(latestMarkerEntry(*topic_id))) {
    owners[std::string(kForeignOwner)] = std::move(*decoded);
  }
}

std::optional<ResolvedObjectEntry> MarkerService::latestMarkerEntry(ObjectTopicId topic_id) const {
  return object_store_.latestAt(topic_id, std::numeric_limits<Timestamp>::max());
}

std::optional<sdk::PlotMarkers> MarkerService::decodeStoredMarkers(const std::optional<ResolvedObjectEntry>& entry) {
  if (!entry.has_value()) {
    return std::nullopt;
  }
  Expected<sdk::PlotMarkers> decoded = deserializePlotMarkers(entry->payload.bytes.data(), entry->payload.bytes.size());
  if (!decoded.has_value() || decoded->empty()) {
    return std::nullopt;  // indecodable or already a tombstone — nothing to adopt
  }
  return std::move(*decoded);
}

Status MarkerService::republishUnion(const PublishTarget& target) {
  const auto it = parts_.find(target);
  return republishUnion(target, it != parts_.end() ? it->second : PartsByOwner{});
}

Status MarkerService::republishUnion(const PublishTarget& target, const PartsByOwner& owners) {
  if (owners.size() == 1) {
    return publishMarkerSet(target.first, target.second, owners.begin()->second);
  }
  sdk::PlotMarkers set;
  for (const auto& [owner, part] : owners) {
    set.markers.insert(set.markers.end(), part.markers.begin(), part.markers.end());
  }
  return publishMarkerSet(target.first, target.second, set);
}

Status MarkerService::setPart(const PublishTarget& target, std::string_view owner, sdk::PlotMarkers part) {
  const auto [it, inserted] = parts_.try_emplace(target);
  if (inserted) {
    adoptForeignBlob(target, it->second);  // never clobber a live part with the stale store blob
  }
  it->second[std::string(owner)] = std::move(part);
  return republishUnion(target, it->second);
}

void MarkerService::dropParts(std::string_view owner, const std::vector<PublishTarget>& keep, bool ephemeral) {
  const std::set<PublishTarget> kept(keep.begin(), keep.end());
  for (auto it = parts_.begin(); it != parts_.end();) {
    const PublishTarget& target = it->first;
    if (kept.contains(target)) {
      ++it;
      continue;
    }
    if (it->second.erase(std::string(owner)) == 0) {
      ++it;  // this owner had nothing here — leave the target untouched
      continue;
    }
    if (!it->second.empty()) {
      (void)republishUnion(target);  // other owners remain — republish the smaller union
      ++it;
      continue;
    }
    // The last owner just left: tombstone (persistent) or remove outright (ephemeral).
    if (ephemeral) {
      if (const std::optional<ObjectTopicId> oid = object_store_.findTopic(target.first, target.second)) {
        object_store_.removeTopic(*oid);
      }
    } else {
      publishEmptyMarkers(target.first, target.second);
    }
    it = parts_.erase(it);
  }
}

Status MarkerService::removeGenerator(std::string_view id) {
  const auto it = recipes_.find(std::string(id));
  if (it == recipes_.end()) {
    return unexpected("unknown generator id");
  }
  const bool ephemeral = it->second.front().ephemeral;  // ephemeral is a per-id property, not per-dataset
  shared_sets_.erase(it->first);
  recipes_.erase(it);
  dropParts(id, {}, ephemeral);
  return okStatus();
}

void MarkerService::remapGeneratorsToAnchor(DatasetId anchor, const std::vector<DatasetId>& consumed) {
  const auto is_consumed = [&consumed](const GeneratorRecipe& recipe) {
    return !recipe.all_datasets && std::find(consumed.begin(), consumed.end(), recipe.dataset_id) != consumed.end();
  };
  for (auto& [id, bindings] : recipes_) {
    // Collision rule (see the header): the anchor's own binding wins. mergeMarkerTopics
    // runs before this call (SessionManager::mergeDatasets, (3b)) and already folded
    // every consumed part onto the anchor under this owner id, so retiring the
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
  // The dataset's object topics went with it — nothing left to republish there.
  std::erase_if(parts_, [dataset](const auto& entry) { return entry.first.first == dataset; });

  // The removed dataset may have contributed to a shared set: rebuild each from the
  // survivors. With no contributor left the rule draws nothing (its parts go), but
  // it stays live so a contributor loaded later revives it.
  std::vector<std::string> affected;
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (!recipe.all_datasets || !shared_sets_.contains(recipe.id)) {
      continue;
    }
    Expected<std::optional<sdk::PlotMarkers>> shared = computeShared(recipe);
    if (!shared.has_value()) {
      continue;  // a script error keeps the last good set, as every recompute does
    }
    if (shared->has_value()) {
      if (publishSharedEverywhere(recipe, std::move(**shared)).has_value()) {
        affected.push_back(sdk::markerObjectTopicName(recipe.outputs.front()));
      }
      continue;
    }
    shared_sets_.erase(recipe.id);
    affected.push_back(sdk::markerObjectTopicName(recipe.outputs.front()));
    dropParts(recipe.id, {}, recipe.ephemeral);
  }
  return affected;
}

std::vector<std::string> MarkerService::recomputeForChangedInputs(
    const std::vector<std::string>& changed, DatasetId scope) {
  std::vector<std::string> affected;
  const std::vector<DatasetId> loaded = loadedDatasets();
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
      reachUnpublishedDatasets(recipe, loaded, affected);
      continue;
    }
    // An all_datasets recipe's set is shared, so a change on one contributing
    // dataset re-runs the whole union and re-publishes it everywhere — `scope` only
    // attributes the change, it never bounds where the result lands.
    runAndAccumulate(recipe, affected);
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

void MarkerService::foldAnchorPartsBeforeMerge(DatasetId anchor, std::set<std::string>& touched) {
  for (const ObjectTopicId tid : object_store_.listTopics(anchor)) {
    const ObjectTopicDescriptor desc = object_store_.descriptor(tid);
    if (!isMarkerObjectTopic(desc.topic_name)) {
      continue;
    }
    const PublishTarget target{anchor, desc.topic_name};
    const std::optional<ResolvedObjectEntry> stored = latestMarkerEntry(tid);
    const Timestamp shift = stored.has_value() ? stored->payload_stamp_shift : Timestamp{0};

    if (const auto it = parts_.find(target); it != parts_.end()) {
      if (shift != 0) {
        for (auto& [owner, part] : it->second) {
          shiftMarkers(part.markers, shift);
        }
      }
    } else if (std::optional<sdk::PlotMarkers> decoded = decodeStoredMarkers(stored); decoded.has_value()) {
      shiftMarkers(decoded->markers, shift);
      parts_[target][std::string(kForeignOwner)] = std::move(*decoded);
    }
    touched.insert(desc.topic_name);
  }
}

void MarkerService::foldSourcePartsOntoAnchor(
    DatasetId anchor, const DatasetMergeSource& source, std::set<std::string>& touched) {
  std::vector<ObjectTopicId> to_remove;
  for (const ObjectTopicId tid : object_store_.listTopics(source.dataset_id)) {
    const ObjectTopicDescriptor desc = object_store_.descriptor(tid);
    if (!isMarkerObjectTopic(desc.topic_name)) {
      continue;
    }
    const PublishTarget src_target{source.dataset_id, desc.topic_name};
    const PublishTarget dst_target{anchor, desc.topic_name};
    const std::optional<ResolvedObjectEntry> stored = latestMarkerEntry(tid);
    const Timestamp total_shift = source.raw_shift_ns + (stored.has_value() ? stored->payload_stamp_shift : 0);
    const auto append_to = [](sdk::PlotMarkers& dst, std::vector<sdk::PlotMarker>&& markers) {
      dst.markers.insert(
          dst.markers.end(), std::make_move_iterator(markers.begin()), std::make_move_iterator(markers.end()));
    };

    if (const auto it = parts_.find(src_target); it != parts_.end()) {
      PartsByOwner& dst_owners = parts_[dst_target];
      for (auto& [owner, part] : it->second) {
        // A shared (all_datasets) set is the SAME set on every dataset: the anchor's
        // copy already holds it, so the source's copy would only double every marker.
        if (shared_sets_.contains(owner) && dst_owners.contains(owner)) {
          continue;
        }
        shiftMarkers(part.markers, total_shift);
        append_to(dst_owners[owner], std::move(part.markers));
      }
      parts_.erase(it);
    } else if (std::optional<sdk::PlotMarkers> decoded = decodeStoredMarkers(stored); decoded.has_value()) {
      shiftMarkers(decoded->markers, total_shift);
      append_to(parts_[dst_target][std::string(kForeignOwner)], std::move(decoded->markers));
    }
    touched.insert(desc.topic_name);
    to_remove.push_back(tid);
  }
  for (const ObjectTopicId tid : to_remove) {
    object_store_.removeTopic(tid);
  }
}

std::vector<std::string> MarkerService::mergeMarkerTopics(
    DatasetId anchor, const std::vector<DatasetMergeSource>& sources) {
  // Names of every anchor marker target touched by this merge (dataset is always
  // `anchor`) — republished once at the end, in a deterministic sorted order.
  std::set<std::string> touched;

  // MUST run before the source loop below — see foldAnchorPartsBeforeMerge's doc.
  foldAnchorPartsBeforeMerge(anchor, touched);

  for (const DatasetMergeSource& src : sources) {
    foldSourcePartsOntoAnchor(anchor, src, touched);
  }

  // Republish every touched anchor target once, as the union of its parts.
  std::vector<std::string> affected;
  for (const std::string& name : touched) {
    if (republishUnion(PublishTarget{anchor, name}).has_value()) {
      affected.push_back(name);
    }
  }
  return affected;
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
