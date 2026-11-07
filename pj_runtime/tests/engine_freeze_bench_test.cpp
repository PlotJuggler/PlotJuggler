// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// EngineFreezeBench: how long the GUI thread is blocked when a whole-series
// computation is applied: a Luau transform over a long log (IncrementalEngine,
// via DataProcessorService::upsertTransform), marker rules over a long series
// (BatchEngine::upsertGenerator, the bounded wait of a layout load) and a rule's re-run after its
// input changed (the brake, off the GUI thread). The marker creates split the cost:
// staging alone (the resolver's copy), a trivial threshold rule (staging plus the
// per-sample binding every rule pays) and the heavy rule (adding 10 math.sin per
// sample). Each case applies five fresh recipes (the re-run: re-runs one rule five
// times) and prints the medians of three numbers, measured from the call until its
// result is complete (a sliced replay or a scheduled run finishing from the event
// loop included):
//   - GUI-thread CPU time, max_slice: the longest stretch the event loop could not
//     turn (the call itself, or one event-loop pass);
//   - GUI-thread CPU time, total: the sum of those stretches;
//   - worst wall-clock stall: the greatest lateness of a 1 ms heartbeat timer on
//     the GUI thread, i.e. the longest the GUI could not react, which the CPU
//     clock cannot see when the thread waits on a lock or is preempted.
// CPU time excludes time spent idle waiting for events. On Windows the thread
// clock ticks in scheduler quanta (about 15.6 ms), so read the Linux numbers.
// Printed, never asserted: machine-dependent thresholds only make a bench flaky.
// Skipped unless PJ_BENCH is set. Baselines: docs/derived_objects/ARCHITECTURE.md §19.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "batch_engine_test_utils.h"
#include "on_demand_test_utils.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/dataset.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_base/types.hpp"
#include "pj_bench_stats.h"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/TopicDemandTracker.h"

namespace PJ {
namespace {

struct GuiCost {
  double max_slice_ms = 0.0;    ///< GUI-thread CPU time: longest single stretch
  double total_ms = 0.0;        ///< GUI-thread CPU time: sum of the stretches
  double worst_stall_ms = 0.0;  ///< wall clock: worst heartbeat lateness
};

/// Wall-clock responsiveness probe: a 1 ms timer on the GUI thread, whose worst
/// lateness is the longest the event loop went unserved.
class Heartbeat {
 public:
  Heartbeat() {
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(1);
    QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] { beat(); });
  }

  void start() {
    expected_ = Clock::now() + kInterval;
    timer_.start();
  }

  /// Stops the timer and returns the worst lateness since `start`. A stall still
  /// open at the stop counts: a blocking call that returned without the event loop
  /// turning never got to fire the timer.
  double stop() {
    timer_.stop();
    beat();
    return worst_ms_;
  }

 private:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::milliseconds kInterval{1};

  void beat() {
    const Clock::time_point now = Clock::now();
    worst_ms_ = std::max(worst_ms_, std::chrono::duration<double, std::milli>(now - expected_).count());
    expected_ = now + kInterval;
  }

  QTimer timer_;
  Clock::time_point expected_;
  double worst_ms_ = 0.0;
};

/// Runs `apply` on this (the GUI) thread, then one event-loop pass at a time until
/// `done`, timing the call and each pass with the thread's CPU clock and the whole
/// span with the heartbeat.
GuiCost measureGuiCost(const std::function<void()>& apply, const std::function<bool()>& done) {
  GuiCost cost;
  Heartbeat heartbeat;
  heartbeat.start();
  const auto timed = [&cost](const std::function<void()>& work) {
    const double start = pj_bench::threadCpuMs();
    work();
    const double spent = pj_bench::threadCpuMs() - start;
    cost.max_slice_ms = std::max(cost.max_slice_ms, spent);
    cost.total_ms += spent;
  };
  timed(apply);
  while (!done()) {
    timed([] { QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents); });
  }
  cost.worst_stall_ms = heartbeat.stop();
  return cost;
}

