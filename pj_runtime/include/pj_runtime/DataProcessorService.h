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

/// Host service that applies "Data Processors" (filters / transforms) to
/// datastore topics by running them as EAGER `pj_datastore::IncrementalEngine` nodes
/// (a `PJ::proc::ProcessorSisoAdapter` wraps each native processor). Owns one
/// `IncrementalEngine` bound to the session's `DataEngine`.
///
/// v1 scope (filters-first, file data): `applyFilter` registers a node, runs the
/// processor over all currently-committed input rows, and materializes a new
/// output topic. Streaming/pause (incremental `advanceOnCommit`) and recipe
/// persistence (layout save/restore) are handled; hidden-output tiers and
/// promotion remain later milestones.
///
/// Threading: GUI/main-thread only. The streaming ingest worker writes samples
/// directly through the `DataEngine` (serialized by the engine lock) and never
/// calls into this service; `advanceOnCommit` is hopped to the GUI thread
/// (`QMetaObject::invokeMethod`) before it runs. So the recipe maps are read
/// (`advanceOnCommit`) and mutated (`applyFilter` / `upsertTransform` /
/// `removeTransform`) only from that one thread — no internal locking needed.
class DataProcessorService {
 public:
  /// Wall time one materialization tick may take: half a 60 Hz frame. In a
  /// browser Qt shares the page's main thread and the zero-timer between two
  /// ticks is a clamped nested setTimeout (measured ~5-6 ms after a busy tick in
  /// Chromium and Firefox), so a tick cycle is ~13-14 ms and input and paint run
  /// in the gap. A 10 ms budget stalled Firefox's page timers for ~100 ms once
  /// per replay, so the browser keeps 8 ms too.
  static constexpr std::chrono::milliseconds kMaterializationSliceBudget{8};
  /// Least interval between two announcements of a growing replay's rows (about
  /// 15 Hz): each one makes every plot showing the output re-read it.
  static constexpr std::chrono::milliseconds kMaterializationNotifyInterval{66};

  /// A filter applied to one input topic: the engine node id plus the
  /// materialized output topic it produced.
  struct FilterHandle {
    NodeId node_id = 0;
    TopicId output_topic_id = 0;
    DatasetId dataset_id = 0;
    std::string output_name;
  };

  /// Stored description of a live filter, keyed by its output topic — enough to
  /// re-open the editor on the output, repopulate the parameter panels, and edit
  /// the filter in place. Holds the live configured processor (see D2/§9).
  struct FilterRecipe {
    NodeId node_id = 0;
    TopicId output_topic_id = 0;
    TopicId input_topic_id = 0;
    std::size_t input_column_index = 0;
    std::string input_field_path;  ///< semantic leaf identity; survives schema column reorder
    DatasetId dataset_id = 0;
    std::string processor_id;  ///< builtin id (e.g. "scale") — selects the panel
    std::string output_name;
    std::shared_ptr<proc::DataProcessor> processor;  ///< live processor, for re-edit
    /// Full Luau module source the filter class was parsed from, captured from the
    /// catalogue so a layout can embed it as `<source_fallback>` (M7) and reopen on
    /// a machine without the filter installed. Empty for native C++ builtins (which
    /// round-trip by `processor_id` alone).
    std::string filter_source;
  };

  /// A filter as a layout snapshot describes it, with its input already resolved
  /// against the live session — the persisted half of a `FilterRecipe`. Identity is
  /// (`dataset_id`, `output_name`): output names are unique per dataset only. The
  /// other fields decide whether the live filter under that identity is the same.
  struct PersistedFilter {
    TopicId input_topic_id = 0;
    DatasetId dataset_id = 0;
    std::size_t input_column_index = 0;
    std::string processor_id;
    std::string params_json;    ///< what the live processor's `saveParams()` must equal
    std::string filter_source;  ///< embedded Luau module source, empty for native builtins
    std::string output_name;
  };

  /// Persisted identity of one transform input. The session dataset id is only
  /// a same-session hint; source, topic, and field carry the reload-stable
  /// identity, while the column remains a fallback for metadata-poor topics.
  struct TransformInputBinding {
    DatasetId dataset_id = 0;
    std::string dataset_source;
    std::string topic_name;
    std::string field_path;
    std::size_t column_index = 0;
  };

