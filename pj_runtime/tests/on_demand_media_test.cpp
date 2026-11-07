// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Media for on-demand recipes (docs/archive/derived_objects/SCENARIOS.md, rows MED-01..04, MED-06..10, MED-21,
// GAP-26): the real Luau run through OnDemandService, the decoder the session is given, and the annotations' pairing
// as stored.
// A raw-only fake stands in for the codec, which pj_app owns (its tests: pj_app/tests/scene_image_decoder_test.cpp).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "on_demand_test_utils.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/builtin/camera_info.hpp"
#include "pj_base/builtin/depth_image.hpp"
#include "pj_base/builtin/image_annotations.hpp"
#include "pj_base/builtin/video_frame.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/GridRunner.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scripting/image_decoder.h"
#include "pj_scripting/memory_budget.h"
#include "pj_scripting/video_frame_source.h"

namespace PJ {
namespace {

using scripting::DecodedPixels;
using scripting::OutputSpec;

constexpr Timestamp kSec = 1'000'000'000;

/// Decodes tightly packed 16UC1 (little-endian) and rgb8 images, charged as a real decoder charges them; counts its
/// calls (any thread).
class RawDecoder final : public scripting::IImageDecoder {
 public:
  mutable std::atomic<int> calls{0};
  Expected<DecodedPixels> decode(
      const sdk::Image& image, const std::shared_ptr<scripting::native::MemoryBudget>& budget) const override {
    ++calls;
    const bool depth = image.encoding == "16UC1";
    if (!depth && image.encoding != "rgb8") {
      return unexpected("unsupported image encoding: " + image.encoding);
    }
    const uint32_t channels = depth ? 1 : 3;
    const std::size_t count = std::size_t{image.width} * image.height * channels;
    if (image.data.size() != count * (depth ? 2 : 1)) {
      return unexpected("image buffer does not match its size");
    }
    auto charge = std::make_shared<scripting::native::MemoryReservation>(budget);
    if (Status grown = charge->grow(count * sizeof(float)); !grown) {
      return unexpected(grown.error());
    }
    auto samples = std::make_shared<std::vector<float>>(count);
    for (std::size_t index = 0; index < count; ++index) {
      (*samples)[index] = depth ? float(image.data[2 * index] | (image.data[2 * index + 1] << 8)) : image.data[index];
    }
    DecodedPixels out;
    out.width = image.width;
    out.height = image.height;
    out.channels = channels;
    out.samples = std::move(samples);
    out.charge = std::move(charge);
    return out;
  }
};

/// One dataset "log" whose object topics carry their builtin type in their metadata, the scalar series
/// `threshold`, and an on-demand service running the real Luau engine with `decoder` injected. An input may be
/// qualified "log:<topic>".
struct Fixture {
  Fixture() {
    dataset = engine.createDataset(DatasetDescriptor{.source_name = "log"}).value();
    threshold = writer.registerScalarSeries(dataset, "threshold", NumericType::kFloat64).value();
    OnDemandService::Services services;
    services.resolver = [this](std::string_view input) {
      const std::string_view bare = input.starts_with("log:") ? input.substr(4) : input;
      if (bare == "threshold") {
        return std::vector<RecipeGraph::InputCandidate>{{dataset, sdk::BuiltinObjectType::kNone, "threshold"}};
      }
      const auto type = types.find(std::string(bare));
      if (type == types.end()) {
        return std::vector<RecipeGraph::InputCandidate>{};
      }
      return std::vector<RecipeGraph::InputCandidate>{{dataset, type->second, std::string(bare)}};
    };
    services.scalars =
        [this](DatasetId in, std::string_view name, Timestamp t) -> std::optional<InputStager::ScalarSample> {
      if (in != dataset || name != "threshold") {
        return std::nullopt;
      }
      return InputStager::sampleAt(engine, threshold.topic_id, 0, t);
    };
    services.parsers = [](ObjectTopicId) { return SessionManager::ParserBinding{}; };
    services.scheduling.image_decoder = [this] { return decoder; };
    services.scheduling.video_source = [this] { return video; };
    service = std::make_unique<OnDemandService>(store, engine, pool, budget, std::move(services));
  }

