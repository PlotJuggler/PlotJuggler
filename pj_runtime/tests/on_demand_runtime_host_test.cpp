// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The on-demand plugin bridge (docs/derived_objects/ARCHITECTURE.md §18): `pj.data_processors.v1` for kind
// "on_demand" over a real AppSession, through the SDK's own client view. The scheduled and evaluation runs are the fake
// (`test::FakeRun`) unless a case needs the Luau engine; every finished report is read back with
// `sdk::parseEvaluationReport`.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "event_pump_test_utils.h"
#include "on_demand_test_utils.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/platform.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_bench_stats.h"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plugins/sdk/evaluation_report.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DerivedTopics.h"
#include "pj_runtime/GridRunner.h"
#include "pj_runtime/OnDemandRuntimeHost.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "pj_scripting/python_object_script.h"
#ifdef PJ_PYTHON_TEST_PROBE
#include "python_test_probe.h"
#endif

namespace PJ {
namespace {

using sdk::EvaluationState;
using Stop = sdk::EvaluationStop;

constexpr Timestamp kSec = 1'000'000'000;
constexpr const char* kCloudMetadata = R"({"builtin_object_type":"kPointCloud"})";
constexpr const char* kScript =
    "local inputs = ...\nreturn { cropped = inputs.cloud, count = inputs.cloud:count(), label = 'ok' }\n";

constexpr const char* kPythonScript =
    "def evaluate(inputs, params):\n"
    "    cloud = inputs['cloud']\n"
    "    return {'cropped': cloud, 'count': cloud.count(), 'label': 'ok'}\n";

std::vector<uint8_t> canonicalCloud(size_t points) {
  return serializeBuiltinObject(sdk::BuiltinObject(test::makeCloud(points))).value();
}

/// `id` reading `cloud`, declaring a cloud, a number and a string.
sdk::DataProcessorRequest request(std::string id) {
  sdk::DataProcessorRequest made;
  made.id = std::move(id);
  made.kind = "on_demand";
  made.language = "luau";
  made.script = kScript;
  made.inputs = {"cloud"};
  made.outputs = {{"cropped", "kPointCloud"}, {"count", "number"}, {"label", "string"}};
  return made;
}

/// A submit of the installed node `id` at `t`.
sdk::DataProcessorRequest installedAt(std::string id, Timestamp t) {
  sdk::DataProcessorRequest made;
  made.id = std::move(id);
  made.kind = "on_demand";
  made.instant_ns = t;
  return made;
}

/// A scripted submit (`request(id)`) over [start, end].
sdk::DataProcessorRequest scriptedOver(std::string id, Timestamp start, Timestamp end) {
  sdk::DataProcessorRequest made = request(std::move(id));
  made.window = std::make_pair(start, end);
  return made;
}

/// What one finished evaluation polled.
struct Finished {
  EvaluationState state = EvaluationState::kPending;
  std::string json;
  sdk::EvaluationReport report;
};

/// An AppSession with the dataset `log` holding the cloud topic `cloud` (entries at 5, 10 and 20 s) and the bridge of
/// plugin `pluginA` over its on-demand service.
struct Fixture {
  explicit Fixture(bool real_run = false) : scoped_run(real_run ? nullptr : std::make_unique<test::ScopedRun>(fake)) {
    ds = addDataset("log");
    addCloudTopic(ds, "cloud", {5 * kSec, 10 * kSec, 20 * kSec});
  }
  ~Fixture() {
    fake.allow(1 << 20);  // a gated run left waiting must not hold the pool's join
  }

  DatasetId addDataset(const std::string& name) {
    auto domain = manager.dataEngine().createTimeDomain(name + "_domain");
    EXPECT_TRUE(domain.has_value());
    return manager.dataEngine()
        .createDataset(DatasetDescriptor{.source_name = name, .time_domain_id = *domain})
        .value();
  }
  ObjectTopicId addCloudTopic(DatasetId dataset, const std::string& name, const std::vector<Timestamp>& stamps) {
    const ObjectTopicId topic = manager.objectStore().registerTopic({dataset, name, kCloudMetadata}).value();
    for (const Timestamp t : stamps) {
      EXPECT_TRUE(manager.objectStore().pushOwned(topic, t, canonicalCloud(4)).has_value());
    }
    session.catalogModel().rebuildFromDatastore();
    return topic;
  }
  void addScalar(DatasetId dataset, const std::string& name, const std::vector<Timestamp>& stamps) {
    DataWriter writer = manager.dataEngine().createWriter();
    auto handle = writer.registerScalarSeries(dataset, name, NumericType::kFloat64);
    ASSERT_TRUE(handle.has_value());
    for (const Timestamp t : stamps) {
      writer.appendScalar(*handle, t, 1.0);
    }
    ASSERT_FALSE(manager.commitChunks(writer.flushAll()).empty());
    session.catalogModel().rebuildFromDatastore();
  }
  /// Runs what is posted, then publishes everything submitted.
  void settle() {
    QCoreApplication::processEvents();
    manager.enginePool().waitIdle();
    QCoreApplication::processEvents();
  }
  void display(const std::string& topic) {
    session.topicDemandTracker().addReference(ds, QString::fromStdString(topic));
  }
  void seek(Timestamp t) {
    const DisplayOffset offset = manager.displayOffset(ds);
    session.playbackEngine().setRangeAndCurrentTime(
        DisplayRange{rawToDisplaySeconds(0, offset), rawToDisplaySeconds(100 * kSec, offset)},
        rawToDisplaySeconds(t, offset));
    settle();
  }
  [[nodiscard]] const RecipeGraph::Node* node(const std::string& key) const {
    return manager.onDemandService().graph().find(key);
  }
  [[nodiscard]] std::optional<ObjectTopicId> topic(const std::string& name, DatasetId dataset = 0) const {
    return manager.objectStore().findTopic(dataset != 0 ? dataset : ds, name);
  }
  [[nodiscard]] std::vector<Timestamp> ring(const std::string& name) const {
    const auto id = topic(name);
    return id ? test::timestampsOf(manager.objectStore(), *id) : std::vector<Timestamp>{};
  }
  [[nodiscard]] nlohmann::json config(std::string_view id) const {
    const auto json = view.recipeOf(id);
    EXPECT_TRUE(json.has_value()) << json.error();
    return json ? nlohmann::json::parse(*json) : nlohmann::json{};
  }
  /// Pumps the event loop (a job's room comes from a drain) until `runs` runs have started; false after 5 s.
  [[nodiscard]] bool waitStarted(int runs) {
    test::pumpUntil([&] { return fake.started() >= runs; }, 5000);
    return fake.started() >= runs;
  }
  /// Pumps until `handle` is no longer pending; its report must parse.
  Finished await(uint64_t handle) {
    Finished done;
    test::pumpUntil(
        [&] {
          const auto polled = view.pollEvaluation(handle);
          EXPECT_TRUE(polled.has_value()) << polled.error();
          if (!polled) {
            return true;
          }
          done.state = polled->state;
          done.json = polled->json;
          return polled->state != EvaluationState::kPending;
        },
        5000);
    EXPECT_NE(done.state, EvaluationState::kPending) << "the evaluation did not finish";
    if (done.state != EvaluationState::kPending) {
      auto parsed = sdk::parseEvaluationReport(done.json);
      EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error()) << "\n" << done.json;
      if (parsed) {
        done.report = std::move(*parsed);
      }
    }
    return done;
  }
  Finished evaluate(const sdk::DataProcessorRequest& made, const sdk::EvaluationBudget& budget = {}) {
    const auto handle = view.submitEvaluation(made, budget);
    EXPECT_TRUE(handle.has_value()) << handle.error();
    return handle ? await(*handle) : Finished{};
  }

  test::FakeRun fake;  ///< outlives the session, whose pool joins the runs
  std::unique_ptr<test::ScopedRun> scoped_run;
  QTemporaryDir dir;
  AppSession session{dir.path()};
  SessionManager& manager = session.sessionManager();
  DatasetId ds = 0;
  int topics_changed = 0;
  OnDemandRuntimeHost bridge{manager.onDemandService(), manager.objectStore(), "pluginA", [this] { ++topics_changed; }};
  sdk::DataProcessorsHostView view{bridge.raw()};
};

// ---- Create, list, config, remove, validate ------------------------------------------------------------------------

// ABI-01 (changed): the v1 create refuses kind on_demand, naming the typed create.
TEST(OnDemandRuntimeHostTest, V1CreateRejectsOnDemandNamingCreateV2) {
  Fixture fx;
  const std::vector<std::string_view> inputs{"cloud"};
  const std::vector<std::string_view> outputs{"count:number"};
  const auto created = fx.view.create("crop", "on_demand", "luau", inputs, outputs, kScript, "{}", 0);
  ASSERT_FALSE(created.has_value());
  EXPECT_NE(created.error().find("create_data_processor_v2"), std::string::npos) << created.error();
  EXPECT_EQ(fx.node("pluginA/crop"), nullptr);
}

// ABI-01/06: out_topics are 1:1 with the outputs, an object output's derived topic and a scalar's bare name; the key is
// namespaced per plugin.
TEST(OnDemandRuntimeHostTest, CreateV2ReturnsTypedOutTopicsAndRegistersDerivedTopics) {
  Fixture fx;
  const auto topics = fx.view.createV2(request("crop"));
  ASSERT_TRUE(topics.has_value()) << topics.error();
  EXPECT_EQ(*topics, (std::vector<std::string>{"pluginA/crop/cropped", "count", "label"}));
  const auto cropped = fx.topic("pluginA/crop/cropped");
  ASSERT_TRUE(cropped.has_value());
  EXPECT_TRUE(isDerivedObjectTopic(fx.manager.objectStore().descriptor(*cropped)));
  EXPECT_EQ(fx.topics_changed, 1);
  const RecipeGraph::Node* node = fx.node("pluginA/crop");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->dataset, fx.ds);
  EXPECT_FALSE(node->recipe.ephemeral);
}

// ABI-02: list strips the namespace; config carries the recipe for re-edit; a second remove is refused.
TEST(OnDemandRuntimeHostTest, ListConfigRemoveRoundTrip) {
  Fixture fx;
  ASSERT_TRUE(fx.view.createV2(request("crop")).has_value());
  EXPECT_EQ(*fx.view.list(), std::vector<std::string>{"crop"});

  const nlohmann::json config = fx.config("crop");
  EXPECT_EQ(config["kind"], "on_demand");
  EXPECT_EQ(config["language"], "luau");
  EXPECT_EQ(config["inputs"], nlohmann::json::array({"cloud"}));
  EXPECT_EQ(config["outputs"], nlohmann::json::parse(R"([{"name":"cropped","type":"kPointCloud"},
          {"name":"count","type":"number"},{"name":"label","type":"string"}])"));
  EXPECT_EQ(config["params"], nlohmann::json::object());
  EXPECT_EQ(config["history_exempt"], false);
  EXPECT_EQ(config["ephemeral"], false);
  EXPECT_FALSE(config.contains("pinned_ns"));

  ASSERT_TRUE(fx.view.remove("crop").has_value());
  EXPECT_TRUE(fx.view.list()->empty());
  EXPECT_FALSE(fx.view.recipeOf("crop").has_value());
  EXPECT_FALSE(fx.topic("pluginA/crop/cropped").has_value());
  const auto again = fx.view.remove("crop");
  ASSERT_FALSE(again.has_value());
  EXPECT_NE(again.error().find("unknown data processor id"), std::string::npos) << again.error();
}