  /// A plugin-created TRANSFORM: a named, owner-tagged, session-persisted node,
  /// distinct from a per-curve `FilterRecipe`. It shares the eager `IncrementalEngine`
  /// substrate but is keyed by a namespaced `"<plugin_id>/<id>"` key and carries
  /// its script payload + bindings BY VALUE, so it round-trips through a
  /// session/workspace recipe and replays on load WITHOUT the originating plugin
  /// (which is needed only for re-editing). The first three field groups are the
  /// persisted recipe; `node_id`/`output_topic_ids`/`dataset_id` are runtime state
  /// re-derived on (re)install.
  struct TransformRecipe {
    std::string key;           ///< "<plugin_id>/<id>" — the authoritative upsert key
    std::string owner_plugin;  ///< stable manifest plugin id (NOT the DSO path)
    /// Persisted in layout files but outside undo/redo authority. A layout
    /// replacement or explicit removal may still replace it.
    bool history_exempt = false;
    std::string user_id;                 ///< per-plugin id (no prefix); also the script's class id
    std::vector<std::string> inputs;     ///< input topic names (v1: exactly 1)
    std::size_t input_column_index = 0;  ///< column within the input topic (default 0)
    std::vector<std::string> outputs;    ///< named catalog output topics (v1: exactly 1)
    std::string script;                  ///< full backend payload (Luau source today; binary-safe)
    std::string params_json;             ///< create(params), forwarded verbatim to the backend
    std::string backend = "luau";        ///< inferred from the payload: "luau" | (future) "python"/"wasm"
    std::string api_version = "1";
    std::string backend_version;
    /// Exact input identities captured after a successful initial resolution.
    /// Empty means a legacy name-only recipe.
    std::vector<TransformInputBinding> input_bindings;
    /// Ephemeral (preview) node: a real `IncrementalEngine` node like any other, but
    /// excluded from `transformRecipes()` so it never persists to a layout. The
    /// caller also keeps its output topics out of the catalog (does not rebuild),
    /// so a live preview leaves no trace once `removeTransform`d. Not persisted.
    bool ephemeral = false;
    NodeId node_id = 0;                     ///< runtime: the engine node (not persisted)
    std::vector<TopicId> input_topic_ids;   ///< runtime: exact live input topics
    std::vector<TopicId> output_topic_ids;  ///< runtime: materialized output topics
    DatasetId dataset_id = 0;               ///< runtime: resolved from the input topic's dataset
  };

  explicit DataProcessorService(DataEngine& engine);
  explicit DataProcessorService(SessionManager& session);
  ~DataProcessorService();
  DataProcessorService(const DataProcessorService&) = delete;
  DataProcessorService& operator=(const DataProcessorService&) = delete;

  /// Apply a filter by its catalogue `processor_id` (e.g. "absolute",
  /// "moving_average") to column `input_column_index` (default 0) of
  /// `input_topic_id`, materializing a new output topic named `output_name` in
  /// `dataset_id`, then running it over all committed input rows. The column
  /// index selects one field of a multi-column topic (e.g. `/imu/.../x`).
  /// Returns the handle, or an error string (unknown id / engine rejection /
  /// empty output / column out of range / a preview transform's output as input,
  /// since a filter is persistent).
  [[nodiscard]] Expected<FilterHandle> applyFilter(
      TopicId input_topic_id, DatasetId dataset_id, std::string_view processor_id, std::string output_name,
      std::size_t input_column_index = 0);

  /// Apply an already-CONFIGURED processor (params set by the caller, e.g. the
  /// Filter Editor dialog) to column `input_column_index` of the input topic.
  /// Same materialize-and-run semantics as the by-id overload; takes ownership
  /// of `processor`.
  [[nodiscard]] Expected<FilterHandle> applyFilter(
      TopicId input_topic_id, DatasetId dataset_id, std::unique_ptr<proc::DataProcessor> processor,
      std::string output_name, std::size_t input_column_index = 0);

  /// Drop a filter, retire its output, and remove exact-TopicId downstream
  /// processors leaf-first. An unknown node id is an error.
  Status removeFilter(NodeId node_id);

