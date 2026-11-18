#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_runtime/HistoryScope.h"

class QTimer;

namespace PJ {

class DataEngine;
class IncrementalEngine;
class SessionManager;
namespace proc {
class DataProcessor;
}
namespace scripting {
class FilterCatalogue;
}

/// Runs filters and transforms as eager `IncrementalEngine` nodes over the session's `DataEngine`, and owns that
/// `IncrementalEngine`.
///
/// GUI thread only, so the recipe maps need no lock. Streaming ingest writes through the `DataEngine` and never calls
/// in here; `StreamingSourceManager` calls `advanceOnCommit` from its GUI-thread tick.
class DataProcessorService {
 public:
  /// Wall time one materialization tick may take: half a 60 Hz frame (TECHNICAL_NOTES.md "Time-sliced replay in the
  /// browser" for why the browser keeps it too).
  static constexpr std::chrono::milliseconds kMaterializationSliceBudget{8};
  /// Least interval between two announcements of a growing replay's rows: each makes every plot showing the output
  /// re-read it.
  static constexpr std::chrono::milliseconds kMaterializationNotifyInterval{66};

  struct FilterHandle {
    NodeId node_id = 0;
    TopicId output_topic_id = 0;
    DatasetId dataset_id = 0;
    std::string output_name;
  };

  /// A live filter, keyed by its output topic: what the Filter Editor needs to re-open on the output and edit it in
  /// place, including the live configured processor.
  struct FilterRecipe {
    NodeId node_id = 0;
    TopicId output_topic_id = 0;
    TopicId input_topic_id = 0;
    std::size_t input_column_index = 0;
    std::string input_field_path;  ///< the leaf's identity; survives a schema column reorder
    DatasetId dataset_id = 0;
    std::string processor_id;  ///< catalogue id (e.g. "scale"), selects the panel
    std::string output_name;
    std::shared_ptr<proc::DataProcessor> processor;
    /// The Luau module source of the filter class, embedded in a layout as `<source_fallback>` so it reopens on a
    /// machine without the filter. Empty for native builtins, which round-trip by `processor_id`.
    std::string filter_source;
  };

  /// A filter as a layout snapshot describes it, input resolved against the live session. Identity is (`dataset_id`,
  /// `output_name`): output names are unique per dataset only. The other fields decide whether the live filter is the
  /// same.
  struct PersistedFilter {
    TopicId input_topic_id = 0;
    DatasetId dataset_id = 0;
    std::size_t input_column_index = 0;
    std::string processor_id;
    std::string params_json;    ///< must equal the live processor's `saveParams()`
    std::string filter_source;  ///< empty for native builtins
    std::string output_name;
  };

  /// Persisted identity of one transform input. `dataset_id` is only a same-session hint; source, topic and field are
  /// stable across reloads; the column is a fallback for metadata-poor topics.
  struct TransformInputBinding {
    DatasetId dataset_id = 0;
    std::string dataset_source;
    std::string topic_name;
    std::string field_path;
    std::size_t column_index = 0;
  };

  /// A plugin-created transform: named, owner-tagged, keyed `"<plugin_id>/<id>"`, carrying its script and bindings by
  /// value so it replays from a layout without the plugin (needed only to re-edit it). The last four fields are
  /// runtime state, re-derived on install.
  struct TransformRecipe {
    std::string key;           ///< "<plugin_id>/<id>", the upsert key
    std::string owner_plugin;  ///< manifest plugin id, not the DSO path
    /// Persisted, but outside undo/redo; a layout replacement or an explicit removal still replaces it.
    bool history_exempt = false;
    std::string user_id;                 ///< the per-plugin id; also the script's class id
    std::vector<std::string> inputs;     ///< input names: a topic, or `topic/field`
    std::size_t input_column_index = 0;  ///< column of a single whole-topic input
    std::vector<std::string> outputs;    ///< output topic names; `"t:a,b"` is one grouped topic `t`
    std::string script;                  ///< backend payload, binary-safe
    std::string params_json;             ///< create(params), verbatim
    std::string backend = "luau";        ///< "luau" or "python", inferred from the payload
    std::string api_version = "1";
    std::string backend_version;
    /// Exact input identities captured at the first successful resolution; empty for a name-only recipe.
    std::vector<TransformInputBinding> input_bindings;
    /// A preview: a real node, but not in `transformRecipes()` (never persisted). The caller keeps its outputs out of
    /// the catalog, so nothing remains once it is removed.
    bool ephemeral = false;
    NodeId node_id = 0;                     ///< runtime
    std::vector<TopicId> input_topic_ids;   ///< runtime
    std::vector<TopicId> output_topic_ids;  ///< runtime
    DatasetId dataset_id = 0;               ///< runtime: the first input's dataset
  };

