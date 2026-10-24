// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// InputStager: staging touches no payload, decoding runs on any thread. A real ObjectStore (lazy
// entries with a counting fetch), a real DataEngine behind the scalar sampler, nodes from a real
// RecipeGraph over a fake resolver, and a real ResultHistory for upstream results. No script runs.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "on_demand_test_utils.h"
#include "pj_base/buffer_anchor.hpp"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/InputStager.h"

namespace PJ {
namespace {

using scripting::NamedValue;
using scripting::OutputType;
using scripting::ScriptBundle;
using test::cloudOver;
using test::kPointStride;
using test::medianMicros;
using test::ParseProbe;
using test::prune;
using test::rawPoints;
using test::RawPointsParser;
using Key = ResultHistory::CacheKey;

constexpr Timestamp kSec = 1'000'000'000;

sdk::BuiltinObject cloudObject(size_t points) {
  return sdk::BuiltinObject(test::makeCloud(points));
}
/// What a topic without a parser holds: the canonical bytes of a cloud.
sdk::PayloadView canonicalCloud(size_t points) {
  return sdk::makePayloadView(serializeBuiltinObject(cloudObject(points)).value());
}

const sdk::PointCloud* cloudIn(const NamedValue& value) {
  const auto* object = std::get_if<sdk::BuiltinObject>(&value.value);
  return object != nullptr ? object->get<sdk::PointCloud>() : nullptr;
}

/// Whether the `size` bytes at `data` lie inside `buffer`.
bool within(const uint8_t* data, size_t size, const Span<const uint8_t>& buffer) {
  const std::less_equal<const uint8_t*> at_or_before;
  return at_or_before(buffer.data(), data) && at_or_before(data + size, buffer.data() + buffer.size());
}

/// A lazy entry's source: counts its fetches and records the thread of the last one.
struct LazySource {
  explicit LazySource(sdk::PayloadView payload) : bytes(std::move(payload)) {}
  sdk::PayloadView bytes;
  std::atomic<int> fetches{0};
  std::thread::id thread;  ///< read only after the fetching thread was joined
};

/// `owner/<id>` reading `inputs`, with one point-cloud output `cropped`; the script is never run.
RecipeGraph::Recipe recipe(std::string id, std::vector<std::string> inputs, std::optional<Timestamp> pinned_t = {}) {
  RecipeGraph::Recipe recipe = test::onDemandRecipe(
      std::move(id), std::move(inputs), {{"cropped", OutputType::kObject, sdk::BuiltinObjectType::kPointCloud}});
  recipe.pinned_t = pinned_t;
  return recipe;
}

ScriptBundle croppedBundle(const sdk::BuiltinObject& cloud) {
  ScriptBundle bundle;
  bundle.outputs.push_back({.name = "cropped", .value = cloud});
  return bundle;
}

/// One dataset: the object topics `cloud` (canonical bytes), `lidar` (raw points behind a parser) and
/// `other`, the scalar series `speed` and `heading`, and the string topic `state`. The graph resolves any object
/// topic of the dataset and any name in `scalars`, which the sampler scans as a catalog does and reads with the
/// session's `InputStager::sampleAt`.
struct Fixture {
  Fixture() : history(store) {
    dataset = engine.createDataset(DatasetDescriptor{.source_name = "log"}).value();
    cloud = store.registerTopic({dataset, "cloud", ""}).value();
    lidar = store.registerTopic({dataset, "lidar", ""}).value();
    other = store.registerTopic({dataset, "other", ""}).value();
    speed = writer.registerScalarSeries(dataset, "speed", NumericType::kFloat64).value();
    heading = writer.registerScalarSeries(dataset, "heading", NumericType::kFloat64).value();
    state = test::registerStringTopic(writer, dataset, "state");
    scalars = {{"speed", speed.topic_id}, {"heading", heading.topic_id}, {"state", state}};
    replaceLidarParser();
  }

  /// Installs `recipe` in the graph, then in the history. The node is the graph's: a later upsert of the
  /// same key updates it in place.
  const RecipeGraph::Node& install(const RecipeGraph::Recipe& recipe) {
    const Status upserted = graph.upsert(recipe);
    EXPECT_TRUE(upserted.has_value()) << upserted.error();
    const RecipeGraph::Node* node = graph.find(recipe.key());
    const auto retired = history.install(*node);
    EXPECT_TRUE(retired.has_value()) << retired.error();
    return *node;
  }
  void pushLazy(ObjectTopicId topic, Timestamp t, LazySource& source) {
    const Status pushed = store.pushLazy(topic, t, [&source]() -> std::optional<sdk::PayloadView> {
      ++source.fetches;
      source.thread = std::this_thread::get_id();
      return source.bytes;
    });
    EXPECT_TRUE(pushed.has_value()) << pushed.error();
  }
  void appendScalar(const ScalarSeriesHandle& series, Timestamp t, double value) {
    writer.appendScalar(series, t, value);
    EXPECT_FALSE(engine.commitChunks(writer.flushAll()).empty());
  }
  void appendState(Timestamp t, std::string_view value) {
    test::appendString(writer, state, t, value);
    EXPECT_FALSE(engine.commitChunks(writer.flushAll()).empty());
  }
  /// Registers `lidar`'s parser anew, as a reload does; returns the previous one.
  std::shared_ptr<RawPointsParser> replaceLidarParser() {
    auto replacement = std::make_shared<RawPointsParser>(probe);
    EXPECT_TRUE(replacement->bindSchema("raw_points", {}).has_value());
    return std::exchange(lidar_parser, std::move(replacement));
  }
  /// Publishes `bundle` as `node`'s result at `stamp`, requested there.
  void publish(
      const RecipeGraph::Node& node, Timestamp stamp, ScriptBundle bundle,
      std::vector<InputStager::Keepalive> keepalives = {}) {
    history.noteRequest(node.recipe.key(), stamp);
    const Key key{.revision = node.revision, .inputs = {static_cast<uint64_t>(stamp)}};
    const Status published = history.publish(node, stamp, stamp, key, std::move(bundle), std::move(keepalives));
    EXPECT_TRUE(published.has_value()) << published.error();
  }
  [[nodiscard]] ObjectTopicId topic(const std::string& name) const {
    return store.findTopic(dataset, name).value_or(ObjectTopicId{});
  }

