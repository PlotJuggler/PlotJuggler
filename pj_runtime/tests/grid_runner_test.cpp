// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// GridRunner: explicit evaluations over a window or at an instant (REQUIREMENTS D19). A real ObjectStore, DataEngine,
// EnginePool, InFlightBudget, RecipeGraph, ResultHistory and InputStager; a NodeScheduler beside the runner for the
// installed nodes' own cursor requests; an injected fake script run (one end-to-end case runs the real Luau engine)
// and an injected clock. Every report is read back with the SDK's `parseEvaluationReport`.

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "event_pump_test_utils.h"
#include "on_demand_test_utils.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_bench_stats.h"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/GridRunner.h"
#include "pj_runtime/NodeScheduler.h"
#include "pj_runtime/SeriesRunner.h"

namespace PJ {
namespace {

using namespace std::chrono_literals;
using scripting::NamedValue;
using scripting::OutputSpec;
using scripting::OutputType;
using scripting::RunOptions;
using scripting::ScriptBundle;
using scripting::ScriptError;
using scripting::Unavailable;
using Stop = sdk::EvaluationStop;
using OutputStatus = sdk::EvaluationOutputStatus;
using Budget = GridRunner::Budget;

constexpr Timestamp kSec = 1'000'000'000;

OutputSpec cloudSpec(std::string name = "cropped") {
  return {std::move(name), OutputType::kObject, sdk::BuiltinObjectType::kPointCloud};
}
OutputSpec numberSpec(std::string name = "count") {
  return {std::move(name), OutputType::kNumber, sdk::BuiltinObjectType::kNone};
}
OutputSpec stringSpec(std::string name = "label") {
  return {std::move(name), OutputType::kString, sdk::BuiltinObjectType::kNone};
}

/// `owner/<id>` reading `inputs`, with `outputs`; `params` configure the fake run (see `FakeRun`), which also names
/// the run by `id`.
RecipeGraph::Recipe recipe(
    const std::string& id, std::vector<std::string> inputs, std::vector<OutputSpec> outputs,
    nlohmann::json params = nlohmann::json::object()) {
  RecipeGraph::Recipe made = test::onDemandRecipe(id, std::move(inputs), std::move(outputs));
  params["id"] = id;
  made.params_json = params.dump();
  return made;
}

/// `made` in Python: `body` is the indented body of its `evaluate`, which follows `imports`.
RecipeGraph::Recipe inPython(RecipeGraph::Recipe made, const std::string& body, const std::string& imports = "") {
  made.language = "python";
  made.script = imports + "def evaluate(inputs, params):\n" + body;
  return made;
}

std::vector<uint8_t> canonicalCloud(size_t points) {
  return serializeBuiltinObject(sdk::BuiltinObject(test::makeCloud(points))).value();
}

/// A script run that records what ran. Its recipe's params say what it does: `fail` (the message) or `fail_at` (fail
/// with "boom" at that query time), `unavailable` (output names, "no points"), `empty` (empty clouds), `fresh` (a cloud
/// output gets bytes of its own, `lastFresh`), `big` (the size of every string output), `nan` (numbers are NaN),
/// `clock` (it read the query clock). Otherwise a cloud output
/// is its first cloud input (a pass-through), a number the first cloud's point count (or the first number input), a
/// string "ok"; with no declared outputs it returns `count` and `label`. Each run moves the injected clock forward by
/// `clock_step_ms`. Set the knobs while nothing runs.
class FakeRun {
 public:
  std::atomic<bool> gated{false};       ///< each run waits for `allow` (or its cancellation)
  std::atomic<bool> deaf{false};        ///< ... or only for `allow`: a native call that cannot see a cancellation
  std::atomic<size_t> vm_bytes{0};      ///< the last run's VM cap
  std::atomic<size_t> native_bytes{0};  ///< ... and its native budget
  std::atomic<int64_t> clock_ms{0};
  int64_t clock_step_ms = 0;

  NodeScheduler::RunFunction function() {
    return [this](
               const scripting::CompiledOnDemandScript&, Span<const NamedValue> inputs, std::string_view params,
               Span<const OutputSpec> outputs,
               const RunOptions& options) { return run(inputs, params, outputs, options); };
  }
  void allow(int runs) {
    const std::lock_guard<std::mutex> lock(mutex_);
    permits_ += runs;
    cv_.notify_all();
  }
  [[nodiscard]] int started() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return started_;
  }
  /// Runs of the recipe `id`, in order of their query times.
  [[nodiscard]] std::vector<Timestamp> runsOf(const std::string& id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = runs_.find(id);
    return found == runs_.end() ? std::vector<Timestamp>{} : found->second;
  }
  [[nodiscard]] int cancelledRuns() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_;
  }
  /// The bytes of the last cloud a `fresh` run made; expired once nothing holds that cloud.
  [[nodiscard]] std::weak_ptr<const void> lastFresh() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_fresh_;
  }

 private:
  Expected<ScriptBundle, ScriptError> run(
      Span<const NamedValue> inputs, std::string_view params_json, Span<const OutputSpec> outputs,
      const RunOptions& options) {
    const nlohmann::json params = nlohmann::json::parse(params_json);
    const auto cancelled = [&options] { return options.cancelled && options.cancelled(); };
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ++started_;
      cv_.notify_all();
      const auto until = std::chrono::steady_clock::now() + 5s;
      while (gated && permits_ == 0 && (deaf || !cancelled()) && std::chrono::steady_clock::now() < until) {
        cv_.wait_for(lock, 1ms);
      }
      if (gated && permits_ > 0) {
        --permits_;
      }
      runs_[params.value("id", std::string())].push_back(options.query_time);
      if (cancelled()) {
        ++cancelled_;
      }
    }
    clock_ms += clock_step_ms;
    vm_bytes = options.limits.mem_bytes;
    native_bytes = options.native_budget_bytes;
    if (cancelled()) {
      return unexpected(ScriptError{"script cancelled"});
    }
    if (params.contains("fail")) {
      return unexpected(ScriptError{params["fail"].get<std::string>()});
    }
    if (params.contains("fail_at") && params["fail_at"].get<Timestamp>() == options.query_time) {
      return unexpected(ScriptError{"boom"});
    }
    const sdk::PointCloud* cloud = nullptr;
    std::optional<double> number;
    for (const NamedValue& input : inputs) {
      if (const auto* object = std::get_if<sdk::BuiltinObject>(&input.value); object != nullptr && cloud == nullptr) {
        cloud = object->get<sdk::PointCloud>();
      } else if (const auto* value = std::get_if<double>(&input.value); value != nullptr && !number) {
        number = *value;
      }
    }
    const double count = params.value("nan", false) ? std::nan("")
                         : cloud != nullptr         ? static_cast<double>(cloud->width)
                                                    : number.value_or(0.0);
    const std::string text(params.value("big", size_t{0}), 'x');
    const std::vector<std::string> unavailable = params.value("unavailable", std::vector<std::string>{});
    ScriptBundle bundle;
    bundle.observed_query_clock = params.value("clock", false);
    if (outputs.empty()) {
      bundle.outputs.push_back({.name = "count", .value = count});
      bundle.outputs.push_back({.name = "label", .value = std::string("ok")});
      return bundle;
    }
    for (const OutputSpec& output : outputs) {
      if (std::find(unavailable.begin(), unavailable.end(), output.name) != unavailable.end()) {
        bundle.outputs.push_back({.name = output.name, .value = Unavailable{"no points"}});
      } else if (output.type == OutputType::kNumber) {
        bundle.outputs.push_back({.name = output.name, .value = count});
      } else if (output.type == OutputType::kString) {
        bundle.outputs.push_back({.name = output.name, .value = text.empty() ? std::string("ok") : text});
      } else {
        const bool empty = params.value("empty", false) || cloud == nullptr;
        sdk::PointCloud made = empty ? test::makeCloud(0) : *cloud;
        if (params.value("fresh", false) && !empty) {
          made = test::makeCloud(cloud->width);  // bytes of its own, unlike a pass-through
          const std::lock_guard<std::mutex> lock(mutex_);
          last_fresh_ = made.anchor;
        }
        bundle.outputs.push_back({.name = output.name, .value = sdk::BuiltinObject(std::move(made))});
      }
    }
    return bundle;
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  int permits_ = 0;
  int started_ = 0;
  int cancelled_ = 0;
  std::map<std::string, std::vector<Timestamp>> runs_;
  std::weak_ptr<const void> last_fresh_;
};

/// What one evaluation produced: the runner's report, its JSON and the SDK's reading of it.
struct Evaluated {
  GridRunner::Report report;
  std::string json;
  sdk::EvaluationReport parsed;
};

/// One dataset: the object topic `cloud` (canonical clouds), the scalar series `threshold`, and `dense` (and its twin
/// `dense_twin`), a scalar series with `dense_count` samples, at 0, 1, 2, ... ns, that only the change lookup and the
/// sampler know.
struct Fixture {
  explicit Fixture(std::size_t budget_bytes = 4 * InFlightBudget::kRunBytes, bool real_run = false)
      : budget(budget_bytes) {
    dataset = engine.createDataset(DatasetDescriptor{.source_name = "log"}).value();
    cloud = store.registerTopic({dataset, "cloud", ""}).value();
    threshold = writer.registerScalarSeries(dataset, "threshold", NumericType::kFloat64).value();
    NodeScheduler::Hooks scheduling;
    scheduling.epoch = [this](DatasetId) { return epoch; };
    scheduling.run = fake.function();
    scheduler = std::make_unique<NodeScheduler>(store, graph, history, stager, pool, budget, std::move(scheduling));
    runner = std::make_unique<GridRunner>(store, graph, history, stager, pool, budget, hooks(real_run));
  }

  GridRunner::Hooks hooks(bool real_run = false) {
    GridRunner::Hooks made;
    made.scalar_changes = [this](DatasetId in, std::string_view name, Timestamp start, Timestamp end, size_t limit) {
      return scalarChanges(in, name, start, end, limit);
    };
    made.epoch = [this](DatasetId) { return epoch; };
    if (!real_run) {
      made.run = fake.function();
    }
    made.now = [this] { return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(fake.clock_ms); };
    return made;
  }

  GridRunner::ScalarChanges scalarChanges(
      DatasetId in, std::string_view name, Timestamp start, Timestamp end, size_t limit) {
    GridRunner::ScalarChanges changes;
    if (in != dataset) {
      return changes;
    }
    if (name.starts_with("dense")) {
      last_dense_limit = limit;
      changes.first = 0;
      const Timestamp lo = std::max<Timestamp>(start, 0);
      const Timestamp hi = std::min<Timestamp>(end, static_cast<Timestamp>(dense_count) - 1);
      const size_t count = hi < lo ? 0 : static_cast<size_t>(hi - lo + 1);
      changes.over_limit = count > limit;
      for (Timestamp t = lo; !changes.over_limit && t <= hi; ++t) {
        changes.instants.push_back(t);
      }
      return changes;
    }
    const DataReader reader(engine);
    auto values = reader.series(threshold.topic_id, 0);
    if (name != "threshold" || !values) {
      return changes;
    }
    if (const auto first = values->sampleAtOrAfterTime(std::numeric_limits<Timestamp>::min())) {
      changes.first = first->timestamp;
    }
    values->samples(Range<Timestamp>{start, end}).forEach([&](const SeriesSample& sample) {
      changes.instants.push_back(sample.timestamp);
    });
    changes.over_limit = changes.instants.size() > limit;
    if (changes.over_limit) {
      changes.instants.clear();
    }
    return changes;
  }

  /// Installs `made` in the graph and the history; returns a copy of its node.
  RecipeGraph::Node install(const RecipeGraph::Recipe& made) {
    const Status upserted = graph.upsert(made);
    EXPECT_TRUE(upserted.has_value()) << upserted.error();
    const RecipeGraph::Node* node = graph.find(made.key());
    if (node == nullptr) {
      return {};
    }
    const auto installed = history.install(*node);
    EXPECT_TRUE(installed.has_value()) << installed.error();
    return *node;
  }
  /// A transient node: a preview recipe bound, installed nowhere.
  RecipeGraph::Node bind(RecipeGraph::Recipe made) {
    made.ephemeral = true;
    auto bound = graph.bind(std::move(made));
    EXPECT_TRUE(bound.has_value()) << bound.error();
    return bound ? std::move(*bound) : RecipeGraph::Node{};
  }
  /// Pumps the event loop (a job's room comes from a drain) until `runs` runs have started; false after 5 s.
  [[nodiscard]] bool waitStarted(int runs) {
    test::pumpUntil([&] { return fake.started() >= runs; }, 5000);
    return fake.started() >= runs;
  }
  void pushCloud(Timestamp t, size_t points = 4) {
    ASSERT_TRUE(store.pushOwned(cloud, t, canonicalCloud(points)).has_value());
  }
  void pushThreshold(Timestamp t, double value) {
    writer.appendScalar(threshold, t, value);
    EXPECT_FALSE(engine.commitChunks(writer.flushAll()).empty());
  }
  /// The cursor's own request of an installed node, settled.
  void request(std::string_view key, Timestamp t) {
    scheduler->requestAt(key, t);
    pool.waitIdle();
  }