  ObjectTopicId topic(const std::string& name, sdk::BuiltinObjectType type) {
    types[name] = type;
    return store.registerTopic({dataset, name, "{\"builtin_object_type\":\"" + std::string(sdk::name(type)) + "\"}"})
        .value();
  }
  void push(ObjectTopicId topic, Timestamp entry, const sdk::BuiltinObject& object) {
    ASSERT_TRUE(store.pushOwned(topic, entry, serializeBuiltinObject(object).value()).has_value());
  }
  /// Installs `owner/<id>` running `script`, requests it at `t` and waits for the run to publish.
  void evaluate(
      const std::string& id, std::vector<std::string> inputs, std::vector<OutputSpec> outputs, std::string script,
      Timestamp t) {
    RecipeGraph::Recipe recipe = test::onDemandRecipe(id, std::move(inputs), std::move(outputs));
    recipe.script = std::move(script);
    const auto installed = service->install(recipe);
    ASSERT_TRUE(installed.has_value()) << installed.error();
    service->requestAt(recipe.key(), t);
    pool.waitIdle();
    const auto state = service->state(recipe.key());
    ASSERT_TRUE(state && state->status == NodeScheduler::NodeState::Status::kReady) << (state ? state->detail : "");
  }
  /// The `index`-th output topic of `owner/<id>`.
  ObjectTopicId output(const std::string& id, std::size_t index = 0) {
    return service->history().topicsOf("owner/" + id).at(index);
  }

  std::shared_ptr<RawDecoder> decoder = std::make_shared<RawDecoder>();
  std::shared_ptr<scripting::IVideoFrameSource> video;
  std::map<std::string, sdk::BuiltinObjectType, std::less<>> types;
  DataEngine engine;
  DataWriter writer{engine.createWriter()};
  ObjectStore store;
  DatasetId dataset = 0;
  ScalarSeriesHandle threshold;
  InFlightBudget budget{8 * InFlightBudget::kRunBytes};
  EnginePool pool;  ///< after the store and the budget: destroyed first, joining the runs
  std::unique_ptr<OnDemandService> service;
};

OutputSpec spec(const char* declaration) {
  return scripting::parseOutputSpec(declaration).value();
}

/// A canonical depth image of `samples` (16UC1, millimetres, little-endian), `width` wide.
sdk::DepthImage depthImage(uint32_t width, const std::vector<uint16_t>& samples, Timestamp stamp) {
  std::vector<uint8_t> bytes(samples.size() * 2);
  std::memcpy(bytes.data(), samples.data(), bytes.size());
  const sdk::PayloadView view = sdk::makePayloadView(std::move(bytes));
  sdk::DepthImage depth;
  depth.width = width;
  depth.height = static_cast<uint32_t>(samples.size() / width);
  depth.encoding = "16UC1";
  depth.timestamp_ns = stamp;
  depth.data = view.bytes;
  depth.anchor = view.anchor;
  return depth;
}

// ---- pairing as published (MED-01, MED-02, MED-03, MED-06, MED-08) ------------------------------------------

TEST(OnDemandMediaTest, CanonicalDepthDecodePreservesPairingAndPayloadTime) {
  Fixture fx;
  const ObjectTopicId depth = fx.topic("depth", sdk::BuiltinObjectType::kDepthImage);
  fx.push(depth, 5 * kSec, sdk::BuiltinObject(depthImage(2, {1000, 0}, 3 * kSec)));  // payload 3 s, entry 5 s
  fx.evaluate(
      "pair", {"depth"}, {spec("ann:kImageAnnotations")},
      "local inputs = ...; local a = inputs.depth:decode(); local b = inputs.depth:decode(); "
      "return {ann = b:threshold('>', 500)}",
      7 * kSec);
  EXPECT_EQ(fx.decoder->calls, 1);  // one decode per input and run
  const auto annotations = test::decodeAt<sdk::ImageAnnotations>(fx.store, fx.output("pair"), 7 * kSec);
  EXPECT_EQ(annotations.image_topic, "depth");
  EXPECT_EQ(annotations.timestamp, 3 * kSec);
  ASSERT_EQ(annotations.points.size(), 1u);
  EXPECT_EQ(annotations.points[0].points.size(), 1u);
}

TEST(OnDemandMediaTest, DatasetQualifiedImageInputAnnotatesWithTheStoreTopicName) {
  Fixture fx;
  const ObjectTopicId depth = fx.topic("depth", sdk::BuiltinObjectType::kDepthImage);
  fx.push(depth, 5 * kSec, sdk::BuiltinObject(depthImage(2, {1000, 0}, 3 * kSec)));
  fx.evaluate(
      "qualified", {"log:depth"},
      {spec("ann:kImageAnnotations"), spec("bare:kImageAnnotations"), spec("qualified:kImageAnnotations")},
      R"(local inputs = ...
         local image = inputs["log:depth"]:decode()
         return {
           ann = image:threshold(">", 0),
           bare = pj.annotations.new{image_topic = "depth"}:points{points = {{0, 0}}, type = "points"}:finish(),
           qualified = pj.annotations.new{image_topic = "log:depth"}:points{points = {{0, 0}}, type = "points"}:finish(),
         })",
      7 * kSec);
  for (std::size_t index = 0; index < 3; ++index) {
    SCOPED_TRACE(index);
    const auto annotations = test::decodeAt<sdk::ImageAnnotations>(fx.store, fx.output("qualified", index), 7 * kSec);
    EXPECT_EQ(annotations.image_topic, "depth");
    EXPECT_EQ(annotations.timestamp, 3 * kSec);
  }
}

TEST(OnDemandMediaTest, ImageAnnotationsOutputIsStampedAndMarkedSnapshot) {
  Fixture fx;
  const ObjectTopicId depth = fx.topic("depth", sdk::BuiltinObjectType::kDepthImage);
  fx.push(depth, 10 * kSec, sdk::BuiltinObject(depthImage(2, {1000, 0}, 10 * kSec)));
  fx.writer.appendScalar(fx.threshold, 15 * kSec, 500.0);  // a newer scalar input drives the result's stamp
  ASSERT_FALSE(fx.engine.commitChunks(fx.writer.flushAll()).empty());
  fx.evaluate(
      "stamped", {"depth", "threshold"},
      {spec("bright:kImageAnnotations"), spec("built:kImageAnnotations"), spec("other:kImageAnnotations")},
      R"(local inputs = ...
         local image = inputs.depth:decode()
         return {
           bright = image:threshold(">", inputs.threshold),
           built = pj.annotations.new{image_topic = "depth"}:points{points = {{0, 0}}, type = "points"}:finish(),
           other = pj.annotations.new{image_topic = "/cam"}:points{points = {{1, 2}}, type = "points"}:finish(),
         })",
      20 * kSec);
  for (std::size_t index = 0; index < 3; ++index) {
    SCOPED_TRACE(index);
    const ObjectTopicId topic = fx.output("stamped", index);
    EXPECT_NE(fx.store.descriptor(topic).metadata_json.find(sdk::kSnapshotMetadataKey), std::string::npos);
    const auto entry = fx.store.latestAt(topic, 20 * kSec);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->timestamp, 15 * kSec);  // the store entry takes the result's stamp
    const auto annotations = test::decodeAt<sdk::ImageAnnotations>(fx.store, topic, 20 * kSec);
    // Paired with the image: its topic and its time, not the newer scalar's; no image input: the result's stamp.
    EXPECT_EQ(annotations.image_topic, index == 2 ? "/cam" : "depth");
    EXPECT_EQ(annotations.timestamp, index == 2 ? 15 * kSec : 10 * kSec);
  }
}