  /// `stage`, which must succeed.
  [[nodiscard]] InputStager::Staged staged(const RecipeGraph::Node& node, Timestamp t, uint64_t epoch = 0) const {
    auto result = stager.stage(node, t, epoch);
    if (!result) {
      ADD_FAILURE() << result.error();
      return {};
    }
    return std::move(*result);
  }
  /// The error `stage` reports; empty when it succeeds.
  [[nodiscard]] std::string stageError(const RecipeGraph::Node& node, Timestamp t) const {
    const auto result = stager.stage(node, t, 0);
    return result ? std::string{} : result.error();
  }
  /// `decode`, which must succeed.
  [[nodiscard]] InputStager::Decoded decode(const InputStager::Staged& staged) const {
    auto result = InputStager::decode(store, staged);
    if (!result) {
      ADD_FAILURE() << result.error();
      return {};
    }
    return std::move(*result);
  }

  [[nodiscard]] const TopicId* scalar(std::string_view name) const {
    const auto found =
        std::find_if(scalars.begin(), scalars.end(), [&](const auto& named) { return named.first == name; });
    return found == scalars.end() ? nullptr : &found->second;
  }

  DataEngine engine;
  DataWriter writer{engine.createWriter()};
  ObjectStore store;
  DatasetId dataset = 0;
  ObjectTopicId cloud;
  ObjectTopicId lidar;
  ObjectTopicId other;
  ScalarSeriesHandle speed;
  ScalarSeriesHandle heading;
  TopicId state = 0;
  std::vector<std::pair<std::string, TopicId>> scalars;
  ParseProbe probe;
  std::shared_ptr<RawPointsParser> lidar_parser;
  RecipeGraph graph{[this](std::string_view input) {
    if (store.findTopic(dataset, input)) {
      return std::vector<RecipeGraph::InputCandidate>{
          {dataset, sdk::BuiltinObjectType::kPointCloud, std::string(input)}};
    }
    if (scalar(input) != nullptr) {
      return std::vector<RecipeGraph::InputCandidate>{{dataset, sdk::BuiltinObjectType::kNone, std::string(input)}};
    }
    return std::vector<RecipeGraph::InputCandidate>{};
  }};
  ResultHistory history;
  InputStager stager{
      store, history,
      [this](DatasetId in, std::string_view name, Timestamp t) -> std::optional<InputStager::ScalarSample> {
        const TopicId* topic = in == dataset ? scalar(name) : nullptr;
        return topic != nullptr ? InputStager::sampleAt(engine, *topic, 0, t) : std::nullopt;
      },
      [this](ObjectTopicId topic) {
        return topic == lidar && lidar_parser
                   ? SessionManager::ParserBinding{lidar_parser.get(), probe.topic_lock, lidar_parser}
                   : SessionManager::ParserBinding{};
      }};
};

// ---- Where the work happens -------------------------------------------------------------------------

// §9 requirement 5: staging resolves a lazy entry by identity; only decoding fetches it.
TEST(InputStagerTest, StagingFetchesNoPayload) {
  Fixture fx;
  LazySource source(canonicalCloud(4));
  fx.pushLazy(fx.cloud, 10 * kSec, source);
  const auto& node = fx.install(recipe("crop", {"cloud"}));

  const InputStager::Staged staged = fx.staged(node, 12 * kSec);
  EXPECT_EQ(source.fetches.load(), 0);

  const auto decoded = fx.decode(staged);
  EXPECT_EQ(source.fetches.load(), 1);
  ASSERT_EQ(decoded.values.size(), 1u);
  EXPECT_EQ(decoded.values[0].name, "cloud");
  ASSERT_NE(cloudIn(decoded.values[0]), nullptr);
  EXPECT_EQ(cloudIn(decoded.values[0])->width, 4u);
}

// GAP-26: decode on another thread; the lazy fetch and the parse run there, not on the staging thread.
TEST(InputStagerTest, DecodeRunsOnAnotherThread) {
  Fixture fx;
  LazySource source(rawPoints(8));
  fx.pushLazy(fx.lidar, 10 * kSec, source);
  const auto& node = fx.install(recipe("crop", {"lidar"}));
  const InputStager::Staged staged = fx.staged(node, 10 * kSec);

  std::thread::id worker;
  std::optional<Expected<InputStager::Decoded>> decoded;
  std::thread thread([&] {
    worker = std::this_thread::get_id();
    decoded = InputStager::decode(fx.store, staged);
  });
  thread.join();

  ASSERT_TRUE(decoded.has_value() && decoded->has_value()) << (decoded ? decoded->error() : "");
  EXPECT_NE(worker, std::this_thread::get_id());
  EXPECT_EQ(source.thread, worker) << "the lazy fetch ran on the decoding thread";
  EXPECT_EQ(fx.probe.thread, worker) << "so did the parse";
  ASSERT_NE(cloudIn((*decoded)->values[0]), nullptr);
  EXPECT_EQ(cloudIn((*decoded)->values[0])->width, 8u);
}

// The parser keeps stateful scratch: its parse holds the topic's parser lock.
TEST(InputStagerTest, ParserBackedInputDecodesUnderItsTopicLock) {
  Fixture fx;
  LazySource source(rawPoints(5));
  fx.pushLazy(fx.lidar, 10 * kSec, source);
  const auto& node = fx.install(recipe("crop", {"lidar"}));

  const auto decoded = fx.decode(fx.staged(node, 10 * kSec));
  EXPECT_EQ(fx.probe.parses.load(), 1);
  EXPECT_TRUE(fx.probe.lock_held);
  ASSERT_EQ(decoded.values.size(), 1u);
  ASSERT_NE(cloudIn(decoded.values[0]), nullptr);
  EXPECT_EQ(cloudIn(decoded.values[0])->width, 5u);
}

// GAP-49, the input half: staging and decoding never serialize nor copy an input. Each decoded object is
// the bytes it came from: the parser's view of the fetched bytes, a canonical point cloud's view over the
// store payload, an upstream output's view over its ring entry. A consumer sizes it from its fields.
TEST(InputStagerTest, StagingAndDecodingNeverSerialize) {
  constexpr size_t kPoints = 1000;
  Fixture fx;
  LazySource raw(rawPoints(kPoints));
  LazySource canonical(canonicalCloud(kPoints));
  fx.pushLazy(fx.lidar, 10 * kSec, raw);
  fx.pushLazy(fx.cloud, 10 * kSec, canonical);
  const auto& crop = fx.install(recipe("crop", {"cloud"}));
  const sdk::BuiltinObject cropped = cloudObject(kPoints);
  fx.publish(crop, 10 * kSec, croppedBundle(cropped));
  const auto& count = fx.install(recipe("count", {"lidar", "cloud", "owner/crop/cropped"}));

  const auto decoded = fx.decode(fx.staged(count, 10 * kSec));
  ASSERT_EQ(decoded.values.size(), 3u);
  for (const NamedValue& value : decoded.values) {
    ASSERT_NE(cloudIn(value), nullptr) << value.name;
    const size_t estimate = scripting::estimateBytes(value.value);
    EXPECT_GE(estimate, kPoints * kPointStride) << value.name;
    EXPECT_LT(estimate, kPoints * kPointStride + 4096) << value.name;
  }
  EXPECT_EQ(cloudIn(decoded.values[0])->data.data(), raw.bytes.bytes.data()) << "lidar";
  EXPECT_TRUE(within(cloudIn(decoded.values[1])->data.data(), kPoints * kPointStride, canonical.bytes.bytes))
      << "cloud";
  EXPECT_TRUE(within(
      cloudIn(decoded.values[2])->data.data(), kPoints * kPointStride,
      fx.store.latestAt(fx.topic("owner/crop/cropped"), 10 * kSec)->payload.bytes))
      << "the upstream output";
}

// An object a parser made can run code of its plugin library when destroyed: the decoded inputs and the
// cached result an input was passed through to hold the parser's keepalive until the last holder goes. A
// downstream decodes the ring's canonical bytes, so it holds neither the upstream's bundle nor its parser.
TEST(InputStagerTest, AParserMadeInputHoldsItsParserToTheLastHolder) {
  Fixture fx;
  LazySource source(rawPoints(4));
  fx.pushLazy(fx.lidar, 10 * kSec, source);
  const auto& crop = fx.install(recipe("crop", {"lidar"}));
  const auto& count = fx.install(recipe("count", {"owner/crop/cropped"}));
  std::optional<InputStager::Decoded> inputs = fx.decode(fx.staged(crop, 10 * kSec));
  ASSERT_EQ(inputs->values.size(), 1u);
  const std::weak_ptr<RawPointsParser> parser = fx.lidar_parser;
  fx.lidar_parser.reset();  // the topic's registration goes; the staged inputs went already
  EXPECT_FALSE(parser.expired()) << "the decoded input holds it";

  fx.publish(crop, 10 * kSec, croppedBundle(std::get<sdk::BuiltinObject>(inputs->values[0].value)), inputs->keepalives);
  inputs.reset();
  EXPECT_FALSE(parser.expired()) << "the cached result holds it";
  std::weak_ptr<const ScriptBundle> crop_bundle;
  {
    const Key request{.revision = crop.revision, .inputs = {static_cast<uint64_t>(10 * kSec)}};
    const auto hit = fx.history.lookup(crop.recipe.key(), request, 10 * kSec, 10 * kSec, 0);
    ASSERT_TRUE(hit.has_value() && hit->bundle != nullptr);
    crop_bundle = hit->bundle;
  }

  std::optional<InputStager::Decoded> downstream = fx.decode(fx.staged(count, 12 * kSec));
  ASSERT_EQ(downstream->values.size(), 1u);
  (void)fx.history.forget(crop.recipe.key());
  EXPECT_TRUE(crop_bundle.expired()) << "the downstream does not pin the upstream's bundle";
  EXPECT_TRUE(parser.expired()) << "nor its parser";
  EXPECT_EQ(cloudIn(downstream->values[0])->width, 4u);
}

// A lazy entry whose fetch fails (its source truncated or replaced) is a missing input, not a decode error.
TEST(InputStagerTest, AFailedFetchIsAMissingInput) {
  Fixture fx;
  ASSERT_TRUE(fx.store.pushLazy(fx.cloud, 10 * kSec, [] { return std::optional<sdk::PayloadView>{}; }).has_value());
  const auto& node = fx.install(recipe("crop", {"cloud"}));

  const auto decoded = InputStager::decode(fx.store, fx.staged(node, 10 * kSec));
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error(), "on-demand: missing input 'cloud'");
}

// ---- What staging resolves --------------------------------------------------------------------------

// CUR-01, the staging half: the result time is the newest resolved input, an upstream contributing its
// result stamp. A scalar is copied at staging.
TEST(InputStagerTest, StampIsTheNewestResolvedInput) {
  Fixture fx;
  LazySource source(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 10 * kSec, source);
  fx.appendScalar(fx.speed, 5 * kSec, 1.0);
  fx.appendScalar(fx.speed, 11 * kSec, 2.0);
  const auto& crop = fx.install(recipe("crop", {"cloud", "speed"}));

  const InputStager::Staged at12 = fx.staged(crop, 12 * kSec);
  EXPECT_EQ(at12.stamp, 11 * kSec) << "the speed sample";
  ASSERT_EQ(at12.inputs.size(), 2u);
  EXPECT_EQ(at12.inputs[1].alias, "speed");
  ASSERT_TRUE(std::holds_alternative<double>(at12.inputs[1].value));
  EXPECT_EQ(std::get<double>(at12.inputs[1].value), 2.0);
  EXPECT_EQ(fx.staged(crop, 10 * kSec + kSec / 2).stamp, 10 * kSec) << "the cloud entry";

  fx.publish(crop, 15 * kSec, croppedBundle(cloudObject(2)));
  const auto& count = fx.install(recipe("count", {"owner/crop/cropped", "speed"}));
  EXPECT_EQ(fx.staged(count, 20 * kSec).stamp, 15 * kSec) << "the upstream's result";
}

// D4, D5: a string series is staged like a number: its sample at or before `t` copied, its identity (the series and
// the sample's row) in the cache key; the decode hands the script the string.
TEST(InputStagerTest, StringSampleIsStagedByIdentity) {
  Fixture fx;
  LazySource source(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 10 * kSec, source);
  fx.appendState(5 * kSec, "idle");
  fx.appendState(11 * kSec, "moving");
  const auto& node = fx.install(recipe("annotate", {"cloud", "state"}));

  const InputStager::Staged at12 = fx.staged(node, 12 * kSec);
  EXPECT_EQ(at12.stamp, 11 * kSec) << "the state sample";
  ASSERT_EQ(at12.inputs.size(), 2u);
  ASSERT_TRUE(std::holds_alternative<std::string>(at12.inputs[1].value));
  EXPECT_EQ(std::get<std::string>(at12.inputs[1].value), "moving");
  EXPECT_EQ(at12.inputs[1].resolved, 11 * kSec);
  const InputStager::Decoded decoded = fx.decode(at12);
  ASSERT_EQ(decoded.values.size(), 2u);
  EXPECT_EQ(decoded.values[1].name, "state");
  ASSERT_TRUE(std::holds_alternative<std::string>(decoded.values[1].value));
  EXPECT_EQ(std::get<std::string>(decoded.values[1].value), "moving");

  const InputStager::Staged at10 = fx.staged(node, 10 * kSec);
  EXPECT_EQ(std::get<std::string>(at10.inputs[1].value), "idle");
  EXPECT_NE(at10.key, at12.key) << "another sample";
  EXPECT_EQ(fx.staged(node, 13 * kSec).key, at12.key) << "the same sample";
  fx.appendState(12 * kSec, "moving");
  EXPECT_NE(fx.staged(node, 13 * kSec).key, at12.key) << "a new sample, of the same value";
  EXPECT_EQ(source.fetches.load(), 1) << "only the decode fetched";
}

// D6: a string input with nothing at or before `t` is missing by its alias, as a number is.
TEST(InputStagerTest, StringInputBeforeItsFirstSampleIsMissing) {
  Fixture fx;
  LazySource source(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 10 * kSec, source);
  fx.appendState(12 * kSec, "idle");
  const auto& node = fx.install(recipe("annotate", {"cloud", "state"}));

  EXPECT_EQ(fx.stageError(node, 11 * kSec), "on-demand: missing input 'state'");
  EXPECT_EQ(fx.stageError(node, 12 * kSec), "");
  EXPECT_EQ(fx.stager.newestInputStamp(node), 12 * kSec) << "the live edge sees the string sample";
}

// D6: a null string row is a missing input at and after its time, yet it moves the live edge, so the result the
// previous string made does not stay displayed.
TEST(InputStagerTest, ANullStringRowIsMissingAndMovesTheLiveEdge) {
  Fixture fx;
  LazySource source(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 10 * kSec, source);
  fx.appendState(10 * kSec, "idle");
  test::appendNullString(fx.writer, fx.state, 20 * kSec);
  ASSERT_FALSE(fx.engine.commitChunks(fx.writer.flushAll()).empty());
  const auto& node = fx.install(recipe("annotate", {"cloud", "state"}));
  const auto& label = fx.install(recipe("label", {"state"}));

  EXPECT_EQ(fx.stageError(node, 15 * kSec), "");
  EXPECT_EQ(fx.stageError(node, 20 * kSec), "on-demand: missing input 'state'");
  EXPECT_EQ(fx.stager.newestInputStamp(node), 20 * kSec) << "the null row's time";
  EXPECT_EQ(fx.stager.newestInputStamp(label), 20 * kSec) << "a string-only recipe has a live edge too";
  std::vector<uint64_t> identities;
  EXPECT_FALSE(fx.stager.appendRawIdentities(node, 20 * kSec, identities)) << "nothing to read there";
}

// §10: equal-time string rows are told apart by their row, not by a hash of their text: these two hash alike on a
// 64-bit libstdc++ (0x83cc3862a8c12ca1), so a key holding the hash would serve the first one's result for the second.
TEST(InputStagerTest, EqualTimeStringRowsNeverShareAnIdentity) {
  Fixture fx;
  const auto& node = fx.install(recipe("label", {"state"}));
  fx.appendState(10 * kSec, "idle_state_00000");
  const InputStager::Staged first = fx.staged(node, 10 * kSec);
  fx.appendState(10 * kSec, "00006326\"M3hg`za");
  const InputStager::Staged second = fx.staged(node, 10 * kSec);
  ASSERT_EQ(std::get<std::string>(second.inputs[0].value), "00006326\"M3hg`za") << "the later row wins the tie";
  EXPECT_NE(first.key, second.key);

  // Two rows of one chunk.
  test::appendString(fx.writer, fx.state, 20 * kSec, "a");
  test::appendString(fx.writer, fx.state, 30 * kSec, "a");
  ASSERT_FALSE(fx.engine.commitChunks(fx.writer.flushAll()).empty());
  EXPECT_NE(fx.staged(node, 20 * kSec).key, fx.staged(node, 30 * kSec).key);
}

// D6: an input with nothing at or before `t` is reported by its alias, and nothing runs.
TEST(InputStagerTest, MissingInputIsReportedByName) {
  Fixture fx;
  LazySource source(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 10 * kSec, source);
  fx.appendScalar(fx.speed, 12 * kSec, 1.0);
  const auto& crop = fx.install(recipe("crop", {"cloud", "speed"}));

  EXPECT_EQ(fx.stageError(crop, 9 * kSec), "on-demand: missing input 'cloud'");
  EXPECT_EQ(fx.stageError(crop, 11 * kSec), "on-demand: missing input 'speed'");
  EXPECT_EQ(fx.stageError(crop, 12 * kSec), "");

  const auto& count = fx.install(recipe("count", {"owner/crop/cropped"}));
  EXPECT_EQ(fx.stageError(count, 12 * kSec), "on-demand: missing input 'owner/crop/cropped'") << "no upstream result";
  EXPECT_EQ(source.fetches.load(), 0);
}

// An entry the store evicted between staging and decoding is a missing input too.
TEST(InputStagerTest, AnEntryEvictedBeforeDecodeIsMissing) {
  Fixture fx;
  LazySource source(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 10 * kSec, source);
  const auto& node = fx.install(recipe("crop", {"cloud"}));
  const InputStager::Staged staged = fx.staged(node, 10 * kSec);

  prune(fx.store, fx.cloud);
  const auto decoded = InputStager::decode(fx.store, staged);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error(), "on-demand: missing input 'cloud'");
  EXPECT_EQ(source.fetches.load(), 0);
}

