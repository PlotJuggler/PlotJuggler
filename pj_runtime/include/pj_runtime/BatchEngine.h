#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"  // sdk::PlotMarkers
#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/merge_result.hpp"  // DatasetMergeSource
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/HistoryScope.h"
#include "pj_runtime/InFlightBudget.h"

class QThread;

namespace PJ {

class ObjectStore;
class SeriesReader;    // pj_datastore/query.hpp; by reference only in readResolvedSeries
struct ObjectTopicId;  // pj_datastore/object_store.hpp; by value only in a private declaration below

/// The `kind` of a `pj.data_processors.v1` processor this engine runs; `kind=transform` goes to the IncrementalEngine.
enum class GeneratorKind {
  kMarkers,  ///< emits a discrete PlotMarkers set into the ObjectStore
};

/// Runs the `kind=markers` processors of `pj.data_processors.v1` (marker rules): whole-series Luau scripts whose
/// `PlotMarkers` it publishes into the `ObjectStore`. It writes only to the `ObjectStore`, never the `DataEngine`.
/// A rule is data (script, input series keys, output topic, params); this service owns its execution, so the output
/// survives the plugin's unload. Any input change re-runs the whole script and republishes the whole set. Series
/// resolution is injected (`SeriesResolver`).
///
/// Execution: a run is staged on the GUI thread (the resolver copies the inputs into a `StagedRun`), computed on an
/// `EnginePool` worker over that copy only, and published on the GUI thread. An input change, or a catalog addition
/// for an `all_datasets` rule, only marks bindings stale; `Mode::kAutomatic` runs the stale, demanded ones through a
/// single-shot brake at `reloadHz()`, `Mode::kManual` waits for `refresh`/`refreshAll`. The whole-dataset recomputes
/// (`recomputeForDataset`, `clearGeneratorsForDataset`) are synchronous: a reload, restore or removal is complete
/// when it returns.
///
/// Result policy: each binding has at most one run in flight and one pending `refresh`, which captures its inputs
/// when that run ends. A completed run is published even when its inputs changed meanwhile; the binding stays stale
/// and runs again, so markers keep updating while data streams. A failed run (script error, a compute that threw)
/// publishes nothing: the last set stays drawn, the binding stays stale, `errorOf` says why until a run succeeds,
/// and the brake does not resubmit it until its inputs change (`refresh` or an edit runs it at once). A run is
/// cancelled, its result dropped, only when its meaning changes: the recipe is edited, rescoped or removed, or its
/// dataset is reloaded, merged (either side) or removed.
///
/// Invariants:
/// 1. Each binding writes its own object topic `__markers__/<key>#<id>` (`markerOwnerTopicName`; `#` is refused in
///    ids and keys), registered with metadata naming scope, target and owner (`topicMetadata`). Two rules on one key
///    never clobber each other; retiring one tombstones the topics whose metadata names it as owner (a store scan, so
///    an output a merge moved onto the anchor goes too). The overlay draws the union of a key's family, so the store
///    is the only copy, and a direct toolbox write to the bare topic is a family member this service never touches.
/// 2. `recipes_` is keyed by id (the plugin ABI's identity), each holding per-dataset bindings; an `all_datasets` or
///    `ephemeral` id has exactly one, and no id holds zero.
/// 3. Scope says where markers are drawn. A dataset-bound rule reads and draws on its dataset. An `all_datasets` rule
///    reads every loaded dataset carrying its inputs and publishes the union once on `kAllDatasetsMarkerDataset`, in
///    the display frame (raw minus each contributor's alignment offset), so a Source Timeline drag republishes
///    nothing.
class BatchEngine : public QObject {
  Q_OBJECT
 public:
  /// One input series; timestamps in nanoseconds. Kept free of the marker engine's types, so this header does not
  /// depend on pj_scripting.
  struct ResolvedSeries {
    std::vector<double> timestamps;
    std::vector<double> values;
  };

  /// Raw dataset nanoseconds, inclusive; nullopt = the whole series.
  using Window = std::optional<std::pair<Timestamp, Timestamp>>;