// An explicit evaluation (GridRunner) decodes through the same decoder and stamps annotations the same way.
TEST(OnDemandMediaTest, AnEvaluationDecodesAndPairsLikeARequest) {
  Fixture fx;
  const ObjectTopicId depth = fx.topic("depth", sdk::BuiltinObjectType::kDepthImage);
  fx.push(depth, 10 * kSec, sdk::BuiltinObject(depthImage(2, {1000, 0}, 10 * kSec)));
  fx.writer.appendScalar(fx.threshold, 15 * kSec, 1.0);
  ASSERT_FALSE(fx.engine.commitChunks(fx.writer.flushAll()).empty());
  RecipeGraph::Recipe recipe =
      test::onDemandRecipe("eval", {"depth", "threshold"}, {spec("width:number"), spec("stamp:number")});
  recipe.script = R"(local inputs = ...
    local other = pj.annotations.new{image_topic = "/cam"}:finish()
    return {width = inputs.depth:decode().width, stamp = other.timestamp:to_number()})";
  ASSERT_TRUE(fx.service->install(recipe).has_value());
  std::optional<GridRunner::Report> report;
  const auto job = fx.service->evaluations().submit(
      *fx.service->graph().find(recipe.key()), true, 20 * kSec, 20 * kSec, {},
      [&](GridRunner::Report done) { report = std::move(done); });
  ASSERT_TRUE(job.has_value()) << job.error();
  for (int pass = 0; pass < 100 && !report; ++pass) {
    fx.pool.waitIdle();
    QCoreApplication::processEvents();
  }
  ASSERT_TRUE(report);
  ASSERT_EQ(report->bundles.size(), 1u) << report->error;
  EXPECT_EQ(report->bundles[0].outputs.at("width").value, 2);
  EXPECT_EQ(report->bundles[0].outputs.at("stamp").value, 15 * kSec);  // the result's stamp
}