// A merge shifts a source's store timestamps but not the stamps inside its payloads: the decoded input
// carries the entry's shift, for the engine to add.
TEST(InputStagerTest, DecodedInputCarriesItsEntryStampShift) {
  Fixture fx;
  constexpr DatasetId kMerged = 99;
  const ObjectTopicId merged_cloud = fx.store.registerTopic({kMerged, "cloud", ""}).value();
  LazySource source(canonicalCloud(1));
  fx.pushLazy(merged_cloud, 10 * kSec, source);
  ASSERT_TRUE(fx.store.mergeDatasets(fx.dataset, {{kMerged, 5 * kSec}}).has_value());
  const auto& node = fx.install(recipe("crop", {"cloud"}));

  const InputStager::Staged staged = fx.staged(node, 15 * kSec);
  EXPECT_EQ(staged.stamp, 15 * kSec);
  const auto decoded = fx.decode(staged);
  ASSERT_EQ(decoded.values.size(), 1u);
  EXPECT_EQ(decoded.values[0].stamp_shift, 5 * kSec);
}

// ---- Upstream results -------------------------------------------------------------------------------

// GAP-13 (changed, decided 2026-10-18): the compute cache keeps no decoded copy of a result the ring holds,
// so a downstream decodes its upstream's output from the ring entry, a point cloud as a view over its bytes.
// An entry gone before the decode is a missing input.
TEST(InputStagerTest, DownstreamDecodesItsUpstreamOutputFromTheRing) {
  constexpr size_t kPoints = 1000;
  Fixture fx;
  LazySource source(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 10 * kSec, source);
  const auto& crop = fx.install(recipe("crop", {"cloud"}));
  fx.publish(crop, 10 * kSec, croppedBundle(cloudObject(kPoints)));
  const auto& count = fx.install(recipe("count", {"owner/crop/cropped"}));

  const InputStager::Staged staged = fx.staged(count, 12 * kSec);
  const auto decoded = fx.decode(staged);
  ASSERT_EQ(decoded.values.size(), 1u);
  ASSERT_NE(cloudIn(decoded.values[0]), nullptr);
  EXPECT_EQ(cloudIn(decoded.values[0])->width, kPoints);
  const auto entry = fx.store.latestAt(fx.topic("owner/crop/cropped"), 12 * kSec);
  ASSERT_TRUE(entry.has_value());
  EXPECT_TRUE(within(cloudIn(decoded.values[0])->data.data(), kPoints * kPointStride, entry->payload.bytes))
      << "a view over the ring entry";

  prune(fx.store, fx.topic("owner/crop/cropped"));
  const auto gone = InputStager::decode(fx.store, staged);
  ASSERT_FALSE(gone.has_value());
  EXPECT_EQ(gone.error(), "on-demand: missing input 'owner/crop/cropped'");
}

