#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"  // sdk::PlotMarkers — stored by value in parts_
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/merge_result.hpp"  // DatasetMergeSource
#include "pj_runtime/HistoryScope.h"
#include "pj_runtime/MarkerTopics.h"  // MarkerScope

namespace PJ {

class ObjectStore;
struct ObjectTopicId;  // pj_datastore/object_store.hpp; only used by-value in private decls below
struct ResolvedObjectEntry;

/// Output shape of a data processor (the `kind` discriminator of `pj.data_processors.v1`).
/// Only "markers" (objects → ObjectStore) is handled by THIS engine. "transform"
/// (per-sample timeseries → DerivedEngine) is routed to the DerivedEngine bridge owned
/// by the transform-editor work, not here.
enum class GeneratorKind {
  kMarkers,  ///< emits a discrete PlotMarkers set into the ObjectStore
};

/// Host service that runs plugin-submitted WHOLE-SERIES *object generators* — the
/// kind="markers" path of the unified `pj.data_processors.v1` SDK service — and
/// publishes their `PlotMarkers` into the `ObjectStore` (`__markers__/…`).
///
/// Engine boundary (invariant): this is the OBJECT engine — it writes ONLY to the
/// `ObjectStore`, never the `DataEngine`. Timeseries outputs come from transform
/// generators (`kind=transform`) routed to the `DerivedEngine`, NOT from here.
///
/// Host-driven model: a generator is pure DATA — a Luau script + input series keys +
/// output topic + params. Nothing executable crosses any boundary; THIS service owns
/// execution and re-runs on data change, so output survives plugin unload and
/// recomputes live. Scripts run through `pj_scripting`'s `runMarkerScript` — the SAME
/// engine the headless `anomaly_runner` links — so a generator yields identical
/// output in the GUI and in CI ("GUI == headless"). Whole-series, so there is NO
/// incremental path: any change to an input re-runs the whole script and
/// republishes the marker set whole.
///
/// Series resolution is INJECTED (`SeriesResolver`), not wired to the catalog, so the
/// service is unit-testable against synthetic data and the catalog coupling stays in
/// the wiring layer. The public header stays free of the `pj_scripting` dependency
/// (linked PRIVATE) — hence the local `ResolvedSeries` rather than the engine's view.
///
/// Invariants:
/// 1. Every marker write goes through `setPart`/`dropParts` → `republishUnion`: a
///    (dataset, marker topic) target's blob is always the union of every
///    generator's last PART for it, so two rules on one target never clobber each
///    other and retiring one drops only its own part.
/// 2. A blob with no `parts_` entry is FOREIGN (a direct toolbox write, a pre-merge
///    anchor set): it is adopted under `kForeignOwner` before the first union
///    publish or merge touches that target, never silently dropped.
/// 3. `parts_` never names a dataset the ObjectStore no longer holds.
/// 4. `recipes_` is keyed by id — the plugin ABI's identity — holding that id's
///    per-dataset BINDINGS (add / replace / reject rules: `upsertGenerator`); an
///    `all_datasets` or `ephemeral` id has exactly one, and no id holds zero.
/// 5. Scope says where markers are DRAWN, not where the script runs. A dataset-bound
///    recipe reads and draws on its one dataset. An `all_datasets` recipe reads from
///    every loaded dataset that carries its inputs, and the union of those runs is one
///    session-wide set drawn on EVERY loaded dataset — those without the inputs and
///    those loaded later included (`reachNewDatasets`). That set is kept ONCE in the
///    DISPLAY frame (`shared_sets_`, raw − the contributor's display offset) and each
///    dataset's part is that set shifted into the dataset's own frame, so a marker
///    sits at the same display instant on every plot no matter how the datasets are
///    aligned; `rebaseForDisplayOffset` re-derives a dataset's part when its offset
///    moves, `clearGeneratorsForDataset` rebuilds the set when a contributor goes.
class MarkerService {
 public:
  /// Samples of one resolved input series: timestamps in nanoseconds + values.
  struct ResolvedSeries {
    std::vector<double> timestamps;
    std::vector<double> values;
  };

  /// Resolve one input series key (e.g. "/imu/accel/x") within a dataset to its
  /// samples, or `nullopt` if the key is unknown. Called once per input per run.
  using SeriesResolver = std::function<std::optional<ResolvedSeries>(DatasetId, const std::string&)>;

