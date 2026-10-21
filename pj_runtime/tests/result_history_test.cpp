// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// ResultHistory: the ring (derived topics in a real ObjectStore) and the compute cache, driven
// directly, as the scheduler (slice 6a-4) drives them. Nodes come from a real RecipeGraph over a
// fake resolver; bundles are built by hand, no script runs.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "on_demand_test_utils.h"
#include "pj_base/buffer_anchor.hpp"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_runtime/DerivedTopics.h"
#include "pj_runtime/ResultHistory.h"
#include "pj_scripting/python_object_script.h"

namespace PJ {
namespace {

using scripting::OutputSpec;
using scripting::OutputType;
using scripting::ScriptBundle;
using scripting::Unavailable;
using Key = ResultHistory::CacheKey;
using Limits = ResultHistory::Limits;
using test::decodeAt;
using test::kPointStride;
using test::makeCloud;
using test::prune;
using test::timestampsOf;

constexpr DatasetId kDataset = 1;
constexpr Timestamp kSec = 1'000'000'000;
constexpr Timestamp kMin = std::numeric_limits<Timestamp>::min();
constexpr Timestamp kMax = std::numeric_limits<Timestamp>::max();

OutputSpec croppedSpec() {
  return {"cropped", OutputType::kObject, sdk::BuiltinObjectType::kPointCloud};
}
OutputSpec witnessSpec() {
  return {"witness", OutputType::kObject, sdk::BuiltinObjectType::kSceneEntities};
}

/// `owner/<id>`, reading `cloud`, with `outputs`; the script is never run.
RecipeGraph::Recipe recipeWith(std::string id, std::vector<OutputSpec> outputs) {
  return test::onDemandRecipe(std::move(id), {"cloud"}, std::move(outputs));
}
/// One point-cloud output, `cropped`.
RecipeGraph::Recipe passthroughRecipe(std::string id = "crop") {
  return recipeWith(std::move(id), {croppedSpec()});
}
/// The reference shape: two object outputs, a number and a string.
RecipeGraph::Recipe cropRecipe(std::string id = "crop") {
  return recipeWith(
      std::move(id), {croppedSpec(),
                      witnessSpec(),
                      {"max_z", OutputType::kNumber, sdk::BuiltinObjectType::kNone},
                      {"report", OutputType::kString, sdk::BuiltinObjectType::kNone}});
}

sdk::SceneEntities makeScene(std::string frame = "base_link") {
  sdk::SceneEntity entity;
  entity.frame_id = std::move(frame);
  entity.id = "w";
  entity.spheres.emplace_back();
  sdk::SceneEntities scene;
  scene.entities.push_back(std::move(entity));
  return scene;
}

ScriptBundle cloudBundle(size_t points, Timestamp stamp = 0) {
  ScriptBundle bundle;
  bundle.outputs.push_back({.name = "cropped", .value = sdk::BuiltinObject(makeCloud(points, stamp))});
  return bundle;
}
ScriptBundle cropBundle(size_t points, double max_z) {
  ScriptBundle bundle;
  bundle.outputs.push_back({.name = "cropped", .value = sdk::BuiltinObject(makeCloud(points))});
  bundle.outputs.push_back({.name = "witness", .value = sdk::BuiltinObject(makeScene())});
  bundle.outputs.push_back({.name = "max_z", .value = max_z});
  bundle.outputs.push_back({.name = "report", .value = std::to_string(points) + " points"});
  return bundle;
}
/// What a failed run publishes: every output unavailable.
ScriptBundle unavailableBundle(const RecipeGraph::Recipe& recipe, const std::string& reason) {
  ScriptBundle bundle;
  for (const OutputSpec& output : recipe.outputs) {
    bundle.outputs.push_back({.name = output.name, .value = Unavailable{reason}});
  }
  return bundle;
}

/// The request key of `node` whose inputs are identified by `input`.
Key keyOf(const RecipeGraph::Node& node, uint64_t input) {
  return Key{.revision = node.revision, .inputs = {input}};
}

size_t serializedSize(const sdk::BuiltinObject& object) {
  return serializeBuiltinObject(object).value().size();
}

/// A real store and graph; the resolver binds `cloud` in `kDataset`.
struct Fixture {
  explicit Fixture(Limits limits = {}) : history(store, limits) {}

  /// Installs `recipe` in the graph, then in the history. The node is the graph's: a later upsert of
  /// the same key updates it in place.
  const RecipeGraph::Node& install(const RecipeGraph::Recipe& recipe) {
    const Status upserted = graph.upsert(recipe);
    EXPECT_TRUE(upserted.has_value()) << upserted.error();
    const RecipeGraph::Node* node = graph.find(recipe.key());
    const auto retired = history.install(*node);
    EXPECT_TRUE(retired.has_value()) << retired.error();
    return *node;
  }
  [[nodiscard]] ObjectTopicId topic(const std::string& name) const {
    const auto id = store.findTopic(kDataset, name);
    EXPECT_TRUE(id.has_value()) << name;
    return id.value_or(ObjectTopicId{});
  }
  /// Requested and stamped at `t`, under an input identity of `t` itself: the request moves the cursor
  /// first, as the scheduler's does.
  void publishAt(const RecipeGraph::Node& node, Timestamp t, const ScriptBundle& bundle) {
    history.noteRequest(node.recipe.key(), t);
    const Status published = history.publish(node, t, t, keyOf(node, static_cast<uint64_t>(t)), bundle);
    EXPECT_TRUE(published.has_value()) << published.error();
  }

  ObjectStore store;
  RecipeGraph graph{[](std::string_view input) {
    return input == "cloud"
               ? std::vector<RecipeGraph::InputCandidate>{{kDataset, sdk::BuiltinObjectType::kPointCloud, "cloud"}}
               : std::vector<RecipeGraph::InputCandidate>{};
  }};
  ResultHistory history;
};

// ---- CUR: what a publication writes ---------------------------------------------------------------

// CUR-01: the entry sits at the result time (the newest resolved input), not at the requested time,
// and the payload keeps the stamp its script left.
TEST(ResultHistoryTest, ObjectOutputsAreStampedAtTheirInputsNotAtTheRequestedTime) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  EXPECT_TRUE(isDerivedObjectTopic(fx.store.descriptor(topic)));

  const Key at10 = keyOf(node, 10);
  ASSERT_TRUE(fx.history.publish(node, 12 * kSec, 10 * kSec, at10, cloudBundle(3, 7 * kSec)).has_value());
  EXPECT_EQ(timestampsOf(fx.store, topic), std::vector<Timestamp>{10 * kSec});
  EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, topic, 10 * kSec).timestamp_ns, 7 * kSec) << "no restamping";

  // The same inputs resolve at 14 s: a hit, whose republication writes nothing.
  const auto hit = fx.history.lookup(node.recipe.key(), at10, 10 * kSec, 14 * kSec, 0);
  ASSERT_TRUE(hit.has_value() && hit->bundle != nullptr);
  EXPECT_EQ(hit->stamp, 10 * kSec);
  const auto uid = fx.store.latestAt(topic, kMax)->sequential_uid;
  fx.history.noteRequest(node.recipe.key(), 14 * kSec);
  ASSERT_TRUE(fx.history.publish(node, 14 * kSec, hit->stamp, at10, *hit->bundle).has_value());
  EXPECT_EQ(fx.store.entryCount(topic), 1u);
  EXPECT_EQ(fx.store.latestAt(topic, kMax)->sequential_uid, uid);

  // An input changes at 15 s: a second entry, at 15 s.
  ASSERT_TRUE(fx.history.publish(node, 16 * kSec, 15 * kSec, keyOf(node, 15), cloudBundle(2)).has_value());
  EXPECT_EQ(timestampsOf(fx.store, topic), (std::vector<Timestamp>{10 * kSec, 15 * kSec}));
}

// CUR-03: seeking back to a computed instant is a cache hit with the same values; nothing new lands.
TEST(ResultHistoryTest, SeekBackAndForthReusesCacheAndDedupes) {
  Fixture fx;
  const auto& node = fx.install(cropRecipe());
  fx.publishAt(node, 20 * kSec, cropBundle(2, 1.2));
  fx.publishAt(node, 5 * kSec, cropBundle(1, 0.4));

  const auto hit = fx.history.lookup(node.recipe.key(), keyOf(node, 20 * kSec), 20 * kSec, 20 * kSec, 0);
  ASSERT_TRUE(hit.has_value() && hit->bundle != nullptr);
  EXPECT_DOUBLE_EQ(std::get<double>(hit->bundle->outputs[2].value), 1.2);
  EXPECT_EQ(std::get<std::string>(hit->bundle->outputs[3].value), "2 points");
  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, hit->stamp, keyOf(node, 20 * kSec), *hit->bundle).has_value());
  EXPECT_EQ(fx.store.entryCount(fx.topic("owner/crop/cropped")), 2u);
  EXPECT_EQ(fx.store.entryCount(fx.topic("owner/crop/witness")), 2u);
}

// CUR-05: an unavailable object output writes an empty object of its type (a cloud with no points, a
// scene that deletes everything), so the layer clears; the next available result draws again.
TEST(ResultHistoryTest, UnavailableOutputPublishesEmptyObject) {
  Fixture fx;
  const auto recipe = cropRecipe();
  const auto& node = fx.install(recipe);
  const ObjectTopicId cropped = fx.topic("owner/crop/cropped");
  const ObjectTopicId witness = fx.topic("owner/crop/witness");
  fx.publishAt(node, 20 * kSec, cropBundle(2, 1.2));

  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 1), unavailableBundle(recipe, "max_z")));
  EXPECT_EQ(fx.store.entryCount(cropped), 1u) << "a different result at the same stamp replaces it";
  EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, cropped, 20 * kSec).width, 0u);
  const auto scene = decodeAt<sdk::SceneEntities>(fx.store, witness, 20 * kSec);
  EXPECT_TRUE(scene.entities.empty());
  ASSERT_EQ(scene.deletions.size(), 1u);
  EXPECT_EQ(scene.deletions[0].type, sdk::SceneEntityDeletion::Type::kAll);
  EXPECT_EQ(scene.deletions[0].timestamp, 20 * kSec);

  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 2), cropBundle(3, 1.9)));
  EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, cropped, 20 * kSec).width, 3u);
}

// CUR-09 (the ring half): a failed run's tombstones go in the frame each output was last published in,
// at the result time.
TEST(ResultHistoryTest, ScriptErrorPublishesEmptyObjectsInTheLastFrame) {
  Fixture fx;
  const auto recipe = cropRecipe();
  const auto& node = fx.install(recipe);
  fx.publishAt(node, 10 * kSec, cropBundle(2, 1.2));

  ASSERT_TRUE(
      fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 20), unavailableBundle(recipe, "threshold too low")));
  const auto cloud = decodeAt<sdk::PointCloud>(fx.store, fx.topic("owner/crop/cropped"), 20 * kSec);
  EXPECT_EQ(cloud.width, 0u);
  EXPECT_TRUE(cloud.fields.empty());
  EXPECT_EQ(cloud.frame_id, "base_link");
  EXPECT_EQ(cloud.timestamp_ns, 20 * kSec);
  const auto scene = decodeAt<sdk::SceneEntities>(fx.store, fx.topic("owner/crop/witness"), 20 * kSec);
  EXPECT_TRUE(scene.entities.empty());
  ASSERT_EQ(scene.deletions.size(), 1u);
  EXPECT_EQ(scene.deletions[0].type, sdk::SceneEntityDeletion::Type::kAll);
  EXPECT_EQ(scene.deletions[0].timestamp, 20 * kSec);
}

