// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/BatchEngine.h"

#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "SeriesKey.h"
#include "TextCompare.h"
#include "pj_base/builtin/plot_markers.hpp"        // sdk::PlotMarker(s), markerObjectTopicName, kPreviewMarkerTopic
#include "pj_base/builtin/plot_markers_codec.hpp"  // serializePlotMarkers, deserializePlotMarkers
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/query.hpp"
#include "pj_runtime/InFlightBudget.h"
#include "pj_runtime/MarkerTopics.h"     // isMarkerTopic, markerOwnerTopicName, kAllDatasetsMarkerDataset
#include "pj_scripting/marker_engine.h"  // scripting::SeriesView/SeriesProvider, runMarkerScript

namespace PJ {

namespace {

Q_LOGGING_CATEGORY(lcBatchEngine, "pj.runtime.batch_engine")

constexpr const char* kModeKey = "Preferences::derived_batch_mode";
constexpr const char* kReloadHzKey = "Preferences::derived_batch_reload_hz";
constexpr const char* kManualMode = "manual";
constexpr const char* kAutomaticMode = "automatic";

/// Clips `series` in place to `[start, end]` (raw ns, inclusive); its timestamps are
/// sorted. An empty result is valid. A safety net: the resolver already copied only
/// the window, unless it ignores it.
void clipToWindow(BatchEngine::ResolvedSeries& series, Timestamp start, Timestamp end) {
  const auto begin = series.timestamps.begin();
  const auto lower = std::distance(begin, std::lower_bound(begin, series.timestamps.end(), static_cast<double>(start)));
  const auto upper = std::distance(begin, std::upper_bound(begin, series.timestamps.end(), static_cast<double>(end)));
  series.timestamps.erase(begin + upper, series.timestamps.end());
  series.timestamps.erase(series.timestamps.begin(), series.timestamps.begin() + lower);
  series.values.erase(series.values.begin() + upper, series.values.end());
  series.values.erase(series.values.begin(), series.values.begin() + lower);
}

/// Widens `range` to cover the first and last sample of every series in `inputs`.
void widenCoverage(
    std::optional<std::pair<Timestamp, Timestamp>>& range,
    const std::unordered_map<std::string, BatchEngine::ResolvedSeries>& inputs) {
  for (const auto& [key, series] : inputs) {
    if (series.timestamps.empty()) {
      continue;
    }
    const auto first = static_cast<Timestamp>(series.timestamps.front());
    const auto last = static_cast<Timestamp>(series.timestamps.back());
    range = range.has_value() ? std::pair{std::min(range->first, first), std::max(range->second, last)}
                              : std::pair{first, last};
  }
}

/// The hook `setComputeThreadProbeForTest` installs.
std::function<void(QThread*)>& computeThreadProbe() {
  static std::function<void(QThread*)> probe;
  return probe;
}

/// `kCreateBudget`, unless `setCreateBudgetForTest` replaced it.
std::chrono::milliseconds& createBudget() {
  static std::chrono::milliseconds budget = BatchEngine::kCreateBudget;
  return budget;
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

/// Shift every marker's [t_start, t_end] window by `shift` — the rebase a marker set
/// needs when it moves onto another dataset's clock. kValueBand markers ignore
/// t_start/t_end, so shifting them is a harmless no-op for that kind.
void shiftMarkers(std::vector<sdk::PlotMarker>& markers, Timestamp shift) {
  for (sdk::PlotMarker& marker : markers) {
    marker.t_start += shift;
    marker.t_end += shift;
  }
}

/// Whether `recipe` reads a series rooted at one of the topics `names`.
bool readsAnyTopicOf(const BatchEngine::GeneratorRecipe& recipe, const std::vector<std::string>& names) {
  return std::any_of(recipe.inputs.begin(), recipe.inputs.end(), [&names](const std::string& input) {
    return std::any_of(
        names.begin(), names.end(), [&input](const std::string& name) { return seriesKeyBelongsToTopic(input, name); });
  });
}

/// Whether a restore under `intent` has authority over the id whose first binding is
/// `first`: previews are never the restore's, and `kHistory` leaves the exempt ones.
/// `ephemeral` / `history_exempt` are per-id properties, so the first binding decides.
bool inAuthority(const BatchEngine::GeneratorRecipe& first, RestoreIntent intent) {
  return !first.ephemeral && !(intent == RestoreIntent::kHistory && first.history_exempt);
}

}  // namespace

struct BatchEngine::RunResult {
  std::mutex mutex;
  std::condition_variable finished_cv;
  bool finished = false;  ///< the compute returned or threw: `value` is final
  bool consumed = false;  ///< GUI thread: the create path took `value`; the publish drops it
  Expected<std::optional<sdk::PlotMarkers>> value = unexpected(std::string("not run"));
  /// GUI thread: the run's room in the in-flight budget, returned at its publication (or with the pool's closures).
  std::optional<InFlightBudget::Reservation> room;
};

BatchEngine::BatchEngine(ObjectStore& object_store, SeriesResolver resolver)
    : object_store_(object_store), resolver_(std::move(resolver)) {
  brake_.setSingleShot(true);
  connect(&brake_, &QTimer::timeout, this, &BatchEngine::onBrakeFired);
  since_last_run_.start();
}

BatchEngine::~BatchEngine() {
  for (const auto& [key, state] : runs_) {
    if (state.running.has_value()) {
      state.running->cancel();
    }
  }
}

void BatchEngine::setResolver(SeriesResolver resolver) {
  resolver_ = std::move(resolver);
}

void BatchEngine::setDatasetLister(DatasetLister lister) {
  dataset_lister_ = std::move(lister);
}

void BatchEngine::setDisplayOffsetResolver(DisplayOffsetResolver resolver) {
  display_offset_resolver_ = std::move(resolver);
}

void BatchEngine::setPool(EnginePool* pool) {
  if (pool != pool_) {
    dropRunsInFlight();
    pool_ = pool;
  }
}

const std::size_t BatchEngine::kMarkerRunBytes = scripting::BudgetLimits{}.mem_bytes + scripting::kMaxMarkerOutputBytes;

void BatchEngine::setInFlightBudget(InFlightBudget* budget) {
  if (budget != budget_) {
    dropRunsInFlight();
    budget_ = budget;
  }
}

void BatchEngine::dropRunsInFlight() {
  bool cancelled = false;
  for (auto& [key, state] : runs_) {
    if (state.running.has_value()) {
      state.running->cancel();
      state.running.reset();
      cancelled = true;
    }
    state.pending = false;
    state.room = {};      // a waiting run would start on what changed
    state.admission = 0;  // so would one being staged
  }
  if (cancelled) {
    queueSignal(running_changed_queued_, &BatchEngine::runningChanged);
  }
}

std::size_t BatchEngine::StagedRun::bytes() const {
  std::size_t total = 0;
  for (const StagedContribution& contribution : contributions) {
    for (const auto& [key, series] : contribution.inputs) {
      total += (series.timestamps.size() + series.values.size()) * sizeof(double);
    }
  }
  return total;
}

void BatchEngine::setDemandPredicate(DemandPredicate predicate) {
  demand_predicate_ = std::move(predicate);
}

void BatchEngine::demandChanged() {
  scheduleDirty();
}

void BatchEngine::setMode(Mode mode) {
  mode_ = mode;
  if (mode_ == Mode::kManual) {
    brake_.stop();
  }
  scheduleDirty();
}

void BatchEngine::setReloadHz(double hz) {
  reload_hz_ = std::clamp(hz, kMinReloadHz, kMaxReloadHz);
}

BatchEngine::Settings BatchEngine::loadSettings() {
  QSettings store;
  Settings loaded;
  if (store.value(QString::fromLatin1(kModeKey)).toString() == QLatin1String(kManualMode)) {
    loaded.mode = Mode::kManual;
  }
  const double hz = store.value(QString::fromLatin1(kReloadHzKey), kDefaultReloadHz).toDouble();
  loaded.reload_hz = std::isfinite(hz) ? std::clamp(hz, kMinReloadHz, kMaxReloadHz) : kDefaultReloadHz;
  return loaded;
}

void BatchEngine::saveSettings(const Settings& settings) {
  QSettings store;
  store.setValue(
      QString::fromLatin1(kModeKey),
      QString::fromLatin1(settings.mode == Mode::kManual ? kManualMode : kAutomaticMode));
  store.setValue(QString::fromLatin1(kReloadHzKey), settings.reload_hz);
}

void BatchEngine::setSettings(const Settings& settings) {
  setReloadHz(settings.reload_hz);
  setMode(settings.mode);
}

std::chrono::milliseconds BatchEngine::setCreateBudgetForTest(std::chrono::milliseconds budget) {
  return std::exchange(createBudget(), budget);
}

void BatchEngine::setDiagnosticSink(DiagnosticSink sink) {
  diagnostic_sink_ = std::move(sink);
}

bool BatchEngine::isRunning(std::string_view id) const {
  return std::any_of(runs_.begin(), runs_.end(), [id](const auto& entry) {
    return entry.first.first == id && entry.second.running.has_value();
  });
}

bool BatchEngine::isBindingRunning(const GeneratorRecipe& binding) const {
  const auto it = runs_.find(bindingKey(binding));
  return it != runs_.end() && it->second.running.has_value();
}

std::string BatchEngine::bindingError(const GeneratorRecipe& binding) const {
  const auto it = runs_.find(bindingKey(binding));
  return it == runs_.end() ? std::string{} : it->second.error;
}

std::vector<std::string> BatchEngine::runningIds() const {
  std::set<std::string> ids;
  for (const auto& [key, state] : runs_) {
    if (state.running.has_value()) {
      ids.insert(key.first);
    }
  }
  return {ids.begin(), ids.end()};
}

void BatchEngine::refresh(std::string_view id) {
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (recipe.id == id) {
      submitRun(recipe);
    }
  }
}

void BatchEngine::refreshAll(DatasetId dataset) {
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (recipe.all_datasets || recipe.dataset_id == dataset) {
      submitRun(recipe);
    }
  }
}