  /// Remove every live filter except those whose output TopicId is in
  /// `keep_outputs`: for each one, drop its engine node AND `retireTopic` its
  /// materialized output (so `CatalogModel::rebuildFromDatastore` stops listing it
  /// — `removeNode` alone leaves a catalog zombie), then forget its recipe. The
  /// reconcile primitive behind snapshot restore (undo/redo + layout load): clear
  /// what the snapshot drops or changes, then re-apply the rest, so a restored
  /// state can never duplicate or leak a filter. Removed leaf-first (a filter's own
  /// dependents are cascaded via `removeFilter` before the filter itself), so a
  /// kept filter can still fall to the cascade of a removed input. The output
  /// `TopicStorage` is kept alive (only retired), so any cached reader pointer sees
  /// an empty deque, not freed memory. Idempotent; a no-op when nothing is live. A
  /// `FilterRecipe` carries no exemption bit: history keeps a filter only by
  /// `keep_outputs`, never by intent.
  void clearAllFilters(const std::unordered_set<TopicId>& keep_outputs = {});
  /// Output TopicIds of the live filters a restore leaves unchanged: those whose
  /// (dataset, `output_name`) matches a `wanted` entry with the same resolved
  /// input topic and column, processor id, `saveParams()` and embedded source.
  /// Feed the result to `clearAllFilters` / `liveNonExemptNodeIds` as
  /// `keep_outputs` so the kept filters survive with those TopicIds.
  [[nodiscard]] std::unordered_set<TopicId> unchangedFilterOutputs(const std::vector<PersistedFilter>& wanted) const;
  /// Whether a filter named `output_name` is live in `dataset_id` — the
  /// (dataset, output name) identity `PersistedFilter` carries.
  [[nodiscard]] bool isFilterLive(DatasetId dataset_id, std::string_view output_name) const;
  /// Whether a persisted (non-ephemeral) transform with this namespaced key is
  /// live — membership in what `transformRecipes()` lists, without the copy.
  /// `include_ephemeral` also answers for a preview.
  [[nodiscard]] bool isTransformLive(std::string_view namespaced_key, bool include_ephemeral = false) const;

  /// Remove every persistent transform, leaf-first (a preview stays, unless the
  /// cascade of a removed input takes it). `RestoreIntent::kHistory`
  /// reconciles only the non-exempt transforms and leaves the `history_exempt`
  /// ones untouched; `kReplace` removes all. `keep_keys` names further transforms
  /// to leave live (a restore keeps those whose recipe the snapshot does not
  /// change, so their output TopicIds — and every curve bound to them — survive).
  /// A kept transform can still fall to the removal cascade of one of its inputs.
  void clearTransforms(RestoreIntent intent, const std::unordered_set<std::string>& keep_keys = {});
  /// Keys of the live transforms a restore leaves unchanged: those within the
  /// intent's authority (every one under `kReplace`; the non-exempt ones under
  /// `kHistory`) whose persisted recipe equals the `wanted` entry with the same
  /// key. Compares exactly the fields a layout file carries — runtime node/topic
  /// ids are irrelevant — so a binding-less legacy `wanted` entry never matches.
  /// Feed the result to `clearTransforms` / `liveNonExemptNodeIds` as `keep_keys`.
  [[nodiscard]] std::unordered_set<std::string> unchangedTransformKeys(
      const std::vector<TransformRecipe>& wanted, RestoreIntent intent) const;
  /// Whether any live transform is `history_exempt` (cheap gate for the
  /// history-restore dependency preflight).
  [[nodiscard]] bool hasHistoryExemptTransforms() const;

  /// The recipe behind a filter output topic, or nullptr if `output_topic_id` is
  /// not a known filter output. Lets the Filter Editor re-open on an existing
  /// filtered curve and restore its parameters.
  [[nodiscard]] const FilterRecipe* filterConfig(TopicId output_topic_id) const;

  /// Replace a live filter's processor IN PLACE (same node + same output topic)
  /// and recompute — the editor's "apply an edit" path, so the plotted curve
  /// updates without re-binding. Updates the stored recipe. Takes a shared
  /// processor so the recipe can keep editing it later.
  [[nodiscard]] Status updateFilter(NodeId node_id, std::shared_ptr<proc::DataProcessor> processor);

  /// Drive the eager engine after a commit and BEFORE retention eviction — the
  /// streaming correctness order (plan §5/D6). Advances all live filter nodes
  /// over the newly committed input so stateful processors (integral, …) consume
  /// every sample before it can be evicted, then returns the filter output topics
  /// for the caller to re-notify (so plots refresh). MUST be called between the
  /// raw commit and `DataEngine::enforceRetention`. Best-effort: never throws
  /// mid-ingest. Returns empty if no filters are registered.
  /// [main-thread] — the streaming worker hops here via the GUI thread (see class
  /// note); it never calls this directly, so reading the recipe maps needs no lock.
  [[nodiscard]] std::vector<TopicId> advanceOnCommit(const std::vector<TopicId>& changed_inputs);