  /// One input series key in a dataset, in time order; nullopt for an unknown key. GUI thread, once per input per
  /// run. Copy only `Window`'s range: the result is clipped to it again.
  using SeriesResolver = std::function<std::optional<ResolvedSeries>(DatasetId, const std::string&, const Window&)>;

  using DatasetLister = std::function<std::vector<DatasetId>()>;

  /// "Not attributable to one dataset": reach every loaded one.
  static constexpr DatasetId kAnyDataset = 0;

  /// Persisted in the layout unless `ephemeral`.
  struct GeneratorRecipe {
    std::string id;  ///< plugin-namespaced, the upsert key
    GeneratorKind kind = GeneratorKind::kMarkers;
    std::string language = "luau";  ///< only "luau" is accepted
    DatasetId dataset_id = 0;       ///< where the output lives unless all_datasets
    /// Bare series keys: the host strips a bound rule's dataset qualifier and refuses one under `all_datasets`.
    std::vector<std::string> inputs;
    /// The names the plugin declared, parallel to `inputs` (or empty when none changed): the script reads each series
    /// under this spelling (`series("src:key")`) and under its bare key.
    std::vector<std::string> declared_inputs;
    std::vector<std::string> outputs;  ///< [marker_topic]; may be empty for an ephemeral preview
    std::string script;                ///< rule source (binary-safe blob)
    std::string params_json;           ///< forwarded verbatim to the engine
    /// Every series is clipped to it. An `all_datasets` rule clips each contributor in its own raw domain, so it only
    /// means something across datasets sharing a clock. An empty window runs the rule over empty series.
    Window window;
    bool all_datasets = false;
    bool ephemeral = false;  ///< a preview: not in recipes(), dropped on remove
    /// Persisted, but outside undo/redo; a layout replacement or an explicit removal still replaces it.
    bool history_exempt = false;
  };

  /// `object_store` and the data `resolver` reads must outlive this service.
  BatchEngine(ObjectStore& object_store, SeriesResolver resolver);
  /// Cancels the runs in flight; none is published.
  ~BatchEngine() override;

  /// The shell injects the catalog-backed resolver later: SessionManager builds this before the catalog exists.
  void setResolver(SeriesResolver resolver);

  void setDatasetLister(DatasetLister lister);

  /// A dataset's alignment offset (`display = raw - offset`), putting each `all_datasets` contribution in the display
  /// frame. Defaults to 0.
  using DisplayOffsetResolver = std::function<Timestamp(DatasetId)>;
  void setDisplayOffsetResolver(DisplayOffsetResolver resolver);

  /// Without a pool, `refresh` and the brake run inline. Changing it cancels the runs in flight; set nullptr before
  /// destroying the pool if this engine outlives it.
  void setPool(EnginePool* pool);

  /// What a run reserves before it stages: the VM cap plus the marker output ceiling, one ceiling per run however many
  /// datasets contribute. The staged copy is charged on top once staged (`StagedRun::bytes`).
  static const std::size_t kMarkerRunBytes;
  /// The in-flight budget shared with the on-demand runs; null bounds nothing. A pooled run waits in its FIFO line for
  /// `kMarkerRunBytes`, stages, then grows its reservation by the staged copy; a copy that does not fit keeps its place
  /// at the head of the line. The room is held until publication (a cancelled run's until the pool lets it go). Inline
  /// runs reserve nothing. Changing it cancels the runs in flight; every budget given must outlive this engine and the
  /// pool.
  void setInFlightBudget(InFlightBudget* budget);

  /// Whether `dataset` deserves an automatic run. An `all_datasets` rule is demanded when one contributor is (a dataset
  /// carrying one of its inputs, or any when it has none). Without a predicate every dataset is.
  using DemandPredicate = std::function<bool(DatasetId)>;
  void setDemandPredicate(DemandPredicate predicate);
  /// Re-arms the brake, so a stale rule whose dataset just became demanded runs without waiting for an ingest.
  void demandChanged();

  /// `kManual` only marks stale and stops an armed brake; switching to automatic runs what is stale.
  enum class Mode { kAutomatic, kManual };
  void setMode(Mode mode);
  [[nodiscard]] Mode mode() const noexcept {
    return mode_;
  }