  /// List every loaded dataset id — for `all_datasets` generators. Injected by the shell.
  using DatasetLister = std::function<std::vector<DatasetId>()>;

  /// Scope sentinel: "not attributable to one dataset", i.e. reach every loaded one.
  static constexpr DatasetId kAnyDataset = 0;

  /// A submitted generator: pure data, persisted in the layout (unless `ephemeral`).
  struct GeneratorRecipe {
    std::string id;  ///< plugin-namespaced stable key (upsert key)
    GeneratorKind kind = GeneratorKind::kMarkers;
    std::string language = "luau";  ///< script backend; only "luau" accepted today
    DatasetId dataset_id = 0;       ///< dataset the output lives in (when !all_datasets)
    /// Series keys the resolver reads, bare: a bound generator's dataset qualifier is
    /// stripped by the host, and a scope=all generator's keys are literal (the host
    /// refuses a qualifier there).
    std::vector<std::string> inputs;
    /// The names the plugin declared, parallel to `inputs` (same length, or empty when
    /// no name changed): the script enumerates and reads each series under this
    /// spelling, e.g. `series("src:key")`, and the bare key stays readable too.
    std::vector<std::string> declared_inputs;
    std::vector<std::string> outputs;  ///< markers: [marker_topic]; may be empty for an ephemeral preview
    std::string script;                ///< rule source (binary-safe blob)
    std::string params_json;           ///< forwarded verbatim to the engine
    bool all_datasets = false;         ///< publish to EVERY dataset (markers only)
    bool ephemeral = false;            ///< preview: excluded from recipes(); dropped on remove
    /// Persisted in layout files but outside undo/redo authority. A layout
    /// replacement or explicit removal may still replace it.
    bool history_exempt = false;
  };

  /// `object_store` and the data the `resolver` reads from must outlive this service.
  MarkerService(ObjectStore& object_store, SeriesResolver resolver);

  /// Replace the series resolver (the shell injects the catalog-backed resolver
  /// after construction, since `SessionManager` builds this before the catalog exists).
  void setResolver(SeriesResolver resolver);

  /// Set the dataset lister used by `all_datasets` generators (defaults to empty).
  void setDatasetLister(DatasetLister lister);

  /// A dataset's display offset in ns (`display = raw − offset`: the alignment shift
  /// the Source Timeline edits). Keeps an `all_datasets` set at the same display
  /// instant on every dataset (invariant 5). Defaults to 0 for every dataset.
  using DisplayOffsetResolver = std::function<Timestamp(DatasetId)>;
  void setDisplayOffsetResolver(DisplayOffsetResolver resolver);

  /// `dataset`'s display offset moved: republish every `all_datasets` set on it,
  /// shifted into its new frame (no script runs). Returns the republished object
  /// topics — empty when no all-datasets generator exists.
  [[nodiscard]] std::vector<std::string> rebaseForDisplayOffset(DatasetId dataset);

  /// Publish every `all_datasets` recipe's shared set onto each loaded dataset that
  /// does not hold its part yet (no script runs) and return the republished object
  /// topics. The hook for a dataset that just became listed.
  [[nodiscard]] std::vector<std::string> reachNewDatasets();

  /// Every loaded dataset as the injected lister reports it (empty without one).
  [[nodiscard]] std::vector<DatasetId> loadedDatasets() const;

  /// Compile-only validation for `kind`/`language` (drives the editor red/green dot):
  /// rejects a non-"luau" language, else compiles + module-loads the script WITHOUT
  /// running it (no inputs, no side effects). Runtime errors are NOT caught here.
  [[nodiscard]] Status validateScript(GeneratorKind kind, std::string_view language, const std::string& script) const;

  /// Create or replace (by `id`) a generator and run it immediately. Returns the
  /// resolved physical output topic name(s) (the marker object topic). On a script
  /// error returns the message and leaves any prior output AND the prior recipe
  /// untouched (a bad upsert never destroys a working generator). `ephemeral`
  /// recipes are excluded from `recipes()`. A `declared_inputs` list whose length
  /// differs from `inputs` is rejected.
  ///
  /// An id's `ephemeral` and `history_exempt` flags are homogeneous across its
  /// bindings: a binding that would disagree with a surviving sibling is rejected
  /// (naming the id) before anything runs. A recipe whose `all_datasets` differs
  /// from the id's current bindings REPLACES the whole id — every old binding and
  /// its output is retired — so no id ever owns both a Dataset-scope and a Global
  /// output.
  [[nodiscard]] Expected<std::vector<std::string>> upsertGenerator(GeneratorRecipe recipe);