// A run requested earlier lands after the user moved back: the request, not the publication, is the
// cursor, so the result on display stays and the late one is the farthest.
TEST(ResultHistoryTest, ALatePublicationDoesNotMoveTheCursor) {
  const size_t one = serializedSize(sdk::BuiltinObject(makeCloud(1)));
  Fixture fx(Limits{.ring_bytes = one});
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  fx.publishAt(node, 10 * kSec, cloudBundle(1));
  fx.history.noteRequest(node.recipe.key(), 20 * kSec);  // a run starts for 20 s
  fx.history.noteRequest(node.recipe.key(), 10 * kSec);  // back at 10 s: a hit
  const auto hit = fx.history.lookup(node.recipe.key(), keyOf(node, 10 * kSec), 10 * kSec, 10 * kSec, 0);
  ASSERT_TRUE(hit.has_value() && hit->bundle != nullptr);
  ASSERT_TRUE(fx.history.publish(node, 10 * kSec, hit->stamp, keyOf(node, 10 * kSec), *hit->bundle));

  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 20 * kSec), cloudBundle(1)));
  EXPECT_EQ(timestampsOf(fx.store, topic), std::vector<Timestamp>{10 * kSec}) << "the displayed result stays";
}

// A late publication's window trim spares the result the cursor shows, even one farther than the window
// before the cursor: at most that one entry stays past the window.
TEST(ResultHistoryTest, ALatePublicationDoesNotTrimTheDisplayedResult) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  fx.publishAt(node, 50 * kSec, cloudBundle(1));
  fx.history.noteRequest(node.recipe.key(), 5 * kSec);    // a run starts for 5 s
  fx.history.noteRequest(node.recipe.key(), 100 * kSec);  // the cursor seeks on: 50 s is displayed
  ASSERT_TRUE(fx.history.publish(node, 5 * kSec, 5 * kSec, keyOf(node, 5 * kSec), cloudBundle(1)));
  const auto shown = fx.store.latestAt(topic, 100 * kSec);
  ASSERT_TRUE(shown.has_value());
  EXPECT_EQ(shown->timestamp, 50 * kSec);
}

// One pass takes victims from both sides of the displayed result, farthest first, and spares it.
TEST(ResultHistoryTest, EvictionTakesBothSidesOfTheDisplayedResult) {
  const size_t one = serializedSize(sdk::BuiltinObject(makeCloud(1)));
  const size_t big = serializedSize(sdk::BuiltinObject(makeCloud(100)));
  Fixture fx(Limits{.ring_bytes = big + 2 * one});
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  for (const Timestamp sec : {1, 2, 4, 5}) {
    fx.publishAt(node, sec * kSec, cloudBundle(1));
  }
  fx.publishAt(node, 3 * kSec, cloudBundle(100));  // displayed at 3 s, two results over the budget
  EXPECT_EQ(timestampsOf(fx.store, topic), (std::vector<Timestamp>{2 * kSec, 3 * kSec, 4 * kSec}));
  EXPECT_EQ(fx.history.ringBytes(), big + 2 * one);
}

// An entry evicted behind the history's back is neither a node's displayed result nor charged: past
// the budget it leaves the ledger before any victim is chosen.
TEST(ResultHistoryTest, AnEntryGoneFromTheStoreIsNeitherDisplayedNorCharged) {
  const size_t one = serializedSize(sdk::BuiltinObject(makeCloud(1)));
  Fixture fx(Limits{.ring_bytes = 3 * one});
  const auto& first = fx.install(passthroughRecipe("first"));
  const auto& second = fx.install(passthroughRecipe("second"));
  const ObjectTopicId first_topic = fx.topic("owner/first/cropped");
  fx.publishAt(first, 10 * kSec, cloudBundle(1));
  fx.publishAt(first, 20 * kSec, cloudBundle(1));
  std::vector<OwnedObjectWrite> evict(1);
  evict[0].topic = first_topic;
  evict[0].append = false;
  evict[0].drop = [](Timestamp t, SequentialUID) { return t == 20 * kSec; };
  ASSERT_TRUE(fx.store.pushOwnedBundle(std::move(evict)).has_value());
  fx.history.noteRequest(first.recipe.key(), 25 * kSec);

  fx.publishAt(second, 24 * kSec, cloudBundle(1));
  fx.publishAt(second, 25 * kSec, cloudBundle(1));
  EXPECT_EQ(timestampsOf(fx.store, first_topic), std::vector<Timestamp>{10 * kSec});
  EXPECT_EQ(fx.store.entryCount(fx.topic("owner/second/cropped")), 2u);
  EXPECT_EQ(fx.history.ringBytes(), 3 * one) << "the ghost's bytes are released";
}

// A clock-observing run with unchanged inputs makes a new key at the same stamp for every request; the
// cache places each at the instant it observed, so the window bounds them.
TEST(ResultHistoryTest, ClockObservingResultsAtOneStampStayWithinTheWindow) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  const Key request = keyOf(node, 10);
  constexpr Timestamp kPeriod = kSec / 10;
  size_t one = 0;
  for (Timestamp i = 0; i < 1000; ++i) {
    const Timestamp t = 10 * kSec + i * kPeriod;
    Key observed = request;
    observed.query_clock = t;
    fx.history.noteRequest(node.recipe.key(), t);
    ASSERT_TRUE(fx.history.publish(node, t, 10 * kSec, observed, cloudBundle(1)));
    one = one == 0 ? fx.history.cacheBytes() : one;
  }
  EXPECT_LE(fx.history.cacheBytes(), (2 * 30 * kSec / kPeriod + 2) * one);
  EXPECT_FALSE(fx.history.lookup(node.recipe.key(), request, 10 * kSec, 10 * kSec, 0).has_value())
      << "the first instant left the window";
}

// An unavailable output of every type a script can produce writes an empty object of that type at the
// result time.
TEST(ResultHistoryTest, AnUnavailableOutputOfAnyProducibleTypeWritesAStampedTombstone) {
  Fixture fx;
  const auto recipe = recipeWith(
      "all", {{"points", OutputType::kObject, sdk::BuiltinObjectType::kPointCloud},
              {"scene", OutputType::kObject, sdk::BuiltinObjectType::kSceneEntities},
              {"annotations", OutputType::kObject, sdk::BuiltinObjectType::kImageAnnotations},
              {"image", OutputType::kObject, sdk::BuiltinObjectType::kImage},
              {"depth", OutputType::kObject, sdk::BuiltinObjectType::kDepthImage},
              {"camera", OutputType::kObject, sdk::BuiltinObjectType::kCameraInfo},
              {"voxels", OutputType::kObject, sdk::BuiltinObjectType::kVoxelGrid}});
  const auto& node = fx.install(recipe);
  fx.history.noteRequest(node.recipe.key(), 20 * kSec);
  const Status published =
      fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 1), unavailableBundle(recipe, "x"));
  ASSERT_TRUE(published.has_value()) << published.error();
  const Timestamp t = 20 * kSec;
  EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, fx.topic("owner/all/points"), t).timestamp_ns, t);
  const auto scene = decodeAt<sdk::SceneEntities>(fx.store, fx.topic("owner/all/scene"), t);
  ASSERT_EQ(scene.deletions.size(), 1u);
  EXPECT_EQ(scene.deletions[0].timestamp, t);
  EXPECT_EQ(decodeAt<sdk::ImageAnnotations>(fx.store, fx.topic("owner/all/annotations"), t).timestamp, t);
  const auto image = decodeAt<sdk::Image>(fx.store, fx.topic("owner/all/image"), t);
  EXPECT_EQ(image.width, 0u);
  EXPECT_EQ(image.timestamp_ns, t);
  const auto depth = decodeAt<sdk::DepthImage>(fx.store, fx.topic("owner/all/depth"), t);
  EXPECT_EQ(depth.width, 0u);
  EXPECT_EQ(depth.timestamp_ns, t);
  EXPECT_EQ(decodeAt<sdk::CameraInfo>(fx.store, fx.topic("owner/all/camera"), t).timestamp_ns, t);
  const auto voxels = decodeAt<sdk::VoxelGrid>(fx.store, fx.topic("owner/all/voxels"), t);
  EXPECT_EQ(voxels.column_count, 0u);
  EXPECT_EQ(voxels.timestamp_ns, t);
}

// ---- PUB: derived topics --------------------------------------------------------------------------

// PUB-01: a new revision keeps the topic id and drops the previous revision's entries; the same
// revision again changes nothing; forgetting the node retires the topic.
TEST(ResultHistoryTest, ReinstallAndForgetClearPreviousEntries) {
  Fixture fx;
  auto recipe = passthroughRecipe();
  const auto& node = fx.install(recipe);
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  fx.publishAt(node, 20 * kSec, cloudBundle(2));
  fx.publishAt(node, 5 * kSec, cloudBundle(2));
  ASSERT_EQ(fx.store.entryCount(topic), 2u);

  const auto again = fx.history.install(node);
  ASSERT_TRUE(again.has_value());
  EXPECT_TRUE(again->empty());
  EXPECT_EQ(fx.store.entryCount(topic), 2u) << "the same revision keeps its results";

  recipe.params_json = R"({"zmin": 0.5})";
  const uint64_t previous_revision = node.revision;
  fx.install(recipe);
  ASSERT_NE(node.revision, previous_revision);
  EXPECT_EQ(fx.topic("owner/crop/cropped"), topic);
  EXPECT_EQ(fx.store.entryCount(topic), 0u);
  EXPECT_FALSE(fx.history.lookup(node.recipe.key(), keyOf(node, 20 * kSec), 20 * kSec, 20 * kSec, 0).has_value());
  fx.publishAt(node, 20 * kSec, cloudBundle(1));
  EXPECT_EQ(fx.store.entryCount(topic), 1u);

  const auto retired = fx.history.forget(node.recipe.key());
  EXPECT_EQ(retired, std::vector<ObjectTopicId>{topic});
  EXPECT_FALSE(fx.store.findTopic(kDataset, "owner/crop/cropped").has_value());
  EXPECT_EQ(fx.history.ringBytes(), 0u);
  EXPECT_EQ(fx.history.cacheBytes(), 0u);
}

// PUB-02: removal retires every object topic of the node and returns them; an edit retires none; a
// second removal has nothing to retire. Number and string outputs never get a topic.
TEST(ResultHistoryTest, RemovalRetiresObjectTopicsButEditKeepsThem) {
  Fixture fx;
  auto recipe = cropRecipe();
  fx.install(recipe);
  const ObjectTopicId cropped = fx.topic("owner/crop/cropped");
  const ObjectTopicId witness = fx.topic("owner/crop/witness");
  EXPECT_EQ(fx.store.listTopics(kDataset).size(), 2u);

  recipe.params_json = R"({"zmin": 0.5})";
  ASSERT_TRUE(fx.graph.upsert(recipe).has_value());
  const auto edited = fx.history.install(*fx.graph.find(recipe.key()));
  ASSERT_TRUE(edited.has_value());
  EXPECT_TRUE(edited->empty());

  auto retired = fx.history.forget(recipe.key());
  std::sort(retired.begin(), retired.end(), [](auto lhs, auto rhs) { return lhs.id < rhs.id; });
  EXPECT_EQ(retired, (std::vector<ObjectTopicId>{cropped, witness}));
  EXPECT_TRUE(fx.store.listTopics(kDataset).empty());
  EXPECT_TRUE(fx.history.forget(recipe.key()).empty());
}

