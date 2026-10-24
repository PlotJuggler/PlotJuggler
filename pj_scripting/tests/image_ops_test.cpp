// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Native image and depth operations over decoded pixels, and one run's media state
// (docs/archive/derived_objects/SCENARIOS.md, rows MED-01, MED-19, MED-20, MED-21, BUD-22, BUD-23).

#include "native/image_ops.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "native/depth_ops.h"
#include "pj_base/sdk/platform.hpp"
#include "pj_scripting/memory_budget.h"
#include "pj_scripting/video_frame_source.h"
#include "script_media.h"

namespace PJ::scripting {
namespace {

using native::CompareOp;
using native::MemoryBudget;
using native::NativeLimits;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

NativeLimits limitedTo(std::size_t bytes) {
  NativeLimits limits;
  limits.budget = std::make_shared<MemoryBudget>(bytes);
  return limits;
}

DecodedPixels pixels(uint32_t width, uint32_t height, uint32_t channels, std::vector<float> samples) {
  DecodedPixels out;
  out.width = width;
  out.height = height;
  out.channels = channels;
  out.samples = std::make_shared<const std::vector<float>>(std::move(samples));
  return out;
}

/// 4 x 3, mono, sample = its index; paired with "/camera" at 42.
DecodedPixels gradient() {
  DecodedPixels out = pixels(4, 3, 1, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
  out.image_topic = "/camera";
  out.timestamp_ns = 42;
  return out;
}

std::array<float, 3> pointAt(const sdk::PointCloud& cloud, std::size_t index) {
  std::array<float, 3> xyz{};
  std::memcpy(xyz.data(), cloud.data.data() + index * cloud.point_step, sizeof(xyz));
  return xyz;
}

// ---- image operations (MED-20, BUD-22) ------------------------------------------

TEST(ImageOps, CropRetainsPairingAndOriginalCoordinates) {
  const NativeLimits limits;
  auto cropped = native::imageCrop(gradient(), 1, 1, 2, 2, limits);
  ASSERT_TRUE(cropped) << cropped.error();
  auto pixel = native::imagePixel(*cropped, 0, 0);
  ASSERT_TRUE(pixel);
  EXPECT_EQ((*pixel)[0], 5);
  EXPECT_FALSE(native::imagePixel(*cropped, 2, 0));
  auto annotations = native::imageThreshold(*cropped, CompareOp::kGt, 8, 0, limits);
  ASSERT_TRUE(annotations) << annotations.error();
  EXPECT_EQ(annotations->value.image_topic, "/camera");
  EXPECT_EQ(annotations->value.timestamp, 42);
  ASSERT_EQ(annotations->value.points.size(), 1u);
  ASSERT_EQ(annotations->value.points[0].points.size(), 2u);
  EXPECT_EQ(annotations->value.points[0].points[0], (sdk::Point2{1, 2}));  // the source's coordinates
  EXPECT_EQ(annotations->value.points[0].points[1], (sdk::Point2{2, 2}));
}

TEST(ImageOps, ThresholdLimitsAreAppliedToMatchesWithoutPartialResults) {
  // Room for two points (16 bytes each) beyond the header: two matches fit, three fail whole.
  NativeLimits two = limitedTo(sizeof(sdk::ImageAnnotations) + sizeof(sdk::PointsAnnotation) + 7 + 2 * 16);
  EXPECT_TRUE(native::imageThreshold(gradient(), CompareOp::kGt, 9, 0, two));
  EXPECT_EQ(two.budget->used(), 0u);
  auto too_many = native::imageThreshold(gradient(), CompareOp::kGt, 8, 0, two);
  ASSERT_FALSE(too_many);
  EXPECT_TRUE(too_many.error().starts_with(native::kNativeLimitError)) << too_many.error();
  EXPECT_EQ(two.budget->used(), 0u);
  EXPECT_FALSE(native::imageThreshold(gradient(), CompareOp::kGt, 0, 1, NativeLimits{}));  // no channel 1
  EXPECT_FALSE(native::imageThreshold(gradient(), CompareOp::kGt, std::nan(""), 0, NativeLimits{}));
  auto none = native::imageThreshold(gradient(), CompareOp::kGt, 100, 0, NativeLimits{});
  ASSERT_TRUE(none);
  EXPECT_TRUE(none->value.points.empty());
}

TEST(ImageOps, ThresholdComparesEveryOperatorOnTheChosenChannel) {
  const DecodedPixels rgb = pixels(2, 1, 3, {1, 5, 9, 2, 6, 10});
  const auto matches = [&](CompareOp op, double value, uint32_t channel) {
    auto out = native::imageThreshold(rgb, op, value, channel, NativeLimits{});
    EXPECT_TRUE(out) << out.error();
    return out && !out->value.points.empty() ? out->value.points[0].points.size() : 0u;
  };
  EXPECT_EQ(matches(CompareOp::kLt, 2, 0), 1u);
  EXPECT_EQ(matches(CompareOp::kLe, 2, 0), 2u);
  EXPECT_EQ(matches(CompareOp::kGt, 5, 1), 1u);
  EXPECT_EQ(matches(CompareOp::kGe, 5, 1), 2u);
  EXPECT_EQ(matches(CompareOp::kEq, 10, 2), 1u);
  EXPECT_EQ(matches(CompareOp::kNe, 10, 2), 1u);
}

// The threshold is a Luau number, a double: rounding it to float would make 1.00000001 equal to a sample of 1.
TEST(ImageOps, ThresholdComparesAgainstTheExactDoubleValue) {
  const DecodedPixels one = pixels(1, 1, 1, {1});
  const auto matches = [&](CompareOp op, double value) {
    auto out = native::imageThreshold(one, op, value, 0, NativeLimits{});
    EXPECT_TRUE(out) << out.error();
    return out && !out->value.points.empty();
  };
  EXPECT_TRUE(matches(CompareOp::kLt, 1.00000001));
  EXPECT_FALSE(matches(CompareOp::kGe, 1.00000001));
  EXPECT_FALSE(matches(CompareOp::kEq, 1.00000001));
  EXPECT_TRUE(matches(CompareOp::kNe, 1.00000001));
  EXPECT_TRUE(matches(CompareOp::kGt, 0.99999999));
  EXPECT_FALSE(matches(CompareOp::kLe, 0.99999999));
}

TEST(ImageOps, GrayStatsAndInvalidPixels) {
  DecodedPixels image = pixels(2, 1, 3, {255, 0, 0, 0, 255, 0});
  auto gray = native::imageToGray(image, NativeLimits{});
  ASSERT_TRUE(gray);
  EXPECT_EQ(gray->channels, 1u);
  EXPECT_NEAR((*gray->samples)[0], 76.245, 0.001);
  EXPECT_NEAR((*gray->samples)[1], 149.685, 0.001);
  auto stats = native::imageStats(gradient(), NativeLimits{});
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->min, 0);
  EXPECT_EQ(stats->max, 11);
  EXPECT_EQ(stats->mean, 5.5);
  EXPECT_EQ(stats->count, 12u);
  image = pixels(2, 1, 1, {kNaN, 65535});
  stats = native::imageStats(image, NativeLimits{});
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->mean, 65535);
  EXPECT_EQ(stats->count, 1u);
  auto threshold = native::imageThreshold(image, CompareOp::kNe, 0, 0, NativeLimits{});
  ASSERT_TRUE(threshold);
  ASSERT_EQ(threshold->value.points.size(), 1u);
  EXPECT_EQ(threshold->value.points[0].points.size(), 1u);  // NaN != 0 never matches
  auto none = native::imageStats(pixels(1, 1, 1, {kNaN}), NativeLimits{});
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error(), "image has no finite samples");
}