// ABI-03: what the typed create refuses, each by name, leaving nothing installed.
TEST(OnDemandRuntimeHostTest, RejectsWrongKindLanguageOutputsAndFlags) {
  Fixture fx;
  const auto refused = [&fx](sdk::DataProcessorRequest made, std::string_view says) {
    const auto created = fx.view.createV2(made);
    ASSERT_FALSE(created.has_value()) << says;
    EXPECT_NE(created.error().find(says), std::string::npos) << created.error();
  };
  auto made = request("a");
  made.kind = "transform";
  refused(made, "unknown data processor kind");
  made = request("b");
  made.language = "cobol";
  refused(made, "luau");
  made = request("c");
  made.outputs[1].type = "";
  refused(made, "'count'");
  made = request("d");
  made.outputs.clear();
  refused(made, "typed outputs");
  made = request("e");
  made.flags = 1u << 7;
  refused(made, "flags");
  made = request("f");
  made.window = std::make_pair(Timestamp{0}, 10 * kSec);
  refused(made, "submit_evaluation");
  made = request("~g");
  refused(made, "'~'");

  EXPECT_TRUE(fx.view.list()->empty());
  EXPECT_TRUE(fx.manager.onDemandService().graph().keys().empty());
  EXPECT_EQ(fx.topics_changed, 0);
}

// ABI-04: a Python recipe is accepted end to end: created over the ABI, run by the Python runner at the cursor, and
// evaluated with a report; its config names its language. A build without Python refuses it with the reason.
TEST(OnDemandRuntimeHostTest, PythonRecipeIsAccepted) {
  Fixture fx(/*real_run=*/true);
  auto made = request("py");
  made.language = "python";
  made.script = kPythonScript;
  const auto created = fx.view.createV2(made);
  if (!scripting::pythonAvailable()) {
    ASSERT_FALSE(created.has_value());
    EXPECT_NE(created.error().find("python is not available"), std::string::npos) << created.error();
    return;
  }
  ASSERT_TRUE(created.has_value()) << created.error();
  EXPECT_EQ(fx.config("py")["language"], "python");
  fx.display("pluginA/py/cropped");
  fx.seek(10 * kSec);
  EXPECT_EQ(fx.config("py")["state"], "ready") << fx.config("py").dump();
  EXPECT_EQ(fx.ring("pluginA/py/cropped"), (std::vector<Timestamp>{10 * kSec}));

  const Finished done = fx.evaluate(installedAt("py", 20 * kSec));
  ASSERT_EQ(done.state, EvaluationState::kCompleted) << done.json;
  ASSERT_EQ(done.report.bundles.size(), 1u) << done.json;
  EXPECT_EQ(done.report.bundles[0].outputs.at("count").value, 4.0);
}

/// Lengthens the Python validate's wait while alive (`scripting::setPythonValidateWaitForTest`), for a test asserting
/// a checked result: a loaded runner must not turn a 1 ms compile into "unchecked" at the 100 ms production bound.
struct CheckedPythonValidate {
  CheckedPythonValidate() {
    scripting::setPythonValidateWaitForTest(10'000);
  }
  ~CheckedPythonValidate() {
    scripting::setPythonValidateWaitForTest(0);
  }
  CheckedPythonValidate(const CheckedPythonValidate&) = delete;
  CheckedPythonValidate& operator=(const CheckedPythonValidate&) = delete;
};

// The bridge never waits on the GIL without a bound: while a script stuck in a builtin C loop holds the interpreter (a
// held GIL here), a Python create returns at once (the script compiles at its first run, on the Python lane), and so
// does removing a Python recipe (its compiled script's last release); a validate waits at most
// `kPythonValidateWaitMillis`, then reports the script unchecked (success, even for a syntax error, and UNCHECKED
// through the three-way check). The created recipe is pending while the lane waits for the GIL, and ready once the GIL
// is free. An unbounded wait would hang the test; the time bounds are lax.
TEST(OnDemandRuntimeHostTest, APythonCreateNeverWaitsForTheInterpreter) {
#ifndef PJ_PYTHON_TEST_PROBE
  GTEST_SKIP() << "this build has no Python";
#else
  Fixture fx(/*real_run=*/true);
  auto first = request("first");
  first.language = "python";
  first.script = kPythonScript;
  ASSERT_TRUE(fx.view.createV2(first).has_value());
  ASSERT_TRUE(scripting::compileOnDemandScriptPython(kPythonScript).has_value());  // boots the interpreter
  auto held = first;
  held.id = "held";
  {
    const test::PythonGilHold hold;
    const auto begin = std::chrono::steady_clock::now();
    const auto created = fx.view.createV2(held);
    ASSERT_TRUE(created.has_value()) << created.error();
    fx.display("pluginA/held/cropped");
    const auto validate_begin = std::chrono::steady_clock::now();
    const auto validated = fx.view.validateScript("on_demand", "python", "def evaluate(inputs, params:\n", "{}");
    EXPECT_TRUE(validated.has_value()) << "unchecked while the GIL is held: " << validated.error();
    EXPECT_LT(
        std::chrono::steady_clock::now() - validate_begin,
        std::chrono::milliseconds(scripting::kPythonValidateWaitMillis + 100))
        << "the validate waits its bound at most";
    // The three-way check says so instead of passing it: the helper is still busy with the abandoned compile.
    const auto check = fx.view.checkScript("on_demand", "python", "def evaluate(inputs, params:\n", "{}");
    ASSERT_TRUE(check.has_value()) << check.error();
    EXPECT_EQ(check->status, sdk::ScriptCheckStatus::kUnchecked) << check->message;
    EXPECT_FALSE(check->message.empty());
    EXPECT_TRUE(fx.view.remove("first").has_value());
    const DisplayOffset offset = fx.manager.displayOffset(fx.ds);
    fx.session.playbackEngine().setRangeAndCurrentTime(
        DisplayRange{rawToDisplaySeconds(0, offset), rawToDisplaySeconds(100 * kSec, offset)},
        rawToDisplaySeconds(10 * kSec, offset));
    QCoreApplication::processEvents();
    EXPECT_LT(
        std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(scripting::kPythonValidateWaitMillis + 150))
        << "no wait for the GIL but the validate's bound";
    EXPECT_EQ(fx.config("held")["state"], "pending") << fx.config("held").dump();
  }
  fx.settle();
  EXPECT_EQ(fx.config("held")["state"], "ready") << fx.config("held").dump();
  EXPECT_EQ(fx.ring("pluginA/held/cropped"), (std::vector<Timestamp>{10 * kSec}));
#endif
}

// A Python compile error reaches the node's state, where every run error of the recipe is reported (`config`'s state
// and detail), with the message an eager compile gives; an evaluation of the node reports it too, and so does a
// validate of the source while the interpreter is free. The create succeeds: it never compiles Python.
TEST(OnDemandRuntimeHostTest, APythonCompileErrorReachesTheNodesState) {
  if (!scripting::pythonAvailable()) {
    GTEST_SKIP() << "this build has no Python";
  }
  Fixture fx(/*real_run=*/true);
  constexpr const char* kBroken = "def evaluate(inputs, params:\n    return {}\n";
  auto made = request("broken");
  made.language = "python";
  made.script = kBroken;
  const auto created = fx.view.createV2(made);
  ASSERT_TRUE(created.has_value()) << created.error();
  fx.display("pluginA/broken/cropped");
  fx.seek(10 * kSec);
  const auto eager = scripting::compileOnDemandScriptPython(kBroken);
  ASSERT_FALSE(eager.has_value());
  const nlohmann::json config = fx.config("broken");
  EXPECT_EQ(config["state"], "error") << config.dump();
  EXPECT_EQ(config["detail"], eager.error()) << config.dump();

  const Finished done = fx.evaluate(installedAt("broken", 20 * kSec));
  EXPECT_NE(done.json.find("SyntaxError"), std::string::npos) << done.json;

  // The editor's check: with the interpreter free, validate returns the same error.
  const CheckedPythonValidate checked;
  const auto validated = fx.view.validateScript("on_demand", "python", kBroken, "{}");
  ASSERT_FALSE(validated.has_value());
  EXPECT_NE(validated.error().find(eager.error()), std::string::npos) << validated.error();
}

// ABI-05: validate compiles for kind on_demand in the language it names: Luau or Python source each passes its own
// compiler and fails the other's; a language no runner has, and another kind, are refused. The three-way check agrees.
TEST(OnDemandRuntimeHostTest, ValidateScriptRoutesByKindAndLanguage) {
  Fixture fx;
  // Booted, and the bound long, so the Python validate is checked; kept, so no release keeps the helper busy.
  const CheckedPythonValidate checked;
  const auto booted = scripting::compileOnDemandScriptPython(kPythonScript);
  ASSERT_EQ(booted.has_value(), scripting::pythonAvailable());
  EXPECT_TRUE(fx.view.validateScript("on_demand", "luau", "return {}", "{}").has_value());
  const auto syntax = fx.view.validateScript("on_demand", "luau", "return {", "{}");
  EXPECT_FALSE(syntax.has_value());
  EXPECT_FALSE(fx.view.validateScript("on_demand", "luau", kPythonScript, "{}").has_value());
  const auto python = fx.view.validateScript("on_demand", "python", kPythonScript, "{}");
  EXPECT_EQ(python.has_value(), scripting::pythonAvailable()) << (python ? "" : python.error());
  EXPECT_FALSE(fx.view.validateScript("on_demand", "python", "return {}", "{}").has_value());
  const auto ruby = fx.view.validateScript("on_demand", "ruby", "return {}", "{}");
  ASSERT_FALSE(ruby.has_value());
  EXPECT_NE(ruby.error().find("unsupported language 'ruby'"), std::string::npos) << ruby.error();
  const auto markers = fx.view.validateScript("markers", "luau", "return {}", "{}");
  ASSERT_FALSE(markers.has_value());
  EXPECT_NE(markers.error().find("unknown data processor kind"), std::string::npos) << markers.error();

  // The three-way check decides the same scripts the same way; what validate refuses it cannot ask.
  const auto checked_as = [&fx](std::string_view language, std::string_view script) {
    const auto check = fx.view.checkScript("on_demand", language, script, "{}");
    EXPECT_TRUE(check.has_value()) << language << ": " << (check ? "" : check.error());
    return check ? check->status : sdk::ScriptCheckStatus::kUnchecked;
  };
  EXPECT_EQ(checked_as("luau", "return {}"), sdk::ScriptCheckStatus::kValid);
  const auto luau_syntax = fx.view.checkScript("on_demand", "luau", "return {", "{}");
  ASSERT_TRUE(luau_syntax.has_value()) << luau_syntax.error();
  EXPECT_EQ(luau_syntax->status, sdk::ScriptCheckStatus::kInvalid);
  EXPECT_EQ(luau_syntax->message, syntax.error());
  if (scripting::pythonAvailable()) {
    EXPECT_EQ(checked_as("python", kPythonScript), sdk::ScriptCheckStatus::kValid);
    EXPECT_EQ(checked_as("python", "return {}"), sdk::ScriptCheckStatus::kInvalid);
  }
  const auto ruby_check = fx.view.checkScript("on_demand", "ruby", "return {}", "{}");
  ASSERT_FALSE(ruby_check.has_value());
  EXPECT_NE(ruby_check.error().find("unsupported language 'ruby'"), std::string::npos) << ruby_check.error();
  const auto markers_check = fx.view.checkScript("markers", "luau", "return {}", "{}");
  ASSERT_FALSE(markers_check.has_value());
  EXPECT_NE(markers_check.error().find("unknown data processor kind"), std::string::npos) << markers_check.error();
}

// ABI-06: an INSTANT create is a finding, evaluated at its pin once: its one entry sits at the finding timestamp and
// its payload carries the pinned time (the fake names a cloud's frame after its query time).
TEST(OnDemandRuntimeHostTest, InstantCreatePinsAndEvaluatesAtThePin) {
  Fixture fx;
  auto made = request("pin");
  made.instant_ns = 20 * kSec;
  ASSERT_TRUE(fx.view.createV2(made).has_value());
  fx.settle();
  EXPECT_EQ(fx.node("pluginA/pin")->recipe.pinned_t, 20 * kSec);
  EXPECT_EQ(fx.ring("pluginA/pin/cropped"), (std::vector<Timestamp>{kFindingTimestamp}));
  EXPECT_EQ(
      test::decodeAt<sdk::PointCloud>(fx.manager.objectStore(), *fx.topic("pluginA/pin/cropped"), 0).frame_id,
      std::to_string(20 * kSec));
  EXPECT_EQ(fx.fake.finished(), 1);
  EXPECT_EQ(fx.config("pin")["pinned_ns"], 20 * kSec);
}

// A create of an identical recipe (the count-then-fill pair of calls) is the same revision: nothing runs again.
TEST(OnDemandRuntimeHostTest, CountThenFillDoesNotReevaluateAnIdenticalCreate) {
  Fixture fx;
  auto made = request("pin");
  made.instant_ns = 20 * kSec;
  std::vector<PJ_string_view_t> inputs;
  std::vector<PJ_data_processor_output_t> outputs;
  const PJ_data_processor_request_t abi = sdk::detail::toAbiRequest(made, inputs, outputs);
  const PJ_data_processors_host_t raw = fx.bridge.raw();
  uint64_t count = 0;
  PJ_error_t error{};
  ASSERT_TRUE(raw.vtable->create_data_processor_v2(raw.ctx, &abi, nullptr, 0, &count, &error)) << error.message;
  ASSERT_EQ(count, 3u);
  fx.settle();
  const uint64_t revision = fx.node("pluginA/pin")->revision;
  std::vector<PJ_string_view_t> topics(count);
  ASSERT_TRUE(raw.vtable->create_data_processor_v2(raw.ctx, &abi, topics.data(), count, &count, &error));
  EXPECT_EQ(sdk::toStringView(topics[0]), "pluginA/pin/cropped");
  fx.settle();
  EXPECT_EQ(fx.node("pluginA/pin")->revision, revision);
  EXPECT_EQ(fx.fake.finished(), 1);
}

// A refused edit leaves the installed recipe, its revision, topics and config as they were.
TEST(OnDemandRuntimeHostTest, FailedCreatePreservesPreviousRecipeAndOutputs) {
  Fixture fx;
  ASSERT_TRUE(fx.view.createV2(request("crop")).has_value());
  const uint64_t revision = fx.node("pluginA/crop")->revision;
  const ObjectTopicId cropped = *fx.topic("pluginA/crop/cropped");

  auto edited = request("crop");
  edited.inputs = {"missing"};
  const auto refused = fx.view.createV2(edited);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("'missing'"), std::string::npos) << refused.error();
  EXPECT_EQ(fx.node("pluginA/crop")->revision, revision);
  EXPECT_EQ(fx.topic("pluginA/crop/cropped"), cropped);
  EXPECT_EQ(fx.config("crop")["inputs"], nlohmann::json::array({"cloud"}));
}