/// Prints the medians of `costs` on one line, tagged with the case name.
void report(const char* name, const std::vector<GuiCost>& costs) {
  std::vector<double> max_slices;
  std::vector<double> totals;
  std::vector<double> stalls;
  for (const GuiCost& cost : costs) {
    max_slices.push_back(cost.max_slice_ms);
    totals.push_back(cost.total_ms);
    stalls.push_back(cost.worst_stall_ms);
  }
  std::cout << "[EngineFreezeBench] " << name
            << " GUI-thread CPU time: max_slice_median=" << pj_bench::stats(max_slices).median
            << " ms total_median=" << pj_bench::stats(totals).median
            << " ms | worst wall-clock stall: median=" << pj_bench::stats(stalls).median << " ms" << std::endl;
}

constexpr int kIterations = 5;
constexpr int kBenchHz = 100;
constexpr int kBenchSeconds = 5000;
constexpr std::size_t kBenchRows = static_cast<std::size_t>(kBenchHz) * static_cast<std::size_t>(kBenchSeconds);
constexpr std::size_t kBenchChunkRows = 50'000;

// One dataset of `session` holding one float64 column "a" at 100 Hz for 5000 s
// (500 000 rows), committed in 50k-row chunks. The samples are kept alongside for
// the create case's resolver, so that case times the script, not a storage read.
struct BenchSeries {
  DatasetId ds = 0;
  std::vector<double> timestamps_ns;
  std::vector<double> values;

  explicit BenchSeries(SessionManager& session) {
    DataEngine& engine = session.dataEngine();
    ds = *engine.createDataset(DatasetDescriptor{.source_name = "bench", .time_domain_id = 0});
    DataWriter writer = engine.createWriter();
    const auto handle = *writer.registerScalarSeries(ds, "a", NumericType::kFloat64);
    timestamps_ns.reserve(kBenchRows);
    values.reserve(kBenchRows);
    constexpr Timestamp kStepNs = 1'000'000'000LL / kBenchHz;
    for (std::size_t row = 0; row < kBenchRows; ++row) {
      const Timestamp stamp = static_cast<Timestamp>(row) * kStepNs;
      const double value = std::sin(static_cast<double>(row) * 0.01);
      writer.appendScalar(handle, stamp, value);
      timestamps_ns.push_back(static_cast<double>(stamp));
      values.push_back(value);
      if ((row + 1) % kBenchChunkRows == 0) {
        static_cast<void>(session.commitChunks(writer.flushAll()));
      }
    }
    static_cast<void>(session.commitChunks(writer.flushAll()));
  }
};

struct BenchDataset {
  SessionManager session;
  BenchSeries a{session};
};

// The series in a whole AppSession, cataloged, so a marker rule reads it through the
// resolver the app runs (AppSession installs it), as the series key "a/value".
struct BenchAppDataset {
  QTemporaryDir extensions_dir;
  AppSession app{extensions_dir.path()};
  BenchSeries a{app.sessionManager()};

  BenchAppDataset() {
    app.catalogModel().rebuildFromDatastore();
  }
};

// A Luau SISO transform whose `calculate` costs 50 `math.sin` calls per sample, a
// stand-in for a real transform's per-sample work. The class `id` must equal the
// `id` given to `upsertTransform`, which locates the class by it.
std::string heavyTransformScript(const std::string& id) {
  return "-- pj-script: luau\nreturn { id=\"" + id +
         "\", name=\"BenchHeavy\", output=\"same\",\n"
         "  create = function(p) return { calculate = function(t, v)\n"
         "    local s = 0\n"
         "    for k = 1, 50 do s = s + math.sin(v * k) end\n"
         "    return s\n"
         "  end } end }\n";
}

// A marker rule scanning the series `key` with 10 `math.sin` calls per sample and
// marking every sample above 0.99. The whole scan runs under ONE script fuel
// budget (pj_scripting/sandbox.h), unlike the transform's per-sample `calculate`,
// hence 10 calls per sample instead of 50.
std::string markerRuleScript(const std::string& key) {
  return "local s = series(\"" + key + "\")\n" + R"LUAU(
  for i = 0, s:size() - 1 do
    local pt = s:at(i)
    local acc = 0
    for k = 1, 10 do
      acc = acc + math.sin(pt.v * k)
    end
    if pt.v > 0.99 then
      createPointMarker(pt.t, pt.v, {label="hi"})
    end
  end
)LUAU";
}

