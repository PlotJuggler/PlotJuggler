// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/composite_media_source.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_scene2d_core/borrowed_media_source.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_core/undistort_remap.h"

namespace PJ {
namespace {

// Mock source that emits a configurable MediaFrame on the next takeFrame()
// after setTimestamp(). Tracks how many times setTimestamp was called.
class MockSource final : public MediaSource {
 public:
  int set_calls = 0;
  int invalidate_calls = 0;
  bool emit_base = false;
  int n_overlays = 0;
  int64_t base_w = 0;
  int gpu_rectify_calls = 0;
  bool last_gpu_rectify = false;

  void invalidate() override {
    ++invalidate_calls;
  }

  void setGpuRectificationAvailable(bool available) override {
    ++gpu_rectify_calls;
    last_gpu_rectify = available;
  }

  std::shared_ptr<const UndistortMap> base_rectify_map;  // attached to the emitted base frame when set

  void setTimestamp(int64_t /*ts_ns*/) override {
    ++set_calls;
    pending_.emplace();
    if (emit_base) {
      DecodedFrame df;
      df.width = static_cast<int>(base_w);
      df.rectify_map = base_rectify_map;
      pending_->base = df;
    }
    for (int i = 0; i < n_overlays; ++i) {
      SceneFrame sf;
      sf.timestamp = static_cast<int64_t>(1000 + i);
      pending_->overlays.push_back(std::move(sf));
    }
  }

  std::optional<MediaFrame> takeFrame() override {
    if (!pending_.has_value()) {
      return std::nullopt;
    }
    auto out = std::move(*pending_);
    pending_.reset();
    return out;
  }

