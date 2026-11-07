// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// NodeScheduler under D30, driven through the OnDemandService facade that wires it: a real ObjectStore,
// DataEngine, EnginePool, ResultHistory and InputStager, and an injected fake script run that records where and
// what it ran and waits for a gate or a delay (90 ms for the slow script). One end-to-end test runs the real Luau
// engine. Waits are `EnginePool::waitIdle` or a bounded event pump, never a nested WaitForMoreEvents loop.

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "batch_engine_test_utils.h"
#include "event_pump_test_utils.h"
#include "on_demand_test_utils.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/OnDemandService.h"
#ifdef PJ_PYTHON_TEST_PROBE
#include "pj_scripting/python_object_script.h"
#include "python_test_probe.h"
#endif

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
using NodeState = NodeScheduler::NodeState;
using Status = NodeState::Status;

constexpr Timestamp kSec = 1'000'000'000;
constexpr Timestamp kMs = 1'000'000;

OutputSpec cloudSpec(std::string name = "cropped") {
  return {std::move(name), OutputType::kObject, sdk::BuiltinObjectType::kPointCloud};
}
OutputSpec numberSpec(std::string name = "count") {
  return {std::move(name), OutputType::kNumber, sdk::BuiltinObjectType::kNone};
}

/// `owner/<id>` reading `inputs`, with `outputs`; its script is the fake's to run.
RecipeGraph::Recipe recipe(
    std::string id, std::vector<std::string> inputs = {"cloud"}, std::vector<OutputSpec> outputs = {cloudSpec()}) {
  return test::onDemandRecipe(std::move(id), std::move(inputs), std::move(outputs));
}

/// The canonical bytes of a cloud of `points` points.
std::vector<uint8_t> canonicalCloud(size_t points) {
  return serializeBuiltinObject(sdk::BuiltinObject(test::makeCloud(points))).value();
}

using test::FakeRun;

/// One dataset: the object topics `cloud` (canonical bytes) and `lidar` (raw points behind a parser), the scalar
/// series `threshold` and the string topic `state`. The service runs the fake unless built with the real Luau run.
struct Fixture {
  explicit Fixture(
      std::size_t budget_bytes = 8 * InFlightBudget::kRunBytes, bool real_run = false,
      ResultHistory::Limits limits = {})
      : budget(budget_bytes) {
    dataset = engine.createDataset(DatasetDescriptor{.source_name = "log"}).value();
    cloud = store.registerTopic({dataset, "cloud", ""}).value();
    lidar = store.registerTopic({dataset, "lidar", ""}).value();
    threshold = writer.registerScalarSeries(dataset, "threshold", NumericType::kFloat64).value();
    status_topic = test::registerStringTopic(writer, dataset, "state");
    lidar_parser = std::make_shared<test::RawPointsParser>(probe);
    EXPECT_TRUE(lidar_parser->bindSchema("raw_points", {}).has_value());
    service = makeService(real_run, limits);
  }

  std::unique_ptr<OnDemandService> makeService(bool real_run, ResultHistory::Limits limits) {
    OnDemandService::Services services;
    services.limits = limits;
    services.resolver = [this](std::string_view input) {
      if (store.findTopic(dataset, input)) {
        return std::vector<RecipeGraph::InputCandidate>{
            {dataset, sdk::BuiltinObjectType::kPointCloud, std::string(input)}};
      }
      if (input == "threshold" || input == "state") {
        return std::vector<RecipeGraph::InputCandidate>{{dataset, sdk::BuiltinObjectType::kNone, std::string(input)}};
      }
      return std::vector<RecipeGraph::InputCandidate>{};
    };
    services.scalars =
        [this](DatasetId in, std::string_view name, Timestamp t) -> std::optional<InputStager::ScalarSample> {
      if (in != dataset || (name != "threshold" && name != "state")) {
        return std::nullopt;
      }
      return InputStager::sampleAt(engine, name == "state" ? status_topic : threshold.topic_id, 0, t);
    };
    services.parsers = [this](ObjectTopicId topic) {
      std::shared_ptr<void> library = lidar_library ? lidar_library : std::shared_ptr<void>(lidar_parser);
      return topic == lidar ? SessionManager::ParserBinding{lidar_parser.get(), probe.topic_lock, std::move(library)}
                            : SessionManager::ParserBinding{};
    };
    services.scheduling.tf = [this](DatasetId) { return tf; };
    services.scheduling.tf_revision = [this](DatasetId) { return tf != nullptr ? tf->revision() : 0; };
    services.scheduling.epoch = [this](DatasetId) { return epoch; };
    if (!real_run) {
      services.scheduling.run = fake.function();
    }
    services.scheduling.published = [this](const std::string& key) { published.push_back(key); };
    services.scheduling.state_changed = [this](const std::string& key) { state_changes.push_back(key); };
    return std::make_unique<OnDemandService>(store, engine, pool, budget, std::move(services));
  }

  void install(RecipeGraph::Recipe recipe) {
    const auto installed = service->install(std::move(recipe));
    ASSERT_TRUE(installed.has_value()) << installed.error();
  }
  void pushCloud(Timestamp t, size_t points = 4) {
    ASSERT_TRUE(store.pushOwned(cloud, t, canonicalCloud(points)).has_value());
  }
  void pushThreshold(Timestamp t, double value) {
    writer.appendScalar(threshold, t, value);
    EXPECT_FALSE(engine.commitChunks(writer.flushAll()).empty());
  }
  void pushState(Timestamp t, std::string_view value) {
    test::appendString(writer, status_topic, t, value);
    EXPECT_FALSE(engine.commitChunks(writer.flushAll()).empty());
  }
  void request(std::string_view key, Timestamp t) {
    service->requestAt(key, t);
  }
  /// Publishes everything submitted, and what that submits.
  void settle() {
    pool.waitIdle();
  }
  [[nodiscard]] NodeState state(std::string_view key) const {
    const auto found = service->state(key);
    EXPECT_TRUE(found.has_value()) << "no state for " << key;
    return found.value_or(NodeState{});
  }
  [[nodiscard]] ObjectTopicId topic(const std::string& name) const {
    return store.findTopic(dataset, name).value_or(ObjectTopicId{});
  }
  [[nodiscard]] std::vector<Timestamp> ring(const std::string& name) const {
    return test::timestampsOf(store, topic(name));
  }
  [[nodiscard]] sdk::PointCloud shown(const std::string& name, Timestamp t) const {
    return test::decodeAt<sdk::PointCloud>(store, topic(name), t);
  }
  /// Whether `points` lie in the payload of the entry `name` shows at `t`: a view over that ring entry, not a copy.
  [[nodiscard]] bool inRingEntry(const std::string& name, Timestamp t, const uint8_t* points) const {
    const auto entry = store.latestAt(topic(name), t);
    if (!entry || points == nullptr) {
      return false;
    }
    const std::less_equal<const uint8_t*> at_or_before;
    const uint8_t* begin = entry->payload.bytes.data();
    return at_or_before(begin, points) && at_or_before(points, begin + entry->payload.bytes.size());
  }
  [[nodiscard]] size_t publishedCount(const std::string& key) const {
    return static_cast<size_t>(std::count(published.begin(), published.end(), key));
  }

  DataEngine engine;
  DataWriter writer{engine.createWriter()};
  ObjectStore store;
  DatasetId dataset = 0;
  ObjectTopicId cloud;
  ObjectTopicId lidar;
  ScalarSeriesHandle threshold;
  TopicId status_topic = 0;  ///< the string topic `state`
  test::ParseProbe probe;
  std::shared_ptr<test::RawPointsParser> lidar_parser;
  std::shared_ptr<void> lidar_library;  ///< set: the lidar binding's library keepalive, in place of the parser
  std::shared_ptr<const pj::tf::TransformSnapshot> tf;
  uint64_t epoch = 0;
  FakeRun fake;
  std::vector<std::string> published;
  std::vector<std::string> state_changes;
  InFlightBudget budget;  ///< outlives the pool: a run still on it holds its reservation
  EnginePool pool;        ///< after the store, the fake and the budget: destroyed first, joining the runs
  std::unique_ptr<OnDemandService> service;
};

std::string frameOf(Timestamp query_time) {
  return std::to_string(query_time);
}

// ---- lanes --------------------------------------------------------------------------------------------------------

// PY-01: a Python recipe's runs take the Python lane, the one thread the GIL serializes anyway: two Python nodes run
// there and settle ready, a Luau node beside them runs on the shared lane.
TEST(NodeSchedulerTest, PythonRecipeEvaluatesOnThePythonLane) {
  Fixture fx;
  const std::thread::id python_lane = test::pythonLaneThread(fx.pool);
  fx.pushCloud(10 * kSec);
  fx.pushCloud(20 * kSec);
  fx.install(test::pythonRecipe("first", {"cloud"}, {cloudSpec()}));
  fx.install(test::pythonRecipe("second", {"cloud"}, {cloudSpec()}));
  fx.install(recipe("luau"));
  fx.request("owner/first", 10 * kSec);
  fx.request("owner/second", 10 * kSec);
  fx.request("owner/luau", 20 * kSec);  // the time names its run
  fx.settle();

  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 3u);
  for (const FakeRun::Record& record : records) {
    EXPECT_EQ(record.thread == python_lane, record.query_time == 10 * kSec) << record.query_time;
  }
  for (const char* key : {"owner/first", "owner/second", "owner/luau"}) {
    EXPECT_EQ(fx.state(key).status, Status::kReady) << key;
  }
}