  /// Retire the rule on every dataset it is bound to (persistent or ephemeral; see
  /// invariant 4). For an ephemeral preview the preview object topic is removed.
  /// Unknown `id` is an error.
  Status removeGenerator(std::string_view id);

  /// Re-run every generator whose input is affected by a change, re-publishing its
  /// output — the whole-series recompute hook. An input is affected when one of its
  /// series keys STARTS WITH a `changed` entry; an EMPTY `changed` re-runs ALL.
  /// Returns the affected marker object topic names so the caller can re-notify
  /// overlays; generators whose script errors on re-run are skipped.
  ///
  /// `scope` bounds the work to the dataset that actually ingested; `kAnyDataset`
  /// means the change cannot be attributed to one. It matters because `changed`
  /// carries bare topic NAMES, which repeat across datasets: unscoped, a stream tick
  /// on one dataset re-runs generators bound to another, and re-runs every
  /// `all_datasets` generator over EVERY loaded dataset — including a large file that
  /// did not change and has no retention bounding it. There is deliberately no default:
  /// the unbounded branch should be chosen, not inherited.
  [[nodiscard]] std::vector<std::string> recomputeForChangedInputs(
      const std::vector<std::string>& changed, DatasetId scope);

  /// Re-run every generator bound to `dataset` (plus every `all_datasets` generator,
  /// on `dataset` only), re-publishing its output — the whole-dataset recompute a
  /// reload/replace needs, where matching by changed-input name misses generators
  /// whose inputs vanished or were renamed by the swap. Returns the affected marker
  /// object-topic names.
  [[nodiscard]] std::vector<std::string> recomputeForDataset(DatasetId dataset);

  /// Set-aware merge of the marker sets when datasets fold into `anchor`. Markers
  /// opt out of the generic ObjectStore fold (single-entry supersede at a sentinel
  /// timestamp — the generic interleave+retention fold keeps only one set); this
  /// concatenates, per marker object-topic name, the anchor's set plus each
  /// source's set shifted by its `raw_shift_ns` onto the anchor clock, republishes
  /// one blob per name to the anchor, and drops the source marker topics. Preserves
  /// every dataset's findings (no re-evaluation). MUST run while the source datasets
  /// still hold their marker topics (i.e. before they are dropped). Returns the
  /// affected marker object-topic names.
  std::vector<std::string> mergeMarkerTopics(DatasetId anchor, const std::vector<DatasetMergeSource>& sources);

  /// True when at least one generator exists (including ephemeral previews, which
  /// should also recompute live) — lets a hot commit path skip the machinery cheaply.
  [[nodiscard]] bool hasGenerators() const noexcept {
    return !recipes_.empty();
  }

  /// Snapshot of every PERSISTENT generator recipe (one per binding, so an id bound
  /// to several datasets appears once per dataset) — for layout persistence.
  /// EPHEMERAL previews are excluded.
  [[nodiscard]] std::vector<GeneratorRecipe> recipes() const;

  /// Every PERSISTENT generator id, each once regardless of how many datasets it
  /// is bound to — the plugin ABI's `list()` vocabulary.
  [[nodiscard]] std::vector<std::string> generatorIds() const;

  /// `id`'s first-applied binding (the one the plugin ABI's `config()` reports; a
  /// replacement on that dataset keeps the slot), or null for an unknown id.
  [[nodiscard]] const GeneratorRecipe* firstBinding(std::string_view id) const;

  /// Output names of persistent history-exempt generators that read any series
  /// rooted at one of `removed_inputs`. A generator input matches a removed topic
  /// via `seriesKeyBelongsToTopic`: the input IS the topic, or a field path beneath
  /// it at a slash boundary — deliberately stricter than `recomputeForChangedInputs`'s
  /// raw prefix match (`in.rfind(prefix, 0) == 0`), which also matches a differently
  /// named sibling topic (e.g. `imu_raw/x` for prefix `imu`). Only the slash-bounded
  /// match is a real dependency, so over-matching here would reject a restore that
  /// does not actually strand anything.
  [[nodiscard]] std::vector<std::string> exemptDependentsOf(const std::vector<std::string>& removed_inputs) const;