  /// Strict reload path: re-resolve semantic fields in place, preserve node and
  /// output ids, then replay the complete affected graph once.
  [[nodiscard]] Expected<std::vector<TopicId>> rebindAndRecomputeForReplacedSources(
      const std::vector<TopicId>& replaced_inputs);

  /// Snapshot of every live filter recipe (unspecified order) — for layout
  /// persistence: the host serializes these and re-applies them on load.
  [[nodiscard]] std::vector<FilterRecipe> recipes() const;

  /// (id, human-label) for every catalogued filter, in catalogue order — for
  /// populating a menu/dialog without the UI touching the script engine directly.
  /// Empty when no catalogue is installed.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> availableFilters() const;

  /// Install the Luau filter catalogue (set by the app at startup). The by-id
  /// `applyFilter` and `availableFilters` resolve through it; without it the service
  /// has no filters (the bundled resource always installs one in the real app).
  void setFilterCatalogue(std::shared_ptr<scripting::FilterCatalogue> catalogue);

  /// Resolve a persisted layout entry to a processor for restore, in priority
  /// order (never silently mis-binds): the live catalogue by `id` → the layout's
  /// embedded `source_fallback` (compiled through the sandboxed engine, even with
  /// no catalogue installed) → `nullptr` (the caller logs a skip). The embedded-
  /// source leg lets a layout open on a machine without the filter installed.
  [[nodiscard]] std::unique_ptr<proc::DataProcessor> makeRestoredProcessor(
      const std::string& id, const std::string& params_json, const std::string& source_fallback) const;

  // --- Plugin-created transforms (pj.data_processors.v1 host side) ---

  /// Create-or-replace (upsert by `"<plugin_id>/<id>"`) a named, plugin-owned
  /// transform. Initial name resolution is reject-ambiguous and captures a
  /// dataset/source/topic/field binding for every input. Restores resolve that
  /// binding through SessionManager's dataset identity policy. Output topics are
  /// created in the first input's dataset. Shape is
  /// N->M: 1->1 installs a SISO node (`LuaSisoTransform`), anything else a MIMO node
  /// (`LuaMimoTransform` via `addMimoTransform`, which joins inputs on exact
  /// timestamp). A compatible replacement swaps the operator in place and rolls
  /// back root plus dependents on replay failure; shape-changing replacements are
  /// rejected before mutation.
  /// [main-thread]
  ///
  /// `ephemeral` marks a preview node: it installs and runs identically, but is left
  /// out of `transformRecipes()` (never persisted). The caller is responsible for not
  /// surfacing its output in the catalog and for `removeTransform`-ing it when done.
  ///
  /// `history_exempt` marks the installed recipe immune to undo/redo (see
  /// `TransformRecipe::history_exempt`); trailing so every existing positional call
  /// site (which stops at `input_column_index`) keeps compiling unchanged.
  ///
  /// A persistent transform reading an ephemeral one's output is refused: the
  /// preview's removal would cascade into it. Another preview may read it.
  ///
  /// A NEW persistent key replays the committed input history time-sliced: only the
  /// first `kMaterializationSliceBudget` slice runs inside the call (so a script
  /// failing on its first rows is still rejected), the rest from a zero-timer on
  /// the calling thread (see `stepMaterialization`), so the event loop keeps turning
  /// while a large history replays. The recipe and its output topic exist on return
  /// and the output grows in time order until it equals a synchronous replay's. An
  /// ephemeral create and an in-place replacement replay synchronously.
  [[nodiscard]] Expected<TransformRecipe> upsertTransform(
      std::string_view plugin_id, std::string_view id, std::vector<std::string> inputs,
      std::vector<std::string> outputs, std::string_view script, std::string_view params_json, bool ephemeral = false,
      std::size_t input_column_index = 0, bool history_exempt = false);

  /// One tick of the pending time-sliced replays (`IncrementalEngine::deferredNodes`,
  /// oldest first) for about `budget` of wall time; processors chained on a growing
  /// output follow it within the same budget. The new rows are announced through
  /// `SessionManager::notifyIngest` at most every `kMaterializationNotifyInterval`,
  /// and at once when a replay completes or fails. Returns true when none is
  /// pending afterwards. Removing, editing or recomputing a transform ends its
  /// replay by itself. The service's zero-timer calls this; tests may too.
  /// [main-thread]
  bool stepMaterialization(std::chrono::nanoseconds budget = kMaterializationSliceBudget);