bool BatchEngine::isStale(std::string_view id) const {
  return std::any_of(
      runs_.begin(), runs_.end(), [id](const auto& entry) { return entry.first.first == id && entry.second.stale; });
}

std::vector<std::string> BatchEngine::staleIds() const {
  std::set<std::string> ids;
  for (const auto& [key, state] : runs_) {
    if (state.stale) {
      ids.insert(key.first);
    }
  }
  return {ids.begin(), ids.end()};
}

std::string BatchEngine::errorOf(std::string_view id) const {
  for (const auto& [key, state] : runs_) {
    if (key.first == id && !state.error.empty()) {
      return state.error;
    }
  }
  return {};
}

std::optional<std::pair<Timestamp, Timestamp>> BatchEngine::coverageOf(std::string_view id) const {
  const GeneratorRecipe* recipe = firstBinding(id);
  if (recipe == nullptr) {
    return std::nullopt;
  }
  const auto it = published_.find(bindingKey(*recipe));
  return it == published_.end() ? std::nullopt : it->second.coverage;
}

void BatchEngine::setComputeThreadProbeForTest(std::function<void(QThread*)> probe) {
  computeThreadProbe() = std::move(probe);
}

Timestamp BatchEngine::displayOffsetOf(DatasetId dataset) const {
  return display_offset_resolver_ ? display_offset_resolver_(dataset) : Timestamp{0};
}

void BatchEngine::reachNewDatasets() {
  bool marked = false;
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (recipe.all_datasets) {
      markStale(recipe);
      marked = true;
    }
  }
  if (marked) {
    scheduleDirty();
  }
}

std::vector<DatasetId> BatchEngine::loadedDatasets() const {
  return dataset_lister_ ? dataset_lister_() : std::vector<DatasetId>{};
}

Status BatchEngine::checkLanguage(std::string_view language) {
  if (!language.empty() && language != "luau") {
    return unexpected("unsupported script language '" + std::string(language) + "' (only 'luau')");
  }
  return okStatus();
}

Status BatchEngine::validateScript(GeneratorKind /*kind*/, std::string_view language, const std::string& script) const {
  if (Status s = checkLanguage(language); !s.has_value()) {
    return s;
  }
  std::string err;
  if (!scripting::validateScript(script, &err)) {
    return unexpected(err);
  }
  return okStatus();
}

// ---- kind=markers: stage (GUI) / compute (worker) / publish (GUI) -----------