  static constexpr double kMinReloadHz = 0.2;
  static constexpr double kMaxReloadHz = 20.0;
  static constexpr double kDefaultReloadHz = 2.0;

  /// Hz, clamped to [kMinReloadHz, kMaxReloadHz].
  void setReloadHz(double hz);
  [[nodiscard]] double reloadHz() const noexcept {
    return reload_hz_;
  }

  /// The user's choice (Preferences).
  struct Settings {
    Mode mode = Mode::kAutomatic;
    double reload_hz = kDefaultReloadHz;
  };
  /// QSettings keys (persisted): Preferences::derived_batch_mode ("automatic"/"manual") and
  /// Preferences::derived_batch_reload_hz, clamped so a hand-edited .ini cannot stall or flood the brake.
  [[nodiscard]] static Settings loadSettings();
  static void saveSettings(const Settings& settings);
  void setSettings(const Settings& settings);

  /// How long a `CreateWait::kBounded` create waits on the GUI thread for the first run; the same budget as a
  /// transform's first replay slice (`DataProcessorService::kMaterializationSliceBudget`).
  static constexpr std::chrono::milliseconds kCreateBudget{8};
  /// Test seam for every engine of the process; returns the previous value. GUI thread.
  static std::chrono::milliseconds setCreateBudgetForTest(std::chrono::milliseconds budget);

  /// Where a failure no caller hears goes (refresh, brake, a bounded create past its budget, a whole-dataset
  /// recompute): an error `Diagnostic` once per binding and distinct error, again only after a success. Without a
  /// sink it is only logged.
  void setDiagnosticSink(DiagnosticSink sink);

  /// A scheduled run, or a create's first run past its budget.
  [[nodiscard]] bool isRunning(std::string_view id) const;
  /// Every generator id with a run in flight, sorted, each once.
  [[nodiscard]] std::vector<std::string> runningIds() const;
  /// Per binding (its id on its dataset); the id-level queries merge a rule's bindings.
  [[nodiscard]] bool isBindingRunning(const GeneratorRecipe& binding) const;
  [[nodiscard]] std::string bindingError(const GeneratorRecipe& binding) const;

  /// Runs every binding of `id` now, ignoring brake, mode and demand. A binding with a run in flight keeps one pending
  /// request, which captures its inputs when that run completes. Unknown `id`: no-op.
  void refresh(std::string_view id);
  /// `refresh` every rule bound to `dataset`, plus every `all_datasets` one.
  void refreshAll(DatasetId dataset);

  /// From an input change (or a scheduled run starting) until a run that captured the newest inputs is published.
  [[nodiscard]] bool isStale(std::string_view id) const;
  [[nodiscard]] std::vector<std::string> staleIds() const;
  /// Why the last run of one of `id`'s bindings failed; empty once a run succeeds.
  [[nodiscard]] std::string errorOf(std::string_view id) const;

  /// Raw ns staged by the last published run of `id`'s first binding, after the window and retention; nullopt before a
  /// run or when it staged nothing. A failed run keeps the previous value.
  [[nodiscard]] std::optional<std::pair<Timestamp, Timestamp>> coverageOf(std::string_view id) const;

  /// Test-only: called with the current thread per staged contribution computed. Set before, clear after the runs.
  static void setComputeThreadProbeForTest(std::function<void(QThread*)> probe);

  /// The catalog grew: marks every `all_datasets` rule stale (the commit recompute runs before the catalog rebuild,
  /// so it cannot see a newcomer). The brake runs them, so a burst of additions costs one run.
  void reachNewDatasets();

  /// Empty without a lister.
  [[nodiscard]] std::vector<DatasetId> loadedDatasets() const;

  /// Anything but "luau" (empty means luau) is refused; shared by validate, upsert and the bridge's script check.
  [[nodiscard]] static Status checkLanguage(std::string_view language);

  /// Compiles and module-loads the script without running it; runtime errors are not caught.
  [[nodiscard]] Status validateScript(GeneratorKind kind, std::string_view language, const std::string& script) const;

