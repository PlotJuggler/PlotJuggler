// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/GridRunner.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <variant>

#include "OnDemandRun.h"
#include "pj_scripting/object_summary.h"

namespace PJ {
namespace {

using scripting::OutputSpec;
using scripting::OutputType;
using scripting::ScriptBundle;
using scripting::Unavailable;
using Stop = sdk::EvaluationStop;
using OutputStatus = sdk::EvaluationOutputStatus;

/// The tag of a job's own result's identity in a reader's cache key, apart from a ring entry's
/// (`InputStager::kRingIdentity`).
constexpr uint64_t kJobIdentity = 1;
/// What `toJson` adds around the envelope and the bundles: `{"bundles":[` and `],`, less the envelope's `{`.
constexpr std::size_t kBundlesFrame = 13;
/// Indexed by `sdk::EvaluationStop` and `sdk::EvaluationOutputStatus`, in their declaration order.
constexpr std::string_view kStopNames[] = {
    "complete", "budget_time", "budget_evaluations", "budget_bytes", "budget_report", "cancelled", "error"};
constexpr std::string_view kStatusNames[] = {"ok", "unavailable", "empty", "error"};
static_assert(std::size(kStopNames) == static_cast<std::size_t>(Stop::kError) + 1, "a name per EvaluationStop");
static_assert(
    std::size(kStatusNames) == static_cast<std::size_t>(OutputStatus::kError) + 1, "a name per EvaluationOutputStatus");

/// Invalid UTF-8 (a script's string, a plugin's alias) becomes U+FFFD instead of throwing.
std::string dumpJson(const nlohmann::json& value) {
  return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

/// `text` cut to at most `bytes` bytes, on a UTF-8 character boundary.
std::string utf8Prefix(std::string text, std::size_t bytes) {
  if (text.size() <= bytes) {
    return text;
  }
  while (bytes > 0 && (static_cast<unsigned char>(text[bytes]) & 0xC0) == 0x80) {
    --bytes;
  }
  text.resize(bytes);
  return text;
}

/// A report output type: "number", "string", a builtin object type's name, or "unknown" (an unavailable value).
std::string typeName(const scripting::ScriptValue& value) {
  if (std::holds_alternative<double>(value)) {
    return "number";
  }
  if (std::holds_alternative<std::string>(value)) {
    return "string";
  }
  if (const auto* object = std::get_if<sdk::BuiltinObject>(&value)) {
    return std::string(sdk::name(sdk::typeOf(*object)));
  }
  return "unknown";
}

/// A bundle's outputs: each one's status and its value, summary or reason. An object travels as its summary, never as
/// bytes; JSON has no NaN, so a non-finite number is that output's error.
std::map<std::string, sdk::EvaluationOutput> reportOutputs(const ScriptBundle& bundle) {
  std::map<std::string, sdk::EvaluationOutput> outputs;
  for (const scripting::NamedValue& output : bundle.outputs) {
    sdk::EvaluationOutput& entry = outputs[output.name];
    if (const auto* number = std::get_if<double>(&output.value)) {
      if (std::isfinite(*number)) {
        entry.value = *number;
      } else {
        entry.status = OutputStatus::kError;
        entry.reason = "not a finite number";
      }
    } else if (const auto* text = std::get_if<std::string>(&output.value)) {
      entry.value = *text;
    } else if (const auto* object = std::get_if<sdk::BuiltinObject>(&output.value)) {
      entry.status = scripting::isEmptyObject(*object) ? OutputStatus::kEmpty : OutputStatus::kOk;
      entry.summary = scripting::summarizeObject(*object);
    } else {
      entry.status = OutputStatus::kUnavailable;
      entry.reason = std::get<Unavailable>(output.value).reason;
    }
  }
  return outputs;
}

/// One bundle as the report writes it.
std::string bundleJson(const sdk::EvaluationBundle& bundle) {
  nlohmann::json outputs = nlohmann::json::object();
  for (const auto& [name, output] : bundle.outputs) {
    nlohmann::json entry{{"status", kStatusNames[static_cast<std::size_t>(output.status)]}};
    if (!output.value.is_null()) {
      entry["value"] = output.value;
    }
    if (!output.summary.is_null()) {
      entry["summary"] = output.summary;
    }
    if (!output.reason.empty()) {
      entry["reason"] = output.reason;
    }
    outputs[name] = std::move(entry);
  }
  return dumpJson({{"requested_ns", bundle.requested_ns}, {"inputs", bundle.inputs}, {"outputs", std::move(outputs)}});
}

/// The report without its bundles: `{"coverage":{...}}`, and the root `outputs` when something was inferred.
std::string envelope(
    uint64_t candidates, uint64_t evaluated, Stop stopped, const std::string* error,
    const std::vector<sdk::EvaluationGap>& gaps, const std::vector<sdk::DataProcessorOutput>& inferred) {
  nlohmann::json coverage{
      {"candidates", candidates},
      {"evaluated", evaluated},
      {"stopped", kStopNames[static_cast<std::size_t>(stopped)]},
      {"gaps", nlohmann::json::array()}};
  if (error != nullptr) {
    coverage["error"] = *error;
  }
  for (const sdk::EvaluationGap& gap : gaps) {
    coverage["gaps"].push_back({{"from_ns", gap.from_ns}, {"to_ns", gap.to_ns}});
  }
  nlohmann::json doc{{"coverage", std::move(coverage)}};
  if (!inferred.empty()) {
    nlohmann::json& outputs = doc["outputs"] = nlohmann::json::array();
    for (const sdk::DataProcessorOutput& output : inferred) {
      outputs.push_back({{"name", output.name}, {"type", output.type}});
    }
  }
  return dumpJson(doc);
}

GridRunner::Budget clamp(const GridRunner::Budget& asked) {
  constexpr GridRunner::Budget caps = GridRunner::kHostCaps;
  return {
      std::min(asked.max_millis, caps.max_millis), std::min(asked.max_bytes, caps.max_bytes),
      std::min(asked.max_evaluations, caps.max_evaluations), std::min(asked.max_report_bytes, caps.max_report_bytes)};
}

}  // namespace

struct GridRunner::Result {
  /// What determined a run, with what it observed: the next instant reuses the result while this serves it. Unset
  /// for a result read from the history, which is looked up again at every instant.
  std::optional<ResultHistory::CacheKey> key;
  std::vector<std::pair<std::string, InputStager::Override>> outputs;  ///< the object outputs, as readers stage them
  std::map<std::string, sdk::EvaluationOutput> report;                 ///< the evaluated node's outputs, summarized
  std::vector<sdk::DataProcessorOutput> types;  ///< ... and their types, for a node that declares none
  std::vector<double> row;                      ///< a series chunk's node: its number outputs, in declaration order
  std::string gap;                              ///< ... or why it has no row
};

/// One evaluation. GUI thread, except the run fields, which the worker writes between a submission and its
/// publication, while the GUI thread leaves them alone.
struct GridRunner::Job {
  struct Step {
    RecipeGraph::Node node;
    std::string key;         ///< `node.recipe.key()`, taken once: readers look chain nodes up by it at every instant
    bool installed = false;  ///< its revision guards the job, and its history may serve it (an upstream)
    std::optional<Result> memo;  ///< its last result in this job
  };
  std::vector<Step> chain;  ///< the upstreams before their readers, the evaluated node last
  std::vector<std::pair<std::string, uint64_t>> guards;  ///< the installed nodes read or evaluated, by revision
  uint64_t epoch = 0;
  std::vector<Timestamp> grid;
  std::size_t next = 0;  ///< the instant being evaluated
  std::size_t step = 0;  ///< the chain node being evaluated at it
  Budget budget;
  scripting::RunOptions options;  ///< the memory split and the TF snapshot, set when the room comes
  uint64_t tf_revision = 0;
  std::chrono::steady_clock::time_point began;
  Report report;
  std::size_t bundle_bytes = 0;  ///< the bundles as `toJson` writes them, and the commas between them
  uint64_t next_identity = 1;
  Done done;
  std::optional<InFlightBudget::Reservation> room;
  InFlightBudget::Ticket ticket;
  bool finished = false;
  bool running = false;
  EnginePool::CancelToken token;
  InputStager::Staged staged;                                   ///< the run's inputs
  StagedRun ran;                                                ///< written by the worker
  std::map<std::string, sdk::EvaluationOutput> report_outputs;  ///< written by the worker, for the evaluated node
  /// A series chunk (`submitRows`): what it wrote so far, the indices of the node's number outputs, and its callback.
  std::optional<Rows> rows;
  std::vector<std::size_t> columns;
  RowsDone rows_done;
  std::function<void()> released;  ///< `cancel`'s, called when the cancelled run's publication lets it go
};

namespace {

/// Drops what only the job's execution needed (its results, its run's inputs and outputs, the TF snapshot), then its
/// room: an ended job, even one its caller keeps, holds nothing outside the budget. Not while a run computes.
void release(GridRunner::Job& job) {
  for (GridRunner::Job::Step& step : job.chain) {
    step.memo.reset();
  }
  job.staged = {};
  job.ran = {};
  job.report_outputs.clear();
  job.options.tf.reset();
  job.options.native_budget.reset();
  job.room.reset();
}

/// Owned by a run's publication: releases the job when the pool discards the publication (destroyed before it
/// drains); running the publication disarms it.
struct DiscardGuard {
  explicit DiscardGuard(std::shared_ptr<GridRunner::Job> held) : job(std::move(held)) {}
  DiscardGuard(const DiscardGuard&) = delete;
  DiscardGuard& operator=(const DiscardGuard&) = delete;
  ~DiscardGuard() {
    if (job != nullptr) {
      job->running = false;
      release(*job);
    }
  }
  std::shared_ptr<GridRunner::Job> job;
};

/// Appends `node`'s upstreams, then `node`, to `job.chain`, each once; a finding is read, so it is only guarded.
Status appendChain(
    const RecipeGraph& graph, RecipeGraph::Node node, bool installed, GridRunner::Job& job,
    std::vector<std::string>& findings) {
  for (const RecipeGraph::Binding& binding : node.bindings) {
    if (binding.upstream.empty() ||
        std::ranges::any_of(job.chain, [&](const auto& step) { return step.key == binding.upstream; }) ||
        std::ranges::find(findings, binding.upstream) != findings.end()) {
      continue;
    }
    const RecipeGraph::Node* upstream = graph.find(binding.upstream);
    if (upstream == nullptr) {
      return unexpected("on-demand: upstream '" + binding.upstream + "' removed");
    }
    if (std::optional<std::string> why = graph.missingInput(binding.upstream)) {
      return unexpected(std::move(*why));
    }
    job.guards.emplace_back(binding.upstream, upstream->revision);
    if (upstream->recipe.pinned_t) {
      findings.push_back(binding.upstream);
    } else if (Status appended = appendChain(graph, *upstream, true, job, findings); !appended) {
      return appended;
    }
  }
  std::string key = node.recipe.key();
  job.chain.push_back({std::move(node), std::move(key), installed, std::nullopt});
  return okStatus();
}

/// An installed upstream's result for `staged` that its ring holds, as readers stage it; nullopt otherwise (a result
/// only the compute cache holds runs again: its identity is no ring entry's).
std::optional<GridRunner::Result> fromHistory(
    const ResultHistory& history, const GridRunner::Job::Step& step, const InputStager::Staged& staged, Timestamp t,
    uint64_t tf_revision) {
  const RecipeGraph::Node& node = step.node;
  const std::string& key = step.key;
  const std::optional<ResultHistory::Hit> hit = history.lookup(key, staged.key, staged.stamp, t, tf_revision);
  if (!hit || !hit->in_ring) {
    return std::nullopt;
  }
  GridRunner::Result result;
  for (const OutputSpec& output : node.recipe.outputs) {
    if (output.type != OutputType::kObject) {
      continue;
    }
    const std::optional<ResultHistory::OutputAt> at = history.resultAt(key, output.name, hit->stamp);
    if (!at || at->stamp != hit->stamp) {
      return std::nullopt;
    }
    // Given at once: Clang cannot default-construct the variant (`Entry`, nested, has default member initializers).
    InputStager::Override override{
        .stamp = at->stamp,
        .identity = {InputStager::kRingIdentity, at->uid.value},
        .value = Unavailable{at->unavailable.value_or("")}};
    if (!at->unavailable) {
      override.value = InputStager::Entry{at->topic, at->uid, output.object_type, {}};
    }
    result.outputs.emplace_back(output.name, std::move(override));
  }
  return result;
}

}  // namespace

GridRunner::GridRunner(
    const ObjectStore& store, const RecipeGraph& graph, const ResultHistory& history, const InputStager& stager,
    EnginePool& pool, InFlightBudget& budget, Hooks hooks)
    : store_(store),
      graph_(graph),
      history_(history),
      stager_(stager),
      pool_(pool),
      budget_(budget),
      hooks_(std::move(hooks)) {
  if (!hooks_.run) {
    hooks_.run = &scripting::runOnDemand;
  }
  if (!hooks_.now) {
    hooks_.now = &std::chrono::steady_clock::now;
  }
}

namespace {

/// Visits, ascending and each once, the union of the ascending `ranges`; false when `visit` stopped it.
// ponytail: a linear scan of the range heads per instant; a heap if jobs ever read many inputs.
template <typename Visit>
bool mergeRanges(std::vector<std::pair<const Timestamp*, const Timestamp*>> heads, Visit&& visit) {
  for (;;) {
    const Timestamp* lowest = nullptr;
    for (const auto& [from, to] : heads) {
      if (from != to && (lowest == nullptr || *from < *lowest)) {
        lowest = from;
      }
    }
    if (lowest == nullptr) {
      return true;
    }
    const Timestamp instant = *lowest;
    for (auto& [from, to] : heads) {
      while (from != to && *from == instant) {
        ++from;
      }
    }
    if (!visit(instant)) {
      return false;
    }
  }
}

/// The raw inputs the chain reads, each once: its object topics and its scalar series, by bare name.
struct RawInputs {
  std::set<std::string> objects;
  std::map<std::string, std::string> scalars;  // series name -> the alias a step reads it by
};
RawInputs rawInputsOf(const GridRunner::Job& job) {
  RawInputs inputs;
  for (const GridRunner::Job::Step& step : job.chain) {
    for (std::size_t index = 0; index < step.node.bindings.size(); ++index) {
      const RecipeGraph::Binding& binding = step.node.bindings[index];
      if (binding.upstream.empty() && binding.object_type == sdk::BuiltinObjectType::kNone) {
        inputs.scalars.emplace(binding.name, step.node.recipe.inputs[index]);
      } else if (binding.upstream.empty()) {
        inputs.objects.insert(binding.name);
      }
    }
  }
  return inputs;
}

/// The object topics among `names` in `dataset` that exist, through the stager's remembered ids.
std::vector<ObjectTopicId> objectTopics(
    const InputStager& stager, DatasetId dataset, const std::set<std::string>& names) {
  std::vector<ObjectTopicId> topics;
  for (const std::string& name : names) {
    if (const std::optional<ObjectTopicId> topic = stager.topicOf(dataset, name)) {
      topics.push_back(*topic);
    }
  }
  return topics;
}

/// The entry stamps of `topics`, each view holding its topic's read lock, empty ones included.
std::vector<EntryTimestampsView> entryTimes(const ObjectStore& store, const std::vector<ObjectTopicId>& topics) {
  std::vector<EntryTimestampsView> views;
  views.reserve(topics.size());
  for (const ObjectTopicId topic : topics) {
    views.push_back(store.entryTimestamps(topic));
  }
  return views;
}

/// The real result stamps of the findings, sorted: in a chain, a finding is an input only from its stamp.
std::vector<Timestamp> findingStamps(const ResultHistory& history, const std::vector<std::string>& findings) {
  std::vector<Timestamp> stamps;
  for (const std::string& finding : findings) {
    if (const auto shown = history.shownAt(finding, std::numeric_limits<Timestamp>::max())) {
      stamps.push_back(shown->stamp);
    }
  }
  std::ranges::sort(stamps);
  return stamps;
}

}  // namespace

Expected<std::shared_ptr<GridRunner::Job>> GridRunner::chainJob(
    RecipeGraph::Node node, bool installed, std::vector<std::string>& findings) const {
  auto job = std::make_shared<Job>();
  job->epoch = hooks_.epoch ? hooks_.epoch(node.dataset) : 0;
  if (installed) {
    job->guards.emplace_back(node.recipe.key(), node.revision);
  }
  if (Status chained = appendChain(graph_, std::move(node), installed, *job, findings); !chained) {
    return unexpected(chained.error());
  }
  return job;
}

Expected<std::shared_ptr<GridRunner::Job>> GridRunner::submit(
    RecipeGraph::Node node, bool installed, Timestamp start, Timestamp end, Budget budget, Done done) {
  if (end < start) {
    return unexpected("on-demand: the window ends before it starts");
  }
  const DatasetId dataset = node.dataset;
  std::vector<std::string> findings;
  Expected<std::shared_ptr<Job>> made = chainJob(std::move(node), installed, findings);
  if (!made) {
    return made;
  }
  std::shared_ptr<Job> job = std::move(*made);
  job->budget = clamp(budget);

  // The leading gap first: up to the newest of the raw inputs' oldest samples, where the grid starts. Then the grid:
  // the sorted union of that start and every input change instant from it to the window's end, counted (each instant
  // once) before it is allocated.
  const auto refuse = [] {
    return unexpected(
        "on-demand: the window holds more than " + std::to_string(kMaxGridInstants) +
        " input change instants; evaluate a shorter one");
  };
  const RawInputs inputs = rawInputsOf(*job);
  Timestamp from = start;  // past `end`: the gap covers the whole window, and the grid is empty
  std::vector<std::vector<Timestamp>> scalar_instants;
  for (const auto& [name, alias] : inputs.scalars) {
    if (!hooks_.scalar_changes) {
      break;
    }
    ScalarChanges changes = hooks_.scalar_changes(dataset, name, start, end, kMaxGridInstants);
    if (changes.over_limit) {
      return refuse();
    }
    if (changes.unreadable != ScalarChanges::Unreadable::kNone && end > start) {
      return unexpected(
          "on-demand: input '" + alias +
          (changes.unreadable == ScalarChanges::Unreadable::kString
               ? "' is a string series, whose changes a window evaluation cannot read yet; evaluate an instant"
               : "' is neither a numeric nor a string series, so a window evaluation cannot read its changes"));
    }
    from = std::max(from, changes.first.value_or(start));
    scalar_instants.push_back(std::move(changes.instants));
  }
  std::vector<EntryTimestampsView> views =
      entryTimes(store_, objectTopics(stager_, dataset, inputs.objects));  // locked until the grid is built
  std::erase_if(views, [](const EntryTimestampsView& times) { return times.empty(); });
  for (const EntryTimestampsView& times : views) {
    from = std::max(from, times[0]);
  }
  std::vector<std::pair<const Timestamp*, const Timestamp*>> ranges;
  if (from <= end) {
    ranges.emplace_back(&from, &from + 1);
  }
  for (const std::vector<Timestamp>& instants : scalar_instants) {
    ranges.emplace_back(
        std::lower_bound(instants.data(), instants.data() + instants.size(), from), instants.data() + instants.size());
  }
  for (const EntryTimestampsView& times : views) {
    const Timestamp* first = std::lower_bound(times.begin(), times.end(), from);
    ranges.emplace_back(first, std::upper_bound(first, times.end(), end));
  }
  std::vector<Timestamp> findings_at = findingStamps(history_, findings);
  std::erase_if(findings_at, [&](Timestamp stamp) { return stamp < from || stamp > end; });
  ranges.emplace_back(findings_at.data(), findings_at.data() + findings_at.size());
  std::size_t count = 0;
  if (!mergeRanges(ranges, [&count](Timestamp) { return ++count <= kMaxGridInstants; })) {
    return refuse();
  }
  std::vector<Timestamp>& grid = job->grid;
  grid.reserve(count);
  mergeRanges(ranges, [&grid](Timestamp instant) {
    grid.push_back(instant);
    return true;
  });
  views.clear();

  Report& report = job->report;
  report.candidates = grid.size();
  if (from > start) {
    report.gaps.push_back({start, std::min(from, end)});
  }
  // Every stop leaves at least this, an "error" with an empty reason included.
  const std::size_t smallest =
      kBundlesFrame + envelope(report.candidates, 0, Stop::kBudgetEvaluations, nullptr, report.gaps, {}).size();
  if (smallest > job->budget.max_report_bytes) {
    return unexpected(
        "on-demand: max_report_bytes (" + std::to_string(job->budget.max_report_bytes) +
        ") is below the smallest report of this evaluation (" + std::to_string(smallest) + " bytes)");
  }

  job->done = std::move(done);
  waitForRoom(job);
  return job;
}

Expected<std::shared_ptr<GridRunner::Job>> GridRunner::submitRows(
    RecipeGraph::Node node, std::optional<Timestamp> after, std::optional<Timestamp> until, SequentialUID seen,
    std::size_t limit, RowsDone done) {
  const DatasetId dataset = node.dataset;
  std::vector<std::size_t> columns;
  for (std::size_t index = 0; index < node.recipe.outputs.size(); ++index) {
    if (node.recipe.outputs[index].type == OutputType::kNumber) {
      columns.push_back(index);
    }
  }
  std::vector<std::string> findings;
  Expected<std::shared_ptr<Job>> made = chainJob(std::move(node), true, findings);
  if (!made) {
    return made;
  }
  std::shared_ptr<Job> job = std::move(*made);
  job->budget = clamp(Budget{.max_millis = kRowsMaxMillis});
  job->rows.emplace();
  const std::vector<ObjectTopicId> topics = objectTopics(stager_, dataset, rawInputsOf(*job).objects);
  const std::vector<Timestamp> findings_at = findingStamps(history_, findings);
  // The clock in (`from`, `until`], cut while every view holds its topic's read lock. `seen` is minted under those
  // locks, so an entry with a greater uid is in no view.
  const auto cut = [&](std::optional<Timestamp> from) {
    const std::vector<EntryTimestampsView> views = entryTimes(store_, topics);
    job->rows->seen = SequentialUID::getNext();
    std::vector<std::pair<const Timestamp*, const Timestamp*>> ranges;
    uint64_t left = 0;
    uint64_t before = 0;
    const auto add = [&](const Timestamp* begin, const Timestamp* last) {
      const Timestamp* first = from ? std::upper_bound(begin, last, *from) : begin;
      last = until ? std::upper_bound(first, last, *until) : last;
      left += static_cast<uint64_t>(last - first);
      before += static_cast<uint64_t>(first - begin);
      ranges.emplace_back(first, last);
    };
    for (const EntryTimestampsView& times : views) {
      add(times.begin(), times.end());
    }
    add(findings_at.data(), findings_at.data() + findings_at.size());
    job->grid.clear();
    mergeRanges(ranges, [&job, limit](Timestamp instant) {
      job->grid.push_back(instant);
      return job->grid.size() < limit;
    });
    job->rows->left = left;
    job->rows->before = before;
  };
  cut(after);
  // Checked after the cut: an entry that missed the previous clock either shows here or has a uid past the new `seen`.
  // Below the watermark only: a live clock ends at every input's newest entry, so an in-order arrival is never there.
  const auto late = [&](ObjectTopicId topic) {
    const std::optional<Timestamp> oldest = store_.oldestArrivalAfter(topic, seen);
    return oldest && *oldest < *after;
  };
  if (after && seen.valid() && std::ranges::any_of(topics, late)) {
    job->rows->restarted = true;
    cut(std::nullopt);
  }
  if (job->grid.empty() && !job->rows->restarted) {
    return std::shared_ptr<Job>{};
  }
  job->rows->times.reserve(job->grid.size());
  job->rows->values.reserve(job->grid.size() * columns.size());
  job->columns = std::move(columns);
  job->rows_done = std::move(done);
  waitForRoom(job);
  return job;
}

void GridRunner::waitForRoom(const std::shared_ptr<Job>& job) {
  job->ticket = budget_.whenRoom(
      static_cast<std::size_t>(job->budget.max_bytes),
      [this, alive = std::weak_ptr<const int>(lifetime_),
       weak = std::weak_ptr<Job>(job)](InFlightBudget::Reservation room) {
        const std::shared_ptr<Job> granted = weak.lock();
        if (alive.expired() || granted == nullptr) {
          return;  // the room returns at once
        }
        granted->room.emplace(std::move(room));
        granted->began = hooks_.now();
        if (granted->budget.max_bytes == 0) {
          finish(granted, Stop::kBudgetBytes);
          return;
        }
        // The VM's and the native share of the run's memory, in their default ratio.
        constexpr uint64_t kVm = scripting::BudgetLimits{}.mem_bytes;
        const uint64_t vm = granted->budget.max_bytes * kVm / InFlightBudget::kRunBytes;
        granted->options.limits.mem_bytes = static_cast<std::size_t>(vm);
        granted->options.native_budget_bytes = static_cast<std::size_t>(granted->budget.max_bytes - vm);
        // One native ledger for every run of the job: what the chain keeps for its readers counts against them.
        granted->options.native_budget = scripting::makeNativeBudget(granted->options.native_budget_bytes);
        const DatasetId dataset = granted->chain.back().node.dataset;
        granted->options.tf = hooks_.tf ? hooks_.tf(dataset) : nullptr;
        granted->tf_revision = granted->options.tf != nullptr ? granted->options.tf->revision() : 0;
        advance(granted);
      });
}

bool GridRunner::restarts(const Job& job) {
  return job.rows && job.rows->restarted;
}

bool GridRunner::cancel(const std::shared_ptr<Job>& job, std::function<void()> released) {
  if (job == nullptr || job->finished) {
    return false;
  }
  job->finished = true;
  job->done = {};
  job->rows_done = {};
  job->ticket = {};
  job->token.cancel();
  if (!job->running) {
    release(*job);
    return false;
  }
  job->released = std::move(released);  // the run's publication releases it
  return true;
}

void GridRunner::advance(const std::shared_ptr<Job>& job) {
  while (!job->finished) {
    if (replaced(*job)) {
      finish(job, Stop::kError, "on-demand: evaluation inputs were replaced");
      return;
    }
    if (job->step == 0) {
      if (job->next == job->grid.size()) {
        finish(job, Stop::kComplete);
        return;
      }
      if (job->report.bundles.size() >= job->budget.max_evaluations) {
        finish(job, Stop::kBudgetEvaluations);
        return;
      }
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(hooks_.now() - job->began).count();
      if (job->next > 0 && static_cast<uint64_t>(std::max<int64_t>(elapsed, 0)) >= job->budget.max_millis) {
        finish(job, Stop::kBudgetTime);
        return;
      }
    }
    Job::Step& step = job->chain[job->step];
    const Timestamp t = job->grid[job->next];
    const InputStager::Overrides overrides = [&chain = job->chain](std::string_view upstream, std::string_view output) {
      const auto found =
          std::ranges::find_if(chain, [upstream](const Job::Step& step) { return step.key == upstream; });
      if (found == chain.end() || !found->memo) {
        return static_cast<const InputStager::Override*>(nullptr);
      }
      const auto& outputs = found->memo->outputs;
      const auto named = std::ranges::find_if(outputs, [output](const auto& entry) { return entry.first == output; });
      return named == outputs.end() ? nullptr : &named->second;
    };
    Expected<InputStager::Staged> staged = stager_.stage(step.node, t, job->epoch, overrides);
    if (!staged && job->rows) {
      skipInstant(*job, staged.error());
      continue;
    }
    if (!staged) {
      finish(job, Stop::kError, std::move(staged.error()));
      return;
    }
    if (!(step.memo && step.memo->key && step.memo->key->serves(staged->key, t, job->tf_revision))) {
      const bool upstream = job->step + 1 < job->chain.size();
      std::optional<Result> stored =
          step.installed && upstream ? fromHistory(history_, step, *staged, t, job->tf_revision) : std::nullopt;
      if (!stored) {
        step.memo.reset();  // its previous result: its objects would count against this run
        submitRun(job, std::move(*staged));
        return;
      }
      step.memo = std::move(stored);
    }
    if (!proceed(job, *staged)) {
      return;
    }
  }
}

void GridRunner::submitRun(const std::shared_ptr<Job>& job, InputStager::Staged staged) {
  const Job::Step& step = job->chain[job->step];
  job->options.query_time = step.node.recipe.query_time ? job->grid[job->next] : staged.stamp;
  job->options.image_decoder = hooks_.image_decoder ? hooks_.image_decoder() : nullptr;
  // ponytail: a window's instants over one video topic take turns on its decoder and each seeks to its own frame, one
  // GOP per sample. Upgrade: a batched sampled decode (StreamingVideoDecoder::decodeSampled).
  job->options.video_source = hooks_.video_source ? hooks_.video_source() : nullptr;
  job->options.result_stamp = staged.stamp;
  job->staged = std::move(staged);
  job->running = true;
  EnginePool::Task task;
  task.compute = [store = &store_, run = hooks_.run, job,
                  target = job->step + 1 == job->chain.size() && !job->rows](const EnginePool::CancelToken& token) {
    const Job::Step& running = job->chain[job->step];
    job->ran = runStaged(
        *store, job->staged, run, *running.node.compiled, running.node.recipe.params_json, running.node.recipe.outputs,
        job->options, token);
    if (target && job->ran.result && !token.cancelled()) {
      job->report_outputs = reportOutputs(*job->ran.result);
    }
  };
  task.publish = [this, alive = std::weak_ptr<const int>(lifetime_),
                  guard = std::make_shared<DiscardGuard>(job)](const EnginePool::Outcome& outcome) {
    const std::shared_ptr<Job> job = std::exchange(guard->job, nullptr);
    if (alive.expired()) {
      job->running = false;
      release(*job);
      return;
    }
    collect(job, outcome);
  };
  job->token = pool_.submit(std::move(task), laneOf(step.node));
}

void GridRunner::collect(const std::shared_ptr<Job>& job, const EnginePool::Outcome& outcome) {
  job->running = false;
  if (job->finished) {
    release(*job);  // cancelled: the pool let the run go
    if (const std::function<void()> released = std::exchange(job->released, {})) {
      released();
    }
    return;
  }
  StagedRun ran = std::move(job->ran);
  if (replaced(*job)) {
    finish(job, Stop::kError, "on-demand: evaluation inputs were replaced");
    return;
  }
  if (job->rows && (outcome.status == EnginePool::Outcome::Status::kFailed || !ran.result)) {
    skipInstant(
        *job, outcome.status == EnginePool::Outcome::Status::kFailed ? outcome.error : ran.result.error().message);
    ran = {};
    advance(job);
    return;
  }
  if (outcome.status == EnginePool::Outcome::Status::kFailed) {
    finish(job, Stop::kError, outcome.error);  // the compute threw
    return;
  }
  if (!ran.result) {
    if (ran.result.error().over_budget) {
      finish(job, Stop::kBudgetBytes);
    } else {
      finish(job, Stop::kError, std::move(ran.result.error().message));
    }
    return;
  }
  Job::Step& step = job->chain[job->step];
  ScriptBundle& bundle = *ran.result;
  Result result;
  result.key = std::move(job->staged.key);
  if (bundle.observed_query_clock && step.node.recipe.query_time) {
    result.key->query_clock = job->grid[job->next];
  }
  if (bundle.observed_tf) {
    result.key->tf_revision = job->tf_revision;
  }
  if (job->step + 1 == job->chain.size() && job->rows) {
    // An installed node declares its outputs: one value per declaration, in its order.
    result.row.reserve(job->columns.size());
    for (const std::size_t index : job->columns) {
      const scripting::NamedValue* output = index < bundle.outputs.size() ? &bundle.outputs[index] : nullptr;
      const double* number = output != nullptr ? std::get_if<double>(&output->value) : nullptr;
      if (number == nullptr || !std::isfinite(*number)) {
        const std::string& name = step.node.recipe.outputs[index].name;
        const auto* unavailable = output != nullptr ? std::get_if<Unavailable>(&output->value) : nullptr;
        result.gap = number != nullptr        ? "on-demand: output '" + name + "' is not a finite number"
                     : unavailable != nullptr ? "on-demand: output '" + name + "' unavailable: " + unavailable->reason
                                              : "on-demand: output '" + name + "' unavailable";
        result.row.clear();
        break;
      }
      result.row.push_back(*number);
    }
  } else if (job->step + 1 == job->chain.size()) {
    result.report = std::move(job->report_outputs);
    if (step.node.recipe.outputs.empty()) {
      for (const scripting::NamedValue& output : bundle.outputs) {
        result.types.push_back({output.name, typeName(output.value)});
      }
    }
  } else {
    // An upstream is installed, so it declares its outputs: one value per declaration, in its order.
    const uint64_t identity = job->next_identity++;
    const std::vector<OutputSpec>& specs = step.node.recipe.outputs;
    for (std::size_t index = 0; index < specs.size() && index < bundle.outputs.size(); ++index) {
      if (specs[index].type != OutputType::kObject) {
        continue;
      }
      InputStager::Override override{
          .stamp = job->staged.stamp, .identity = {kJobIdentity, identity}, .value = Unavailable{}};
      scripting::ScriptValue& value = bundle.outputs[index].value;
      if (auto* object = std::get_if<sdk::BuiltinObject>(&value)) {
        // The output's own keepalive carries a builder's native charge, which must count while a reader holds it.
        std::vector<InputStager::Keepalive> owners = ran.keepalives;
        if (bundle.outputs[index].keepalive != nullptr) {
          owners.push_back(std::move(bundle.outputs[index].keepalive));
        }
        override.value = InputStager::Object{std::move(owners), std::move(*object)};
      } else if (auto* unavailable = std::get_if<Unavailable>(&value)) {
        override.value = std::move(*unavailable);
      }
      result.outputs.emplace_back(specs[index].name, std::move(override));
    }
  }
  step.memo = std::move(result);
  ran = {};  // what the result did not take
  const InputStager::Staged staged = std::move(job->staged);
  if (proceed(job, staged)) {
    advance(job);
  }
}

bool GridRunner::proceed(const std::shared_ptr<Job>& job, const InputStager::Staged& staged) {
  if (job->step + 1 < job->chain.size()) {
    ++job->step;
    return true;
  }
  if (job->rows) {
    const Result& result = *job->chain.back().memo;
    if (!result.gap.empty()) {
      skipInstant(*job, result.gap);
      return true;
    }
    job->rows->times.push_back(job->grid[job->next]);
    job->rows->values.insert(job->rows->values.end(), result.row.begin(), result.row.end());
    job->rows->through = job->grid[job->next];
  } else if (!addBundle(*job, staged, *job->chain.back().memo)) {
    finish(job, Stop::kBudgetReport);
    return false;
  }
  job->step = 0;
  ++job->next;
  return true;
}

void GridRunner::skipInstant(Job& job, std::string_view reason) {
  Rows& rows = *job.rows;
  if (rows.gaps++ == 0) {
    rows.first_gap = utf8Prefix(std::string(reason), kMaxErrorBytes);
  }
  rows.through = job.grid[job.next];
  job.step = 0;
  ++job.next;
}

bool GridRunner::addBundle(Job& job, const InputStager::Staged& staged, const Result& result) {
  sdk::EvaluationBundle bundle{
      .requested_ns = job.grid[job.next], .stamp_ns = staged.stamp, .inputs = {}, .outputs = result.report};
  for (const InputStager::Input& input : staged.inputs) {
    bundle.inputs.emplace(input.alias, input.resolved);
  }
  Report& report = job.report;
  std::vector<sdk::DataProcessorOutput> inferred = report.inferred_outputs;
  for (const sdk::DataProcessorOutput& output : result.types) {
    const auto known = std::ranges::find(inferred, output.name, &sdk::DataProcessorOutput::name);
    if (known == inferred.end()) {
      inferred.push_back(output);
    }
  }
  const std::size_t bytes = job.bundle_bytes + (report.bundles.empty() ? 0 : 1) + bundleJson(bundle).size();
  const std::size_t total =
      kBundlesFrame + bytes +
      envelope(report.candidates, report.bundles.size() + 1, Stop::kComplete, nullptr, report.gaps, inferred).size();
  if (total > job.budget.max_report_bytes) {
    return false;
  }
  report.inferred_outputs = std::move(inferred);
  report.bundles.push_back(std::move(bundle));
  job.bundle_bytes = bytes;
  return true;
}

void GridRunner::finish(const std::shared_ptr<Job>& job, Stop stopped, std::string error) {
  job->finished = true;
  job->ticket = {};
  release(*job);  // no run computes: a job finishes between runs
  if (job->rows) {
    Rows rows = std::move(*job->rows);
    rows.left -= std::min<uint64_t>(rows.left, job->next);
    if (stopped == Stop::kError) {
      rows.error = utf8Prefix(std::move(error), kMaxErrorBytes);
    }
    if (RowsDone done = std::exchange(job->rows_done, {})) {
      done(std::move(rows));
    }
    return;
  }
  Report& report = job->report;
  const uint64_t limit = job->budget.max_report_bytes;
  const auto size = [&](Stop stop, const std::string& reason) {
    return kBundlesFrame + job->bundle_bytes +
           envelope(
               report.candidates, report.bundles.size(), stop, stop == Stop::kError ? &reason : nullptr, report.gaps,
               report.inferred_outputs)
               .size();
  };
  // Every bundle was checked against a "complete" report: a longer stop may need the last bundle's room.
  const auto dropLast = [&] {
    job->bundle_bytes -= bundleJson(report.bundles.back()).size() + (report.bundles.size() > 1 ? 1 : 0);
    report.bundles.pop_back();
  };
  if (stopped == Stop::kError) {
    error = utf8Prefix(std::move(error), kMaxErrorBytes);
  }
  while (size(stopped, {}) > limit && !report.bundles.empty()) {
    dropLast();
    if (stopped != Stop::kError) {
      stopped = Stop::kBudgetReport;
    }
  }
  if (stopped == Stop::kError) {
    // The longest prefix whose escaped text fits: the report grows with the prefix.
    std::size_t fits = 0;
    for (std::size_t over = error.size(); fits < over;) {
      const std::size_t middle = fits + (over - fits + 1) / 2;
      if (size(stopped, utf8Prefix(error, middle)) <= limit) {
        fits = middle;
      } else {
        over = middle - 1;
      }
    }
    report.error = utf8Prefix(std::move(error), fits);
  }
  report.stopped = stopped;
  report.evaluated = report.bundles.size();
  Done done = std::move(job->done);
  job->done = {};
  if (done) {
    done(std::move(report));
  }
}

bool GridRunner::replaced(const Job& job) const {
  if ((hooks_.epoch ? hooks_.epoch(job.chain.back().node.dataset) : 0) != job.epoch) {
    return true;
  }
  return std::ranges::any_of(job.guards, [this](const auto& guard) {
    const RecipeGraph::Node* node = graph_.find(guard.first);
    return node == nullptr || node->revision != guard.second;
  });
}

std::string GridRunner::toJson(const Report& report) {
  std::string json = R"({"bundles":[)";
  for (std::size_t index = 0; index < report.bundles.size(); ++index) {
    if (index > 0) {
      json += ',';
    }
    json += bundleJson(report.bundles[index]);
  }
  json += "],";
  json.append(
      envelope(
          report.candidates, report.bundles.size(), report.stopped,
          report.stopped == Stop::kError ? &report.error : nullptr, report.gaps, report.inferred_outputs),
      1);
  return json;
}

}  // namespace PJ