// PUB-03: an edit that drops or retypes an output retires that output's topic only.
TEST(ResultHistoryTest, EditRetiresTopicsOfDroppedOutputsAndKeepsTheRest) {
  Fixture fx;
  auto recipe = recipeWith("edit", {croppedSpec(), witnessSpec()});
  fx.install(recipe);
  const ObjectTopicId cropped = fx.topic("owner/edit/cropped");
  const ObjectTopicId witness = fx.topic("owner/edit/witness");

  recipe.outputs = {croppedSpec()};
  ASSERT_TRUE(fx.graph.upsert(recipe).has_value());
  auto retired = fx.history.install(*fx.graph.find(recipe.key()));
  ASSERT_TRUE(retired.has_value());
  EXPECT_EQ(*retired, std::vector<ObjectTopicId>{witness});
  EXPECT_FALSE(fx.store.findTopic(kDataset, "owner/edit/witness").has_value());
  EXPECT_EQ(fx.topic("owner/edit/cropped"), cropped);

  recipe.outputs = {{"cropped", OutputType::kObject, sdk::BuiltinObjectType::kSceneEntities}};
  ASSERT_TRUE(fx.graph.upsert(recipe).has_value());
  retired = fx.history.install(*fx.graph.find(recipe.key()));
  ASSERT_TRUE(retired.has_value());
  EXPECT_EQ(*retired, std::vector<ObjectTopicId>{cropped});
  const ObjectTopicId retyped = fx.topic("owner/edit/cropped");
  EXPECT_NE(retyped, cropped);
  EXPECT_EQ(fx.store.descriptor(retyped).metadata_json, derivedTopicMetadata(sdk::BuiltinObjectType::kSceneEntities));
}

// A same-named topic the node does not own (other metadata) is refused, never adopted nor retired; the
// node cannot publish until an install succeeds.
TEST(ResultHistoryTest, InstallRefusesATopicItDoesNotOwn) {
  Fixture fx;
  const auto foreign = fx.store.registerTopic(
      ObjectTopicDescriptor{kDataset, "owner/crop/cropped", R"({"builtin_object_type":"kPointCloud"})"});
  ASSERT_TRUE(foreign.has_value());
  ASSERT_TRUE(fx.graph.upsert(passthroughRecipe()).has_value());
  const RecipeGraph::Node& node = *fx.graph.find("owner/crop");
  EXPECT_FALSE(fx.history.install(node).has_value());
  EXPECT_FALSE(fx.history.publish(node, kSec, kSec, keyOf(node, 1), cloudBundle(1)).has_value());
  EXPECT_TRUE(fx.history.forget("owner/crop").empty());
  EXPECT_EQ(fx.store.findTopic(kDataset, "owner/crop/cropped"), *foreign);
}

// A topic of the node's name and kind already in the store is reused, as `findOrRegisterTopic` reuses it.
TEST(ResultHistoryTest, InstallReusesATopicOfItsOwnKind) {
  Fixture fx;
  const auto existing = fx.store.registerTopic(
      ObjectTopicDescriptor{kDataset, "owner/crop/cropped", derivedTopicMetadata(sdk::BuiltinObjectType::kPointCloud)});
  ASSERT_TRUE(existing.has_value());
  fx.install(passthroughRecipe());
  EXPECT_EQ(fx.topic("owner/crop/cropped"), *existing);
}

// An edit whose new output a foreign topic holds the name of fails before anything changes: the outputs it
// would drop keep their topics, every entry stays, the node's results still serve, and the error names
// the output.
TEST(ResultHistoryTest, InstallConflictLeavesEveryPreviousOutputUntouched) {
  Fixture fx;
  auto recipe = cropRecipe();
  const auto& node = fx.install(recipe);
  const ObjectTopicId cropped = fx.topic("owner/crop/cropped");
  const ObjectTopicId witness = fx.topic("owner/crop/witness");
  fx.publishAt(node, 10 * kSec, cropBundle(2, 1.2));
  const Key previous = keyOf(node, 10 * kSec);
  const size_t ring = fx.history.ringBytes();
  const size_t cache = fx.history.cacheBytes();
  const auto foreign = fx.store.registerTopic(
      ObjectTopicDescriptor{kDataset, "owner/crop/extra", R"({"builtin_object_type":"kPointCloud"})"});
  ASSERT_TRUE(foreign.has_value());

  recipe.outputs = {croppedSpec(), {"extra", OutputType::kObject, sdk::BuiltinObjectType::kPointCloud}};
  ASSERT_TRUE(fx.graph.upsert(recipe).has_value());
  const auto edited = fx.history.install(node);
  ASSERT_FALSE(edited.has_value());
  EXPECT_NE(edited.error().find("'owner/crop/extra'"), std::string::npos) << edited.error();

  EXPECT_EQ(fx.store.findTopic(kDataset, "owner/crop/witness"), witness) << "the output it drops is not retired";
  EXPECT_EQ(fx.store.findTopic(kDataset, "owner/crop/cropped"), cropped);
  EXPECT_EQ(fx.store.findTopic(kDataset, "owner/crop/extra"), *foreign);
  EXPECT_EQ(fx.store.entryCount(cropped), 1u);
  EXPECT_EQ(fx.store.entryCount(witness), 1u);
  EXPECT_EQ(fx.history.ringBytes(), ring);
  EXPECT_EQ(fx.history.cacheBytes(), cache);
  EXPECT_TRUE(fx.history.lookup(node.recipe.key(), previous, 10 * kSec, 10 * kSec, 0).has_value());
  EXPECT_FALSE(fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 20), cloudBundle(1)).has_value())
      << "the new revision is not installed";
}

// ---- MAT: the window, the budget, the bundle ------------------------------------------------------

// MAT-01: entries farther than the window from the requested time are trimmed, also by a publication
// that writes nothing new; the cache and the ledger follow, so a revisit computes and lands again.
TEST(ResultHistoryTest, MaterializationWindowTrimsFarEntriesAndRepublishesRevisits) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  for (Timestamp sec = 0; sec <= 100; sec += 5) {
    fx.publishAt(node, sec * kSec, cloudBundle(1));
    EXPECT_GE(fx.store.timeRange(topic).first, (sec - 30) * kSec);
    EXPECT_LE(fx.store.entryCount(topic), 7u);
  }
  // A distant jump whose inputs resolve to the 100 s result: nothing new, but the window moves.
  fx.history.noteRequest(node.recipe.key(), 150 * kSec);
  ASSERT_TRUE(fx.history.publish(node, 150 * kSec, 100 * kSec, keyOf(node, 100 * kSec), cloudBundle(1)));
  EXPECT_EQ(timestampsOf(fx.store, topic), std::vector<Timestamp>{100 * kSec});

  EXPECT_FALSE(fx.history.lookup(node.recipe.key(), keyOf(node, 95 * kSec), 95 * kSec, 95 * kSec, 0).has_value());
  fx.publishAt(node, 95 * kSec, cloudBundle(1));
  EXPECT_EQ(fx.store.entryCount(topic), 2u) << "no stale dedup record for a trimmed entry";
}

// MAT-02 (changed, N7): a dense window is never refused; past the ring budget the result farthest from
// the cursor goes.
TEST(ResultHistoryTest, RingEvictsTheOldestInsteadOfRefusingADenseWindow) {
  const size_t one = serializedSize(sdk::BuiltinObject(makeCloud(1)));
  Fixture fx(Limits{.ring_bytes = 2 * one});
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  fx.publishAt(node, 5 * kSec, cloudBundle(1));
  fx.publishAt(node, 10 * kSec, cloudBundle(1));
  fx.publishAt(node, 20 * kSec, cloudBundle(1));
  EXPECT_EQ(timestampsOf(fx.store, topic), (std::vector<Timestamp>{10 * kSec, 20 * kSec}));
  EXPECT_EQ(fx.history.ringBytes(), 2 * one);

  fx.publishAt(node, 100 * kSec, cloudBundle(1));
  EXPECT_EQ(timestampsOf(fx.store, topic), std::vector<Timestamp>{100 * kSec});
}

// MAT-03 (changed, N7): one global budget; an oversized result is not refused, the displayed one is
// never evicted, and the excess is reported.
TEST(ResultHistoryTest, GlobalBudgetNeverRefusesAnOversizedResultNorEvictsTheDisplayedOne) {
  const size_t one = serializedSize(sdk::BuiltinObject(makeCloud(1)));
  Fixture fx(Limits{.ring_bytes = one - 1});
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  fx.publishAt(node, 10 * kSec, cloudBundle(1));
  EXPECT_EQ(fx.store.entryCount(topic), 1u);
  EXPECT_EQ(fx.history.protectedExcessBytes(), 1u);

  fx.publishAt(node, 11 * kSec, cloudBundle(1));
  EXPECT_EQ(timestampsOf(fx.store, topic), std::vector<Timestamp>{11 * kSec}) << "the newly displayed one stays";
  EXPECT_EQ(fx.history.protectedExcessBytes(), 1u);
}

// MAT-04: the window and the eviction distance saturate at the Timestamp limits.
TEST(ResultHistoryTest, MaterializationWindowSaturatesAtTimestampLimits) {
  {
    // A non-current neighbour next to each limit stays in the window: the window saturates, never wraps.
    Fixture fx;
    const auto& node = fx.install(passthroughRecipe());
    const ObjectTopicId topic = fx.topic("owner/crop/cropped");
    fx.publishAt(node, kMin + 1, cloudBundle(1));
    fx.publishAt(node, kMin + 2, cloudBundle(1));
    EXPECT_EQ(timestampsOf(fx.store, topic), (std::vector<Timestamp>{kMin + 1, kMin + 2}));
    fx.publishAt(node, kMax - 1, cloudBundle(1));
    fx.publishAt(node, kMax, cloudBundle(1));
    EXPECT_EQ(timestampsOf(fx.store, topic), (std::vector<Timestamp>{kMax - 1, kMax}));
  }
  {
    Fixture fx(Limits{.window = kMax});
    const auto& node = fx.install(passthroughRecipe());
    const ObjectTopicId topic = fx.topic("owner/crop/cropped");
    fx.publishAt(node, kMin + 1, cloudBundle(1));
    fx.publishAt(node, 20 * kSec, cloudBundle(1));
    fx.history.noteRequest(node.recipe.key(), kMax);
    ASSERT_TRUE(fx.history.publish(node, kMax, 20 * kSec, keyOf(node, 20 * kSec), cloudBundle(1)));
    EXPECT_EQ(timestampsOf(fx.store, topic), std::vector<Timestamp>{20 * kSec});
  }
  {
    // A result at the bottom of the range and its node's cursor at the top are 2^64 - 2 apart: farther
    // than any signed difference can say, and the farthest result of all.
    const size_t one = serializedSize(sdk::BuiltinObject(makeCloud(1)));
    Fixture fx(Limits{.ring_bytes = 4 * one - 1});
    const auto& low = fx.install(passthroughRecipe("low"));
    const auto& nearby = fx.install(passthroughRecipe("nearby"));
    fx.publishAt(low, kMin + 1, cloudBundle(1));
    fx.publishAt(low, kMin + 2, cloudBundle(1));
    fx.history.noteRequest(low.recipe.key(), kMax);
    fx.publishAt(nearby, 0, cloudBundle(1));
    // The pass below has both candidates: low's older result and nearby's.
    ASSERT_EQ(timestampsOf(fx.store, fx.topic("owner/low/cropped")), (std::vector<Timestamp>{kMin + 1, kMin + 2}));
    ASSERT_EQ(fx.store.entryCount(fx.topic("owner/nearby/cropped")), 1u);
    fx.publishAt(nearby, kSec, cloudBundle(1));
    EXPECT_EQ(timestampsOf(fx.store, fx.topic("owner/low/cropped")), std::vector<Timestamp>{kMin + 2});
    EXPECT_EQ(fx.store.entryCount(fx.topic("owner/nearby/cropped")), 2u);
  }
}