// The fixed part of every marker rule: a threshold over the series `key`, reading
// each sample once and marking the ones above 0.99, with no other work per sample.
std::string trivialMarkerRuleScript(const std::string& key) {
  return "local s = series(\"" + key + "\")\n" + R"LUAU(
  for i = 0, s:size() - 1 do
    local pt = s:at(i)
    if pt.v > 0.99 then
      createPointMarker(pt.t, pt.v, {label="hi"})
    end
  end
)LUAU";
}

// Creates `script` five times as a rule over "a/value", read through the resolver
// AppSession installs, and reports the medians as `name`.
void benchCreateThroughTheAppResolver(const char* name, const std::string& script) {
  const PJ::test::ScopedCreateBudget budget(BatchEngine::kCreateBudget);
  BenchAppDataset fx;
  BatchEngine& batch = fx.app.sessionManager().batchEngine();
  std::vector<GuiCost> costs;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    BatchEngine::GeneratorRecipe recipe;
    recipe.id = std::string("bench/") + name + std::to_string(iteration);
    recipe.dataset_id = fx.a.ds;
    recipe.inputs = {"a/value"};
    recipe.outputs = {"bench_markers"};
    recipe.script = script;
    costs.push_back(measureGuiCost(
        [&] {
          const auto result = batch.upsertGenerator(recipe, BatchEngine::CreateWait::kBounded);
          ASSERT_TRUE(result.has_value()) << result.error();
        },
        [&] { return !batch.isRunning(recipe.id); }));
  }
  report(name, costs);
}

TEST(EngineFreezeBench, ApplyTransformOverLongLog) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  BenchDataset fx;
  DataProcessorService& service = fx.session.dataProcessorService();
  std::vector<GuiCost> costs;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const std::string id = "t" + std::to_string(iteration);
    costs.push_back(measureGuiCost(
        [&] {
          const auto result =
              service.upsertTransform("bench", id, {"a"}, {id + "_out"}, heavyTransformScript(id), "{}");
          ASSERT_TRUE(result.has_value()) << result.error();
        },
        [&] { return !service.materializationPending(); }));
  }
  report("ApplyTransformOverLongLog", costs);
}

TEST(EngineFreezeBench, ApplyMarkerRuleOverLongSeries) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  const PJ::test::ScopedCreateBudget budget(BatchEngine::kCreateBudget);
  BenchDataset fx;
  BatchEngine& batch = fx.session.batchEngine();
  batch.setResolver(
      [&fx](
          DatasetId dataset, const std::string& key,
          const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
        if (dataset != fx.a.ds || key != "a") {
          return std::nullopt;
        }
        return BatchEngine::ResolvedSeries{.timestamps = fx.a.timestamps_ns, .values = fx.a.values};
      });
  std::vector<GuiCost> costs;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    BatchEngine::GeneratorRecipe recipe;
    recipe.id = "bench/m" + std::to_string(iteration);
    recipe.dataset_id = fx.a.ds;
    recipe.inputs = {"a"};
    recipe.outputs = {"bench_markers"};
    recipe.script = markerRuleScript("a");
    // The bounded create (a layout load's) waits kCreateBudget for the run, which then
    // publishes from a drain: the result is complete once it no longer runs.
    costs.push_back(measureGuiCost(
        [&] {
          const auto result = batch.upsertGenerator(recipe, BatchEngine::CreateWait::kBounded);
          ASSERT_TRUE(result.has_value()) << result.error();
        },
        [&] { return !batch.isRunning(recipe.id); }));
  }
  report("ApplyMarkerRuleOverLongSeries", costs);
}

// Staging alone: the resolver copies the 500k rows, and the script only looks the
// series up (no per-sample binding).
TEST(EngineFreezeBench, StageMarkerInputOverLongSeries) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  benchCreateThroughTheAppResolver(
      "StageMarkerInputOverLongSeries", "local s = series(\"a/value\")\nlocal n = s:size()\n");
}