// GAP-31: a typed dataset id binds every input there; a bare name two datasets carry needs it; a qualified input naming
// another dataset, or an unknown id, is refused.
TEST(OnDemandRuntimeHostTest, DatasetIdBindsThatDatasetAndRejectsAForeignQualifier) {
  Fixture fx;
  const DatasetId other = fx.addDataset("other");
  fx.addCloudTopic(other, "cloud", {7 * kSec});

  const auto ambiguous = fx.view.createV2(request("crop"));
  ASSERT_FALSE(ambiguous.has_value());
  EXPECT_NE(ambiguous.error().find("more than one dataset"), std::string::npos) << ambiguous.error();

  auto bound = request("crop");
  bound.dataset_id = static_cast<uint32_t>(other);
  ASSERT_TRUE(fx.view.createV2(bound).has_value());
  EXPECT_EQ(fx.node("pluginA/crop")->dataset, other);
  EXPECT_EQ(fx.config("crop")["dataset_id"], other) << "config carries the binding a re-edit needs";
  EXPECT_TRUE(fx.topic("pluginA/crop/cropped", other).has_value());

  auto foreign = request("foreign");
  foreign.dataset_id = static_cast<uint32_t>(fx.ds);
  foreign.inputs = {"other:cloud"};
  foreign.script = "local inputs = ...\nreturn { cropped = inputs['other:cloud'], count = 1, label = 'x' }\n";
  const auto refused = fx.view.createV2(foreign);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("'other:cloud'"), std::string::npos) << refused.error();
  EXPECT_NE(refused.error().find("dataset " + std::to_string(fx.ds)), std::string::npos) << refused.error();

  auto unknown = request("unknown");
  unknown.dataset_id = 999;
  EXPECT_FALSE(fx.view.createV2(unknown).has_value());
  EXPECT_EQ(fx.node("pluginA/foreign"), nullptr);
  EXPECT_EQ(fx.node("pluginA/unknown"), nullptr);
}

// OWN-02: a preview is hidden from list, config answers its owner, and it can be evaluated.
TEST(OnDemandRuntimeHostTest, EphemeralRecipeIsHiddenFromListButConfigAnswersTheOwner) {
  Fixture fx;
  auto made = request("probe");
  made.flags = PJ_DATA_PROCESSOR_FLAG_EPHEMERAL;
  const auto topics = fx.view.createV2(made);
  ASSERT_TRUE(topics.has_value()) << topics.error();
  EXPECT_TRUE(fx.view.list()->empty());
  EXPECT_EQ(fx.config("probe")["ephemeral"], true);
  EXPECT_TRUE(fx.bridge.owns("probe"));
  EXPECT_FALSE(fx.view.recipeOf("absent").has_value());
  EXPECT_EQ(fx.bridge.previews(), (std::unordered_set<std::string>{(*topics)[0].substr(0, (*topics)[0].rfind('/'))}));

  const Finished done = fx.evaluate(installedAt("probe", 10 * kSec));
  EXPECT_EQ(done.state, EvaluationState::kCompleted);
  EXPECT_EQ(done.report.bundles.size(), 1u);
}

// SER-01, SER-11: a preview with an object input and a number output computes its series as its plugin's trial (no
// display needed), and its config carries the series' report; the series is the topic `series.topic`, one column per
// number output, while out_topics keep the bare number name the contract gives.
TEST(OnDemandRuntimeHostTest, EphemeralConfigCarriesTheSeriesReport) {
  Fixture fx;
  auto made = request("trial");
  made.flags = PJ_DATA_PROCESSOR_FLAG_EPHEMERAL;
  const auto topics = fx.view.createV2(made);
  ASSERT_TRUE(topics.has_value()) << topics.error();
  EXPECT_EQ((*topics)[1], "count");
  nlohmann::json series;
  test::pumpUntil(
      [&] {
        series = fx.config("trial")["series"];
        return series.is_object() && series["complete"] == true;
      },
      5000);
  ASSERT_TRUE(series.is_object()) << fx.config("trial").dump();
  EXPECT_EQ(series["rows"], 3);
  EXPECT_EQ(series["failed"], 0);
  EXPECT_EQ(series["done"], 3);
  EXPECT_EQ(series["total"], 3);
  EXPECT_EQ(series["stopped"], false);
  EXPECT_EQ(series["first_error"], "");
  const std::string name = series["topic"].get<std::string>();
  EXPECT_EQ(name, fx.node(name)->recipe.key());
  const DataReader reader = fx.manager.createReader();
  const auto state = fx.manager.onDemandService().series().state(name);
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(reader.getMetadata(state->topic)->name, name);
  const auto column = reader.series(state->topic, 0);
  ASSERT_TRUE(column.has_value()) << column.error();
  std::size_t rows = 0;
  column->samples(Range<Timestamp>{0, 100 * kSec}).forEach([&rows](const SeriesSample&) { ++rows; });
  EXPECT_EQ(rows, 3u) << "the number output is the series' one column";

  auto pinned = request("pinned");
  pinned.instant_ns = 10 * kSec;
  ASSERT_TRUE(fx.view.createV2(pinned).has_value());
  EXPECT_FALSE(fx.config("pinned").contains("series")) << "a finding has no series";
}