  explicit DataProcessorService(DataEngine& engine);
  explicit DataProcessorService(SessionManager& session);
  ~DataProcessorService();
  DataProcessorService(const DataProcessorService&) = delete;
  DataProcessorService& operator=(const DataProcessorService&) = delete;

  /// Applies the catalogue filter `processor_id` to column `input_column_index` of `input_topic_id`, materializing
  /// `output_name` in `dataset_id` and running it over every committed input row. Errors: unknown id, engine
  /// rejection, empty output, column out of range, or a preview transform's output as input (a filter persists).
  [[nodiscard]] Expected<FilterHandle> applyFilter(
      TopicId input_topic_id, DatasetId dataset_id, std::string_view processor_id, std::string output_name,
      std::size_t input_column_index = 0);

  /// As the by-id overload, with a processor the caller configured (the Filter Editor).
  [[nodiscard]] Expected<FilterHandle> applyFilter(
      TopicId input_topic_id, DatasetId dataset_id, std::unique_ptr<proc::DataProcessor> processor,
      std::string output_name, std::size_t input_column_index = 0);

  /// Drops a filter, retires its output and removes its exact-TopicId dependents leaf-first. Unknown id: error.
  Status removeFilter(NodeId node_id);

  /// Removes every live filter except those whose output is in `keep_outputs`: drops the node and retires its output
  /// (`removeNode` alone leaves a catalog zombie), dependents first, so a kept filter can still fall to the cascade of
  /// a removed input. The retired `TopicStorage` stays allocated, so a cached reader pointer sees an empty deque. The
  /// reconcile primitive of snapshot restore (undo/redo, layout load). A filter has no exemption bit: history keeps
  /// one only through `keep_outputs`.
  void clearAllFilters(const std::unordered_set<TopicId>& keep_outputs = {});
  /// Outputs of the live filters a restore leaves unchanged: (dataset, `output_name`) matches a `wanted` entry with
  /// the same input topic and column, processor id, `saveParams()` and source. Pass them as `keep_outputs`.
  [[nodiscard]] std::unordered_set<TopicId> unchangedFilterOutputs(const std::vector<PersistedFilter>& wanted) const;
  [[nodiscard]] bool isFilterLive(DatasetId dataset_id, std::string_view output_name) const;
  /// A persistent transform with this key (what `transformRecipes()` lists); `include_ephemeral` adds previews.
  [[nodiscard]] bool isTransformLive(std::string_view namespaced_key, bool include_ephemeral = false) const;

  /// Removes every persistent transform, leaf-first (a preview stays unless an input's cascade takes it). `kHistory`
  /// leaves the `history_exempt` ones; `kReplace` removes all. `keep_keys` names others to keep, so their output
  /// TopicIds and the curves bound to them survive; a kept one can still fall to an input's cascade.
  void clearTransforms(RestoreIntent intent, const std::unordered_set<std::string>& keep_keys = {});
  /// Keys of the live transforms within the intent's authority whose persisted recipe equals the `wanted` entry of
  /// the same key, field by field as a layout carries it; a `wanted` entry without bindings never matches. Pass them
  /// as `keep_keys`.
  [[nodiscard]] std::unordered_set<std::string> unchangedTransformKeys(
      const std::vector<TransformRecipe>& wanted, RestoreIntent intent) const;
  /// The cheap gate of the history-restore dependency preflight.
  [[nodiscard]] bool hasHistoryExemptTransforms() const;