// The fixed overhead every marker rule pays: staging plus reading each sample once.
TEST(EngineFreezeBench, ApplyTrivialMarkerRuleOverLongSeries) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  benchCreateThroughTheAppResolver("ApplyTrivialMarkerRuleOverLongSeries", trivialMarkerRuleScript("a/value"));
}

TEST(EngineFreezeBench, RerunMarkerRuleOverLongSeries) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  BenchAppDataset fx;
  BatchEngine& batch = fx.app.sessionManager().batchEngine();
  batch.setReloadHz(BatchEngine::kMaxReloadHz);
  BatchEngine::GeneratorRecipe recipe;
  recipe.id = "bench/rerun";
  recipe.dataset_id = fx.a.ds;
  recipe.inputs = {"a/value"};
  recipe.outputs = {"bench_markers"};
  recipe.script = markerRuleScript("a/value");
  const auto created = batch.upsertGenerator(recipe);
  ASSERT_TRUE(created.has_value()) << created.error();
  std::vector<GuiCost> costs;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    costs.push_back(measureGuiCost(
        [&] { ASSERT_FALSE(batch.recomputeForChangedInputs({"a"}, fx.a.ds).empty()); },
        [&] { return batch.staleIds().empty(); }));
  }
  report("RerunMarkerRuleOverLongSeries", costs);
}