  /// Completes now the pending replays whose output — or the output of a processor
  /// chained on it — is `read`, for a reader that must not see a partial series.
  /// Installed as the engine's `DataEngine::awaitCompleteSeries` barrier, which
  /// whole-series readers (toolbox snapshot reads, marker generator inputs) call.
  /// [main-thread]
  void finishPendingMaterialization(TopicId read);

  /// Where transform failures are reported: every transform whose node fails in a
  /// scheduling pass (a streaming commit, a replay tick, a chained processor) is
  /// reported once as an error `Diagnostic` keyed by its transform key, and again
  /// only after it recovered. Without a sink the failure is only logged.
  void setDiagnosticSink(DiagnosticSink sink) {
    diagnostic_sink_ = std::move(sink);
  }

  /// Receives the topic names of processor outputs (transform or filter) whose rows
  /// changed, with the dataset they live in (never an empty list). Called when an
  /// upsert, a filter apply, an in-place edit or a restore leaves an output complete,
  /// when a time-sliced replay ends (not on its partial output), and before a removal
  /// (cascade included); the processors chained on an edited or replayed output are
  /// named too. A reload's rebind does not call it: `RefillGuard::recomputeProcessors`
  /// runs the marker rules itself afterwards. `SessionManager` marks the marker rules
  /// reading the names stale. [main-thread]
  using OutputsChangedSink = std::function<void(const std::vector<std::string>& names, DatasetId dataset)>;
  void setOutputsChangedSink(OutputsChangedSink sink) {
    outputs_changed_sink_ = std::move(sink);
  }

  /// True while a time-sliced replay has input left.
  [[nodiscard]] bool materializationPending() const;

  /// Refuses a script language no transform backend runs: anything but "luau" or "python" (empty means luau). The
  /// one guard of `validateScript` and the plugin bridge's script check.
  [[nodiscard]] static Status checkLanguage(std::string_view language);

  /// Validate a transform script WITHOUT installing anything: compile it with the
  /// backend for `language` ("luau" today; other values are rejected until their
  /// backend lands, e.g. "python") and run one synthetic test point to catch
  /// syntax + trivial runtime errors (a nil/non-numeric return). Returns ok if the
  /// script is well-formed, else the compiler/runtime error message. [main-thread]
  Status validateScript(std::string_view script, std::string_view language, std::string_view params_json);

  /// Transitive closure of named transforms consuming exact removed TopicIds.
  /// Filters participate in traversal so mixed chains remain visible.
  [[nodiscard]] std::vector<TransformRecipe> transformsDependingOn(const std::vector<TopicId>& removed_topics) const;

  /// Output TopicIds of every processor (filter or transform, ephemeral
  /// included — matching what removeProcessorsDependingOn would delete)
  /// transitively depending on any of `input_topics`; parents before children.
  [[nodiscard]] std::vector<TopicId> dependentProcessorOutputs(const std::vector<TopicId>& input_topics) const;

  /// Remove every filter/transform transitively consuming `input_topics`, in
  /// leaf-first order so no recipe remains attached to a retired topic.
  [[nodiscard]] Status removeProcessorsDependingOn(const std::vector<TopicId>& input_topics);

  /// Every live processor node a history restore is about to remove: the filters
  /// whose output TopicId is not in `keep_filter_outputs` plus the non-exempt
  /// transforms whose key is not in `keep_keys`. Feed the result to
  /// `exemptDependentsOf` before clearing anything, so a history restore can
  /// reject up front instead of cascading into an exempt dependent.
  [[nodiscard]] std::vector<NodeId> liveNonExemptNodeIds(
      const std::unordered_set<std::string>& keep_keys = {},
      const std::unordered_set<TopicId>& keep_filter_outputs = {}) const;

  /// What a restore with these keep sets would take down, computed in one
  /// dependency walk from the processors its clears remove: the filters outside
  /// `keep_filter_outputs` and the transforms outside `keep_keys`, sparing the
  /// history-exempt ones only under `RestoreIntent::kHistory` (`kReplace` removes them).
  struct RemovalImpact {
    /// Output names of every history-exempt transform the removal cascades into
    /// (sorted, unique). Non-empty under `kHistory` means the restore must be rejected.
    std::vector<std::string> exempt_transform_names;
    /// Materialized output topic names of every processor removed, directly or by
    /// cascade (a kept filter or transform reading a removed output falls with it,
    /// and is re-created under a new TopicId). A grouped declaration `"t:a,b"`
    /// reports its single topic `"t"`. Marker generators declare dependencies by
    /// series name, so this is the view their preflight and their unchanged check
    /// match against.
    std::vector<std::string> removed_output_names;
  };
  [[nodiscard]] RemovalImpact removalImpact(
      RestoreIntent intent, const std::unordered_set<std::string>& keep_keys,
      const std::unordered_set<TopicId>& keep_filter_outputs) const;