// D25 (DAG-06, GAP-30): an unavailable upstream output is a missing input with its reason, never the empty
// object the ring holds for it; a sibling output of the same result still flows.
TEST(InputStagerTest, AnUnavailableUpstreamOutputIsAMissingInput) {
  Fixture fx;
  LazySource source(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 10 * kSec, source);
  RecipeGraph::Recipe split = recipe("split", {"cloud"});
  split.outputs.push_back({"rest", OutputType::kObject, sdk::BuiltinObjectType::kPointCloud});
  const auto& upstream = fx.install(split);
  ScriptBundle bundle;
  bundle.outputs.push_back({.name = "cropped", .value = scripting::Unavailable{"no points in the box"}});
  bundle.outputs.push_back({.name = "rest", .value = cloudObject(2)});
  fx.publish(upstream, 10 * kSec, std::move(bundle));

  const auto& cropped = fx.install(recipe("count", {"owner/split/cropped"}));
  EXPECT_EQ(fx.stageError(cropped, 12 * kSec), "on-demand: missing input 'owner/split/cropped': no points in the box");
  const auto& rest = fx.install(recipe("rest", {"owner/split/rest"}));
  EXPECT_EQ(fx.stageError(rest, 12 * kSec), "");
}

// Decided 2026-10-18: a computation reads a pinned finding only from its real stamp, although the store
// holds it at `kFindingTimestamp` and the display shows it at any instant.
TEST(InputStagerTest, APinnedFindingIsAnInputOnlyFromItsRealStamp) {
  Fixture fx;
  LazySource source(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 15 * kSec, source);
  const auto& finding = fx.install(recipe("finding", {"cloud"}, 15 * kSec));
  fx.publish(finding, 15 * kSec, croppedBundle(cloudObject(3)));
  const EntryTimestampsView stored = fx.store.entryTimestamps(fx.topic("owner/finding/cropped"));
  ASSERT_EQ(stored.size(), 1u);
  EXPECT_EQ(stored[0], kFindingTimestamp);

  const auto& count = fx.install(recipe("count", {"owner/finding/cropped"}));
  EXPECT_EQ(fx.stageError(count, 10 * kSec), "on-demand: missing input 'owner/finding/cropped'");
  EXPECT_EQ(fx.staged(count, 15 * kSec).stamp, 15 * kSec);
  EXPECT_EQ(fx.staged(count, 20 * kSec).stamp, 15 * kSec);
}