  /// How long a create holds its caller for the first run.
  enum class CreateWait {
    /// Computed on the calling thread and published before the create returns, so every failure is returned. Plugin
    /// creates (a released plugin reads the markers right after create returns) and undo/redo (the user expects the
    /// previous markers back, not a blank set) use it.
    kToCompletion,
    /// Computed on the pool while the caller waits at most `kCreateBudget`; a run still going continues as the
    /// binding's scheduled run. Without room in the budget, the rule is installed and its run waits in the line like
    /// a refresh. Without a pool, as `kToCompletion`. A layout load uses it: nobody reads the markers at once.
    kBounded,
  };

  /// Creates or replaces (by `id`) a rule and runs it; returns its own object topic (`markerOwnerTopicName`). Compile
  /// and staging happen on the GUI thread, then the first run computes as `wait` says. When it finished in the call,
  /// a failure (compile or script error, no input in any dataset) is returned with the prior output and recipe
  /// untouched. A `kBounded` run still going publishes later like a scheduled one, its failure reported through
  /// `errorOf` and the diagnostic sink. Rejected: a `declared_inputs` length mismatch, an empty id, an id or output key
  /// that is not UTF-8 or contains `#`, a binding disagreeing with the id's `ephemeral`/`history_exempt` flags. A
  /// recipe whose `all_datasets` differs from the id's bindings replaces the whole id, so no id owns both scopes.
  [[nodiscard]] Expected<std::vector<std::string>> upsertGenerator(
      GeneratorRecipe recipe, CreateWait wait = CreateWait::kToCompletion);

  /// Retires the rule on every dataset it is bound to; a preview's topic is removed. Unknown `id` is an error.
  Status removeGenerator(std::string_view id);

  /// Marks stale every rule with an affected input and arms the brake; nothing runs here. An input is affected when
  /// one of its series keys is rooted at a `changed` topic (`imu` does not affect `imu_raw/x`); an empty `changed`
  /// affects all. Returns the affected ids.
  ///
  /// `scope` is the dataset that ingested; `kAnyDataset` when the change belongs to no single one. `changed` holds
  /// bare topic names, which repeat across datasets: unscoped, a tick on one dataset marks rules bound to another and
  /// re-runs every `all_datasets` rule over every dataset, large unchanged files included. No default, on purpose.
  [[nodiscard]] std::vector<std::string> recomputeForChangedInputs(
      const std::vector<std::string>& changed, DatasetId scope);

  /// Re-runs every rule bound to `dataset`, and every `all_datasets` rule on `dataset` only: the recompute a reload
  /// needs, where name matching misses inputs the swap renamed or dropped. Cancels those bindings' runs in flight.
  /// Synchronous: its callers repaint from the returned topics, and a rollback leaves no marker computed from the data
  /// it rolled back. A failure is recorded like a scheduled one. Returns the affected marker topic names.
  [[nodiscard]] std::vector<std::string> recomputeForDataset(DatasetId dataset);

  /// Merges marker topics when datasets fold into `anchor` (the generic ObjectStore fold would keep one set): each
  /// source marker topic moves onto the anchor's same-named topic, shifted by `raw_shift_ns` and appended, and the
  /// source topic is dropped. Nothing re-runs; the all-datasets set is untouched. Run it before the sources are
  /// dropped. Returns the touched anchor topics.
  std::vector<std::string> mergeMarkerTopics(DatasetId anchor, const std::vector<DatasetMergeSource>& sources);

  /// Includes previews, which also recompute live.
  [[nodiscard]] bool hasGenerators() const noexcept {
    return !recipes_.empty();
  }

  /// Every persistent binding (an id on several datasets appears once each), for layout persistence.
  [[nodiscard]] std::vector<GeneratorRecipe> recipes() const;

  /// `recipes().size()` without the copies.
  [[nodiscard]] std::size_t persistentBindingCount() const;

  /// Every persistent id once: the plugin ABI's `list()`.
  [[nodiscard]] std::vector<std::string> generatorIds() const;

  /// The first-applied binding (what the plugin ABI's `config()` reports; a replacement on its dataset keeps the
  /// slot), or null.
  [[nodiscard]] const GeneratorRecipe* firstBinding(std::string_view id) const;

