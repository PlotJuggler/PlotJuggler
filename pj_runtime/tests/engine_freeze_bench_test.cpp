// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// EngineFreezeBench: how long the GUI thread is blocked when a whole-series
// computation is applied: a Luau transform over a long log (IncrementalEngine,
// via DataProcessorService::upsertTransform) and a marker rule over a long series
// (BatchEngine::upsertGenerator). Each case applies five fresh recipes and prints
// the medians of three numbers, measured from the call until its result is
// complete (a sliced replay finishing from the event loop included):
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
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_base/types.hpp"
#include "pj_bench_stats.h"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/SessionManager.h"

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

// One dataset holding one float64 column "a" at 100 Hz for 5000 s (500 000 rows),
// committed in 50k-row chunks. `a`'s samples are kept alongside for the marker
// case's resolver, so that case times the script, not a second storage read.
struct BenchDataset {
  SessionManager session;
  DatasetId ds = 0;
  std::vector<double> a_timestamps_ns;
  std::vector<double> a_values;

  BenchDataset() {
    DataEngine& engine = session.dataEngine();
    ds = *engine.createDataset(DatasetDescriptor{.source_name = "bench", .time_domain_id = 0});
    DataWriter writer = engine.createWriter();
    const auto handle = *writer.registerScalarSeries(ds, "a", NumericType::kFloat64);
    a_timestamps_ns.reserve(kBenchRows);
    a_values.reserve(kBenchRows);
    constexpr Timestamp kStepNs = 1'000'000'000LL / kBenchHz;
    for (std::size_t row = 0; row < kBenchRows; ++row) {
      const Timestamp stamp = static_cast<Timestamp>(row) * kStepNs;
      const double value = std::sin(static_cast<double>(row) * 0.01);
      writer.appendScalar(handle, stamp, value);
      a_timestamps_ns.push_back(static_cast<double>(stamp));
      a_values.push_back(value);
      if ((row + 1) % kBenchChunkRows == 0) {
        static_cast<void>(session.commitChunks(writer.flushAll()));
      }
    }
    static_cast<void>(session.commitChunks(writer.flushAll()));
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

// A marker rule scanning the whole series with 10 `math.sin` calls per sample and
// marking every sample above 0.99. The whole scan runs under ONE script fuel
// budget (pj_scripting/sandbox.h), unlike the transform's per-sample `calculate`,
// hence 10 calls per sample instead of 50.
constexpr const char* kMarkerRuleScript = R"LUAU(
  local s = series("a")
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
  BenchDataset fx;
  BatchEngine& batch = fx.session.batchEngine();
  batch.setResolver([&fx](DatasetId dataset, const std::string& key) -> std::optional<BatchEngine::ResolvedSeries> {
    if (dataset != fx.ds || key != "a") {
      return std::nullopt;
    }
    return BatchEngine::ResolvedSeries{.timestamps = fx.a_timestamps_ns, .values = fx.a_values};
  });
  std::vector<GuiCost> costs;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    BatchEngine::GeneratorRecipe recipe;
    recipe.id = "bench/m" + std::to_string(iteration);
    recipe.dataset_id = fx.ds;
    recipe.inputs = {"a"};
    recipe.outputs = {"bench_markers"};
    recipe.script = kMarkerRuleScript;
    costs.push_back(measureGuiCost(
        [&] {
          const auto result = batch.upsertGenerator(recipe);
          ASSERT_TRUE(result.has_value()) << result.error();
        },
        [] { return true; }));
  }
  report("ApplyMarkerRuleOverLongSeries", costs);
}

}  // namespace
}  // namespace PJ