// The live edge's lookup (ARCHITECTURE §16): the newest raw input stamp, found by identity (nothing fetched or
// parsed), an upstream's output left to the caller; no sample is no stamp, a negative one is a stamp.
TEST(InputStagerTest, NewestInputStampReadsIdentitiesOnly) {
  Fixture fx;
  LazySource cloud(canonicalCloud(4));
  LazySource raw(rawPoints(1));
  fx.pushLazy(fx.cloud, 10 * kSec, cloud);
  fx.pushLazy(fx.cloud, 12 * kSec, cloud);
  fx.pushLazy(fx.lidar, 9 * kSec, raw);
  fx.appendScalar(fx.speed, 11 * kSec, 1.0);
  const auto& crop = fx.install(recipe("crop", {"cloud", "speed", "lidar"}));
  EXPECT_EQ(fx.stager.newestInputStamp(crop), 12 * kSec);
  fx.appendScalar(fx.speed, 15 * kSec, 2.0);
  EXPECT_EQ(fx.stager.newestInputStamp(crop), 15 * kSec) << "a scalar's newest sample";
  EXPECT_EQ(cloud.fetches.load() + raw.fetches.load(), 0);
  EXPECT_EQ(fx.probe.parses.load(), 0);

  fx.publish(crop, 30 * kSec, croppedBundle(cloudObject(1)));
  const auto& count = fx.install(recipe("count", {"owner/crop/cropped"}));
  const ObjectTopicId namesake = fx.store.registerTopic({fx.dataset, "cropped", ""}).value();
  fx.pushLazy(namesake, 50 * kSec, cloud);  // a raw topic named like the upstream's output is not its input
  EXPECT_FALSE(fx.stager.newestInputStamp(count).has_value()) << "the upstream's inputs are the caller's to follow";
  const auto& other = fx.install(recipe("other", {"other"}));
  EXPECT_FALSE(fx.stager.newestInputStamp(other).has_value()) << "no sample";
  fx.pushLazy(fx.other, -1 * kSec, cloud);
  EXPECT_EQ(fx.stager.newestInputStamp(other), -1 * kSec);
}

