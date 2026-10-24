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

/// Output shape of a data processor (the `kind` discriminator of `pj.data_processors.v1`).
/// Only "markers" (objects → ObjectStore) is handled by THIS engine. "transform"
/// (per-sample timeseries → IncrementalEngine) is routed to the IncrementalEngine bridge owned
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
/// generators (`kind=transform`) routed to the `IncrementalEngine`, NOT from here.
///
/// Host-driven model: a generator is pure DATA — a Luau script + input series keys +
/// output topic + params. Nothing executable crosses any boundary; THIS service owns
/// execution and re-runs on data change, so output survives plugin unload. Scripts
/// run through `pj_scripting`'s `runMarkerScript` — the SAME engine the headless
/// `anomaly_runner` links, so GUI == headless. Whole-series: any input change
/// re-runs the whole script and republishes the set whole. Series resolution is
/// INJECTED (`SeriesResolver`) so the service is testable against synthetic data;
/// the header stays free of the marker engine's types, hence `ResolvedSeries`.
///
/// Execution (docs/derived_objects/ARCHITECTURE.md §11): a run is staged on the GUI
/// thread (the resolver copies the input series into a `StagedRun`), computed on an
/// `EnginePool` worker (the script only, over that copy) and published on the GUI
/// thread. An input change, or a catalog addition for an `all_datasets` rule
/// (`reachNewDatasets`), only marks its generators STALE; `Mode::kAutomatic` runs
/// the stale ones whose dataset is demanded through a single-shot brake at
/// `reloadHz()`, `Mode::kManual` waits for `refresh`/`refreshAll`. `upsertGenerator`
/// runs its first run to completion unless the caller asks for the bounded wait
/// (`CreateWait`). The whole-dataset recomputes (`recomputeForDataset`,
/// `clearGeneratorsForDataset`) stay SYNCHRONOUS: a reload, a restore or a removal is
/// complete when it returns.
///
/// Result policy (D30; stated here only, the docs point here): each
/// binding has at most one run in flight and one pending `refresh`, which holds no
/// inputs and captures them when that run ends. A run that completes is published
/// even when its inputs changed meanwhile; the binding then stays stale and runs
/// again with the newest inputs, so markers keep updating while data streams. A run
/// that fails (a script error, or a compute that threw) publishes nothing: the last
/// set stays drawn, the binding stays stale and `errorOf` says why until a run
/// succeeds; the brake does not resubmit it until its inputs change (a `refresh` or
/// an edit runs it at once). A run is cancelled, and its result dropped, only when
/// its meaning changes: the recipe is edited, rescoped or removed, its dataset is
/// reloaded, merged (as the anchor or merged away) or removed.
///
/// Invariants:
/// 1. Every generator writes its OWN object topic, named `__markers__/<key>#<id>`
///    (`markerOwnerTopicName`, an opaque id; `#` is refused in ids and keys) and
///    registered with metadata that says what it is: scope, target and owner
///    (`topicMetadata`). Two rules on one key never clobber each other, and retiring
///    one tombstones only the topics it owns — found by scanning the store's metadata,
///    so an output a merge moved onto the anchor is retired too. The overlay draws the union of a key's family, so
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
///    republish. `reachNewDatasets` marks it stale when the catalog grows and
///    `clearGeneratorsForDataset` re-runs it when a contributor goes.
class BatchEngine : public QObject {
  Q_OBJECT
 public:
  /// Samples of one resolved input series: timestamps in nanoseconds + values.
  struct ResolvedSeries {
    std::vector<double> timestamps;
    std::vector<double> values;
  };

  /// `[start, end]` RAW dataset nanoseconds, inclusive; `nullopt` = the whole series.
  using Window = std::optional<std::pair<Timestamp, Timestamp>>;

  /// Resolve one input series key (e.g. "/imu/accel/x") within a dataset to its
  /// samples in time order, or `nullopt` if the key is unknown. Called on the GUI
  /// thread once per input per run, with the recipe's window: copy only that range
  /// (the result is clipped to it again, so ignoring it costs only the copy).
  using SeriesResolver = std::function<std::optional<ResolvedSeries>(DatasetId, const std::string&, const Window&)>;