  [[nodiscard]] Expected<std::shared_ptr<GridRunner::Job>> submit(
      const RecipeGraph::Node& node, bool installed, Timestamp start, Timestamp end,
      std::optional<GridRunner::Report>& done, Budget limits = {}) {
    return runner->submit(
        node, installed, start, end, limits, [&done](GridRunner::Report report) { done = std::move(report); });
  }
  /// Evaluates `node` over [start, end] to its report, which must parse and fit its budget.
  Evaluated evaluate(
      const RecipeGraph::Node& node, bool installed, Timestamp start, Timestamp end, Budget limits = {}) {
    std::optional<GridRunner::Report> done;
    auto job = submit(node, installed, start, end, done, limits);
    if (!job) {
      ADD_FAILURE() << "submit failed: " << job.error();
      return {};
    }
    // Each run's publication submits the next, so waitIdle carries the job to its end. `pumpUntil` sleeps once per
    // run, and a Windows sleep can last a timer tick: the 2000 runs of a capped job would take most of its timeout.
    QCoreApplication::processEvents();  // the job's room, which submits its first run
    pool.waitIdle();
    test::pumpUntil([&done] { return done.has_value(); }, 20000);
    if (!done) {
      ADD_FAILURE() << "no report";
      return {};
    }
    return read(std::move(*done), limits);
  }
  static Evaluated read(GridRunner::Report report, const Budget& limits = {}) {
    Evaluated out{std::move(report), {}, {}};
    out.json = GridRunner::toJson(out.report);
    auto parsed = sdk::parseEvaluationReport(out.json);
    EXPECT_TRUE(parsed.has_value()) << parsed.error() << "\n" << out.json.substr(0, 2000);
    if (parsed) {
      out.parsed = std::move(*parsed);
    }
    EXPECT_LE(out.json.size(), std::min(limits.max_report_bytes, GridRunner::kHostCaps.max_report_bytes));
    return out;
  }
  [[nodiscard]] ObjectTopicId topic(const std::string& name) const {
    return store.findTopic(dataset, name).value_or(ObjectTopicId{});
  }

  DataEngine engine;
  DataWriter writer{engine.createWriter()};
  ObjectStore store;
  DatasetId dataset = 0;
  ObjectTopicId cloud;
  ScalarSeriesHandle threshold;
  size_t dense_count = 0;
  size_t last_dense_limit = 0;
  uint64_t epoch = 0;
  RecipeGraph graph{[this](std::string_view input) {
    if (store.findTopic(dataset, input)) {
      return std::vector<RecipeGraph::InputCandidate>{
          {dataset, sdk::BuiltinObjectType::kPointCloud, std::string(input)}};
    }
    if (input == "threshold" || input.starts_with("dense")) {
      return std::vector<RecipeGraph::InputCandidate>{{dataset, sdk::BuiltinObjectType::kNone, std::string(input)}};
    }
    return std::vector<RecipeGraph::InputCandidate>{};
  }};
  ResultHistory history{store};
  InputStager stager{
      store, history,
      [this](DatasetId in, std::string_view name, Timestamp t) -> std::optional<InputStager::ScalarSample> {
        if (in != dataset) {
          return std::nullopt;
        }
        if (name.starts_with("dense")) {
          if (t < 0) {
            return std::nullopt;
          }
          const Timestamp stamp = std::min<Timestamp>(t, static_cast<Timestamp>(dense_count) - 1);
          return InputStager::ScalarSample{
              .topic = threshold.topic_id,
              .column = 1,
              .timestamp = stamp,
              .chunk = 0,
              .row = static_cast<std::size_t>(stamp),
              .value = 0.0};
        }
        return name == "threshold" ? InputStager::sampleAt(engine, threshold.topic_id, 0, t) : std::nullopt;
      },
      [this](ObjectTopicId topic) { return parsers ? parsers(topic) : SessionManager::ParserBinding{}; }};
  std::function<SessionManager::ParserBinding(ObjectTopicId)> parsers;  ///< empty: no topic has a parser
  FakeRun fake;
  InFlightBudget budget;  ///< outlives the pool: a run still on it holds its reservation
  EnginePool pool;        ///< after the store, the fake and the budget: destroyed first, joining the runs
  std::unique_ptr<NodeScheduler> scheduler;
  std::unique_ptr<GridRunner> runner;
};

/// Clouds at 0, 5, 10 and 20 s (2, 2, 3 and 4 points) and a threshold at 0 and 15 s.
void pushReferenceData(Fixture& fx) {
  fx.pushCloud(0, 2);
  fx.pushCloud(5 * kSec, 2);
  fx.pushCloud(10 * kSec, 3);
  fx.pushCloud(20 * kSec, 4);
  fx.pushThreshold(0, 1.0);
  fx.pushThreshold(15 * kSec, 2.0);
}

std::vector<int64_t> requestedTimes(const sdk::EvaluationReport& report) {
  std::vector<int64_t> times;
  for (const auto& bundle : report.bundles) {
    times.push_back(bundle.requested_ns);
  }
  return times;
}

double numberAt(const sdk::EvaluationReport& report, size_t bundle, const std::string& output = "count") {
  if (bundle >= report.bundles.size() || !report.bundles[bundle].outputs.contains(output)) {
    ADD_FAILURE() << "no output " << output << " in bundle " << bundle;
    return -1;
  }
  const auto& value = report.bundles[bundle].outputs.at(output).value;
  return value.is_number() ? value.get<double>() : -1;
}

// ---- The grid -----------------------------------------------------------------------------------------------

// EVAL-01: a window evaluates at its start and at every instant inside it where an input changes, in time order,
// each input resolved independently at or before the instant (D5); an instant is exactly its time.
TEST(GridRunnerTest, WindowGridIsTheUnionOfInputChangeInstants) {
  Fixture fx;
  pushReferenceData(fx);
  const RecipeGraph::Node crop = fx.bind(recipe("crop", {"cloud", "threshold"}, {numberSpec()}));

  const Evaluated window = fx.evaluate(crop, false, 0, 25 * kSec);
  EXPECT_EQ(window.parsed.candidates, 5u);
  EXPECT_EQ(window.parsed.evaluated, 5u);
  EXPECT_EQ(window.parsed.stopped, Stop::kComplete);
  EXPECT_EQ(requestedTimes(window.parsed), (std::vector<int64_t>{0, 5 * kSec, 10 * kSec, 15 * kSec, 20 * kSec}));
  ASSERT_EQ(window.parsed.bundles.size(), 5u);
  const auto& at15 = window.parsed.bundles[3];
  EXPECT_EQ(at15.inputs.at("cloud"), 10 * kSec);
  EXPECT_EQ(at15.inputs.at("threshold"), 15 * kSec);
  EXPECT_EQ(at15.stamp_ns, 15 * kSec);
  EXPECT_EQ(numberAt(window.parsed, 3), 3.0);

  const Evaluated instant = fx.evaluate(crop, false, 12 * kSec, 12 * kSec);
  EXPECT_EQ(instant.parsed.candidates, 1u);
  ASSERT_EQ(instant.parsed.bundles.size(), 1u);
  EXPECT_EQ(instant.parsed.bundles[0].requested_ns, 12 * kSec);
  EXPECT_EQ(instant.parsed.bundles[0].inputs.at("cloud"), 10 * kSec);
  EXPECT_EQ(instant.parsed.bundles[0].inputs.at("threshold"), 0);
}

// The grid is admitted by its unique instants, counted before it is allocated: synchronized inputs, and an input whose
// first instant is the window's start, count each instant once; one instant past the cap is refused, and so is a scalar
// input past it alone. An end before the start is refused too.
TEST(GridRunnerTest, GridOverflowIsRejectedBeforeAllocating) {
  Fixture fx;
  fx.pushCloud(0);  // retained from the window's start: no leading gap skips the dense samples
  fx.pushCloud(5 * kSec);
  fx.pushCloud(10 * kSec);
  std::optional<GridRunner::Report> done;
  fx.dense_count = GridRunner::kMaxGridInstants;  // at 0, 1, 2, ... ns: the first is the window's start
  const RecipeGraph::Node synced = fx.bind(recipe("synced", {"dense", "dense_twin"}, {numberSpec()}));
  const auto admitted = fx.submit(synced, false, 0, 20 * kSec, done);
  ASSERT_TRUE(admitted.has_value()) << admitted.error();
  fx.runner->cancel(*admitted);
  EXPECT_EQ(fx.last_dense_limit, GridRunner::kMaxGridInstants) << "each scalar input may hold the whole cap";

  const RecipeGraph::Node node = fx.bind(recipe("crop", {"cloud", "dense"}, {numberSpec()}));
  fx.dense_count = GridRunner::kMaxGridInstants - 1;  // the clouds at 5 and 10 s make one instant too many
  const auto over = fx.submit(node, false, 0, 20 * kSec, done);
  ASSERT_FALSE(over.has_value());
  EXPECT_NE(over.error().find("input change instants"), std::string::npos) << over.error();
  fx.dense_count = GridRunner::kMaxGridInstants + 1;
  const auto alone = fx.submit(node, false, 0, 20 * kSec, done);
  ASSERT_FALSE(alone.has_value());
  EXPECT_NE(alone.error().find("input change instants"), std::string::npos) << alone.error();

  const auto reversed = fx.submit(node, false, 10 * kSec, 5 * kSec, done);
  ASSERT_FALSE(reversed.has_value());
  EXPECT_EQ(reversed.error(), "on-demand: the window ends before it starts");
  test::pumpFor(20);
  EXPECT_FALSE(done.has_value());
}

// DAG-08: a chained window takes its grid from the raw leaves of the whole chain, and evaluates the upstream itself:
// nothing is materialized for the reader or the upstream.
TEST(GridRunnerTest, ChainedWindowUsesRawLeafGridBeforeAnyMaterialization) {
  Fixture fx;
  pushReferenceData(fx);
  fx.install(recipe("crop", {"cloud", "threshold"}, {cloudSpec()}));
  const RecipeGraph::Node child = fx.bind(recipe("child", {"owner/crop/cropped"}, {numberSpec()}));

  const Evaluated report = fx.evaluate(child, false, 5 * kSec, 20 * kSec);
  EXPECT_EQ(report.parsed.stopped, Stop::kComplete) << report.parsed.error;
  EXPECT_EQ(report.parsed.candidates, 4u) << "clouds at 5, 10, 20 and the threshold at 15";
  ASSERT_EQ(report.parsed.bundles.size(), 4u);
  EXPECT_EQ(report.parsed.bundles[2].requested_ns, 15 * kSec);
  EXPECT_EQ(report.parsed.bundles[2].inputs.at("owner/crop/cropped"), 15 * kSec);
  EXPECT_EQ(numberAt(report.parsed, 2), 3.0);
  EXPECT_FALSE(fx.store.findTopic(fx.dataset, "owner/child/count").has_value());
  EXPECT_EQ(fx.store.entryCount(fx.topic("owner/crop/cropped")), 0u) << "the upstream's ring is untouched";
}

// DAG-11 (decided 2026-10-18): a pinned upstream is read, not evaluated. It adds its real stamp to the grid, not its
// raw inputs' instants, and a reader resolves it only from that stamp.
TEST(GridRunnerTest, PinnedUpstreamContributesItsRealStampToTheWindowGrid) {
  Fixture fx;
  pushReferenceData(fx);
  auto finding = recipe("crop", {"cloud"}, {cloudSpec()});
  finding.pinned_t = 12 * kSec;
  fx.install(finding);
  fx.request("owner/crop", 0);  // computed at its pin: the cloud at 10 s, 3 points
  const RecipeGraph::Node child = fx.bind(recipe("child", {"owner/crop/cropped"}, {numberSpec()}));

  const Evaluated crossing = fx.evaluate(child, false, 5 * kSec, 25 * kSec);
  EXPECT_EQ(crossing.parsed.candidates, 2u) << "the start and the finding's real stamp; not the cloud at 20 s";
  EXPECT_EQ(crossing.parsed.stopped, Stop::kError);
  EXPECT_EQ(crossing.parsed.error, "on-demand: missing input 'owner/crop/cropped'");

  const Evaluated from_pin = fx.evaluate(child, false, 10 * kSec, 25 * kSec);
  EXPECT_EQ(from_pin.parsed.stopped, Stop::kComplete) << from_pin.parsed.error;
  EXPECT_EQ(from_pin.parsed.candidates, 1u);
  ASSERT_EQ(from_pin.parsed.bundles.size(), 1u);
  EXPECT_EQ(numberAt(from_pin.parsed, 0), 3.0);

  const Evaluated after_pin = fx.evaluate(child, false, 21 * kSec, 25 * kSec);
  ASSERT_EQ(after_pin.parsed.bundles.size(), 1u);
  EXPECT_EQ(after_pin.parsed.bundles[0].inputs.at("owner/crop/cropped"), 10 * kSec) << "its real stamp";
  EXPECT_EQ(fx.fake.runsOf("crop").size(), 1u) << "the finding never runs again";
}