// MAT-05 (changed, N7): the outputs of one result land together or not at all. A serialization
// failure (an output of the wrong type) or a staging failure (a topic gone) leaves every output's
// previous entry, and the history, as they were.
TEST(ResultHistoryTest, BundlePublicationIsAllOrNothing) {
  Fixture fx;
  const auto& node = fx.install(recipeWith("crop", {croppedSpec(), witnessSpec()}));
  const ObjectTopicId cropped = fx.topic("owner/crop/cropped");
  const ObjectTopicId witness = fx.topic("owner/crop/witness");
  ScriptBundle first;
  first.outputs = {
      {.name = "cropped", .value = sdk::BuiltinObject(makeCloud(2))},
      {.name = "witness", .value = sdk::BuiltinObject(makeScene())}};
  fx.publishAt(node, 10 * kSec, first);
  const auto uid = fx.store.latestAt(cropped, kMax)->sequential_uid;
  const size_t ring = fx.history.ringBytes();

  ScriptBundle mistyped = first;
  mistyped.outputs[1].value = sdk::BuiltinObject(makeCloud(1));
  EXPECT_FALSE(fx.history.publish(node, 100 * kSec, 100 * kSec, keyOf(node, 100), mistyped).has_value());
  EXPECT_EQ(fx.store.entryCount(cropped), 1u);
  EXPECT_EQ(fx.store.entryCount(witness), 1u);
  EXPECT_EQ(fx.store.latestAt(cropped, kMax)->sequential_uid, uid);
  EXPECT_FALSE(fx.history.lookup(node.recipe.key(), keyOf(node, 100), 100 * kSec, 100 * kSec, 0).has_value());

  fx.store.removeTopic(witness);
  EXPECT_FALSE(fx.history.publish(node, 100 * kSec, 100 * kSec, keyOf(node, 100), first).has_value());
  EXPECT_EQ(fx.store.entryCount(cropped), 1u);
  EXPECT_EQ(fx.store.latestAt(cropped, kMax)->sequential_uid, uid);
  EXPECT_EQ(fx.history.ringBytes(), ring);
}

// A payload serialized off the GUI thread (`serializeOutputs`) is moved into the ring, not serialized again.
TEST(ResultHistoryTest, APreparedPayloadIsMovedIntoTheRing) {
  Fixture fx;
  const auto recipe = cropRecipe();
  const auto& node = fx.install(recipe);
  const ScriptBundle bundle = cropBundle(3, 1.0);
  ResultPayloads payloads = ResultHistory::serializeOutputs(recipe.outputs, bundle);
  ASSERT_EQ(payloads.size(), 2u) << "the object outputs only";
  ASSERT_TRUE(payloads[0].has_value() && payloads[1].has_value());
  const uint8_t* cropped = payloads[0]->data();
  fx.history.noteRequest(node.recipe.key(), 10 * kSec);
  ASSERT_TRUE(fx.history.publish(
      node, 10 * kSec, 10 * kSec, keyOf(node, 1), bundle, {}, ResultPublishOptions{.payloads = std::move(payloads)}));
  EXPECT_EQ(fx.store.latestAt(fx.topic("owner/crop/cropped"), kMax)->payload.bytes.data(), cropped);
  EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, fx.topic("owner/crop/cropped"), 10 * kSec).width, 3u);
}

// ---- What the scheduler reads --------------------------------------------------------------------

// resultAt is what a downstream input takes: the newest stored result at or before `t` whose entries are
// still in the store, by its real stamp (a finding before it was computed is none), with an unavailable
// output's reason.
TEST(ResultHistoryTest, ResultAtIsTheNewestStoredOutputAtOrBefore) {
  Fixture fx;
  const auto recipe = recipeWith("crop", {croppedSpec(), witnessSpec()});
  const auto& node = fx.install(recipe);
  const ObjectTopicId cropped = fx.topic("owner/crop/cropped");
  ScriptBundle drawn;
  drawn.outputs = {
      {.name = "cropped", .value = sdk::BuiltinObject(makeCloud(2))},
      {.name = "witness", .value = sdk::BuiltinObject(makeScene())}};
  fx.publishAt(node, 10 * kSec, drawn);
  fx.history.noteRequest(node.recipe.key(), 20 * kSec);
  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 20), unavailableBundle(recipe, "no points")));

  const auto at15 = fx.history.resultAt(node.recipe.key(), "cropped", 15 * kSec);
  ASSERT_TRUE(at15.has_value());
  EXPECT_EQ(at15->stamp, 10 * kSec);
  EXPECT_EQ(at15->topic, cropped);
  EXPECT_EQ(at15->uid, fx.store.latestAt(cropped, 15 * kSec)->sequential_uid);
  EXPECT_FALSE(at15->unavailable.has_value());
  const auto at25 = fx.history.resultAt(node.recipe.key(), "witness", 25 * kSec);
  ASSERT_TRUE(at25.has_value());
  EXPECT_EQ(at25->stamp, 20 * kSec);
  EXPECT_EQ(at25->unavailable, std::optional<std::string>("no points"));
  EXPECT_FALSE(fx.history.resultAt(node.recipe.key(), "cropped", 5 * kSec).has_value()) << "nothing at or before";
  EXPECT_FALSE(fx.history.resultAt(node.recipe.key(), "max_z", 15 * kSec).has_value()) << "no such object output";
  EXPECT_FALSE(fx.history.resultAt("owner/other", "cropped", 15 * kSec).has_value());
  prune(fx.store, fx.topic("owner/crop/witness"));
  EXPECT_FALSE(fx.history.resultAt(node.recipe.key(), "cropped", 25 * kSec).has_value())
      << "a result missing an entry is not stored any more";

  auto pinned = passthroughRecipe("pin");
  pinned.pinned_t = 12 * kSec;
  const auto& finding = fx.install(pinned);
  ASSERT_TRUE(fx.history.publish(finding, 12 * kSec, 12 * kSec, keyOf(finding, 1), cloudBundle(1, 12 * kSec)));
  EXPECT_FALSE(fx.history.resultAt("owner/pin", "cropped", 11 * kSec).has_value()) << "not computed yet";
  const auto found = fx.history.resultAt("owner/pin", "cropped", 13 * kSec);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->stamp, 12 * kSec) << "the real stamp, never kFindingTimestamp";
}

// shownAt is what the ring shows of a node at `t`, and whether an output draws there.
TEST(ResultHistoryTest, ShownAtSaysWhatTheRingShows) {
  Fixture fx;
  const auto recipe = passthroughRecipe();
  const auto& node = fx.install(recipe);
  fx.publishAt(node, 10 * kSec, cloudBundle(1));
  fx.history.noteRequest(node.recipe.key(), 20 * kSec);
  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 20), unavailableBundle(recipe, "x")));

  EXPECT_FALSE(fx.history.shownAt(node.recipe.key(), 5 * kSec).has_value());
  const auto drawn = fx.history.shownAt(node.recipe.key(), 15 * kSec);
  ASSERT_TRUE(drawn.has_value());
  EXPECT_EQ(drawn->stamp, 10 * kSec);
  EXPECT_TRUE(drawn->available);
  const auto cleared = fx.history.shownAt(node.recipe.key(), 25 * kSec);
  ASSERT_TRUE(cleared.has_value());
  EXPECT_EQ(cleared->stamp, 20 * kSec);
  EXPECT_FALSE(cleared->available) << "a tombstone draws nothing";
  EXPECT_FALSE(fx.history.shownAt("owner/other", 15 * kSec).has_value());

  auto pinned = passthroughRecipe("pin");
  pinned.pinned_t = 12 * kSec;
  const auto& finding = fx.install(pinned);
  ASSERT_TRUE(fx.history.publish(finding, 12 * kSec, 12 * kSec, keyOf(finding, 1), cloudBundle(1, 12 * kSec)));
  EXPECT_FALSE(fx.history.shownAt("owner/pin", 11 * kSec).has_value());
  const auto computed = fx.history.shownAt("owner/pin", 13 * kSec);
  ASSERT_TRUE(computed.has_value());
  EXPECT_EQ(computed->stamp, 12 * kSec) << "the real stamp";
}

// A result that serves the request at `t` removes the tombstones (results with no input identity) stamped
// after it up to `t`, which would hide it; never a result, nor a tombstone past `t`.
TEST(ResultHistoryTest, SupersedeTombstonesDropsOnlyTheTombstonesItWouldHide) {
  Fixture fx;
  const auto recipe = passthroughRecipe();
  const auto& node = fx.install(recipe);
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  const Key tombstone{.revision = node.revision};
  fx.history.noteRequest(node.recipe.key(), 30 * kSec);
  ASSERT_TRUE(fx.history.publish(node, 30 * kSec, 10 * kSec, keyOf(node, 10), cloudBundle(1)));
  for (const Timestamp sec : {12, 16, 40}) {
    ASSERT_TRUE(fx.history.publish(node, 30 * kSec, sec * kSec, tombstone, unavailableBundle(recipe, "missing")));
  }
  ASSERT_TRUE(fx.history.publish(node, 30 * kSec, 14 * kSec, keyOf(node, 14), cloudBundle(1)));
  const size_t ring = fx.history.ringBytes();

  EXPECT_TRUE(fx.history.supersedeTombstones(node.recipe.key(), 10 * kSec, 30 * kSec));
  EXPECT_EQ(timestampsOf(fx.store, topic), (std::vector<Timestamp>{10 * kSec, 14 * kSec, 40 * kSec}));
  EXPECT_LT(fx.history.ringBytes(), ring);
  EXPECT_FALSE(fx.history.supersedeTombstones(node.recipe.key(), 10 * kSec, 30 * kSec)) << "none left";
  EXPECT_FALSE(fx.history.supersedeTombstones("owner/other", 10 * kSec, 30 * kSec));
}