// The shell gives the session its decoder (pj_app's SceneImageDecoder); without one, `decode` says so.
TEST(OnDemandMediaTest, TheSessionDecodesThroughTheDecoderItIsGiven) {
  SessionManager session;
  const DatasetId dataset = session.dataEngine().createDataset(DatasetDescriptor{.source_name = "log"}).value();
  const ObjectTopicId depth =
      session.objectStore().registerTopic({dataset, "depth", R"({"builtin_object_type":"kDepthImage"})"}).value();
  ASSERT_TRUE(
      session.objectStore()
          .pushOwned(depth, kSec, serializeBuiltinObject(sdk::BuiltinObject(depthImage(1, {1000}, kSec))).value())
          .has_value());
  RecipeGraph::Recipe recipe = test::onDemandRecipe("pair", {"depth"}, {spec("ann:kImageAnnotations")});
  recipe.script =
      "local inputs = ...; local pixels = assert(inputs.depth:decode()); return {ann = pixels:threshold('>', 500)}";
  ASSERT_TRUE(session.onDemandService().install(recipe).has_value());
  const auto settle = [&] {
    session.onDemandService().requestAt(recipe.key(), kSec);
    session.enginePool().waitIdle();
    QCoreApplication::processEvents();
    return session.onDemandService().state(recipe.key());
  };
  auto state = settle();
  ASSERT_TRUE(state && state->status == NodeScheduler::NodeState::Status::kError);
  EXPECT_NE(state->detail.find("no image decoder"), std::string::npos) << state->detail;

  const auto decoder = std::make_shared<RawDecoder>();
  session.setImageDecoder(decoder);
  session.onDemandService().rerun({recipe.key()});
  state = settle();
  ASSERT_TRUE(state && state->status == NodeScheduler::NodeState::Status::kReady) << (state ? state->detail : "");
  EXPECT_EQ(decoder->calls, 1);
  const auto annotations = test::decodeAt<sdk::ImageAnnotations>(
      session.objectStore(), session.onDemandService().history().topicsOf(recipe.key()).at(0), kSec);
  ASSERT_EQ(annotations.points.size(), 1u);
  EXPECT_EQ(annotations.image_topic, "depth");
}

// ---- depth through a camera (MED-09) ---------------------------------------------------------------------------

TEST(OnDemandMediaTest, DepthRecipeUnprojectsThroughTheCamera) {
  Fixture fx;
  const ObjectTopicId depth = fx.topic("depth", sdk::BuiltinObjectType::kDepthImage);
  const ObjectTopicId camera_topic = fx.topic("camera", sdk::BuiltinObjectType::kCameraInfo);
  fx.push(depth, 10 * kSec, sdk::BuiltinObject(depthImage(2, {1000, 2000, 0, 4000}, 10 * kSec)));
  sdk::CameraInfo camera;
  camera.K = {2, 0, 0, 0, 2, 0, 0, 0, 1};
  camera.frame_id = "optical";
  fx.push(camera_topic, 0, sdk::BuiltinObject(camera));
  fx.evaluate(
      "cloud", {"depth", "camera"}, {spec("cloud:kPointCloud")},
      "local inputs = ...; return {cloud = inputs.depth:to_point_cloud(inputs.camera)}", 10 * kSec);
  const auto cloud = test::decodeAt<sdk::PointCloud>(fx.store, fx.output("cloud"), 10 * kSec);
  ASSERT_EQ(cloud.width, 2u);
  ASSERT_EQ(cloud.height, 2u);
  ASSERT_EQ(cloud.data.size(), 4u * cloud.point_step);
  EXPECT_EQ(cloud.frame_id, "optical");
  EXPECT_EQ(cloud.timestamp_ns, 10 * kSec);
  EXPECT_FALSE(cloud.is_dense);
  std::array<float, 3> last{};
  std::memcpy(last.data(), cloud.data.data() + 3 * cloud.point_step, sizeof(last));
  EXPECT_EQ(last, (std::array<float, 3>{2, 2, 4}));
}

