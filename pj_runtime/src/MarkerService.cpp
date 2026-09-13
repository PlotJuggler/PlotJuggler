// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/MarkerService.h"

#include <algorithm>
#include <limits>
#include <map>
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

Status MarkerService::runMarkersToObjectTopic(
    DatasetId dataset_id, const GeneratorRecipe& recipe, const std::string& object_topic_name) {
  std::unordered_map<std::string, scripting::SeriesView> views =
      materializeInputs(resolver_, dataset_id, recipe.inputs);
  scripting::SeriesProvider provider = buildProvider(views, recipe.inputs, recipe.declared_inputs);

  std::string err;
  std::vector<sdk::PlotMarker> markers = scripting::runMarkerScript(recipe.script, provider, &err);
  if (!err.empty()) {
    return unexpected(err);
  }

  sdk::PlotMarkers set;
  set.markers = std::move(markers);
  if (Status pushed = publishMarkerSet(dataset_id, object_topic_name, set); !pushed.has_value()) {
    return unexpected(pushed.error());
  }
  return okStatus();
}

Expected<std::vector<std::string>> MarkerService::runMarkers(const GeneratorRecipe& recipe, DatasetId scope) {
  const std::string marker_topic = recipe.outputs.empty() ? std::string{} : recipe.outputs.front();
  const std::string object_topic_name = sdk::markerObjectTopicName(marker_topic);
  // One dataset when bound, the scoped set when global — each computed over that
  // dataset's own series. Keep going on a per-dataset error and report the last, so
  // one unresolvable dataset does not suppress the others' markers.
  Status result = okStatus();
  for (const DatasetId dataset_id : targetDatasets(recipe, scope)) {
    if (Status s = runMarkersToObjectTopic(dataset_id, recipe, object_topic_name); !s.has_value()) {
      result = s;
    }
  }
  if (!result.has_value()) {
    return unexpected(result.error());
  }
  return std::vector<std::string>{object_topic_name};
}

// ---- routing + lifecycle ---------------------------------------------------