  /// Remove every PERSISTENT generator (tombstoning its output); ephemeral
  /// previews are always left untouched. `RestoreIntent::kHistory` reconciles
  /// only the non-exempt generators and leaves the `history_exempt` ones live;
  /// `kReplace` removes all.
  void clearGenerators(RestoreIntent intent);
  /// Whether any live generator is `history_exempt` (cheap gate for the
  /// history-restore dependency preflight).
  [[nodiscard]] bool hasHistoryExemptGenerators() const;

  /// `clearGenerators(RestoreIntent::kReplace)` — every PERSISTENT
  /// generator at once; ephemeral previews are left untouched. The clear half of
  /// the clear-all + replay a layout restore runs (mirrors
  /// DataProcessorService::clearAllTransforms).
  void clearAllGenerators();

  /// Rebind every generator bound to one of the `consumed` datasets onto `anchor`.
  /// A merge folds the sources' data into the anchor and drops them from the catalog,
  /// so a recipe still naming a source would resolve zero inputs from then on — the
  /// user's rule stops producing without any error. `all_datasets` generators are
  /// untouched (they resolve through the dataset lister, not a stored id). When the
  /// anchor already holds its own binding of the same id, that binding wins and the
  /// consumed one is retired: `mergeMarkerTopics` (which runs first) already folded
  /// the consumed output onto the anchor under this id, and the anchor's next
  /// recompute replaces it with a run over the merged data.
  void remapGeneratorsToAnchor(DatasetId anchor, const std::vector<DatasetId>& consumed);

  /// Forget every generator bound to `dataset`; `all_datasets` generators are kept
  /// (they belong to the session, not to one dataset) but their shared sets are
  /// rebuilt from the surviving contributors — with none left, the rule's parts are
  /// cleared everywhere and the rule stays live for a later contributor. The markers
  /// twin of `DataProcessorService::clearTransformsForDataset`. Call when a dataset
  /// is removed: its object topics go with it, but a surviving recipe still names the
  /// dead id, and the next recompute would resolve nothing and re-register a phantom
  /// marker topic under a dataset that no longer exists (publishMarkerSet registers
  /// on a miss). The removed dataset's outputs are not tombstoned — its topics are
  /// already gone. Returns the object topics republished on the survivors.
  std::vector<std::string> clearGeneratorsForDataset(DatasetId dataset);

 private:
  /// The bindings of one id, in the order they were applied (a replacement keeps
  /// its slot). Never empty while the id is in `recipes_`.
  using GeneratorBindings = std::vector<GeneratorRecipe>;

  /// The dataset a binding is keyed on. A producer's id carries the rule's SCOPE
  /// ("rule/__global__", "rule/__all__", …) but not the dataset it targets, so
  /// applying the same dataset-bound id on a second dataset is "the same rule
  /// elsewhere" — a new binding — while re-applying it on the SAME dataset must
  /// replace. An `all_datasets` or `ephemeral` recipe is pinned to `kAnyDataset`
  /// so it stays one-per-id, since it is not bound to any one dataset.
  [[nodiscard]] static DatasetId bindingDataset(const GeneratorRecipe& recipe);

  /// The stored binding `recipe` would replace: same id, same `bindingDataset`.
  /// Null when there is none.
  [[nodiscard]] GeneratorRecipe* findBinding(const GeneratorRecipe& recipe);

  /// Every binding of every id, flattened (id order, then application order).
  [[nodiscard]] std::vector<std::reference_wrapper<const GeneratorRecipe>> allRecipes() const;

  /// One place a recipe publishes to: the dataset and the object-topic name.
  using PublishTarget = std::pair<DatasetId, std::string>;

  /// The datasets a recipe draws on: its own when bound, every loaded one when
  /// `all_datasets`. The single enumerator behind both publishing and retiring —
  /// deriving "where does this land" twice is how a rename strands a topic. The
  /// `loaded` overload takes an already-fetched `loadedDatasets()` so a loop over
  /// many recipes queries the lister once.
  [[nodiscard]] std::vector<DatasetId> targetDatasets(const GeneratorRecipe& recipe) const;
  [[nodiscard]] static std::vector<DatasetId> targetDatasets(
      const GeneratorRecipe& recipe, const std::vector<DatasetId>& loaded);

  /// After a successful re-run of `recipe`: drop every target of its previous
  /// revision that NEITHER the new revision NOR a sibling binding of the same id
  /// still covers, so a retargeting upsert (renamed output, rebound dataset,
  /// all-datasets toggled off) strands no topic.
  void dropStaleTargets(const GeneratorRecipe& recipe);