  /// Output names of every history-exempt TRANSFORM transitively depending on any
  /// of `nodes`. Empty means removing `nodes` would strand nothing exempt. The
  /// closure matches exactly what `removeProcessorsDependingOn` would cascade
  /// into. Scoped to filters/transforms (this service's own graph); a marker
  /// generator's dependency on a removed transform output is not tracked here.
  [[nodiscard]] std::vector<std::string> exemptDependentsOf(const std::vector<NodeId>& nodes) const;

  /// Materialized output topic names (sorted, unique) that `removeTransform(transform_key)` would retire: the
  /// transform's own and those of every processor its removal cascades into, the view name-bound readers (marker
  /// generators, on-demand recipes) match against. Empty for an unknown key.
  [[nodiscard]] std::vector<std::string> removedOutputNames(std::string_view transform_key) const;

  /// Remove a transform by its namespaced key, cascading through every exact
  /// downstream processor and retiring all affected outputs. Unknown key = error.
  Status removeTransform(std::string_view namespaced_key);

  /// Remove the EPHEMERAL (preview) transforms among `keys`, cascading like
  /// `removeTransform`; a key that is gone or persistent is skipped. A preview belongs
  /// to the plugin instance that made it: `DerivedRuntimeHosts` passes the ones its
  /// bridge created when the instance ends. Persistent transforms are the user's data.
  void removeEphemerals(const std::unordered_set<std::string>& keys);

  /// `clearTransforms(RestoreIntent::kReplace)` — tear down every persistent
  /// transform at once (reconcile primitive for a full restore).
  void clearAllTransforms();

  /// Tear down every transform whose resolved output lives in `dataset_id`
  /// (dataset removal).
  void clearTransformsForDataset(DatasetId dataset_id);

  /// The per-plugin ids (no prefix) of `plugin_id`'s live transforms — backs the
  /// ABI `list` slot (scoped to the calling plugin).
  [[nodiscard]] std::vector<std::string> transformIdsForPlugin(std::string_view plugin_id) const;

  /// A transform's recipe as
  /// `{"inputs":[…],"outputs":[…],"params":{…},"backend":…,"history_exempt":…,`
  /// `"gaps":[{"from_ns":…,"to_ns":…}]}` for the ABI `config` slot (re-edit/probe).
  /// `gaps` are the retention gaps `IncrementalEngine::gaps` recorded for the
  /// node (oldest first, at most 16): ranges of input evicted before it consumed
  /// it (`from_ns` is null for a node that had fed nothing yet).
  /// `nullopt` if the key is unknown.
  [[nodiscard]] std::optional<std::string> transformRecipeJson(std::string_view namespaced_key) const;

  /// Snapshot of every live transform recipe (unspecified order) — for layout save.
  [[nodiscard]] std::vector<TransformRecipe> transformRecipes() const;

  /// The live transform (a preview excluded, as in `transformRecipes()`) one of whose outputs is `output_topic`;
  /// nullopt when no transform writes it.
  [[nodiscard]] std::optional<TransformRecipe> transformOwning(TopicId output_topic) const;

  /// Resolved SOURCE topics of a displayed Data Processor output: `output_topic_id`
  /// is looked up first as a filter output, then as a transform output, and each
  /// input topic name is resolved back to its live (dataset, name) via the same
  /// name scan `resolveInputTopic` uses. Lets a demand-tracking consumer (see
  /// `PJ::TopicDemandTracker`) keep a displayed derived series' inputs subscribed —
  /// without this a streaming source would unsubscribe the input the moment only
  /// the derived output is on screen, silently stalling the filter. Read-only.
  /// Empty when `output_topic_id` is not a known filter/transform output, or when
  /// an input topic can no longer be resolved by name (e.g. removed upstream).
  [[nodiscard]] std::vector<std::pair<DatasetId, std::string>> sourceTopicsForOutput(TopicId output_topic_id) const;

  /// The materialized output topic ids of every EPHEMERAL transform (the preview
  /// nodes). The catalog excludes these from `rebuildFromDatastore`, so a live
  /// preview never surfaces its prefixed output topics in the Sources tree.
  [[nodiscard]] std::unordered_set<TopicId> ephemeralOutputTopics() const;