TEST(ImageOps, RejectsMalformedStorageBoundsAndCancelledWork) {
  DecodedPixels image = gradient();
  EXPECT_FALSE(native::imageCrop(image, 3, 0, 2, 1, NativeLimits{}));
  EXPECT_FALSE(native::imageCrop(image, 0, 0, 0, 1, NativeLimits{}));
  EXPECT_FALSE(native::imageCrop(image, 0, 3, 1, 1, NativeLimits{}));
  NativeLimits cancelled;
  cancelled.cancelled = [] { return true; };
  for (const auto& result :
       {native::imageThreshold(image, CompareOp::kGt, 0, 0, cancelled).error(),
        native::imageStats(image, cancelled).error(), native::imageCrop(image, 0, 0, 2, 2, cancelled).error(),
        native::imageToGray(pixels(1, 1, 3, {1, 2, 3}), cancelled).error()}) {
    EXPECT_EQ(result, native::kCancelledError);
  }
  EXPECT_FALSE(native::validatePixels(pixels(1, 1, 2, {1, 2})));  // two channels
  EXPECT_FALSE(native::validatePixels(pixels(2, 1, 1, {1})));     // short samples
  image.width = UINT32_MAX;
  image.height = UINT32_MAX;
  EXPECT_FALSE(native::imagePixel(image, 0, 0));
  image.samples.reset();
  EXPECT_FALSE(native::validatePixels(image));
}