// A preview never holds a key a layout can name (a restore never clears a preview, and refuses one holding a wanted
// key): it lives under `~preview/<n>/`, and one id names one recipe, so the other lifetime is refused until it is
// removed.
TEST(OnDemandRuntimeHostTest, APreviewNeverTakesTheKeyOfAPersistentRecipe) {
  Fixture fx;
  auto preview = request("crop");
  preview.flags = PJ_DATA_PROCESSOR_FLAG_EPHEMERAL;
  const auto topics = fx.view.createV2(preview);
  ASSERT_TRUE(topics.has_value()) << topics.error();
  ASSERT_EQ(fx.bridge.previews().size(), 1u);
  const std::string key = *fx.bridge.previews().begin();
  EXPECT_TRUE(key.starts_with("pluginA/~preview/") && key.ends_with("/crop")) << key;
  EXPECT_EQ((*topics)[0], key + "/cropped");
  EXPECT_EQ(fx.node("pluginA/crop"), nullptr);
  ASSERT_NE(fx.node(key), nullptr);
  EXPECT_TRUE(fx.node(key)->recipe.ephemeral);

  const auto persistent = fx.view.createV2(request("crop"));
  ASSERT_FALSE(persistent.has_value());
  EXPECT_NE(persistent.error().find("remove"), std::string::npos) << persistent.error();
  ASSERT_TRUE(fx.view.remove("crop").has_value());
  EXPECT_TRUE(fx.bridge.previews().empty());

  ASSERT_TRUE(fx.view.createV2(request("crop")).has_value());
  EXPECT_FALSE(fx.view.createV2(preview).has_value());
  EXPECT_EQ(fx.node(key), nullptr);
}

// Two instances of one plugin (a panel and a headless binding) each own their previews: one id in both is two
// previews, neither instance sees, removes or ends the other's, and one instance's teardown leaves the other's.
TEST(OnDemandRuntimeHostTest, TwoInstancesOfOnePluginNeverShareAPreview) {
  Fixture fx;
  auto other = std::make_unique<OnDemandRuntimeHost>(fx.manager.onDemandService(), fx.manager.objectStore(), "pluginA");
  const sdk::DataProcessorsHostView other_view(other->raw());
  auto preview = request("probe");
  preview.flags = PJ_DATA_PROCESSOR_FLAG_EPHEMERAL;
  const auto mine = fx.view.createV2(preview);
  ASSERT_TRUE(mine.has_value()) << mine.error();
  auto theirs_request = preview;
  theirs_request.params_json = R"({"theirs":true})";
  const auto theirs = other_view.createV2(theirs_request);
  ASSERT_TRUE(theirs.has_value()) << theirs.error();
  EXPECT_NE((*mine)[0], (*theirs)[0]);
  ASSERT_EQ(fx.bridge.previews().size(), 1u);
  ASSERT_EQ(other->previews().size(), 1u);
  EXPECT_NE(*fx.bridge.previews().begin(), *other->previews().begin());
  EXPECT_EQ(fx.config("probe")["params"], nlohmann::json::object());
  EXPECT_EQ(nlohmann::json::parse(*other_view.recipeOf("probe"))["params"]["theirs"], true);

  // Ending the other instance ends its preview only.
  const std::unordered_set<std::string> ended = other->previews();
  other.reset();
  fx.manager.removeEphemerals({}, {}, ended);
  EXPECT_TRUE(fx.bridge.owns("probe"));
  EXPECT_TRUE(fx.topic((*mine)[0]).has_value());
  EXPECT_FALSE(fx.topic((*theirs)[0]).has_value());
  ASSERT_TRUE(fx.view.remove("probe").has_value());
  EXPECT_TRUE(fx.manager.onDemandService().graph().keys().empty());
}

// A persistent recipe another instance of the plugin creates under the id of this instance's preview is not this
// instance's: its remove, config and re-create all act on its own preview, and the persistent recipe survives.
TEST(OnDemandRuntimeHostTest, APreviewOutranksAnotherInstancesPersistentRecipeOfTheSameId) {
  Fixture fx;
  OnDemandRuntimeHost other(fx.manager.onDemandService(), fx.manager.objectStore(), "pluginA");
  const sdk::DataProcessorsHostView other_view(other.raw());
  auto preview = request("x");
  preview.flags = PJ_DATA_PROCESSOR_FLAG_EPHEMERAL;
  ASSERT_TRUE(fx.view.createV2(preview).has_value());
  auto persistent = request("x");
  persistent.params_json = R"({"persistent":true})";
  ASSERT_TRUE(other_view.createV2(persistent).has_value());
  const uint64_t revision = fx.node("pluginA/x")->revision;

  EXPECT_EQ(fx.config("x")["ephemeral"], true);
  EXPECT_EQ(nlohmann::json::parse(*other_view.recipeOf("x"))["ephemeral"], false);

  preview.params_json = R"({"again":true})";
  ASSERT_TRUE(fx.view.createV2(preview).has_value()) << "re-creating the own preview is an upsert";
  EXPECT_EQ(fx.config("x")["params"]["again"], true);
  EXPECT_EQ(fx.node("pluginA/x")->revision, revision);

  ASSERT_TRUE(fx.view.remove("x").has_value());
  EXPECT_TRUE(fx.bridge.previews().empty());
  ASSERT_NE(fx.node("pluginA/x"), nullptr);
  EXPECT_EQ(fx.node("pluginA/x")->revision, revision);
  EXPECT_EQ(nlohmann::json::parse(*other_view.recipeOf("x"))["params"]["persistent"], true);
}

// A '/' in a plugin id or a local id cannot join two plugins' namespaces: plugin `a`'s `b/r` and plugin `a/b`'s `r`
// share a key string, yet neither plugin sees, configures, evaluates, removes or replaces the other's recipe.
TEST(OnDemandRuntimeHostTest, ASlashNeverReachesAnotherPluginsRecipe) {
  Fixture fx;
  for (const bool outer_first : {true, false}) {
    SCOPED_TRACE(outer_first ? "plugin 'a' first" : "plugin 'a/b' first");
    OnDemandRuntimeHost outer(fx.manager.onDemandService(), fx.manager.objectStore(), "a");
    OnDemandRuntimeHost inner(fx.manager.onDemandService(), fx.manager.objectStore(), "a/b");
    const sdk::DataProcessorsHostView owner_view(outer_first ? outer.raw() : inner.raw());
    const sdk::DataProcessorsHostView intruder_view(outer_first ? inner.raw() : outer.raw());
    const OnDemandRuntimeHost& intruder = outer_first ? inner : outer;
    const std::string owner_id = outer_first ? "b/r" : "r";
    const std::string intruder_id = outer_first ? "r" : "b/r";

    ASSERT_TRUE(owner_view.createV2(request(owner_id)).has_value());
    ASSERT_NE(fx.node("a/b/r"), nullptr);
    const uint64_t revision = fx.node("a/b/r")->revision;
    EXPECT_FALSE(intruder.owns(intruder_id));
    EXPECT_FALSE(intruder_view.recipeOf(intruder_id).has_value());
    EXPECT_FALSE(intruder_view.submitEvaluation(installedAt(intruder_id, 10 * kSec)).has_value());
    EXPECT_FALSE(intruder_view.remove(intruder_id).has_value());
    auto replacement = request(intruder_id);
    replacement.params_json = R"({"replaced":true})";
    const auto replaced = intruder_view.createV2(replacement);
    ASSERT_FALSE(replaced.has_value());
    EXPECT_NE(replaced.error().find("another owner"), std::string::npos) << replaced.error();
    EXPECT_TRUE(intruder_view.list()->empty());
    ASSERT_NE(fx.node("a/b/r"), nullptr);
    EXPECT_EQ(fx.node("a/b/r")->revision, revision);
    EXPECT_EQ(fx.node("a/b/r")->recipe.owner_plugin, outer_first ? "a" : "a/b");
    ASSERT_TRUE(owner_view.remove(owner_id).has_value());
  }
}