  /// Every live materialized processor output, including preview transforms.
  [[nodiscard]] std::unordered_set<TopicId> processorOutputTopics() const;

  /// Replay a persisted transform recipe on session load. Same install path as
  /// `upsertTransform`; an unknown/unavailable backend returns an error WITHOUT
  /// storing it (the caller keeps the persisted XML and surfaces the diagnostic).
  [[nodiscard]] Expected<TransformRecipe> restoreTransform(const TransformRecipe& recipe);

  /// The namespaced upsert key `"<plugin_id>/<id>"` — the single source of truth
  /// for how the ABI bridge maps a plugin's per-id calls onto stored transforms.
  [[nodiscard]] static std::string makeTransformKey(std::string_view plugin_id, std::string_view id);

  /// The topic names `recipe`'s outputs materialize: a grouped declaration `"t:a,b"` is
  /// ONE topic `"t"`, any other output is its own scalar topic under its declared name.
  /// What a series key of a downstream processor or marker rule is rooted at.
  [[nodiscard]] static std::vector<std::string> materializedTopicNames(const TransformRecipe& recipe);

  [[nodiscard]] IncrementalEngine& incrementalEngine() noexcept {
    return *derived_;
  }

 private:
  /// Shared upsert/restore install path (see `upsertTransform`). Fills the runtime
  /// fields of `recipe` and stores it on success.
  /// `time_sliced`: a new node replays only its first slice here (see upsertTransform).
  [[nodiscard]] Expected<TransformRecipe> installTransform(TransformRecipe recipe, bool time_sliced = false);
  /// Starts the zero-timer that drives stepMaterialization() (a no-op without a Qt
  /// application: tests then step by hand).
  void armMaterializationTimer();
  /// `node`'s output topics followed by those of every processor chained on them;
  /// `dependents` (optional) receives the chained processors' node ids, oldest first.
  [[nodiscard]] std::vector<TopicId> outputsWithDependents(
      NodeId node, std::vector<NodeId>* dependents = nullptr) const;
  /// Announces the outputs grown since the last announcement.
  void announceGrownOutputs();
  /// Hands `outputs`' names, grouped by dataset, to the outputs-changed sink.
  void announceChangedOutputs(const std::vector<TopicId>& outputs) const;
  /// Whether one of `inputs` is a preview transform's output. A preview's removal
  /// cascades into what reads it, so a persistent recipe may not.
  [[nodiscard]] bool readsAPreview(const std::vector<TopicId>& inputs) const;
  /// Reports each transform that failed since the last call (see setDiagnosticSink)
  /// and forgets the ones that recovered or were removed. Called around every
  /// scheduling pass, so a recovery by an edit between passes re-arms the report.
  void reportTransformFailures();
  /// Make `DataEngine::awaitCompleteSeries` finish a pending replay of that topic.
  void installCompleteReadBarrier();
  [[nodiscard]] Status removeFilterOnly(NodeId node_id);
  [[nodiscard]] Status removeTransformOnly(std::string_view namespaced_key);
  /// Resolve a topic NAME to (topic id, dataset id) by scanning the engine — there
  /// is no name→id index. `nullopt` if no live topic has that name, or several do
  /// (`*ambiguous` is then set, to tell the two apart).
  [[nodiscard]] std::optional<std::pair<TopicId, DatasetId>> resolveInputTopic(
      const std::string& name, std::optional<DatasetId> only_dataset = std::nullopt, bool* ambiguous = nullptr) const;
  /// An input resolved to a specific leaf column of a topic.
  struct ResolvedInput {
    TopicId topic_id = 0;
    DatasetId dataset_id = 0;
    std::size_t column = 0;  ///< flattened-leaf column index feeding the transform
  };
  /// Resolve an input NAME — either a whole topic ("pose/orientation" → column 0) or
  /// a topic-field path ("pose/orientation/x") — to (topic, dataset, leaf column).
  /// `nullopt` if no live topic matches, or the field is not a leaf of that topic,
  /// or several candidates match (`*ambiguous` is then set, to tell the two apart).
  /// `only_dataset` restricts the scan to one dataset — the dataset-qualified
  /// plugin input path resolves the qualifier first and then looks the bare
  /// name up in there. Topic names are not guaranteed unique even inside one
  /// dataset: a whole-topic name matching twice is ambiguous, and no shorter
  /// "<topic>/<field>" reading is attempted for it.
  [[nodiscard]] std::optional<ResolvedInput> resolveInputField(
      const std::string& name, std::optional<DatasetId> only_dataset = std::nullopt, bool* ambiguous = nullptr) const;
  /// Raw source name of a loaded dataset (the qualifier form), nullopt when unknown.
  [[nodiscard]] std::optional<std::string> datasetSourceName(DatasetId dataset_id) const;
  /// The input spelling the config read-back echoes for one bound input: bare, unless
  /// the bare name is ambiguous across the loaded datasets AND "source:bare" resolves
  /// back to the binding's dataset. A name for which neither spelling round-trips is
  /// unrepresentable and echoes bare.
  [[nodiscard]] std::string readBackInputName(const std::string& bare, const TransformInputBinding& binding) const;
  /// Resolve a saved binding's dataset through SessionManager's shared identity
  /// policy, then resolve its exact topic and semantic leaf.
  [[nodiscard]] Expected<ResolvedInput> resolveInputBinding(const TransformInputBinding& binding) const;

