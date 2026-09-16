#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"  // sdk::PlotMarkers
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/merge_result.hpp"  // DatasetMergeSource
#include "pj_runtime/HistoryScope.h"

namespace PJ {

class ObjectStore;
struct ObjectTopicId;  // pj_datastore/object_store.hpp; by value only in a private declaration below

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
/// execution and re-runs on data change, so output survives plugin unload. Scripts
/// run through `pj_scripting`'s `runMarkerScript` — the SAME engine the headless
/// `anomaly_runner` links, so GUI == headless. Whole-series: any input change
/// re-runs the whole script and republishes the set whole. Series resolution is
/// INJECTED (`SeriesResolver`) so the service is testable against synthetic data;
/// the header stays free of `pj_scripting` (linked PRIVATE), hence `ResolvedSeries`.
///
/// Invariants:
/// 1. Every generator writes its OWN object topic, `__markers__/<key>#<id>`
///    (`markerOwnerTopicName`; `#` is refused in ids and keys): two rules on one key
///    never clobber each other, and retiring one tombstones only the topics ending
///    in its `#<id>` — found by scanning the store, so an output a merge moved onto
///    the anchor is retired too. The overlay draws the union of a key's family, so
///    the store is the ONLY copy — merge, reload and removal are the store's
///    per-topic operations — and a direct toolbox write to the bare topic is just
///    another family member this service never reads or rewrites.
/// 2. `recipes_` is keyed by id — the plugin ABI's identity — holding that id's
///    per-dataset BINDINGS (add / replace / reject rules: `upsertGenerator`); an
///    `all_datasets` or `ephemeral` id has exactly one, and no id holds zero.
/// 3. Scope says where markers are DRAWN, not where the script runs. A dataset-bound
///    recipe reads and draws on its one dataset. An `all_datasets` recipe reads from
///    every loaded dataset that carries its inputs, and the union of those runs is
///    ONE session-wide set published once on `kAllDatasetsMarkerDataset`, in the
///    DISPLAY frame (raw − the contributor's alignment offset): the overlay draws it
///    on every plot at the same display instant, so a Source Timeline drag costs no
///    republish. `reachNewDatasets` re-runs it when the dataset roster grows and
///    `clearGeneratorsForDataset` when a contributor goes.
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

  /// A dataset's alignment offset in ns (`display = raw − offset`: the shift the
  /// Source Timeline edits). Puts each contributor's run of an `all_datasets` rule
  /// into the display frame (invariant 3). Defaults to 0 for every dataset.
  using DisplayOffsetResolver = std::function<Timestamp(DatasetId)>;
  void setDisplayOffsetResolver(DisplayOffsetResolver resolver);

  /// The catalog grew (a dataset or a topic was published): re-run every
  /// `all_datasets` recipe, since the newcomer may contribute — the commit-time
  /// recompute runs before the catalog rebuild, so it cannot see it. Returns the
  /// republished object topics.
  [[nodiscard]] std::vector<std::string> reachNewDatasets();

  /// Every loaded dataset as the injected lister reports it (empty without one).
  [[nodiscard]] std::vector<DatasetId> loadedDatasets() const;

  /// Compile-only validation for `kind`/`language` (drives the editor red/green dot):
  /// rejects a non-"luau" language, else compiles + module-loads the script WITHOUT
  /// running it (no inputs, no side effects). Runtime errors are NOT caught here.
  [[nodiscard]] Status validateScript(GeneratorKind kind, std::string_view language, const std::string& script) const;

  /// Create or replace (by `id`) a generator and run it immediately. Returns the
  /// generator's own object topic (`markerOwnerTopicName`), the physical name a
  /// plugin can read back. A script error returns the message and leaves
  /// the prior output AND recipe untouched. A `declared_inputs` length mismatch is
  /// rejected. An id's `ephemeral`/`history_exempt` flags are homogeneous across its
  /// bindings (a disagreeing binding is rejected before anything runs), and a recipe
  /// whose `all_datasets` differs from the id's bindings REPLACES the whole id, so no
  /// id ever owns both a Dataset-scope and a Global output.
  [[nodiscard]] Expected<std::vector<std::string>> upsertGenerator(GeneratorRecipe recipe);

  /// Retire the rule on every dataset it is bound to (persistent or ephemeral; see
  /// invariant 2). For an ephemeral preview the preview object topic is removed.
  /// Unknown `id` is an error.
  Status removeGenerator(std::string_view id);

  /// Re-run every generator whose input is affected by a change, re-publishing its
  /// output — the whole-series recompute hook. An input is affected when one of its
  /// series keys STARTS WITH a `changed` entry; an EMPTY `changed` re-runs ALL.
  /// Returns the republished object topics so the caller can re-notify overlays;
  /// a generator whose script errors on re-run keeps its last set, and an
  /// `all_datasets` one left without any contributor is emptied.
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

  /// Set-aware merge of the marker topics when datasets fold into `anchor`. Markers
  /// opt out of the generic ObjectStore fold (its interleave+retention would keep
  /// one set): every source marker topic moves onto the anchor's same-named topic,
  /// shifted by `raw_shift_ns` and concatenated after the anchor's set, then the
  /// source topic is dropped. No re-evaluation; the shared all-datasets set lives
  /// on no source and is untouched. MUST run before the sources are dropped.
  /// Returns the touched anchor topic names.
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