BatchEngine::StagedRun BatchEngine::stageRun(const GeneratorRecipe& recipe) const {
  StagedRun staged;
  staged.recipe = recipe;
  // The resolver may re-enter this engine and replace the binding `recipe` names: read the copy from here on.
  const GeneratorRecipe& copy = staged.recipe;
  const auto stage = [&](DatasetId dataset_id) {
    StagedContribution contribution;
    contribution.dataset_id = dataset_id;
    for (const std::string& key : copy.inputs) {
      if (contribution.inputs.contains(key) || !resolver_) {
        continue;
      }
      if (std::optional<ResolvedSeries> series = resolver_(dataset_id, key, copy.window)) {
        if (copy.window.has_value()) {
          clipToWindow(*series, copy.window->first, copy.window->second);
        }
        contribution.inputs.emplace(key, std::move(*series));
      }
    }
    return contribution;
  };
  if (!copy.all_datasets) {
    staged.contributions.push_back(stage(copy.dataset_id));
  } else {
    for (const DatasetId dataset_id : loadedDatasets()) {
      StagedContribution contribution = stage(dataset_id);
      if (!copy.inputs.empty() && contribution.inputs.empty()) {
        continue;  // not one of this rule's datasets
      }
      contribution.display_shift = -displayOffsetOf(dataset_id);
      staged.contributions.push_back(std::move(contribution));
    }
  }
  for (const StagedContribution& contribution : staged.contributions) {
    widenCoverage(staged.coverage_ns, contribution.inputs);
  }
  return staged;
}

Expected<std::optional<sdk::PlotMarkers>> BatchEngine::computeRun(
    StagedRun& staged, const EnginePool::CancelToken* cancel) {
  // Any script error aborts before publishing: a partial union would silently
  // misrepresent the rule on every dataset it is drawn on.
  if (staged.contributions.empty()) {
    return std::optional<sdk::PlotMarkers>{};
  }
  sdk::PlotMarkers shared;
  const auto cancelled = [cancel] { return cancel != nullptr && cancel->cancelled(); };
  std::size_t output_left = scripting::kMaxMarkerOutputBytes;  // one ceiling for the whole run (kMarkerRunBytes)
  for (StagedContribution& contribution : staged.contributions) {
    if (cancelled()) {
      return unexpected("cancelled");
    }
    if (const auto& probe = computeThreadProbe(); probe) {
      probe(QThread::currentThread());
    }
    std::unordered_map<std::string, scripting::SeriesView> views;
    for (auto& [key, series] : contribution.inputs) {
      views.emplace(key, scripting::SeriesView{std::move(series.timestamps), std::move(series.values)});
    }
    const scripting::SeriesProvider provider =
        buildProvider(views, staged.recipe.inputs, staged.recipe.declared_inputs);
    std::string err;
    std::size_t output_bytes = 0;
    std::vector<sdk::PlotMarker> markers =
        scripting::runMarkerScript(staged.recipe.script, provider, &err, {}, cancelled, output_left, &output_bytes);
    if (!err.empty()) {
      return unexpected(err);
    }
    output_left -= output_bytes;
    shiftMarkers(markers, contribution.display_shift);
    shared.markers.insert(
        shared.markers.end(), std::make_move_iterator(markers.begin()), std::make_move_iterator(markers.end()));
  }
  return std::optional<sdk::PlotMarkers>{std::move(shared)};
}

Status BatchEngine::publishRun(const StagedRun& staged, std::optional<sdk::PlotMarkers> computed) {
  const PublishTarget target = publishTarget(staged.recipe);
  if (!computed.has_value()) {
    publishEmptyMarkers(target.first, target.second);
  } else if (
      Status pushed = publishMarkerSet(target.first, target.second, topicMetadata(staged.recipe), *computed);
      !pushed.has_value()) {
    return pushed;
  }
  PublishedRun& published = published_[bindingKey(staged.recipe)];
  published.coverage = staged.coverage_ns;
  published.display_shifts.clear();
  if (staged.recipe.all_datasets) {
    for (const StagedContribution& contribution : staged.contributions) {
      published.display_shifts.emplace_back(contribution.dataset_id, contribution.display_shift);
    }
  }
  return okStatus();
}

void BatchEngine::rerunNow(const GeneratorRecipe& recipe, std::vector<std::string>& affected) {
  const BindingKey key = bindingKey(recipe);
  // Kept across the cancel, so the same failure again is not reported twice.
  const auto previous = runs_.find(key);
  std::string previous_error = previous == runs_.end() ? std::string{} : previous->second.error;
  cancelRun(key);
  StagedRun staged = stageRun(recipe);
  Expected<std::optional<sdk::PlotMarkers>> computed = computeRun(staged, /*cancel=*/nullptr);
  const Status published =
      computed.has_value() ? publishRun(staged, std::move(*computed)) : Status(unexpected(computed.error()));
  if (published.has_value()) {
    affected.push_back(publishTarget(recipe).second);
    return;
  }
  RunState& state = runs_[key];
  state.error = std::move(previous_error);
  recordFailure(key, state.change_seq, published.error());
}

// ---- scheduled runs (D30) ---------------------------------------------------

void BatchEngine::markStale(const GeneratorRecipe& recipe) {
  RunState& state = runs_[bindingKey(recipe)];
  ++state.change_seq;
  state.stale = true;
}

void BatchEngine::submitRun(const GeneratorRecipe& recipe) {
  const BindingKey key = bindingKey(recipe);
  RunState& state = runs_[key];
  if (state.running.has_value()) {
    state.pending = true;  // the one pending request: it captures when the run ends
    return;
  }
  state.stale = true;
  queueSignal(running_changed_queued_, &BatchEngine::runningChanged);
  if (pool_ == nullptr) {
    const uint64_t staged_seq = state.change_seq;
    StagedRun staged = stageRun(recipe);
    state.running = EnginePool::CancelToken{};  // inline: never cancelled
    finishRun(key, staged, staged_seq, computeRun(staged, /*cancel=*/nullptr));
    return;
  }
  std::optional<InFlightBudget::Reservation> room;
  if (budget_ != nullptr && !(room = budget_->tryReserve(kMarkerRunBytes))) {
    // Nothing is staged before the run has its room. The ticket dies with the binding's state, so the grant never
    // finds this engine or the binding gone.
    state.running = EnginePool::CancelToken{};  // in flight, waiting
    state.room = budget_->whenRoom(kMarkerRunBytes, [this, key](InFlightBudget::Reservation granted) {
      if (const GeneratorRecipe* binding = bindingOf(key)) {
        stageAndStart(key, *binding, std::move(granted));
      }
    });
    return;
  }
  stageAndStart(key, recipe, std::move(room));
}