 private:
  std::optional<MediaFrame> pending_;
};

TEST(CompositeMediaSourceTest, EmptyCompositorReturnsNullopt) {
  CompositeMediaSource composite;
  composite.setTimestamp(0);
  EXPECT_FALSE(composite.takeFrame().has_value());
  EXPECT_EQ(composite.layerCount(), 0u);
}

TEST(CompositeMediaSourceTest, NullLayerIgnored) {
  CompositeMediaSource composite;
  composite.addLayer(nullptr);
  EXPECT_EQ(composite.layerCount(), 0u);
}

TEST(CompositeMediaSourceTest, ImageOnlyLayerForwarded) {
  auto image = std::make_unique<MockSource>();
  image->emit_base = true;
  image->base_w = 640;

  CompositeMediaSource composite;
  composite.addLayer(std::move(image));
  composite.setTimestamp(100);

  auto frame = composite.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_EQ(frame->base->width, 640);
  EXPECT_TRUE(frame->overlays.empty());
}

TEST(CompositeMediaSourceTest, OverlayOnlyLayerForwarded) {
  auto markers = std::make_unique<MockSource>();
  markers->n_overlays = 2;

  CompositeMediaSource composite;
  composite.addLayer(std::move(markers));
  composite.setTimestamp(100);

  auto frame = composite.takeFrame();
  ASSERT_TRUE(frame.has_value());
  EXPECT_FALSE(frame->base.has_value());
  EXPECT_EQ(frame->overlays.size(), 2u);
}

TEST(CompositeMediaSourceTest, ImagePlusMarkersFused) {
  auto image = std::make_unique<MockSource>();
  image->emit_base = true;
  image->base_w = 1920;
  auto markers = std::make_unique<MockSource>();
  markers->n_overlays = 3;

  CompositeMediaSource composite;
  composite.addLayer(std::move(image));
  composite.addLayer(std::move(markers));
  composite.setTimestamp(0);

  auto frame = composite.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_EQ(frame->base->width, 1920);
  EXPECT_EQ(frame->overlays.size(), 3u);
}

TEST(CompositeMediaSourceTest, MultipleOverlayLayersConcatenateInOrder) {
  auto first = std::make_unique<MockSource>();
  first->n_overlays = 2;
  auto second = std::make_unique<MockSource>();
  second->n_overlays = 1;

  CompositeMediaSource composite;
  composite.addLayer(std::move(first));
  composite.addLayer(std::move(second));
  composite.setTimestamp(0);

  auto frame = composite.takeFrame();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->overlays.size(), 3u);
  // first layer's overlays come first (timestamps 1000, 1001), then second (1000)
  EXPECT_EQ(frame->overlays[0].timestamp, 1000);
  EXPECT_EQ(frame->overlays[1].timestamp, 1001);
  EXPECT_EQ(frame->overlays[2].timestamp, 1000);
}

TEST(CompositeMediaSourceTest, MultipleBasesFirstWins) {
  auto a = std::make_unique<MockSource>();
  a->emit_base = true;
  a->base_w = 100;
  auto b = std::make_unique<MockSource>();
  b->emit_base = true;
  b->base_w = 200;

  CompositeMediaSource composite;
  composite.addLayer(std::move(a));
  composite.addLayer(std::move(b));
  composite.setTimestamp(0);

  auto frame = composite.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_EQ(frame->base->width, 100);  // first layer's base wins
}

TEST(CompositeMediaSourceTest, SetTimestampForwardsToAllLayers) {
  auto a = std::make_unique<MockSource>();
  auto b = std::make_unique<MockSource>();
  auto* a_ptr = a.get();
  auto* b_ptr = b.get();

  CompositeMediaSource composite;
  composite.addLayer(std::move(a));
  composite.addLayer(std::move(b));

  composite.setTimestamp(100);
  composite.setTimestamp(200);
  composite.setTimestamp(300);

  EXPECT_EQ(a_ptr->set_calls, 3);
  EXPECT_EQ(b_ptr->set_calls, 3);
}

TEST(CompositeMediaSourceTest, GpuRectificationAvailabilityFansOutToAllLayers) {
  auto a = std::make_unique<MockSource>();
  auto b = std::make_unique<MockSource>();
  auto* a_ptr = a.get();
  auto* b_ptr = b.get();

  CompositeMediaSource composite;
  composite.addLayer(std::move(a));
  composite.addLayer(std::move(b));

  composite.setGpuRectificationAvailable(true);

  EXPECT_EQ(a_ptr->gpu_rectify_calls, 1);
  EXPECT_TRUE(a_ptr->last_gpu_rectify);
  EXPECT_EQ(b_ptr->gpu_rectify_calls, 1);
  EXPECT_TRUE(b_ptr->last_gpu_rectify);
}

TEST(CompositeMediaSourceTest, PreservesRectifyMapWhenPromotingBaseToPixelLayer) {
  // A camera image enters the composite as a layer's `base` and is reassembled
  // into the composite's `pixel_layers`. The deferred-GPU-rectify handle must
  // survive that promotion, or the widget's pixel-layer path cannot rectify it
  // (the bug: composited cameras rendered raw + misaligned overlays).
  UndistortMap built;
  built.out_width = 1920;
  built.out_height = 886;
  built.src_width = 480;
  built.src_height = 221;
  auto map = std::make_shared<const UndistortMap>(built);
  auto layer = std::make_unique<MockSource>();
  auto* layer_ptr = layer.get();
  layer_ptr->emit_base = true;
  layer_ptr->base_w = 480;
  layer_ptr->base_rectify_map = map;

  CompositeMediaSource composite;
  composite.addLayer(std::move(layer));
  composite.setTimestamp(100);

  auto frame = composite.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_FALSE(frame->pixel_layers.empty());
  ASSERT_NE(frame->pixel_layers[0].frame.rectify_map, nullptr);
  EXPECT_EQ(frame->pixel_layers[0].frame.rectify_map->out_width, 1920);
}

TEST(CompositeMediaSourceTest, NoNewDataReturnsNullopt) {
  auto layer = std::make_unique<MockSource>();
  CompositeMediaSource composite;
  composite.addLayer(std::move(layer));

  // No setTimestamp call → mock's pending_ is empty → takeFrame returns nullopt
  EXPECT_FALSE(composite.takeFrame().has_value());
}

// Reproduces the "black on composite rebuild" symptom. On every layer change
// (add / remove / show-hide) Scene2DDockWidget builds a FRESH CompositeMediaSource
// wrapping the same underlying sources. The rebuilt composite starts with empty
// per-layer caches, so its first takeFrame() yields nothing until every source
// re-decodes — which for an async/lazy source (ImagePipelineSource, video) lands
// a moment later, leaving the viewer black in between. adoptContributions() must
// carry a persisting layer's last fused output into the rebuilt composite so it
// paints the current frame immediately, with no re-decode.
TEST(CompositeMediaSourceTest, RebuiltCompositeRetainsPersistingLayerFrame) {
  MockSource src;
  src.emit_base = true;
  src.base_w = 640;

  // Original composite: the source has produced and shown a frame at t=100.
  CompositeMediaSource before;
  before.addLayer(std::make_unique<BorrowedMediaSource>(&src), 1.0f, &src);
  before.setTimestamp(100);
  ASSERT_TRUE(before.takeFrame().has_value());

  // Rebuild at the SAME tracker time, wrapping the SAME underlying source.
  CompositeMediaSource after;
  after.addLayer(std::make_unique<BorrowedMediaSource>(&src), 1.0f, &src);
  after.adoptContributions(before);

  // Without any new setTimestamp()/decode the rebuilt composite must still yield
  // the current frame (otherwise: black until the async re-decode lands).
  const int set_calls_before = src.set_calls;
  auto frame = after.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_FALSE(frame->pixel_layers.empty());
  EXPECT_EQ(frame->pixel_layers.front().frame.width, 640);
  EXPECT_EQ(src.set_calls, set_calls_before);  // carried over, not re-decoded
}

// Complements RebuiltCompositeRetainsPersistingLayerFrame. Carry-over keeps
// PERSISTING layers visible across a rebuild without re-decoding; invalidate()
// is the other half — it must reach every layer's source so a source can drop
// its per-timestamp dedup and re-decode at the unchanged time. That path is what
// covers a re-shown previously-hidden layer (which carry-over CANNOT carry, since
// it was excluded from the prior composite). So the two are not redundant.
TEST(CompositeMediaSourceTest, InvalidateFansOutThroughBorrowedToSources) {
  MockSource a;
  MockSource b;
  CompositeMediaSource composite;
  composite.addLayer(std::make_unique<BorrowedMediaSource>(&a), 1.0f, &a);
  composite.addLayer(std::make_unique<BorrowedMediaSource>(&b), 1.0f, &b);

  composite.invalidate();
  EXPECT_EQ(a.invalidate_calls, 1);  // composite -> BorrowedMediaSource -> source
  EXPECT_EQ(b.invalidate_calls, 1);
}

// SCN-11 (changed by D30: no readiness gate): a derived overlay whose node computes publishes nothing new, so at
// the next cursor the composite keeps its previous overlay beside the fresh image.
TEST(CompositeMediaSourceTest, PendingDerivedLayerKeepsItsPreviousOverlay) {
  MockSource image;
  image.emit_base = true;
  image.base_w = 640;
  MockSource overlay;
  overlay.n_overlays = 1;
  CompositeMediaSource composite;
  composite.addLayer(std::make_unique<BorrowedMediaSource>(&image), 1.0f, &image);
  composite.addLayer(std::make_unique<BorrowedMediaSource>(&overlay), 1.0f, &overlay);
  composite.setTimestamp(100);
  ASSERT_TRUE(composite.takeFrame().has_value());

  composite.setTimestamp(200);
  (void)overlay.takeFrame();  // the pending node's ring shows nothing new at 200
  const auto frame = composite.takeFrame();
  ASSERT_TRUE(frame.has_value());
  EXPECT_FALSE(frame->pixel_layers.empty());
  EXPECT_EQ(frame->overlays.size(), 1u) << "the previous result stays shown while the next computes";
}

// SCN-12 (changed by D30): a missing input or an error publishes an empty tombstone, which replaces the overlay:
// the image stays, with no overlay.
TEST(CompositeMediaSourceTest, ATombstoneShowsTheImageWithAnEmptyOverlay) {
  MockSource image;
  image.emit_base = true;
  image.base_w = 640;
  MockSource overlay;
  overlay.n_overlays = 1;
  CompositeMediaSource composite;
  composite.addLayer(std::make_unique<BorrowedMediaSource>(&image), 1.0f, &image);
  composite.addLayer(std::make_unique<BorrowedMediaSource>(&overlay), 1.0f, &overlay);
  composite.setTimestamp(100);
  ASSERT_TRUE(composite.takeFrame().has_value());

  overlay.n_overlays = 0;  // the tombstone: an empty annotation set
  composite.setTimestamp(200);
  const auto frame = composite.takeFrame();
  ASSERT_TRUE(frame.has_value());
  EXPECT_FALSE(frame->pixel_layers.empty());
  EXPECT_TRUE(frame->overlays.empty());
}

// ---- pairing (MED-23) ----

/// Hands out `next` once.
class PairingSource final : public MediaSource {
 public:
  std::optional<MediaFrame> next;
  void setTimestamp(int64_t /*ts_ns*/) override {}
  std::optional<MediaFrame> takeFrame() override {
    return std::exchange(next, std::nullopt);
  }
};

TEST(CompositeMediaSource, PairingUsesDeliveredPixelsAndRechecksCachedAnnotations) {
  auto image = std::make_unique<PairingSource>();
  auto annotations = std::make_unique<PairingSource>();
  auto* image_source = image.get();
  auto* annotation_source = annotations.get();
  CompositeMediaSource composite;
  composite.addLayer(std::move(image), 1, image_source);
  composite.addLayer(std::move(annotations), 1, annotation_source);
  std::string warning;
  composite.setLayerPairing(image_source, "/cam", false, [](int64_t) { return 200'000'000; }, {});
  composite.setLayerPairing(annotation_source, "", true, {}, [&](const std::string& text) { warning = text; });
  ImageAnnotation annotation;
  annotation.image_topic = "/cam";
  annotation.timestamp = 1'000'000'000;
  annotation.circles.emplace_back();
  annotation_source->next.emplace();
  annotation_source->next->overlays.push_back(SceneFrame{annotation.timestamp, {annotation}});
  auto result = composite.takeFrame();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->overlays.empty());  // no image delivered yet: never paired with the tracker's instant
  EXPECT_FALSE(warning.empty());