  /// Outputs of persistent history-exempt rules reading a series rooted at one of `removed_inputs`.
  [[nodiscard]] std::vector<std::string> exemptDependentsOf(const std::vector<std::string>& removed_inputs) const;

  /// The ids a restore to `wanted` (dataset identities resolved) may leave live, keeping topic, published set, run
  /// state and coverage. Per id: each persistent binding equals the wanted binding on the same dataset on every
  /// persisted field (script and params ignoring line endings), with no extra binding on either side. Never kept: an id
  /// outside the intent's authority; one reading a series rooted at a name in `removed_upstream` (outputs the restore
  /// removes or re-creates); an `all_datasets` rule whose contributor offsets moved since its last publication (its
  /// set is stored in the display frame), that has a run in flight, or that never published. A failed rule is kept
  /// as it is.
  [[nodiscard]] std::unordered_set<std::string> unchangedGeneratorIds(
      const std::vector<GeneratorRecipe>& wanted, RestoreIntent intent,
      const std::vector<std::string>& removed_upstream) const;

  /// Removes every persistent rule (tombstoning its output) except `keep`; previews stay. `kHistory` leaves the
  /// `history_exempt` ones live; `kReplace` removes all.
  void clearGenerators(RestoreIntent intent, const std::unordered_set<std::string>& keep = {});
  /// The cheap gate of the history-restore dependency preflight.
  [[nodiscard]] bool hasHistoryExemptGenerators() const;

  /// `clearGenerators(RestoreIntent::kReplace)`.
  void clearAllGenerators();

  /// `"<owner_plugin>/"`: the namespace `MarkersRuntimeHost` mints ids in.
  [[nodiscard]] static std::string ownerPrefix(std::string_view owner_plugin);

  /// Removes the previews among `ids` and their topics; gone or persistent ids are skipped. `DerivedRuntimeHosts`
  /// passes its bridge's previews when the plugin instance ends. Returns whether anything went.
  bool removeEphemerals(const std::unordered_set<std::string>& ids);

  /// Cancels the runs in flight of every binding reading `dataset` (bound, or `all_datasets`) and forgets their
  /// scheduling state: a reload starting, a merge.
  void cancelRunsOn(DatasetId dataset);

  /// Rebinds the rules bound to a `consumed` dataset onto `anchor`: a merge drops the sources from the catalog, so a
  /// rule naming one would silently stop producing. `all_datasets` rules are untouched (they use the lister). When the
  /// anchor has its own binding of the id, that one wins and the consumed one is retired (`mergeMarkerTopics`, run
  /// first, already moved its output onto the anchor's topic). Runs staged before the merge are cancelled, and every
  /// anchor and `all_datasets` binding is marked stale.
  void remapGeneratorsToAnchor(DatasetId anchor, const std::vector<DatasetId>& consumed);

  /// Forgets the rules bound to a removed `dataset` (one surviving would re-register a phantom topic under the dead
  /// id). `all_datasets` rules stay, their set rebuilt from the survivors, or emptied when none is left. Returns the
  /// republished topics.
  std::vector<std::string> clearGeneratorsForDataset(DatasetId dataset);

 signals:
  /// Scheduled runs published their sets (GUI thread). Queued, once per pool drain. The synchronous paths do not
  /// emit it: their callers notify.
  void markersPublished();
  /// `runningIds()` or an `errorOf` answer may have changed. Queued and coalesced like `markersPublished`.
  void runningChanged();

 private:
  /// In application order (a replacement keeps its slot); never empty.
  using GeneratorBindings = std::vector<GeneratorRecipe>;

  /// The dataset a binding is keyed on: re-applying an id on the same dataset replaces, on another dataset adds.
  /// `all_datasets` and `ephemeral` recipes use `kAnyDataset` (one per id).
  [[nodiscard]] static DatasetId bindingDataset(const GeneratorRecipe& recipe);

  [[nodiscard]] std::vector<std::reference_wrapper<const GeneratorRecipe>> allRecipes() const;

  using PublishTarget = std::pair<DatasetId, std::string>;

  /// The markers type, the scope and target of its output key (`markerInfoForKey`), and the id as owner.
  [[nodiscard]] static std::string topicMetadata(const GeneratorRecipe& recipe);