// PERF-01, PERF-04: the GUI thread's CPU time of one playhead tick with one demanded on-demand recipe over a lazily
// stored 260k-point cloud, through AppSession (the demanded keys, the conversion, the staging, the history lookup and
// the submission): cold, a new instant each tick (a miss, submitted); warm, between two instants already computed (a
// hit). The real Luau run passes the cloud through on a worker, off the timer. Then the CPU time of an install (the
// resolver, the compile, the history install) in a dataset of 1 and of 10,000 object topics: the difference is the
// resolver's scan, which runs per install, never per tick.
TEST(EngineFreezeBench, OnDemandColdAndWarm) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr int kTicks = 101;
  constexpr Timestamp kStep = 100'000'000;
  constexpr const char* kMetadata = R"({"builtin_object_type":"kPointCloud"})";
  QTemporaryDir dir;
  AppSession app(dir.path());
  SessionManager& session = app.sessionManager();
  ObjectStore& store = session.objectStore();
  const DatasetId ds = session.dataEngine().createDataset(DatasetDescriptor{.source_name = "log"}).value();
  const ObjectTopicId cloud = store.registerTopic({ds, "cloud", kMetadata}).value();
  const sdk::PayloadView bytes =
      sdk::makePayloadView(serializeBuiltinObject(sdk::BuiltinObject(test::makeCloud(260'000))).value());
  for (int tick = 0; tick < kTicks; ++tick) {
    ASSERT_TRUE(store.pushLazy(cloud, tick * kStep, [bytes]() -> std::optional<sdk::PayloadView> { return bytes; })
                    .has_value());
  }
  const auto passThrough = [](std::string id, std::string input) {
    RecipeGraph::Recipe recipe = test::onDemandRecipe(
        std::move(id), {std::move(input)},
        {{"cropped", scripting::OutputType::kObject, sdk::BuiltinObjectType::kPointCloud}});
    recipe.script = "local inputs = ...\nreturn { cropped = inputs.cloud }\n";
    return recipe;
  };
  ASSERT_TRUE(session.onDemandService().install(passThrough("crop", "cloud")).has_value());
  app.topicDemandTracker().addReference(ds, QStringLiteral("owner/crop/cropped"));
  PlaybackEngine& playback = app.playbackEngine();
  playback.setRange(DisplayRange{DisplaySeconds{0.0}, DisplaySeconds{20.0}});
  const auto tickCpuUs = [&](Timestamp t) {
    const double start = pj_bench::threadCpuMs();
    playback.setCurrentTime(rawToDisplaySeconds(t, DisplayOffset{}));
    return (pj_bench::threadCpuMs() - start) * 1000.0;
  };
  std::vector<double> cold;
  for (int tick = 1; tick < kTicks; ++tick) {
    cold.push_back(tickCpuUs(tick * kStep));
    session.enginePool().waitIdle();
  }
  ASSERT_EQ(session.onDemandService().state("owner/crop")->status, NodeScheduler::NodeState::Status::kReady);
  std::vector<double> warm;
  for (int tick = 0; tick < kTicks; ++tick) {
    warm.push_back(tickCpuUs((kTicks - 1 - tick % 2) * kStep));
    session.enginePool().waitIdle();
  }
  const auto median = [](std::vector<double> values) {
    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
    return values[values.size() / 2];
  };

  const auto installCpuUs = [&](DatasetId dataset, const std::string& input, int installs) {
    std::vector<double> times;
    for (int index = 0; index < installs; ++index) {
      RecipeGraph::Recipe recipe = passThrough("scan" + std::to_string(dataset) + "_" + std::to_string(index), input);
      const double start = pj_bench::threadCpuMs();
      EXPECT_TRUE(session.onDemandService().install(std::move(recipe)).has_value());
      times.push_back((pj_bench::threadCpuMs() - start) * 1000.0);
    }
    return median(std::move(times));
  };
  const DatasetId small = session.dataEngine().createDataset(DatasetDescriptor{.source_name = "small"}).value();
  ASSERT_TRUE(store.registerTopic({small, "only", kMetadata}).has_value());
  const DatasetId large = session.dataEngine().createDataset(DatasetDescriptor{.source_name = "large"}).value();
  for (int index = 0; index < 10'000; ++index) {
    ASSERT_TRUE(store.registerTopic({large, "topic_" + std::to_string(index), kMetadata}).has_value());
  }
  const double one = installCpuUs(small, "only", 11);
  const double ten_thousand = installCpuUs(large, "topic_9999", 11);
  std::printf(
      "[bench] OnDemandColdAndWarm, a demanded 260k-point recipe: GUI CPU per tick cold %.2f us, warm %.2f us "
      "(medians of %d); install among 1 / 10k object topics: %.1f / %.1f us\n",
      median(cold), median(warm), kTicks - 1, one, ten_thousand);

  // The same ticks for a recipe that also reads 4 scalar series, which resolve through the catalog at every staging,
  // with the 10k topics above listed in the catalog.
  DataWriter writer = session.dataEngine().createWriter();
  RecipeGraph::Recipe scaled = passThrough("scaled", "cloud");
  for (int index = 0; index < 4; ++index) {
    const ScalarSeriesHandle gain =
        writer.registerScalarSeries(ds, "gain_" + std::to_string(index), NumericType::kFloat64).value();
    for (int tick = 0; tick < kTicks; ++tick) {
      writer.appendScalar(gain, tick * kStep, static_cast<double>(tick));
    }
    scaled.inputs.push_back("gain_" + std::to_string(index) + "/value");
  }
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
  app.catalogModel().rebuildFromDatastore();
  const size_t items = app.catalogModel().items().size();
  app.topicDemandTracker().removeReference(ds, QStringLiteral("owner/crop/cropped"));
  ASSERT_TRUE(session.onDemandService().install(scaled).has_value());
  app.topicDemandTracker().addReference(ds, QStringLiteral("owner/scaled/cropped"));
  session.enginePool().waitIdle();
  std::vector<double> scalar_cold;
  for (int tick = 1; tick < kTicks; ++tick) {
    scalar_cold.push_back(tickCpuUs(tick * kStep));
    session.enginePool().waitIdle();
  }
  ASSERT_EQ(session.onDemandService().state("owner/scaled")->status, NodeScheduler::NodeState::Status::kReady);
  std::vector<double> scalar_warm;
  for (int tick = 0; tick < kTicks; ++tick) {
    scalar_warm.push_back(tickCpuUs((kTicks - 1 - tick % 2) * kStep));
    session.enginePool().waitIdle();
  }
  std::printf(
      "[bench] OnDemandColdAndWarm, the recipe plus 4 scalar inputs among %zu catalog items: GUI CPU per tick cold "
      "%.2f us, warm %.2f us\n",
      items, median(scalar_cold), median(scalar_warm));
}

}  // namespace
}  // namespace PJ