// The stager remembers which topic an input names; a topic removed and registered again under the name (a new id)
// is followed, a removed one is a missing input.
TEST(InputStagerTest, AnInputTopicRegisteredAgainUnderItsNameIsFollowed) {
  Fixture fx;
  LazySource cloud(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 10 * kSec, cloud);
  const auto& crop = fx.install(recipe("crop", {"cloud"}));
  ASSERT_EQ(fx.stager.newestInputStamp(crop), 10 * kSec);
  ASSERT_EQ(fx.stageError(crop, 10 * kSec), "");

  fx.store.removeTopic(fx.cloud);
  EXPECT_FALSE(fx.stager.newestInputStamp(crop).has_value());
  EXPECT_EQ(fx.stageError(crop, 10 * kSec), "on-demand: missing input 'cloud'");

  const ObjectTopicId again = fx.store.registerTopic({fx.dataset, "cloud", ""}).value();
  ASSERT_NE(again.id, fx.cloud.id);
  fx.pushLazy(again, 20 * kSec, cloud);
  EXPECT_EQ(fx.stager.newestInputStamp(crop), 20 * kSec);
  const InputStager::Staged staged = fx.staged(crop, 20 * kSec);
  ASSERT_EQ(staged.inputs.size(), 1u);
  EXPECT_EQ(std::get<InputStager::Entry>(staged.inputs[0].value).topic, again);
}

