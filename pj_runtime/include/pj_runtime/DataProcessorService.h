#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

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

#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_runtime/HistoryScope.h"

namespace PJ {

class DataEngine;
class DerivedEngine;
class SessionManager;
namespace proc {
class DataProcessor;
}
namespace scripting {
class FilterCatalogue;
}

/// Host service that applies "Data Processors" (filters / transforms) to
/// datastore topics by running them as EAGER `pj_datastore::DerivedEngine` nodes
/// (a `PJ::proc::ProcessorSisoAdapter` wraps each native processor). Owns one
/// `DerivedEngine` bound to the session's `DataEngine`.
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
  /// distinct from a per-curve `FilterRecipe`. It shares the eager `DerivedEngine`
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
    /// Ephemeral (preview) node: a real `DerivedEngine` node like any other, but
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
  /// empty output / column out of range).
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

  /// Remove every live filter: for each one, drop its engine node AND
  /// `retireTopic` its materialized output (so `CatalogModel::rebuildFromDatastore`
  /// stops listing it — `removeNode` alone leaves a catalog zombie), then forget
  /// its recipe. The reconcile primitive behind snapshot restore (undo/redo +
  /// layout load): clear, then re-apply the snapshot's set, so a restored state
  /// can never duplicate or leak a filter. Removed leaf-first (a filter's own
  /// dependents are cascaded via `removeFilter` before the filter itself). The
  /// output `TopicStorage` is kept alive (only retired), so any cached reader
  /// pointer sees an empty deque, not freed memory. Idempotent; a no-op when
  /// nothing is live. A `FilterRecipe` carries no exemption bit, so there is no
  /// predicate form — a filter is always in scope.
  void clearAllFilters();

  /// Remove every live transform, leaf-first. `RestoreIntent::kHistory`
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
  [[nodiscard]] Expected<TransformRecipe> upsertTransform(
      std::string_view plugin_id, std::string_view id, std::vector<std::string> inputs,
      std::vector<std::string> outputs, std::string_view script, std::string_view params_json, bool ephemeral = false,
      std::size_t input_column_index = 0, bool history_exempt = false);

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

  /// Every live processor node a history restore is about to remove: every filter
  /// (never exempt) plus every transform whose `history_exempt` bit is unset and
  /// whose key is not in `keep_keys` — the same exclusions `clearTransforms` takes,
  /// so the preflight and the clear agree on one removal set. Feed the result to
  /// `exemptDependentsOf` before clearing anything, so a history restore can reject
  /// up front instead of cascading into an exempt dependent.
  [[nodiscard]] std::vector<NodeId> liveNonExemptNodeIds(const std::unordered_set<std::string>& keep_keys = {}) const;

  /// Materialized output topic names produced by `liveNonExemptNodeIds(keep_keys)`
  /// (a grouped declaration `"t:a,b"` reports its single topic `"t"`). Marker
  /// generators declare dependencies by series name rather than by datastore node
  /// id, so a history restore uses this view to check their dependencies before
  /// clearing any processor.
  [[nodiscard]] std::vector<std::string> liveNonExemptOutputNames(
      const std::unordered_set<std::string>& keep_keys = {}) const;

  /// Output names of every history-exempt TRANSFORM transitively depending on any
  /// of `nodes` (typically `liveNonExemptNodeIds(...)` — the set a history restore is
  /// about to remove). Empty means removing `nodes` would strand nothing exempt.
  /// Walks `forEachDependentProcessor` with `include_ephemeral_transforms=true`
  /// from `nodes`' output topics, so the closure matches exactly what
  /// `removeProcessorsDependingOn` would cascade into. Scoped to
  /// filters/transforms (this service's own graph); a marker generator's
  /// dependency on a removed transform output is not tracked here.
  [[nodiscard]] std::vector<std::string> exemptDependentsOf(const std::vector<NodeId>& nodes) const;

  /// Remove a transform by its namespaced key, cascading through every exact
  /// downstream processor and retiring all affected outputs. Unknown key = error.
  Status removeTransform(std::string_view namespaced_key);

  /// Tear down every transform owned by `plugin_id` (plugin uninstall). Does NOT
  /// run on ordinary DSO unload — the host owns the script, so a transform
  /// survives unload (the bridge object dies, the node lives on).
  void clearTransformsForPlugin(std::string_view plugin_id);

  /// `clearTransforms(RestoreIntent::kReplace)` — tear down every
  /// transform at once (session close / reconcile primitive for a full restore).
  void clearAllTransforms();

  /// Tear down every transform whose resolved output lives in `dataset_id`
  /// (dataset removal).
  void clearTransformsForDataset(DatasetId dataset_id);

  /// The per-plugin ids (no prefix) of `plugin_id`'s live transforms — backs the
  /// ABI `list` slot (scoped to the calling plugin).
  [[nodiscard]] std::vector<std::string> transformIdsForPlugin(std::string_view plugin_id) const;

  /// A transform's recipe as
  /// `{"inputs":[…],"outputs":[…],"params":{…},"backend":…,"history_exempt":…}`
  /// for the ABI `config` slot (re-edit/probe). `nullopt` if the key is unknown.
  [[nodiscard]] std::optional<std::string> transformRecipeJson(std::string_view namespaced_key) const;

  /// Snapshot of every live transform recipe (unspecified order) — for layout save.
  [[nodiscard]] std::vector<TransformRecipe> transformRecipes() const;

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

  [[nodiscard]] DerivedEngine& derivedEngine() noexcept {
    return *derived_;
  }

 private:
  /// Shared upsert/restore install path (see `upsertTransform`). Fills the runtime
  /// fields of `recipe` and stores it on success.
  [[nodiscard]] Expected<TransformRecipe> installTransform(TransformRecipe recipe);
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
  std::unique_ptr<DerivedEngine> derived_;
  std::unordered_map<TopicId, FilterRecipe> recipes_;                   ///< output topic id -> live recipe
  std::unordered_map<std::string, TransformRecipe> transform_recipes_;  ///< "<plugin>/<id>" -> live transform
  std::shared_ptr<scripting::FilterCatalogue> filter_catalogue_;        ///< optional Luau filter source
};

}  // namespace PJ