void BatchEngine::stageAndStart(
    const BindingKey& key, const GeneratorRecipe& recipe, std::optional<InFlightBudget::Reservation> room) {
  uint64_t admission = 0;
  uint64_t staged_seq = 0;
  {
    RunState& staging = runs_[key];
    staging.running = EnginePool::CancelToken{};  // in flight while it stages: a re-entrant refresh only waits
    staging.admission = admission = ++next_admission_;
    staged_seq = staging.change_seq;
  }
  auto staged = std::make_shared<StagedRun>(stageRun(recipe));
  // The resolver may have re-entered this engine and cancelled, removed or replaced the run, or changed the pool.
  const auto found = runs_.find(key);
  if (found == runs_.end() || found->second.admission != admission) {
    return;  // the copy and the room go with this call
  }
  RunState& state = found->second;
  auto result = std::make_shared<RunResult>();
  const std::size_t copy_bytes = staged->bytes();
  if (room.has_value() && !room->grow(copy_bytes)) {
    // The copy does not fit beside the runs in flight: the run keeps it, gives its room back and waits at the head
    // of the line for room for both. At the head, so no later waiter is let in to stage a copy of its own meanwhile.
    const std::size_t whole = room->bytes() + copy_bytes;
    room.reset();
    state.running = EnginePool::CancelToken{};  // in flight, waiting
    state.room = budget_->whenRoom(
        whole,
        [this, key, staged, staged_seq, result](InFlightBudget::Reservation granted) {
          result->room.emplace(std::move(granted));
          runs_[key].running = startRun(key, staged, staged_seq, result);
        },
        InFlightBudget::Place::kFront);
    return;
  }
  result->room = std::move(room);
  state.running = startRun(key, std::move(staged), staged_seq, std::move(result));
}

EnginePool::CancelToken BatchEngine::startRun(
    const BindingKey& key, std::shared_ptr<StagedRun> staged, uint64_t staged_seq, std::shared_ptr<RunResult> result) {
  using OutcomeStatus = EnginePool::Outcome::Status;
  EnginePool::Task task;
  // The compute catches what the script throws so the create path, which waits on
  // `finished`, learns the outcome too; the pool would only tell the publish.
  task.compute = [staged, result](const EnginePool::CancelToken& token) {
    Expected<std::optional<sdk::PlotMarkers>> value = unexpected(std::string("unknown exception"));
    try {
      value = computeRun(*staged, &token);
    } catch (const std::exception& error) {
      value = unexpected(std::string(error.what()));
    } catch (...) {  // NOLINT(bugprone-empty-catch): `value` already says so
    }
    const std::lock_guard<std::mutex> lock(result->mutex);
    result->value = std::move(value);
    result->finished = true;
    result->finished_cv.notify_all();
  };
  // A cancelled run's canceller already reset the binding; the publish also runs
  // after this engine is gone, hence the guard. The budget outlives the pool.
  task.publish = [guard = QPointer<BatchEngine>(this), budget = budget_, key, staged, staged_seq,
                  result](const EnginePool::Outcome& outcome) {
    result->room.reset();  // whatever the outcome: the run holds nothing any more
    if (guard != nullptr && outcome.status != OutcomeStatus::kCancelled) {
      Expected<std::optional<sdk::PlotMarkers>> value = unexpected(outcome.error);
      bool consumed = false;
      {
        const std::lock_guard<std::mutex> lock(result->mutex);
        consumed = result->consumed;
        if (!consumed && outcome.status != OutcomeStatus::kFailed) {
          value = std::move(result->value);
        }
      }
      if (!consumed) {
        guard->finishRun(key, *staged, staged_seq, std::move(value));
      }
    }
    if (budget != nullptr) {
      budget->drain();  // the waiters of either engine take the room before the next publication
    }
  };
  return pool_->submit(std::move(task));
}

void BatchEngine::finishRun(
    const BindingKey& key, const StagedRun& staged, uint64_t staged_seq,
    Expected<std::optional<sdk::PlotMarkers>> result) {
  const auto it = runs_.find(key);
  if (it == runs_.end()) {
    return;
  }
  RunState& state = it->second;
  state.running.reset();
  // D30: a completed run is published even when its inputs changed meanwhile.
  const Status published =
      result.has_value() ? publishRun(staged, std::move(*result)) : Status(unexpected(result.error()));
  const bool inputs_changed = state.change_seq != staged_seq;
  if (published.has_value()) {
    state.stale = inputs_changed;
    state.error.clear();
    state.failed_seq.reset();
  } else {
    recordFailure(key, staged_seq, published.error());
  }
  if (std::exchange(state.pending, false)) {
    if (const GeneratorRecipe* recipe = bindingOf(key)) {
      submitRun(*recipe);
    }
  } else if (inputs_changed) {
    scheduleDirty();  // the newest inputs run at the brake's pace
  }
  // One plot refresh per burst: the runs of one pool drain share this queued emit.
  if (published.has_value()) {
    queueSignal(markers_published_queued_, &BatchEngine::markersPublished);
  }
  queueSignal(running_changed_queued_, &BatchEngine::runningChanged);
}

void BatchEngine::recordFailure(const BindingKey& key, uint64_t staged_seq, const std::string& error) {
  // The last set stays drawn; say why once per cause.
  RunState& state = runs_[key];
  state.stale = true;
  state.failed_seq = staged_seq;
  if (error == state.error) {
    return;
  }
  state.error = error;
  queueSignal(running_changed_queued_, &BatchEngine::runningChanged);
  // A first create, or one that moved the rule to another target, has no previous set.
  const std::string message = "Marker rule '" + key.first + "' failed: " + error +
                              ". Any previous markers are kept; fix the rule and apply it again.";
  if (diagnostic_sink_) {
    diagnostic_sink_(
        Diagnostic{.level = DiagnosticLevel::kError, .source = "Data Processors", .id = key.first, .message = message});
  } else {
    qCWarning(lcBatchEngine).noquote() << QString::fromStdString(message);
  }
}