// Slice 8a-1: an explicit evaluation feeds a reader its own upstream results through overrides: the reader stages
// them (stamp, identity, value) instead of what the ring shows, and an unavailable one is its missing input (D25).
TEST(InputStagerTest, OverridesFeedADownstreamWithoutTheHistory) {
  Fixture fx;
  LazySource source(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 10 * kSec, source);
  const auto& crop = fx.install(recipe("crop", {"cloud"}));
  fx.publish(crop, 10 * kSec, croppedBundle(cloudObject(5)));  // what the ring shows
  const auto& count = fx.install(recipe("count", {"owner/crop/cropped", "cloud"}));
  const sdk::BuiltinObject evaluated = cloudObject(7);
  const auto keepalive = std::make_shared<int>(0);
  InputStager::Override override{
      .stamp = 11 * kSec, .identity = {42, 43}, .value = InputStager::Object{{keepalive}, evaluated}};
  const InputStager::Overrides overrides = [&override](std::string_view upstream, std::string_view output) {
    return upstream == "owner/crop" && output == "cropped" ? &override : nullptr;
  };

  auto staged = fx.stager.stage(count, 12 * kSec, 0, overrides);
  ASSERT_TRUE(staged.has_value()) << staged.error();
  EXPECT_EQ(staged->stamp, 11 * kSec);
  ASSERT_EQ(staged->inputs.size(), 2u);
  EXPECT_EQ(staged->inputs[0].resolved, 11 * kSec);
  EXPECT_EQ(staged->inputs[1].resolved, 10 * kSec);
  const auto& key = staged->key.inputs;
  EXPECT_TRUE(std::search(key.begin(), key.end(), override.identity.begin(), override.identity.end()) != key.end())
      << "the override's identity is the input's";
  const auto decoded = fx.decode(*staged);
  ASSERT_EQ(decoded.values.size(), 2u);
  ASSERT_NE(cloudIn(decoded.values[0]), nullptr);
  EXPECT_EQ(cloudIn(decoded.values[0])->data.data(), evaluated.get<sdk::PointCloud>()->data.data()) << "not a copy";
  EXPECT_NE(std::find(decoded.keepalives.begin(), decoded.keepalives.end(), keepalive), decoded.keepalives.end());

  override.value = scripting::Unavailable{"no points"};
  const auto unavailable = fx.stager.stage(count, 12 * kSec, 0, overrides);
  ASSERT_FALSE(unavailable.has_value());
  EXPECT_EQ(unavailable.error(), "on-demand: missing input 'owner/crop/cropped': no points");
  EXPECT_EQ(fx.staged(count, 12 * kSec).inputs[0].resolved, 10 * kSec) << "without overrides, the ring";
}

// ---- The cache key ----------------------------------------------------------------------------------

// §10: each input identity is part of the request's key (an object's uid and parser, a scalar sample's
// series, timestamp and value, an upstream's entry, which a new upstream revision replaces), and so is the
// epoch; a write to a topic the node does not read is not.
TEST(InputStagerTest, CacheKeyChangesWithEachInputIdentity) {
  Fixture fx;
  LazySource first(canonicalCloud(1));
  LazySource second(canonicalCloud(1));
  LazySource raw(rawPoints(1));
  LazySource unrelated(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 10 * kSec, first);
  fx.pushLazy(fx.lidar, 10 * kSec, raw);
  fx.appendScalar(fx.speed, 10 * kSec, 1.0);
  RecipeGraph::Recipe crop_recipe = recipe("crop", {"cloud"});
  const auto& crop = fx.install(crop_recipe);
  fx.publish(crop, 10 * kSec, croppedBundle(cloudObject(1)));
  const auto& count = fx.install(recipe("count", {"cloud", "speed", "owner/crop/cropped", "lidar"}));

  constexpr Timestamp kT = 12 * kSec;
  Key key = fx.staged(count, kT).key;
  EXPECT_EQ(key.revision, count.revision);
  EXPECT_EQ(key.epoch, 0u);
  EXPECT_FALSE(key.query_clock.has_value());
  EXPECT_FALSE(key.tf_revision.has_value());
  const auto expectKeyChange = [&](bool changed, const char* why, uint64_t epoch = 0) {
    const Key now = fx.staged(count, kT, epoch).key;
    EXPECT_EQ(now != key, changed) << why;
    key = now;
  };

  fx.pushLazy(fx.other, 11 * kSec, unrelated);
  fx.appendScalar(fx.heading, 11 * kSec, 3.0);
  expectKeyChange(false, "a write to topics the node does not read");
  fx.pushLazy(fx.cloud, 11 * kSec, second);
  expectKeyChange(true, "a new object entry");
  fx.appendScalar(fx.speed, 11 * kSec, 1.0);
  expectKeyChange(true, "a new scalar sample, of the same value");
  // The value word cannot be exercised: the engine only appends, and the at-or-before read keeps the
  // first-committed row of a timestamp, so a sample's value never changes under its timestamp (a reload
  // replaces the storage under a new epoch). The word keeps the key from relying on that.
  crop_recipe.params_json = R"({"z": 1})";
  fx.install(crop_recipe);
  fx.publish(crop, 10 * kSec, croppedBundle(cloudObject(1)));
  expectKeyChange(true, "a new upstream revision");
  const auto previous_parser = fx.replaceLidarParser();  // held, so the new one cannot reuse its address
  expectKeyChange(true, "another parser for the same entry");
  expectKeyChange(true, "a new epoch", 1);
  EXPECT_EQ(first.fetches.load() + second.fetches.load() + raw.fetches.load() + unrelated.fetches.load(), 0);
}