  /// Every (dataset, object-topic) a recipe publishes to right now — one entry for a
  /// per-dataset recipe, one per loaded dataset for an `all_datasets` one.
  [[nodiscard]] std::vector<PublishTarget> publishTargets(const GeneratorRecipe& recipe) const;

  /// Parts of one target, keyed by owner (generator id) — std::map so the union
  /// order is deterministic (by owner id).
  using PartsByOwner = std::map<std::string, sdk::PlotMarkers>;

  /// (dataset, object topic) -> owner -> that owner's last published part. The
  /// ObjectStore blob for a target is always the ordered concatenation of its
  /// parts (see `republishUnion`) — this service never writes it any other way.
  std::map<PublishTarget, PartsByOwner> parts_;

  /// Owner id reserved for a store blob no generator owns: a direct toolbox write,
  /// or a pre-existing set adopted on first union publish / merge. Cannot collide
  /// with a recipe id: those are `<plugin>/<local id>` (`MarkersRuntimeHost::makeKey`),
  /// so every one of them contains a '/'.
  static constexpr std::string_view kForeignOwner = "foreign";

  /// Set `owner`'s part of `target` to `part` and republish the union. If `target`
  /// has no tracked parts yet, adopts any pre-existing store blob as foreign first
  /// (see `adoptForeignBlob`) so it is not overwritten unseen.
  [[nodiscard]] Status setPart(const PublishTarget& target, std::string_view owner, sdk::PlotMarkers part);

  /// Drop `owner`'s part from every `parts_` entry except those in `keep` (its
  /// still-live targets). A touched target left with no owners is tombstoned
  /// (persistent) or removed outright (`ephemeral`) and erased from `parts_`;
  /// otherwise its union is republished.
  void dropParts(std::string_view owner, const std::vector<PublishTarget>& keep, bool ephemeral);

  /// Adopt the store's current blob at `target` into `owners` (a fresh, empty
  /// `parts_` entry) under `kForeignOwner`. No-op when the store has nothing
  /// published there, the blob is empty, or it fails to decode.
  void adoptForeignBlob(const PublishTarget& target, PartsByOwner& owners) const;

  /// The store's currently published entry at `topic_id` (nullopt when never
  /// published). Its payload decodes via `decodeStoredMarkers`; its
  /// `payload_stamp_shift` is what a merge bakes into an already-tracked part.
  [[nodiscard]] std::optional<ResolvedObjectEntry> latestMarkerEntry(ObjectTopicId topic_id) const;

  /// Decode `entry`'s blob. Returns `nullopt` when there is nothing worth adopting
  /// — no entry, an empty/tombstoned set, or undecodable bytes — so every call
  /// site gets one decode-then-bail instead of hand-rolling the same guard.
  [[nodiscard]] static std::optional<sdk::PlotMarkers> decodeStoredMarkers(
      const std::optional<ResolvedObjectEntry>& entry);

  /// Concatenate `parts_[target]`'s parts (map order = owner id) into one
  /// `sdk::PlotMarkers` and publish it — the union's single write to the
  /// ObjectStore (besides the tombstone `dropParts` publishes directly). A
  /// single-owner target publishes that part as-is, without building a union.
  [[nodiscard]] Status republishUnion(const PublishTarget& target);
  [[nodiscard]] Status republishUnion(const PublishTarget& target, const PartsByOwner& owners);

  /// Run `recipe` and route its output by kind. Returns the resolved output topic(s).
  [[nodiscard]] Expected<std::vector<std::string>> runAndPublish(const GeneratorRecipe& recipe);

  /// kind=markers: run the script and publish PlotMarkers to the object topic(s) —
  /// on the one dataset when bound; when `all_datasets`, the union of every
  /// contributing dataset's run is published on every loaded dataset (invariant 5).
  [[nodiscard]] Expected<std::vector<std::string>> runMarkers(const GeneratorRecipe& recipe);

  /// The display-frame union of every contributing dataset's run of an
  /// `all_datasets` `recipe` — nothing is published. `nullopt` means no loaded
  /// dataset carries its inputs.
  [[nodiscard]] Expected<std::optional<sdk::PlotMarkers>> computeShared(const GeneratorRecipe& recipe);