void BatchEngine::cancelRun(const BindingKey& key) {
  const auto it = runs_.find(key);
  if (it == runs_.end()) {
    return;
  }
  const bool was_running = it->second.running.has_value();
  const bool had_error = !it->second.error.empty();
  if (was_running) {
    it->second.running->cancel();
  }
  runs_.erase(it);
  if (was_running || had_error) {
    queueSignal(running_changed_queued_, &BatchEngine::runningChanged);
  }
}

void BatchEngine::cancelRunsOf(std::string_view id) {
  std::vector<BindingKey> keys;
  for (const auto& [key, state] : runs_) {
    if (key.first == id) {
      keys.push_back(key);
    }
  }
  for (const BindingKey& key : keys) {
    cancelRun(key);
  }
}

void BatchEngine::scheduleDirty() {
  if (mode_ == Mode::kManual || brake_.isActive()) {
    return;  // an armed brake already covers every change within its period
  }
  const auto period_ms = static_cast<qint64>(1000.0 / reload_hz_);
  brake_.start(static_cast<int>(std::max<qint64>(0, period_ms - since_last_run_.elapsed())));
}

void BatchEngine::queueSignal(bool& queued, void (BatchEngine::*signal)()) {
  if (std::exchange(queued, true)) {
    return;
  }
  QMetaObject::invokeMethod(
      this,
      [this, &queued, signal] {
        queued = false;
        (this->*signal)();
      },
      Qt::QueuedConnection);
}

bool BatchEngine::isDemanded(const GeneratorRecipe& recipe) const {
  if (!demand_predicate_) {
    return true;
  }
  if (!recipe.all_datasets) {
    return demand_predicate_(recipe.dataset_id);
  }
  // The probe window holds at most the samples at the end of time: it asks whether a
  // dataset carries the input without copying its series.
  const Window probe = std::pair{std::numeric_limits<Timestamp>::max(), std::numeric_limits<Timestamp>::max()};
  const auto contributes = [&](DatasetId dataset) {
    return recipe.inputs.empty() ||
           std::any_of(recipe.inputs.begin(), recipe.inputs.end(), [&](const std::string& key) {
             return resolver_ && resolver_(dataset, key, probe).has_value();
           });
  };
  const std::vector<DatasetId> datasets = loadedDatasets();
  return std::any_of(datasets.begin(), datasets.end(), [&](DatasetId dataset) {
    return demand_predicate_(dataset) && contributes(dataset);
  });
}

void BatchEngine::onBrakeFired() {
  if (mode_ == Mode::kManual) {
    return;
  }
  since_last_run_.restart();
  for (const GeneratorRecipe& recipe : allRecipes()) {
    const auto state = runs_.find(bindingKey(recipe));
    // A run in flight reschedules itself when it ends (finishRun); a failed binding
    // waits for its inputs to change.
    if (state == runs_.end() || !state->second.stale || state->second.running.has_value() ||
        state->second.failed_seq == state->second.change_seq) {
      continue;
    }
    if (isDemanded(recipe)) {
      submitRun(recipe);
    }
  }
}

// ---- routing + lifecycle ---------------------------------------------------