// ---- Measurement ------------------------------------------------------------------------------------

constexpr int kBenchRuns = 101;

// GAP-26 (PJ_BENCH=1): the GUI cost of staging a lazily stored 260k-point cloud against a 1-point one,
// and the worker cost of decoding it (its fetch, a view here, plus the canonical decode, a view too).
TEST(InputStagerBench, StageAndDecodeALazyPointCloud) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr size_t kPoints = 260'000;
  Fixture fx;
  LazySource large(canonicalCloud(kPoints));
  LazySource small(canonicalCloud(1));
  fx.pushLazy(fx.cloud, 10 * kSec, large);
  fx.pushLazy(fx.other, 10 * kSec, small);
  const auto& large_node = fx.install(recipe("large", {"cloud"}));
  const auto& small_node = fx.install(recipe("small", {"other"}));
  const InputStager::Staged staged = fx.staged(large_node, 10 * kSec);

  const double stage_large =
      medianMicros(kBenchRuns, [&] { EXPECT_TRUE(fx.stager.stage(large_node, 10 * kSec, 0).has_value()); });
  const double stage_small =
      medianMicros(kBenchRuns, [&] { EXPECT_TRUE(fx.stager.stage(small_node, 10 * kSec, 0).has_value()); });
  const double decode_large =
      medianMicros(kBenchRuns, [&] { EXPECT_TRUE(InputStager::decode(fx.store, staged).has_value()); });
  EXPECT_EQ(large.fetches.load(), kBenchRuns) << "only the decodes fetched";
  std::printf(
      "[InputStagerBench] stage (GUI): %.2f us for %zu points, %.2f us for 1 point; decode (worker): %.1f us "
      "(%zu bytes)\n",
      stage_large, kPoints, stage_small, decode_large, large.bytes.bytes.size());
}

// PJ_BENCH=1: staging among 10k object topics and 10k scalar names, the node's topic and series registered
// last: what the store's topic lookup and a catalog-like scalar lookup, both linear scans, cost per stage.
TEST(InputStagerBench, StageAmongTenThousandTopics) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr int kTopics = 10'000;
  Fixture fx;
  for (int index = 0; index < kTopics; ++index) {
    ASSERT_TRUE(fx.store.registerTopic({fx.dataset, "noise/" + std::to_string(index), ""}).has_value());
    fx.scalars.emplace_back("noise_scalar/" + std::to_string(index), fx.heading.topic_id);
  }
  const ObjectTopicId last_topic = fx.store.registerTopic({fx.dataset, "last", ""}).value();
  fx.scalars.emplace_back("last_speed", fx.speed.topic_id);
  LazySource source(canonicalCloud(1));
  fx.pushLazy(last_topic, 10 * kSec, source);
  fx.appendScalar(fx.speed, 10 * kSec, 1.0);
  const auto& node = fx.install(recipe("last", {"last", "last_speed"}));

  const double stage = medianMicros(kBenchRuns, [&] { EXPECT_TRUE(fx.stager.stage(node, 10 * kSec, 0).has_value()); });
  std::printf(
      "[InputStagerBench] stage among %d object topics and %d scalar names: %.1f us\n", kTopics, kTopics, stage);
}

// PJ_BENCH=1: the GUI cost of staging one string input against one number, each read through `sampleAt` (the
// session's read) over 10k samples: a string costs the failed numeric probe, the row lookup and a copy.
TEST(InputStagerBench, StageAStringInputAgainstANumber) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr int kSamples = 10'000;
  Fixture fx;
  for (int index = 0; index < kSamples; ++index) {
    fx.writer.appendScalar(fx.speed, index * kSec, index);
    test::appendString(fx.writer, fx.state, index * kSec, index % 2 == 0 ? "waiting_for_goal" : "executing_goal");
  }
  ASSERT_FALSE(fx.engine.commitChunks(fx.writer.flushAll()).empty());
  const auto& number = fx.install(recipe("number", {"speed"}));
  const auto& text = fx.install(recipe("text", {"state"}));
  constexpr Timestamp kT = kSamples / 2 * kSec + kSec / 2;

  const double stage_number =
      medianMicros(kBenchRuns, [&] { EXPECT_TRUE(fx.stager.stage(number, kT, 0).has_value()); });
  const double stage_text = medianMicros(kBenchRuns, [&] { EXPECT_TRUE(fx.stager.stage(text, kT, 0).has_value()); });
  const InputStager::Staged staged = fx.staged(text, kT);
  const double decode_text =
      medianMicros(kBenchRuns, [&] { EXPECT_TRUE(InputStager::decode(fx.store, staged).has_value()); });
  std::printf(
      "[InputStagerBench] stage (GUI) over %d samples: number %.0f ns, string %.0f ns; decode (worker) string %.0f "
      "ns\n",
      kSamples, stage_number * 1000.0, stage_text * 1000.0, decode_text * 1000.0);
}

}  // namespace
}  // namespace PJ