TEST(ImageOps, AggregateLiveBudgetReleasesAndAliasesChargeOnce) {
  const NativeLimits limits = limitedTo(std::size_t{1} << 20);
  const DecodedPixels input = pixels(256, 768, 1, std::vector<float>(std::size_t{256} * 768));
  {
    auto first = native::imageCrop(input, 0, 0, 256, 768, limits);
    ASSERT_TRUE(first);
    EXPECT_EQ(limits.budget->used(), std::size_t{768} << 10);
    auto alias = native::imageToGray(*first, limits);  // mono: the same samples and charge
    ASSERT_TRUE(alias);
    EXPECT_EQ(alias->samples, first->samples);
    EXPECT_EQ(limits.budget->used(), std::size_t{768} << 10);
    auto second = native::imageCrop(input, 0, 0, 256, 768, limits);
    ASSERT_FALSE(second);
    EXPECT_TRUE(second.error().starts_with(native::kNativeLimitError)) << second.error();
  }
  EXPECT_EQ(limits.budget->used(), 0u);
}

TEST(ImageOps, CancelledNativeAllocationReleasesReservation) {
  NativeLimits limits = limitedTo(1024);
  limits.cancelled = [] { return true; };
  EXPECT_FALSE(native::imageCrop(gradient(), 0, 0, 2, 2, limits));
  EXPECT_FALSE(native::imageThreshold(gradient(), CompareOp::kGt, 0, 0, limits));
  EXPECT_EQ(limits.budget->used(), 0u);
}

TEST(ImageOps, RepresentationNamesTheStorageNotTheDecode) {
  sdk::Image image;
  for (const auto& [encoding, expected] : std::vector<std::pair<std::string, std::string>>{
           {"jpeg", "jpeg"},
           {"png", "png"},
           {"mono8", "raw:mono8"},
           {"bgr8", "raw:bgr8"},
           {"32FC1", "raw:32FC1"},
           {"h264", "compressed:h264"}}) {
    image.encoding = encoding;
    EXPECT_EQ(native::imageRepresentation(image), expected);
  }
  sdk::DepthImage depth;
  depth.encoding = "16UC1";
  EXPECT_EQ(native::imageRepresentation(depth), "depth:16UC1");
  EXPECT_EQ(native::countAnnotationPoints(sdk::ImageAnnotations{}), 0u);
}

// ---- depth unprojection (MED-19) ------------------------------------------------