// A Python proxy of a parser-backed input that the script parks past its run (on a class its module copy shares, the
// trust model's documented limit) keeps the parser's library loaded: removing the node, the entry and the session's
// binding leaves the library loaded, and the payload's deleter (plugin code) runs before it unloads.
TEST(NodeSchedulerTest, APythonProxyKeptPastItsRunKeepsTheParserLibraryLoaded) {
  Fixture fx(8 * InFlightBudget::kRunBytes, /*real_run=*/true);
  auto library = std::make_shared<int>(1);
  const std::weak_ptr<int> loaded = library;
  fx.lidar_library = library;
  auto deletes_after_unload = std::make_shared<std::atomic<int>>(0);
  ASSERT_TRUE(
      fx.store.pushLazy(fx.lidar, 10 * kSec, test::lazyPointsNotingUnload(loaded, deletes_after_unload)).has_value());
  fx.pushCloud(10 * kSec);
  auto keeper = test::pythonRecipe("keeper", {"lidar"}, {numberSpec("n")});
  keeper.script =
      "import statistics\ndef evaluate(inputs, params):\n    statistics.NormalDist.kept = inputs['lidar']\n"
      "    return {'n': inputs['lidar'].count()}\n";
  fx.install(keeper);
  fx.request("owner/keeper", 10 * kSec);
  fx.settle();
  ASSERT_EQ(fx.state("owner/keeper").status, Status::kReady) << fx.state("owner/keeper").detail;

  (void)fx.service->remove("owner/keeper");
  test::prune(fx.store, fx.lidar);
  fx.lidar_library.reset();
  library.reset();
  EXPECT_FALSE(loaded.expired()) << "the parked proxy keeps the library loaded";

  auto release = test::pythonRecipe("release", {"cloud"}, {numberSpec("n")});
  release.script =
      "import statistics\ndef evaluate(inputs, params):\n    del statistics.NormalDist.kept\n    return 1\n";
  fx.install(release);
  fx.request("owner/release", 10 * kSec);
  fx.settle();
  ASSERT_EQ(fx.state("owner/release").status, Status::kReady) << fx.state("owner/release").detail;
  EXPECT_EQ(deletes_after_unload->load(), 0);
  EXPECT_TRUE(loaded.expired()) << "the proxy released the library with the object";
}

// The scheduler's cancellation reaches a running Python loop through the watchdog: removing its node stops the run with
// "script cancelled", and the Python lane then runs the next node. The runs' timeout is 600 s, so a cancellation that
// never reached the loop hangs the test instead of passing on a timeout.
TEST(NodeSchedulerTest, RemovingARunningPythonLoopCancelsIt) {
  std::mutex mutex;
  std::vector<std::string> outcomes;
  std::atomic<int> started{0};
  OnDemandService::setRunForTest([&](const scripting::CompiledOnDemandScript& script, Span<const NamedValue> inputs,
                                     std::string_view params, Span<const OutputSpec> outputs,
                                     const RunOptions& options) {
    ++started;
    RunOptions patient = options;
    patient.limits.python_watchdog_millis = 600'000;
    auto ran = scripting::runOnDemand(script, inputs, params, outputs, patient);
    const std::lock_guard<std::mutex> lock(mutex);
    outcomes.push_back(ran ? std::string("ok") : ran.error().message);
    return ran;
  });
  struct RestoreRun {
    ~RestoreRun() {
      OnDemandService::setRunForTest({});
    }
  } restore;
  Fixture fx(8 * InFlightBudget::kRunBytes, /*real_run=*/true);
  fx.pushCloud(10 * kSec);
  auto loop = test::pythonRecipe("loop", {"cloud"}, {numberSpec("n")});
  loop.script = "def evaluate(inputs, params):\n    while True:\n        pass\n";
  fx.install(loop);
  fx.request("owner/loop", 10 * kSec);
  for (int spin = 0; spin < 500 && started.load() == 0; ++spin) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(started.load(), 1);
  (void)fx.service->remove("owner/loop");
  auto next = test::pythonRecipe("next", {"cloud"}, {numberSpec("n")});
  next.script = "def evaluate(inputs, params):\n    return 1\n";
  fx.install(next);
  fx.request("owner/next", 10 * kSec);
  fx.settle();
  EXPECT_EQ(fx.state("owner/next").status, Status::kReady) << fx.state("owner/next").detail;
  const std::lock_guard<std::mutex> lock(mutex);
  EXPECT_EQ(outcomes, (std::vector<std::string>{"script cancelled", "ok"}));
}

#ifdef PJ_PYTHON_TEST_PROBE
/// The real script run behind `OnDemandService::setRunForTest`, counting the runs that start and keeping each one's
/// outcome ("ok" or its error); the default run comes back with it. Set it up before the fixture, which reads it.
struct ObservedRun {
  ObservedRun() {
    // Booted first, so a `test::PythonGilHold` can take the GIL: an eager compile boots the interpreter.
    EXPECT_TRUE(scripting::compileOnDemandScriptPython("def evaluate(inputs, params):\n    return 1\n").has_value());
    OnDemandService::setRunForTest([this](
                                       const scripting::CompiledOnDemandScript& script, Span<const NamedValue> inputs,
                                       std::string_view params, Span<const OutputSpec> outputs,
                                       const RunOptions& options) {
      ++started;
      auto ran = scripting::runOnDemand(script, inputs, params, outputs, options);
      const std::lock_guard<std::mutex> lock(mutex);
      outcomes.push_back(ran ? std::string("ok") : ran.error().message);
      return ran;
    });
  }
  ~ObservedRun() {
    OnDemandService::setRunForTest({});
  }
  ObservedRun(const ObservedRun&) = delete;
  ObservedRun& operator=(const ObservedRun&) = delete;

  /// Waits up to 5 s for `runs` runs to have started.
  [[nodiscard]] bool awaitStarted(int runs) const {
    for (int spin = 0; spin < 500 && started.load() < runs; ++spin) {
      std::this_thread::sleep_for(10ms);
    }
    return started.load() >= runs;
  }
  [[nodiscard]] std::vector<std::string> results() {
    const std::lock_guard<std::mutex> lock(mutex);
    return outcomes;
  }

  std::atomic<int> started{0};
  std::mutex mutex;
  std::vector<std::string> outcomes;
};
#endif

// An edit while the old revision's run waits on the Python lane to compile (the GIL held, as by a script stuck in a
// builtin C loop) returns at once; that run, a compile error, ends dropped: the node never shows it, and the new
// revision runs and settles ready.
TEST(NodeSchedulerTest, AnEditWhileItsOldRevisionCompilesDropsTheOldResult) {
#ifndef PJ_PYTHON_TEST_PROBE
  GTEST_SKIP() << "this build has no Python";
#else
  ObservedRun observed;
  Fixture fx(8 * InFlightBudget::kRunBytes, /*real_run=*/true);
  fx.pushCloud(10 * kSec);
  auto edited = test::pythonRecipe("edited", {"cloud"}, {numberSpec("n")});
  edited.script = "def evaluate(inputs, params:\n    return 1\n";
  {
    const test::PythonGilHold hold;
    fx.install(edited);
    fx.request("owner/edited", 10 * kSec);
    ASSERT_TRUE(observed.awaitStarted(1));
    EXPECT_EQ(fx.state("owner/edited").status, Status::kPending);
    edited.script = "def evaluate(inputs, params):\n    return 2\n";
    const auto begin = std::chrono::steady_clock::now();
    fx.install(edited);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 250ms) << "the edit never waits for the GIL";
  }
  fx.settle();
  const NodeState state = fx.state("owner/edited");
  EXPECT_EQ(state.status, Status::kReady) << state.detail;
  EXPECT_EQ(state.detail, "");
  const std::vector<std::string> outcomes = observed.results();
  ASSERT_EQ(outcomes.size(), 2u);
  EXPECT_NE(outcomes[0].find("SyntaxError"), std::string::npos) << outcomes[0] << ": the old revision compiled";
  EXPECT_EQ(outcomes[1], "ok");
#endif
}

// A recipe removed while its run waits on the Python lane to compile: the removal returns at once, and the run, once
// the GIL is free, compiles, sees its cancellation and ends "script cancelled", publishing nothing and touching no
// state.
TEST(NodeSchedulerTest, RemovingAPythonRecipeWhileItCompilesPublishesNothing) {
#ifndef PJ_PYTHON_TEST_PROBE
  GTEST_SKIP() << "this build has no Python";
#else
  ObservedRun observed;
  Fixture fx(8 * InFlightBudget::kRunBytes, /*real_run=*/true);
  fx.pushCloud(10 * kSec);
  auto gone = test::pythonRecipe("gone", {"cloud"}, {cloudSpec()});
  gone.script = "def evaluate(inputs, params):\n    return {'cropped': inputs['cloud']}\n";
  {
    const test::PythonGilHold hold;
    fx.install(gone);
    fx.request("owner/gone", 10 * kSec);
    ASSERT_TRUE(observed.awaitStarted(1));
    const auto begin = std::chrono::steady_clock::now();
    (void)fx.service->remove("owner/gone");
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 250ms) << "the removal never waits for the GIL";
  }
  fx.settle();
  EXPECT_EQ(observed.results(), (std::vector<std::string>{"script cancelled"}));
  EXPECT_EQ(fx.publishedCount("owner/gone"), 0u);
  EXPECT_FALSE(fx.service->state("owner/gone").has_value());
#endif
}

// ---- D30: the result policy --------------------------------------------------------------------------------

// GAP-02: requests every 33 ms against a 90 ms run keep producing results while the cursor moves. Every completed
// run is published (none dropped for being overtaken by a newer time), the display only moves forward, and it
// converges on the last request.
TEST(NodeSchedulerTest, SlowScriptStillPublishesWhileCursorMoves) {
  Fixture fx;
  fx.fake.delay = 90ms;
  constexpr int kRequests = 30;
  for (int index = 0; index < kRequests; ++index) {
    fx.pushCloud(index * 33 * kMs);
  }
  fx.install(recipe("crop"));
  for (int index = 0; index < kRequests; ++index) {
    fx.request("owner/crop", index * 33 * kMs);
    test::pumpFor(33);
  }
  const size_t while_moving = fx.published.size();
  fx.settle();

  EXPECT_GE(while_moving, 3u) << "results kept coming while the cursor moved";
  EXPECT_EQ(fx.published.size(), static_cast<size_t>(fx.fake.finished())) << "no completed run was dropped";
  EXPECT_LT(fx.fake.finished(), kRequests) << "the pending slot took the requests made during a run";
  const auto records = fx.fake.records();
  for (size_t index = 1; index < records.size(); ++index) {
    EXPECT_LT(records[index - 1].query_time, records[index].query_time) << "each run computes a newer time";
  }
  const Timestamp last = (kRequests - 1) * 33 * kMs;
  const NodeState state = fx.state("owner/crop");
  EXPECT_EQ(state.status, Status::kReady);
  EXPECT_EQ(state.requested_t, last);
  EXPECT_EQ(state.lag, Timestamp{0});
  EXPECT_EQ(fx.shown("owner/crop/cropped", last).frame_id, frameOf(last));
}