TEST(OnDemandMediaTest, CanonicalDepthThresholdKeepsPairing) {
  Fixture fx;
  const ObjectTopicId depth = fx.topic("depth", sdk::BuiltinObjectType::kDepthImage);
  fx.push(depth, 100, sdk::BuiltinObject(depthImage(1, {1000}, 123)));
  fx.evaluate(
      "pair", {"depth"}, {spec("overlay:kImageAnnotations")},
      "local inputs = ...; return {overlay = inputs.depth:decode():threshold('>', 500)}", 100);
  const auto annotations = test::decodeAt<sdk::ImageAnnotations>(fx.store, fx.output("pair"), 100);
  EXPECT_EQ(annotations.image_topic, "depth");
  EXPECT_EQ(annotations.timestamp, 123);
  ASSERT_EQ(annotations.points.size(), 1u);
}

// ---- video (MED-04, MED-07) -------------------------------------------------------------------------------------

/// A video over the fixture's store: the frame at `t` is its newest entry at or before `t`, presented at that entry's
/// stamp, one rgb8 pixel. Counts its calls (any thread); with `hold` set, the next call waits in it for `resume`.
class StoreVideo final : public scripting::IVideoFrameSource {
 public:
  explicit StoreVideo(const ObjectStore& store) : store_(store) {}
  std::atomic<int> calls{0};
  std::atomic<bool> hold{false};
  std::promise<void> entered;
  std::promise<void> resume;
  std::function<void()> during;  ///< run inside each call, on the worker (a stream pushing packets meanwhile)

  Expected<scripting::VideoFrameResult> frameAt(
      ObjectTopicId topic, Timestamp t, const std::shared_ptr<scripting::native::MemoryBudget>& /*budget*/,
      const std::function<bool()>& /*cancelled*/) override {
    ++calls;
    if (hold.exchange(false)) {
      entered.set_value();
      resume.get_future().wait();
    }
    if (during) {
      during();
    }
    const auto entry = store_.latestEntryIdAt(topic, t);
    if (!entry) {
      return unexpected("no video frame at the requested time");
    }
    auto rgb = std::make_shared<std::vector<uint8_t>>(3, 200);
    scripting::VideoFrameResult frame;
    frame.presentation_ns = entry->timestamp;
    frame.image.encoding = "rgb8";
    frame.image.width = 1;
    frame.image.height = 1;
    frame.image.row_step = 3;
    frame.image.timestamp_ns = entry->timestamp;
    frame.image.data = {rgb->data(), rgb->size()};
    frame.image.anchor = rgb;
    return frame;
  }
  void retireDataset(DatasetId dataset) override {
    retired.push_back(dataset);
  }
  std::vector<DatasetId> retired;

 private:
  const ObjectStore& store_;
};

sdk::BuiltinObject videoPacket(Timestamp pts) {
  sdk::VideoFrame packet;
  packet.format = "h264";
  packet.timestamp_ns = pts;
  return sdk::BuiltinObject(packet);
}

/// Overlay text naming the presentation time of the frame `query` (a Luau expression) selects, paired with it.
std::string overlayScript(const std::string& query) {
  return "local inputs = ...; local frame = inputs.video:frame_at(" + query +
         "); if frame == nil then return {ann = pj.unavailable('absent')} end; "
         "return {ann = pj.annotations.new{image_topic = 'video'}:text{position = {0, 0}, font_size = 10, "
         "text = tostring(frame.presentation_ns)}:finish()}";
}

/// Requests `key` at `t` and waits until the request settles.
void settle(Fixture& fx, const std::string& key, Timestamp t) {
  fx.service->requestAt(key, t);
  fx.pool.waitIdle();
  QCoreApplication::processEvents();
}