Expected<std::vector<std::string>> MarkerService::runAndPublish(const GeneratorRecipe& recipe, DatasetId scope) {
  switch (recipe.kind) {
    case GeneratorKind::kMarkers:
      return runMarkers(recipe, scope);
  }
  return unexpected("unknown generator kind");
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

  Expected<std::vector<std::string>> resolved = runAndPublish(recipe);
  if (!resolved.has_value()) {
    return resolved;  // leave any prior published output + recipe untouched
  }

  // An upsert that retargets (renamed output topic, rebound dataset, global toggled
  // off) leaves the previous topic published forever with a set nothing ever revisits
  // — no later recompute or remove names it again. Retire only what the new revision
  // does NOT cover: the overlap was just rewritten above, and tombstoning it would
  // erase the fresh set.
  if (const auto prev = recipes_.find(recipe.id); prev != recipes_.end()) {
    std::vector<PublishTarget> stale = publishTargets(prev->second);
    const std::vector<PublishTarget> live = publishTargets(recipe);
    std::erase_if(stale, [&live](const PublishTarget& target) {
      return std::find(live.begin(), live.end(), target) != live.end();
    });
    retirePublished(stale, prev->second.ephemeral);
  }

  recipes_[recipe.id] = std::move(recipe);
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

std::vector<DatasetId> MarkerService::targetDatasets(const GeneratorRecipe& recipe, DatasetId scope) const {
  if (!recipe.all_datasets) {
    return {recipe.dataset_id};
  }
  if (scope != kAllDatasets) {
    return {scope};
  }
  return loadedDatasets();
}

std::vector<MarkerService::PublishTarget> MarkerService::publishTargets(const GeneratorRecipe& recipe) const {
  std::vector<PublishTarget> targets;
  if (recipe.outputs.empty()) {
    return targets;
  }
  const std::string object_topic_name = sdk::markerObjectTopicName(recipe.outputs.front());
  for (const DatasetId dataset_id : targetDatasets(recipe, kAllDatasets)) {
    targets.emplace_back(dataset_id, object_topic_name);
  }
  return targets;
}

void MarkerService::retirePublished(const std::vector<PublishTarget>& targets, bool ephemeral) {
  for (const auto& [dataset_id, object_topic_name] : targets) {
    if (ephemeral) {
      if (const std::optional<ObjectTopicId> oid = object_store_.findTopic(dataset_id, object_topic_name)) {
        object_store_.removeTopic(*oid);
      }
    } else {
      publishEmptyMarkers(dataset_id, object_topic_name);
    }
  }
}

Status MarkerService::removeGenerator(std::string_view id) {
  const auto it = recipes_.find(std::string(id));
  if (it == recipes_.end()) {
    return unexpected("unknown generator id");
  }
  retirePublished(publishTargets(it->second), it->second.ephemeral);
  recipes_.erase(it);
  return okStatus();
}

void MarkerService::remapGeneratorsToAnchor(DatasetId anchor, const std::vector<DatasetId>& consumed) {
  for (auto& entry : recipes_) {
    GeneratorRecipe& recipe = entry.second;
    if (recipe.all_datasets) {
      continue;  // resolved through the dataset lister; no stored id to rebind
    }
    if (std::find(consumed.begin(), consumed.end(), recipe.dataset_id) != consumed.end()) {
      recipe.dataset_id = anchor;
    }
  }
}

void MarkerService::clearGeneratorsForDataset(DatasetId dataset) {
  std::erase_if(recipes_, [dataset](const auto& entry) {
    return !entry.second.all_datasets && entry.second.dataset_id == dataset;
  });
}

std::vector<std::string> MarkerService::recomputeForChangedInputs(
    const std::vector<std::string>& changed, DatasetId scope) {
  std::vector<std::string> affected;
  for (const auto& entry : recipes_) {
    const GeneratorRecipe& recipe = entry.second;
    // Topic names are not unique across datasets, so the name test alone would re-run
    // a generator bound to a dataset that never changed.
    if (scope != kAllDatasets && !recipe.all_datasets && recipe.dataset_id != scope) {
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
    if (Expected<std::vector<std::string>> resolved = runAndPublish(recipe, scope); resolved.has_value()) {
      for (std::string& topic : *resolved) {
        affected.push_back(std::move(topic));
      }
    }
  }
  return affected;
}

std::vector<std::string> MarkerService::exemptDependentsOf(const std::vector<std::string>& removed_inputs) const {
  std::vector<std::string> outputs;
  for (const auto& [id, recipe] : recipes_) {
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
      outputs.push_back(id);
    } else {
      outputs.insert(outputs.end(), recipe.outputs.begin(), recipe.outputs.end());
    }
  }
  std::sort(outputs.begin(), outputs.end());
  outputs.erase(std::unique(outputs.begin(), outputs.end()), outputs.end());
  return outputs;
}

bool MarkerService::hasHistoryExemptGenerators() const {
  return std::any_of(recipes_.begin(), recipes_.end(), [](const auto& entry) { return entry.second.history_exempt; });
}

void MarkerService::clearGenerators(RestoreIntent intent) {
  const bool keep_history_exempt = intent == RestoreIntent::kHistory;
  std::vector<std::string> ids;
  ids.reserve(recipes_.size());
  for (const auto& [id, recipe] : recipes_) {
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
  // `changed` re-runs every recipe, and the scope keeps a global rule from also
  // re-publishing onto datasets the reload never touched.
  return recomputeForChangedInputs(/*changed=*/{}, dataset);
}

std::vector<std::string> MarkerService::mergeMarkerTopics(
    DatasetId anchor, const std::vector<DatasetMergeSource>& sources) {
  // Accumulate, per marker object-topic name, the full set to republish to the anchor.
  std::map<std::string, sdk::PlotMarkers> merged;

  const auto foldTopic = [this, &merged](ObjectTopicId tid, const std::string& name, Timestamp shift) {
    const std::optional<ResolvedObjectEntry> entry = object_store_.latestAt(tid, std::numeric_limits<Timestamp>::max());
    if (!entry.has_value()) {
      return;
    }
    Expected<sdk::PlotMarkers> decoded =
        deserializePlotMarkers(entry->payload.bytes.data(), entry->payload.bytes.size());
    if (!decoded.has_value()) {
      return;
    }
    // Bake any pending merge delta plus this source's shift into the marker times
    // so the republished blob needs no side-channel shift. kValueBand ignores
    // t_start/t_end, so shifting them is a harmless no-op for that kind.
    const Timestamp total_shift = shift + entry->payload_stamp_shift;
    std::vector<sdk::PlotMarker>& dst = merged[name].markers;
    for (sdk::PlotMarker& m : decoded->markers) {
      m.t_start += total_shift;
      m.t_end += total_shift;
      dst.push_back(std::move(m));
    }
  };

  // Seed with the anchor's own marker sets (kept on the anchor clock, shift 0).
  for (const ObjectTopicId tid : object_store_.listTopics(anchor)) {
    const ObjectTopicDescriptor desc = object_store_.descriptor(tid);
    if (isMarkerObjectTopic(desc.topic_name)) {
      foldTopic(tid, desc.topic_name, 0);
    }
  }

  // Fold each source's marker sets (shifted onto the anchor clock), then drop them.
  for (const DatasetMergeSource& src : sources) {
    std::vector<ObjectTopicId> to_remove;
    for (const ObjectTopicId tid : object_store_.listTopics(src.dataset_id)) {
      const ObjectTopicDescriptor desc = object_store_.descriptor(tid);
      if (!isMarkerObjectTopic(desc.topic_name)) {
        continue;
      }
      foldTopic(tid, desc.topic_name, src.raw_shift_ns);
      to_remove.push_back(tid);
    }
    for (const ObjectTopicId tid : to_remove) {
      object_store_.removeTopic(tid);
    }
  }

  // Republish one consolidated blob per marker topic to the anchor.
  std::vector<std::string> affected;
  for (const auto& [object_topic_name, set] : merged) {
    if (publishMarkerSet(anchor, object_topic_name, set).has_value()) {
      affected.push_back(object_topic_name);
    }
  }
  return affected;
}

std::vector<MarkerService::GeneratorRecipe> MarkerService::recipes() const {
  std::vector<GeneratorRecipe> out;
  out.reserve(recipes_.size());
  for (const auto& entry : recipes_) {
    if (!entry.second.ephemeral) {
      out.push_back(entry.second);
    }
  }
  return out;
}

}  // namespace PJ