// GAP-03: one running run and one pending request, which keeps only the latest time: nine requests during a run
// cost one more run, at the newest time.
TEST(NodeSchedulerTest, PendingSlotKeepsOnlyTheLatestTime) {
  Fixture fx;
  fx.fake.gated = true;
  for (int second = 1; second <= 10; ++second) {
    fx.pushCloud(second * kSec);
  }
  fx.install(recipe("crop"));
  fx.request("owner/crop", 1 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  for (int second = 2; second <= 10; ++second) {
    fx.request("owner/crop", second * kSec);
  }
  EXPECT_EQ(fx.budget.reserved(), InFlightBudget::kRunBytes) << "one run in flight";
  EXPECT_EQ(fx.state("owner/crop").status, Status::kPending);
  EXPECT_EQ(fx.state("owner/crop").requested_t, 10 * kSec);

  fx.fake.allow(2);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].query_time, 1 * kSec);
  EXPECT_EQ(records[1].query_time, 10 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.state("owner/crop").requested_t, 10 * kSec);
}

// D30, ASY-09: a seek back while a run computes cancels nothing. The running run completes and is published, then
// the pending request runs, and the display settles on the newest request's result.
TEST(NodeSchedulerTest, TimeOnlyChangeDoesNotCancelTheRunningRun) {
  Fixture fx;
  fx.fake.gated = true;
  fx.pushCloud(5 * kSec);
  fx.pushCloud(20 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 20 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  fx.request("owner/crop", 5 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kPending);

  fx.fake.allow(2);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_FALSE(records[0].cancelled) << "a time-only change never cancels";
  EXPECT_EQ(records[1].query_time, 5 * kSec);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{5 * kSec, 20 * kSec})) << "both published";
  EXPECT_EQ(fx.shown("owner/crop/cropped", 5 * kSec).frame_id, frameOf(5 * kSec));
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.state("owner/crop").requested_t, 5 * kSec);
}

// D30: a change of meaning cancels the running run and drops its result. A new revision (ASY-03) re-evaluates at
// the node's cursor without a new request (CUR-13). A new epoch of the dataset (ASY-04) drops the run's result at
// its publication, which serves the cursor again, and cancels the run at the next request. Removal:
// RemovalMidFlightPublishesNothing.
TEST(NodeSchedulerTest, MeaningChangeCancelsTheRunningRun) {
  Fixture fx;
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  RecipeGraph::Recipe crop = recipe("crop");
  fx.install(crop);
  fx.request("owner/crop", 10 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  crop.params_json = R"({"edited": true})";
  fx.install(crop);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kPending) << "the edit re-requests at the cursor";
  fx.fake.allow(2);
  fx.settle();
  auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_TRUE(records[0].cancelled) << "the old revision's run";
  EXPECT_EQ(records[1].query_time, 10 * kSec) << "the new revision at the cursor";
  EXPECT_EQ(fx.publishedCount("owner/crop"), 1u) << "only the new revision's result";
  EXPECT_EQ(fx.ring("owner/crop/cropped"), std::vector<Timestamp>{10 * kSec});
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);

  fx.pushCloud(20 * kSec);
  fx.request("owner/crop", 20 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(3));
  fx.epoch = 1;  // the dataset's storage was replaced, and nothing requests the node again
  fx.fake.allow(2);
  fx.settle();
  records = fx.fake.records();
  ASSERT_EQ(records.size(), 4u);
  EXPECT_FALSE(records[2].cancelled) << "no request saw the new epoch while it ran";
  EXPECT_EQ(fx.publishedCount("owner/crop"), 2u) << "only the new epoch's result";
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady) << "not left pending";

  fx.pushCloud(30 * kSec);
  fx.request("owner/crop", 30 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(5));
  fx.epoch = 2;
  fx.request("owner/crop", 30 * kSec);
  fx.fake.allow(2);
  fx.settle();
  records = fx.fake.records();
  ASSERT_EQ(records.size(), 6u);
  EXPECT_TRUE(records[4].cancelled) << "the request saw the run's epoch go";
  EXPECT_EQ(fx.publishedCount("owner/crop"), 3u);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
}

// D30: an older completion never replaces a newer result. The 10 s run lands after the 20 s request was served from
// the cache: the store's at-or-before read at 20 s still shows the 20 s result, and the state stays the newest
// request's.
TEST(NodeSchedulerTest, OlderCompletionNeverReplacesNewerResult) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.pushCloud(20 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 20 * kSec);
  fx.settle();
  fx.fake.gated = true;
  fx.request("owner/crop", 10 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(2));
  fx.request("owner/crop", 20 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady) << "served from the cache";

  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{10 * kSec, 20 * kSec}));
  EXPECT_EQ(fx.shown("owner/crop/cropped", 20 * kSec).frame_id, frameOf(20 * kSec));
  const NodeState state = fx.state("owner/crop");
  EXPECT_EQ(state.status, Status::kReady);
  EXPECT_EQ(state.requested_t, 20 * kSec);
  EXPECT_EQ(state.lag, Timestamp{0});
}

// D30 (clarified 2026-10-18): the lag is how far the shown result is behind the one the current request produces.
// With a sparse input the up-to-date result shows no lag, though the cursor is 5 s past its stamp.
TEST(NodeSchedulerTest, LagIsMeasuredAgainstTheCurrentRequestsTarget) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 10 * kSec);
  fx.settle();
  fx.request("owner/crop", 15 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.state("owner/crop").lag, Timestamp{0}) << "not cursor minus stamp";
  EXPECT_EQ(fx.fake.finished(), 1);

  fx.fake.gated = true;
  fx.pushCloud(20 * kSec);
  fx.request("owner/crop", 25 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kPending);
  EXPECT_EQ(fx.state("owner/crop").lag, 10 * kSec) << "the 10 s result shown, the 20 s one coming";
  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.state("owner/crop").lag, Timestamp{0});
}

// ---- D6, D25: missing and unavailable inputs ---------------------------------------------------------------

// ASY-01: a request goes pending, then ready once its run is published.
TEST(NodeSchedulerTest, RequestAtGoesPendingThenReady) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  EXPECT_TRUE(fx.ring("owner/crop/cropped").empty()) << "an install publishes nothing";
  fx.request("owner/crop", 10 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kPending);
  EXPECT_TRUE(fx.ring("owner/crop/cropped").empty());
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), std::vector<Timestamp>{10 * kSec});
}

// ASY-07, CUR-06, CUR-11 (D6): a missing input runs nothing, says which input, and with nothing shown to clear
// writes nothing.
TEST(NodeSchedulerTest, AMissingInputRunsNothingAndNamesTheInput) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 5 * kSec);
  const NodeState state = fx.state("owner/crop");
  EXPECT_EQ(state.status, Status::kMissingInput);
  EXPECT_EQ(state.detail, "on-demand: missing input 'cloud'");
  EXPECT_EQ(state.requested_t, 5 * kSec);
  EXPECT_EQ(fx.budget.reserved(), 0u) << "no run";
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 0);
  EXPECT_TRUE(fx.ring("owner/crop/cropped").empty());
}

// D6: a missing input clears the display even where a previous result was shown.
TEST(NodeSchedulerTest, MissingInputClearsEvenWithPreviousResult) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.pushThreshold(11 * kSec, 1.0);
  fx.install(recipe("crop", {"cloud", "threshold"}));
  fx.request("owner/crop", 12 * kSec);
  fx.settle();
  ASSERT_EQ(fx.shown("owner/crop/cropped", 12 * kSec).width, 4u);

  test::prune(fx.store, fx.cloud);  // retention dropped the cloud
  fx.request("owner/crop", 12 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kMissingInput);
  EXPECT_EQ(fx.state("owner/crop").detail, "on-demand: missing input 'cloud'");
  EXPECT_FALSE(fx.state("owner/crop").lag.has_value());
  EXPECT_EQ(fx.shown("owner/crop/cropped", 12 * kSec).width, 0u) << "the previous geometry is not shown";
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 1);
}

// D6: a run whose input is lost while it computes, its time requested again meanwhile, does not show its result as
// ready when it ends: after every completion the scheduler serves the current request again.
TEST(NodeSchedulerTest, AnInputLostWhileItsRunComputesIsMissingWhenTheRunEnds) {
  Fixture fx;
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 10 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));  // decoded
  test::prune(fx.store, fx.cloud);
  fx.request("owner/crop", 10 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kMissingInput);
  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop").status, Status::kMissingInput);
  EXPECT_EQ(fx.shown("owner/crop/cropped", 10 * kSec).width, 0u) << "the old geometry is not shown";
}

// D6, D30: a result served for the current request drops the tombstones it supersedes, stamped after it up to the
// cursor. The cloud is lost and 20 s clears; a cloud stamped 18 s arrives and 20 s is requested again: it shows.
TEST(NodeSchedulerTest, AResultForTheCurrentRequestDropsTheTombstonesItSupersedes) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 10 * kSec);
  fx.settle();
  test::prune(fx.store, fx.cloud);
  fx.request("owner/crop", 25 * kSec);  // after the cursor below: it stays
  fx.request("owner/crop", 20 * kSec);
  ASSERT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{10 * kSec, 20 * kSec, 25 * kSec}));

  fx.pushCloud(18 * kSec);
  fx.request("owner/crop", 20 * kSec);
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{10 * kSec, 18 * kSec, 25 * kSec}));
  EXPECT_EQ(fx.shown("owner/crop/cropped", 20 * kSec).width, 4u);
}

// D6: a request its remembered failure serves clears what the ring shows there before reporting the error: 12 s
// fails, an earlier time succeeds, and 12 s is revisited.
TEST(NodeSchedulerTest, ARevisitedFailureClearsWhatIsShown) {
  Fixture fx;
  fx.fake.fail_on_number = 2.0;
  fx.pushCloud(10 * kSec);
  fx.pushThreshold(10 * kSec, 1.0);
  fx.pushThreshold(11 * kSec, 2.0);
  fx.install(recipe("crop", {"cloud", "threshold"}));
  fx.request("owner/crop", 12 * kSec);
  fx.settle();
  ASSERT_EQ(fx.state("owner/crop").status, Status::kError);
  fx.request("owner/crop", 10 * kSec + 500 * kMs);
  fx.settle();
  ASSERT_EQ(fx.state("owner/crop").status, Status::kReady);

  fx.request("owner/crop", 12 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kError);
  EXPECT_EQ(fx.shown("owner/crop/cropped", 12 * kSec).width, 0u) << "not the 10 s geometry";
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 2);
}