// clear drops one node's results, in the store and in the cache, and keeps its topics; another node's stay.
TEST(ResultHistoryTest, ClearDropsOneNodesResultsAndKeepsItsTopics) {
  Fixture fx;
  const auto& first = fx.install(passthroughRecipe("first"));
  const auto& second = fx.install(passthroughRecipe("second"));
  const ObjectTopicId first_topic = fx.topic("owner/first/cropped");
  fx.publishAt(first, 10 * kSec, cloudBundle(1));
  fx.publishAt(second, 10 * kSec, cloudBundle(1));
  const size_t second_ring = serializedSize(sdk::BuiltinObject(makeCloud(1)));

  fx.history.clear("owner/first");
  EXPECT_EQ(fx.store.entryCount(first_topic), 0u);
  EXPECT_EQ(fx.topic("owner/first/cropped"), first_topic);
  EXPECT_FALSE(fx.history.lookup("owner/first", keyOf(first, 10 * kSec), 10 * kSec, 10 * kSec, 0).has_value());
  EXPECT_EQ(fx.store.entryCount(fx.topic("owner/second/cropped")), 1u);
  EXPECT_EQ(fx.history.ringBytes(), second_ring);
  EXPECT_TRUE(fx.history.lookup("owner/second", keyOf(second, 10 * kSec), 10 * kSec, 10 * kSec, 0).has_value());
  fx.history.clear("owner/other");  // an unknown node: nothing to drop
}

// A cached bundle releases its outputs before the keepalives published with it: an object a parser made
// (an input passed through to an output) is destroyed while its library is still loaded.
TEST(ResultHistoryTest, ACachedBundleReleasesItsOutputsBeforeTheirKeepalives) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  fx.publishAt(node, 10 * kSec, cloudBundle(1));  // the ring's entry at 10 s: the result below is only cached
  auto released = std::make_shared<std::vector<std::string>>();
  {
    sdk::PointCloud cloud = makeCloud(1);
    cloud.anchor = std::shared_ptr<const void>(new sdk::BufferAnchor(cloud.anchor), [released](const void* held) {
      released->push_back("object");
      delete static_cast<const sdk::BufferAnchor*>(held);
    });
    std::vector<std::shared_ptr<const void>> keepalives{
        std::shared_ptr<const void>(new int(0), [released](const void* held) {
          released->push_back("library");
          delete static_cast<const int*>(held);
        })};
    ScriptBundle bundle;
    bundle.outputs.push_back({.name = "cropped", .value = sdk::BuiltinObject(std::move(cloud))});
    ASSERT_TRUE(fx.history.publish(
        node, 10 * kSec, 10 * kSec, keyOf(node, 2), std::move(bundle), std::move(keepalives),
        ResultPublishOptions{.cache_only = true}));
  }
  EXPECT_TRUE(released->empty()) << "the cache holds both";
  fx.history.forget(node.recipe.key());
  EXPECT_EQ(*released, (std::vector<std::string>{"object", "library"}));
}

// MAT-06: a different result at the same stamp replaces the entry; an external eviction leaves no
// stale dedup record, so the next publication of the same key lands again.
TEST(ResultHistoryTest, SameStampReplacementAndExternalEvictionDoNotLeaveStaleDedup) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 1), cloudBundle(1)));
  const auto original = fx.store.latestAt(topic, kMax)->sequential_uid;

  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 2), cloudBundle(2)));
  EXPECT_EQ(fx.store.entryCount(topic), 1u);
  EXPECT_NE(fx.store.latestAt(topic, kMax)->sequential_uid, original);

  std::vector<OwnedObjectWrite> evict(1);
  evict[0].topic = topic;
  evict[0].append = false;
  evict[0].drop = [](Timestamp, SequentialUID) { return true; };
  ASSERT_TRUE(fx.store.pushOwnedBundle(std::move(evict)).has_value());
  ASSERT_EQ(fx.store.entryCount(topic), 0u);

  // The cache kept no decoded copy of it: a miss, and the next publication of the same key lands again.
  EXPECT_FALSE(fx.history.lookup(node.recipe.key(), keyOf(node, 2), 20 * kSec, 20 * kSec, 0).has_value());
  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 2), cloudBundle(2)));
  EXPECT_EQ(fx.store.entryCount(topic), 1u);
}

// ---- CACHE: identity and observations -------------------------------------------------------------

// CACHE-01: another dataset epoch is another result.
TEST(ResultHistoryTest, EpochInvalidatesTheCache) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  fx.publishAt(node, 20 * kSec, cloudBundle(1));
  Key reloaded = keyOf(node, 20 * kSec);
  reloaded.epoch = 1;
  EXPECT_FALSE(fx.history.lookup(node.recipe.key(), reloaded, 20 * kSec, 20 * kSec, 0).has_value());
  EXPECT_TRUE(fx.history.lookup(node.recipe.key(), keyOf(node, 20 * kSec), 20 * kSec, 20 * kSec, 0).has_value());
}

// CACHE-02, CACHE-03: a run that never read the query clock (input-clock mode never does) is one
// result at every instant its inputs resolve the same: one publication, one entry.
TEST(ResultHistoryTest, AnUnobservedClockReusesOneResultAcrossInstants) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  const Key request = keyOf(node, 10);
  ASSERT_TRUE(fx.history.publish(node, 10 * kSec, 10 * kSec, request, cloudBundle(1)));
  const auto uid = fx.store.latestAt(topic, kMax)->sequential_uid;
  for (int i = 1; i < 30; ++i) {
    const Timestamp t = 10 * kSec + i * (5 * kSec / 30);
    fx.history.noteRequest(node.recipe.key(), t);
    const auto hit = fx.history.lookup(node.recipe.key(), request, 10 * kSec, t, 0);
    ASSERT_TRUE(hit.has_value() && hit->bundle != nullptr) << "instant " << i;
    ASSERT_TRUE(fx.history.publish(node, t, hit->stamp, request, *hit->bundle));
  }
  EXPECT_EQ(fx.store.entryCount(topic), 1u);
  EXPECT_EQ(fx.store.latestAt(topic, kMax)->sequential_uid, uid) << "no republication at the same stamp";

  Key observed = request;
  observed.query_clock = 12 * kSec;
  const auto& clocked = fx.install(passthroughRecipe("clocked"));
  observed.revision = clocked.revision;
  ASSERT_TRUE(fx.history.publish(clocked, 12 * kSec, 10 * kSec, observed, cloudBundle(1)));
  Key request_clocked = request;
  request_clocked.revision = clocked.revision;
  EXPECT_FALSE(fx.history.lookup("owner/clocked", request_clocked, 10 * kSec, 13 * kSec, 0).has_value());
}

// CACHE-04: a run that read the clock is keyed by its instant. The ring holds one result per stamp, so the
// 13 s result replaces the 12 s one at their shared stamp, and the cache, which kept no decoded copy of
// it, lets it go too: back at 12 s is a miss.
TEST(ResultHistoryTest, ReadingTheClockKeysPerInstant) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  const Key request = keyOf(node, 10);
  Key at12 = request;
  at12.query_clock = 12 * kSec;
  ASSERT_TRUE(fx.history.publish(node, 12 * kSec, 10 * kSec, at12, cloudBundle(1)));
  EXPECT_FALSE(fx.history.lookup(node.recipe.key(), request, 10 * kSec, 13 * kSec, 0).has_value());
  Key at13 = request;
  at13.query_clock = 13 * kSec;
  ASSERT_TRUE(fx.history.publish(node, 13 * kSec, 10 * kSec, at13, cloudBundle(2)));

  EXPECT_TRUE(fx.history.lookup(node.recipe.key(), request, 10 * kSec, 13 * kSec, 0).has_value());
  EXPECT_FALSE(fx.history.lookup(node.recipe.key(), request, 10 * kSec, 12 * kSec, 0).has_value())
      << "replaced in the ring at its stamp";
}

// CACHE-05: a run that read the TF tree at an explicit time observed the TF revision, not the clock:
// a hit at every instant until the TF changes.
TEST(ResultHistoryTest, ObservingOnlyTheTfKeysByTheTfRevision) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  const Key request = keyOf(node, 20);
  Key observed = request;
  observed.tf_revision = 7;
  ASSERT_TRUE(fx.history.publish(node, 21 * kSec, 20 * kSec, observed, cloudBundle(1)));
  EXPECT_TRUE(fx.history.lookup(node.recipe.key(), request, 20 * kSec, 22 * kSec, 7).has_value());
  EXPECT_FALSE(fx.history.lookup(node.recipe.key(), request, 20 * kSec, 22 * kSec, 8).has_value());
}

// CACHE-06: observation is per run: a result that observed nothing stays timeless next to results of
// other inputs that are keyed per instant.
TEST(ResultHistoryTest, ObservationIsPerRun) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  const Key small = keyOf(node, 5);
  const Key big = keyOf(node, 10);
  ASSERT_TRUE(fx.history.publish(node, 6 * kSec, 5 * kSec, small, cloudBundle(2)));
  EXPECT_TRUE(fx.history.lookup(node.recipe.key(), small, 5 * kSec, 7 * kSec, 0).has_value());

  Key big_at11 = big;
  big_at11.query_clock = 11 * kSec;
  ASSERT_TRUE(fx.history.publish(node, 11 * kSec, 10 * kSec, big_at11, cloudBundle(3)));
  EXPECT_FALSE(fx.history.lookup(node.recipe.key(), big, 10 * kSec, 12 * kSec, 0).has_value());

  const auto timeless = fx.history.lookup(node.recipe.key(), small, 5 * kSec, 8 * kSec, 0);
  ASSERT_TRUE(timeless.has_value());
  EXPECT_TRUE(timeless->in_ring);
  EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, fx.topic("owner/crop/cropped"), 8 * kSec).width, 2u);
}

// A cached result serves only a request at its own stamp: when the cache lost the 10 s tombstone, a lookup at
// 10 s for 20 s answers from the ring, never with the 20 s tombstone that shares its key.
TEST(ResultHistoryTest, ACachedResultServesOnlyItsOwnStamp) {
  const auto recipe = passthroughRecipe();
  size_t one = 0;
  {
    Fixture probe;
    const auto& node = probe.install(recipe);
    probe.history.noteRequest(node.recipe.key(), 20 * kSec);
    ASSERT_TRUE(probe.history.publish(
        node, 20 * kSec, 10 * kSec, Key{.revision = node.revision}, unavailableBundle(recipe, "missing")));
    one = probe.history.cacheBytes();
  }
  Fixture fx(Limits{.cache_bytes = one});
  const auto& node = fx.install(recipe);
  const Key key{.revision = node.revision};
  fx.history.noteRequest(node.recipe.key(), 20 * kSec);
  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 10 * kSec, key, unavailableBundle(recipe, "at 10 s")));
  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 20 * kSec, key, unavailableBundle(recipe, "at 20 s")));
  ASSERT_EQ(fx.history.cacheBytes(), one) << "the 10 s tombstone left the cache, the farthest from 20 s";

  const auto hit = fx.history.lookup(node.recipe.key(), key, 10 * kSec, 20 * kSec, 0);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->stamp, 10 * kSec);
  EXPECT_EQ(hit->bundle, nullptr) << "only the ring holds the 10 s result";
}