  /// Output names of persistent history-exempt generators reading a series rooted
  /// at one of `removed_inputs` (the topic itself or a slash-bounded field below it
  /// — stricter than `recomputeForChangedInputs`'s raw prefix match, which would
  /// also hit `imu_raw/x` for `imu` and reject a restore that strands nothing).
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
  /// consumed one is retired: `mergeMarkerTopics` (which runs first) already moved
  /// the consumed output onto the anchor's topic of this id, and the anchor's next
  /// recompute replaces it with a run over the merged data.
  void remapGeneratorsToAnchor(DatasetId anchor, const std::vector<DatasetId>& consumed);

  /// Forget every generator bound to a removed `dataset` (its topics went with it —
  /// a surviving recipe would otherwise re-register a phantom topic under the dead
  /// id on the next recompute); `all_datasets` generators are kept and their shared
  /// set rebuilt from the surviving contributors — emptied when none is left, the
  /// rule staying live. Returns the republished object topics.
  std::vector<std::string> clearGeneratorsForDataset(DatasetId dataset);

 private:
  /// The bindings of one id, in the order they were applied (a replacement keeps
  /// its slot). Never empty while the id is in `recipes_`.
  using GeneratorBindings = std::vector<GeneratorRecipe>;

  /// The dataset a binding is keyed on: a producer's id carries the rule's SCOPE
  /// but not its dataset, so the same dataset-bound id on a second dataset is a
  /// new binding while re-applying it on the SAME dataset replaces. `all_datasets`
  /// and `ephemeral` recipes are pinned to `kAnyDataset` (one per id).
  [[nodiscard]] static DatasetId bindingDataset(const GeneratorRecipe& recipe);

  /// The stored binding `recipe` would replace: same id, same `bindingDataset`.
  /// Null when there is none.
  [[nodiscard]] GeneratorRecipe* findBinding(const GeneratorRecipe& recipe);

  /// Every binding of every id, flattened (id order, then application order).
  [[nodiscard]] std::vector<std::reference_wrapper<const GeneratorRecipe>> allRecipes() const;

  /// The one place a recipe publishes to: the dataset and its OWN object-topic name.
  using PublishTarget = std::pair<DatasetId, std::string>;

  /// `recipe`'s target: (`dataset_id`, `<key>#<id>`) when bound,
  /// (`kAllDatasetsMarkerDataset`, …) when `all_datasets`. Requires an output
  /// (`upsertGenerator` guarantees one on every stored binding).
  [[nodiscard]] static PublishTarget publishTarget(const GeneratorRecipe& recipe);

  /// Retire `target`: tombstone it (persistent) or remove the topic outright
  /// (`ephemeral`). A no-op when it equals `keep` — the target the new revision of
  /// the same binding just rewrote.
  void retireTarget(const PublishTarget& target, bool ephemeral, const PublishTarget* keep = nullptr);

  /// `retireTarget` every store topic ending in `#<id>`, `keep` excepted. A store
  /// scan rather than the recipe's targets: a merge moves a consumed binding's
  /// output onto the anchor and drops the binding, so no recipe names it anymore.
  void retireOwnedTopics(std::string_view id, bool ephemeral, const PublishTarget* keep = nullptr);

  /// Run `recipe` and route its output by kind. Returns the resolved output topic(s).
  [[nodiscard]] Expected<std::vector<std::string>> runAndPublish(const GeneratorRecipe& recipe);

  /// kind=markers: run the script and publish PlotMarkers to `publishTarget(recipe)`
  /// — the one dataset's run when bound, the display-frame union of every
  /// contributor's run when `all_datasets` (invariant 3).
  [[nodiscard]] Expected<std::vector<std::string>> runMarkers(const GeneratorRecipe& recipe);

  /// The display-frame union of every contributing dataset's run of an
  /// `all_datasets` `recipe` — nothing is published. `nullopt` means no loaded
  /// dataset carries its inputs.
  [[nodiscard]] Expected<std::optional<sdk::PlotMarkers>> computeShared(const GeneratorRecipe& recipe);

  /// Run `recipe`'s script over `dataset_id`'s series — nothing is published.
  /// `nullopt` = an `all_datasets` recipe whose inputs ALL fail to resolve there;
  /// a dataset-bound recipe never skips (a missing input is a script error).
  [[nodiscard]] Expected<std::optional<sdk::PlotMarkers>> evaluateMarkers(
      const GeneratorRecipe& recipe, DatasetId dataset_id);

  /// `runAndPublish(recipe)` and append the topics it resolved to `affected`; a
  /// failed run contributes nothing (the recompute paths never surface errors).
  void runAndAccumulate(const GeneratorRecipe& recipe, std::vector<std::string>& affected);

  /// Re-run an existing `all_datasets` `recipe` over the current roster and
  /// republish it, appending its topic to `affected`; with no contributor left the
  /// set is emptied (tombstoned), not kept stale — only the initial upsert treats
  /// that as an error. A script error keeps the last good set.
  void republishShared(const GeneratorRecipe& recipe, std::vector<std::string>& affected);

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

  /// The set stored on `topic_id` shifted by `shift` plus any pending
  /// `payload_stamp_shift`; empty for a tombstone, a never-published topic or
  /// undecodable bytes.
  [[nodiscard]] sdk::PlotMarkers storedMarkers(ObjectTopicId topic_id, Timestamp shift) const;

  ObjectStore& object_store_;
  SeriesResolver resolver_;
  DatasetLister dataset_lister_;
  DisplayOffsetResolver display_offset_resolver_;
  std::map<std::string, GeneratorBindings> recipes_;  ///< id → per-dataset bindings (incl. ephemeral); invariant 2
};

}  // namespace PJ