  /// nullptr when `output_topic_id` is no filter output.
  [[nodiscard]] const FilterRecipe* filterConfig(TopicId output_topic_id) const;

  /// Replaces a live filter's processor in place (same node and output topic) and recomputes, so the plotted curve
  /// updates without re-binding.
  [[nodiscard]] Status updateFilter(NodeId node_id, std::shared_ptr<proc::DataProcessor> processor);

  /// Advances every live filter over the newly committed input and returns the filter outputs to re-notify. Call it
  /// between the raw commit and `DataEngine::enforceRetention`: a stateful processor (integral) must consume every
  /// sample before it is evicted. Never throws. GUI thread.
  [[nodiscard]] std::vector<TopicId> advanceOnCommit(const std::vector<TopicId>& changed_inputs);

  /// Reload path: re-resolves inputs by their semantic fields in place, keeping node and output ids, then replays the
  /// affected graph once.
  [[nodiscard]] Expected<std::vector<TopicId>> rebindAndRecomputeForReplacedSources(
      const std::vector<TopicId>& replaced_inputs);

  /// Every live filter, unordered, for layout persistence.
  [[nodiscard]] std::vector<FilterRecipe> recipes() const;

  /// (id, label) of every catalogued filter, in catalogue order; empty without a catalogue.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> availableFilters() const;

  /// The by-id `applyFilter` and `availableFilters` resolve through it; without one there are no filters.
  void setFilterCatalogue(std::shared_ptr<scripting::FilterCatalogue> catalogue);
  [[nodiscard]] const scripting::FilterCatalogue* filterCatalogue() const {
    return filter_catalogue_.get();
  }

  /// A processor for a persisted filter: the catalogue entry `id`, else the layout's embedded `source_fallback`
  /// (compiled even with no catalogue installed), else nullptr (the caller logs a skip).
  [[nodiscard]] std::unique_ptr<proc::DataProcessor> makeRestoredProcessor(
      const std::string& id, const std::string& params_json, const std::string& source_fallback) const;

  // pj.data_processors.v1 transforms

  /// Creates or replaces (by `"<plugin_id>/<id>"`) a plugin transform. GUI thread. The first resolution refuses
  /// ambiguous names and captures a dataset/source/topic/field binding per input; restores resolve it through
  /// SessionManager's identity policy. Outputs live in the first input's dataset. 1->1 installs a SISO node, anything
  /// else a MIMO node joining inputs on exact timestamps. A compatible replacement swaps the operator in place and
  /// rolls back root and dependents if the replay fails; a shape change is rejected before mutating.
  ///
  /// `ephemeral`: a preview, never persisted; the caller keeps it out of the catalog and removes it. A persistent
  /// transform reading a preview's output is refused (the preview's removal would cascade into it).
  ///
  /// A new persistent key replays the input history time-sliced: only the first `kMaterializationSliceBudget` slice
  /// runs in the call (so a script failing on its first rows is still rejected), the rest from a zero-timer
  /// (`stepMaterialization`). Recipe and output exist on return and the output grows in time order to what a
  /// synchronous replay gives. Previews and in-place replacements replay synchronously.
  [[nodiscard]] Expected<TransformRecipe> upsertTransform(
      std::string_view plugin_id, std::string_view id, std::vector<std::string> inputs,
      std::vector<std::string> outputs, std::string_view script, std::string_view params_json, bool ephemeral = false,
      std::size_t input_column_index = 0, bool history_exempt = false);

  /// One tick of the pending replays (oldest first) for about `budget`; processors chained on a growing output follow
  /// within the same budget. New rows are announced through `SessionManager::notifyIngest` at most every
  /// `kMaterializationNotifyInterval`, and at once when a replay completes or fails. True when none is pending.
  /// Removing, editing or recomputing a transform ends its replay. GUI thread.
  bool stepMaterialization(std::chrono::nanoseconds budget = kMaterializationSliceBudget);

  /// Completes the pending replays whose output (or a chained processor's) is `read`. Installed as the
  /// `DataEngine::awaitCompleteSeries` barrier for whole-series readers. GUI thread.
  void finishPendingMaterialization(TopicId read);