// Two results of one clock-reading key at different stamps share the cache's position (the instant they read):
// each is cached, and found, at its own stamp.
TEST(ResultHistoryTest, ClockReadingResultsOfOneKeyAreCachedPerStamp) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  Key at12 = keyOf(node, 1);
  at12.query_clock = 12 * kSec;
  fx.history.noteRequest(node.recipe.key(), 12 * kSec);
  ASSERT_TRUE(fx.history.publish(node, 12 * kSec, 11 * kSec, at12, cloudBundle(1)));
  ASSERT_TRUE(fx.history.publish(node, 12 * kSec, 10 * kSec, at12, cloudBundle(2)));
  const auto hit = fx.history.lookup(node.recipe.key(), keyOf(node, 1), 10 * kSec, 12 * kSec, 0);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->stamp, 10 * kSec);
  EXPECT_NE(hit->bundle, nullptr) << "the 10 s result has its own cache entry";
}

// ---- PIN: findings ---------------------------------------------------------------------------------

// PIN-02 (store half, changed: D26): a finding's single entry sits at kFindingTimestamp and keeps its
// stamp in the payload; the same result again writes nothing, a new one replaces it.
TEST(ResultHistoryTest, PinnedFindingSitsAtTheFindingTimestamp) {
  Fixture fx;
  auto recipe = passthroughRecipe();
  recipe.pinned_t = 20 * kSec;
  const auto& node = fx.install(recipe);
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 1), cloudBundle(2, 20 * kSec)));
  EXPECT_EQ(timestampsOf(fx.store, topic), std::vector<Timestamp>{kFindingTimestamp});
  EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, topic, kFindingTimestamp).timestamp_ns, 20 * kSec);
  const auto uid = fx.store.latestAt(topic, kMax)->sequential_uid;

  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 1), cloudBundle(2, 20 * kSec)));
  EXPECT_EQ(fx.store.latestAt(topic, kMax)->sequential_uid, uid);
  const auto stored = fx.history.lookup(node.recipe.key(), keyOf(node, 1), 20 * kSec, 20 * kSec, 0);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->stamp, 20 * kSec) << "the result time, not the store's";

  ASSERT_TRUE(fx.history.publish(node, 20 * kSec, 20 * kSec, keyOf(node, 2), cloudBundle(3, 20 * kSec)));
  EXPECT_EQ(timestampsOf(fx.store, topic), std::vector<Timestamp>{kFindingTimestamp});
  EXPECT_NE(fx.store.latestAt(topic, kMax)->sequential_uid, uid);
  EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, topic, 0).width, 3u);
}

// PIN-03: a finding's bytes count against the ring; past the budget they
// are the protected excess, never evicted.
TEST(ResultHistoryTest, FindingsAccountingCountsPinnedBytes) {
  const size_t one = serializedSize(sdk::BuiltinObject(makeCloud(2, 20 * kSec)));
  Fixture fx(Limits{.ring_bytes = one / 2});
  auto recipe = passthroughRecipe("pin");
  recipe.pinned_t = 20 * kSec;
  const auto& pin = fx.install(recipe);
  fx.install(passthroughRecipe("live"));
  ASSERT_TRUE(fx.history.publish(pin, 20 * kSec, 20 * kSec, keyOf(pin, 1), cloudBundle(2, 20 * kSec)));
  EXPECT_EQ(fx.history.ringBytes(), one);
  EXPECT_EQ(fx.history.protectedExcessBytes(), one - one / 2);
  EXPECT_EQ(fx.store.entryCount(fx.topic("owner/pin/cropped")), 1u);
}

// PIN-04 (store half, changed: D26): a finding is never trimmed by the window nor evicted by the
// budget, whatever the cursors; forgetting it (dismiss) removes it and its bytes.
TEST(ResultHistoryTest, PinnedFindingsAreNeverTrimmedNorEvicted) {
  const size_t one = serializedSize(sdk::BuiltinObject(makeCloud(1)));
  Fixture fx(Limits{.ring_bytes = 2 * one});
  auto recipe = passthroughRecipe("pin");
  recipe.pinned_t = 10 * kSec;
  const auto& pin = fx.install(recipe);
  const auto& live = fx.install(passthroughRecipe("live"));
  const ObjectTopicId finding = fx.topic("owner/pin/cropped");
  ASSERT_TRUE(fx.history.publish(pin, 10 * kSec, 10 * kSec, keyOf(pin, 1), cloudBundle(1)));
  ASSERT_TRUE(fx.history.publish(pin, 1000 * kSec, 10 * kSec, keyOf(pin, 1), cloudBundle(1)));
  for (Timestamp sec = 1; sec <= 5; ++sec) {
    fx.publishAt(live, sec * kSec, cloudBundle(1));
  }
  EXPECT_EQ(timestampsOf(fx.store, finding), std::vector<Timestamp>{kFindingTimestamp});
  EXPECT_EQ(fx.store.entryCount(fx.topic("owner/live/cropped")), 1u) << "the live node yields, the finding stays";

  fx.history.forget(pin.recipe.key());
  EXPECT_EQ(fx.history.ringBytes(), one) << "the live node's displayed result only";
  EXPECT_FALSE(fx.store.findTopic(kDataset, "owner/pin/cropped").has_value());
}

// GAP-01 (store half): an at-or-before read returns the finding at any instant, the sentinel itself
// included; the payload keeps the real stamp.
TEST(ResultHistoryTest, PinnedFindingIsVisibleAtAnyInstant) {
  Fixture fx;
  auto recipe = passthroughRecipe();
  const Timestamp stamp = 12 * kSec + 400'000'000;
  recipe.pinned_t = stamp;
  const auto& node = fx.install(recipe);
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  ASSERT_TRUE(fx.history.publish(node, stamp, stamp, keyOf(node, 1), cloudBundle(2, stamp)));
  for (const Timestamp t : {kFindingTimestamp, Timestamp{0}, 5 * kSec, stamp, kMax}) {
    const auto entry = fx.store.latestAt(topic, t);
    ASSERT_TRUE(entry.has_value()) << t;
    EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, topic, t).timestamp_ns, stamp);
  }
}

// ---- The ring and the cache under one rule ---------------------------------------------------------

// GAP-11: the ring evicts the result farthest from the cursor, not the oldest; a backward seek within
// the retained span is served from the ring.
TEST(ResultHistoryTest, RingEvictsFarthestFromCursor) {
  const size_t one = serializedSize(sdk::BuiltinObject(makeCloud(1)));
  Fixture fx(Limits{.ring_bytes = 4 * one, .cache_bytes = 1});
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  for (Timestamp sec = 1; sec <= 6; ++sec) {
    fx.publishAt(node, sec * kSec, cloudBundle(1));
  }
  EXPECT_EQ(timestampsOf(fx.store, topic), (std::vector<Timestamp>{3 * kSec, 4 * kSec, 5 * kSec, 6 * kSec}));

  fx.history.noteRequest(node.recipe.key(), 3 * kSec);
  const auto back = fx.history.lookup(node.recipe.key(), keyOf(node, 3 * kSec), 3 * kSec, 3 * kSec, 0);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->bundle, nullptr) << "the cache holds nothing at a 1-byte budget";

  // The cursor is now at 2 s: the farthest result is the newest.
  fx.publishAt(node, 2 * kSec, cloudBundle(1));
  EXPECT_EQ(timestampsOf(fx.store, topic), (std::vector<Timestamp>{2 * kSec, 3 * kSec, 4 * kSec, 5 * kSec}));
}

// GAP-41: one budget across nodes: the victim is the result farthest from its own node's cursor,
// never a node's displayed result, and nothing is refused.
TEST(ResultHistoryTest, GlobalBudgetEvictsFarthestAcrossNodes) {
  const size_t one = serializedSize(sdk::BuiltinObject(makeCloud(1)));
  Fixture fx(Limits{.ring_bytes = 4 * one});
  const auto& nearby = fx.install(passthroughRecipe("nearby"));
  const auto& distant = fx.install(passthroughRecipe("distant"));
  const ObjectTopicId nearby_topic = fx.topic("owner/nearby/cropped");
  const ObjectTopicId distant_topic = fx.topic("owner/distant/cropped");
  fx.publishAt(distant, 1 * kSec, cloudBundle(1));
  fx.publishAt(distant, 20 * kSec, cloudBundle(1));  // distant's cursor: 20 s, its 1 s result is 19 s away
  fx.publishAt(nearby, 9 * kSec, cloudBundle(1));
  fx.publishAt(nearby, 10 * kSec, cloudBundle(1));  // nearby's cursor: 10 s
  EXPECT_EQ(fx.history.ringBytes(), 4 * one);

  fx.publishAt(nearby, 11 * kSec, cloudBundle(1));
  EXPECT_EQ(timestampsOf(fx.store, distant_topic), std::vector<Timestamp>{20 * kSec});
  EXPECT_EQ(timestampsOf(fx.store, nearby_topic), (std::vector<Timestamp>{9 * kSec, 10 * kSec, 11 * kSec}));

  // Only displayed results left: each node keeps its own, the excess is reported.
  Fixture tight(Limits{.ring_bytes = one});
  const auto& first = tight.install(passthroughRecipe("first"));
  const auto& second = tight.install(passthroughRecipe("second"));
  tight.publishAt(first, 5 * kSec, cloudBundle(1));
  tight.publishAt(second, 5 * kSec, cloudBundle(1));
  EXPECT_EQ(tight.store.entryCount(tight.topic("owner/first/cropped")), 1u);
  EXPECT_EQ(tight.store.entryCount(tight.topic("owner/second/cropped")), 1u);
  EXPECT_EQ(tight.history.protectedExcessBytes(), one);
}

// GAP-10: a 100 Hz live edge for 40 s of stamps never fails a publication, and the window bounds the
// entries per topic.
TEST(ResultHistoryTest, LiveEdgeNeverRefusesAt100Hz) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  constexpr Timestamp kPeriod = kSec / 100;
  const ScriptBundle bundle = cloudBundle(1);
  size_t most = 0;
  for (Timestamp t = 0; t <= 40 * kSec; t += kPeriod) {
    fx.history.noteRequest(node.recipe.key(), t);
    const Status published = fx.history.publish(node, t, t, keyOf(node, static_cast<uint64_t>(t)), bundle);
    ASSERT_TRUE(published.has_value()) << "at " << t << ": " << published.error();
    most = std::max(most, fx.store.entryCount(topic));
  }
  EXPECT_LE(most, static_cast<size_t>(30 * kSec / kPeriod) + 1);
  EXPECT_GE(fx.store.timeRange(topic).first, 10 * kSec);
}

// GAP-50: a compute-cache miss whose result is still in the ring is answered "stored": the store
// already shows it, so nothing is recomputed. An entry gone from the store is a miss.
TEST(ResultHistoryTest, CacheMissChecksTheStoreFirst) {
  Fixture fx(Limits{.cache_bytes = 1});
  const auto& node = fx.install(cropRecipe());
  fx.publishAt(node, 20 * kSec, cropBundle(2, 1.2));
  fx.publishAt(node, 5 * kSec, cropBundle(1, 0.4));
  EXPECT_EQ(fx.history.cacheBytes(), 0u);

  const auto stored = fx.history.lookup(node.recipe.key(), keyOf(node, 20 * kSec), 20 * kSec, 20 * kSec, 0);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->bundle, nullptr);
  EXPECT_EQ(stored->stamp, 20 * kSec);
  EXPECT_TRUE(stored->in_ring);

  std::vector<OwnedObjectWrite> evict(1);
  evict[0].topic = fx.topic("owner/crop/witness");
  evict[0].append = false;
  evict[0].drop = [](Timestamp t, SequentialUID) { return t == 20 * kSec; };
  ASSERT_TRUE(fx.store.pushOwnedBundle(std::move(evict)).has_value());
  EXPECT_FALSE(fx.history.lookup(node.recipe.key(), keyOf(node, 20 * kSec), 20 * kSec, 20 * kSec, 0).has_value())
      << "one output gone: the result is not in the store any more";
}

