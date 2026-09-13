#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/merge_result.hpp"  // DatasetMergeSource
#include "pj_runtime/HistoryScope.h"

namespace PJ {

namespace sdk {
struct PlotMarkers;
}

class ObjectStore;

/// Output shape of a data processor (the `kind` discriminator of `pj.data_processors.v1`).
/// Only "markers" (objects → ObjectStore) is handled by THIS engine. "transform"
/// (per-sample timeseries → DerivedEngine) is routed to the DerivedEngine bridge owned
/// by the transform-editor work, not here.
enum class GeneratorKind {
  kMarkers,  ///< emits a discrete PlotMarkers set into the ObjectStore
};

/// Host service that runs plugin-submitted WHOLE-SERIES *object generators* — the
/// kind="markers" path of the unified `pj.data_processors.v1` SDK service (it supersedes
/// the old `pj.markers.v1` and the interim `pj.generators.v1`). For `kind=markers` it
/// publishes `PlotMarkers` into the `ObjectStore` (`__markers__/…`).
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
/// output in the GUI and in CI ("GUI == headless").
///
/// Whole-series, so there is NO incremental path (unlike `DataProcessorService`'s
/// `advanceOnCommit`): any change to an input re-runs the whole script and
/// republishes the marker set whole.
///
/// NOTE: the class/file name is kept as `MarkerService` to bound churn. A rename to
/// `ObjectEngine` (to admit object kinds beyond markers) is a cosmetic follow-up.
///
/// Series resolution is INJECTED (`SeriesResolver`), not wired to the catalog, so the
/// service is unit-testable against synthetic data and the catalog coupling stays in
/// the wiring layer. The public header stays free of the `pj_scripting` dependency
/// (linked PRIVATE) — hence the local `ResolvedSeries` rather than the engine's view.
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
  static constexpr DatasetId kAllDatasets = 0;

  /// A submitted generator: pure data, persisted in the layout (unless `ephemeral`).
  struct GeneratorRecipe {
    std::string id;  ///< plugin-namespaced stable key (upsert key)
    GeneratorKind kind = GeneratorKind::kMarkers;
    std::string language = "luau";     ///< script backend; only "luau" accepted today
    DatasetId dataset_id = 0;          ///< dataset the output lives in (when !all_datasets)
    std::vector<std::string> inputs;   ///< series keys the script reads
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

  /// Compile-only validation for `kind`/`language` (drives the editor red/green dot):
  /// rejects a non-"luau" language, else compiles + module-loads the script WITHOUT
  /// running it (no inputs, no side effects). Runtime errors are NOT caught here.
  [[nodiscard]] Status validateScript(GeneratorKind kind, std::string_view language, const std::string& script) const;

  /// Create or replace (by `id`) a generator and run it immediately. Returns the
  /// resolved physical output topic name(s) (the marker object topic). On a script
  /// error returns the message and leaves any prior output AND the prior recipe
  /// untouched (a bad upsert never destroys a working generator). `ephemeral`
  /// recipes are excluded from `recipes()`.
  [[nodiscard]] Expected<std::vector<std::string>> upsertGenerator(GeneratorRecipe recipe);

  /// Drop a generator by `id` (persistent or ephemeral). For an ephemeral preview the
  /// preview object topic is removed. Unknown `id` is an error.
  Status removeGenerator(std::string_view id);

  /// Re-run every generator whose input is affected by a change, re-publishing its
  /// output — the whole-series recompute hook. An input is affected when one of its
  /// series keys STARTS WITH a `changed` entry; an EMPTY `changed` re-runs ALL.
  /// Returns the affected marker object topic names so the caller can re-notify
  /// overlays; generators whose script errors on re-run are skipped.
  ///
  /// `scope` bounds the work to the dataset that actually ingested; `kAllDatasets`
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

  /// Snapshot of every PERSISTENT generator recipe (unspecified order) — for layout
  /// persistence. EPHEMERAL previews are excluded.
  [[nodiscard]] std::vector<GeneratorRecipe> recipes() const;

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
  /// untouched (they resolve through the dataset lister, not a stored id).
  void remapGeneratorsToAnchor(DatasetId anchor, const std::vector<DatasetId>& consumed);

  /// Forget every generator bound to `dataset`; `all_datasets` generators are kept
  /// (they belong to the session, not to one dataset). The markers twin of
  /// `DataProcessorService::clearTransformsForDataset`. Call when a dataset is removed:
  /// its object topics go with it, but a surviving recipe still names the dead id, and
  /// the next recompute would resolve nothing and re-register a phantom marker topic
  /// under a dataset that no longer exists (publishMarkerSet registers on a miss).
  /// Outputs are not tombstoned — the topics are already gone with the dataset.
  void clearGeneratorsForDataset(DatasetId dataset);

 private:
  /// One place a recipe publishes to: the dataset and the object-topic name.
  using PublishTarget = std::pair<DatasetId, std::string>;

  /// The datasets a recipe touches: its own when bound, `scope` when scoped, every
  /// loaded one otherwise. The single enumerator behind both publishing and retiring —
  /// deriving "where does this land" twice is how a rename strands a topic.
  [[nodiscard]] std::vector<DatasetId> targetDatasets(const GeneratorRecipe& recipe, DatasetId scope) const;

  /// Every (dataset, object-topic) a recipe publishes to right now — one entry for a
  /// per-dataset recipe, one per loaded dataset for an `all_datasets` one.
  [[nodiscard]] std::vector<PublishTarget> publishTargets(const GeneratorRecipe& recipe) const;

  /// Retire published output: an ephemeral preview's throwaway topic is removed
  /// outright, a persistent one is tombstoned (emptied in place) so a later
  /// merge/reload sees a live-but-empty set rather than a stale one.
  void retirePublished(const std::vector<PublishTarget>& targets, bool ephemeral);

  /// Run `recipe` and route its output by kind. Returns the resolved output topic(s).
  /// `scope` bounds where an `all_datasets` recipe publishes (see `targetDatasets`).
  [[nodiscard]] Expected<std::vector<std::string>> runAndPublish(
      const GeneratorRecipe& recipe, DatasetId scope = kAllDatasets);

  /// kind=markers: run the script and publish PlotMarkers to the object topic(s).
  [[nodiscard]] Expected<std::vector<std::string>> runMarkers(const GeneratorRecipe& recipe, DatasetId scope);

  /// Run the marker script over `inputs` (in `dataset_id`) and publish its PlotMarkers
  /// to `object_topic_name`. Shared by committed + preview marker paths.
  [[nodiscard]] Status runMarkersToObjectTopic(
      DatasetId dataset_id, const std::vector<std::string>& inputs, const std::string& script,
      const std::string& object_topic_name);

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

  ObjectStore& object_store_;
  SeriesResolver resolver_;
  DatasetLister dataset_lister_;
  std::unordered_map<std::string, GeneratorRecipe> recipes_;  ///< id → recipe (incl. ephemeral)
};

}  // namespace PJ