Expected<std::vector<std::string>> BatchEngine::upsertGenerator(GeneratorRecipe recipe, CreateWait wait) {
  if (recipe.id.empty()) {
    return unexpected("generator id must not be empty");
  }
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

  // The owner separator is what keeps `<key>#<id>` unambiguous (MarkerTopics.h).
  const auto carries_separator = [](const std::string& text) {
    return text.find(kMarkerOwnerSeparator) != std::string::npos;
  };
  if (carries_separator(recipe.id) || std::any_of(recipe.outputs.begin(), recipe.outputs.end(), carries_separator)) {
    return unexpected("'#' is reserved as the marker owner separator (generator id and output topic)");
  }

  // Normalize the scope-encoded output topic host-side (see MarkerTopics.h): a
  // producer always addresses "__global__"; all_datasets rewrites that to the
  // ALL-DATASETS key, so the two scopes never share one family. A dataset-bound
  // recipe targeting the ALL-DATASETS key is rejected outright — silently
  // reinterpreting its scope would move the rule between UI rows with no signal.
  if (recipe.all_datasets && recipe.outputs.front() == sdk::kGlobalMarkerTopic) {
    recipe.outputs.front() = std::string(kAllDatasetsMarkerTopic);
  } else if (!recipe.all_datasets && recipe.outputs.front() == kAllDatasetsMarkerTopic) {
    return unexpected("output '__all__' is reserved for all-datasets generators");
  }
  // The topic's metadata is JSON: an id or key that is not UTF-8 cannot be described in it.
  if (!markerTopicInfo(topicMetadata(recipe)).has_value()) {
    return unexpected("generator id and output marker topic must be non-empty UTF-8 text");
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

  // The first run: compiled and staged here, then computed here or, bounded, on the
  // pool while this thread waits at most the create budget. Any failure known by
  // then leaves the prior output and recipe untouched.
  if (std::string error; !scripting::validateScript(recipe.script, &error)) {
    return unexpected(error);
  }
  const BindingKey key = bindingKey(recipe);
  auto staged = std::make_shared<StagedRun>(stageRun(recipe));
  if (staged->contributions.empty()) {
    return unexpected("none of the inputs exist in any loaded dataset");
  }
  std::optional<EnginePool::CancelToken> late_run;
  bool waits_for_room = false;
  Expected<std::optional<sdk::PlotMarkers>> computed = unexpected(std::string("not run"));
  std::optional<InFlightBudget::Reservation> room;
  const bool on_pool = pool_ != nullptr && wait == CreateWait::kBounded;
  if (on_pool && budget_ != nullptr) {
    room = budget_->tryReserve(kMarkerRunBytes + staged->bytes());
  }
  if (!on_pool) {
    computed = computeRun(*staged, /*cancel=*/nullptr);
  } else if (budget_ != nullptr && !room.has_value()) {
    // No room at once: once installed, the first run waits in the line like a refresh. Its copy goes now and is
    // staged again when the line lets it in, so no copy waits uncharged.
    waits_for_room = true;
  } else {
    // staged_seq 0: the binding's run state starts afresh below (cancelRun).
    const auto result = std::make_shared<RunResult>();
    result->room = std::move(room);
    const EnginePool::CancelToken token = startRun(key, staged, /*staged_seq=*/0, result);
    std::unique_lock<std::mutex> lock(result->mutex);
    if (result->finished_cv.wait_for(lock, createBudget(), [&result] { return result->finished; })) {
      result->consumed = true;
      computed = std::move(result->value);
      result->room.reset();  // the run is over: the next create may take its room before the drain publishes
    } else {
      late_run = token;
    }
  }
  if (!late_run.has_value() && !waits_for_room) {
    if (!computed.has_value()) {
      return unexpected(computed.error());
    }
    if (Status pushed = publishRun(*staged, std::move(*computed)); !pushed.has_value()) {
      return unexpected(pushed.error());
    }
  }
  // A new revision changes the binding's meaning: a run of the old one must not land.
  if (scope_changed) {
    cancelRunsOf(recipe.id);
  }
  cancelRun(key);

  // The previous revision's topic would otherwise keep drawing a set nothing ever
  // revisits: retire it unless this run just rewrote it (same dataset, same key).
  const PublishTarget fresh = publishTarget(recipe);
  if (scope_changed) {
    // The rule moved between Dataset and Global scope: every old output goes.
    retireOwnedTopics(recipe.id, existing->second.front().ephemeral, &fresh);
    recipes_.erase(existing);
  } else if (const GeneratorRecipe* replaced = bindingOf(key)) {
    retireTarget(publishTarget(*replaced), replaced->ephemeral, &fresh);
  }
  if (GeneratorRecipe* replaced = bindingOf(key)) {
    *replaced = std::move(recipe);
  } else {
    recipes_[recipe.id].push_back(std::move(recipe));
  }
  if (late_run.has_value()) {
    // Past the budget the first run carries on as this binding's scheduled run.
    RunState& state = runs_[key];
    state.running = *late_run;
    state.stale = true;
    queueSignal(running_changed_queued_, &BatchEngine::runningChanged);
  } else if (waits_for_room) {
    submitRun(*bindingOf(key));
  }
  return std::vector<std::string>{fresh.second};
}

Status BatchEngine::publishMarkerSet(
    DatasetId dataset_id, const std::string& object_topic_name, const std::string& metadata_json,
    const sdk::PlotMarkers& set) {
  ObjectTopicId topic_id;
  if (const std::optional<ObjectTopicId> existing = object_store_.findTopic(dataset_id, object_topic_name)) {
    if (markerTopicInfo(object_store_.descriptor(*existing).metadata_json) != markerTopicInfo(metadata_json)) {
      return unexpected("object topic '" + object_topic_name + "' already exists and describes another marker set");
    }
    topic_id = *existing;
  } else {
    Expected<ObjectTopicId> registered = object_store_.registerTopic(
        ObjectTopicDescriptor{
            .dataset_id = dataset_id, .topic_name = object_topic_name, .metadata_json = metadata_json});
    if (!registered.has_value()) {
      return unexpected(registered.error());
    }
    topic_id = *registered;
    object_store_.setRetentionBudget(
        topic_id, RetentionBudget{.time_window_ns = 0, .max_memory_bytes = 0, .max_entries = 1});
  }
  return object_store_.pushOwned(topic_id, Timestamp{0}, serializePlotMarkers(set));
}

void BatchEngine::publishEmptyMarkers(DatasetId dataset_id, const std::string& object_topic_name) {
  // The tombstone must NOT register a topic that was never published — so this uses
  // findTopic directly rather than publishMarkerSet (which registers on a miss).
  const std::optional<ObjectTopicId> topic_id = object_store_.findTopic(dataset_id, object_topic_name);
  if (!topic_id.has_value()) {
    return;  // never published on this dataset → nothing to clear
  }
  (void)object_store_.pushOwned(*topic_id, Timestamp{0}, serializePlotMarkers(sdk::PlotMarkers{}));
}

sdk::PlotMarkers BatchEngine::storedMarkers(ObjectTopicId topic_id, Timestamp shift) const {
  const std::optional<ResolvedObjectEntry> entry =
      object_store_.latestAt(topic_id, std::numeric_limits<Timestamp>::max());
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    return {};
  }
  Expected<sdk::PlotMarkers> decoded = deserializePlotMarkers(entry->payload.bytes.data(), entry->payload.bytes.size());
  if (!decoded.has_value()) {
    return {};
  }
  shiftMarkers(decoded->markers, shift + entry->payload_stamp_shift);
  return std::move(*decoded);
}

std::string BatchEngine::topicMetadata(const GeneratorRecipe& recipe) {
  MarkerTopicInfo info = markerInfoForKey(recipe.outputs.front());
  info.owner = recipe.id;
  return markerTopicMetadataJson(info);
}

BatchEngine::PublishTarget BatchEngine::publishTarget(const GeneratorRecipe& recipe) {
  return PublishTarget{
      recipe.all_datasets ? kAllDatasetsMarkerDataset : recipe.dataset_id,
      markerOwnerTopicName(recipe.outputs.front(), recipe.id)};
}

void BatchEngine::retireTarget(const PublishTarget& target, bool ephemeral, const PublishTarget* keep) {
  if (keep != nullptr && *keep == target) {
    return;
  }
  if (!ephemeral) {
    publishEmptyMarkers(target.first, target.second);
    return;
  }
  if (const std::optional<ObjectTopicId> topic_id = object_store_.findTopic(target.first, target.second)) {
    object_store_.removeTopic(*topic_id);
  }
}

void BatchEngine::retireOwnedTopics(std::string_view id, bool ephemeral, const PublishTarget* keep) {
  if (id.empty()) {
    return;  // direct writes carry no owner: an empty id would match all of them
  }
  for (const ObjectTopicId topic_id : object_store_.listTopics()) {
    const ObjectTopicDescriptor descriptor = object_store_.descriptor(topic_id);
    const std::optional<MarkerTopicInfo> info = markerTopicInfo(descriptor.metadata_json);
    if (info.has_value() && info->owner == id) {
      retireTarget(PublishTarget{descriptor.dataset_id, descriptor.topic_name}, ephemeral, keep);
    }
  }
}

Status BatchEngine::removeGenerator(std::string_view id) {
  const auto it = recipes_.find(std::string(id));
  if (it == recipes_.end()) {
    return unexpected("unknown generator id");
  }
  cancelRunsOf(id);
  retireOwnedTopics(id, it->second.front().ephemeral);
  recipes_.erase(it);
  std::erase_if(published_, [id](const auto& entry) { return entry.first.first == id; });
  return okStatus();
}