  /// Every transform whose node fails in a scheduling pass is reported once as an error `Diagnostic` keyed by its
  /// transform key, and again only after it recovered. Without a sink, only logged.
  void setDiagnosticSink(DiagnosticSink sink) {
    diagnostic_sink_ = std::move(sink);
  }

  /// Receives the names of processor outputs whose rows changed, with their dataset (never empty): when an upsert,
  /// filter apply, in-place edit or restore leaves an output complete, when a time-sliced replay ends, and before a
  /// removal (cascade included); chained processors are named too. A reload's rebind does not call it
  /// (`RefillGuard::recomputeProcessors` runs the marker rules itself). GUI thread.
  using OutputsChangedSink = std::function<void(const std::vector<std::string>& names, DatasetId dataset)>;
  void setOutputsChangedSink(OutputsChangedSink sink) {
    outputs_changed_sink_ = std::move(sink);
  }

  /// True while a time-sliced replay has input left.
  [[nodiscard]] bool materializationPending() const;

  /// Anything but "luau" or "python" (empty means luau) is refused; shared by `validateScript` and the bridge.
  [[nodiscard]] static Status checkLanguage(std::string_view language);

  /// Compiles for `language` and runs one synthetic point to catch syntax and trivial runtime errors (a nil or
  /// non-numeric return), installing nothing. GUI thread.
  Status validateScript(std::string_view script, std::string_view language, std::string_view params_json);

  /// Transforms transitively consuming the removed TopicIds; filters are traversed so mixed chains count.
  [[nodiscard]] std::vector<TransformRecipe> transformsDependingOn(const std::vector<TopicId>& removed_topics) const;

  /// Outputs of every processor (previews included, as removeProcessorsDependingOn) transitively depending on
  /// `input_topics`, parents first.
  [[nodiscard]] std::vector<TopicId> dependentProcessorOutputs(const std::vector<TopicId>& input_topics) const;

  /// Leaf-first, so no recipe stays attached to a retired topic. A failed removal does not stop the others; the
  /// first error is returned.
  [[nodiscard]] Status removeProcessorsDependingOn(const std::vector<TopicId>& input_topics);

  /// The processor nodes a history restore would remove (filters outside `keep_filter_outputs`, non-exempt transforms
  /// outside `keep_keys`): pass them to `exemptDependentsOf` before clearing, to reject instead of cascading into an
  /// exempt dependent.
  [[nodiscard]] std::vector<NodeId> liveNonExemptNodeIds(
      const std::unordered_set<std::string>& keep_keys = {},
      const std::unordered_set<TopicId>& keep_filter_outputs = {}) const;

  /// What a restore with these keep sets takes down, in one dependency walk; exempt transforms are spared only under
  /// `kHistory`.
  struct RemovalImpact {
    /// Output names (sorted, unique) of the exempt transforms the cascade reaches; non-empty under `kHistory` means
    /// reject.
    std::vector<std::string> exempt_transform_names;
    /// Topic names of every processor removed, directly or by cascade (a kept processor reading a removed output falls
    /// too and returns under a new TopicId); `"t:a,b"` reports `"t"`. Marker rules match their series keys against it.
    std::vector<std::string> removed_output_names;
  };
  [[nodiscard]] RemovalImpact removalImpact(
      RestoreIntent intent, const std::unordered_set<std::string>& keep_keys,
      const std::unordered_set<TopicId>& keep_filter_outputs) const;

  /// Output names of the exempt transforms transitively depending on `nodes`, as removeProcessorsDependingOn would
  /// cascade. Marker rules' dependencies on these outputs are not tracked here.
  [[nodiscard]] std::vector<std::string> exemptDependentsOf(const std::vector<NodeId>& nodes) const;

  /// Topic names (sorted, unique) `removeTransform(transform_key)` would retire, cascade included. Empty for an unknown
  /// key.
  [[nodiscard]] std::vector<std::string> removedOutputNames(std::string_view transform_key) const;