// The missing-input tombstone sits at the requested time, not at the shown result's stamp (where it would hide
// that result at earlier times too) nor at 0 (where an empty annotation set is zero bytes).
TEST(NodeSchedulerTest, AMissingInputTombstoneIsStampedAtTheRequestedTime) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 10 * kSec);
  fx.settle();
  test::prune(fx.store, fx.cloud);
  fx.request("owner/crop", 13 * kSec);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{10 * kSec, 13 * kSec}));
  EXPECT_EQ(fx.shown("owner/crop/cropped", 12 * kSec).width, 4u) << "the result before it stays";
  EXPECT_EQ(fx.shown("owner/crop/cropped", 13 * kSec).width, 0u);
  fx.request("owner/crop", 14 * kSec);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{10 * kSec, 13 * kSec}))
      << "nothing drawn at 14 s: no second tombstone";
}

// D25, DAG-06: an output marked unavailable is a missing input of the node reading it, with the reason; a sibling
// output, here a valid empty cloud, still feeds its reader.
TEST(NodeSchedulerTest, SiblingOutputStillFlowsWhenOneIsUnavailable) {
  Fixture fx;
  fx.fake.unavailable = {"low"};
  fx.fake.empty_clouds = true;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("split", {"cloud"}, {cloudSpec("low"), cloudSpec("high")}));
  fx.install(recipe("lower", {"owner/split/low"}));
  fx.install(recipe("upper", {"owner/split/high"}));
  fx.request("owner/lower", 10 * kSec);
  fx.request("owner/upper", 10 * kSec);
  fx.settle();

  EXPECT_EQ(fx.state("owner/lower").status, Status::kMissingInput);
  EXPECT_EQ(fx.state("owner/lower").detail, "on-demand: missing input 'owner/split/low': no points");
  EXPECT_EQ(fx.state("owner/upper").status, Status::kReady);
  EXPECT_EQ(fx.fake.finished(), 2) << "the split once, then only the upper reader";
}

// DAG-06: a failed upstream is its reader's missing input, with the failure's text; the reader does not run.
TEST(NodeSchedulerTest, AnUpstreamErrorIsItsReadersMissingInput) {
  Fixture fx;
  fx.fake.fail = "boom";
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.install(recipe("again", {"owner/crop/cropped"}));
  fx.request("owner/again", 10 * kSec);
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop").status, Status::kError);
  EXPECT_EQ(fx.state("owner/crop").detail, "boom");
  EXPECT_EQ(fx.state("owner/again").status, Status::kMissingInput);
  EXPECT_EQ(fx.state("owner/again").detail, "on-demand: missing input 'owner/crop/cropped': boom");
  EXPECT_EQ(fx.fake.finished(), 1);
}

// CUR-09 (D6): a failed run clears the display, its empty objects in the output's last frame, and says why. The
// same inputs fail again without a run; new inputs run again.
TEST(NodeSchedulerTest, AFailedRunClearsTheDisplayAndIsNotRetriedOnTheSameInputs) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.pushThreshold(10 * kSec, 1.0);
  fx.install(recipe("crop", {"cloud", "threshold"}));
  fx.request("owner/crop", 10 * kSec);
  fx.settle();

  fx.fake.fail = "boom";
  fx.pushThreshold(11 * kSec, 2.0);
  fx.request("owner/crop", 12 * kSec);
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop").status, Status::kError);
  EXPECT_EQ(fx.state("owner/crop").detail, "boom");
  const sdk::PointCloud cleared = fx.shown("owner/crop/cropped", 12 * kSec);
  EXPECT_EQ(cleared.width, 0u);
  EXPECT_EQ(cleared.frame_id, frameOf(10 * kSec)) << "in the frame the output was last published in";

  fx.request("owner/crop", 13 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kError) << "settled without a run";
  EXPECT_EQ(fx.budget.reserved(), 0u);
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 2);

  fx.pushThreshold(14 * kSec, 3.0);
  fx.request("owner/crop", 15 * kSec);
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 3) << "new inputs may succeed";
}

// D6, D30: a failure clears the display only when it serves the current request. The clock-reading run for 10.2 s
// fails once the cursor is at 10.3 s: the result shown there (10.1 s's, stamped 10 s) stays while 10.3 s computes.
// The failure is still remembered: a revisit fails and clears (ARevisitedFailureClearsWhatIsShown).
TEST(NodeSchedulerTest, ALateFailureForAnOlderRequestKeepsTheDisplay) {
  Fixture fx;
  fx.fake.observe_clock = true;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 10100 * kMs);
  fx.settle();
  ASSERT_EQ(fx.shown("owner/crop/cropped", 10300 * kMs).width, 4u);

  fx.fake.gated = true;
  fx.fake.fail = "boom";
  fx.request("owner/crop", 10200 * kMs);
  ASSERT_TRUE(fx.fake.waitStarted(2));
  fx.request("owner/crop", 10300 * kMs);
  fx.fake.allow(1);
  test::pumpUntil([&] { return fx.fake.started() == 3; });  // the 10.2 s failure applied, the 10.3 s run submitted
  const sdk::PointCloud shown = fx.shown("owner/crop/cropped", 10300 * kMs);
  EXPECT_EQ(shown.width, 4u) << "not cleared by the 10.2 s failure";
  EXPECT_EQ(shown.frame_id, frameOf(10100 * kMs));
  EXPECT_EQ(fx.state("owner/crop").status, Status::kPending);

  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop").status, Status::kError);
  EXPECT_EQ(fx.shown("owner/crop/cropped", 10300 * kMs).width, 0u) << "the current request's failure clears";
}

// A failed fetch or decode of an input is not the script's failure: the request reports it once, without retrying,
// and the next request on the same inputs runs again, since the source may read them now.
TEST(NodeSchedulerTest, AFailedDecodeRunsAgainAtTheNextRequest) {
  Fixture fx;
  const auto fetches = std::make_shared<std::atomic<int>>(0);
  const sdk::PayloadView bytes = sdk::makePayloadView(canonicalCloud(4));
  const auto fetch = [fetches, bytes]() -> std::optional<sdk::PayloadView> {
    return fetches->fetch_add(1) == 0 ? std::nullopt : std::optional<sdk::PayloadView>(bytes);
  };
  ASSERT_TRUE(fx.store.pushLazy(fx.cloud, 10 * kSec, fetch).has_value());
  fx.install(recipe("crop"));
  fx.request("owner/crop", 10 * kSec);
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop").status, Status::kError);
  EXPECT_EQ(fetches->load(), 1);
  EXPECT_EQ(fx.fake.started(), 0);

  fx.request("owner/crop", 10 * kSec);
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 1) << "run again";
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), std::vector<Timestamp>{10 * kSec});
}

// A run whose result serves the current request settles it, even when no history keeps that result to look up
// again (a node with only number outputs, and no compute cache): it never runs again for the same request.
TEST(NodeSchedulerTest, AResultThatServesTheCurrentRequestSettlesItWithoutALookup) {
  Fixture fx(8 * InFlightBudget::kRunBytes, false, ResultHistory::Limits{.cache_bytes = 0});
  fx.pushCloud(10 * kSec);
  fx.install(recipe("count", {"cloud"}, {numberSpec()}));
  fx.request("owner/count", 10 * kSec);
  test::pumpUntil([&] { return fx.state("owner/count").status == Status::kReady || fx.fake.finished() > 3; });
  EXPECT_EQ(fx.state("owner/count").status, Status::kReady);
  EXPECT_EQ(fx.fake.finished(), 1);
}

// ---- Serving from the history ------------------------------------------------------------------------------

// CUR-10, ASY-06: a revisit is served from the history inline: no run, no publication, the result the run made.
TEST(NodeSchedulerTest, AHitComputesNothingAndCopiesNoBundle) {
  Fixture fx;
  fx.pushCloud(5 * kSec);
  fx.pushCloud(20 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 20 * kSec);
  fx.settle();
  fx.request("owner/crop", 5 * kSec);
  fx.settle();
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  ASSERT_EQ(fx.ring("owner/crop/cropped").size(), 2u);
  const SequentialUID uid = fx.store.latestEntryIdAt(topic, 20 * kSec)->uid;
  const size_t cached = fx.service->history().cacheBytes();
  const size_t published = fx.published.size();

  fx.request("owner/crop", 20 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady) << "inline";
  EXPECT_EQ(fx.budget.reserved(), 0u) << "no run";
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 2);
  EXPECT_EQ(fx.published.size(), published) << "nothing republished";
  EXPECT_EQ(fx.store.latestEntryIdAt(topic, 20 * kSec)->uid, uid);
  EXPECT_EQ(fx.ring("owner/crop/cropped").size(), 2u);
  EXPECT_EQ(fx.service->history().cacheBytes(), cached) << "no copy of the bundle reaches the history";
}

// D30: a late result never replaces the one the ring shows for the current request. A run for 11 s, which read the
// query clock, ends after 12 s was served at the same stamp: it is kept in the cache, the ring keeps the 12 s result,
// and nothing has to run again, even with no compute cache to bring the 12 s result back from.
TEST(NodeSchedulerTest, ALateResultReLooksUpTheCursor) {
  Fixture fx(8 * InFlightBudget::kRunBytes, false, ResultHistory::Limits{.cache_bytes = 0});
  fx.fake.observe_clock = true;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 12 * kSec);
  fx.settle();
  fx.fake.gated = true;
  fx.request("owner/crop", 11 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(2));
  fx.request("owner/crop", 12 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);

  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 2) << "no run to bring the 12 s result back";
  EXPECT_EQ(fx.shown("owner/crop/cropped", 12 * kSec).frame_id, frameOf(12 * kSec));
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.state("owner/crop").requested_t, 12 * kSec);
}

// ASY-08: a run that reads the query clock serves only its own instant: 13 s waits in the pending slot and gets its
// own run, never the 12 s result.
TEST(NodeSchedulerTest, RequestsAtDifferentInstantsOfAClockReadingRunNeverShareAResult) {
  Fixture fx;
  fx.fake.observe_clock = true;
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 12 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  fx.request("owner/crop", 13 * kSec);
  fx.fake.allow(2);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[1].query_time, 13 * kSec);
  EXPECT_EQ(fx.shown("owner/crop/cropped", 13 * kSec).frame_id, frameOf(13 * kSec));

  fx.request("owner/crop", 12 * kSec);
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 3) << "the 13 s result replaced it at their shared stamp (CACHE-04): it runs again";
  EXPECT_EQ(fx.shown("owner/crop/cropped", 12 * kSec).frame_id, frameOf(12 * kSec));
}