  /// The processors a clear or a history restore removes directly: the filters
  /// outside `keep_filter_outputs` and the persistent transforms outside `keep_keys`
  /// (minus the `history_exempt` ones when `keep_history_exempt`); a preview ends
  /// with its plugin instance, never with a restore. The clears and the
  /// preflight queries all derive from this one set, so they cannot disagree.
  struct RemovalSet {
    std::vector<NodeId> filter_nodes;
    std::vector<std::string> transform_keys;
    std::unordered_set<TopicId> outputs;  ///< every output topic of the above
  };
  [[nodiscard]] RemovalSet removalSet(
      const std::unordered_set<std::string>& keep_keys, const std::unordered_set<TopicId>& keep_filter_outputs,
      bool keep_history_exempt) const;
  /// Grows `affected` through every processor the removal of those topics
  /// cascades into and returns the output names of the history-exempt
  /// transforms among them (sorted, unique).
  [[nodiscard]] std::vector<std::string> exemptTransformsDependingOn(std::unordered_set<TopicId>& affected) const;
  /// Names of the materialized topics among `topics`, sorted and unique (a retired or unknown id is skipped).
  [[nodiscard]] std::vector<std::string> topicNames(const std::unordered_set<TopicId>& topics) const;

  /// Fixpoint walk of every processor transitively depending on a topic in
  /// `affected` (which grows with each visited processor's outputs). Visits
  /// each recipe exactly once, parents before children. When
  /// !include_ephemeral_transforms, ephemeral transform recipes are neither
  /// visited nor propagated through (transformsDependingOn's contract).
  void forEachDependentProcessor(
      std::unordered_set<TopicId>& affected, bool include_ephemeral_transforms,
      const std::function<void(TopicId, const FilterRecipe&)>& on_filter,
      const std::function<void(const std::string&, const TransformRecipe&)>& on_transform) const;
  /// True if `name` is already a live engine topic or another transform's output
  /// (a transform owned by `except_key` is excluded, so an in-place replace is allowed).
  [[nodiscard]] bool outputNameInUse(const std::string& name, const std::string& except_key) const;

  DataEngine& engine_;  ///< engine derived_ writes into; needed to retire orphaned filter outputs on clear
  SessionManager* session_ = nullptr;  ///< production identity resolver; null only in engine-level tests
  std::unique_ptr<IncrementalEngine> derived_;
  std::unordered_map<TopicId, FilterRecipe> recipes_;                   ///< output topic id -> live recipe
  std::unordered_map<std::string, TransformRecipe> transform_recipes_;  ///< "<plugin>/<id>" -> live transform
  std::shared_ptr<scripting::FilterCatalogue> filter_catalogue_;        ///< optional Luau filter source
  DiagnosticSink diagnostic_sink_;                                      ///< receives transform failures; may be empty
  OutputsChangedSink outputs_changed_sink_;                             ///< may be empty
  std::unordered_set<std::string> reported_failures_;                   ///< keys of failed transforms already reported
  std::unordered_set<TopicId> unannounced_outputs_;                     ///< replay outputs grown since the last notify
  std::chrono::steady_clock::time_point last_announcement_{};
  /// Zero-interval timer driving stepMaterialization(); created on first use. Declared
  /// after derived_ so it dies first and can never fire into a destroyed engine.
  std::unique_ptr<QTimer> materialize_timer_;
};

}  // namespace PJ