  /// Cascades through every exact downstream processor. Unknown key: error.
  Status removeTransform(std::string_view namespaced_key);
  /// For a removal the owner asks for (a plugin, an agent): refused, removing nothing, when the cascade would take a
  /// processor of another owner (a transform of another `owner_plugin`, or a filter); the error names each. The shell
  /// asks the user and uses `removeTransform`.
  Status removeOwnTransform(std::string_view namespaced_key);

  /// Removes the previews among `keys`, cascading; gone or persistent keys are skipped. `DerivedRuntimeHosts` passes
  /// its bridge's previews when the plugin instance ends.
  void removeEphemerals(const std::unordered_set<std::string>& keys);

  void clearAllTransforms();

  /// Every transform whose output lives in `dataset_id`.
  void clearTransformsForDataset(DatasetId dataset_id);

  /// For the ABI `list` slot.
  [[nodiscard]] std::vector<std::string> transformIdsForPlugin(std::string_view plugin_id) const;

  /// For the ABI `config` slot: `{"inputs","outputs","params","backend","history_exempt","gaps"}`, `gaps` being the
  /// node's retention gaps (`IncrementalEngine::gaps`: input evicted before the node consumed it; `from_ns` null when
  /// it had consumed nothing). nullopt for an unknown key.
  [[nodiscard]] std::optional<std::string> transformRecipeJson(std::string_view namespaced_key) const;

  /// Every live transform, unordered, for layout save.
  [[nodiscard]] std::vector<TransformRecipe> transformRecipes() const;

  /// Previews excluded.
  [[nodiscard]] std::optional<TransformRecipe> transformOwning(TopicId output_topic) const;

  /// The live (dataset, topic name) inputs of a filter or transform output, so `TopicDemandTracker` keeps a displayed
  /// derived series' inputs subscribed (otherwise a streaming source would unsubscribe them and stall the filter).
  /// Empty for an unknown output or an input no longer resolvable by name.
  [[nodiscard]] std::vector<std::pair<DatasetId, std::string>> sourceTopicsForOutput(TopicId output_topic_id) const;

  /// Preview outputs; the catalog leaves them out.
  [[nodiscard]] std::unordered_set<TopicId> ephemeralOutputTopics() const;

  /// Previews included.
  [[nodiscard]] std::unordered_set<TopicId> processorOutputTopics() const;

  /// Layout load: the `upsertTransform` install path. An unavailable backend is an error and stores nothing (the
  /// caller keeps the XML and reports it).
  [[nodiscard]] Expected<TransformRecipe> restoreTransform(const TransformRecipe& recipe);

  [[nodiscard]] static std::string makeTransformKey(std::string_view plugin_id, std::string_view id);

  /// The topic names `recipe`'s outputs materialize (`"t:a,b"` is one topic `"t"`): what downstream series keys are
  /// rooted at.
  [[nodiscard]] static std::vector<std::string> materializedTopicNames(const TransformRecipe& recipe);

  [[nodiscard]] IncrementalEngine& incrementalEngine() noexcept {
    return *derived_;
  }