// A window entirely before a finding cannot read it, although the display shows the finding at any time.
TEST(GridRunnerTest, WindowBeforeAFindingCannotReadIt) {
  Fixture fx;
  pushReferenceData(fx);
  auto finding = recipe("crop", {"cloud"}, {cloudSpec()});
  finding.pinned_t = 10 * kSec;
  fx.install(finding);
  fx.request("owner/crop", 0);
  ASSERT_TRUE(fx.store.latestAt(fx.topic("owner/crop/cropped"), 0).has_value()) << "shown at any time";
  const RecipeGraph::Node child = fx.bind(recipe("child", {"owner/crop/cropped"}, {numberSpec()}));

  const Evaluated before = fx.evaluate(child, false, 0, 9 * kSec);
  EXPECT_EQ(before.parsed.candidates, 1u);
  EXPECT_EQ(before.parsed.evaluated, 0u);
  EXPECT_EQ(before.parsed.stopped, Stop::kError);
  EXPECT_EQ(before.parsed.error, "on-demand: missing input 'owner/crop/cropped'");
}

// EVAL-04: a window starting before an input's oldest retained sample reports that leading part as a gap and
// evaluates from its end, the newest of the raw inputs' oldest samples, so inputs that begin at different times still
// give bundles; a window the gap covers whole evaluates nothing.
TEST(GridRunnerTest, WindowReportsGapsBeforeRetention) {
  Fixture fx;
  fx.pushCloud(5 * kSec);
  fx.pushCloud(10 * kSec);
  fx.pushCloud(20 * kSec);
  fx.pushThreshold(8 * kSec, 1.0);
  const RecipeGraph::Node crop = fx.bind(recipe("crop", {"cloud", "threshold"}, {numberSpec()}));
  using Gaps = std::vector<std::pair<int64_t, int64_t>>;
  const auto gaps = [](const Evaluated& evaluated) {
    Gaps out;
    for (const auto& gap : evaluated.parsed.gaps) {
      out.emplace_back(gap.from_ns, gap.to_ns);
    }
    return out;
  };

  const Evaluated retained = fx.evaluate(crop, false, 8 * kSec, 20 * kSec);
  EXPECT_TRUE(gaps(retained).empty());
  EXPECT_EQ(retained.parsed.stopped, Stop::kComplete) << retained.parsed.error;

  const Evaluated leading = fx.evaluate(crop, false, 0, 20 * kSec);
  EXPECT_EQ(gaps(leading), (Gaps{{0, 8 * kSec}})) << "up to the threshold's oldest sample, the newest oldest";
  EXPECT_EQ(leading.parsed.stopped, Stop::kComplete) << leading.parsed.error;
  EXPECT_EQ(leading.parsed.candidates, 3u);
  EXPECT_EQ(requestedTimes(leading.parsed), (std::vector<int64_t>{8 * kSec, 10 * kSec, 20 * kSec}));
  ASSERT_EQ(leading.parsed.bundles.size(), 3u);
  EXPECT_EQ(leading.parsed.bundles[0].inputs.at("cloud"), 5 * kSec);

  fx.store.evictBefore(fx.cloud, 12 * kSec);
  const Evaluated evicted = fx.evaluate(crop, false, 0, 20 * kSec);
  EXPECT_EQ(gaps(evicted), (Gaps{{0, 20 * kSec}}));
  EXPECT_EQ(evicted.parsed.stopped, Stop::kComplete) << evicted.parsed.error;
  EXPECT_EQ(requestedTimes(evicted.parsed), (std::vector<int64_t>{20 * kSec}));

  const Evaluated covered = fx.evaluate(crop, false, 0, 15 * kSec);
  EXPECT_EQ(gaps(covered), (Gaps{{0, 15 * kSec}}));
  EXPECT_EQ(covered.parsed.stopped, Stop::kComplete) << covered.parsed.error;
  EXPECT_EQ(covered.parsed.candidates, 0u);
  EXPECT_EQ(covered.parsed.evaluated, 0u);
  EXPECT_TRUE(gaps(fx.evaluate(crop, false, 20 * kSec, 25 * kSec)).empty());
}

// ---- Series rows (the column sink) ---------------------------------------------------------------------------

/// The rows of a series chunk of `node` (installed) after `after`, at most `limit` instants up to `until`, past the
/// previous chunk's `seen`; nullopt when nothing was submitted (an empty clock).
std::optional<GridRunner::Rows> rowsOf(
    Fixture& fx, const RecipeGraph::Node& node, std::optional<Timestamp> after = std::nullopt, size_t limit = 64,
    SequentialUID seen = {}, std::optional<Timestamp> until = std::nullopt) {
  std::optional<GridRunner::Rows> done;
  auto job = fx.runner->submitRows(
      node, after, until, seen, limit, [&done](GridRunner::Rows rows) { done = std::move(rows); });
  if (!job) {
    ADD_FAILURE() << "submitRows failed: " << job.error();
    return std::nullopt;
  }
  if (*job == nullptr) {
    return std::nullopt;
  }
  test::pumpUntil([&done] { return done.has_value(); }, 20000);
  EXPECT_TRUE(done.has_value()) << "no rows";
  return done;
}

// SER-02, SER-03, D10: a chunk runs at each object entry of its clock (a scalar side input's change adds no instant),
// writes one row per instant whose number outputs are all finite, and leaves a gap for a failed run, counting it and
// keeping its reason, without stopping. `after` and `limit` slice the clock; an empty clock submits nothing.
TEST(GridRunnerTest, RowsRunEachClockInstantOnceAndGapTheFailures) {
  Fixture fx;
  pushReferenceData(fx);  // clouds at 0, 5, 10, 20 s; the threshold changes at 15 s
  const RecipeGraph::Node count = fx.install(
      recipe("count", {"cloud", "threshold"}, {numberSpec(), numberSpec("twice")}, {{"fail_at", 10 * kSec}}));

  const auto rows = rowsOf(fx, count);
  ASSERT_TRUE(rows.has_value());
  EXPECT_EQ(rows->times, (std::vector<Timestamp>{0, 5 * kSec, 20 * kSec}));
  EXPECT_EQ(rows->values, (std::vector<double>{2, 2, 2, 2, 4, 4})) << "row-major, both columns";
  EXPECT_EQ(rows->through, 20 * kSec);
  EXPECT_EQ(rows->gaps, 1u);
  EXPECT_EQ(rows->first_gap, "boom");
  EXPECT_EQ(rows->left, 0u);
  EXPECT_TRUE(rows->error.empty());
  EXPECT_EQ(fx.fake.runsOf("count"), (std::vector<Timestamp>{0, 5 * kSec, 10 * kSec, 20 * kSec}));

  const auto sliced = rowsOf(fx, count, 0, 2);
  ASSERT_TRUE(sliced.has_value());
  EXPECT_EQ(sliced->times, (std::vector<Timestamp>{5 * kSec}));
  EXPECT_EQ(sliced->through, 10 * kSec);
  EXPECT_EQ(sliced->left, 1u) << "the entry at 20 s";
  EXPECT_FALSE(rowsOf(fx, count, 20 * kSec).has_value()) << "nothing after the last entry";
  EXPECT_EQ(fx.budget.reserved(), 0u);
}

// GAP-55: a chunk resumed after a watermark starts again from the clock's first instant when an entry arrived since the
// previous chunk's cut before the watermark (out of order); a newer append only extends the clock after it, and an
// entry at the watermark's stamp (a latched source republishing it) changes nothing.
TEST(GridRunnerTest, RowsRestartWhenAnEntryArrivesBeforeTheWatermark) {
  Fixture fx;
  for (const Timestamp t : {0 * kSec, 10 * kSec, 20 * kSec}) {
    fx.pushCloud(t, 2);
  }
  const RecipeGraph::Node count = fx.install(recipe("count", {"cloud"}, {numberSpec()}));
  const auto first = rowsOf(fx, count);
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(first->restarted);
  EXPECT_FALSE(rowsOf(fx, count, 20 * kSec, 64, first->seen).has_value()) << "nothing new";

  fx.pushCloud(30 * kSec, 2);
  const auto newer = rowsOf(fx, count, 20 * kSec, 64, first->seen);
  ASSERT_TRUE(newer.has_value());
  EXPECT_FALSE(newer->restarted);
  EXPECT_EQ(newer->times, (std::vector<Timestamp>{30 * kSec}));

  fx.pushCloud(15 * kSec, 2);  // out of order, under the watermark
  const auto late = rowsOf(fx, count, 30 * kSec, 64, newer->seen);
  ASSERT_TRUE(late.has_value());
  EXPECT_TRUE(late->restarted);
  EXPECT_EQ(late->times, (std::vector<Timestamp>{0, 10 * kSec, 15 * kSec, 20 * kSec, 30 * kSec}));
  EXPECT_EQ(late->left, 0u);

  fx.pushCloud(30 * kSec, 3);  // republishes the watermark's stamp
  EXPECT_FALSE(rowsOf(fx, count, 30 * kSec, 64, late->seen).has_value()) << "nothing to compute, nothing replayed";
}

// GAP-60: `until` ends the clock (a live dataset's edge): the instants after it are neither evaluated nor counted.
TEST(GridRunnerTest, RowsStopAtUntil) {
  Fixture fx;
  pushReferenceData(fx);  // clouds at 0, 5, 10, 20 s
  const RecipeGraph::Node count = fx.install(recipe("count", {"cloud"}, {numberSpec()}));
  const auto capped = rowsOf(fx, count, std::nullopt, 64, {}, 10 * kSec);
  ASSERT_TRUE(capped.has_value());
  EXPECT_EQ(capped->times, (std::vector<Timestamp>{0, 5 * kSec, 10 * kSec}));
  EXPECT_EQ(capped->left, 0u);
  EXPECT_FALSE(rowsOf(fx, count, 10 * kSec, 64, capped->seen, 15 * kSec).has_value()) << "nothing up to 15 s";
}

// GAP-61: a series chunk's time budget is `kRowsMaxMillis`, not an evaluation's default, so a playhead request waits
// behind it for about that long at most on a lane of one thread.
TEST(GridRunnerTest, ARowsChunkEndsOnItsOwnTimeBudget) {
  Fixture fx;
  for (int index = 0; index < 10; ++index) {
    fx.pushCloud(index * kSec, 2);
  }
  fx.fake.clock_step_ms = 40;  // each run: 0 + 40 + 40 reach 100 ms after the third instant
  const RecipeGraph::Node count = fx.install(recipe("count", {"cloud"}, {numberSpec()}));
  const auto rows = rowsOf(fx, count);
  ASSERT_TRUE(rows.has_value());
  EXPECT_EQ(rows->times.size(), 3u);
  EXPECT_EQ(rows->left, 7u);
}

// GAP-17: the clock is the union of every object input's entries, each instant once; a chain adds its upstream's raw
// object inputs, and the upstream's ring stays untouched (SER-10).
TEST(GridRunnerTest, RowsClockIsTheUnionOfTheChainsObjectInputs) {
  Fixture fx;
  fx.pushCloud(0, 2);
  fx.pushCloud(10 * kSec, 3);
  const ObjectTopicId fast = fx.store.registerTopic({fx.dataset, "fast", ""}).value();
  for (const Timestamp t : {0 * kSec, 4 * kSec, 8 * kSec}) {
    ASSERT_TRUE(fx.store.pushOwned(fast, t, canonicalCloud(1)).has_value());
  }
  fx.install(recipe("crop", {"cloud"}, {cloudSpec()}));
  const RecipeGraph::Node reader = fx.install(recipe("reader", {"owner/crop/cropped", "fast"}, {numberSpec()}));

  const auto rows = rowsOf(fx, reader);
  ASSERT_TRUE(rows.has_value());
  EXPECT_EQ(rows->times, (std::vector<Timestamp>{0, 4 * kSec, 8 * kSec, 10 * kSec}));
  EXPECT_EQ(rows->values, (std::vector<double>{2, 2, 2, 3})) << "the first cloud input is the upstream's crop";
  EXPECT_EQ(rows->gaps, 0u) << rows->first_gap;
  EXPECT_EQ(fx.fake.runsOf("crop"), (std::vector<Timestamp>{0, 10 * kSec})) << "the upstream runs once per entry";
  EXPECT_EQ(fx.store.entryCount(fx.topic("owner/crop/cropped")), 0u) << "the upstream's ring is untouched";
}

// SER-04: a side input with nothing at or before an instant is that instant's gap ("missing input"); an unavailable or
// non-finite number is one too, naming the output.
TEST(GridRunnerTest, RowsGapAMissingSideInputAndAnUnavailableOrNonFiniteNumber) {
  Fixture fx;
  fx.pushCloud(0, 2);
  fx.pushCloud(5 * kSec, 2);
  fx.pushCloud(10 * kSec, 3);
  fx.pushThreshold(5 * kSec, 1.0);
  const RecipeGraph::Node side = fx.install(recipe("side", {"cloud", "threshold"}, {numberSpec()}));
  const auto rows = rowsOf(fx, side);
  ASSERT_TRUE(rows.has_value());
  EXPECT_EQ(rows->times, (std::vector<Timestamp>{5 * kSec, 10 * kSec}));
  EXPECT_EQ(rows->gaps, 1u);
  EXPECT_EQ(rows->first_gap, "on-demand: missing input 'threshold'");

  const RecipeGraph::Node unavailable =
      fx.install(recipe("unavailable", {"cloud"}, {numberSpec()}, {{"unavailable", {"count"}}}));
  const auto none = rowsOf(fx, unavailable);
  ASSERT_TRUE(none.has_value());
  EXPECT_TRUE(none->times.empty());
  EXPECT_EQ(none->gaps, 3u);
  EXPECT_EQ(none->first_gap, "on-demand: output 'count' unavailable: no points");
  const RecipeGraph::Node nan = fx.install(recipe("nan", {"cloud"}, {numberSpec()}, {{"nan", true}}));
  const auto not_finite = rowsOf(fx, nan);
  ASSERT_TRUE(not_finite.has_value());
  EXPECT_EQ(not_finite->gaps, 3u);
  EXPECT_EQ(not_finite->first_gap, "on-demand: output 'count' is not a finite number");
}