TEST(DepthOps, UnprojectsMillimetersIntoOrganizedMetricCloud) {
  sdk::CameraInfo camera;
  camera.K = {2, 0, 0, 0, 2, 0, 0, 0, 1};
  camera.frame_id = "optical";
  const DecodedPixels depth = pixels(2, 2, 1, {1000, 2000, 0, 4000});
  const NativeLimits limits;
  auto cloud = native::depthToPointCloud(depth, "16UC1", 77, camera, {}, limits);
  ASSERT_TRUE(cloud) << cloud.error();
  EXPECT_EQ(cloud->width, 2u);
  EXPECT_EQ(cloud->height, 2u);
  EXPECT_EQ(cloud->frame_id, "optical");
  EXPECT_EQ(cloud->timestamp_ns, 77);
  EXPECT_FALSE(cloud->is_dense);
  ASSERT_TRUE(native::resolveXyz(*cloud));
  EXPECT_EQ(pointAt(*cloud, 3), (std::array<float, 3>{2, 2, 4}));  // pixel (1, 1) at 4 m, f = 2
  EXPECT_TRUE(std::isnan(pointAt(*cloud, 2)[2]));                  // zero depth keeps its place
  native::DepthOptions within_3m;
  within_3m.max_depth = 3.0;
  cloud = native::depthToPointCloud(depth, "mono16", 0, camera, within_3m, limits);
  ASSERT_TRUE(cloud);
  EXPECT_TRUE(std::isnan(pointAt(*cloud, 3)[2]));
  EXPECT_EQ(pointAt(*cloud, 1)[2], 2.0f);
  cloud = native::depthToPointCloud(pixels(1, 1, 1, {1.5f}), "32FC1", 0, camera, {}, limits);
  ASSERT_TRUE(cloud);
  EXPECT_EQ(pointAt(*cloud, 0)[2], 1.5f);  // metres stay metres
  native::DepthOptions step;
  step.step = 2;
  cloud = native::depthToPointCloud(depth, "16UC1", 0, camera, step, limits);
  ASSERT_TRUE(cloud);
  EXPECT_EQ(cloud->width, 1u);
  EXPECT_EQ(cloud->height, 1u);
  EXPECT_FALSE(native::depthToPointCloud(depth, "16UC1", 0, camera, {}, limitedTo(4 * 12 - 1)));
  EXPECT_FALSE(native::depthToPointCloud(depth, "mono8", 0, camera, {}, limits));
  step.step = 0;
  EXPECT_FALSE(native::depthToPointCloud(depth, "16UC1", 0, camera, step, limits));
  camera.width = 3;
  EXPECT_FALSE(native::depthToPointCloud(depth, "16UC1", 0, camera, {}, limits));
  camera.width = 0;
  camera.D = {0.1};
  camera.distortion_model = "plumb_bob";
  EXPECT_FALSE(native::depthToPointCloud(depth, "16UC1", 0, camera, {}, limits));
  camera.D.clear();
  camera.K[0] = 0;  // singular
  auto singular = native::depthToPointCloud(depth, "16UC1", 0, camera, {}, limits);
  ASSERT_FALSE(singular);
  EXPECT_EQ(singular.error(), "invalid camera intrinsics");
}

TEST(DepthOps, DepthAsImageSharesTheBytes) {
  sdk::DepthImage depth;
  depth.width = 2;
  depth.height = 1;
  depth.encoding = "16UC1";
  depth.timestamp_ns = 5;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  depth.data = Span<const uint8_t>(bytes.data(), bytes.size());
  const sdk::Image image = native::depthAsImage(depth);
  EXPECT_EQ(image.data.data(), bytes.data());
  EXPECT_EQ(image.encoding, "16UC1");
  EXPECT_EQ(image.width, 2u);
  EXPECT_EQ(image.timestamp_ns, 5);
  EXPECT_FALSE(image.is_bigendian);
}

// ---- one run's media state (MED-01, MED-21, BUD-23) -------------------------------

/// Decodes any image into one sample per pixel, the image's width; counts its calls.
class CountingDecoder final : public IImageDecoder {
 public:
  mutable int calls = 0;
  Expected<DecodedPixels> decode(
      const sdk::Image& image, const std::shared_ptr<native::MemoryBudget>& budget) const override {
    ++calls;
    auto charge = std::make_shared<native::MemoryReservation>(budget);
    if (Status grown = charge->grow(sizeof(float)); !grown) {
      return unexpected(grown.error());
    }
    DecodedPixels out = pixels(1, 1, 1, {static_cast<float>(image.width)});
    out.charge = std::move(charge);
    return out;
  }
};

TEST(ScriptMediaTest, DecodeWithoutInjectedCodecFailsExplicitly) {
  ScriptMedia media(nullptr, NativeLimits{}, 0);
  const sdk::Image image;
  auto decoded = media.decode(&image, image);
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error(), "no image decoder");
}