  /// (`dataset_id`, `<key>#<id>`), or (`kAllDatasetsMarkerDataset`, ...) for `all_datasets`. Requires an output.
  [[nodiscard]] static PublishTarget publishTarget(const GeneratorRecipe& recipe);

  /// Tombstones a persistent target, removes an ephemeral one; no-op when it equals `keep` (what the new revision of
  /// the same binding just wrote).
  void retireTarget(const PublishTarget& target, bool ephemeral, const PublishTarget* keep = nullptr);

  /// By store metadata, not the recipe's targets: a merge moves a consumed binding's output onto the anchor and drops
  /// the binding.
  void retireOwnedTopics(std::string_view id, bool ephemeral, const PublishTarget* keep = nullptr);

  /// `display_shift`: 0 for a bound rule, `-displayOffsetOf(dataset_id)` for an `all_datasets` one.
  struct StagedContribution {
    DatasetId dataset_id = 0;
    std::unordered_map<std::string, ResolvedSeries> inputs;
    Timestamp display_shift = 0;
  };

  /// What a run reads, captured on the GUI thread: one contribution for a bound rule (a missing input is a script
  /// error), one per dataset carrying any input for an `all_datasets` one.
  struct StagedRun {
    GeneratorRecipe recipe;
    std::vector<StagedContribution> contributions;
    /// `[first, last]` raw ns across every staged sample; `nullopt` when none.
    std::optional<std::pair<Timestamp, Timestamp>> coverage_ns;
    /// Timestamps and values, no vector slack: what the in-flight budget charges.
    [[nodiscard]] std::size_t bytes() const;
  };

  /// GUI thread.
  [[nodiscard]] StagedRun stageRun(const GeneratorRecipe& recipe) const;

  /// Any thread; touches no engine or store state. Moves the staged series out. nullopt when nothing was staged.
  [[nodiscard]] static Expected<std::optional<sdk::PlotMarkers>> computeRun(
      StagedRun& staged, const EnginePool::CancelToken* cancel);

  /// GUI thread: publishes, or tombstones on nullopt (an `all_datasets` rule without contributors), and records the
  /// coverage and the display shifts used.
  [[nodiscard]] Status publishRun(const StagedRun& staged, std::optional<sdk::PlotMarkers> computed);

  /// A whole-dataset recompute of one binding on the calling thread: cancels its run in flight, then stages, computes,
  /// publishes. An `all_datasets` rule without contributors is emptied, not kept stale. A failure is recorded.
  void rerunNow(const GeneratorRecipe& recipe, std::vector<std::string>& affected);

  [[nodiscard]] Timestamp displayOffsetOf(DatasetId dataset) const;

  /// Finds or registers the topic (retention capped at one snapshot: sets are republished whole at a sentinel
  /// timestamp) and publishes `set`. An existing topic must describe the same set (scope, target, owner) or the
  /// publish fails and the topic is untouched. The one publish path of runs and merges.
  [[nodiscard]] Status publishMarkerSet(
      DatasetId dataset_id, const std::string& object_topic_name, const std::string& metadata_json,
      const sdk::PlotMarkers& set);

  /// The removal tombstone: an empty set, so the topic keeps its id and retention and draws nothing. No-op for a
  /// never-published topic.
  void publishEmptyMarkers(DatasetId dataset_id, const std::string& object_topic_name);

  /// The stored set shifted by `shift` plus any pending `payload_stamp_shift`; empty for a tombstone, a never-published
  /// topic or undecodable bytes.
  [[nodiscard]] sdk::PlotMarkers storedMarkers(ObjectTopicId topic_id, Timestamp shift) const;

  using BindingKey = std::pair<std::string, DatasetId>;
  [[nodiscard]] static BindingKey bindingKey(const GeneratorRecipe& recipe);
  [[nodiscard]] GeneratorRecipe* bindingOf(const BindingKey& key);