// GAP-18: a series replay holds one reservation per chunk, so a cursor request waiting in the budget's line runs
// between two chunks instead of after the whole replay.
TEST(GridRunnerTest, CursorRequestsAreNotStarvedBehindASeriesReplay) {
  Fixture fx(InFlightBudget::kRunBytes);
  for (int index = 0; index < 100; ++index) {
    fx.pushCloud(index * kSec, 2);
  }
  fx.install(recipe("crop", {"cloud"}, {cloudSpec()}));
  const RecipeGraph::Node count = fx.install(recipe("count", {"cloud"}, {numberSpec()}));
  SeriesRunner series(fx.engine, fx.graph, *fx.runner, {});
  constexpr int kChunk = static_cast<int>(SeriesRunner::kInstantsPerChunk);
  fx.fake.gated = true;
  series.install(count, /*demanded=*/false);
  series.request("owner/count");
  ASSERT_TRUE(fx.waitStarted(1)) << "the first chunk has the room";
  fx.scheduler->requestAt("owner/crop", 50 * kSec);  // waits in the line behind it
  fx.fake.allow(kChunk);
  ASSERT_TRUE(fx.waitStarted(kChunk + 1));
  fx.fake.allow(1);
  test::pumpUntil([&] { return !fx.fake.runsOf("crop").empty(); });
  EXPECT_EQ(fx.fake.runsOf("crop"), (std::vector<Timestamp>{50 * kSec}));
  EXPECT_EQ(fx.fake.runsOf("count").size(), SeriesRunner::kInstantsPerChunk) << "the cursor ran before the next chunk";

  fx.fake.allow(1000);
  test::pumpUntil([&] { return series.state("owner/count")->complete; }, 5000);
  EXPECT_EQ(series.state("owner/count")->rows, 100u);
}

// D30 under series load: with 32 series wanted, a cursor request waits for at most one series chunk, not one per
// series: the series take turns outside the budget's line.
TEST(GridRunnerTest, ACursorRequestWaitsForAtMostOneSeriesChunk) {
  Fixture fx(InFlightBudget::kRunBytes);
  for (const Timestamp t : {0 * kSec, 5 * kSec, 10 * kSec}) {
    fx.pushCloud(t, 2);
  }
  fx.install(recipe("crop", {"cloud"}, {cloudSpec()}));
  SeriesRunner series(fx.engine, fx.graph, *fx.runner, {});
  constexpr int kSeries = 32;
  constexpr int kChunkRuns = 3;  // one chunk covers the whole clock
  fx.fake.gated = true;
  for (int index = 0; index < kSeries; ++index) {
    series.install(fx.install(recipe("s" + std::to_string(index), {"cloud"}, {numberSpec()})), /*demanded=*/true);
  }
  ASSERT_TRUE(fx.waitStarted(1)) << "the first chunk has the room";
  fx.scheduler->requestAt("owner/crop", 10 * kSec);
  fx.fake.allow(kChunkRuns);
  ASSERT_TRUE(fx.waitStarted(kChunkRuns + 1));
  fx.fake.allow(1);
  test::pumpUntil([&] { return !fx.fake.runsOf("crop").empty(); }, 2000);
  EXPECT_EQ(fx.fake.runsOf("crop"), (std::vector<Timestamp>{10 * kSec})) << "the cursor waited for one chunk at most";

  fx.fake.allow(1 << 20);
  const auto complete = [&] {
    for (int index = 0; index < kSeries; ++index) {
      const auto state = series.state("owner/s" + std::to_string(index));
      if (!state || !state->complete || state->replaying) {
        return false;
      }
    }
    return true;
  };
  test::pumpUntil(complete, 10000);
  ASSERT_TRUE(complete());
  for (int index = 0; index < kSeries; ++index) {
    EXPECT_EQ(series.state("owner/s" + std::to_string(index))->rows, 3u);
  }
}

// The series waiting their turn get it when the computing one stops being wanted, not only when a chunk ends.
TEST(GridRunnerTest, HidingTheComputingSeriesHandsTheTurnOn) {
  Fixture fx;
  fx.pushCloud(0, 2);
  SeriesRunner series(fx.engine, fx.graph, *fx.runner, {});
  fx.fake.gated = true;
  series.install(fx.install(recipe("first", {"cloud"}, {numberSpec()})), /*demanded=*/true);
  series.install(fx.install(recipe("second", {"cloud"}, {numberSpec()})), /*demanded=*/true);
  ASSERT_TRUE(fx.waitStarted(1));
  EXPECT_TRUE(series.state("owner/second")->replaying) << "waiting its turn";
  series.setDemand(fx.dataset, {"owner/second"});
  EXPECT_FALSE(series.state("owner/first")->replaying);
  ASSERT_TRUE(fx.waitStarted(2)) << "the second series got the turn";
  fx.fake.allow(1 << 20);
  test::pumpUntil([&] { return series.state("owner/second")->complete; }, 5000);
  EXPECT_EQ(series.state("owner/second")->rows, 1u);
}

// A replay whose every instant is a gap (a side input that starts after the clock) stages and skips on the GUI thread
// without a run; its chunks still take an event-loop pass each, so no pass evaluates more than a chunk or two.
TEST(GridRunnerTest, AGapOnlyReplayYieldsToTheEventLoopBetweenChunks) {
  Fixture fx;
  constexpr uint64_t kEntries = 5'000;  // 79 chunks
  const std::vector<uint8_t> payload = canonicalCloud(1);
  for (uint64_t index = 0; index < kEntries; ++index) {
    ASSERT_TRUE(fx.store.pushOwned(fx.cloud, static_cast<Timestamp>(index) * 1000, payload).has_value());
  }
  fx.pushThreshold(static_cast<Timestamp>(kEntries) * 1000, 1.0);  // after every entry: each instant misses it
  const RecipeGraph::Node node = fx.install(recipe("count", {"cloud", "threshold"}, {numberSpec()}));
  SeriesRunner series(fx.engine, fx.graph, *fx.runner, {});
  series.install(node, /*demanded=*/true);

  uint64_t last = 0;
  uint64_t longest = 0;  // the most instants evaluated within one event-loop pass
  const auto sample = [&] {
    const uint64_t done = series.state("owner/count")->done;
    longest = std::max(longest, done - last);
    last = done;
  };
  std::function<void()> tick;
  QObject context;  // destroyed before `tick`: drops the tick still posted
  tick = [&] {
    sample();
    QMetaObject::invokeMethod(&context, tick, Qt::QueuedConnection);  // runs in the next pass
  };
  QMetaObject::invokeMethod(&context, tick, Qt::QueuedConnection);
  const auto complete = [&] {
    const auto state = series.state("owner/count");
    return state->complete && !state->replaying;
  };
  test::pumpUntil(complete, 30000);
  sample();
  ASSERT_TRUE(complete());
  EXPECT_EQ(series.state("owner/count")->failed, kEntries);
  EXPECT_EQ(series.state("owner/count")->first_error, "on-demand: missing input 'threshold'");
  EXPECT_LE(longest, 2 * SeriesRunner::kInstantsPerChunk) << "one pass ran " << longest << " instants";
}

// GAP-62, D30: a cancelled chunk whose run still holds the lane's thread counts as the one chunk in the runner until
// the pool lets it go, so the next series' chunk does not queue ahead of a playhead request meanwhile.
TEST(GridRunnerTest, ACancelledChunkHoldsTheTurnUntilItsRunEnds) {
  Fixture fx(InFlightBudget::kRunBytes);
  for (const Timestamp t : {0 * kSec, 5 * kSec, 10 * kSec}) {
    fx.pushCloud(t, 2);
  }
  fx.install(recipe("crop", {"cloud"}, {cloudSpec()}));
  SeriesRunner series(fx.engine, fx.graph, *fx.runner, {});
  fx.fake.gated = true;
  fx.fake.deaf = true;
  series.install(fx.install(recipe("first", {"cloud"}, {numberSpec()})), /*demanded=*/true);
  series.install(fx.install(recipe("second", {"cloud"}, {numberSpec()})), /*demanded=*/true);
  ASSERT_TRUE(fx.waitStarted(1));
  series.setDemand(fx.dataset, {"owner/second"});  // cancels the first chunk; its run keeps the thread
  test::pumpFor(20);
  fx.scheduler->requestAt("owner/crop", 10 * kSec);
  fx.fake.allow(2);  // the cancelled run, then whichever is first in the line
  test::pumpUntil([&] { return fx.fake.started() >= 2 && fx.pool.pendingPublishes() == 0; }, 2000);
  test::pumpFor(20);
  EXPECT_EQ(fx.fake.runsOf("crop"), (std::vector<Timestamp>{10 * kSec})) << "the playhead went first";
  EXPECT_TRUE(fx.fake.runsOf("second").empty());

  fx.fake.allow(1 << 20);
  test::pumpUntil([&] { return series.state("owner/second")->complete; }, 5000);
  EXPECT_EQ(series.state("owner/second")->rows, 3u);
}

// GAP-63, for 11b's progress: every change of a series' state is reported once, a stop and a chunk refused for a
// removed upstream included, and so is its removal.
TEST(GridRunnerTest, EveryStateChangeIsReported) {
  Fixture fx;
  fx.pushCloud(0, 2);
  fx.pushCloud(5 * kSec, 2);
  std::vector<std::string> reported;
  SeriesRunner::Hooks hooks;
  hooks.state_changed = [&reported](const std::string& key) { reported.push_back(key); };
  SeriesRunner series(fx.engine, fx.graph, *fx.runner, std::move(hooks));
  fx.fake.gated = true;
  series.install(fx.install(recipe("count", {"cloud"}, {numberSpec()})), /*demanded=*/true);
  ASSERT_TRUE(fx.waitStarted(1));
  EXPECT_FALSE(reported.empty()) << "installed, replaying";
  reported.clear();
  EXPECT_TRUE(series.stop("owner/count"));
  EXPECT_EQ(reported, (std::vector<std::string>{"owner/count"})) << "stopped";
  fx.fake.allow(1 << 20);
  test::pumpFor(20);

  fx.install(recipe("crop", {"cloud"}, {cloudSpec()}));
  series.install(fx.install(recipe("reader", {"owner/crop/cropped"}, {numberSpec()})), /*demanded=*/false);
  ASSERT_TRUE(fx.graph.remove("owner/crop").has_value());
  reported.clear();
  series.request("owner/reader");
  EXPECT_FALSE(series.state("owner/reader")->first_error.empty());
  EXPECT_EQ(reported, (std::vector<std::string>{"owner/reader"})) << "the refused chunk";
  reported.clear();
  series.remove("owner/reader");
  EXPECT_EQ(reported, (std::vector<std::string>{"owner/reader"})) << "gone";
}

// ---- Chains --------------------------------------------------------------------------------------------------

// DAG-02: an explicit evaluation of a reader evaluates its upstream itself, at each requested time, and leaves the
// installed upstream's ring entries, their uids, its node state and cursor, and both history budgets as they were.
TEST(GridRunnerTest, InstantEvaluationOfAReaderLeavesItsUpstreamUntouched) {
  Fixture fx;
  pushReferenceData(fx);
  fx.install(recipe("crop", {"cloud"}, {cloudSpec()}));
  const RecipeGraph::Node child = fx.install(recipe("child", {"owner/crop/cropped"}, {numberSpec()}));
  fx.request("owner/crop", 20 * kSec);
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  const auto uid = fx.store.latestAt(topic, 20 * kSec)->sequential_uid;
  const auto entries = fx.store.entryCount(topic);
  const auto state = fx.scheduler->state("owner/crop");
  const size_t ring_bytes = fx.history.ringBytes();
  const size_t cache_bytes = fx.history.cacheBytes();

  for (const Timestamp t : {5 * kSec, 10 * kSec}) {
    const Evaluated report = fx.evaluate(child, true, t, t);
    ASSERT_EQ(report.parsed.bundles.size(), 1u) << report.parsed.error;
    EXPECT_EQ(numberAt(report.parsed, 0), t == 5 * kSec ? 2.0 : 3.0);
  }
  EXPECT_EQ(fx.fake.runsOf("crop"), (std::vector<Timestamp>{20 * kSec, 5 * kSec, 10 * kSec}));
  EXPECT_EQ(fx.store.entryCount(topic), entries);
  EXPECT_EQ(fx.store.latestAt(topic, 20 * kSec)->sequential_uid, uid);
  EXPECT_EQ(fx.scheduler->state("owner/crop"), state);
  EXPECT_FALSE(fx.scheduler->state("owner/child").has_value()) << "the reader was never requested";
  EXPECT_EQ(fx.history.ringBytes(), ring_bytes);
  EXPECT_EQ(fx.history.cacheBytes(), cache_bytes);
}