// Params are bounded at entry: more than 1 MiB, or nested deeper than the scripting engine's 32 levels, is refused at
// create and at submit (a deeper value would overflow the GUI thread's stack when config copies or dumps it); 32
// levels are accepted.
TEST(OnDemandRuntimeHostTest, OversizedOrDeeplyNestedParamsAreRefused) {
  Fixture fx;
  const auto nested = [](std::size_t levels) {
    std::string json;
    for (std::size_t i = 0; i < levels; ++i) {
      json += R"({"a":)";
    }
    json += "1";
    json.append(levels, '}');
    return json;
  };
  const auto refused = [&fx](std::string params, std::string_view says) {
    auto made = request("deep");
    made.params_json = params;
    const auto created = fx.view.createV2(made);
    ASSERT_FALSE(created.has_value()) << says;
    EXPECT_NE(created.error().find(says), std::string::npos) << created.error();
    auto scripted = scriptedOver("deep", 10 * kSec, 10 * kSec);
    scripted.params_json = std::move(params);
    const auto submitted = fx.view.submitEvaluation(scripted);
    ASSERT_FALSE(submitted.has_value()) << says;
    EXPECT_NE(submitted.error().find(says), std::string::npos) << submitted.error();
  };
  refused(nested(33), "nested deeper than 32 levels");
  refused(nested(100'000), "nested deeper than 32 levels");
  refused(R"({"a":")" + std::string(std::size_t{1} << 20, 'x') + R"("})", "params_json is larger than 1048576 bytes");
  EXPECT_EQ(fx.node("pluginA/deep"), nullptr);
  EXPECT_EQ(fx.bridge.liveHandles(), 0u);

  auto made = request("deep");
  made.params_json = nested(32);
  ASSERT_TRUE(fx.view.createV2(made).has_value());
  EXPECT_EQ(fx.config("deep")["params"], nlohmann::json::parse(nested(32)));
}

// ---- Config: state, time, lag, upstream, findings -----------------------------------------------------------------

// ABI-19: config carries the node's state, its newest requested time and its lag (null when none), and its upstreams.
TEST(OnDemandRuntimeHostTest, ConfigExposesStateRequestedTimeAndLag) {
  Fixture fx;
  ASSERT_TRUE(fx.view.createV2(request("crop")).has_value());
  nlohmann::json config = fx.config("crop");
  EXPECT_EQ(config["state"], "idle");
  EXPECT_TRUE(config["requested_ns"].is_null());
  EXPECT_TRUE(config["lag_ns"].is_null());
  EXPECT_EQ(config["detail"], "");
  EXPECT_EQ(config["upstream"], nlohmann::json::array());

  fx.display("pluginA/crop/cropped");
  fx.seek(20 * kSec);
  config = fx.config("crop");
  EXPECT_EQ(config["state"], "ready");
  EXPECT_EQ(config["requested_ns"], 20 * kSec);
  EXPECT_EQ(config["lag_ns"], 0);

  fx.seek(1 * kSec);
  config = fx.config("crop");
  EXPECT_EQ(config["state"], "missing_input");
  EXPECT_NE(config["detail"].get<std::string>().find("'cloud'"), std::string::npos) << config.dump();

  auto reader = request("reader");
  reader.inputs = {"pluginA/crop/cropped"};
  reader.script = "local inputs = ...\nreturn { cropped = inputs['pluginA/crop/cropped'], count = 1, label = 'x' }\n";
  ASSERT_TRUE(fx.view.createV2(reader).has_value());
  EXPECT_EQ(fx.config("reader")["upstream"], nlohmann::json::array({"pluginA/crop"}));
}

// PIN-06, PIN-03's per-owner sum: a finding's config counts its owner's findings and their bytes; another owner's are
// not counted, and a node that is not pinned has no `findings`.
TEST(OnDemandRuntimeHostTest, ConfigOfPinnedNodeReportsFindings) {
  Fixture fx;
  for (const char* id : {"pin1", "pin2"}) {
    auto made = request(id);
    made.instant_ns = 10 * kSec;
    ASSERT_TRUE(fx.view.createV2(made).has_value());
  }
  OnDemandRuntimeHost other(fx.manager.onDemandService(), fx.manager.objectStore(), "pluginB");
  auto foreign = request("pin3");
  foreign.instant_ns = 10 * kSec;
  ASSERT_TRUE(sdk::DataProcessorsHostView(other.raw()).createV2(foreign).has_value());
  ASSERT_TRUE(fx.view.createV2(request("plain")).has_value());
  fx.settle();

  const ResultHistory& history = fx.manager.onDemandService().history();
  const std::size_t bytes = history.ringBytesOf("pluginA/pin1") + history.ringBytesOf("pluginA/pin2");
  EXPECT_GT(history.ringBytesOf("pluginA/pin1"), 0u);
  const nlohmann::json findings = fx.config("pin1")["findings"];
  EXPECT_EQ(findings["pinned"], 2);
  EXPECT_EQ(findings["bytes"], bytes);
  EXPECT_FALSE(fx.config("plain").contains("findings"));
}

// ---- Evaluations ---------------------------------------------------------------------------------------------------

// ABI-07: an instant on an installed node: pending first (empty JSON), then one bundle at the instant.
TEST(OnDemandRuntimeHostTest, SubmitInstantOnInstalledNodeReturnsBundleJson) {
  Fixture fx;
  ASSERT_TRUE(fx.view.createV2(request("crop")).has_value());
  const auto handle = fx.view.submitEvaluation(installedAt("crop", 20 * kSec));
  ASSERT_TRUE(handle.has_value()) << handle.error();
  const auto pending = fx.view.pollEvaluation(*handle);
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(pending->state, EvaluationState::kPending);
  EXPECT_TRUE(pending->json.empty());

  const Finished done = fx.await(*handle);
  ASSERT_EQ(done.state, EvaluationState::kCompleted);
  ASSERT_EQ(done.report.bundles.size(), 1u);
  const sdk::EvaluationBundle& bundle = done.report.bundles[0];
  EXPECT_EQ(bundle.requested_ns, 20 * kSec);
  EXPECT_EQ(bundle.stamp_ns, 20 * kSec);
  EXPECT_EQ(bundle.outputs.at("count").value, static_cast<double>(20 * kSec));
  EXPECT_EQ(bundle.outputs.at("cropped").summary.at("points"), 4);
  EXPECT_TRUE(fx.view.releaseEvaluation(*handle).has_value());
}

// ABI-08: a scripted window publishes nothing, reports its coverage (the leading gap before the first cloud) and stops
// on the evaluation budget.
TEST(OnDemandRuntimeHostTest, SubmitWindowOnEphemeralRecipeReportsCoverageAndStopsOnBudget) {
  Fixture fx;
  const std::size_t topics = fx.manager.objectStore().listTopics().size();
  sdk::EvaluationBudget budget;
  budget.max_evaluations = 2;
  const Finished done = fx.evaluate(scriptedOver("probe", 0, 30 * kSec), budget);
  ASSERT_EQ(done.state, EvaluationState::kCompleted);
  EXPECT_EQ(done.report.candidates, 3u);
  EXPECT_EQ(done.report.evaluated, 2u);
  EXPECT_EQ(done.report.stopped, Stop::kBudgetEvaluations);
  ASSERT_EQ(done.report.gaps.size(), 1u);
  EXPECT_EQ(done.report.gaps[0].from_ns, 0);
  EXPECT_EQ(done.report.gaps[0].to_ns, 5 * kSec);
  EXPECT_EQ(fx.manager.objectStore().listTopics().size(), topics) << "an evaluation publishes nothing";
  EXPECT_TRUE(fx.manager.onDemandService().graph().keys().empty());
}

// A window over a scalar input runs at the series' sample times (the session's change lookup), after the gap before
// its first sample.
TEST(OnDemandRuntimeHostTest, AWindowOverAScalarInputRunsAtItsSampleTimes) {
  Fixture fx;
  fx.addScalar(fx.ds, "threshold", {2 * kSec, 4 * kSec, 6 * kSec});
  auto made = scriptedOver("probe", 0, 5 * kSec);
  made.inputs = {"threshold"};
  made.outputs = {{"count", "number"}};
  const Finished done = fx.evaluate(made);
  ASSERT_EQ(done.state, EvaluationState::kCompleted) << done.json;
  std::vector<int64_t> instants;
  for (const auto& bundle : done.report.bundles) {
    instants.push_back(bundle.requested_ns);
  }
  EXPECT_EQ(instants, (std::vector<int64_t>{2 * kSec, 4 * kSec}));
  ASSERT_EQ(done.report.gaps.size(), 1u);
  EXPECT_EQ(done.report.gaps[0].to_ns, 2 * kSec);
}

// ABI-09 (changed): a scripted submit that declares no output lists what its run inferred (the Luau engine runs).
TEST(OnDemandRuntimeHostTest, SubmitWithNoOutputsReportsInferredOutputs) {
  Fixture fx(/*real_run=*/true);
  auto made = scriptedOver("probe", 10 * kSec, 10 * kSec);
  made.outputs.clear();
  const Finished done = fx.evaluate(made);
  ASSERT_EQ(done.state, EvaluationState::kCompleted) << done.json;
  std::vector<std::pair<std::string, std::string>> listed;
  for (const auto& output : done.report.inferred_outputs) {
    listed.emplace_back(output.name, output.type);
  }
  EXPECT_EQ(
      listed, (std::vector<std::pair<std::string, std::string>>{
                  {"count", "number"}, {"cropped", "kPointCloud"}, {"label", "string"}}));
  EXPECT_EQ(done.report.bundles.at(0).outputs.at("count").value, 4.0);
}

// ABI-10 (changed): a scripted submit is a preview by definition: EPHEMERAL is optional, HISTORY_EXEMPT refused.
TEST(OnDemandRuntimeHostTest, AScriptedSubmitIsEphemeralWithoutTheFlagAndRejectsHistoryExempt) {
  Fixture fx;
  EXPECT_EQ(fx.evaluate(scriptedOver("probe", 10 * kSec, 10 * kSec)).state, EvaluationState::kCompleted);
  auto exempt = scriptedOver("probe", 10 * kSec, 10 * kSec);
  exempt.flags = PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT;
  const auto refused = fx.view.submitEvaluation(exempt);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("HISTORY_EXEMPT"), std::string::npos) << refused.error();
  EXPECT_EQ(fx.bridge.liveHandles(), 1u);
}

// A script that does not compile, or an input that does not bind, is refused at submit: no handle, no FAILED report.
TEST(OnDemandRuntimeHostTest, ANonCompilingSubmitIsRejectedNotFailed) {
  Fixture fx;
  auto broken = scriptedOver("probe", 10 * kSec, 10 * kSec);
  broken.script = "return {";
  EXPECT_FALSE(fx.view.submitEvaluation(broken).has_value());
  auto unbound = scriptedOver("probe", 10 * kSec, 10 * kSec);
  unbound.inputs = {"missing"};
  const auto refused = fx.view.submitEvaluation(unbound);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("'missing'"), std::string::npos) << refused.error();
  const auto unknown = fx.view.submitEvaluation(installedAt("absent", 0));
  ASSERT_FALSE(unknown.has_value());
  EXPECT_NE(unknown.error().find("unknown data processor id"), std::string::npos) << unknown.error();
  EXPECT_EQ(fx.bridge.liveHandles(), 0u);
}

// ABI-11: an unknown handle is refused; a handle is released exactly once.
TEST(OnDemandRuntimeHostTest, PollUnknownHandleIsRejected) {
  Fixture fx;
  const auto polled = fx.view.pollEvaluation(12345);
  ASSERT_FALSE(polled.has_value());
  EXPECT_NE(polled.error().find("unknown evaluation handle"), std::string::npos) << polled.error();
}

TEST(OnDemandRuntimeHostTest, ReleaseTwiceIsRejected) {
  Fixture fx;
  const auto handle = fx.view.submitEvaluation(scriptedOver("probe", 10 * kSec, 10 * kSec));
  ASSERT_TRUE(handle.has_value());
  EXPECT_TRUE(fx.view.releaseEvaluation(*handle).has_value());
  EXPECT_FALSE(fx.view.releaseEvaluation(*handle).has_value());
  EXPECT_FALSE(fx.view.pollEvaluation(*handle).has_value());
  const auto next = fx.view.submitEvaluation(scriptedOver("probe", 10 * kSec, 10 * kSec));
  ASSERT_TRUE(next.has_value());
  EXPECT_GT(*next, *handle) << "handles increase and are never reused";
}

/// A request in a buffer `extra` bytes longer than this SDK's struct, the tail zeroed.
struct PaddedRequest {
  PJ_data_processor_request_t request;
  uint8_t tail[16] = {};
};

// ABI-12: a struct_size larger than the host's is read by its prefix; an unknown flag bit is still refused.
TEST(OnDemandRuntimeHostTest, RequestLargerThanHostIsAcceptedWhenFlagsAreKnown) {
  Fixture fx;
  std::vector<PJ_string_view_t> inputs;
  std::vector<PJ_data_processor_output_t> outputs;
  const sdk::DataProcessorRequest made = request("crop");
  PaddedRequest padded{sdk::detail::toAbiRequest(made, inputs, outputs)};
  padded.request.struct_size = sizeof(PaddedRequest);
  const PJ_data_processors_host_t raw = fx.bridge.raw();
  PJ_error_t error{};
  EXPECT_TRUE(raw.vtable->create_data_processor_v2(raw.ctx, &padded.request, nullptr, 0, nullptr, &error))
      << error.message;
  padded.request.flags = 1u << 9;
  EXPECT_FALSE(raw.vtable->create_data_processor_v2(raw.ctx, &padded.request, nullptr, 0, nullptr, &error));
  EXPECT_NE(std::string(error.message).find("flags"), std::string::npos) << error.message;
}