  /// One binding's scheduling state.
  struct RunState {
    uint64_t change_seq = 0;  ///< bumped by every input change
    bool stale = false;
    bool pending = false;  ///< a `refresh` waiting for the running run; holds no inputs
    /// While a run is in flight: its pool token, or a default (never cancelled) one while it runs inline or waits.
    std::optional<EnginePool::CancelToken> running;
    InFlightBudget::Ticket room;  ///< its place in the budget's line while it waits for room
    /// The run being staged, recognised once the resolver returned; 0 once dropped.
    uint64_t admission = 0;
    std::string error;  ///< the last run's failure; empty after a success
    /// The `change_seq` the last failed run staged: the brake skips the binding until its inputs change.
    std::optional<uint64_t> failed_seq;
  };

  void markStale(const GeneratorRecipe& recipe);
  struct RunResult;
  /// Its publish calls `finishRun` unless the run was cancelled or the create path consumed `result`.
  EnginePool::CancelToken startRun(
      const BindingKey& key, std::shared_ptr<StagedRun> staged, uint64_t staged_seq, std::shared_ptr<RunResult> result);
  /// Stage now, compute on the pool (or inline), publish in `finishRun`; with a run in flight only sets `pending`.
  /// With a budget it first waits in the line for `kMarkerRunBytes`.
  void submitRun(const GeneratorRecipe& recipe);
  /// A run cancelled, removed or replaced while it staged (or whose pool or budget changed) starts nothing.
  void stageAndStart(
      const BindingKey& key, const GeneratorRecipe& recipe, std::optional<InFlightBudget::Reservation> room);
  /// Also the waiting and staging runs and the pending refreshes: the pool or the budget changed.
  void dropRunsInFlight();
  /// GUI thread.
  void finishRun(
      const BindingKey& key, const StagedRun& staged, uint64_t staged_seq,
      Expected<std::optional<sdk::PlotMarkers>> result);
  /// The binding stays stale until an input change; the sink hears a new error once.
  void recordFailure(const BindingKey& key, uint64_t staged_seq, const std::string& error);
  [[nodiscard]] bool isDemanded(const GeneratorRecipe& recipe) const;
  /// Its result is never published; the scheduling state is forgotten.
  void cancelRun(const BindingKey& key);
  void cancelRunsOf(std::string_view id);
  /// Automatic mode: arms the brake for the rest of the current period unless armed.
  void scheduleDirty();
  /// Automatic mode: submits every stale, demanded binding with no run in flight that did not fail on its inputs.
  void onBrakeFired();
  /// Emits from the event loop, once however often called before.
  void queueSignal(bool& queued, void (BatchEngine::*signal)());

  ObjectStore& object_store_;
  SeriesResolver resolver_;
  DatasetLister dataset_lister_;
  DisplayOffsetResolver display_offset_resolver_;
  std::map<std::string, GeneratorBindings> recipes_;  ///< id → per-dataset bindings, previews included

  EnginePool* pool_ = nullptr;
  InFlightBudget* budget_ = nullptr;
  uint64_t next_admission_ = 0;  ///< the last `RunState::admission` handed out
  DemandPredicate demand_predicate_;
  Mode mode_ = Mode::kAutomatic;
  double reload_hz_ = kDefaultReloadHz;
  QTimer brake_;
  QElapsedTimer since_last_run_;  ///< restarted when the brake fires
  std::map<BindingKey, RunState> runs_;
  struct PublishedRun {
    std::optional<std::pair<Timestamp, Timestamp>> coverage;  ///< `coverageOf`
    /// Recorded for `all_datasets` rules only (their set is stored in the display frame).
    std::vector<std::pair<DatasetId, Timestamp>> display_shifts;
  };
  std::map<BindingKey, PublishedRun> published_;
  bool markers_published_queued_ = false;
  bool running_changed_queued_ = false;
  DiagnosticSink diagnostic_sink_;
};

/// A stored scalar series (retention floor applied, null rows skipped) as timestamps in ns doubles, sorted (chunk
/// order is not, after out-of-order ingest), limited to `window`. One O(n) pass: AppSession's resolver calls it on
/// the GUI thread while a run's inputs are staged, so never loop over `sampleAt(i)`.
[[nodiscard]] BatchEngine::ResolvedSeries readResolvedSeries(
    const SeriesReader& series, const BatchEngine::Window& window = std::nullopt);

}  // namespace PJ