// CUR-04: a scalar input's new sample is a new request: it runs again on the same cloud entry.
TEST(NodeSchedulerTest, AScalarInputChangeRunsAgainOnTheSameCloud) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.pushThreshold(10 * kSec, 1.0);
  fx.install(recipe("crop", {"cloud", "threshold"}));
  fx.request("owner/crop", 20 * kSec);
  fx.settle();
  fx.pushThreshold(19 * kSec, 2.0);
  fx.request("owner/crop", 20 * kSec);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].input_points, records[1].input_points) << "the same cloud entry";
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{10 * kSec, 19 * kSec}));
}

// CUR-04 with a string: a recorded state that changes runs the node again on the same cloud, and the run reads the
// state as a string.
TEST(NodeSchedulerTest, AStringStateChangeReEvaluatesWithoutANewCloud) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.pushState(10 * kSec, "idle");
  fx.install(recipe("annotate", {"cloud", "state"}));
  fx.request("owner/annotate", 20 * kSec);
  fx.settle();
  fx.request("owner/annotate", 20 * kSec);
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 1) << "an unchanged state is a hit";
  fx.pushState(19 * kSec, "moving");
  fx.request("owner/annotate", 20 * kSec);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].input_points, records[1].input_points) << "the same cloud entry";
  EXPECT_EQ(records[0].strings, std::vector<std::string>{"idle"});
  EXPECT_EQ(records[1].strings, std::vector<std::string>{"moving"});
  EXPECT_EQ(fx.ring("owner/annotate/cropped"), (std::vector<Timestamp>{10 * kSec, 19 * kSec}));
}

// The run reads the TF snapshot taken when it was submitted, not a later one; a run that read the tree serves only
// that snapshot's revision, so when the TF changed during the run, serving the current request again after it runs
// once more under the new revision.
TEST(NodeSchedulerTest, TheRunReadsTheTfSnapshotTakenAtItsSubmission) {
  Fixture fx;
  pj::tf::TransformBuffer submitted_buffer;
  pj::tf::TransformBuffer later_buffer;
  fx.tf = submitted_buffer.snapshot();
  fx.fake.observe_tf = true;
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 10 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  const auto submitted = fx.tf;
  fx.tf = later_buffer.snapshot();
  fx.fake.allow(2);
  // Bounded: a run that never serves its request again would run forever.
  test::pumpUntil([&] { return fx.fake.finished() >= 2 && fx.state("owner/crop").status == Status::kReady; });
  ASSERT_EQ(fx.state("owner/crop").status, Status::kReady);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u) << "another TF revision is a miss";
  EXPECT_EQ(records[0].tf, submitted);
  EXPECT_EQ(records[1].tf, fx.tf);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  fx.request("owner/crop", 10 * kSec);
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 2) << "the same revision is a hit";
}

// ---- Threads ------------------------------------------------------------------------------------------------

// Invariant 6: the decode (the parser here) and the script run on a pool worker, never on the GUI thread.
TEST(NodeSchedulerTest, DecodeAndScriptDoNotRunOnTheGuiThread) {
  Fixture fx;
  ASSERT_TRUE(fx.store.pushOwned(fx.lidar, 10 * kSec, std::vector<uint8_t>(8 * test::kPointStride, 0)).has_value());
  fx.install(recipe("crop", {"lidar"}));
  fx.request("owner/crop", 10 * kSec);
  fx.settle();
  ASSERT_EQ(fx.fake.finished(), 1);
  EXPECT_EQ(fx.probe.parses.load(), 1);
  EXPECT_NE(fx.probe.thread, std::this_thread::get_id()) << "the decode";
  EXPECT_TRUE(fx.probe.lock_held);
  EXPECT_NE(fx.fake.records()[0].thread, std::this_thread::get_id()) << "the script";
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
}

// ---- Chains ------------------------------------------------------------------------------------------------

// DAG-01, DAG-07, GAP-13: a request for a reader first requests its upstream at the same time and waits for it; a
// missing leaf input reaches the reader; the reader decodes the upstream's output from its ring entry, a view
// with no copy; a revisit is served from both histories.
TEST(NodeSchedulerTest, ChainRunsUpstreamFirstAndReadsItsResultFromTheRing) {
  Fixture fx;
  fx.pushCloud(20 * kSec);
  fx.install(recipe("crop"));
  fx.install(recipe("count", {"owner/crop/cropped"}, {numberSpec()}));
  fx.request("owner/count", 1 * kSec);
  EXPECT_EQ(fx.state("owner/count").status, Status::kMissingInput);
  EXPECT_EQ(
      fx.state("owner/count").detail,
      "on-demand: missing input 'owner/crop/cropped': on-demand: missing input 'cloud'");

  fx.request("owner/count", 20 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kPending) << "the upstream runs first";
  EXPECT_EQ(fx.state("owner/count").status, Status::kPending) << "the reader waits for it";
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_NE(records[0].input_points, nullptr);
  EXPECT_TRUE(fx.inRingEntry("owner/crop/cropped", 20 * kSec, records[1].input_points)) << "a view, not a copy";
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.state("owner/count").status, Status::kReady);

  fx.request("owner/count", 21 * kSec);
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 2);
  EXPECT_EQ(fx.state("owner/count").status, Status::kReady);
}

void readerKeepsUpdatingWhileItsSlowUpstreamLags(size_t budget_bytes);

// D30 for chains: a reader keeps updating while its slow upstream lags behind a moving cursor. A moving cursor never
// lets the upstream's newest request settle, so the reader runs on each result the upstream publishes, and it
// converges on the upstream's last result once the cursor stops.
TEST(NodeSchedulerTest, AReaderKeepsUpdatingWhileItsSlowUpstreamLags) {
  for (const size_t runs : {8, 1}) {
    SCOPED_TRACE(testing::Message() << "room for " << runs << " runs");
    readerKeepsUpdatingWhileItsSlowUpstreamLags(runs * InFlightBudget::kRunBytes);
  }
}

// One budget: with room for one run, the upstream and its reader take turns (§11: waiters first in, first out).
void readerKeepsUpdatingWhileItsSlowUpstreamLags(size_t budget_bytes) {
  Fixture fx(budget_bytes);
  fx.fake.slow_delay = 90ms;
  constexpr int kRequests = 30;
  for (int index = 0; index < kRequests; ++index) {
    fx.pushCloud(index * 33 * kMs);
  }
  RecipeGraph::Recipe crop = recipe("crop");
  crop.params_json = R"({"slow": true})";
  fx.install(crop);
  fx.install(recipe("again", {"owner/crop/cropped"}));
  for (int index = 0; index < kRequests; ++index) {
    fx.request("owner/again", index * 33 * kMs);
    test::pumpFor(33);
  }
  const size_t while_moving = fx.publishedCount("owner/again");
  fx.settle();

  EXPECT_GE(while_moving, 3u) << "the reader kept updating while its upstream lagged";
  const Timestamp last = (kRequests - 1) * 33 * kMs;
  EXPECT_EQ(fx.state("owner/again").status, Status::kReady);
  EXPECT_EQ(fx.state("owner/again").requested_t, last);
  const auto records = fx.fake.records();
  const auto last_upstream = std::find_if(records.rbegin(), records.rend(), [](const auto& r) { return r.slow; });
  const auto last_reader = std::find_if(records.rbegin(), records.rend(), [](const auto& r) { return !r.slow; });
  ASSERT_TRUE(last_upstream != records.rend() && last_reader != records.rend());
  EXPECT_EQ(last_upstream->query_time, last);
  EXPECT_TRUE(fx.inRingEntry("owner/crop/cropped", last, last_reader->input_points))
      << "converged on the upstream's last result";
}

// A publication of the upstream for another time does not release a reader whose time it does not cover: after a
// seek back, the reader waits for the upstream's result there instead of finding its input missing.
TEST(NodeSchedulerTest, AReaderWaitsForItsUpstreamAtABackwardSeek) {
  Fixture fx;
  fx.fake.gated = true;
  fx.pushCloud(5 * kSec);
  fx.pushCloud(20 * kSec);
  fx.install(recipe("crop"));
  fx.install(recipe("again", {"owner/crop/cropped"}));
  fx.request("owner/again", 20 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  fx.request("owner/again", 5 * kSec);
  fx.fake.allow(1);
  test::pumpUntil([&] { return fx.fake.started() == 2; });  // the upstream's 20 s result is published
  EXPECT_EQ(fx.state("owner/again").status, Status::kPending) << "the 20 s result is no input at 5 s";
  fx.fake.allow(2);
  fx.settle();
  EXPECT_EQ(fx.state("owner/again").status, Status::kReady);
  EXPECT_EQ(fx.state("owner/again").requested_t, 5 * kSec);
}

// A request for a reader waits for its upstream's result at the new time, rather than taking the older one the
// upstream still shows there.
TEST(NodeSchedulerTest, AReaderWaitsForItsUpstreamsResultAtANewTime) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.pushCloud(20 * kSec);
  fx.install(recipe("crop"));
  fx.install(recipe("again", {"owner/crop/cropped"}));
  fx.request("owner/again", 10 * kSec);
  fx.settle();
  fx.fake.gated = true;
  fx.request("owner/again", 20 * kSec);
  EXPECT_EQ(fx.state("owner/again").status, Status::kPending) << "not the 10 s result the upstream still shows";
  fx.fake.allow(2);
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 4);
  EXPECT_EQ(fx.state("owner/again").status, Status::kReady);
}

// Every settlement of an upstream reaches its waiting readers, whatever the order of their keys: the source fails,
// and `c`, `b` and `a`, each waiting on the one before, all settle missing their input, with the reason.
TEST(NodeSchedulerTest, ASettlementReachesEveryReaderDownTheChain) {
  Fixture fx;
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("source"));
  fx.install(recipe("c", {"owner/source/cropped"}));
  fx.install(recipe("b", {"owner/c/cropped"}));
  fx.install(recipe("a", {"owner/b/cropped"}));
  fx.request("owner/a", 10 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  for (const char* key : {"owner/a", "owner/b", "owner/c"}) {
    EXPECT_EQ(fx.state(key).status, Status::kPending) << key;
  }
  fx.fake.fail = "boom";
  fx.fake.allow(1);
  fx.settle();
  for (const char* key : {"owner/a", "owner/b", "owner/c"}) {
    EXPECT_EQ(fx.state(key).status, Status::kMissingInput) << key;
  }
  EXPECT_EQ(
      fx.state("owner/a").detail,
      "on-demand: missing input 'owner/b/cropped': on-demand: missing input 'owner/c/cropped': on-demand: missing "
      "input 'owner/source/cropped': boom");
  EXPECT_EQ(fx.fake.finished(), 1);
}