// Installed upstreams whose ring already holds the results for an instant are read from there (no run), still without
// moving anything; a history-read upstream feeds its reader the identity ordinary staging gives it, so the next
// installed node in the chain is served from its history too.
TEST(GridRunnerTest, AnInstalledUpstreamIsServedFromItsHistory) {
  Fixture fx;
  pushReferenceData(fx);
  fx.install(recipe("crop", {"cloud"}, {cloudSpec()}));
  fx.install(recipe("mid", {"owner/crop/cropped"}, {cloudSpec()}));
  fx.request("owner/mid", 20 * kSec);  // the cursor's runs of both
  const RecipeGraph::Node child = fx.bind(recipe("child", {"owner/mid/cropped"}, {numberSpec()}));

  const Evaluated report = fx.evaluate(child, false, 20 * kSec, 20 * kSec);
  ASSERT_EQ(report.parsed.bundles.size(), 1u) << report.parsed.error;
  EXPECT_EQ(numberAt(report.parsed, 0), 4.0);
  EXPECT_EQ(fx.fake.runsOf("crop").size(), 1u) << "only the cursor's run";
  EXPECT_EQ(fx.fake.runsOf("mid").size(), 1u) << "only the cursor's run";
}

// A diamond's shared ancestor is evaluated once per instant, not once per path.
TEST(GridRunnerTest, SharedAncestorRunsOncePerInstant) {
  Fixture fx;
  pushReferenceData(fx);
  fx.install(recipe("a", {"cloud"}, {cloudSpec()}));
  fx.install(recipe("b", {"owner/a/cropped"}, {cloudSpec()}));
  fx.install(recipe("c", {"owner/a/cropped"}, {cloudSpec()}));
  const RecipeGraph::Node top = fx.bind(recipe("top", {"owner/b/cropped", "owner/c/cropped"}, {numberSpec()}));

  const Evaluated report = fx.evaluate(top, false, 5 * kSec, 20 * kSec);
  EXPECT_EQ(report.parsed.stopped, Stop::kComplete) << report.parsed.error;
  const std::vector<Timestamp> instants{5 * kSec, 10 * kSec, 20 * kSec};
  EXPECT_EQ(fx.fake.runsOf("a"), instants);
  EXPECT_EQ(fx.fake.runsOf("b"), instants);
  EXPECT_EQ(fx.fake.runsOf("c"), instants);
  EXPECT_EQ(fx.fake.runsOf("top"), instants);
}

// D25: an upstream output marked unavailable gates only the readers of that output; a reader of its sibling runs.
TEST(GridRunnerTest, UnavailableOutputGatesOnlyItsReaders) {
  Fixture fx;
  pushReferenceData(fx);
  fx.install(recipe("split", {"cloud"}, {cloudSpec("a"), cloudSpec("b")}, {{"unavailable", {"a"}}}));
  const RecipeGraph::Node sibling = fx.bind(recipe("sibling", {"owner/split/b"}, {numberSpec()}));
  const RecipeGraph::Node gated = fx.bind(recipe("gated", {"owner/split/a"}, {numberSpec()}));

  const Evaluated ok = fx.evaluate(sibling, false, 10 * kSec, 10 * kSec);
  EXPECT_EQ(ok.parsed.stopped, Stop::kComplete) << ok.parsed.error;
  EXPECT_EQ(numberAt(ok.parsed, 0), 3.0);
  const Evaluated missing = fx.evaluate(gated, false, 10 * kSec, 10 * kSec);
  EXPECT_EQ(missing.parsed.stopped, Stop::kError);
  EXPECT_EQ(missing.parsed.error, "on-demand: missing input 'owner/split/a': no points");
  EXPECT_TRUE(fx.fake.runsOf("gated").empty());
}

// CACHE-03: an upstream whose run did not read the query clock is reused across the instants where its own inputs
// do not change; one that read it runs at every instant.
TEST(GridRunnerTest, UnobservedClockReusesOneRunAcrossTheGrid) {
  Fixture fx;
  pushReferenceData(fx);
  for (const Timestamp t : {12 * kSec, 13 * kSec, 14 * kSec}) {
    fx.pushThreshold(t, 3.0);
  }
  fx.install(recipe("still", {"cloud"}, {cloudSpec()}));
  fx.install(recipe("ticking", {"cloud"}, {cloudSpec()}, {{"clock", true}}));
  const RecipeGraph::Node top =
      fx.bind(recipe("top", {"owner/still/cropped", "owner/ticking/cropped", "threshold"}, {numberSpec()}));

  const Evaluated report = fx.evaluate(top, false, 5 * kSec, 20 * kSec);
  EXPECT_EQ(report.parsed.stopped, Stop::kComplete) << report.parsed.error;
  EXPECT_EQ(report.parsed.candidates, 7u);
  EXPECT_EQ(fx.fake.runsOf("still"), (std::vector<Timestamp>{5 * kSec, 10 * kSec, 20 * kSec}));
  EXPECT_EQ(fx.fake.runsOf("ticking").size(), 7u);
  EXPECT_EQ(fx.fake.runsOf("top").size(), 7u);
}

// ---- What an evaluation leaves alone -----------------------------------------------------------------------------

// Nothing is published: an installed node's ring and both history budgets stay as they were, and a transient node
// registers no topic.
TEST(GridRunnerTest, NothingIsPublishedByAWindow) {
  Fixture fx;
  pushReferenceData(fx);
  const RecipeGraph::Node installed = fx.install(recipe("crop", {"cloud"}, {cloudSpec(), numberSpec()}));
  const RecipeGraph::Node transient = fx.bind(recipe("preview", {"cloud"}, {cloudSpec()}));

  EXPECT_EQ(fx.evaluate(installed, true, 0, 25 * kSec).parsed.evaluated, 4u);
  EXPECT_EQ(fx.evaluate(transient, false, 0, 25 * kSec).parsed.evaluated, 4u);
  EXPECT_EQ(fx.store.entryCount(fx.topic("owner/crop/cropped")), 0u);
  EXPECT_FALSE(fx.store.findTopic(fx.dataset, "owner/preview/cropped").has_value());
  EXPECT_EQ(fx.history.ringBytes(), 0u);
  EXPECT_EQ(fx.history.cacheBytes(), 0u);
}

// GAP-33 (D19, D22): evaluating an installed node never moves its cursor, its state or what its ring shows.
TEST(GridRunnerTest, EvaluationNeverMovesTheCursor) {
  Fixture fx;
  pushReferenceData(fx);
  const RecipeGraph::Node crop = fx.install(recipe("crop", {"cloud"}, {cloudSpec()}));
  fx.request("owner/crop", 7 * kSec);
  const auto state = fx.scheduler->state("owner/crop");
  ASSERT_TRUE(state.has_value());
  const auto ring = test::timestampsOf(fx.store, fx.topic("owner/crop/cropped"));

  EXPECT_EQ(fx.evaluate(crop, true, 0, 25 * kSec).parsed.evaluated, 4u);
  EXPECT_EQ(fx.scheduler->state("owner/crop"), state);
  EXPECT_EQ(fx.scheduler->state("owner/crop")->requested_t, 7 * kSec);
  EXPECT_EQ(test::timestampsOf(fx.store, fx.topic("owner/crop/cropped")), ring);
}

// ---- Budgets -------------------------------------------------------------------------------------------------

// EVAL-02: the time budget is checked between instants, on the injected clock: each run here takes 400 ms, so a
// 1000 ms budget ends after the third instant, and a zero one after the first.
TEST(GridRunnerTest, WindowStopsOnTimeBudget) {
  Fixture fx;
  pushReferenceData(fx);
  fx.fake.clock_step_ms = 400;
  const RecipeGraph::Node crop = fx.bind(recipe("crop", {"cloud", "threshold"}, {numberSpec()}));

  const Evaluated second = fx.evaluate(crop, false, 0, 25 * kSec, {.max_millis = 1000});
  EXPECT_EQ(second.parsed.evaluated, 3u);
  EXPECT_EQ(second.parsed.candidates, 5u);
  EXPECT_EQ(second.parsed.stopped, Stop::kBudgetTime);
  const Evaluated zero = fx.evaluate(crop, false, 0, 25 * kSec, {.max_millis = 0});
  EXPECT_EQ(zero.parsed.evaluated, 1u) << "the first instant always runs";
  EXPECT_EQ(zero.parsed.stopped, Stop::kBudgetTime);
}

TEST(GridRunnerTest, WindowStopsOnEvaluationBudget) {
  Fixture fx;
  pushReferenceData(fx);
  const RecipeGraph::Node crop = fx.bind(recipe("crop", {"cloud", "threshold"}, {numberSpec()}));

  const Evaluated two = fx.evaluate(crop, false, 0, 25 * kSec, {.max_evaluations = 2});
  EXPECT_EQ(two.parsed.evaluated, 2u);
  EXPECT_EQ(two.parsed.stopped, Stop::kBudgetEvaluations);
  const Evaluated none = fx.evaluate(crop, false, 0, 25 * kSec, {.max_evaluations = 0});
  EXPECT_EQ(none.parsed.evaluated, 0u);
  EXPECT_EQ(none.parsed.stopped, Stop::kBudgetEvaluations);
  const Evaluated no_bytes = fx.evaluate(crop, false, 0, 25 * kSec, {.max_bytes = 0});
  EXPECT_EQ(no_bytes.parsed.evaluated, 0u);
  EXPECT_EQ(no_bytes.parsed.stopped, Stop::kBudgetBytes);
  EXPECT_EQ(fx.fake.runsOf("crop").size(), 2u) << "only the first job ran";
}

// BUD-08: the whole report, its coverage included, fits `max_report_bytes`: a budget of exactly its size completes
// it, one byte less drops the last bundle (`budget_report`), and a budget below the smallest report is refused.
TEST(GridRunnerTest, CompleteReportIncludingCoverageFitsRequestedBytes) {
  Fixture fx;
  pushReferenceData(fx);
  const RecipeGraph::Node crop = fx.bind(recipe("crop", {"cloud", "threshold"}, {numberSpec(), stringSpec()}));
  const Evaluated full = fx.evaluate(crop, false, 0, 25 * kSec);
  ASSERT_EQ(full.parsed.stopped, Stop::kComplete);
  ASSERT_LT(full.json.size(), 1024u);

  const Evaluated exact = fx.evaluate(crop, false, 0, 25 * kSec, {.max_report_bytes = full.json.size()});
  EXPECT_EQ(exact.parsed.stopped, Stop::kComplete);
  EXPECT_EQ(exact.json, full.json);
  const Evaluated short_one = fx.evaluate(crop, false, 0, 25 * kSec, {.max_report_bytes = full.json.size() - 1});
  EXPECT_EQ(short_one.parsed.stopped, Stop::kBudgetReport);
  EXPECT_EQ(short_one.parsed.evaluated, 4u);
  const Evaluated small = fx.evaluate(crop, false, 0, 25 * kSec, {.max_report_bytes = 512});
  EXPECT_EQ(small.parsed.stopped, Stop::kBudgetReport);
  EXPECT_GT(small.parsed.evaluated, 0u);

  // A stop longer than "complete" (every bundle is checked against a complete report) gives up the last bundle.
  const Evaluated four = fx.evaluate(crop, false, 0, 25 * kSec, {.max_evaluations = 4});
  ASSERT_EQ(four.parsed.stopped, Stop::kBudgetEvaluations);
  const size_t four_complete = four.json.size() - (std::string_view("budget_evaluations").size() - 8);
  const Evaluated longer =
      fx.evaluate(crop, false, 0, 25 * kSec, {.max_evaluations = 4, .max_report_bytes = four_complete});
  EXPECT_EQ(longer.parsed.stopped, Stop::kBudgetReport);
  EXPECT_EQ(longer.parsed.evaluated, 3u);

  std::optional<GridRunner::Report> done;
  const auto refused = fx.submit(crop, false, 0, 25 * kSec, done, {.max_report_bytes = 40});
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("max_report_bytes"), std::string::npos) << refused.error();
}

// ABI-13's engine half: every budget is clamped to the host caps, so asking more is never refused: memory (the
// reservation), time, evaluations and the report's size.
TEST(GridRunnerTest, BudgetIsClampedToTheHostCaps) {
  Fixture fx(64 * InFlightBudget::kRunBytes);
  pushReferenceData(fx);
  const Budget greedy{
      .max_millis = 1'000'000,
      .max_bytes = 16 * InFlightBudget::kRunBytes,
      .max_evaluations = 1'000'000,
      .max_report_bytes = 1ull << 30};
  const RecipeGraph::Node crop = fx.bind(recipe("crop", {"cloud", "threshold"}, {numberSpec()}));

  fx.fake.gated = true;
  std::optional<GridRunner::Report> done;
  auto job = fx.submit(crop, false, 0, 25 * kSec, done, greedy);
  ASSERT_TRUE(job.has_value()) << job.error();
  ASSERT_TRUE(fx.waitStarted(1));
  EXPECT_EQ(fx.budget.reserved(), GridRunner::kHostCaps.max_bytes);
  fx.fake.allow(100);
  test::pumpUntil([&done] { return done.has_value(); });
  fx.fake.gated = false;

  fx.fake.clock_step_ms = 6000;
  const Evaluated timed = fx.evaluate(crop, false, 0, 25 * kSec, greedy);
  EXPECT_EQ(timed.parsed.evaluated, 1u);
  EXPECT_EQ(timed.parsed.stopped, Stop::kBudgetTime);
  fx.fake.clock_step_ms = 0;

  fx.dense_count = GridRunner::kHostCaps.max_evaluations + 50;
  const RecipeGraph::Node dense = fx.bind(recipe("dense", {"dense"}, {numberSpec()}));
  const Evaluated counted = fx.evaluate(dense, false, 0, 1 * kSec, greedy);
  EXPECT_EQ(counted.parsed.evaluated, GridRunner::kHostCaps.max_evaluations);
  EXPECT_EQ(counted.parsed.stopped, Stop::kBudgetEvaluations);

  fx.dense_count = 12;
  const RecipeGraph::Node big = fx.bind(recipe("big", {"dense"}, {stringSpec()}, {{"big", 1u << 20}}));
  const Evaluated sized = fx.evaluate(big, false, 0, 1 * kSec, greedy);
  EXPECT_EQ(sized.parsed.stopped, Stop::kBudgetReport);
  EXPECT_EQ(sized.parsed.evaluated, 7u) << "8 MiB hold seven 1 MiB bundles";
  EXPECT_EQ(fx.fake.runsOf("big").size(), 8u) << "the job stops at the first bundle that does not fit";
}