TEST(ScriptMediaTest, DecodesOncePerInputAndRunAndPairsWithTheInput) {
  auto decoder = std::make_shared<CountingDecoder>();
  const NativeLimits limits = limitedTo(16);
  {
    ScriptMedia media(decoder, limits, 99);
    sdk::Image image;
    image.width = 7;
    image.timestamp_ns = 3;
    sdk::Image other = image;
    media.noteInput(&image, "/cam", "s:/cam", 3);
    auto first = media.decode(&image, image);
    auto again = media.decode(&image, image);
    ASSERT_TRUE(first && again);
    EXPECT_EQ(decoder->calls, 1);
    EXPECT_EQ(first->samples, again->samples);  // one decode, one charge
    EXPECT_EQ(first->image_topic, "/cam");
    EXPECT_EQ(first->timestamp_ns, 3);
    EXPECT_EQ(limits.budget->used(), sizeof(float));
    auto unpaired = media.decode(&other, other);  // not an input: decoded, unpaired, at its own stamp
    ASSERT_TRUE(unpaired);
    EXPECT_EQ(decoder->calls, 2);
    EXPECT_TRUE(unpaired->image_topic.empty());
    EXPECT_EQ(unpaired->timestamp_ns, 3);
    EXPECT_EQ(limits.budget->used(), 2 * sizeof(float));
  }
  EXPECT_EQ(limits.budget->used(), 0u);  // the run's decodes lapse with it
  ScriptMedia tiny(decoder, limitedTo(sizeof(float) - 1), 0);
  const sdk::Image image;
  EXPECT_FALSE(tiny.decode(&image, image));
}

TEST(ScriptMediaTest, AnnotationsPairWithTheImageInputTheyName) {
  ScriptMedia media(nullptr, NativeLimits{}, 15);
  const int identity = 0;
  media.noteInput(&identity, "depth", "s:depth", 0);  // a stamp of 0 is a stamp
  EXPECT_EQ(media.pairing("depth"), (std::pair<std::string, Timestamp>{"depth", 0}));
  EXPECT_EQ(media.pairing("s:depth"), (std::pair<std::string, Timestamp>{"depth", 0}));
  EXPECT_EQ(media.pairing("/other"), (std::pair<std::string, Timestamp>{"/other", 15}));
  const int upstream = 0;
  media.noteInput(&upstream, "", "node/image", 7);  // an evaluation's upstream object has no store topic
  EXPECT_EQ(media.pairing("node/image"), (std::pair<std::string, Timestamp>{"node/image", 7}));
}

// A chained image has no store topic: its decoded pixels, and the annotations made from them, name it by its alias.
TEST(ScriptMediaTest, DecodedPixelsOfAChainedImageNameItByItsAlias) {
  ScriptMedia media(std::make_shared<CountingDecoder>(), limitedTo(16), 15);
  const sdk::Image upstream;
  media.noteInput(&upstream, "", "node/image", 7);
  auto decoded = media.decode(&upstream, upstream);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->image_topic, "node/image");
  EXPECT_EQ(decoded->timestamp_ns, 7);
}

/// A video whose frame at `t` presents at `t`: one rgb8 pixel with its own anchor.
class OnePixelVideo final : public IVideoFrameSource {
 public:
  Expected<VideoFrameResult> frameAt(
      ObjectTopicId /*topic*/, Timestamp t, const std::shared_ptr<MemoryBudget>& /*budget*/,
      const std::function<bool()>& /*cancelled*/) override {
    auto rgb = std::make_shared<std::vector<uint8_t>>(3, 1);
    VideoFrameResult frame;
    frame.presentation_ns = t;
    frame.image.encoding = "rgb8";
    frame.image.width = frame.image.height = 1;
    frame.image.data = {rgb->data(), rgb->size()};
    frame.image.anchor = rgb;
    return frame;
  }
  void retireDataset(DatasetId /*dataset*/) override {}
};