// ABI-13: a budget shorter than the v1 layout is refused; a longer one is read by its prefix.
TEST(OnDemandRuntimeHostTest, BudgetBelowTheV1LayoutIsRejectedAndALargerOneAccepted) {
  Fixture fx;
  std::vector<PJ_string_view_t> inputs;
  std::vector<PJ_data_processor_output_t> outputs;
  const sdk::DataProcessorRequest made = scriptedOver("probe", 10 * kSec, 10 * kSec);
  const PJ_data_processor_request_t abi = sdk::detail::toAbiRequest(made, inputs, outputs);
  struct PaddedBudget {
    PJ_evaluation_budget_t budget{};
    uint8_t tail[16] = {};
  } padded;
  padded.budget.struct_size = 8;
  const PJ_data_processors_host_t raw = fx.bridge.raw();
  PJ_error_t error{};
  uint64_t handle = 0;
  EXPECT_FALSE(raw.vtable->submit_evaluation(raw.ctx, &abi, &padded.budget, &handle, &error));
  EXPECT_NE(std::string(error.message).find("budget"), std::string::npos) << error.message;
  padded.budget.struct_size = sizeof(PaddedBudget);
  ASSERT_TRUE(raw.vtable->submit_evaluation(raw.ctx, &abi, &padded.budget, &handle, &error)) << error.message;
  EXPECT_EQ(fx.await(handle).state, EvaluationState::kCompleted);
}

// ABI-16: a nonzero reserved field, in an output or in the budget, is refused.
TEST(OnDemandRuntimeHostTest, ReservedBitsAreRejected) {
  Fixture fx;
  std::vector<PJ_string_view_t> inputs;
  std::vector<PJ_data_processor_output_t> outputs;
  const sdk::DataProcessorRequest made = request("crop");
  PJ_data_processor_request_t abi = sdk::detail::toAbiRequest(made, inputs, outputs);
  outputs[0].reserved[1] = 1;
  const PJ_data_processors_host_t raw = fx.bridge.raw();
  PJ_error_t error{};
  EXPECT_FALSE(raw.vtable->create_data_processor_v2(raw.ctx, &abi, nullptr, 0, nullptr, &error));
  EXPECT_NE(std::string(error.message).find("reserved"), std::string::npos) << error.message;
  outputs[0].reserved[1] = 0;
  abi.time_flags = PJ_DATA_PROCESSOR_TIME_FLAG_INSTANT;
  PJ_evaluation_budget_t budget{};
  budget.struct_size = sizeof(budget);
  budget.reserved = 1;
  uint64_t handle = 0;
  EXPECT_FALSE(raw.vtable->submit_evaluation(raw.ctx, &abi, &budget, &handle, &error));
  EXPECT_NE(std::string(error.message).find("reserved"), std::string::npos) << error.message;
  EXPECT_EQ(fx.node("pluginA/crop"), nullptr);
}

// ABI-14 (changed): a run that fails before any bundle is FAILED, in the one report shape.
TEST(OnDemandRuntimeHostTest, FailedReportHasTheOneShape) {
  Fixture fx;
  fx.fake.fail = "boom";
  const Finished done = fx.evaluate(scriptedOver("probe", 10 * kSec, 10 * kSec));
  ASSERT_EQ(done.state, EvaluationState::kFailed);
  EXPECT_EQ(done.report.stopped, Stop::kError);
  EXPECT_NE(done.report.error.find("boom"), std::string::npos) << done.json;
  EXPECT_TRUE(done.report.bundles.empty());
  EXPECT_EQ(done.report.evaluated, 0u);
}

// ABI-15: a failure's reason is kept up to 1024 bytes, past PJ_error_t's bound.
TEST(OnDemandRuntimeHostTest, FailedEvaluationErrorIsTruncatedAt1024) {
  Fixture fx;
  fx.fake.fail = std::string(3000, 'x');
  const Finished done = fx.evaluate(scriptedOver("probe", 10 * kSec, 10 * kSec));
  ASSERT_EQ(done.state, EvaluationState::kFailed);
  EXPECT_GT(done.report.error.size(), 224u);
  EXPECT_LE(done.report.error.size(), 1024u);
}

// ABI-17: work released before it ran never runs, and nothing is published.
TEST(OnDemandRuntimeHostTest, ReleasedPendingWorkNeverRunsOrPublishes) {
  Fixture fx;
  const std::size_t topics = fx.manager.objectStore().listTopics().size();
  const auto handle = fx.view.submitEvaluation(scriptedOver("probe", 0, 30 * kSec));
  ASSERT_TRUE(handle.has_value());
  ASSERT_TRUE(fx.view.releaseEvaluation(*handle).has_value());
  test::pumpFor(50);
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 0);
  EXPECT_EQ(fx.manager.objectStore().listTopics().size(), topics);
  EXPECT_FALSE(fx.view.pollEvaluation(*handle).has_value());
}

// ABI-18: a new revision of the installed node fails its pending evaluation ("replaced").
TEST(OnDemandRuntimeHostTest, InstalledReplacementInvalidatesPendingReport) {
  Fixture fx;
  ASSERT_TRUE(fx.view.createV2(request("crop")).has_value());
  fx.fake.gated = true;
  const auto handle = fx.view.submitEvaluation(installedAt("crop", 20 * kSec));
  ASSERT_TRUE(handle.has_value());
  ASSERT_TRUE(fx.waitStarted(1));
  auto edited = request("crop");
  edited.params_json = R"({"zmin":1})";
  ASSERT_TRUE(fx.view.createV2(edited).has_value());
  fx.fake.allow(1 << 20);
  const Finished done = fx.await(*handle);
  ASSERT_EQ(done.state, EvaluationState::kFailed);
  EXPECT_NE(done.report.error.find("replaced"), std::string::npos) << done.json;
}

// BUD-07: at most 64 live handles and 64 MiB of reserved report bytes per bridge; a release returns its capacity.
TEST(OnDemandRuntimeHostTest, PendingReportsHaveBoundedHandlesAndReleaseRestoresCapacity) {
  Fixture fx;
  fx.fake.gated = true;
  sdk::EvaluationBudget small;  // 64 of them reserve far less than the report bound: the handle bound refuses
  small.max_report_bytes = 4096;
  std::vector<uint64_t> handles;
  for (std::size_t i = 0; i < OnDemandRuntimeHost::kMaxHandles; ++i) {
    const auto handle = fx.view.submitEvaluation(scriptedOver("probe", 10 * kSec, 10 * kSec), small);
    ASSERT_TRUE(handle.has_value()) << i << ": " << handle.error();
    handles.push_back(*handle);
  }
  const auto refused = fx.view.submitEvaluation(scriptedOver("probe", 10 * kSec, 10 * kSec), small);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("release"), std::string::npos) << refused.error();
  ASSERT_TRUE(fx.view.releaseEvaluation(handles.back()).has_value());
  handles.pop_back();
  EXPECT_TRUE(fx.view.submitEvaluation(scriptedOver("probe", 10 * kSec, 10 * kSec), small).has_value());

  for (const uint64_t handle : handles) {
    ASSERT_TRUE(fx.view.releaseEvaluation(handle).has_value());
  }
  // Each handle reserves its clamped report budget: eight of the 8 MiB host cap fill the 64 MiB.
  OnDemandRuntimeHost bytes_bound(fx.manager.onDemandService(), fx.manager.objectStore(), "pluginC");
  const sdk::DataProcessorsHostView view(bytes_bound.raw());
  sdk::EvaluationBudget large;
  large.max_report_bytes = std::numeric_limits<uint64_t>::max();
  std::vector<uint64_t> large_handles;
  for (int i = 0; i < 8; ++i) {
    const auto handle = view.submitEvaluation(scriptedOver("probe", 10 * kSec, 10 * kSec), large);
    ASSERT_TRUE(handle.has_value()) << handle.error();
    large_handles.push_back(*handle);
  }
  EXPECT_EQ(bytes_bound.reservedReportBytes(), OnDemandRuntimeHost::kMaxReportBytes);
  EXPECT_FALSE(view.submitEvaluation(scriptedOver("probe", 10 * kSec, 10 * kSec), large).has_value());
  ASSERT_TRUE(view.releaseEvaluation(large_handles.front()).has_value());
  EXPECT_EQ(
      bytes_bound.reservedReportBytes(), OnDemandRuntimeHost::kMaxReportBytes - GridRunner::kHostCaps.max_report_bytes);
  EXPECT_TRUE(view.submitEvaluation(scriptedOver("probe", 10 * kSec, 10 * kSec), large).has_value());
}

// BUD-08: a report fits its requested bytes, coverage included; one below the smallest report is refused at submit.
TEST(OnDemandRuntimeHostTest, CompleteReportIncludingCoverageFitsRequestedBytes) {
  Fixture fx;
  sdk::EvaluationBudget budget;
  budget.max_report_bytes = 512;
  const Finished done = fx.evaluate(scriptedOver("probe", 0, 30 * kSec), budget);
  ASSERT_EQ(done.state, EvaluationState::kCompleted);
  EXPECT_LE(done.json.size(), 512u);
  EXPECT_EQ(done.report.stopped, Stop::kBudgetReport);
  budget.max_report_bytes = 8;
  EXPECT_FALSE(fx.view.submitEvaluation(scriptedOver("probe", 0, 30 * kSec), budget).has_value());
}

// CON-01: releasing a running evaluation stops it, and the shared pool runs the next one.
TEST(OnDemandRuntimeHostTest, ReleaseAfterDispatchLeavesSharedPoolUsable) {
  Fixture fx;
  fx.fake.slow_delay = std::chrono::milliseconds(5000);
  auto slow = scriptedOver("slow", 10 * kSec, 10 * kSec);
  slow.params_json = R"({"slow":true})";
  const auto handle = fx.view.submitEvaluation(slow);
  ASSERT_TRUE(handle.has_value());
  ASSERT_TRUE(fx.waitStarted(1));
  ASSERT_TRUE(fx.view.releaseEvaluation(*handle).has_value());
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(fx.evaluate(scriptedOver("probe", 10 * kSec, 10 * kSec)).state, EvaluationState::kCompleted);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(4)) << "the released run was cancelled";
  // The probe's run may record before the released one does.
  test::pumpUntil([&] { return fx.fake.finished() >= 2; }, 5000);
  const std::vector<test::FakeRun::Record> records = fx.fake.records();
  const auto released = std::ranges::find_if(records, &test::FakeRun::Record::slow);
  ASSERT_NE(released, records.end());
  EXPECT_TRUE(released->cancelled);
}

// GAP-33: an explicit evaluation moves no cursor, state or displayed result of the installed node.
TEST(OnDemandRuntimeHostTest, ExplicitEvaluationDoesNotMoveCursorOrChangeDisplayedResult) {
  Fixture fx;
  ASSERT_TRUE(fx.view.createV2(request("crop")).has_value());
  fx.display("pluginA/crop/cropped");
  fx.seek(10 * kSec);
  const std::vector<Timestamp> ring = fx.ring("pluginA/crop/cropped");
  ASSERT_EQ(ring, (std::vector<Timestamp>{10 * kSec}));
  const auto before = fx.manager.onDemandService().state("pluginA/crop");
  const double cursor = fx.session.playbackEngine().currentTime().value;

  sdk::DataProcessorRequest window = installedAt("crop", 0);
  window.instant_ns.reset();
  window.window = std::make_pair(Timestamp{0}, 30 * kSec);
  const Finished done = fx.evaluate(window);
  ASSERT_EQ(done.state, EvaluationState::kCompleted);
  EXPECT_EQ(done.report.evaluated, 3u);
  EXPECT_EQ(fx.ring("pluginA/crop/cropped"), ring);
  EXPECT_EQ(fx.manager.onDemandService().state("pluginA/crop"), before);
  EXPECT_EQ(fx.session.playbackEngine().currentTime().value, cursor);
}