// ---- Reservations ----------------------------------------------------------------------------------------------

// BUD-05, BUD-10: a job holds one reservation, its memory budget, for its whole life: a chain, an upstream with two
// object outputs and a node that infers its outputs all run under it.
TEST(GridRunnerTest, OneReservationPerEvaluation) {
  Fixture fx;
  pushReferenceData(fx);
  fx.install(recipe("split", {"cloud"}, {cloudSpec("a"), cloudSpec("b")}));
  const RecipeGraph::Node reader = fx.bind(recipe("reader", {"owner/split/a", "owner/split/b"}, {}));
  fx.fake.gated = true;
  std::optional<GridRunner::Report> done;

  auto job = fx.submit(reader, false, 5 * kSec, 10 * kSec, done);
  ASSERT_TRUE(job.has_value()) << job.error();
  ASSERT_TRUE(fx.waitStarted(1));
  EXPECT_EQ(fx.budget.reserved(), InFlightBudget::kRunBytes);
  fx.fake.allow(1);
  ASSERT_TRUE(fx.waitStarted(2));
  test::pumpFor(20);
  EXPECT_EQ(fx.budget.reserved(), InFlightBudget::kRunBytes) << "the reader runs under the same reservation";
  fx.fake.allow(100);
  test::pumpUntil([&done] { return done.has_value(); });
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(Fixture::read(std::move(*done)).parsed.evaluated, 2u);
  EXPECT_EQ(fx.budget.reserved(), 0u);
}

// BUD-09, changed: a job waits in the budget's line for its room instead of being refused, and runs when it comes;
// one that asks more than the whole budget runs alone.
TEST(GridRunnerTest, AJobWaitsForRoomAndRunsWhenItComes) {
  Fixture fx(InFlightBudget::kRunBytes);
  pushReferenceData(fx);
  const RecipeGraph::Node crop = fx.bind(recipe("crop", {"cloud"}, {numberSpec()}));
  auto held = fx.budget.tryReserve(InFlightBudget::kRunBytes);
  ASSERT_TRUE(held.has_value());
  std::optional<GridRunner::Report> done;

  auto job = fx.submit(crop, false, 5 * kSec, 20 * kSec, done);
  ASSERT_TRUE(job.has_value()) << job.error();
  test::pumpFor(50);
  EXPECT_EQ(fx.fake.started(), 0) << "no room yet";
  held.reset();
  test::pumpUntil([&done] { return done.has_value(); });
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(Fixture::read(std::move(*done)).parsed.evaluated, 3u);

  Fixture small(InFlightBudget::kRunBytes / 4);
  pushReferenceData(small);
  const RecipeGraph::Node alone = small.bind(recipe("crop", {"cloud"}, {numberSpec()}));
  EXPECT_EQ(small.evaluate(alone, false, 5 * kSec, 20 * kSec).parsed.evaluated, 3u);
}

// ABI-17's engine half: a job cancelled, or dropped, while it waits for room never runs, never calls back, and leaves
// the line.
TEST(GridRunnerTest, ReleaseWhilePendingNeverRunsOrPublishes) {
  Fixture fx(InFlightBudget::kRunBytes);
  pushReferenceData(fx);
  const RecipeGraph::Node crop = fx.install(recipe("crop", {"cloud"}, {cloudSpec()}));
  auto held = fx.budget.tryReserve(InFlightBudget::kRunBytes);
  std::optional<GridRunner::Report> cancelled_done;
  std::optional<GridRunner::Report> dropped_done;

  auto cancelled = fx.submit(crop, true, 5 * kSec, 20 * kSec, cancelled_done);
  ASSERT_TRUE(cancelled.has_value());
  fx.runner->cancel(*cancelled);
  {
    auto dropped = fx.submit(crop, true, 5 * kSec, 20 * kSec, dropped_done);
    ASSERT_TRUE(dropped.has_value());
  }
  held.reset();
  test::pumpFor(100);
  fx.pool.waitIdle();
  EXPECT_EQ(fx.fake.started(), 0);
  EXPECT_FALSE(cancelled_done.has_value());
  EXPECT_FALSE(dropped_done.has_value());
  EXPECT_EQ(fx.budget.reserved(), 0u);
  EXPECT_EQ(fx.store.entryCount(fx.topic("owner/crop/cropped")), 0u);
}

// CON-01's engine half: a job cancelled while its run computes stops that run inside the script, never calls back,
// returns its room once the pool lets the run go, and leaves the pool usable.
TEST(GridRunnerTest, CancelAfterDispatchLeavesThePoolUsable) {
  Fixture fx;
  pushReferenceData(fx);
  const RecipeGraph::Node crop = fx.bind(recipe("crop", {"cloud"}, {numberSpec()}));
  fx.fake.gated = true;
  std::optional<GridRunner::Report> done;

  auto job = fx.submit(crop, false, 5 * kSec, 20 * kSec, done);
  ASSERT_TRUE(job.has_value());
  ASSERT_TRUE(fx.waitStarted(1));
  fx.runner->cancel(*job);
  EXPECT_EQ(fx.budget.reserved(), InFlightBudget::kRunBytes) << "the run still holds it";
  fx.pool.waitIdle();
  test::pumpFor(20);
  EXPECT_EQ(fx.fake.cancelledRuns(), 1);
  EXPECT_FALSE(done.has_value());
  EXPECT_EQ(fx.budget.reserved(), 0u);

  fx.fake.gated = false;
  EXPECT_EQ(fx.evaluate(crop, false, 5 * kSec, 20 * kSec).parsed.evaluated, 3u);
}

// A job keeps no object once it ends: a kept job, completed or cancelled mid-run, holds none of its upstream results
// (they would be outside the budget once its room is returned).
TEST(GridRunnerTest, AnEndedJobHoldsNoObjects) {
  Fixture fx;
  pushReferenceData(fx);
  fx.install(recipe("crop", {"cloud"}, {cloudSpec()}, {{"fresh", true}}));
  const RecipeGraph::Node child = fx.bind(recipe("child", {"owner/crop/cropped"}, {numberSpec()}));

  std::optional<GridRunner::Report> done;
  const auto completed = fx.submit(child, false, 10 * kSec, 10 * kSec, done);
  ASSERT_TRUE(completed.has_value());
  test::pumpUntil([&done] { return done.has_value(); });
  ASSERT_TRUE(done.has_value());
  EXPECT_TRUE(fx.fake.lastFresh().expired()) << "the completed job, still held, keeps no crop";

  fx.fake.gated = true;
  std::optional<GridRunner::Report> never;
  const auto cancelled = fx.submit(child, false, 10 * kSec, 10 * kSec, never);
  ASSERT_TRUE(cancelled.has_value());
  ASSERT_TRUE(fx.waitStarted(3));
  fx.fake.allow(1);
  ASSERT_TRUE(fx.waitStarted(4));  // the reader computes, its upstream's crop in hand
  EXPECT_FALSE(fx.fake.lastFresh().expired());
  fx.runner->cancel(*cancelled);
  fx.pool.waitIdle();
  EXPECT_TRUE(fx.fake.lastFresh().expired()) << "the cancelled job, still held, keeps no crop";
  EXPECT_EQ(fx.budget.reserved(), 0u);
}

// A publication the pool discards (the pool is destroyed before it drains) still returns the job's room and objects,
// even while a caller holds the job.
TEST(GridRunnerTest, ADiscardedPublicationReleasesTheJob) {
  Fixture fx;
  pushReferenceData(fx);
  fx.install(recipe("crop", {"cloud"}, {cloudSpec()}, {{"fresh", true}}));
  const RecipeGraph::Node child = fx.bind(recipe("child", {"owner/crop/cropped"}, {numberSpec()}));
  auto pool = std::make_unique<EnginePool>();
  auto runner = std::make_unique<GridRunner>(fx.store, fx.graph, fx.history, fx.stager, *pool, fx.budget, fx.hooks());
  fx.fake.gated = true;

  const auto job = runner->submit(child, false, 10 * kSec, 10 * kSec, {}, [](GridRunner::Report) {});
  ASSERT_TRUE(job.has_value());
  ASSERT_TRUE(fx.waitStarted(1));
  fx.fake.allow(1);
  for (const auto until = std::chrono::steady_clock::now() + 5s;
       pool->pendingPublishes() == 0 && std::chrono::steady_clock::now() < until;) {
    std::this_thread::sleep_for(1ms);  // no event pump: the publication stays queued
  }
  ASSERT_EQ(pool->pendingPublishes(), 1u);
  ASSERT_EQ(fx.budget.reserved(), InFlightBudget::kRunBytes);
  runner.reset();
  pool.reset();
  EXPECT_EQ(fx.budget.reserved(), 0u);
  EXPECT_TRUE(fx.fake.lastFresh().expired());
}

// `max_bytes` is the job's whole memory: one native ledger serves every run of the job, so the crops its upstreams keep
// for their readers count against the later runs. Here three crops of a 5.3 MB cloud each fit the 12 MiB native share
// alone but not together, and the job stops with `budget_bytes`; an upstream's previous result is dropped before it
// runs again, so a two-instant window of one crop fits a share that two crops would not.
TEST(GridRunnerTest, RetainedUpstreamObjectsCountAgainstTheJobsMemory) {
  Fixture fx(4 * InFlightBudget::kRunBytes, /*real_run=*/true);
  constexpr size_t kPoints = 330'000;  // 16 bytes each
  fx.pushCloud(10 * kSec, kPoints);
  fx.pushCloud(20 * kSec, kPoints);
  const auto crop = [](const std::string& input) {
    return "local inputs = ...\nreturn inputs['" + input + "']:crop_box{min={-1e9, -1e9, -1e9}, max={1e9, 1e9, 1e9}}\n";
  };
  auto first = recipe("first", {"cloud"}, {cloudSpec()});
  first.script = crop("cloud");
  fx.install(first);
  auto second = recipe("second", {"owner/first/cropped"}, {cloudSpec()});
  second.script = crop("owner/first/cropped");
  fx.install(second);
  const auto counting = [&fx](const std::string& id, const std::string& input) {
    auto made = recipe(id, {input}, {numberSpec()});
    made.script = "local inputs = ...\nreturn inputs['" + input +
                  "']:crop_box{min={-1e9, -1e9, -1e9}, max={1e9, 1e9, 1e9}}:count()\n";
    return fx.bind(made);
  };
  const Budget twenty_four_mib{.max_bytes = 24u << 20};  // 12 MiB VM, 12 MiB native

  const Evaluated alone = fx.evaluate(counting("alone", "cloud"), false, 10 * kSec, 10 * kSec, twenty_four_mib);
  EXPECT_EQ(alone.parsed.stopped, Stop::kComplete) << alone.parsed.error;
  const Evaluated chained =
      fx.evaluate(counting("third", "owner/second/cropped"), false, 10 * kSec, 10 * kSec, twenty_four_mib);
  EXPECT_EQ(chained.parsed.stopped, Stop::kBudgetBytes) << chained.parsed.error;
  EXPECT_EQ(chained.parsed.evaluated, 0u);

  auto reader = recipe("reader", {"owner/first/cropped"}, {numberSpec()});
  reader.script = "local inputs = ...\nreturn inputs['owner/first/cropped']:count()\n";
  const Evaluated window = fx.evaluate(fx.bind(reader), false, 10 * kSec, 20 * kSec, {.max_bytes = 16u << 20});
  EXPECT_EQ(window.parsed.stopped, Stop::kComplete) << window.parsed.error;
  EXPECT_EQ(window.parsed.evaluated, 2u);
}