  image_source->next.emplace();
  image_source->next->base.emplace();
  image_source->next->base->pts = 1'200'000'000;  // exactly one cadence away
  result = composite.takeFrame();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->overlays.size(), 1u);  // the retained annotation is checked again once the image arrives
  EXPECT_TRUE(warning.empty());

  image_source->next.emplace();
  image_source->next->base.emplace();
  image_source->next->base->pts = 1'200'000'001;
  result = composite.takeFrame();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->overlays.empty());
  EXPECT_NE(warning.find("/cam"), std::string::npos) << warning;

  annotation.image_topic = "/other";  // another camera's annotations, at the shown image's time
  annotation.timestamp = 1'200'000'001;
  annotation_source->next.emplace();
  annotation_source->next->overlays.push_back(SceneFrame{annotation.timestamp, {annotation}});
  result = composite.takeFrame();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->overlays.empty());
  EXPECT_FALSE(warning.empty());

  // An annotation that names no image is never checked; an empty (cleared) overlay still clears.
  annotation.image_topic.clear();
  annotation_source->next.emplace();
  annotation_source->next->overlays.push_back(SceneFrame{annotation.timestamp, {annotation}});
  annotation_source->next->overlays.push_back(SceneFrame{annotation.timestamp, {}});
  result = composite.takeFrame();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->overlays.size(), 2u);
  EXPECT_TRUE(warning.empty());
}