// A bound video's content revision joins the request's key: `frame_at` may read any packet of the topic, so a packet
// added anywhere is a miss, and a cursor that stays on the same entries under the same content is a hit. A script
// that reads the clock is keyed by the instant, as any other (ARCHITECTURE §10).
TEST(OnDemandMediaTest, VideoContentRevisionJoinsTheCacheKey) {
  Fixture fx;
  auto video = std::make_shared<StoreVideo>(fx.store);
  fx.video = video;
  const ObjectTopicId topic = fx.topic("video", sdk::BuiltinObjectType::kVideoFrame);
  fx.push(topic, 10 * kSec, videoPacket(10 * kSec));
  fx.push(topic, 20 * kSec, videoPacket(20 * kSec));
  fx.evaluate("own", {"video"}, {spec("ann:kImageAnnotations")}, overlayScript("inputs.video.timestamp_ns"), 12 * kSec);
  ASSERT_EQ(video->calls, 1);
  settle(fx, "owner/own", 15 * kSec);
  EXPECT_EQ(video->calls, 1) << "the same entry under the same content: a hit";
  fx.push(topic, 40 * kSec, videoPacket(40 * kSec));
  settle(fx, "owner/own", 15 * kSec);
  EXPECT_EQ(video->calls, 2) << "a packet the request does not stage is still content frame_at may read";
  settle(fx, "owner/own", 15 * kSec);
  EXPECT_EQ(video->calls, 2);

  fx.evaluate("now", {"video"}, {spec("ann:kImageAnnotations")}, overlayScript("pj.tf.now"), 12 * kSec);
  ASSERT_EQ(video->calls, 3);
  settle(fx, "owner/now", 15 * kSec);
  EXPECT_EQ(video->calls, 4) << "a run that read the clock is keyed by the instant";
  settle(fx, "owner/now", 15 * kSec);
  EXPECT_EQ(video->calls, 4);
  const auto overlay = test::decodeAt<sdk::ImageAnnotations>(fx.store, fx.output("now"), 15 * kSec);
  ASSERT_EQ(overlay.texts.size(), 1u);
  EXPECT_EQ(overlay.texts[0].text, std::to_string(10 * kSec));
  EXPECT_EQ(overlay.image_topic, "video");
  EXPECT_EQ(overlay.timestamp, 10 * kSec) << "paired with the frame's presentation time";
}

// The frame a run looks up joins its identity through the content (D30: a changed dependency is a miss, never an
// error): a future frame appearing republishes the overlay, and one appearing while the run decodes makes the run's
// completion serve the request again.
TEST(OnDemandMediaCacheTest, ActualVideoQueriesInvalidateCacheAndRepublishObjects) {
  Fixture fx;
  auto video = std::make_shared<StoreVideo>(fx.store);
  fx.video = video;
  const ObjectTopicId topic = fx.topic("video", sdk::BuiltinObjectType::kVideoFrame);
  fx.push(topic, 10 * kSec, videoPacket(10 * kSec));
  const std::string ahead = "inputs.video.timestamp_ns:to_number() + 10e9";
  RecipeGraph::Recipe recipe = test::onDemandRecipe("ahead", {"video"}, {spec("ann:kImageAnnotations")});
  recipe.script = overlayScript(ahead);
  ASSERT_TRUE(fx.service->install(recipe).has_value());
  settle(fx, recipe.key(), 10 * kSec);
  ASSERT_EQ(video->calls, 1);
  const auto first = test::decodeAt<sdk::ImageAnnotations>(fx.store, fx.output("ahead"), 10 * kSec);
  ASSERT_EQ(first.texts.size(), 1u);
  EXPECT_EQ(first.texts[0].text, std::to_string(10 * kSec)) << "nothing newer yet: the frame at 10 s";

  fx.push(topic, 20 * kSec, videoPacket(20 * kSec));  // the future frame appears
  settle(fx, recipe.key(), 10 * kSec);
  ASSERT_EQ(video->calls, 2);
  const auto republished = test::decodeAt<sdk::ImageAnnotations>(fx.store, fx.output("ahead"), 10 * kSec);
  ASSERT_EQ(republished.texts.size(), 1u);
  EXPECT_EQ(republished.texts[0].text, std::to_string(20 * kSec));
  EXPECT_EQ(republished.timestamp, 20 * kSec);

  fx.push(topic, 15 * kSec, videoPacket(15 * kSec));
  video->hold = true;
  fx.service->requestAt(recipe.key(), 10 * kSec);
  ASSERT_EQ(video->entered.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);
  fx.push(topic, 18 * kSec, videoPacket(18 * kSec));  // while the run decodes
  video->resume.set_value();
  for (int pass = 0; pass < 3; ++pass) {
    fx.pool.waitIdle();
    QCoreApplication::processEvents();
  }
  EXPECT_EQ(video->calls, 3) << "a packet arriving during the run does not supersede it (D30): it is shown";
  const auto state = fx.service->state(recipe.key());
  ASSERT_TRUE(state && state->status == NodeScheduler::NodeState::Status::kReady) << (state ? state->detail : "");
  settle(fx, recipe.key(), 10 * kSec);
  EXPECT_EQ(video->calls, 4) << "the next request sees the new content: a miss";
  settle(fx, recipe.key(), 10 * kSec);
  EXPECT_EQ(video->calls, 4) << "nothing changed since: a hit";
}