// The Python twin, over builders: a Python builder's native charge lives in its output's keepalive, which a job's
// upstream result keeps for its readers, so the chain stops on the job's memory as the Luau one does. Each step draws
// 330k annotation points (5.3 MB): one fits the 12 MiB native share, three retained at once do not.
TEST(GridRunnerTest, RetainedPythonBuilderOutputsCountAgainstTheJobsMemory) {
  Fixture fx(4 * InFlightBudget::kRunBytes, /*real_run=*/true);
  fx.pushCloud(10 * kSec);
  const OutputSpec annotations{"ann", OutputType::kObject, sdk::BuiltinObjectType::kImageAnnotations};
  const std::string draw = "pj.annotations.new(image_topic='/cam').points(points=[(0, 0)] * 330000, type='points')";
  fx.install(inPython(recipe("first", {"cloud"}, {annotations}), "    return " + draw + ".finish()\n"));
  fx.install(inPython(recipe("second", {"owner/first/ann"}, {annotations}), "    return " + draw + ".finish()\n"));
  const auto counting = [&](const std::string& id, const std::string& input) {
    return fx.bind(inPython(recipe(id, {input}, {numberSpec()}), "    return " + draw + ".finish().count()\n"));
  };
  const Budget twenty_four_mib{.max_bytes = 24u << 20};  // 12 MiB VM, 12 MiB native

  const Evaluated alone = fx.evaluate(counting("alone", "cloud"), false, 10 * kSec, 10 * kSec, twenty_four_mib);
  EXPECT_EQ(alone.parsed.stopped, Stop::kComplete) << alone.parsed.error;
  const Evaluated chained =
      fx.evaluate(counting("third", "owner/second/ann"), false, 10 * kSec, 10 * kSec, twenty_four_mib);
  EXPECT_EQ(chained.parsed.stopped, Stop::kBudgetBytes) << chained.parsed.error;
  EXPECT_EQ(chained.parsed.evaluated, 0u);
}

// A job's upstream result reaches its reader as an override that keeps what its object needs: a Python reader that
// parks it past the run (on a class its module copy shares) keeps the upstream's parser library loaded, and the
// payload's deleter (plugin code) runs before it unloads.
TEST(GridRunnerTest, AnUpstreamObjectAPythonReaderParksKeepsItsParserLibraryLoaded) {
  Fixture fx(4 * InFlightBudget::kRunBytes, /*real_run=*/true);
  test::ParseProbe probe;
  auto parser = std::make_shared<test::RawPointsParser>(probe);
  ASSERT_TRUE(parser->bindSchema("raw_points", {}).has_value());
  auto library = std::make_shared<int>(1);
  const std::weak_ptr<int> loaded = library;
  const ObjectTopicId lidar = fx.store.registerTopic({fx.dataset, "lidar", ""}).value();
  fx.parsers = [lidar, parser, library = std::shared_ptr<void>(library), &probe](ObjectTopicId topic) {
    return topic == lidar ? SessionManager::ParserBinding{parser.get(), probe.topic_lock, library}
                          : SessionManager::ParserBinding{};
  };
  auto deletes_after_unload = std::make_shared<std::atomic<int>>(0);
  ASSERT_TRUE(
      fx.store.pushLazy(lidar, 10 * kSec, test::lazyPointsNotingUnload(loaded, deletes_after_unload)).has_value());
  const std::string imports = "import statistics\n";
  fx.install(inPython(recipe("first", {"lidar"}, {cloudSpec()}), "    return inputs['lidar']\n", imports));
  const Evaluated parked = fx.evaluate(
      fx.bind(inPython(
          recipe("reader", {"owner/first/cropped"}, {numberSpec()}),
          "    statistics.NormalDist.kept = inputs['owner/first/cropped']\n    return 1\n", imports)),
      false, 10 * kSec, 10 * kSec);
  ASSERT_EQ(parked.parsed.stopped, Stop::kComplete) << parked.parsed.error;

  ASSERT_TRUE(fx.graph.remove("owner/first").has_value());
  test::prune(fx.store, lidar);
  fx.parsers = {};
  library.reset();
  EXPECT_FALSE(loaded.expired()) << "the parked proxy keeps the upstream's library loaded";

  fx.pushCloud(10 * kSec);
  const Evaluated released = fx.evaluate(
      fx.bind(inPython(
          recipe("release", {"cloud"}, {numberSpec()}), "    del statistics.NormalDist.kept\n    return 1\n", imports)),
      false, 10 * kSec, 10 * kSec);
  ASSERT_EQ(released.parsed.stopped, Stop::kComplete) << released.parsed.error;
  EXPECT_EQ(deletes_after_unload->load(), 0);
  EXPECT_TRUE(loaded.expired()) << "the proxy released the library with the object";
}

// ---- Errors --------------------------------------------------------------------------------------------------

// ABI-14: a failing run ends the window with the one report shape: the bundles before it, `stopped: "error"` and
// `coverage.error`, the failing instant not counted. A clean report has no error key.
TEST(GridRunnerTest, AnErrorStopsWithTheOneReportShape) {
  Fixture fx;
  pushReferenceData(fx);
  const RecipeGraph::Node failing =
      fx.bind(recipe("crop", {"cloud", "threshold"}, {numberSpec()}, {{"fail_at", 15 * kSec}}));

  const Evaluated report = fx.evaluate(failing, false, 0, 25 * kSec);
  EXPECT_EQ(report.parsed.stopped, Stop::kError);
  EXPECT_EQ(report.parsed.error, "boom");
  EXPECT_EQ(report.parsed.candidates, 5u);
  EXPECT_EQ(report.parsed.evaluated, 3u);
  EXPECT_EQ(requestedTimes(report.parsed), (std::vector<int64_t>{0, 5 * kSec, 10 * kSec}));

  const Evaluated clean = fx.evaluate(failing, false, 0, 10 * kSec);
  EXPECT_EQ(clean.parsed.stopped, Stop::kComplete);
  EXPECT_EQ(clean.json.find("\"error\""), std::string::npos);
}

// ABI-15: a failure's reason is kept up to 1024 bytes, and shortened further only to keep the report within its
// budget.
TEST(GridRunnerTest, FailedEvaluationReasonIsKeptUpTo1024Chars) {
  Fixture fx;
  pushReferenceData(fx);
  const auto failing = [&fx](size_t length) {
    return fx.bind(recipe("crop", {"cloud"}, {numberSpec()}, {{"fail", std::string(length, 'e')}}));
  };

  EXPECT_EQ(fx.evaluate(failing(600), false, 5 * kSec, 5 * kSec).parsed.error, std::string(600, 'e'));
  EXPECT_EQ(fx.evaluate(failing(2000), false, 5 * kSec, 5 * kSec).parsed.error, std::string(1024, 'e'));
  const Evaluated tight = fx.evaluate(failing(2000), false, 5 * kSec, 5 * kSec, {.max_report_bytes = 300});
  EXPECT_EQ(tight.parsed.stopped, Stop::kError);
  EXPECT_LT(tight.parsed.error.size(), 300u);
  EXPECT_GT(tight.parsed.error.size(), 100u);
  EXPECT_EQ(tight.json.size(), 300u) << "shortened only as far as needed";
  // Escaping doubles a quote: the prefix is fitted by its escaped size, not its length.
  const RecipeGraph::Node quotes =
      fx.bind(recipe("quotes", {"cloud"}, {numberSpec()}, {{"fail", std::string(2000, '"')}}));
  const Evaluated quoted = fx.evaluate(quotes, false, 5 * kSec, 5 * kSec, {.max_report_bytes = 300});
  EXPECT_EQ(quoted.parsed.stopped, Stop::kError);
  EXPECT_GT(quoted.parsed.error.size(), 50u);
  EXPECT_EQ(quoted.parsed.error, std::string(quoted.parsed.error.size(), '"'));
  EXPECT_GE(quoted.json.size(), 299u) << "a quote's two bytes may be all that is left";

  std::string euros;
  for (int index = 0; index < 700; ++index) {
    euros += "\u20ac";  // 3 bytes in UTF-8
  }
  const RecipeGraph::Node wide = fx.bind(recipe("wide", {"cloud"}, {numberSpec()}, {{"fail", euros}}));
  EXPECT_EQ(fx.evaluate(wide, false, 5 * kSec, 5 * kSec).parsed.error, euros.substr(0, 341 * 3)) << "whole characters";

  // An error after some bundles: when even its empty reason does not fit, the last bundle gives up its room.
  const RecipeGraph::Node clean = fx.bind(recipe("crop", {"cloud", "threshold"}, {numberSpec()}));
  const size_t two_bundles = fx.evaluate(clean, false, 0, 5 * kSec).json.size();
  const RecipeGraph::Node late =
      fx.bind(recipe("crop", {"cloud", "threshold"}, {numberSpec()}, {{"fail_at", 10 * kSec}}));
  const Evaluated dropped = fx.evaluate(late, false, 0, 25 * kSec, {.max_report_bytes = two_bundles});
  EXPECT_EQ(dropped.parsed.stopped, Stop::kError);
  EXPECT_EQ(dropped.parsed.error, "boom");
  EXPECT_EQ(dropped.parsed.evaluated, 1u);
}

// `max_bytes` is the run's whole memory: the VM's cap and the native budget share it in their default ratio, and the
// job reserves all of it.
TEST(GridRunnerTest, MemoryBudgetIsSplitBetweenTheVmAndTheNativeObjects) {
  Fixture fx;
  pushReferenceData(fx);
  const RecipeGraph::Node crop = fx.bind(recipe("crop", {"cloud"}, {numberSpec()}));

  EXPECT_EQ(fx.evaluate(crop, false, 5 * kSec, 5 * kSec, {.max_bytes = 10u << 20}).parsed.evaluated, 1u);
  EXPECT_EQ(fx.fake.vm_bytes.load(), size_t{5} << 20);
  EXPECT_EQ(fx.fake.native_bytes.load(), size_t{5} << 20);
  EXPECT_EQ(fx.evaluate(crop, false, 5 * kSec, 5 * kSec).parsed.evaluated, 1u);
  EXPECT_EQ(fx.fake.vm_bytes.load(), scripting::BudgetLimits{}.mem_bytes) << "the default is the default run's";
  EXPECT_EQ(fx.fake.native_bytes.load(), scripting::kDefaultNativeBudgetBytes);
}

// ABI-18's engine half: a new revision of the installed node, or a new epoch of its dataset, while the job is pending
// fails it with "evaluation inputs were replaced".
TEST(GridRunnerTest, ReplacingTheInstalledNodeFailsThePendingJob) {
  Fixture fx;
  pushReferenceData(fx);
  auto crop_recipe = recipe("crop", {"cloud"}, {numberSpec()});
  const RecipeGraph::Node crop = fx.install(crop_recipe);
  fx.fake.gated = true;

  std::optional<GridRunner::Report> done;
  auto job = fx.submit(crop, true, 5 * kSec, 20 * kSec, done);
  ASSERT_TRUE(job.has_value());
  ASSERT_TRUE(fx.waitStarted(1));
  crop_recipe.params_json = R"({"id": "crop", "edited": true})";
  fx.install(crop_recipe);
  fx.fake.allow(1);
  test::pumpUntil([&done] { return done.has_value(); });
  ASSERT_TRUE(done.has_value());
  const Evaluated replaced = Fixture::read(std::move(*done));
  EXPECT_EQ(replaced.parsed.stopped, Stop::kError);
  EXPECT_EQ(replaced.parsed.error, "on-demand: evaluation inputs were replaced");
  EXPECT_EQ(replaced.parsed.evaluated, 0u);

  std::optional<GridRunner::Report> epoch_done;
  auto epoch_job = fx.submit(*fx.graph.find("owner/crop"), true, 5 * kSec, 20 * kSec, epoch_done);
  ASSERT_TRUE(epoch_job.has_value());
  ASSERT_TRUE(fx.waitStarted(2));
  fx.epoch = 1;
  fx.fake.allow(100);
  test::pumpUntil([&epoch_done] { return epoch_done.has_value(); });
  ASSERT_TRUE(epoch_done.has_value());
  EXPECT_EQ(Fixture::read(std::move(*epoch_done)).parsed.error, "on-demand: evaluation inputs were replaced");
}

// ---- The report's content ------------------------------------------------------------------------------------

// EVAL-03: a valid empty object is "empty", with its summary, distinct from an unavailable output (D25); every
// object output travels as a summary, never as bytes.
TEST(GridRunnerTest, EmptyStatusForAnEmptyCrop) {
  Fixture fx;
  pushReferenceData(fx);
  const RecipeGraph::Node empty = fx.bind(recipe(
      "crop", {"cloud"}, {cloudSpec(), cloudSpec("gone"), numberSpec()}, {{"empty", true}, {"unavailable", {"gone"}}}));

  const Evaluated report = fx.evaluate(empty, false, 10 * kSec, 10 * kSec);
  ASSERT_EQ(report.parsed.bundles.size(), 1u) << report.parsed.error;
  const auto& outputs = report.parsed.bundles[0].outputs;
  EXPECT_EQ(outputs.at("cropped").status, OutputStatus::kEmpty);
  EXPECT_EQ(outputs.at("cropped").summary.at("points").get<int>(), 0);
  EXPECT_EQ(outputs.at("cropped").summary.at("type").get<std::string>(), "kPointCloud");
  EXPECT_EQ(outputs.at("gone").status, OutputStatus::kUnavailable);
  EXPECT_EQ(outputs.at("gone").reason, "no points");
  EXPECT_EQ(outputs.at("count").status, OutputStatus::kOk);

  const RecipeGraph::Node full = fx.bind(recipe("full", {"cloud"}, {cloudSpec()}));
  const Evaluated shown = fx.evaluate(full, false, 10 * kSec, 10 * kSec);
  ASSERT_EQ(shown.parsed.bundles.size(), 1u) << shown.parsed.error;
  EXPECT_EQ(shown.parsed.bundles[0].outputs.at("cropped").status, OutputStatus::kOk);
  EXPECT_EQ(shown.parsed.bundles[0].outputs.at("cropped").summary.at("points").get<int>(), 3);
}