// Every report a plugin polls, complete, stopped by a budget or failed, is one `parseEvaluationReport` accepts
// (`Fixture::await` parses each), and state 3 is never emitted.
TEST(OnDemandRuntimeHostTest, EveryReportParsesWithTheSdkParser) {
  Fixture fx;
  ASSERT_TRUE(fx.view.createV2(request("crop")).has_value());
  sdk::EvaluationBudget tight;
  tight.max_evaluations = 1;
  std::vector<Finished> reports;
  reports.push_back(fx.evaluate(installedAt("crop", 20 * kSec)));
  reports.push_back(fx.evaluate(scriptedOver("probe", 0, 30 * kSec), tight));
  reports.push_back(fx.evaluate(scriptedOver("probe", 0, 1 * kSec)));  // inside the leading gap: no instant
  fx.fake.fail = "boom";
  reports.push_back(fx.evaluate(scriptedOver("probe", 0, 30 * kSec)));
  for (const Finished& done : reports) {
    EXPECT_TRUE(done.state == EvaluationState::kCompleted || done.state == EvaluationState::kFailed);
    EXPECT_FALSE(done.json.empty());
  }
  EXPECT_EQ(reports[2].report.evaluated, 0u);
  EXPECT_EQ(reports[3].state, EvaluationState::kFailed);
}

// The bridge's destruction cancels its jobs: a run in flight stops, and no callback reaches the gone bridge.
TEST(OnDemandRuntimeHostTest, DestroyingTheBridgeCancelsItsEvaluations) {
  Fixture fx;
  fx.fake.slow_delay = std::chrono::milliseconds(5000);
  auto bridge = std::make_unique<OnDemandRuntimeHost>(fx.manager.onDemandService(), fx.manager.objectStore(), "p");
  auto slow = scriptedOver("slow", 10 * kSec, 10 * kSec);
  slow.params_json = R"({"slow":true})";
  ASSERT_TRUE(sdk::DataProcessorsHostView(bridge->raw()).submitEvaluation(slow).has_value());
  ASSERT_TRUE(sdk::DataProcessorsHostView(bridge->raw()).submitEvaluation(slow).has_value());
  ASSERT_TRUE(fx.waitStarted(1));
  const auto destroyed = std::chrono::steady_clock::now();
  bridge.reset();
  fx.settle();
  test::pumpFor(20);
  fx.settle();
  EXPECT_LT(std::chrono::steady_clock::now() - destroyed, std::chrono::seconds(4)) << "its runs were cancelled";
  const int started = fx.fake.started();
  ASSERT_EQ(fx.fake.finished(), started);
  for (const auto& record : fx.fake.records()) {
    EXPECT_TRUE(record.cancelled);
  }
  test::pumpFor(20);
  EXPECT_EQ(fx.fake.started(), started) << "a job still waiting for room never runs";
}

// ---- At most one evaluation per bridge in the runner (BUD-07, D30) -------------------------------------------------

/// The query times of the finished fake runs, in the order they finished.
std::vector<Timestamp> queryTimes(const test::FakeRun& fake) {
  std::vector<Timestamp> times;
  for (const auto& record : fake.records()) {
    times.push_back(record.query_time);
  }
  return times;
}

// A submit past the cap gets its handle at once and stays PENDING while the running one runs; the queue then runs one
// at a time, in submission order.
TEST(OnDemandRuntimeHostTest, ASecondEvaluationWaitsPendingWhileTheFirstRuns) {
  Fixture fx;
  fx.fake.gated = true;
  const auto first = fx.view.submitEvaluation(scriptedOver("first", 5 * kSec, 5 * kSec));
  const auto second = fx.view.submitEvaluation(scriptedOver("second", 10 * kSec, 10 * kSec));
  const auto third = fx.view.submitEvaluation(scriptedOver("third", 20 * kSec, 20 * kSec));
  ASSERT_TRUE(first.has_value() && second.has_value() && third.has_value());
  ASSERT_TRUE(fx.waitStarted(1));
  test::pumpFor(50);
  EXPECT_EQ(fx.fake.started(), 1) << "the later evaluations wait outside the runner";
  EXPECT_EQ(fx.view.pollEvaluation(*second)->state, EvaluationState::kPending);
  fx.fake.allow(1);
  EXPECT_EQ(fx.await(*first).state, EvaluationState::kCompleted);
  ASSERT_TRUE(fx.waitStarted(2));
  test::pumpFor(50);
  EXPECT_EQ(fx.fake.started(), 2) << "the queue hands over one evaluation at a time";
  EXPECT_EQ(fx.view.pollEvaluation(*third)->state, EvaluationState::kPending);
  fx.fake.allow(2);
  EXPECT_EQ(fx.await(*second).state, EvaluationState::kCompleted);
  EXPECT_EQ(fx.await(*third).state, EvaluationState::kCompleted);
  EXPECT_EQ(queryTimes(fx.fake), (std::vector<Timestamp>{5 * kSec, 10 * kSec, 20 * kSec}));
}

// D30 under plugin load: a playhead request made while many evaluations are queued is served after at most the one
// that runs.
TEST(OnDemandRuntimeHostTest, APlayheadRequestWaitsForAtMostTheRunningEvaluation) {
  Fixture fx;
  ASSERT_TRUE(fx.view.createV2(request("crop")).has_value());
  fx.display("pluginA/crop/cropped");
  PlaybackEngine& playback = fx.session.playbackEngine();
  playback.setRange(DisplayRange{DisplaySeconds{0.0}, DisplaySeconds{100.0}});
  fx.settle();
  const int before = fx.fake.finished();
  fx.fake.gated = true;
  constexpr int kQueued = 32;
  for (int i = 0; i < kQueued; ++i) {
    ASSERT_TRUE(fx.view.submitEvaluation(scriptedOver("probe", 20 * kSec, 20 * kSec)).has_value());
  }
  ASSERT_TRUE(fx.waitStarted(before + 1));
  // The request is made inside this call: on a lane of several threads its run starts beside the running evaluation,
  // on a lane of one it waits in the budget's line ahead of the queued evaluations.
  playback.setCurrentTime(rawToDisplaySeconds(10 * kSec, DisplayOffset{}));
  if (fx.manager.enginePool().sharedLaneThreads() > 1) {
    ASSERT_TRUE(fx.waitStarted(before + 2));
  }
  fx.fake.allow(1 << 20);
  test::pumpUntil([&] { return fx.fake.finished() >= before + kQueued + 1; }, 5000);
  ASSERT_EQ(fx.fake.finished(), before + kQueued + 1);
  // The start order: one `allow` lets the playhead's run and the running evaluation through together, in either order.
  const std::vector<Timestamp> times = fx.fake.startedQueryTimes();
  const auto playhead = std::ranges::find(times.begin() + before, times.end(), 10 * kSec);
  ASSERT_NE(playhead, times.end());
  EXPECT_LE(playhead - (times.begin() + before), 1) << "the playhead waited for queued evaluations";
}

// Releasing a queued evaluation never runs it; releasing the running one hands the runner the next.
TEST(OnDemandRuntimeHostTest, ReleasingAQueuedEvaluationNeverRunsIt) {
  Fixture fx;
  fx.fake.gated = true;
  const auto running = fx.view.submitEvaluation(scriptedOver("a", 5 * kSec, 5 * kSec));
  const auto queued = fx.view.submitEvaluation(scriptedOver("b", 10 * kSec, 10 * kSec));
  const auto next = fx.view.submitEvaluation(scriptedOver("c", 20 * kSec, 20 * kSec));
  ASSERT_TRUE(running.has_value() && queued.has_value() && next.has_value());
  ASSERT_TRUE(fx.waitStarted(1));
  ASSERT_TRUE(fx.view.releaseEvaluation(*queued).has_value());
  ASSERT_TRUE(fx.view.releaseEvaluation(*running).has_value());
  EXPECT_EQ(fx.bridge.liveHandles(), 1u);
  fx.fake.allow(1 << 20);
  EXPECT_EQ(fx.await(*next).state, EvaluationState::kCompleted);
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 2);
  EXPECT_EQ(std::ranges::count(queryTimes(fx.fake), 10 * kSec), 0) << "the released queued evaluation ran";
  EXPECT_FALSE(fx.view.pollEvaluation(*queued).has_value());
}

// Destroying the bridge with a queue runs nothing more.
TEST(OnDemandRuntimeHostTest, DestroyingTheBridgeWithAQueueRunsNothingMore) {
  Fixture fx;
  fx.fake.gated = true;
  auto bridge = std::make_unique<OnDemandRuntimeHost>(fx.manager.onDemandService(), fx.manager.objectStore(), "p");
  for (const Timestamp t : {5 * kSec, 10 * kSec, 20 * kSec}) {
    ASSERT_TRUE(sdk::DataProcessorsHostView(bridge->raw()).submitEvaluation(scriptedOver("q", t, t)).has_value());
  }
  ASSERT_TRUE(fx.waitStarted(1));
  bridge.reset();
  fx.fake.allow(1 << 20);
  fx.settle();
  test::pumpFor(50);
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 1);
}

// A queued evaluation the runner refuses when it is handed over (its upstream was removed meanwhile) is FAILED with
// the runner's error, and the next queued one still runs.
TEST(OnDemandRuntimeHostTest, AQueuedEvaluationWhoseUpstreamWasRemovedFailsAndTheNextRuns) {
  Fixture fx;
  ASSERT_TRUE(fx.view.createV2(request("crop")).has_value());
  fx.fake.gated = true;
  auto reader = scriptedOver("reader", 20 * kSec, 20 * kSec);
  reader.inputs = {"pluginA/crop/cropped"};
  reader.script = "local inputs = ...\nreturn { cropped = inputs['pluginA/crop/cropped'], count = 1, label = 'x' }\n";
  const auto running = fx.view.submitEvaluation(scriptedOver("a", 10 * kSec, 10 * kSec));
  const auto refused = fx.view.submitEvaluation(reader);
  const auto next = fx.view.submitEvaluation(scriptedOver("c", 5 * kSec, 5 * kSec));
  ASSERT_TRUE(running.has_value() && refused.has_value() && next.has_value()) << refused.error();
  ASSERT_TRUE(fx.waitStarted(1));
  ASSERT_TRUE(fx.view.remove("crop").has_value());
  fx.fake.allow(1 << 20);
  EXPECT_EQ(fx.await(*running).state, EvaluationState::kCompleted);
  const Finished failed = fx.await(*refused);
  ASSERT_EQ(failed.state, EvaluationState::kFailed);
  EXPECT_EQ(failed.report.error, "on-demand: upstream 'pluginA/crop' removed") << failed.json;
  EXPECT_EQ(failed.report.evaluated, 0u);
  EXPECT_EQ(fx.await(*next).state, EvaluationState::kCompleted);
  EXPECT_EQ(queryTimes(fx.fake), (std::vector<Timestamp>{10 * kSec, 5 * kSec}));
}