void BatchEngine::cancelRunsOn(DatasetId dataset) {
  for (const GeneratorRecipe& recipe : allRecipes()) {
    const BindingKey key = bindingKey(recipe);
    const auto state = runs_.find(key);
    if ((recipe.all_datasets || recipe.dataset_id == dataset) && state != runs_.end() &&
        state->second.running.has_value()) {
      cancelRun(key);
    }
  }
}

void BatchEngine::remapGeneratorsToAnchor(DatasetId anchor, const std::vector<DatasetId>& consumed) {
  // A run staged before the merge would publish over mergeMarkerTopics' concatenation.
  cancelRunsOn(anchor);
  const auto is_consumed = [&consumed](const GeneratorRecipe& recipe) {
    return !recipe.all_datasets && std::find(consumed.begin(), consumed.end(), recipe.dataset_id) != consumed.end();
  };
  for (auto& [id, bindings] : recipes_) {
    // A consumed dataset's topics are dropped after this call: a run of a consumed
    // binding must not publish (and so resurrect) them.
    for (const GeneratorRecipe& recipe : bindings) {
      if (is_consumed(recipe)) {
        cancelRun(bindingKey(recipe));
      }
    }
    // Collision rule (see the header): the anchor's own binding wins. mergeMarkerTopics
    // runs before this call (SessionManager::mergeDatasets, (3b)) and already moved
    // every consumed output onto the anchor's topic of this id, so retiring the
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
  // The survivors that read the merged data re-run over it.
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (recipe.all_datasets || recipe.dataset_id == anchor) {
      markStale(recipe);
    }
  }
  scheduleDirty();
}

std::vector<std::string> BatchEngine::clearGeneratorsForDataset(DatasetId dataset) {
  const auto bound_here = [dataset](const GeneratorRecipe& recipe) {
    return !recipe.all_datasets && recipe.dataset_id == dataset;
  };
  for (auto& [id, bindings] : recipes_) {
    for (const GeneratorRecipe& recipe : bindings) {
      if (bound_here(recipe)) {
        cancelRun(bindingKey(recipe));
        published_.erase(bindingKey(recipe));
      }
    }
    std::erase_if(bindings, bound_here);
  }
  std::erase_if(recipes_, [](const auto& entry) { return entry.second.empty(); });

  // The removed dataset may have contributed to a shared set: rebuild each from the
  // survivors. With no contributor left the rule draws nothing, but it stays live
  // so a contributor loaded later revives it.
  std::vector<std::string> affected;
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (recipe.all_datasets) {
      rerunNow(recipe, affected);
    }
  }
  return affected;
}

std::vector<std::string> BatchEngine::recomputeForChangedInputs(
    const std::vector<std::string>& changed, DatasetId scope) {
  std::vector<std::string> affected;
  for (const GeneratorRecipe& recipe : allRecipes()) {
    // Topic names are not unique across datasets, so the name test alone would mark
    // a generator bound to a dataset that never changed.
    if (scope != kAnyDataset && !recipe.all_datasets && recipe.dataset_id != scope) {
      continue;
    }
    if (!changed.empty() && !readsAnyTopicOf(recipe, changed)) {
      continue;
    }
    // An all_datasets recipe's set is shared, so a change on one contributing
    // dataset re-runs the whole union — `scope` only attributes the change.
    markStale(recipe);
    affected.push_back(recipe.id);
  }
  std::sort(affected.begin(), affected.end());
  affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
  if (!affected.empty()) {
    scheduleDirty();
  }
  return affected;
}