// JSON has no NaN: a number output that is not finite is that output's error, and the report stays valid.
TEST(GridRunnerTest, ANonFiniteNumberIsAnOutputError) {
  Fixture fx;
  pushReferenceData(fx);
  const RecipeGraph::Node node = fx.bind(recipe("crop", {"cloud"}, {numberSpec()}, {{"nan", true}}));

  const Evaluated report = fx.evaluate(node, false, 10 * kSec, 10 * kSec);
  ASSERT_EQ(report.parsed.bundles.size(), 1u);
  const auto& count = report.parsed.bundles[0].outputs.at("count");
  EXPECT_EQ(count.status, OutputStatus::kError);
  EXPECT_EQ(count.reason, "not a finite number");
}

// ABI-09's engine half: a node that declares no output lists what its runs inferred in the report's root
// "outputs", in first-seen order; a node that declares its outputs has none. The real Luau engine runs.
TEST(GridRunnerTest, InferredOutputsAreListedWhenNoneWereDeclared) {
  Fixture fx(4 * InFlightBudget::kRunBytes, /*real_run=*/true);
  pushReferenceData(fx);
  auto inferred = recipe("crop", {"cloud"}, {});
  inferred.script =
      "local inputs = ...\n"
      "return { count = inputs.cloud:count(), label = 'ok', cloud = inputs.cloud, z = pj.unavailable('none') }\n";
  const RecipeGraph::Node node = fx.bind(inferred);

  const Evaluated report = fx.evaluate(node, false, 10 * kSec, 10 * kSec);
  ASSERT_EQ(report.parsed.bundles.size(), 1u) << report.parsed.error;
  std::vector<std::pair<std::string, std::string>> listed;
  for (const auto& output : report.parsed.inferred_outputs) {
    listed.emplace_back(output.name, output.type);
  }
  EXPECT_EQ(
      listed, (std::vector<std::pair<std::string, std::string>>{
                  {"cloud", "kPointCloud"}, {"count", "number"}, {"label", "string"}, {"z", "unknown"}}));
  EXPECT_EQ(numberAt(report.parsed, 0), 3.0);
  EXPECT_EQ(report.parsed.bundles[0].outputs.at("cloud").summary.at("points").get<int>(), 3);

  auto declared = inferred;
  declared.user_id = "declared";
  declared.outputs = {numberSpec()};
  declared.script = "local inputs = ...\nreturn inputs.cloud:count()\n";
  const Evaluated plain = fx.evaluate(fx.bind(declared), false, 10 * kSec, 10 * kSec);
  EXPECT_EQ(plain.json.find("\"outputs\":["), std::string::npos) << "no root outputs";
  EXPECT_EQ(numberAt(plain.parsed, 0), 3.0);
}

// A Python node's window evaluation runs the Python runner and reports exactly what its Luau twin reports: the same
// bundles, values, inferred outputs, summaries and coverage.
TEST(GridRunnerTest, PythonWindowEvaluationReportsLikeLuau) {
  Fixture fx(4 * InFlightBudget::kRunBytes, /*real_run=*/true);
  pushReferenceData(fx);
  auto luau = recipe("probe", {"cloud"}, {});
  luau.script =
      "local inputs = ...\n"
      "return { count = inputs.cloud:count(), label = 'ok', cloud = inputs.cloud, z = pj.unavailable('none') }\n";
  const auto python = inPython(
      luau,
      "    cloud = inputs['cloud']\n"
      "    return {'count': cloud.count(), 'label': 'ok', 'cloud': cloud, 'z': pj.unavailable('none')}\n");

  const Evaluated expected = fx.evaluate(fx.bind(luau), false, 0, 25 * kSec);
  ASSERT_EQ(expected.parsed.stopped, Stop::kComplete) << expected.parsed.error;
  ASSERT_GE(expected.parsed.bundles.size(), 2u);
  const Evaluated report = fx.evaluate(fx.bind(python), false, 0, 25 * kSec);
  EXPECT_EQ(report.json, expected.json);
}

// Raw nanosecond counts are JSON integers, exact past 2^53, where a double would round them.
TEST(GridRunnerTest, TimestampPastTwoToThe53RoundTripsExactly) {
  Fixture fx;
  constexpr Timestamp kLarge = (Timestamp{1} << 53) + 1;
  fx.pushCloud(kLarge, 3);
  const RecipeGraph::Node node = fx.bind(recipe("crop", {"cloud"}, {numberSpec()}));

  const Evaluated report = fx.evaluate(node, false, kLarge, kLarge);
  ASSERT_EQ(report.parsed.bundles.size(), 1u) << report.parsed.error;
  EXPECT_EQ(report.parsed.bundles[0].requested_ns, kLarge);
  EXPECT_EQ(report.parsed.bundles[0].inputs.at("cloud"), kLarge);
}

// ---- Measurement -----------------------------------------------------------------------------------------------

/// The CPU time this thread has spent, in ns.
int64_t threadCpuNanos() {
  return static_cast<int64_t>(pj_bench::threadCpuMs() * 1e6);
}

// PJ_BENCH=1: building a window's grid over 100k scalar samples and 10k object entries (ns per instant), measured as
// the submission (the grid, the gaps, the envelope check) of a job then cancelled.
TEST(GridRunnerBench, GridOverManyInputChanges) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  Fixture fx;
  for (int index = 0; index < 10'000; ++index) {
    fx.pushCloud(Timestamp{index} * 100'000, 1);
  }
  fx.dense_count = 100'000;
  const RecipeGraph::Node node = fx.bind(recipe("crop", {"cloud", "dense"}, {numberSpec()}));
  std::optional<GridRunner::Report> done;
  const double micros = test::medianMicros(11, [&] {
    auto job = fx.submit(node, false, 0, 1 * kSec, done);
    fx.runner->cancel(*job);
  });
  std::printf(
      "[bench] grid of %zu instants: %.0f us, %.1f ns per instant\n", size_t{110'000}, micros,
      micros * 1000.0 / 110'000);
}

// PJ_BENCH=1: the GUI thread's cost per instant of a 400-instant window over 24k-point clouds (fake pass-through
// runs: the worker's cost is not counted), and the cost per bundle at 1, 200 and 2000 instants, which stays flat:
// the report grows by appending, never by re-serializing. Thread CPU time, so the pump's sleeps are not counted.
TEST(GridRunnerBench, GuiCostPerInstant) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  {
    Fixture fx;
    for (int index = 0; index < 400; ++index) {
      fx.pushCloud(Timestamp{index} * 100'000'000, 24'000);
    }
    const RecipeGraph::Node node = fx.bind(recipe("crop", {"cloud"}, {cloudSpec(), numberSpec()}));
    const int64_t begin = threadCpuNanos();
    const Evaluated report = fx.evaluate(node, false, 0, 40 * kSec, {.max_evaluations = 2000});
    const int64_t spent = threadCpuNanos() - begin;
    std::printf(
        "[bench] 400-instant window over 24k-point clouds: %zu bundles, GUI %.1f us per instant\n",
        report.parsed.bundles.size(), static_cast<double>(spent) / 1000.0 / 400.0);
  }
  for (const size_t instants : {size_t{1}, size_t{200}, size_t{2000}}) {
    Fixture fx;
    fx.dense_count = instants;
    const RecipeGraph::Node node = fx.bind(recipe("crop", {"dense"}, {numberSpec(), stringSpec()}));
    const int64_t begin = threadCpuNanos();
    const Evaluated report = fx.evaluate(node, false, 0, 1 * kSec, {.max_evaluations = 2000});
    const int64_t spent = threadCpuNanos() - begin;
    const int64_t json_begin = threadCpuNanos();
    const std::string json = GridRunner::toJson(report.report);
    const int64_t json_spent = threadCpuNanos() - json_begin;
    std::printf(
        "[bench] %zu instants: GUI %.1f us per bundle; toJson %.0f ns per bundle (%zu bytes)\n", instants,
        static_cast<double>(spent) / 1000.0 / static_cast<double>(instants),
        static_cast<double>(json_spent) / static_cast<double>(instants), json.size());
  }
}

// PJ_BENCH=1: the GUI cost of submitting one series chunk in the middle of 100k object entries (the clock slice found
// by binary search, 64 instants copied), measured as the submission of a job then cancelled, and of finding nothing
// after the last entry: medians of 101.
TEST(GridRunnerBench, RowsChunkAmong100kEntries) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  Fixture fx;
  for (int index = 0; index < 100'000; ++index) {
    fx.pushCloud(Timestamp{index} * 100'000, 1);
  }
  const RecipeGraph::Node node = fx.install(recipe("count", {"cloud"}, {numberSpec()}));
  const SequentialUID seen = SequentialUID::getNext();  // after every entry: the late-arrival check finds none
  const double micros = test::medianMicros(101, [&] {
    auto job = fx.runner->submitRows(
        node, Timestamp{50'000} * 100'000, std::nullopt, seen, SeriesRunner::kInstantsPerChunk, {});
    fx.runner->cancel(*job);
  });
  const double nothing_new = test::medianMicros(101, [&] {
    auto job = fx.runner->submitRows(
        node, Timestamp{99'999} * 100'000, std::nullopt, seen, SeriesRunner::kInstantsPerChunk, {});
    EXPECT_EQ(*job, nullptr);
  });
  std::printf(
      "[bench] a 64-instant chunk among 100k entries: %.2f us to submit; nothing after the watermark (a live tick "
      "with no new entry): %.2f us\n",
      micros, nothing_new);
}

// PJ_BENCH=1: a live tick that brings nothing new to 5 complete series among 10k object topics (`appendLive`, each
// series' live edge looked up through the stager, as `OnDemandService` does for a node without upstreams, and its
// clock found empty after its watermark): the GUI cost of the tick, median of 101.
TEST(GridRunnerBench, IdleSeriesTickAmong10kObjectTopics) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  Fixture fx;
  constexpr int kTopics = 10'000;
  constexpr int kSeries = 5;
  std::vector<ObjectTopicId> topics;
  for (int index = 0; index < kTopics; ++index) {
    topics.push_back(fx.store.registerTopic({fx.dataset, "topic" + std::to_string(index), ""}).value());
  }
  SeriesRunner::Hooks hooks;
  hooks.live_edge = [&fx](const RecipeGraph::Node& node) { return fx.stager.laggingInputStamp(node); };
  SeriesRunner series(fx.engine, fx.graph, *fx.runner, std::move(hooks));
  for (int index = 0; index < kSeries; ++index) {
    const int input = kTopics - 1 - index;
    for (const Timestamp t : {0 * kSec, 5 * kSec, 10 * kSec}) {
      ASSERT_TRUE(fx.store.pushOwned(topics[input], t, canonicalCloud(1)).has_value());
    }
    const std::string id = "s" + std::to_string(index);
    series.install(fx.install(recipe(id, {"topic" + std::to_string(input)}, {numberSpec()})), /*demanded=*/true);
  }
  const auto complete = [&] {
    for (int index = 0; index < kSeries; ++index) {
      const auto state = series.state("owner/s" + std::to_string(index));
      if (!state->complete || state->replaying) {
        return false;
      }
    }
    return true;
  };
  test::pumpUntil(complete, 10000);
  ASSERT_TRUE(complete());
  const double micros = test::medianMicros(101, [&] { series.appendLive(fx.dataset); });
  ASSERT_TRUE(complete()) << "nothing new: nothing submitted";
  std::printf("[bench] an idle live tick of %d series among %d object topics: %.2f us\n", kSeries, kTopics, micros);
}

// PJ_BENCH=1: an explicit two-hop evaluation (two Luau pass-through crops of a 260k-point cloud), from submission to
// the report: median of 21.
TEST(GridRunnerBench, TwoHopExplicitEvaluationLatency) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  Fixture fx(4 * InFlightBudget::kRunBytes, /*real_run=*/true);
  fx.pushCloud(10 * kSec, 260'000);
  auto first = recipe("crop", {"cloud"}, {cloudSpec()});
  first.script = "local inputs = ...\nreturn inputs.cloud:crop_box{min={-1e9, -1e9, -1e9}, max={1e9, 1e9, 1e9}}\n";
  fx.install(first);
  auto second = recipe("child", {"owner/crop/cropped"}, {numberSpec()});
  second.script =
      "local inputs = ...\nlocal c = inputs['owner/crop/cropped']\n"
      "return c:crop_box{min={-1e9, -1e9, -1e9}, max={1e9, 1e9, 1e9}}:count()\n";
  const RecipeGraph::Node child = fx.bind(second);
  const double micros = test::medianMicros(21, [&] {
    const Evaluated report = fx.evaluate(child, false, 10 * kSec, 10 * kSec);
    EXPECT_EQ(report.parsed.stopped, Stop::kComplete) << report.parsed.error;
  });
  std::printf("[bench] two-hop explicit evaluation, 260k points: %.2f ms\n", micros / 1000.0);
}

}  // namespace
}  // namespace PJ