// ---- Plugin reads of derived topics (GAP-12) -----------------------------------------------------------------------

// A plugin reading a derived topic through the object-read service gets the result at or before `t`.
TEST(ToolboxObjectReadHostTest, PluginReadOfDerivedTopicHonoursT) {
  Fixture fx;
  ASSERT_TRUE(fx.view.createV2(request("crop")).has_value());
  fx.display("pluginA/crop/cropped");
  fx.seek(10 * kSec);
  fx.seek(20 * kSec);
  DatastoreToolboxObjectReadHost host(fx.manager.objectStore());
  const sdk::ToolboxObjectReadHostView reader(host.raw());
  const auto topic = reader.lookupTopicOnDataset(fx.ds, "pluginA/crop/cropped");
  ASSERT_TRUE(topic.has_value());
  Timestamp at = 0;
  ASSERT_TRUE(reader.readLatestAt(*topic, 15 * kSec, &at).has_value());
  EXPECT_EQ(at, 10 * kSec);
  ASSERT_TRUE(reader.readLatestAt(*topic, 25 * kSec, &at).has_value());
  EXPECT_EQ(at, 20 * kSec);
  EXPECT_FALSE(reader.readLatestAt(*topic, 5 * kSec, &at).has_value());
}

// A finding is read at any `t`, with the fixed timestamp in `out_timestamp` and the pinned time in its payload.
TEST(ToolboxObjectReadHostTest, FindingReadReportsFixedTimestamp) {
  Fixture fx;
  auto made = request("pin");
  made.instant_ns = 20 * kSec;
  ASSERT_TRUE(fx.view.createV2(made).has_value());
  fx.settle();
  DatastoreToolboxObjectReadHost host(fx.manager.objectStore());
  const sdk::ToolboxObjectReadHostView reader(host.raw());
  const auto topic = reader.lookupTopicOnDataset(fx.ds, "pluginA/pin/cropped");
  ASSERT_TRUE(topic.has_value());
  for (const Timestamp t : {Timestamp{0}, 5 * kSec, 100 * kSec}) {
    Timestamp at = 0;
    auto bytes = reader.readLatestAt(*topic, t, &at);
    ASSERT_TRUE(bytes.has_value()) << bytes.error();
    EXPECT_EQ(at, kFindingTimestamp);
    const auto view = bytes->view();
    auto object = deserializeBuiltinObject(sdk::BuiltinObjectType::kPointCloud, view.data(), view.size());
    ASSERT_TRUE(object.has_value());
    EXPECT_EQ(object->get<sdk::PointCloud>()->frame_id, std::to_string(20 * kSec));
  }
}

// ---- Measurement ---------------------------------------------------------------------------------------------------

// `PJ_BENCH=1`: what the editor's once-a-second config costs, and submit/poll/release per handle with 64 live.
TEST(OnDemandRuntimeHostBench, ConfigAndHandleTableCost) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  Fixture fx;
  auto made = request("pin");
  made.instant_ns = 20 * kSec;
  ASSERT_TRUE(fx.view.createV2(made).has_value());
  ASSERT_TRUE(fx.view.createV2(request("crop")).has_value());
  fx.settle();
  const double config_us = test::medianMicros(201, [&] { (void)fx.view.recipeOf("pin"); });

  fx.fake.gated = true;
  std::vector<uint64_t> handles;
  for (std::size_t i = 0; i + 1 < OnDemandRuntimeHost::kMaxHandles; ++i) {
    handles.push_back(*fx.view.submitEvaluation(installedAt("crop", 20 * kSec)));
  }
  const double cycle_us = test::medianMicros(201, [&] {
    const auto handle = fx.view.submitEvaluation(installedAt("crop", 20 * kSec));
    (void)fx.view.pollEvaluation(*handle);
    (void)fx.view.releaseEvaluation(*handle);
  });
  const double poll_us = test::medianMicros(201, [&] { (void)fx.view.pollEvaluation(handles.back()); });
  std::printf(
      "[bench] OnDemandRuntimeHost: config of a finding %.2f us; with 63 live handles, submit+poll+release %.2f us, "
      "poll %.3f us\n",
      config_us, cycle_us, poll_us);
}

// `PJ_BENCH=1`: the GUI thread's CPU time per playhead tick of one demanded recipe while 64 explicit evaluations of
// both kinds (instants on the installed node, scripted windows) are live, against the same ticks with none. Fake runs
// that take 2 ms each; the tick is the playhead move (its request, staging and submission), as in 6a-5's bench, alone
// and with the drain after it (publications, evaluation steps and completions with their reports).
TEST(OnDemandRuntimeHostBench, PlayheadTicksWhile64EvaluationsRun) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  Fixture fx;
  std::vector<Timestamp> stamps;
  for (int i = 0; i < 400; ++i) {
    stamps.push_back(30 * kSec + i * kSec / 10);
  }
  fx.addCloudTopic(fx.ds, "dense", stamps);
  auto made = request("crop");
  made.inputs = {"dense"};
  ASSERT_TRUE(fx.view.createV2(made).has_value());
  fx.display("pluginA/crop/cropped");
  fx.fake.delay = std::chrono::milliseconds(2);
  PlaybackEngine& playback = fx.session.playbackEngine();
  playback.setRange(DisplayRange{DisplaySeconds{0.0}, DisplaySeconds{100.0}});
  const auto median = [](std::vector<double> values) {
    std::nth_element(values.begin(), values.begin() + 50, values.end());
    return values[50];
  };
  // The medians of the move alone and of the move with its drain.
  const auto ticks = [&](std::size_t from) {
    const int runs = fx.fake.started();
    std::vector<double> move_us;
    std::vector<double> drained_us;
    for (std::size_t i = from; i < from + 100; ++i) {
      const double before = pj_bench::threadCpuMs();
      playback.setCurrentTime(rawToDisplaySeconds(stamps[i], DisplayOffset{}));
      move_us.push_back((pj_bench::threadCpuMs() - before) * 1000.0);
      fx.settle();  // the evaluations advance between ticks too
      drained_us.push_back((pj_bench::threadCpuMs() - before) * 1000.0);
    }
    EXPECT_GE(fx.fake.started() - runs, 100) << "every tick ran the recipe";
    return std::make_pair(median(move_us), median(drained_us));
  };
  const auto idle_us = ticks(0);
  std::vector<uint64_t> handles;
  for (int i = 0; i < 64; ++i) {
    const auto handle = i % 2 == 0 ? fx.view.submitEvaluation(installedAt("crop", stamps[i]))
                                   : fx.view.submitEvaluation(scriptedOver("probe", 0, 30 * kSec));
    ASSERT_TRUE(handle.has_value()) << handle.error();
    handles.push_back(*handle);
  }
  // The playhead's result latency right after the submits: from the request to its result in the ring.
  const Timestamp probe_t = stamps[140];
  const auto requested = std::chrono::steady_clock::now();
  playback.setCurrentTime(rawToDisplaySeconds(probe_t, DisplayOffset{}));
  test::pumpUntil([&] { return std::ranges::count(fx.ring("pluginA/crop/cropped"), probe_t) > 0; }, 30000);
  const double latency_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - requested).count();
  const auto busy_us = ticks(150);
  std::size_t finished = 0;
  for (const uint64_t handle : handles) {
    finished += fx.view.pollEvaluation(handle)->state != EvaluationState::kPending ? 1 : 0;
  }
  std::printf(
      "[bench] playhead tick GUI CPU, median of 100, move / move+drain: %.2f / %.2f us idle, %.2f / %.2f us with 64 "
      "live evaluations (%zu finished during the ticks); playhead result latency right after the 64 submits %.1f ms\n",
      idle_us.first, idle_us.second, busy_us.first, busy_us.second, finished, latency_ms);
}

// `PJ_BENCH=1`: one evaluation whose report reaches the 8 MiB host cap (2000 instants, a 4 KiB string each): the GUI
// thread's longest event-loop pass while it runs (its completion included), and `GridRunner::toJson` of that report
// alone, the work the bridge leaves to a worker. The fake run skips the script, which runs on a worker anyway.
TEST(OnDemandRuntimeHostBench, ANearMaximumReportCompletes) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  Fixture fx;
  std::vector<Timestamp> stamps;
  for (int i = 0; i < 2000; ++i) {
    stamps.push_back(30 * kSec + i * kSec / 100);
  }
  fx.addCloudTopic(fx.ds, "dense", stamps);
  fx.fake.text = std::string(4096, 'x');
  auto made = scriptedOver("big", stamps.front(), stamps.back());
  made.inputs = {"dense"};
  made.script = "local inputs = ...\nreturn { cropped = inputs.dense, count = 1, label = 'x' }\n";
  sdk::EvaluationBudget budget;
  budget.max_millis = GridRunner::kHostCaps.max_millis;
  budget.max_evaluations = GridRunner::kHostCaps.max_evaluations;
  budget.max_report_bytes = GridRunner::kHostCaps.max_report_bytes;

  const auto handle = fx.view.submitEvaluation(made, budget);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  double longest_us = 0.0;
  sdk::EvaluationPoll polled;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  do {
    const double before = pj_bench::threadCpuMs();
    QCoreApplication::processEvents();
    longest_us = std::max(longest_us, (pj_bench::threadCpuMs() - before) * 1000.0);
    polled = *fx.view.pollEvaluation(*handle);
  } while (polled.state == EvaluationState::kPending && std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(polled.state, EvaluationState::kCompleted);
  const auto report = sdk::parseEvaluationReport(polled.json);
  ASSERT_TRUE(report.has_value()) << report.error();

  std::vector<double> serialize_ms;
  for (int i = 0; i < 9; ++i) {
    const double before = pj_bench::threadCpuMs();
    const std::string json = GridRunner::toJson(*report);
    serialize_ms.push_back(pj_bench::threadCpuMs() - before);
    EXPECT_EQ(json.size(), polled.json.size());
  }
  std::nth_element(serialize_ms.begin(), serialize_ms.begin() + 4, serialize_ms.end());
  std::printf(
      "[bench] a %.2f MiB report (%zu bundles): longest GUI event-loop pass %.2f ms; toJson %.2f ms (median of 9)\n",
      static_cast<double>(polled.json.size()) / (1 << 20), report->bundles.size(), longest_us / 1000.0,
      serialize_ms[4]);
}

}  // namespace
}  // namespace PJ
