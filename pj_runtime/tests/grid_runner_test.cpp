// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// GridRunner: explicit evaluations over a window or at an instant (REQUIREMENTS D19). A real ObjectStore, DataEngine,
// EnginePool, InFlightBudget, RecipeGraph, ResultHistory and InputStager; a NodeScheduler beside the runner for the
// installed nodes' own cursor requests; an injected fake script run (one end-to-end case runs the real Luau engine).

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
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
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/GridRunner.h"
#include "pj_runtime/NodeScheduler.h"

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

std::vector<uint8_t> canonicalCloud(size_t points) {
  return serializeBuiltinObject(sdk::BuiltinObject(test::makeCloud(points))).value();
}

/// A script run that records what ran. Its recipe's params say what it does: `fail` (the message) or `fail_at` (fail
/// with "boom" at that query time), `unavailable` (output names, "no points"), `empty` (empty clouds), `fresh` (a cloud
/// output gets bytes of its own, `lastFresh`), `clock` (it read the query clock). Otherwise a cloud output
/// is its first cloud input (a pass-through), a number the first cloud's point count (or the first number input), a
/// string "ok"; with no declared outputs it returns `count` and `label`. Set the knobs while nothing runs.
class FakeRun {
 public:
  std::atomic<bool> gated{false};  ///< each run waits for `allow` (or its cancellation)

  NodeScheduler::RunFunction function() {
    return [this](
               const scripting::CompiledScript&, Span<const NamedValue> inputs, std::string_view params,
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
      while (gated && permits_ == 0 && !cancelled() && std::chrono::steady_clock::now() < until) {
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
    const double count = cloud != nullptr ? static_cast<double>(cloud->width) : number.value_or(0.0);
    const std::vector<std::string> unavailable = params.value("unavailable", std::vector<std::string>{});
    ScriptBundle bundle;
    bundle.observed_query_clock = params.value("clock", false);
    if (outputs.empty()) {
      bundle.outputs.push_back({"count", count});
      bundle.outputs.push_back({"label", std::string("ok")});
      return bundle;
    }
    for (const OutputSpec& output : outputs) {
      if (std::find(unavailable.begin(), unavailable.end(), output.name) != unavailable.end()) {
        bundle.outputs.push_back({output.name, Unavailable{"no points"}});
      } else if (output.type == OutputType::kNumber) {
        bundle.outputs.push_back({output.name, count});
      } else if (output.type == OutputType::kString) {
        bundle.outputs.push_back({output.name, std::string("ok")});
      } else {
        const bool empty = params.value("empty", false) || cloud == nullptr;
        sdk::PointCloud made = empty ? test::makeCloud(0) : *cloud;
        if (params.value("fresh", false) && !empty) {
          made = test::makeCloud(cloud->width);  // bytes of its own, unlike a pass-through
          const std::lock_guard<std::mutex> lock(mutex_);
          last_fresh_ = made.anchor;
        }
        bundle.outputs.push_back({output.name, sdk::BuiltinObject(std::move(made))});
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

/// What one evaluation produced: the runner's report (`parsed` is the same report).
struct Evaluated {
  GridRunner::Report report;
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
      std::optional<GridRunner::Report>& done) {
    return runner->submit(
        node, installed, start, end, [&done](GridRunner::Report report) { done = std::move(report); });
  }
  /// Evaluates `node` over [start, end] to its report.
  Evaluated evaluate(const RecipeGraph::Node& node, bool installed, Timestamp start, Timestamp end) {
    std::optional<GridRunner::Report> done;
    auto job = submit(node, installed, start, end, done);
    if (!job) {
      ADD_FAILURE() << "submit failed: " << job.error();
      return {};
    }
    test::pumpUntil([&done] { return done.has_value(); }, 20000);
    if (!done) {
      ADD_FAILURE() << "no report";
      return {};
    }
    return read(std::move(*done));
  }
  static Evaluated read(GridRunner::Report report) {
    return {report, report};
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
          return t < 0 ? std::nullopt
                       : std::optional<InputStager::ScalarSample>(InputStager::ScalarSample{
                             threshold.topic_id, 1, std::min<Timestamp>(t, static_cast<Timestamp>(dense_count) - 1),
                             0.0});
        }
        const DataReader reader(engine);
        auto values = reader.series(threshold.topic_id, 0);
        const auto sample = name == "threshold" && values ? values->sampleAtOrBeforeTime(t) : std::nullopt;
        if (!sample) {
          return std::nullopt;
        }
        return InputStager::ScalarSample{threshold.topic_id, 0, sample->timestamp, sample->value};
      },
      [](ObjectTopicId) { return SessionManager::ParserBinding{}; }};
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

  const auto job = runner->submit(child, false, 10 * kSec, 10 * kSec, [](GridRunner::Report) {});
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
  EXPECT_TRUE(clean.parsed.error.empty());
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

  auto declared = inferred;
  declared.user_id = "declared";
  declared.outputs = {numberSpec()};
  declared.script = "local inputs = ...\nreturn inputs.cloud:count()\n";
  const Evaluated plain = fx.evaluate(fx.bind(declared), false, 10 * kSec, 10 * kSec);
  EXPECT_TRUE(plain.parsed.inferred_outputs.empty());
  EXPECT_EQ(numberAt(plain.parsed, 0), 3.0);
}

// ---- Measurement -----------------------------------------------------------------------------------------------

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