// DAG-05: a removed upstream is its readers' missing input, with the reason, and clears their display; reinstalled,
// it relinks by name and the readers run again at their cursors.
TEST(NodeSchedulerTest, ARemovedUpstreamIsItsReadersMissingInputUntilReinstalled) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.install(recipe("again", {"owner/crop/cropped"}));
  fx.request("owner/again", 10 * kSec);
  fx.settle();
  ASSERT_EQ(fx.shown("owner/again/cropped", 10 * kSec).width, 4u);

  (void)fx.service->remove("owner/crop");
  EXPECT_EQ(fx.state("owner/again").status, Status::kMissingInput);
  EXPECT_EQ(fx.state("owner/again").detail, "on-demand: upstream 'owner/crop' removed");
  EXPECT_EQ(fx.shown("owner/again/cropped", 10 * kSec).width, 0u);

  fx.install(recipe("crop"));
  fx.settle();
  EXPECT_EQ(fx.state("owner/again").status, Status::kReady);
  EXPECT_EQ(fx.fake.finished(), 4);
  EXPECT_EQ(fx.shown("owner/again/cropped", 10 * kSec).width, 4u);
}

// DAG-04: an upstream's new revision supersedes its readers: their results go at once, a reader's run in flight is
// cancelled (its revision did not change, its input's meaning did), and both re-evaluate at their cursors, upstream
// first.
TEST(NodeSchedulerTest, AnUpstreamEditSupersedesItsReaders) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.pushThreshold(5 * kSec, 1.0);
  RecipeGraph::Recipe crop = recipe("crop");
  fx.install(crop);
  fx.install(recipe("again", {"owner/crop/cropped", "threshold"}));
  fx.request("owner/again", 10 * kSec);
  fx.settle();
  ASSERT_EQ(fx.ring("owner/again/cropped").size(), 1u);

  crop.params_json = R"({"edited": 1})";
  fx.install(crop);
  EXPECT_TRUE(fx.ring("owner/crop/cropped").empty());
  EXPECT_TRUE(fx.ring("owner/again/cropped").empty()) << "computed from the old meaning";
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 4);
  EXPECT_EQ(fx.ring("owner/again/cropped"), std::vector<Timestamp>{10 * kSec});
  EXPECT_EQ(fx.state("owner/again").status, Status::kReady);

  fx.fake.gated = true;
  fx.pushThreshold(8 * kSec, 2.0);
  fx.request("owner/again", 10 * kSec);  // the upstream is a hit, the reader runs on the new threshold
  ASSERT_TRUE(fx.fake.waitStarted(5));
  const size_t published = fx.publishedCount("owner/again");
  crop.params_json = R"({"edited": 2})";
  fx.install(crop);
  fx.fake.allow(3);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 7u);
  EXPECT_EQ(std::count_if(records.begin(), records.end(), [](const auto& record) { return record.cancelled; }), 1)
      << "the reader's run on the old upstream";
  EXPECT_EQ(fx.publishedCount("owner/again"), published + 1) << "only its run on the new upstream";
  EXPECT_EQ(fx.state("owner/again").status, Status::kReady);
}

// ---- Life cycle ---------------------------------------------------------------------------------------------

// ASY-02: a node removed mid-flight publishes nothing and leaves no state; reinstalled, it inherits none.
TEST(NodeSchedulerTest, RemovalMidFlightPublishesNothing) {
  Fixture fx;
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.request("owner/crop", 10 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  EXPECT_EQ(fx.service->remove("owner/crop").size(), 1u);
  EXPECT_FALSE(fx.service->state("owner/crop").has_value());
  EXPECT_FALSE(fx.store.findTopic(fx.dataset, "owner/crop/cropped").has_value());

  fx.fake.allow(1);
  fx.settle();
  EXPECT_TRUE(fx.fake.records()[0].cancelled);
  EXPECT_TRUE(fx.published.empty());
  EXPECT_EQ(fx.budget.reserved(), 0u);

  fx.install(recipe("crop"));
  EXPECT_FALSE(fx.service->state("owner/crop").has_value());
  fx.fake.allow(1);
  fx.request("owner/crop", 10 * kSec);
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), std::vector<Timestamp>{10 * kSec});
}

// The pool outlives the service: a run finishing after the service is gone publishes nothing, safely, and every
// reservation, a removed node's run's included, is returned once the pool lets the run go.
TEST(NodeSchedulerTest, PublishAfterTheServiceIsGoneIsSafe) {
  Fixture fx;
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("crop"));
  fx.install(recipe("removed"));
  fx.request("owner/crop", 10 * kSec);
  fx.request("owner/removed", 10 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  (void)fx.service->remove("owner/removed");
  fx.service.reset();
  fx.fake.allow(2);
  fx.settle();
  EXPECT_EQ(fx.budget.reserved(), 0u);
  for (const FakeRun::Record& record : fx.fake.records()) {
    EXPECT_TRUE(record.cancelled);
  }
  EXPECT_TRUE(fx.published.empty());
}

// ASY-05: two nodes requested together both publish.
TEST(NodeSchedulerTest, TwoNodesRequestedTogetherBothPublish) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("first"));
  fx.install(recipe("second"));
  fx.request("owner/first", 10 * kSec);
  fx.request("owner/second", 10 * kSec);
  fx.settle();
  EXPECT_EQ(fx.ring("owner/first/cropped"), std::vector<Timestamp>{10 * kSec});
  EXPECT_EQ(fx.ring("owner/second/cropped"), std::vector<Timestamp>{10 * kSec});
}

// BUD-04, BUD-05 (§11): each run reserves its VM ceiling from the shared in-flight budget, once whatever its outputs;
// a run that does not fit stays the pending request until room comes back. One larger than the whole budget runs
// alone.
TEST(NodeSchedulerTest, InFlightBudgetDefersARunThatDoesNotFit) {
  Fixture fx(InFlightBudget::kRunBytes * 3 / 2);
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  fx.install(recipe("first", {"cloud"}, {cloudSpec("near"), cloudSpec("far")}));
  fx.install(recipe("second"));
  fx.request("owner/first", 10 * kSec);
  fx.request("owner/second", 10 * kSec);
  EXPECT_EQ(fx.budget.reserved(), InFlightBudget::kRunBytes) << "the second does not fit beside the first";
  EXPECT_EQ(fx.state("owner/second").status, Status::kPending);
  fx.fake.allow(2);
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 2);
  EXPECT_EQ(fx.state("owner/second").status, Status::kReady);
  EXPECT_EQ(fx.budget.reserved(), 0u);

  EXPECT_EQ(
      InFlightBudget(fx.pool).capacity(),
      InFlightBudget::kRunBytes * static_cast<size_t>(std::max(1, fx.pool.sharedLaneThreads())))
      << "by default, one run per thread";

  Fixture small(InFlightBudget::kRunBytes / 2);
  small.pushCloud(10 * kSec);
  small.install(recipe("crop"));
  small.request("owner/crop", 10 * kSec);
  small.settle();
  EXPECT_EQ(small.state("owner/crop").status, Status::kReady) << "alone, rather than never";
}

// §11: runs waiting for room start first in, first out. With room for one run, `b` and then `c` wait behind `a`;
// `b` leaves the line (its input is missing) and joins it again behind `c`, so `c` runs before it.
TEST(NodeSchedulerTest, AWaitForRoomIsFirstInFirstOut) {
  Fixture fx(InFlightBudget::kRunBytes);
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  for (const char* id : {"a", "b", "c"}) {
    fx.install(recipe(id));
  }
  fx.request("owner/a", 10 * kSec);
  fx.request("owner/b", 11 * kSec);
  fx.request("owner/c", 12 * kSec);
  fx.request("owner/b", 5 * kSec);  // before the cloud: settled, out of the line
  EXPECT_EQ(fx.state("owner/b").status, Status::kMissingInput);
  fx.request("owner/b", 13 * kSec);
  fx.fake.allow(3);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].query_time, 10 * kSec);
  EXPECT_EQ(records[1].query_time, 12 * kSec) << "c waited longer";
  EXPECT_EQ(records[2].query_time, 13 * kSec);
}

// ---- One in-flight budget for both engines (BUD-11, §11) ---------------------------------------------------------

/// A manual marker rule on the fixture's dataset, on its pool and its budget, reading the one-sample series "level";
/// each staging is counted, with the on-demand runs started by then.
struct MarkerRule {
  explicit MarkerRule(Fixture& fx)
      : engine(fx.store, [this, &fx](DatasetId, const std::string& key, const BatchEngine::Window&) {
          ++stagings;
          node_runs_at_staging = fx.fake.started();
          if (on_staging) {
            const std::function<void()> hook = std::exchange(on_staging, {});
            hook();
          }
          return key == "level" ? std::optional<BatchEngine::ResolvedSeries>({{0.0}, {1.0}}) : std::nullopt;
        }) {
    engine.setPool(&fx.pool);
    engine.setInFlightBudget(&fx.budget);
    engine.setMode(BatchEngine::Mode::kManual);
    BatchEngine::GeneratorRecipe rule;
    rule.id = "plug/rule";
    rule.dataset_id = fx.dataset;
    rule.inputs = {"level"};
    rule.outputs = {"level"};
    rule.script = "createMarker(0.0)\n";
    EXPECT_TRUE(engine.upsertGenerator(rule).has_value());  // to completion, inline: it holds no room
    stagings = 0;
    node_runs_at_staging = -1;
  }

  int stagings = 0;
  int node_runs_at_staging = -1;
  std::function<void()> on_staging;  ///< runs once, inside the next staging
  BatchEngine engine;
};