// GAP-51: the compute cache is sized by time span, not by an entry count: a 1.5 s jog at 10 Hz stays a
// hit end to end.
TEST(ResultHistoryTest, ComputeCacheIsSizedByTimeSpan) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  constexpr Timestamp kPeriod = kSec / 10;
  for (Timestamp t = 10 * kSec; t <= 11 * kSec + kSec / 2; t += kPeriod) {
    fx.publishAt(node, t, cloudBundle(1));
  }
  for (Timestamp t = 11 * kSec + kSec / 2; t >= 10 * kSec; t -= kPeriod) {
    fx.history.noteRequest(node.recipe.key(), t);
    const auto hit = fx.history.lookup(node.recipe.key(), keyOf(node, static_cast<uint64_t>(t)), t, t, 0);
    ASSERT_TRUE(hit.has_value() && hit->bundle != nullptr) << "at " << t;
  }
}

// BUD-02 (changed, N7): the compute cache is bounded in decoded bytes; past the budget the result
// farthest from its node's cursor goes, and nothing is refused (a 1-byte budget keeps nothing).
TEST(ResultHistoryTest, CacheBytesAreBounded) {
  {
    Fixture fx(Limits{.cache_bytes = 1});
    for (const char* id : {"a", "b", "c"}) {
      const auto& node = fx.install(passthroughRecipe(id));
      fx.publishAt(node, 1 * kSec, cloudBundle(1));
    }
    const auto again = fx.history.lookup("owner/a", keyOf(*fx.graph.find("owner/a"), kSec), kSec, kSec, 0);
    ASSERT_TRUE(again.has_value()) << "still in the ring";
    EXPECT_EQ(again->bundle, nullptr);
    EXPECT_EQ(fx.history.cacheBytes(), 0u);
  }
  {
    Fixture probe;
    const auto& sizing = probe.install(passthroughRecipe());
    probe.publishAt(sizing, kSec, cloudBundle(1));
    const size_t one = probe.history.cacheBytes();
    ASSERT_GT(one, 0u);

    Fixture fx(Limits{.cache_bytes = 2 * one});
    const auto& node = fx.install(passthroughRecipe());
    const auto cached = [&](Timestamp t) {
      const auto hit = fx.history.lookup(node.recipe.key(), keyOf(node, static_cast<uint64_t>(t)), t, t, 0);
      return hit.has_value() && hit->bundle != nullptr;
    };
    fx.publishAt(node, 1 * kSec, cloudBundle(1));
    fx.publishAt(node, 2 * kSec, cloudBundle(1));
    fx.publishAt(node, 3 * kSec, cloudBundle(1));
    EXPECT_FALSE(cached(1 * kSec));
    EXPECT_TRUE(cached(2 * kSec) && cached(3 * kSec));

    fx.publishAt(node, 1 * kSec, cloudBundle(1));
    EXPECT_FALSE(cached(3 * kSec)) << "farthest from the cursor at 1 s";
    EXPECT_TRUE(cached(1 * kSec) && cached(2 * kSec));
  }
}

// GAP-49: a decoded object is sized from its fields (a point cloud's buffer), never by serializing it.
TEST(ResultHistoryTest, ByteEstimateDoesNotSerialize) {
  constexpr size_t kPoints = size_t{1} << 20;  // 16 MiB of points
  const scripting::ScriptValue cloud = sdk::BuiltinObject(makeCloud(kPoints));
  const size_t estimate = scripting::estimateBytes(cloud);
  EXPECT_GE(estimate, kPoints * 16);
  EXPECT_LT(estimate, kPoints * 16 + 4096);

  using Clock = std::chrono::steady_clock;
  const auto serialize_begin = Clock::now();
  ASSERT_TRUE(serializeBuiltinObject(std::get<sdk::BuiltinObject>(cloud)).has_value());
  const auto serialize_time = Clock::now() - serialize_begin;
  auto estimate_time = Clock::duration::max();
  for (int round = 0; round < 3; ++round) {
    const auto begin = Clock::now();
    for (int call = 0; call < 10; ++call) {
      EXPECT_EQ(scripting::estimateBytes(cloud), estimate);
    }
    estimate_time = std::min(estimate_time, Clock::now() - begin);
  }
  EXPECT_LT(estimate_time, serialize_time) << "ten estimates cost less than one copy of the payload";

  // The compute cache charges the estimate for a decoded object it holds: one of a result the ring does not.
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  fx.publishAt(node, kSec, cloudBundle(1));
  const size_t ring_held = fx.history.cacheBytes();
  ScriptBundle bundle;
  bundle.outputs.push_back({.name = "cropped", .value = cloud});
  ASSERT_TRUE(
      fx.history.publish(node, kSec, kSec, keyOf(node, 2), bundle, {}, ResultPublishOptions{.cache_only = true}));
  EXPECT_GE(fx.history.cacheBytes() - ring_held, estimate);
  EXPECT_LT(fx.history.cacheBytes() - ring_held, estimate + 1024) << "plus the entry's own fixed cost";
}

// ---- The cache holds no decoded copy of what the ring holds --------------------------------------

// The compute cache keeps no decoded copy of an object output the ring holds: the cloud is released at the
// publication, the cache charges the entry and the number and string outputs only, and a hit serves those,
// in the ring, with nothing to republish.
TEST(ResultHistoryTest, CacheHoldsNoDecodedCopyOfARingHeldObject) {
  Fixture fx;
  const auto& node = fx.install(cropRecipe());
  constexpr size_t kPoints = 100'000;
  std::weak_ptr<const void> points;
  {
    ScriptBundle bundle = cropBundle(kPoints, 1.2);
    points = std::get<sdk::BuiltinObject>(bundle.outputs[0].value).get<sdk::PointCloud>()->anchor;
    fx.history.noteRequest(node.recipe.key(), 10 * kSec);
    ASSERT_TRUE(fx.history.publish(node, 10 * kSec, 10 * kSec, keyOf(node, 1), std::move(bundle)));
  }
  EXPECT_TRUE(points.expired()) << "the ring holds the cloud serialized";
  EXPECT_LT(fx.history.cacheBytes(), 1024u) << "the entry, the number and the string";
  EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, fx.topic("owner/crop/cropped"), 10 * kSec).width, kPoints);

  const auto hit = fx.history.lookup(node.recipe.key(), keyOf(node, 1), 10 * kSec, 10 * kSec, 0);
  ASSERT_TRUE(hit.has_value() && hit->bundle != nullptr);
  EXPECT_TRUE(hit->in_ring);
  EXPECT_FALSE(hit->key.has_value()) << "nothing to republish";
  EXPECT_DOUBLE_EQ(std::get<double>(hit->bundle->outputs[2].value), 1.2);
  EXPECT_EQ(std::get<std::string>(hit->bundle->outputs[3].value), std::to_string(kPoints) + " points");

  // A Python output also holds its object in its keepalive (its proxy's anchor, with the native charge): released too.
  if (!scripting::pythonAvailable()) {
    return;
  }
  const auto& python = fx.install(passthroughRecipe("py"));
  const auto script = scripting::compileOnDemandScriptPython(
      "def evaluate(inputs, params):\n    return inputs['cloud'].crop_box(min=(-1e9, -1e9, -1e9), max=(1e9, 1e9, "
      "1e9))\n");
  ASSERT_TRUE(script.has_value()) << script.error();
  const std::vector<scripting::NamedValue> inputs{{.name = "cloud", .value = sdk::BuiltinObject(makeCloud(kPoints))}};
  std::weak_ptr<const void> cropped;
  std::weak_ptr<const void> keepalive;
  {
    auto ran = scripting::runOnDemandScriptPython(
        *script, Span<const scripting::NamedValue>(inputs.data(), inputs.size()), "{}",
        Span<const OutputSpec>(python.recipe.outputs.data(), python.recipe.outputs.size()));
    ASSERT_TRUE(ran.has_value()) << ran.error().message;
    cropped = std::get<sdk::BuiltinObject>(ran->outputs[0].value).get<sdk::PointCloud>()->anchor;
    keepalive = ran->outputs[0].keepalive;
    ASSERT_FALSE(keepalive.expired());
    fx.history.noteRequest(python.recipe.key(), 10 * kSec);
    ASSERT_TRUE(fx.history.publish(python, 10 * kSec, 10 * kSec, keyOf(python, 1), std::move(*ran)));
  }
  EXPECT_TRUE(cropped.expired()) << "the ring holds the Python cloud serialized";
  EXPECT_TRUE(keepalive.expired()) << "and its keepalive went with the value";
  EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, fx.topic("owner/py/cropped"), 10 * kSec).width, kPoints);
}

// A cache entry holding no decoded objects cannot serve a hit alone: when its ring entry goes (replaced at
// its stamp, evicted by the budget, removed behind the history's back) the hit goes too, and so do the
// entry's bytes once the history sees it go.
TEST(ResultHistoryTest, ReleasingARingEntryDropsItsStrippedCacheEntry) {
  const size_t one = serializedSize(sdk::BuiltinObject(makeCloud(1)));
  Fixture fx(Limits{.ring_bytes = 2 * one});
  const auto& node = fx.install(passthroughRecipe());
  const auto cached = [&](uint64_t input, Timestamp stamp) {
    return fx.history.lookup(node.recipe.key(), keyOf(node, input), stamp, stamp, 0).has_value();
  };
  fx.history.noteRequest(node.recipe.key(), 10 * kSec);
  ASSERT_TRUE(fx.history.publish(node, 10 * kSec, 10 * kSec, keyOf(node, 1), cloudBundle(1)));
  const size_t entry = fx.history.cacheBytes();
  ASSERT_TRUE(fx.history.publish(node, 10 * kSec, 10 * kSec, keyOf(node, 2), cloudBundle(1)));
  EXPECT_FALSE(cached(1, 10 * kSec)) << "replaced at its stamp";
  EXPECT_TRUE(cached(2, 10 * kSec));
  EXPECT_EQ(fx.history.cacheBytes(), entry);

  fx.publishAt(node, 11 * kSec, cloudBundle(1));
  fx.publishAt(node, 12 * kSec, cloudBundle(1));  // past the ring budget: 10 s is the farthest from 12 s
  EXPECT_FALSE(cached(2, 10 * kSec)) << "evicted by the budget";
  EXPECT_EQ(fx.history.cacheBytes(), 2 * entry);

  prune(fx.store, fx.topic("owner/crop/cropped"));
  EXPECT_FALSE(cached(static_cast<uint64_t>(12 * kSec), 12 * kSec)) << "removed behind the history's back";
}