// A layer not set to pair its annotations (a parser's, whose image_topic is informational) shows them as they are.
TEST(CompositeMediaSource, AnnotationsOfALayerNotPairingThemAreNeverChecked) {
  auto image = std::make_unique<PairingSource>();
  auto annotations = std::make_unique<PairingSource>();
  auto* image_source = image.get();
  auto* annotation_source = annotations.get();
  CompositeMediaSource composite;
  composite.addLayer(std::move(image), 1, image_source);
  composite.addLayer(std::move(annotations), 1, annotation_source);
  std::string warning = "unset";
  composite.setLayerPairing(image_source, "/cam", false, {}, {});
  composite.setLayerPairing(annotation_source, "", false, {}, [&](const std::string& text) { warning = text; });
  image_source->next.emplace();
  image_source->next->base.emplace();
  image_source->next->base->pts = 1'000'000'000;
  ImageAnnotation annotation;
  annotation.image_topic = "frame_7";  // a header frame id, not the shown image's topic
  annotation.timestamp = 9'000'000'000;
  annotation.circles.emplace_back();
  annotation_source->next.emplace();
  annotation_source->next->overlays.push_back(SceneFrame{annotation.timestamp, {annotation}});
  const auto result = composite.takeFrame();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->overlays.size(), 1u);
  EXPECT_TRUE(warning.empty());
}

// Annotations made from an image carry its own stamp, which may differ from its store entry's time: pairing compares
// that stamp, while the cadence is looked up around the entry (not the frame's pts, a parser's record stamp).
TEST(CompositeMediaSource, PairingComparesTheImagesOwnStampAndLooksUpCadenceAtTheEntry) {
  auto image = std::make_unique<PairingSource>();
  auto annotations = std::make_unique<PairingSource>();
  auto* image_source = image.get();
  auto* annotation_source = annotations.get();
  CompositeMediaSource composite;
  composite.addLayer(std::move(image), 1, image_source);
  composite.addLayer(std::move(annotations), 1, annotation_source);
  std::vector<int64_t> cadence_at;
  composite.setLayerPairing(
      image_source, "/cam", false,
      [&](int64_t entry_ts) {
        cadence_at.push_back(entry_ts);
        return 100'000'000;
      },
      {});
  composite.setLayerPairing(annotation_source, "", true, {}, {});
  image_source->next.emplace();
  image_source->next->base.emplace();
  image_source->next->base->pts = 4'000'000'000;  // what a parser reported
  image_source->next->base->entry_ts = 5'000'000'000;
  image_source->next->base->stamp = 3'000'000'000;  // the image's own stamp
  for (const int64_t stamp : {3'000'000'000, 5'000'000'000}) {
    SCOPED_TRACE(stamp);
    ImageAnnotation annotation;
    annotation.image_topic = "/cam";
    annotation.timestamp = stamp;
    annotation.circles.emplace_back();
    annotation_source->next.emplace();
    annotation_source->next->overlays.push_back(SceneFrame{stamp, {annotation}});
    const auto result = composite.takeFrame();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->overlays.size(), stamp == 3'000'000'000 ? 1u : 0u);
  }
  EXPECT_EQ(cadence_at, (std::vector<int64_t>{5'000'000'000, 5'000'000'000}));
}

}  // namespace
}  // namespace PJ