// With room for one run of either engine, an on-demand request waits while a marker run holds the room and runs once
// it publishes; and the inverse, where the marker run waits, unstaged, until the on-demand run publishes.
TEST(BatchEngineTest, MarkerAndOnDemandRunsShareOneBudget) {
  std::atomic<bool> released{false};
  const test::ScopedComputeProbe probe(test::holdPoolComputesUntil(released));
  Fixture fx(InFlightBudget::kRunBytes);
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  fx.pushCloud(20 * kSec);
  fx.install(recipe("crop"));
  MarkerRule markers(fx);

  markers.engine.refresh("plug/rule");
  EXPECT_EQ(fx.budget.reserved(), BatchEngine::kMarkerRunBytes + 2 * sizeof(double));
  fx.request("owner/crop", 10 * kSec);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kPending);
  EXPECT_EQ(fx.fake.started(), 0) << "the on-demand run waits for the marker run's room";
  released = true;
  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 1);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_FALSE(markers.engine.isRunning("plug/rule"));
  EXPECT_EQ(fx.budget.reserved(), 0u);

  released = false;
  fx.request("owner/crop", 20 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(2));
  markers.engine.refresh("plug/rule");
  EXPECT_TRUE(markers.engine.isRunning("plug/rule")) << "waiting for room";
  EXPECT_EQ(markers.stagings, 1) << "a marker run stages once it has its room";
  EXPECT_EQ(fx.budget.reserved(), InFlightBudget::kRunBytes);
  fx.fake.allow(1);
  released = true;
  fx.settle();
  EXPECT_EQ(markers.stagings, 2);
  EXPECT_FALSE(markers.engine.isRunning("plug/rule"));
  EXPECT_FALSE(markers.engine.isStale("plug/rule"));
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.budget.reserved(), 0u);
}

// A node requested again and again keeps one pending request. When its run ends, the marker run that has waited
// since before takes the room first (one line, first in, first out, for both engines): the node's next run waits
// for it, and then runs once, at the newest time.
TEST(BatchEngineTest, AContinuouslyDemandedNodeDoesNotStarveAnEarlierMarkerWaiter) {
  std::atomic<bool> released{false};
  const test::ScopedComputeProbe probe(test::holdPoolComputesUntil(released));
  Fixture fx(InFlightBudget::kRunBytes);
  fx.fake.gated = true;
  for (int second = 1; second <= 10; ++second) {
    fx.pushCloud(second * kSec);
  }
  fx.install(recipe("crop"));
  MarkerRule markers(fx);
  fx.request("owner/crop", 1 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  markers.engine.refresh("plug/rule");
  EXPECT_EQ(markers.stagings, 0) << "it waits for the room the node's run holds";
  for (int second = 2; second <= 5; ++second) {
    fx.request("owner/crop", second * kSec);
  }

  fx.fake.allow(1);
  test::pumpUntil([&] { return markers.stagings == 1; });
  ASSERT_EQ(markers.stagings, 1) << "the marker run started when the node's run ended";
  EXPECT_EQ(markers.node_runs_at_staging, 1) << "ahead of the node's next run";
  for (int second = 6; second <= 10; ++second) {
    fx.request("owner/crop", second * kSec);
    test::pumpFor(5);
  }
  EXPECT_EQ(fx.fake.started(), 1) << "the node waits behind the marker run's room";

  released = true;
  fx.fake.allow(1);
  fx.settle();
  ASSERT_EQ(fx.fake.finished(), 2) << "then one run";
  EXPECT_EQ(fx.fake.records().back().query_time, 10 * kSec) << "at the newest time";
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_FALSE(markers.engine.isRunning("plug/rule"));
  EXPECT_EQ(fx.budget.reserved(), 0u);
}

// A finishing run serves the line last. A marker run it lets in, whose staging requests the finishing node again at
// a newer time, finds that node settled, and the newer request runs; the older result does not settle it.
TEST(BatchEngineTest, AGrantThatRequestsTheFinishingNodeAgainRunsTheNewerRequest) {
  std::atomic<bool> released{false};
  const test::ScopedComputeProbe probe(test::holdPoolComputesUntil(released));
  Fixture fx(InFlightBudget::kRunBytes);
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  fx.pushCloud(20 * kSec);
  fx.install(recipe("crop"));
  MarkerRule markers(fx);
  fx.request("owner/crop", 10 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  markers.engine.refresh("plug/rule");  // waits for the node's room
  markers.on_staging = [&fx] { fx.request("owner/crop", 20 * kSec); };

  released = true;
  fx.fake.allow(2);
  fx.settle();
  EXPECT_EQ(markers.stagings, 1);
  EXPECT_EQ(fx.fake.finished(), 2) << "the newer request ran";
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.state("owner/crop").requested_t, 20 * kSec);
  EXPECT_EQ(fx.shown("owner/crop/cropped", 20 * kSec).frame_id, frameOf(20 * kSec));
  EXPECT_EQ(fx.budget.reserved(), 0u);
}

// DEM-03: a withdrawn node drops its waiting request (behind its run, or in the budget's line) and is idle; its
// running run still publishes into the history and nothing starts after it; a reader that still has a request
// keeps its upstream; the next request serves the node again.
TEST(NodeSchedulerTest, WithdrawDropsTheWaitingRequestOnly) {
  Fixture fx(InFlightBudget::kRunBytes);
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  fx.pushCloud(20 * kSec);
  fx.install(recipe("crop"));
  fx.install(recipe("other"));
  fx.request("owner/crop", 10 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  fx.request("owner/crop", 20 * kSec);   // waits behind its run
  fx.request("owner/other", 20 * kSec);  // waits in the budget's line
  ASSERT_EQ(fx.state("owner/other").status, Status::kPending);

  fx.service->withdraw("owner/crop");
  fx.service->withdraw("owner/other");
  EXPECT_EQ(fx.state("owner/crop").status, Status::kIdle);
  EXPECT_EQ(fx.state("owner/other").status, Status::kIdle);
  fx.fake.allow(3);
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 1) << "neither waiting request ran";
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{10 * kSec})) << "the running run's result is kept";
  EXPECT_EQ(fx.state("owner/crop").status, Status::kIdle);
  EXPECT_EQ(fx.budget.reserved(), 0u);
  RecipeGraph::Recipe edited = recipe("crop");
  edited.params_json = R"({"zmin": 1})";
  fx.install(edited);
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 1) << "a new revision does not request a withdrawn node";

  fx.request("owner/crop", 20 * kSec);
  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{20 * kSec}));

  fx.install(recipe("count", {"owner/crop/cropped"}, {numberSpec()}));
  fx.request("owner/count", 10 * kSec);
  fx.fake.allow(1);
  fx.settle();
  ASSERT_EQ(fx.state("owner/count").status, Status::kReady);
  fx.service->withdraw("owner/crop");
  EXPECT_NE(fx.state("owner/crop").status, Status::kIdle) << "its reader still has a request";
  fx.service->withdraw("owner/count");
  EXPECT_EQ(fx.state("owner/count").status, Status::kIdle);
  EXPECT_EQ(fx.state("owner/crop").status, Status::kIdle) << "nothing else needs its upstream";

  fx.install(recipe("count", {"cloud"}, {numberSpec()}));  // no longer reads crop
  fx.request("owner/count", 10 * kSec);
  fx.request("owner/crop", 10 * kSec);
  fx.fake.allow(2);
  fx.settle();
  fx.service->withdraw("owner/crop");
  EXPECT_EQ(fx.state("owner/crop").status, Status::kIdle) << "a reader that no longer reads it keeps nothing";
}

// ---- The facade ---------------------------------------------------------------------------------------------

// PUB-03, the SDK contract (create_data_processor_v2): a failed edit leaves the previous recipe, its outputs and its
// results intact. The edit adds an output whose name a foreign topic holds: refused, naming it. The previous
// revision stays installed: installing it again changes nothing, and its result is still a hit.
TEST(OnDemandServiceTest, FailedEditPreservesPreviousRecipeTopicsAndResult) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  const RecipeGraph::Recipe crop = recipe("crop");
  fx.install(crop);
  fx.request("owner/crop", 10 * kSec);
  fx.settle();
  const ObjectTopicId cropped = fx.topic("owner/crop/cropped");
  const NodeState before = fx.state("owner/crop");
  ASSERT_EQ(before.status, Status::kReady);
  ASSERT_TRUE(fx.store.registerTopic({fx.dataset, "owner/crop/extra", ""}).has_value());

  RecipeGraph::Recipe edited = crop;
  edited.outputs.push_back(cloudSpec("extra"));
  const auto failed = fx.service->install(edited);
  ASSERT_FALSE(failed.has_value());
  EXPECT_NE(failed.error().find("'owner/crop/extra'"), std::string::npos) << failed.error();
  EXPECT_EQ(fx.topic("owner/crop/cropped"), cropped);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), std::vector<Timestamp>{10 * kSec});
  EXPECT_EQ(fx.state("owner/crop"), before);

  const auto again = fx.service->install(crop);
  ASSERT_TRUE(again.has_value()) << again.error();
  EXPECT_TRUE(again->empty()) << "the same revision: nothing retired";
  fx.request("owner/crop", 10 * kSec);
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 1) << "the previous revision's result is still a hit";
  EXPECT_EQ(fx.publishedCount("owner/crop"), 1u);
  EXPECT_EQ(fx.state("owner/crop"), before);
}

// The failed edit does not supersede the previous revision: its run in flight still publishes.
TEST(OnDemandServiceTest, FailedInstallDoesNotCancelTheExistingRun) {
  Fixture fx;
  fx.fake.gated = true;
  fx.pushCloud(10 * kSec);
  RecipeGraph::Recipe crop = recipe("crop");
  fx.install(crop);
  fx.request("owner/crop", 10 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  ASSERT_TRUE(fx.store.registerTopic({fx.dataset, "owner/crop/extra", ""}).has_value());
  crop.outputs.push_back(cloudSpec("extra"));
  ASSERT_FALSE(fx.service->install(crop).has_value());
  EXPECT_EQ(fx.state("owner/crop").status, Status::kPending);

  fx.fake.allow(1);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_FALSE(records[0].cancelled);
  EXPECT_EQ(fx.publishedCount("owner/crop"), 1u);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), std::vector<Timestamp>{10 * kSec});
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady);
  EXPECT_EQ(fx.budget.reserved(), 0u);
}

// A failed first install leaves nothing behind: no node to request, and the foreign topic untouched.
TEST(OnDemandServiceTest, FailedFirstInstallLeavesNothingBehind) {
  Fixture fx;
  fx.pushCloud(10 * kSec);
  const auto foreign = fx.store.registerTopic({fx.dataset, "owner/crop/cropped", ""});
  ASSERT_TRUE(foreign.has_value());
  const auto failed = fx.service->install(recipe("crop"));
  ASSERT_FALSE(failed.has_value());
  EXPECT_NE(failed.error().find("'owner/crop/cropped'"), std::string::npos) << failed.error();

  fx.request("owner/crop", 10 * kSec);
  fx.settle();
  EXPECT_FALSE(fx.service->state("owner/crop").has_value());
  EXPECT_EQ(fx.fake.started(), 0);
  EXPECT_EQ(fx.topic("owner/crop/cropped"), *foreign);
  EXPECT_TRUE(fx.ring("owner/crop/cropped").empty());
}