 private:
  /// The upsert and restore install path; `time_sliced` replays only the first slice here.
  [[nodiscard]] Expected<TransformRecipe> installTransform(TransformRecipe recipe, bool time_sliced = false);
  /// Without a Qt application tests step by hand.
  void armMaterializationTimer();
  /// `dependents` receives the chained processors' node ids, oldest first.
  [[nodiscard]] std::vector<TopicId> outputsWithDependents(
      NodeId node, std::vector<NodeId>* dependents = nullptr) const;
  void announceGrownOutputs();
  void announceChangedOutputs(const std::vector<TopicId>& outputs) const;
  /// A persistent recipe may not read a preview's output: the preview's removal would cascade into it.
  [[nodiscard]] bool readsAPreview(const std::vector<TopicId>& inputs) const;
  /// Reports transforms newly failed and forgets recovered or removed ones; called around every scheduling pass.
  void reportTransformFailures();
  void installCompleteReadBarrier();
  [[nodiscard]] Status removeFilterOnly(NodeId node_id);
  [[nodiscard]] Status removeTransformOnly(std::string_view namespaced_key);
  /// By scanning the engine (no name index). nullopt for no match or several (`*ambiguous` set).
  [[nodiscard]] std::optional<std::pair<TopicId, DatasetId>> resolveInputTopic(
      const std::string& name, std::optional<DatasetId> only_dataset = std::nullopt, bool* ambiguous = nullptr) const;
  struct ResolvedInput {
    TopicId topic_id = 0;
    DatasetId dataset_id = 0;
    std::size_t column = 0;  ///< flattened-leaf column index feeding the transform
  };
  /// A whole topic ("pose/orientation", column 0) or a field path ("pose/orientation/x") to (topic, dataset, leaf
  /// column). nullopt when nothing matches, the field is no leaf, or several match (`*ambiguous` set). Topic names can
  /// repeat inside one dataset: a whole-topic name matching twice is ambiguous, with no shorter `topic/field` reading.
  [[nodiscard]] std::optional<ResolvedInput> resolveInputField(
      const std::string& name, std::optional<DatasetId> only_dataset = std::nullopt, bool* ambiguous = nullptr) const;
  /// The qualifier form; nullopt when unknown.
  [[nodiscard]] std::optional<std::string> datasetSourceName(DatasetId dataset_id) const;
  /// The spelling the config read-back echoes: bare, unless the bare name is ambiguous across datasets and
  /// "source:bare" resolves back to the binding's dataset.
  [[nodiscard]] std::string readBackInputName(const std::string& bare, const TransformInputBinding& binding) const;
  [[nodiscard]] Expected<ResolvedInput> resolveInputBinding(const TransformInputBinding& binding) const;

  /// The processors a clear or a history restore removes directly; previews end with their plugin instance, never
  /// with a restore. The clears and the preflight queries all derive from it, so they cannot disagree.
  struct RemovalSet {
    std::vector<NodeId> filter_nodes;
    std::vector<std::string> transform_keys;
    std::unordered_set<TopicId> outputs;  ///< every output topic of the above
  };
  [[nodiscard]] RemovalSet removalSet(
      const std::unordered_set<std::string>& keep_keys, const std::unordered_set<TopicId>& keep_filter_outputs,
      bool keep_history_exempt) const;
  /// Grows `affected` through the cascade; returns the exempt transforms' output names.
  [[nodiscard]] std::vector<std::string> exemptTransformsDependingOn(std::unordered_set<TopicId>& affected) const;
  /// Retired or unknown ids are skipped.
  [[nodiscard]] std::vector<std::string> topicNames(const std::unordered_set<TopicId>& topics) const;

  /// Visits each processor transitively depending on `affected` once, parents first; `affected` grows. Without
  /// `include_ephemeral_transforms` previews are neither visited nor traversed.
  void forEachDependentProcessor(
      std::unordered_set<TopicId>& affected, bool include_ephemeral_transforms,
      const std::function<void(TopicId, const FilterRecipe&)>& on_filter,
      const std::function<void(const std::string&, const TransformRecipe&)>& on_transform) const;
  /// A live engine topic or another transform's output; `except_key`'s own outputs are allowed (in-place replace).
  [[nodiscard]] bool outputNameInUse(const std::string& name, const std::string& except_key) const;

  DataEngine& engine_;
  SessionManager* session_ = nullptr;  ///< the identity resolver; null only in engine-level tests
  std::unique_ptr<IncrementalEngine> derived_;
  std::unordered_map<TopicId, FilterRecipe> recipes_;                   ///< by output topic
  std::unordered_map<std::string, TransformRecipe> transform_recipes_;  ///< "<plugin>/<id>" -> live transform
  std::shared_ptr<scripting::FilterCatalogue> filter_catalogue_;
  DiagnosticSink diagnostic_sink_;
  OutputsChangedSink outputs_changed_sink_;
  std::unordered_set<std::string> reported_failures_;  ///< keys of failed transforms already reported
  std::unordered_set<TopicId> unannounced_outputs_;    ///< replay outputs grown since the last notify
  std::chrono::steady_clock::time_point last_announcement_{};
  /// Drives stepMaterialization(). Declared after derived_ so it dies first and never fires into a destroyed engine.
  std::unique_ptr<QTimer> materialize_timer_;
};

}  // namespace PJ