// Playback paused at a fixed instant while a stream keeps adding packets during every run: each completion still
// serves the request (only the video content moved), so the result is shown instead of cached and run again forever.
TEST(OnDemandMediaTest, APausedRequestIsShownWhilePacketsKeepArriving) {
  Fixture fx;
  auto video = std::make_shared<StoreVideo>(fx.store);
  fx.video = video;
  const ObjectTopicId topic = fx.topic("video", sdk::BuiltinObjectType::kVideoFrame);
  fx.push(topic, 10 * kSec, videoPacket(10 * kSec));
  std::atomic<int> streamed{0};
  video->during = [&] {
    if (streamed < 5) {  // bounded, so a regression ends instead of hanging the test
      const Timestamp stamp = (100 + streamed++) * kSec;
      EXPECT_TRUE(fx.store.pushOwned(topic, stamp, serializeBuiltinObject(videoPacket(stamp)).value()).has_value());
    }
  };
  RecipeGraph::Recipe recipe = test::onDemandRecipe("paused", {"video"}, {spec("ann:kImageAnnotations")});
  recipe.script = overlayScript("inputs.video.timestamp_ns");
  ASSERT_TRUE(fx.service->install(recipe).has_value());
  settle(fx, recipe.key(), 10 * kSec);
  for (int pass = 0; pass < 10; ++pass) {
    fx.pool.waitIdle();
    QCoreApplication::processEvents();
  }
  EXPECT_EQ(video->calls, 1);
  const auto state = fx.service->state(recipe.key());
  ASSERT_TRUE(state && state->status == NodeScheduler::NodeState::Status::kReady) << (state ? state->detail : "");
  const auto overlay = test::decodeAt<sdk::ImageAnnotations>(fx.store, fx.output("paused"), 10 * kSec);
  ASSERT_EQ(overlay.texts.size(), 1u);
  EXPECT_EQ(overlay.texts[0].text, std::to_string(10 * kSec));
}

// An explicit evaluation (GridRunner) reads frames through the same video source as a request.
TEST(OnDemandMediaTest, AnEvaluationReadsVideoFramesLikeARequest) {
  Fixture fx;
  auto video = std::make_shared<StoreVideo>(fx.store);
  fx.video = video;
  const ObjectTopicId topic = fx.topic("video", sdk::BuiltinObjectType::kVideoFrame);
  fx.push(topic, 10 * kSec, videoPacket(10 * kSec));
  RecipeGraph::Recipe recipe = test::onDemandRecipe("eval", {"video"}, {spec("pts:string")});
  recipe.script =
      "local inputs = ...; return {pts = tostring(assert(inputs.video:frame_at(pj.tf.now)).presentation_ns)}";
  ASSERT_TRUE(fx.service->install(recipe).has_value());
  std::optional<GridRunner::Report> report;
  const auto job = fx.service->evaluations().submit(
      *fx.service->graph().find(recipe.key()), true, 12 * kSec, 12 * kSec, {},
      [&](GridRunner::Report done) { report = std::move(done); });
  ASSERT_TRUE(job.has_value()) << job.error();
  for (int pass = 0; pass < 100 && !report; ++pass) {
    fx.pool.waitIdle();
    QCoreApplication::processEvents();
  }
  ASSERT_TRUE(report);
  ASSERT_EQ(report->bundles.size(), 1u) << report->error;
  EXPECT_EQ(report->bundles[0].outputs.at("pts").value, std::to_string(10 * kSec));
  EXPECT_EQ(video->calls, 1);
}

// The shell gives the session its video source (pj_app's SceneVideoFrameSource); the session retires a dataset in it
// before the dataset's storage changes, so no decoder outlives the packets it indexed (CON-13).
TEST(OnDemandMediaTest, TheSessionDecodesVideoThroughItsSourceAndRetiresADatasetBeforeItsStorageChanges) {
  SessionManager session;
  auto video = std::make_shared<StoreVideo>(session.objectStore());
  session.setVideoFrameSource(video);
  const DatasetId dataset = session.dataEngine().createDataset(DatasetDescriptor{.source_name = "log"}).value();
  const ObjectTopicId topic =
      session.objectStore().registerTopic({dataset, "video", R"({"builtin_object_type":"kVideoFrame"})"}).value();
  ASSERT_TRUE(
      session.objectStore().pushOwned(topic, kSec, serializeBuiltinObject(videoPacket(kSec)).value()).has_value());
  RecipeGraph::Recipe recipe = test::onDemandRecipe("frames", {"video"}, {spec("ann:kImageAnnotations")});
  recipe.script = overlayScript("inputs.video.timestamp_ns");
  ASSERT_TRUE(session.onDemandService().install(recipe).has_value());
  session.onDemandService().requestAt(recipe.key(), kSec);
  session.enginePool().waitIdle();
  QCoreApplication::processEvents();
  const auto state = session.onDemandService().state(recipe.key());
  ASSERT_TRUE(state && state->status == NodeScheduler::NodeState::Status::kReady) << (state ? state->detail : "");
  EXPECT_EQ(video->calls, 1);
  EXPECT_TRUE(video->retired.empty());
  session.clearAllObjects();
  EXPECT_EQ(video->retired, std::vector<DatasetId>{dataset});
}