  /// List every loaded dataset id — for `all_datasets` generators. Injected by AppSession.
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
    /// Every resolved series is clipped to it before the rule runs. An `all_datasets`
    /// recipe clips each contributor in its own raw domain, so a cross-dataset window
    /// only makes sense when the datasets share a clock. A window with no sample is
    /// not an error: the rule runs over empty series and publishes an empty set.
    Window window;
    bool all_datasets = false;  ///< publish to EVERY dataset (markers only)
    bool ephemeral = false;     ///< preview: excluded from recipes(); dropped on remove
    /// Persisted in layout files but outside undo/redo authority. A layout
    /// replacement or explicit removal may still replace it.
    bool history_exempt = false;
  };

  /// `object_store` and the data the `resolver` reads from must outlive this service.
  BatchEngine(ObjectStore& object_store, SeriesResolver resolver);
  /// Cancels the runs in flight: none of them is published afterwards.
  ~BatchEngine() override;

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

  /// The pool scheduled runs execute on. Without one, `refresh` and the brake run
  /// inline on the calling thread. Changing it cancels the runs in flight; set
  /// `nullptr` before destroying the pool if this engine outlives it.
  void setPool(EnginePool* pool);

  /// What a marker run reserves from the in-flight budget before it stages: the VM cap plus the native marker
  /// output ceiling, ONE ceiling for the whole run however many datasets contribute (`computeRun` shares it). The
  /// staged series copy is charged on top, once staged (`StagedRun::bytes`).
  static const std::size_t kMarkerRunBytes;
  /// The in-flight budget the runs on the pool reserve from, shared with the on-demand runs
  /// (docs/derived_objects/ARCHITECTURE.md §11); null (the default) bounds nothing. A run waits in the budget's
  /// line, first in, first out with every consumer, for `kMarkerRunBytes`, stages once it has them, then grows its
  /// reservation by the staged copy; a copy that does not fit keeps its place at the head of the line until room
  /// for the whole run returns. The room is held until the run's publication (cancelled runs included, until the
  /// pool lets them go). Inline runs (no pool, a create to completion, the whole-dataset recomputes) reserve
  /// nothing: they hold no pool thread. Changing it cancels the runs in flight, as `setPool` does; every budget it
  /// was given must outlive this engine and the pool.
  void setInFlightBudget(InFlightBudget* budget);

  /// Whether `dataset` has a consumer worth an automatic run. A bound rule asks for its
  /// dataset; an `all_datasets` rule is demanded when one of its contributors is (a
  /// loaded dataset that carries one of its inputs, or any dataset when it has none).
  /// Without a predicate, every dataset counts as demanded.
  using DemandPredicate = std::function<bool(DatasetId)>;
  void setDemandPredicate(DemandPredicate predicate);
  /// The demand predicate's answer may have changed: re-arms the brake, so a stale
  /// generator whose dataset just became demanded runs without waiting for an ingest.
  void demandChanged();

  /// `kAutomatic` re-runs stale, demanded generators through the brake;
  /// `kManual` only marks them stale until `refresh`/`refreshAll`, and stops a brake
  /// already armed. Switching to automatic runs what is stale.
  enum class Mode { kAutomatic, kManual };
  void setMode(Mode mode);
  [[nodiscard]] Mode mode() const noexcept {
    return mode_;
  }

  static constexpr double kMinReloadHz = 0.2;
  static constexpr double kMaxReloadHz = 20.0;
  static constexpr double kDefaultReloadHz = 2.0;

  /// Automatic mode's rate limit, in Hz, clamped to [kMinReloadHz, kMaxReloadHz].
  void setReloadHz(double hz);
  [[nodiscard]] double reloadHz() const noexcept {
    return reload_hz_;
  }

  /// The user's choice of mode and reload rate (Preferences, Plotting page).
  struct Settings {
    Mode mode = Mode::kAutomatic;
    double reload_hz = kDefaultReloadHz;
  };
  /// QSettings keys: Preferences::derived_batch_mode ("automatic"/"manual"),
  /// Preferences::derived_batch_reload_hz (clamped to the range above, so a
  /// hand-edited .ini cannot stall or flood the brake).
  [[nodiscard]] static Settings loadSettings();
  static void saveSettings(const Settings& settings);
  void setSettings(const Settings& settings);

  /// How long a `CreateWait::kBounded` create waits on the GUI thread for a rule's
  /// first run. The precedent is a transform's first replay slice
  /// (`DataProcessorService::kMaterializationSliceBudget`, the same 8 ms): a create
  /// blocks the GUI for about half a 60 Hz frame on top of staging, however long the
  /// rule runs.
  static constexpr std::chrono::milliseconds kCreateBudget{8};
  /// Test seam: replaces `kCreateBudget` for every engine of the process and returns
  /// the previous value. GUI thread only.
  static std::chrono::milliseconds setCreateBudgetForTest(std::chrono::milliseconds budget);

  /// Where a failure that no caller is told about goes (a refresh, the brake, a
  /// bounded create's first run that failed after its budget, or a whole-dataset
  /// recompute): an error `Diagnostic` naming the rule and the next step, once per
  /// binding and distinct error, again only after a run succeeded. Without a sink the
  /// failure is only logged.
  void setDiagnosticSink(DiagnosticSink sink);

  /// Whether a run of one of `id`'s bindings is in flight: a scheduled run, or a
  /// create's first run that outlived its budget.
  [[nodiscard]] bool isRunning(std::string_view id) const;
  /// Every generator id with a run in flight, sorted, each once.
  [[nodiscard]] std::vector<std::string> runningIds() const;
  /// `isRunning` and `errorOf` for the one binding `binding` names (its id on its dataset): a rule applied
  /// to several datasets has a state per dataset, which the id-level queries merge.
  [[nodiscard]] bool isBindingRunning(const GeneratorRecipe& binding) const;
  [[nodiscard]] std::string bindingError(const GeneratorRecipe& binding) const;

  /// Runs every binding of `id` now, ignoring the brake, the mode and demand. A
  /// binding with a run in flight keeps one pending request instead, which captures
  /// its inputs when that run completes. Unknown `id` is a no-op.
  void refresh(std::string_view id);
  /// `refresh` every generator bound to `dataset`, plus every `all_datasets` one.
  void refreshAll(DatasetId dataset);

  /// True from an input change (or a scheduled run starting) until a run that
  /// captured the newest inputs is published.
  [[nodiscard]] bool isStale(std::string_view id) const;
  [[nodiscard]] std::vector<std::string> staleIds() const;
  /// Why the last scheduled or recompute run of one of `id`'s bindings failed (a
  /// script error, or a compute that threw); empty once a run succeeds. The failed run
  /// published nothing, so the last set stays drawn and the generator stays stale.
  [[nodiscard]] std::string errorOf(std::string_view id) const;

  /// `[first, last]` raw ns staged by the most recent published run of `id`'s first
  /// binding, after the window and the store's retention. `nullopt` before any run,
  /// or when that run staged no sample. A failed run keeps the previous value.
  [[nodiscard]] std::optional<std::pair<Timestamp, Timestamp>> coverageOf(std::string_view id) const;

  /// Test-only: called with the current thread once per staged contribution a run
  /// computes. Set it before the runs it observes and clear it after they finish.
  static void setComputeThreadProbeForTest(std::function<void(QThread*)> probe);

  /// The catalog grew (a dataset or a topic was published): marks every
  /// `all_datasets` recipe stale, since the newcomer may contribute — the commit-time
  /// recompute runs before the catalog rebuild, so it cannot see it. The brake runs
  /// them, so a burst of additions (a progressive load, a stream announcing topics)
  /// costs one run.
  void reachNewDatasets();

  /// Every loaded dataset as the injected lister reports it (empty without one).
  [[nodiscard]] std::vector<DatasetId> loadedDatasets() const;

  /// Refuses a script language markers do not run: anything but "luau" (empty means luau). The one guard of the
  /// validate, the upsert and the plugin bridge's script check.
  [[nodiscard]] static Status checkLanguage(std::string_view language);

  /// Compile-only validation for `kind`/`language` (drives the editor red/green dot):
  /// rejects a non-"luau" language, else compiles + module-loads the script WITHOUT
  /// running it (no inputs, no side effects). Runtime errors are NOT caught here.
  [[nodiscard]] Status validateScript(GeneratorKind kind, std::string_view language, const std::string& script) const;

  /// How long a create holds its caller for the first run. The caller chooses: the
  /// bounded wait saves the GUI thread only where nobody reads the result right away.
  enum class CreateWait {
    /// The run computes on the calling thread and is published before the create
    /// returns, so every failure is returned. Plugin creates need it: a released
    /// plugin reads the markers right after `create_data_processor` returns. An undo or
    /// redo (`DerivedObjects::restore` under `kHistory`) uses it too, since the user
    /// expects the previous markers back, not a blank set while the run finishes.
    kToCompletion,
    /// The run computes on the pool while the calling thread waits at most
    /// `kCreateBudget`; a run still going then carries on as the binding's scheduled
    /// run. Without room in the in-flight budget at once the rule is installed and its
    /// first run waits in the budget's line like a refresh, publishing late too. Without
    /// a pool it behaves as `kToCompletion`. A layout load uses it
    /// (`DerivedObjects::restore` under `kReplace`, the progressive drain included):
    /// nobody reads a rule's markers right away, and a slow one draws progressively.
    kBounded,
  };

  /// Create or replace (by `id`) a generator and run it. Returns the generator's own
  /// object topic (`markerOwnerTopicName`), the physical name a plugin can read back.
  /// The script is compiled and its inputs staged on the calling (GUI) thread, then
  /// the first run computes as `wait` says. When it finished before this returns, a
  /// failure (a compile or script error, or no input in any dataset) returns the
  /// message and leaves the prior output AND recipe untouched, and a success is
  /// published. When a `kBounded` run is still going, the recipe is installed and this
  /// returns; the run publishes through the pool like a scheduled one (`isRunning`,
  /// `markersPublished`), and its failure is reported there (`errorOf`, the diagnostic
  /// sink), the prior markers kept. A `declared_inputs` length mismatch, an empty id, and
  /// an id or output key that is not UTF-8 or contains `#` are rejected.
  /// An id's `ephemeral`/`history_exempt` flags are homogeneous across its bindings (a
  /// disagreeing binding is rejected before anything runs), and a recipe whose
  /// `all_datasets` differs from the id's bindings REPLACES the whole id, so no id ever
  /// owns both a Dataset-scope and a Global output.
  [[nodiscard]] Expected<std::vector<std::string>> upsertGenerator(
      GeneratorRecipe recipe, CreateWait wait = CreateWait::kToCompletion);

  /// Retire the rule on every dataset it is bound to (persistent or ephemeral; see
  /// invariant 2). For an ephemeral preview the preview object topic is removed.
  /// Unknown `id` is an error.
  Status removeGenerator(std::string_view id);

  /// Marks stale every generator whose input is affected by a change and arms the
  /// brake; nothing runs here. An input is affected when one of its series keys is
  /// rooted at a `changed` topic (the topic itself or a field below a slash: `imu`
  /// does not affect `imu_raw/x`); an EMPTY `changed` affects ALL. Returns the
  /// affected generator ids. A re-run whose script errors keeps the last set, and
  /// an `all_datasets` one left without any contributor is emptied.
  ///
  /// `scope` bounds the work to the dataset that actually ingested; `kAnyDataset`
  /// means the change cannot be attributed to one. It matters because `changed`
  /// carries bare topic NAMES, which repeat across datasets: unscoped, a stream tick
  /// on one dataset marks generators bound to another, and every `all_datasets`
  /// generator then re-runs over EVERY loaded dataset — including a large file that
  /// did not change and has no retention bounding it. There is deliberately no default:
  /// the unbounded branch should be chosen, not inherited.
  [[nodiscard]] std::vector<std::string> recomputeForChangedInputs(
      const std::vector<std::string>& changed, DatasetId scope);

  /// Re-run every generator bound to `dataset` (plus every `all_datasets` generator,
  /// on `dataset` only), re-publishing its output — the whole-dataset recompute a
  /// reload/replace needs, where matching by changed-input name misses generators
  /// whose inputs vanished or were renamed by the swap. Cancels the runs in flight on
  /// those bindings. Synchronous: its callers (`RefillGuard`'s commit and rollback,
  /// `SessionManager::replaceDataset`) repaint from the topics it returns, and a
  /// rollback must leave no marker computed from the data it rolled back once it
  /// returns. A run that fails is recorded like a scheduled one (`errorOf`, the
  /// diagnostic sink, stale). Returns the affected marker object-topic names.
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

  /// `recipes().size()` without the copies.
  [[nodiscard]] std::size_t persistentBindingCount() const;

  /// Every PERSISTENT generator id, each once regardless of how many datasets it
  /// is bound to — the plugin ABI's `list()` vocabulary.
  [[nodiscard]] std::vector<std::string> generatorIds() const;

  /// `id`'s first-applied binding (the one the plugin ABI's `config()` reports; a
  /// replacement on that dataset keeps the slot), or null for an unknown id.
  [[nodiscard]] const GeneratorRecipe* firstBinding(std::string_view id) const;

  /// Output names of persistent history-exempt generators reading a series rooted
  /// at one of `removed_inputs` (the topic itself or a slash-bounded field below it).
  [[nodiscard]] std::vector<std::string> exemptDependentsOf(const std::vector<std::string>& removed_inputs) const;

  /// Ids of the live generators a restore to `wanted` (a snapshot's recipes, dataset
  /// identities resolved) may leave as they are, so they keep their object topic, their
  /// published set, their run state and their coverage. Per id: each persistent binding
  /// equals the wanted binding on the same dataset (`bindingDataset`) on every field a
  /// layout carries (script and params compared ignoring line endings), and neither
  /// side has another binding. Never listed: an id outside the intent's authority (an
  /// exempt one under `kHistory`, which no history restore touches anyway); one reading
  /// a series rooted at a name in `removed_upstream` (the outputs the restore removes or
  /// creates, so the published set describes data that is gone); an `all_datasets` one
  /// whose last published run used a contributor's display offset that has moved since
  /// (invariant 3: the set is stored in the display frame), that has a run in flight
  /// (it staged under offsets that may have moved since; re-creating it cancels the
  /// run), or that never published. A rule whose last run failed is kept like any other:
  /// its error, drawn markers and coverage stay as they were.
  [[nodiscard]] std::unordered_set<std::string> unchangedGeneratorIds(
      const std::vector<GeneratorRecipe>& wanted, RestoreIntent intent,
      const std::vector<std::string>& removed_upstream) const;

  /// Remove every PERSISTENT generator (tombstoning its output) except the ids in
  /// `keep`; ephemeral previews are always left untouched. `RestoreIntent::kHistory`
  /// reconciles only the non-exempt generators and leaves the `history_exempt` ones
  /// live; `kReplace` removes all.
  void clearGenerators(RestoreIntent intent, const std::unordered_set<std::string>& keep = {});
  /// Whether any live generator is `history_exempt` (cheap gate for the
  /// history-restore dependency preflight).
  [[nodiscard]] bool hasHistoryExemptGenerators() const;

  /// `clearGenerators(RestoreIntent::kReplace)` — every PERSISTENT
  /// generator at once; ephemeral previews are left untouched (mirrors
  /// DataProcessorService::clearAllTransforms).
  void clearAllGenerators();

  /// The prefix of every generator id `owner_plugin` creates (`"<owner_plugin>/<local id>"`):
  /// the namespace `MarkersRuntimeHost` mints ids in.
  [[nodiscard]] static std::string ownerPrefix(std::string_view owner_plugin);

  /// Remove the EPHEMERAL (preview) generators among `ids` and their preview topics;
  /// an id that is gone or persistent is skipped. A preview belongs to the plugin
  /// instance that made it: `DerivedRuntimeHosts` passes the ones its bridge created
  /// when the instance ends. Persistent generators are the user's data. Returns
  /// whether anything was removed.
  bool removeEphemerals(const std::unordered_set<std::string>& ids);

  /// Cancels the runs in flight of every binding that reads `dataset` (bound to it, or
  /// `all_datasets`): none of them is published, and their scheduling state is
  /// forgotten. For a change of meaning that is not an edit: a reload starting, a merge.
  void cancelRunsOn(DatasetId dataset);

  /// Rebind every generator bound to one of the `consumed` datasets onto `anchor`.
  /// A merge folds the sources' data into the anchor and drops them from the catalog,
  /// so a recipe still naming a source would resolve zero inputs from then on — the
  /// user's rule stops producing without any error. `all_datasets` generators are
  /// untouched (they resolve through the dataset lister, not a stored id). When the
  /// anchor already holds its own binding of the same id, that binding wins and the
  /// consumed one is retired: `mergeMarkerTopics` (which runs first) already moved
  /// the consumed output onto the anchor's topic of this id, and the anchor's next
  /// recompute replaces it with a run over the merged data. Runs staged before the
  /// merge are cancelled (they would publish over that concatenation), and every
  /// binding of the anchor and every `all_datasets` one is marked stale.
  void remapGeneratorsToAnchor(DatasetId anchor, const std::vector<DatasetId>& consumed);

  /// Forget every generator bound to a removed `dataset` (its topics went with it —
  /// a surviving recipe would otherwise re-register a phantom topic under the dead
  /// id on the next recompute); `all_datasets` generators are kept and their shared
  /// set rebuilt from the surviving contributors — emptied when none is left, the
  /// rule staying live. Returns the republished object topics.
  std::vector<std::string> clearGeneratorsForDataset(DatasetId dataset);

 signals:
  /// Scheduled runs (the brake, `refresh`, or a create's first run past its budget)
  /// published their sets on the GUI thread. Queued, once per burst: the runs one
  /// pool drain publishes announce together. The synchronous paths do not emit it:
  /// their callers notify.
  void markersPublished();
  /// `runningIds()` or an `errorOf` answer may have changed (a failure recorded, or cleared by a success, a
  /// removal or a re-apply). Queued and coalesced like `markersPublished`.
  void runningChanged();

 private:
  /// The bindings of one id, in the order they were applied (a replacement keeps
  /// its slot). Never empty while the id is in `recipes_`.
  using GeneratorBindings = std::vector<GeneratorRecipe>;

  /// The dataset a binding is keyed on: a producer's id carries the rule's SCOPE
  /// but not its dataset, so the same dataset-bound id on a second dataset is a
  /// new binding while re-applying it on the SAME dataset replaces. `all_datasets`
  /// and `ephemeral` recipes are pinned to `kAnyDataset` (one per id).
  [[nodiscard]] static DatasetId bindingDataset(const GeneratorRecipe& recipe);

  /// Every binding of every id, flattened (id order, then application order).
  [[nodiscard]] std::vector<std::reference_wrapper<const GeneratorRecipe>> allRecipes() const;

  /// The one place a recipe publishes to: the dataset and its OWN object-topic name.
  using PublishTarget = std::pair<DatasetId, std::string>;

  /// The metadata `recipe`'s topic is registered with: the markers type, the scope and
  /// target its output key means (`markerInfoForKey`), and the recipe's id as owner.
  [[nodiscard]] static std::string topicMetadata(const GeneratorRecipe& recipe);

  /// `recipe`'s target: (`dataset_id`, `<key>#<id>`) when bound,
  /// (`kAllDatasetsMarkerDataset`, …) when `all_datasets`. Requires an output
  /// (`upsertGenerator` guarantees one on every stored binding).
  [[nodiscard]] static PublishTarget publishTarget(const GeneratorRecipe& recipe);

  /// Retire `target`: tombstone it (persistent) or remove the topic outright
  /// (`ephemeral`). A no-op when it equals `keep` — the target the new revision of
  /// the same binding just rewrote.
  void retireTarget(const PublishTarget& target, bool ephemeral, const PublishTarget* keep = nullptr);

  /// `retireTarget` every marker topic owned by `id` (its metadata), `keep` excepted. A store
  /// scan rather than the recipe's targets: a merge moves a consumed binding's
  /// output onto the anchor and drops the binding, so no recipe names it anymore.
  void retireOwnedTopics(std::string_view id, bool ephemeral, const PublishTarget* keep = nullptr);

  /// One dataset's inputs for one run. `display_shift` is 0 for a bound recipe and
  /// `-displayOffsetOf(dataset_id)` for an `all_datasets` one (invariant 3).
  struct StagedContribution {
    DatasetId dataset_id = 0;
    std::unordered_map<std::string, ResolvedSeries> inputs;
    Timestamp display_shift = 0;
  };

  /// Everything a run reads, captured on the GUI thread: a copy of the recipe and one
  /// contribution per dataset — exactly one for a bound recipe (a missing input there
  /// is a script error, not a skip), one per dataset carrying any input for an
  /// `all_datasets` one.
  struct StagedRun {
    GeneratorRecipe recipe;
    std::vector<StagedContribution> contributions;
    /// `[first, last]` raw ns across every staged sample; `nullopt` when none.
    std::optional<std::pair<Timestamp, Timestamp>> coverage_ns;
    /// The staged samples' bytes (timestamps and values; no vector slack): what the in-flight budget charges for
    /// the copy. One step per staged series, none per sample.
    [[nodiscard]] std::size_t bytes() const;
  };

  /// GUI thread: resolves and window-clips `recipe`'s inputs.
  [[nodiscard]] StagedRun stageRun(const GeneratorRecipe& recipe) const;

  /// Any thread: runs the script over every contribution and unions the results,
  /// moving the staged series out (`staged` keeps its recipe and coverage).
  /// `nullopt` when no contribution was staged. Touches no engine or store state.
  [[nodiscard]] static Expected<std::optional<sdk::PlotMarkers>> computeRun(
      StagedRun& staged, const EnginePool::CancelToken* cancel);

  /// GUI thread: publishes `computed` to `staged.recipe`'s target, or tombstones it
  /// when `nullopt` (an `all_datasets` rule without contributors), and records the
  /// staged range (`coverageOf`) and each contribution's display shift as the
  /// binding's last published run once that succeeded.
  [[nodiscard]] Status publishRun(const StagedRun& staged, std::optional<sdk::PlotMarkers> computed);

  /// The whole-dataset recompute of one binding, synchronously on the calling thread:
  /// cancels its run in flight, then stages, computes and publishes, appending its
  /// topic to `affected`. An `all_datasets` rule left without any contributor is
  /// emptied (tombstoned), not kept stale — only the initial upsert treats that as an
  /// error. A failure keeps the last set and is recorded (`recordFailure`).
  void rerunNow(const GeneratorRecipe& recipe, std::vector<std::string>& affected);

  [[nodiscard]] Timestamp displayOffsetOf(DatasetId dataset) const;

  /// Find-or-register `object_topic_name` on `dataset_id` (capping retention at one
  /// snapshot — markers are republished whole, last-writer-wins at a sentinel
  /// timestamp) and publish `set`. A topic registered here carries `metadata_json`;
  /// one that already exists keeps its own, and must describe the same marker set
  /// (scope, target, owner) or the publish fails and the topic is left untouched.
  /// The single publish path for run + merge.
  [[nodiscard]] Status publishMarkerSet(
      DatasetId dataset_id, const std::string& object_topic_name, const std::string& metadata_json,
      const sdk::PlotMarkers& set);

  /// Republish an EMPTY set to `object_topic_name` on `dataset_id` (the removal
  /// tombstone): the topic survives with its id + retention budget, but draws
  /// nothing. No-op if the topic was never published (never registers a new topic
  /// on remove). Pairs with the codec's empty-buffer→empty-set decode.
  void publishEmptyMarkers(DatasetId dataset_id, const std::string& object_topic_name);

  /// The set stored on `topic_id` shifted by `shift` plus any pending
  /// `payload_stamp_shift`; empty for a tombstone, a never-published topic or
  /// undecodable bytes.
  [[nodiscard]] sdk::PlotMarkers storedMarkers(ObjectTopicId topic_id, Timestamp shift) const;

  /// A binding's identity: its id and `bindingDataset`.
  using BindingKey = std::pair<std::string, DatasetId>;
  [[nodiscard]] static BindingKey bindingKey(const GeneratorRecipe& recipe);
  /// The stored binding `key` names, or null.
  [[nodiscard]] GeneratorRecipe* bindingOf(const BindingKey& key);

  /// The scheduling state of one binding (D30).
  struct RunState {
    uint64_t change_seq = 0;  ///< bumped by every input change
    bool stale = false;
    bool pending = false;  ///< a `refresh` waiting for the running run; holds no inputs
    /// Set while a run is in flight: its pool token, or a default one (never cancelled) while it runs inline or
    /// waits for room.
    std::optional<EnginePool::CancelToken> running;
    InFlightBudget::Ticket room;  ///< its place in the budget's line while it waits for room
    /// Names the run being staged, so it is recognised once the resolver returned; 0 once dropped.
    uint64_t admission = 0;
    std::string error;  ///< the last run's failure; empty after a success
    /// The `change_seq` the last failed run staged: the brake skips the binding until
    /// its inputs change again, so a broken rule does not re-run on every pass.
    std::optional<uint64_t> failed_seq;
  };

  /// Bumps `recipe`'s binding's change sequence and marks it stale (the brake's input).
  void markStale(const GeneratorRecipe& recipe);
  /// What a pool run's compute hands back (defined in the .cpp).
  struct RunResult;
  /// Submits `staged`'s compute to the pool. Its publish calls `finishRun` unless the
  /// run was cancelled or the create path already consumed `result`.
  EnginePool::CancelToken startRun(
      const BindingKey& key, std::shared_ptr<StagedRun> staged, uint64_t staged_seq, std::shared_ptr<RunResult> result);
  /// Starts a run of `recipe`'s binding: stage now, compute on the pool (or inline),
  /// publish in `finishRun`. With a run in flight it only sets `pending`. With a budget,
  /// the run first waits in its line for `kMarkerRunBytes` (`setInFlightBudget`).
  void submitRun(const GeneratorRecipe& recipe);
  /// Stages `recipe`'s run and starts it on the pool with `room` grown by the staged copy; a copy that does not fit
  /// keeps the run waiting, staged, at the head of the budget's line. A run the resolver cancelled, removed or
  /// replaced while it staged (or whose pool or budget it changed) starts nothing.
  void stageAndStart(
      const BindingKey& key, const GeneratorRecipe& recipe, std::optional<InFlightBudget::Reservation> room);
  /// Cancels every run in flight, waiting and staging ones included, and forgets the pending refreshes: the pool or
  /// the budget they would start on changed.
  void dropRunsInFlight();
  /// GUI thread: publishes a completed, uncancelled run and starts what waits.
  void finishRun(
      const BindingKey& key, const StagedRun& staged, uint64_t staged_seq,
      Expected<std::optional<sdk::PlotMarkers>> result);
  /// Records that `key`'s run staged at `staged_seq` failed with `error`: the binding
  /// stays stale and waits for an input change, and the sink hears a new error once.
  void recordFailure(const BindingKey& key, uint64_t staged_seq, const std::string& error);
  /// Whether `recipe` is worth an automatic run (see `DemandPredicate`).
  [[nodiscard]] bool isDemanded(const GeneratorRecipe& recipe) const;
  /// Cancels `key`'s run in flight (its result is never published) and forgets the
  /// binding's scheduling state: its meaning changed.
  void cancelRun(const BindingKey& key);
  /// `cancelRun` on every binding of `id`.
  void cancelRunsOf(std::string_view id);
  /// Automatic mode: arms the brake for what is left of the current period, unless
  /// it is already armed. Manual mode: nothing.
  void scheduleDirty();
  /// Automatic mode: submits every stale, demanded binding that has no run in flight
  /// and did not fail on its current inputs.
  void onBrakeFired();
  /// Emits `signal` from the event loop, once however often this is called before.
  void queueSignal(bool& queued, void (BatchEngine::*signal)());

  ObjectStore& object_store_;
  SeriesResolver resolver_;
  DatasetLister dataset_lister_;
  DisplayOffsetResolver display_offset_resolver_;
  std::map<std::string, GeneratorBindings> recipes_;  ///< id → per-dataset bindings (incl. ephemeral); invariant 2

  EnginePool* pool_ = nullptr;
  InFlightBudget* budget_ = nullptr;
  uint64_t next_admission_ = 0;  ///< the last `RunState::admission` handed out
  DemandPredicate demand_predicate_;
  Mode mode_ = Mode::kAutomatic;
  double reload_hz_ = kDefaultReloadHz;
  QTimer brake_;
  QElapsedTimer since_last_run_;  ///< restarted when the brake fires
  std::map<BindingKey, RunState> runs_;
  /// What a binding's last published run staged.
  struct PublishedRun {
    std::optional<std::pair<Timestamp, Timestamp>> coverage;  ///< `coverageOf`
    /// The display shift each contributor's run used; recorded for `all_datasets` rules only,
    /// whose shared set is stored in the display frame (invariant 3).
    std::vector<std::pair<DatasetId, Timestamp>> display_shifts;
  };
  std::map<BindingKey, PublishedRun> published_;
  /// A `markersPublished` / `runningChanged` is queued and not yet delivered.
  bool markers_published_queued_ = false;
  bool running_changed_queued_ = false;
  DiagnosticSink diagnostic_sink_;
};

/// Copy a stored scalar series (retention floor applied, null rows skipped) into the
/// resolver's `ResolvedSeries` form: timestamps as ns doubles, in time order (the
/// engine assumes sorted input, which chunk order alone does not guarantee after
/// out-of-order ingest), limited to `window` when set. One O(n) pass — never
/// `sampleAt(i)` in a loop: AppSession's resolver calls this on the GUI thread while
/// a run's inputs are captured.
[[nodiscard]] BatchEngine::ResolvedSeries readResolvedSeries(
    const SeriesReader& series, const BatchEngine::Window& window = std::nullopt);

}  // namespace PJ