  /// Run `recipe`'s script over `dataset_id`'s series and return the markers it
  /// produced — nothing is published. `nullopt` means the dataset is not one this
  /// rule reads from: an `all_datasets` recipe whose inputs ALL fail to resolve
  /// there. A dataset-bound recipe never skips — a missing input on its own dataset
  /// is the user's mistake and surfaces as a script error.
  [[nodiscard]] Expected<std::optional<sdk::PlotMarkers>> evaluateMarkers(
      const GeneratorRecipe& recipe, DatasetId dataset_id);

  /// `runAndPublish(recipe)` and append the topics it resolved to `affected`; a
  /// failed run contributes nothing (the recompute paths never surface errors).
  void runAndAccumulate(const GeneratorRecipe& recipe, std::vector<std::string>& affected);

  /// Publish an `all_datasets` recipe's shared (display-frame) set onto every loaded
  /// dataset that does not hold its part yet — a dataset loaded after the rule was
  /// applied never appears in a `changed` batch whose prefixes the recipe's inputs
  /// match, so the recompute prefix test alone can never reach it. No script runs;
  /// only when the recipe has no shared set yet is it run again.
  void reachUnpublishedDatasets(
      const GeneratorRecipe& recipe, const std::vector<DatasetId>& loaded, std::vector<std::string>& affected);

  /// Publish `shared` as `recipe`'s part on every loaded dataset and cache it in
  /// `shared_sets_`. Fails on the first dataset that refuses the publish.
  [[nodiscard]] Status publishSharedEverywhere(const GeneratorRecipe& recipe, sdk::PlotMarkers shared);

  /// Set `recipe`'s part on `dataset` to `shared` shifted into that dataset's frame
  /// (`+ displayOffsetOf(dataset)`) and republish the union there.
  [[nodiscard]] Status publishShared(const GeneratorRecipe& recipe, const sdk::PlotMarkers& shared, DatasetId dataset);

  [[nodiscard]] Timestamp displayOffsetOf(DatasetId dataset) const;

  /// Find-or-register `object_topic_name` on `dataset_id` (capping retention at one
  /// snapshot — markers are republished whole, last-writer-wins at a sentinel
  /// timestamp) and publish `set`. The single publish path for run + merge.
  [[nodiscard]] Status publishMarkerSet(
      DatasetId dataset_id, const std::string& object_topic_name, const sdk::PlotMarkers& set);

  /// Republish an EMPTY set to `object_topic_name` on `dataset_id` (the removal
  /// tombstone): the topic survives with its id + retention budget, but draws
  /// nothing. No-op if the topic was never published (never registers a new topic
  /// on remove). Pairs with the codec's empty-buffer→empty-set decode.
  void publishEmptyMarkers(DatasetId dataset_id, const std::string& object_topic_name);

  /// `mergeMarkerTopics` phase 1: fold `anchor`'s OWN pre-merge marker sets into
  /// `parts_`, baking in any pending payload shift, and record every touched topic
  /// name in `touched`. MUST run before `foldSourcePartsOntoAnchor`: doing this first
  /// means phase 2 never finds an anchor target still untracked when it moves a
  /// source part onto it, so that part is never shifted twice and an untracked
  /// anchor blob is never lost underneath it.
  void foldAnchorPartsBeforeMerge(DatasetId anchor, std::set<std::string>& touched);

  /// `mergeMarkerTopics` phase 2: fold `source`'s marker sets onto `anchor`'s clock
  /// (shifted by `source.raw_shift_ns` plus any pending payload shift), drop
  /// `source`'s marker object topics, and record every touched anchor topic name in
  /// `touched`. Must run after `foldAnchorPartsBeforeMerge` (see its doc-comment).
  void foldSourcePartsOntoAnchor(DatasetId anchor, const DatasetMergeSource& source, std::set<std::string>& touched);

  ObjectStore& object_store_;
  SeriesResolver resolver_;
  DatasetLister dataset_lister_;
  DisplayOffsetResolver display_offset_resolver_;
  /// Display-frame union of each `all_datasets` recipe's contributors — the one copy
  /// every dataset's part is derived from (invariant 5). Keyed by id (an
  /// `all_datasets` recipe has one binding).
  std::map<std::string, sdk::PlotMarkers> shared_sets_;
  std::map<std::string, GeneratorBindings> recipes_;  ///< id → per-dataset bindings (incl. ephemeral); invariant 4
};

}  // namespace PJ