std::vector<std::string> BatchEngine::exemptDependentsOf(const std::vector<std::string>& removed_inputs) const {
  std::vector<std::string> outputs;
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (recipe.ephemeral || !recipe.history_exempt || !readsAnyTopicOf(recipe, removed_inputs)) {
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

bool BatchEngine::hasHistoryExemptGenerators() const {
  const auto recipes = allRecipes();
  return std::any_of(recipes.begin(), recipes.end(), [](const GeneratorRecipe& r) { return r.history_exempt; });
}

std::unordered_set<std::string> BatchEngine::unchangedGeneratorIds(
    const std::vector<GeneratorRecipe>& wanted, RestoreIntent intent,
    const std::vector<std::string>& removed_upstream) const {
  const auto same = [](const GeneratorRecipe& live, const GeneratorRecipe& snapshot) {
    // An all_datasets recipe's dataset_id means nothing and is not persisted.
    return live.kind == snapshot.kind && live.language == snapshot.language &&
           live.all_datasets == snapshot.all_datasets && live.inputs == snapshot.inputs &&
           live.declared_inputs == snapshot.declared_inputs && live.outputs == snapshot.outputs &&
           sameTextIgnoringLineEndings(live.script, snapshot.script) &&
           sameTextIgnoringLineEndings(live.params_json, snapshot.params_json) && live.window == snapshot.window &&
           live.history_exempt == snapshot.history_exempt;
  };
  // Invariant 3: the shared set was published in the display frame of its run. A run in
  // flight staged under offsets the restore may have moved since, and a set never
  // published has no frame to compare: both count as moved.
  const auto display_frame_moved = [this](const GeneratorRecipe& recipe) {
    if (!recipe.all_datasets) {
      return false;
    }
    const auto published = published_.find(bindingKey(recipe));
    if (published == published_.end() || isRunning(recipe.id)) {
      return true;
    }
    return std::any_of(
        published->second.display_shifts.begin(), published->second.display_shifts.end(),
        [this](const auto& entry) { return entry.second != -displayOffsetOf(entry.first); });
  };
  std::unordered_set<std::string> unchanged;
  for (const auto& [id, bindings] : recipes_) {
    if (!inAuthority(bindings.front(), intent)) {
      continue;
    }
    const auto wanted_count =
        std::count_if(wanted.begin(), wanted.end(), [&id](const GeneratorRecipe& recipe) { return recipe.id == id; });
    const bool all_same = static_cast<std::size_t>(wanted_count) == bindings.size() &&
                          std::all_of(bindings.begin(), bindings.end(), [&](const GeneratorRecipe& live) {
                            const auto snapshot =
                                std::find_if(wanted.begin(), wanted.end(), [&](const GeneratorRecipe& candidate) {
                                  return candidate.id == id && bindingDataset(candidate) == bindingDataset(live);
                                });
                            return snapshot != wanted.end() && same(live, *snapshot) &&
                                   !readsAnyTopicOf(live, removed_upstream) && !display_frame_moved(live);
                          });
    if (all_same) {
      unchanged.insert(id);
    }
  }
  return unchanged;
}

void BatchEngine::clearGenerators(RestoreIntent intent, const std::unordered_set<std::string>& keep) {
  std::vector<std::string> ids;
  for (const auto& [id, bindings] : recipes_) {
    if (inAuthority(bindings.front(), intent) && keep.count(id) == 0) {
      ids.push_back(id);
    }
  }
  for (const std::string& id : ids) {
    (void)removeGenerator(id);
  }
}

std::string BatchEngine::ownerPrefix(std::string_view owner_plugin) {
  std::string prefix(owner_plugin);
  prefix.push_back('/');
  return prefix;
}

bool BatchEngine::removeEphemerals(const std::unordered_set<std::string>& ids) {
  bool removed = false;
  for (const std::string& id : ids) {
    if (const GeneratorRecipe* first = firstBinding(id); first != nullptr && first->ephemeral) {
      removed = removeGenerator(id).has_value() || removed;
    }
  }
  return removed;
}

void BatchEngine::clearAllGenerators() {
  clearGenerators(RestoreIntent::kReplace);
}

std::vector<std::string> BatchEngine::recomputeForDataset(DatasetId dataset) {
  // Every recipe bound here, plus every all_datasets recipe (its shared set may have
  // this dataset as a contributor), run now: a reload or restore must be complete
  // when this returns.
  std::vector<std::string> affected;
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (recipe.all_datasets || recipe.dataset_id == dataset) {
      rerunNow(recipe, affected);
    }
  }
  return affected;
}

std::vector<std::string> BatchEngine::mergeMarkerTopics(
    DatasetId anchor, const std::vector<DatasetMergeSource>& sources) {
  // Anchor topic names touched by this merge, republished once each in sorted order.
  std::set<std::string> touched;
  for (const DatasetMergeSource& source : sources) {
    for (const ObjectTopicId source_id : object_store_.listTopics(source.dataset_id)) {
      const ObjectTopicDescriptor source_topic = object_store_.descriptor(source_id);
      if (!isMarkerTopic(source_topic)) {
        continue;
      }
      const std::string& name = source_topic.topic_name;
      // The anchor's own set (unshifted) first, then the source's on the anchor clock.
      sdk::PlotMarkers merged;
      if (const std::optional<ObjectTopicId> anchor_id = object_store_.findTopic(anchor, name)) {
        // Folding would keep the anchor's description for both sets: leave the source's alone.
        if (markerTopicInfo(object_store_.descriptor(*anchor_id).metadata_json) !=
            markerTopicInfo(source_topic.metadata_json)) {
          continue;
        }
        merged = storedMarkers(*anchor_id, Timestamp{0});
      }
      sdk::PlotMarkers moved = storedMarkers(source_id, source.raw_shift_ns);
      merged.markers.insert(
          merged.markers.end(), std::make_move_iterator(moved.markers.begin()),
          std::make_move_iterator(moved.markers.end()));
      object_store_.removeTopic(source_id);
      if (publishMarkerSet(anchor, name, source_topic.metadata_json, merged).has_value()) {
        touched.insert(name);
      }
    }
  }
  return {touched.begin(), touched.end()};
}

std::vector<BatchEngine::GeneratorRecipe> BatchEngine::recipes() const {
  std::vector<GeneratorRecipe> out;
  for (const GeneratorRecipe& recipe : allRecipes()) {
    if (!recipe.ephemeral) {
      out.push_back(recipe);
    }
  }
  return out;
}

std::size_t BatchEngine::persistentBindingCount() const {
  std::size_t count = 0;
  for (const auto& [id, bindings] : recipes_) {
    if (!bindings.front().ephemeral) {
      count += bindings.size();
    }
  }
  return count;
}

std::vector<std::string> BatchEngine::generatorIds() const {
  std::vector<std::string> ids;
  for (const auto& [id, bindings] : recipes_) {
    if (!bindings.front().ephemeral) {
      ids.push_back(id);
    }
  }
  return ids;
}

const BatchEngine::GeneratorRecipe* BatchEngine::firstBinding(std::string_view id) const {
  const auto it = recipes_.find(std::string(id));
  return it == recipes_.end() ? nullptr : &it->second.front();
}

std::vector<std::reference_wrapper<const BatchEngine::GeneratorRecipe>> BatchEngine::allRecipes() const {
  std::vector<std::reference_wrapper<const GeneratorRecipe>> out;
  for (const auto& [id, bindings] : recipes_) {
    out.insert(out.end(), bindings.begin(), bindings.end());
  }
  return out;
}

BatchEngine::BindingKey BatchEngine::bindingKey(const GeneratorRecipe& recipe) {
  return {recipe.id, bindingDataset(recipe)};
}

BatchEngine::GeneratorRecipe* BatchEngine::bindingOf(const BindingKey& key) {
  const auto it = recipes_.find(key.first);
  if (it == recipes_.end()) {
    return nullptr;
  }
  const auto match = std::find_if(it->second.begin(), it->second.end(), [&key](const GeneratorRecipe& binding) {
    return bindingDataset(binding) == key.second;
  });
  return match == it->second.end() ? nullptr : &*match;
}

DatasetId BatchEngine::bindingDataset(const GeneratorRecipe& recipe) {
  return (recipe.all_datasets || recipe.ephemeral) ? kAnyDataset : recipe.dataset_id;
}

BatchEngine::ResolvedSeries readResolvedSeries(const SeriesReader& series, const BatchEngine::Window& window) {
  const Range<Timestamp> range = window.has_value() ? Range<Timestamp>{.min = window->first, .max = window->second}
                                                    : Range<Timestamp>{
                                                          .min = std::numeric_limits<Timestamp>::min(),
                                                          .max = std::numeric_limits<Timestamp>::max()};
  BatchEngine::ResolvedSeries out;
  if (!window.has_value()) {  // a window may keep far fewer samples than the series holds
    out.timestamps.reserve(series.size());
    out.values.reserve(series.size());
  }
  series.samples(range).forEach([&out](const SeriesSample& sample) {
    out.timestamps.push_back(static_cast<double>(sample.timestamp));
    out.values.push_back(sample.value);
  });
  return out;
}

}  // namespace PJ