// ---- benchmarks -----------------------------------------------------------------------------------------------

// PJ_BENCH=1: the GUI-thread cost of requesting a recipe that decodes and thresholds a 1080p image (µs, median of 51
// cold requests). The decode runs on a worker, so the GUI cost does not depend on the image's size or codec (GAP-26);
// the fake decodes raw rgb8, and pj_app's SceneImageDecoderBench measures the real codec.
TEST(OnDemandMediaBench, GuiCostOfA1080pImageRequest) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  Fixture fx;
  const ObjectTopicId camera = fx.topic("camera", sdk::BuiltinObjectType::kImage);
  std::vector<uint8_t> raw(std::size_t{1920} * 1080 * 3, 7);
  for (int index = 0; index < 51; ++index) {
    sdk::Image frame;
    frame.encoding = "rgb8";
    frame.width = 1920;
    frame.height = 1080;
    frame.data = {raw.data(), raw.size()};
    frame.timestamp_ns = index * kSec;
    fx.push(camera, index * kSec, sdk::BuiltinObject(frame));
  }
  RecipeGraph::Recipe recipe = test::onDemandRecipe("bright", {"camera"}, {spec("ann:kImageAnnotations")});
  recipe.script = "local inputs = ...; return {ann = inputs.camera:decode():threshold('>', 250)}";
  ASSERT_TRUE(fx.service->install(recipe).has_value());
  std::vector<double> gui_us;
  for (int index = 0; index < 51; ++index) {
    const auto begin = std::chrono::steady_clock::now();
    fx.service->requestAt(recipe.key(), index * kSec);
    gui_us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
    fx.pool.waitIdle();  // the next request is cold again: staged, a miss, a run submitted
  }
  EXPECT_EQ(fx.decoder->calls, 51);
  std::sort(gui_us.begin(), gui_us.end());
  std::printf(
      "[bench] requestAt GUI cost, 1080p image recipe: median %.2f us, max %.2f us (51 cold requests)\n", gui_us[25],
      gui_us[50]);
}

// PJ_BENCH=1: the GUI-thread cost of requesting a recipe over a video input (µs, median of 51 cold requests): the
// staging reads the topic's content revision beside its entry (one more shared-lock read), nothing is fetched or
// parsed on the GUI thread, and the frame is decoded on a worker (the fake reads the store; pj_app's
// SceneVideoFrameSource decodes the codec there).
TEST(OnDemandMediaBench, GuiCostOfAVideoRequest) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  Fixture fx;
  auto video = std::make_shared<StoreVideo>(fx.store);
  fx.video = video;
  const ObjectTopicId topic = fx.topic("video", sdk::BuiltinObjectType::kVideoFrame);
  for (int index = 0; index < 10'000; ++index) {
    fx.push(topic, index * kSec / 100, videoPacket(index * kSec / 100));
  }
  RecipeGraph::Recipe recipe = test::onDemandRecipe("frames", {"video"}, {spec("ann:kImageAnnotations")});
  recipe.script = overlayScript("inputs.video.timestamp_ns");
  ASSERT_TRUE(fx.service->install(recipe).has_value());
  std::vector<double> gui_us;
  for (int index = 0; index < 51; ++index) {
    const auto begin = std::chrono::steady_clock::now();
    fx.service->requestAt(recipe.key(), index * kSec);
    gui_us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
    fx.pool.waitIdle();
  }
  EXPECT_EQ(video->calls, 51);
  std::sort(gui_us.begin(), gui_us.end());
  std::printf(
      "[bench] requestAt GUI cost, video recipe over 10k packets: median %.2f us, max %.2f us (51 cold requests)\n",
      gui_us[25], gui_us[50]);
}

}  // namespace
}  // namespace PJ