// ---- End to end -------------------------------------------------------------------------------------------

// CUR-02 with the real Luau run: each input resolves at or before the requested time on its own, the result is
// stamped at the newest one, and two requests leave two results in the ring.
TEST(OnDemandServiceTest, ALuauRecipeOverAStoredCloudYieldsTwoResults) {
  Fixture fx(8 * InFlightBudget::kRunBytes, /*real_run=*/true);
  fx.pushCloud(10 * kSec, 4);
  fx.pushThreshold(15 * kSec, 1.0);
  fx.pushCloud(20 * kSec, 6);
  RecipeGraph::Recipe crop = recipe("crop", {"cloud", "threshold"}, {cloudSpec(), numberSpec()});
  crop.script =
      "local inputs = ...\n"
      "return { cropped = inputs.cloud:crop_box{min = {-1, -1, -1}, max = {1, 1, 1}},\n"
      "         count = inputs.cloud:count() * inputs.threshold }\n";
  fx.install(crop);
  fx.request("owner/crop", 17 * kSec);
  fx.settle();
  fx.request("owner/crop", 25 * kSec);
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop").status, Status::kReady) << fx.state("owner/crop").detail;
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{15 * kSec, 20 * kSec}));
  EXPECT_EQ(fx.shown("owner/crop/cropped", 15 * kSec).width, 4u);
  EXPECT_EQ(fx.shown("owner/crop/cropped", 20 * kSec).width, 6u);
}

// ---- Benches (PJ_BENCH=1) ------------------------------------------------------------------------------------

/// A lazily stored entry whose fetch returns a view of shared canonical bytes, as a file source's does.
void pushLazyView(ObjectStore& store, ObjectTopicId topic, Timestamp t, const sdk::PayloadView& bytes) {
  ASSERT_TRUE(store.pushLazy(topic, t, [bytes]() -> std::optional<sdk::PayloadView> { return bytes; }).has_value());
}

// The GUI cost of one request over a 260k-point cloud input, for a Luau and a Python recipe (their lanes differ):
// cold (staged, a miss, submitted; the fake run is instant and off the timer) and warm (a hit). The fake skips nothing
// the GUI thread does: staging, the TF provider, the lookup, the budget and the submission are the production ones.
TEST(NodeSchedulerBench, RequestAtGuiCostFor260kCloud) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr int kRequests = 101;
  for (const bool python : {false, true}) {
    Fixture fx;
    const sdk::PayloadView bytes = sdk::makePayloadView(canonicalCloud(260'000));
    for (int index = 0; index < kRequests; ++index) {
      pushLazyView(fx.store, fx.cloud, index * 100 * kMs, bytes);
    }
    fx.install(python ? test::pythonRecipe("crop", {"cloud"}, {cloudSpec()}) : recipe("crop"));
    std::vector<double> cold;
    for (int index = 0; index < kRequests; ++index) {
      cold.push_back(test::medianMicros(1, [&] { fx.request("owner/crop", index * 100 * kMs); }));
      fx.settle();
    }
    std::nth_element(cold.begin(), cold.begin() + kRequests / 2, cold.end());
    const Timestamp last = (kRequests - 1) * 100 * kMs;
    const double warm = test::medianMicros(kRequests, [&] { fx.request("owner/crop", last); });
    ASSERT_EQ(fx.state("owner/crop").status, Status::kReady);
    std::printf(
        "[bench] requestAt, 260k-point cloud, %s: cold %.2f us (median of %d), warm %.2f us\n",
        python ? "python" : "luau", cold[kRequests / 2], kRequests, warm);
  }
}

// The GUI cost of one request with 600 and 1800 results in the window (10 Hz and 30 Hz over 60 s): a hit at the
// cursor, and a miss (a new epoch, so no result serves it) that is then submitted. The fake run is instant and
// publishes empty clouds, off the timer.
TEST(NodeSchedulerBench, RequestAtWithAFullWindow) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  for (const int entries : {600, 1800}) {
    Fixture fx(8 * InFlightBudget::kRunBytes, false, ResultHistory::Limits{.window = 61 * kSec});
    fx.fake.empty_clouds = true;
    const sdk::PayloadView bytes = sdk::makePayloadView(canonicalCloud(1));
    const Timestamp spacing = 60 * kSec / entries;
    for (int index = 0; index < entries; ++index) {
      pushLazyView(fx.store, fx.cloud, index * spacing, bytes);
    }
    fx.install(recipe("crop"));
    for (int index = 0; index < entries; ++index) {
      fx.request("owner/crop", index * spacing);
      fx.settle();
    }
    const Timestamp middle = (entries / 2) * spacing;
    ASSERT_EQ(fx.ring("owner/crop/cropped").size(), static_cast<size_t>(entries));
    const double warm = test::medianMicros(101, [&] { fx.request("owner/crop", middle); });
    ASSERT_EQ(fx.state("owner/crop").status, Status::kReady);
    std::vector<double> cold;
    for (uint64_t epoch = 1; epoch <= 101; ++epoch) {
      fx.epoch = epoch;
      cold.push_back(test::medianMicros(1, [&] { fx.request("owner/crop", middle); }));
      fx.settle();
    }
    std::nth_element(cold.begin(), cold.begin() + 50, cold.end());
    std::printf(
        "[bench] requestAt with %d results in the window: warm %.2f us, cold %.2f us\n", entries, warm, cold[50]);
  }
}

// The GUI cost of publishing one 4.2 MB point-cloud result into the ring (260k points): serialized at the
// publication, and with the payload a worker serialized (`ResultHistory::serializeOutputs`), as the scheduler does.
TEST(NodeSchedulerBench, GuiPublishCostOfA4MBResult) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr int kRuns = 51;
  ObjectStore store;
  RecipeGraph graph{[](std::string_view input) {
    return std::vector<RecipeGraph::InputCandidate>{{1, sdk::BuiltinObjectType::kPointCloud, std::string(input)}};
  }};
  ResultHistory history(store);
  ASSERT_TRUE(graph.upsert(recipe("crop")).has_value());
  const RecipeGraph::Node& node = *graph.find("owner/crop");
  ASSERT_TRUE(history.install(node).has_value());
  ScriptBundle bundle;
  bundle.outputs.push_back({.name = "cropped", .value = sdk::BuiltinObject(test::makeCloud(260'000))});
  const auto publishAt = [&](Timestamp t, ResultPublishOptions options) {
    history.noteRequest("owner/crop", t);
    const ResultHistory::CacheKey key{.revision = node.revision, .inputs = {static_cast<uint64_t>(t)}};
    ASSERT_TRUE(history.publish(node, t, t, key, bundle, {}, std::move(options)).has_value());
  };
  Timestamp t = 0;
  const double serialized = test::medianMicros(kRuns, [&] { publishAt(t += kSec, {}); });
  std::vector<double> prepared;
  for (int run = 0; run < kRuns; ++run) {
    ResultPublishOptions options{.payloads = ResultHistory::serializeOutputs(node.recipe.outputs, bundle)};
    prepared.push_back(test::medianMicros(1, [&] { publishAt(t += kSec, std::move(options)); }));
  }
  std::nth_element(prepared.begin(), prepared.begin() + kRuns / 2, prepared.end());
  std::printf(
      "[bench] GUI publication of a 4.2 MB cloud: serialized there %.0f us, serialized on a worker %.0f us\n",
      serialized, prepared[kRuns / 2]);
}

/// The cursor-to-drawn latencies (ms, sorted) of a 2-hop chain (crop, then crop again, over a 260k-point cloud) at
/// 10 Hz requests: from the reader's request to its result for that time being published. Requests the chain could
/// not keep up with are taken by the pending slot and not counted.
std::vector<double> chainLatencies(bool real_run) {
  constexpr int kRequests = 50;
  Fixture fx(8 * InFlightBudget::kRunBytes, real_run);
  fx.fake.empty_clouds = true;  // the fake's outputs cost the ring's serialization nothing
  const sdk::PayloadView bytes = sdk::makePayloadView(canonicalCloud(260'000));
  for (int index = 0; index < kRequests; ++index) {
    pushLazyView(fx.store, fx.cloud, index * 100 * kMs, bytes);
  }
  const char* crop_script =
      "local inputs = ...\n"
      "local input = inputs.cloud or inputs['owner/crop/cropped']\n"
      "return { cropped = input:crop_box{min = {-1, -1, -1}, max = {1, 1, 1}} }\n";
  RecipeGraph::Recipe crop = recipe("crop");
  crop.script = crop_script;
  RecipeGraph::Recipe again = recipe("again", {"owner/crop/cropped"});
  again.script = crop_script;
  fx.install(crop);
  fx.install(again);

  QElapsedTimer clock;
  clock.start();
  std::vector<double> latencies;
  for (int index = 0; index < kRequests; ++index) {
    const Timestamp t = index * 100 * kMs;
    const qint64 requested_at = clock.nsecsElapsed();
    fx.request("owner/again", t);
    bool drawn = false;
    while (clock.nsecsElapsed() - requested_at < 100 * kMs) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
      const auto state = fx.service->state("owner/again");
      if (!drawn && state && state->status == Status::kReady && state->requested_t == t) {
        latencies.push_back(static_cast<double>(clock.nsecsElapsed() - requested_at) / 1e6);
        drawn = true;
      }
    }
  }
  fx.settle();
  std::sort(latencies.begin(), latencies.end());
  return latencies;
}

// The 2-hop chain's latency with the real Luau run (two crops keeping all 260k points, each serialized into the ring
// on the GUI thread), and with an instant fake run publishing empty clouds: the scheduling alone, two pool round
// trips and the GUI-thread staging and publication between them.
TEST(NodeSchedulerBench, TwoHopChainLatencyAt10Hz) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  for (const bool real_run : {true, false}) {
    const std::vector<double> latencies = chainLatencies(real_run);
    ASSERT_FALSE(latencies.empty());
    std::printf(
        "[bench] 2-hop chain at 10 Hz, 260k points, %s: %zu of 50 drawn, median %.2f ms, max %.2f ms\n",
        real_run ? "Luau crops" : "instant fake, empty outputs", latencies.size(), latencies[latencies.size() / 2],
        latencies.back());
  }
}

}  // namespace
}  // namespace PJ