// A late result the ring does not hold (`cache_only`, another result at its stamp) keeps its whole decoded
// bundle, charged in full: a hit is not in the ring and carries its key, and once its republication takes
// the ring entry the cache drops the decoded copy.
TEST(ResultHistoryTest, CacheOnlyResultKeepsItsDecodedBundle) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  fx.history.noteRequest(node.recipe.key(), 10 * kSec);
  ASSERT_TRUE(fx.history.publish(node, 10 * kSec, 10 * kSec, keyOf(node, 1), cloudBundle(2)));
  const size_t entry = fx.history.cacheBytes();
  constexpr size_t kPoints = 1000;
  std::weak_ptr<const void> points;
  {
    ScriptBundle late = cloudBundle(kPoints);
    points = std::get<sdk::BuiltinObject>(late.outputs[0].value).get<sdk::PointCloud>()->anchor;
    ASSERT_TRUE(fx.history.publish(
        node, 10 * kSec, 10 * kSec, keyOf(node, 2), std::move(late), {}, ResultPublishOptions{.cache_only = true}));
  }
  EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, topic, 10 * kSec).width, 2u) << "the ring keeps what serves the cursor";
  EXPECT_FALSE(points.expired()) << "the late result's only copy";
  EXPECT_GE(fx.history.cacheBytes(), entry + kPoints * kPointStride);
  {
    const auto hit = fx.history.lookup(node.recipe.key(), keyOf(node, 2), 10 * kSec, 10 * kSec, 0);
    ASSERT_TRUE(hit.has_value() && hit->bundle != nullptr);
    EXPECT_FALSE(hit->in_ring);
    EXPECT_EQ(hit->key, std::optional<Key>(keyOf(node, 2)));
    EXPECT_EQ(std::get<sdk::BuiltinObject>(hit->bundle->outputs[0].value).get<sdk::PointCloud>()->width, kPoints);
    ASSERT_TRUE(fx.history.publish(node, 10 * kSec, hit->stamp, *hit->key, *hit->bundle));
  }
  EXPECT_EQ(decodeAt<sdk::PointCloud>(fx.store, topic, 10 * kSec).width, kPoints);
  EXPECT_TRUE(points.expired()) << "the ring holds it now";
  EXPECT_EQ(fx.history.cacheBytes(), entry) << "the replaced result went with its ring entry";
}

// GAP-14 (store half): a payload a layer holds stays readable and unchanged after a replacement at its
// stamp and an eviction of its entry.
TEST(ResultHistoryTest, HeldFrameSurvivesPublicationAndEviction) {
  const size_t one = serializedSize(sdk::BuiltinObject(makeCloud(3)));
  Fixture fx(Limits{.ring_bytes = one});
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  ASSERT_TRUE(fx.history.publish(node, 10 * kSec, 10 * kSec, keyOf(node, 1), cloudBundle(3)));
  const auto held = fx.store.latestAt(topic, 10 * kSec);
  ASSERT_TRUE(held.has_value());
  const std::vector<uint8_t> before(held->payload.bytes.begin(), held->payload.bytes.end());

  ASSERT_TRUE(fx.history.publish(node, 10 * kSec, 10 * kSec, keyOf(node, 2), cloudBundle(5)));
  fx.publishAt(node, 11 * kSec, cloudBundle(4));
  ASSERT_EQ(timestampsOf(fx.store, topic), std::vector<Timestamp>{11 * kSec}) << "replaced, then evicted";

  const std::vector<uint8_t> after(held->payload.bytes.begin(), held->payload.bytes.end());
  EXPECT_EQ(after, before);
  const auto decoded = deserializeBuiltinObject(sdk::BuiltinObjectType::kPointCloud, after.data(), after.size());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->get<sdk::PointCloud>()->width, 3u);
}

// clearDataset drops the results of every node bound to the dataset and keeps their topics.
TEST(ResultHistoryTest, ClearDatasetDropsResultsAndKeepsTopics) {
  Fixture fx;
  const auto& node = fx.install(passthroughRecipe());
  const ObjectTopicId topic = fx.topic("owner/crop/cropped");
  fx.publishAt(node, 10 * kSec, cloudBundle(1));
  fx.history.clearDataset(kDataset + 1);
  EXPECT_EQ(fx.store.entryCount(topic), 1u);

  fx.history.clearDataset(kDataset);
  EXPECT_EQ(fx.store.entryCount(topic), 0u);
  EXPECT_EQ(fx.topic("owner/crop/cropped"), topic);
  EXPECT_EQ(fx.history.ringBytes(), 0u);
  EXPECT_EQ(fx.history.cacheBytes(), 0u);
  EXPECT_FALSE(fx.history.lookup(node.recipe.key(), keyOf(node, 10 * kSec), 10 * kSec, 10 * kSec, 0).has_value());
}

// Dropping a node's results prunes every entry of its topics, also one the ledger never recorded, and
// skips a topic already gone.
TEST(ResultHistoryTest, DroppingResultsPrunesEveryEntryOfTheTopicsLeft) {
  Fixture fx;
  const auto& node = fx.install(recipeWith("crop", {croppedSpec(), witnessSpec()}));
  const ObjectTopicId cropped = fx.topic("owner/crop/cropped");
  fx.history.noteRequest(node.recipe.key(), 5 * kSec);
  const auto bytes = serializeBuiltinObject(sdk::BuiltinObject(makeCloud(1))).value();
  ASSERT_TRUE(fx.store.pushOwned(cropped, 5 * kSec, bytes).has_value());
  fx.store.removeTopic(fx.topic("owner/crop/witness"));

  fx.history.clearDataset(kDataset);
  EXPECT_EQ(fx.store.entryCount(cropped), 0u);
}

// The GUI-thread cost of one publication (serialize, pushOwnedBundle, ledger, cache, budgets) with
// ~100 entries retained per topic, within the ring budget and with room for only ~50 results, where every
// publication evicts one. Step 0 measured 0.16 ms (small objects) and 0.4-0.85 ms (1.5 MB cloud) for the
// prototype's publication.
TEST(ResultHistoryBench, PublishCost) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr Timestamp kPeriod = kSec / 10;
  constexpr int kCalls = 200;
  for (const size_t points : {size_t{4}, size_t{1'500'000 / 16}}) {
    const ScriptBundle bundle = cropBundle(points, 1.0);
    const size_t result_bytes = serializedSize(std::get<sdk::BuiltinObject>(bundle.outputs[0].value)) +
                                serializedSize(std::get<sdk::BuiltinObject>(bundle.outputs[1].value));
    for (const bool evicting : {false, true}) {
      Fixture fx(Limits{.ring_bytes = evicting ? 50 * result_bytes : Limits{}.ring_bytes, .window = 100 * kPeriod});
      const auto& node = fx.install(cropRecipe());
      Timestamp t = 0;
      const auto publish = [&] {
        t += kPeriod;
        fx.history.noteRequest(node.recipe.key(), t);
        const auto begin = std::chrono::steady_clock::now();
        EXPECT_TRUE(fx.history.publish(node, t, t, keyOf(node, static_cast<uint64_t>(t)), bundle).has_value());
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
      };
      for (int warmup = 0; warmup < 105; ++warmup) {
        publish();
      }
      std::vector<double> micros;
      for (int call = 0; call < kCalls; ++call) {
        micros.push_back(publish());
      }
      std::sort(micros.begin(), micros.end());
      std::printf(
          "ResultHistory::publish, cloud %zu bytes + scene + number + string, %zu entries per topic%s: median %.1f "
          "us, p95 %.1f us\n",
          points * 16, fx.store.entryCount(fx.topic("owner/crop/cropped")), evicting ? ", evicting every publish" : "",
          micros[micros.size() / 2], micros[micros.size() * 95 / 100]);
    }
  }
}

// The GUI-thread cost of a publication that removes nothing (a new stamp inside the window, under the budget)
// with 600 to 800 results retained per node: a cloud of 4 points, a scene, a number and a string.
TEST(ResultHistoryBench, PublishCostAt600RetainedResults) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr Timestamp kPeriod = kSec / 100;
  const ScriptBundle bundle = cropBundle(4, 1.0);
  Fixture fx(Limits{.window = 1000 * kPeriod});
  const auto& node = fx.install(cropRecipe());
  Timestamp t = 0;
  const auto publish = [&] {
    t += kPeriod;
    fx.history.noteRequest(node.recipe.key(), t);
    EXPECT_TRUE(fx.history.publish(node, t, t, keyOf(node, static_cast<uint64_t>(t)), bundle).has_value());
  };
  for (int warmup = 0; warmup < 600; ++warmup) {
    publish();
  }
  const double micros = test::medianMicros(201, publish);
  std::printf(
      "ResultHistory::publish removing nothing, %zu entries per topic at the end: median %.1f us\n",
      fx.store.entryCount(fx.topic("owner/crop/cropped")), micros);
}

// `lookup` at 1, 100 and 10,000 results per node in the window: a hit at the middle stamp (from the compute
// cache) and a miss, median of 1001 calls. The store's `indexOf` per output is the production one.
TEST(ResultHistoryBench, LookupCost) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  for (const int results : {1, 100, 10'000}) {
    Fixture fx;
    const auto& node = fx.install(passthroughRecipe());
    constexpr Timestamp kPeriod = kSec / 1000;  // 10,000 results span 10 s, inside the ±30 s window
    for (int index = 0; index < results; ++index) {
      fx.publishAt(node, index * kPeriod, cloudBundle(1));
    }
    const Timestamp middle = (results / 2) * kPeriod;
    const Key hit_key = keyOf(node, static_cast<uint64_t>(middle));
    const Key miss_key = keyOf(node, 1);
    bool all_hit = true;
    const double hit_us = test::medianMicros(1001, [&] {
      all_hit = fx.history.lookup(node.recipe.key(), hit_key, middle, middle, 0).has_value() && all_hit;
    });
    bool any_hit = false;
    const double miss_us = test::medianMicros(1001, [&] {
      any_hit = fx.history.lookup(node.recipe.key(), miss_key, middle, middle, 0).has_value() || any_hit;
    });
    EXPECT_TRUE(all_hit);
    EXPECT_FALSE(any_hit);
    std::printf(
        "ResultHistory::lookup, %d results per node: hit %.0f ns, miss %.0f ns\n", results, hit_us * 1000,
        miss_us * 1000);
  }
}

// What the compute cache and the ring hold after 600 results of a 260k-point XYZI cloud at 10 Hz under the
// default limits (both budgets 256 MiB, the ±30 s window), the cursor following each result.
TEST(ResultHistoryBench, CacheBytesOfLargeCloudResults) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  Fixture fx;
  const auto& node = fx.install(cropRecipe());
  const ScriptBundle bundle = cropBundle(260'000, 1.0);  // every result shares the one cloud's points
  constexpr Timestamp kPeriod = kSec / 10;
  for (Timestamp index = 0; index < 600; ++index) {
    fx.publishAt(node, index * kPeriod, bundle);
  }
  std::printf(
      "ResultHistory, 600 results of a 260k-point cloud: cache %zu bytes, ring %.1f MiB, %zu ring entries\n",
      fx.history.cacheBytes(), static_cast<double>(fx.history.ringBytes()) / (1 << 20),
      fx.store.entryCount(fx.topic("owner/crop/cropped")));
}

}  // namespace
}  // namespace PJ