// A frame the script dropped never lends its identity to the next image at its address: no memoized pixels, no
// pairing (the allocator may reuse a collected image's address; the test hands the dead address over explicitly).
TEST(ScriptMediaTest, ACollectedFrameNeverLendsItsIdentity) {
  auto decoder = std::make_shared<CountingDecoder>();
  ScriptMedia media(decoder, limitedTo(1024), 0, std::make_shared<OnePixelVideo>());
  const sdk::VideoFrame packet;
  media.noteInput(&packet, "/video", "video", 5, ObjectTopicId{7});
  auto first = media.frameAt(&packet, 10);
  ASSERT_TRUE(first) << first.error();
  const void* dead = first->get<sdk::Image>();
  ASSERT_TRUE(media.decode(dead, *first->get<sdk::Image>()));
  first = sdk::BuiltinObject{};  // collected
  auto second = media.frameAt(&packet, 20);
  ASSERT_TRUE(second) << second.error();
  sdk::Image unrelated = *second->get<sdk::Image>();
  unrelated.timestamp_ns = 99;
  unrelated.anchor = std::make_shared<int>(0);  // another image, now living at the dead frame's address
  const auto decoded = media.decode(dead, unrelated);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoder->calls, 2) << "decoded anew, not the dead frame's memo";
  EXPECT_TRUE(decoded->image_topic.empty());
  EXPECT_EQ(decoded->timestamp_ns, 99);
}

// ---- benchmarks -------------------------------------------------------------------

// PJ_BENCH=1 prints the median ns per pixel of each operation over a 1920 x 1080 image (20 runs).
TEST(ImageOpsBench, NanosecondsPerPixelAt1080p) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr uint32_t kWidth = 1920;
  constexpr uint32_t kHeight = 1080;
  constexpr double kPixels = double(kWidth) * kHeight;
  std::mt19937 random(42);
  std::uniform_real_distribution<float> unit(0.0f, 255.0f);
  std::vector<float> rgb(std::size_t{kWidth} * kHeight * 3);
  std::generate(rgb.begin(), rgb.end(), [&] { return unit(random); });
  const DecodedPixels color = pixels(kWidth, kHeight, 3, std::move(rgb));
  std::vector<float> depth(std::size_t{kWidth} * kHeight);
  std::generate(depth.begin(), depth.end(), [&] { return 500.0f + 10.0f * unit(random); });
  const DecodedPixels mono = pixels(kWidth, kHeight, 1, std::move(depth));
  NativeLimits limits = limitedTo(std::size_t{1} << 30);
  limits.cancelled = [] { return false; };
  sdk::CameraInfo camera;
  camera.K = {1000, 0, 960, 0, 1000, 540, 0, 0, 1};
  const auto measure = [&](const char* name, auto&& call) {
    std::vector<double> ns;
    for (int run = 0; run < 20; ++run) {
      const auto start = std::chrono::steady_clock::now();
      call();
      ns.push_back(
          std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kPixels);
    }
    std::sort(ns.begin(), ns.end());
    std::printf("[bench] %-24s median %6.2f ns/pixel (min %6.2f)\n", name, (ns[9] + ns[10]) / 2.0, ns[0]);
  };
  measure("to_gray (rgb)", [&] { EXPECT_TRUE(native::imageToGray(color, limits)); });
  measure(
      "threshold (rgb, ~50%)", [&] { EXPECT_TRUE(native::imageThreshold(color, CompareOp::kGt, 127.5, 0, limits)); });
  measure(
      "threshold (rgb, ~1%)", [&] { EXPECT_TRUE(native::imageThreshold(color, CompareOp::kGt, 252.5, 0, limits)); });
  measure("stats (rgb)", [&] { EXPECT_TRUE(native::imageStats(color, limits)); });
  measure("crop (rgb, whole)", [&] { EXPECT_TRUE(native::imageCrop(color, 0, 0, kWidth, kHeight, limits)); });
  measure(
      "to_point_cloud (16UC1)", [&] { EXPECT_TRUE(native::depthToPointCloud(mono, "16UC1", 0, camera, {}, limits)); });
}

}  // namespace
}  // namespace PJ::scripting
