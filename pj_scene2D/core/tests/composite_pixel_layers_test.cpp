// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "pj_scene2d_core/composite_media_source.h"

namespace PJ {
namespace {

class PixelSource final : public MediaSource {
 public:
  explicit PixelSource(int width, int64_t overlay_ts) : width_(width), overlay_ts_(overlay_ts) {}

  void setTimestamp(int64_t /*ts_ns*/) override {
    MediaFrame frame;
    DecodedFrame base;
    base.width = width_;
    base.height = 1;
    base.format = PixelFormat::kRGB888;
    base.pixels = std::make_shared<std::vector<uint8_t>>(3, static_cast<uint8_t>(width_));
    frame.base = base;

    SceneFrame overlay;
    overlay.timestamp = overlay_ts_;
    frame.overlays.push_back(std::move(overlay));
    pending_ = std::move(frame);
  }

  std::optional<MediaFrame> takeFrame() override {
    auto out = std::move(pending_);
    pending_.reset();
    return out;
  }

 private:
  int width_ = 0;
  int64_t overlay_ts_ = 0;
  std::optional<MediaFrame> pending_;
};

TEST(CompositePixelLayersTest, BasesBecomeOrderedPixelLayersWithOpacity) {
  CompositeMediaSource composite;
  composite.addLayer(std::make_unique<PixelSource>(10, 100), 0.25f);
  composite.addLayer(std::make_unique<PixelSource>(20, 200), 0.75f);

  composite.setTimestamp(1'000);
  auto frame = composite.takeFrame();

  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_EQ(frame->base->width, 10);

  ASSERT_EQ(frame->pixel_layers.size(), 2u);
  EXPECT_EQ(frame->pixel_layers[0].frame.width, 10);
  EXPECT_FLOAT_EQ(frame->pixel_layers[0].opacity, 0.25f);
  EXPECT_EQ(frame->pixel_layers[1].frame.width, 20);
  EXPECT_FLOAT_EQ(frame->pixel_layers[1].opacity, 0.75f);
}

TEST(CompositePixelLayersTest, ExistingPixelLayersAreForwardedAndOverlaysConcatenate) {
  class LayeredSource final : public MediaSource {
   public:
    void setTimestamp(int64_t /*ts_ns*/) override {
      MediaFrame frame;
      DecodedFrame a;
      a.width = 1;
      frame.pixel_layers.push_back(PixelLayer{a, 0.5f});
      DecodedFrame b;
      b.width = 2;
      frame.pixel_layers.push_back(PixelLayer{b, 0.25f});
      SceneFrame overlay;
      overlay.timestamp = 300;
      frame.overlays.push_back(std::move(overlay));
      pending_ = std::move(frame);
    }

    std::optional<MediaFrame> takeFrame() override {
      auto out = std::move(pending_);
      pending_.reset();
      return out;
    }

   private:
    std::optional<MediaFrame> pending_;
  };

  CompositeMediaSource composite;
  composite.addLayer(std::make_unique<PixelSource>(10, 100), 1.0f);
  composite.addLayer(std::make_unique<LayeredSource>(), 0.5f);

  composite.setTimestamp(1'000);
  auto frame = composite.takeFrame();

  ASSERT_TRUE(frame.has_value());
  ASSERT_EQ(frame->pixel_layers.size(), 3u);
  EXPECT_EQ(frame->pixel_layers[0].frame.width, 10);
  EXPECT_EQ(frame->pixel_layers[1].frame.width, 1);
  EXPECT_FLOAT_EQ(frame->pixel_layers[1].opacity, 0.25f);
  EXPECT_EQ(frame->pixel_layers[2].frame.width, 2);
  EXPECT_FLOAT_EQ(frame->pixel_layers[2].opacity, 0.125f);

  ASSERT_EQ(frame->overlays.size(), 2u);
  EXPECT_EQ(frame->overlays[0].timestamp, 100);
  EXPECT_EQ(frame->overlays[1].timestamp, 300);
}

// Produces a single-base frame only when `produce` is set, modelling a layer
// that has nothing new on a given tick (a sub-source dedup returns nullopt).
class GatedSource final : public MediaSource {
 public:
  explicit GatedSource(int width) : width_(width) {}

  bool produce = true;

  void setTimestamp(int64_t /*ts_ns*/) override {}

  std::optional<MediaFrame> takeFrame() override {
    if (!produce) {
      return std::nullopt;
    }
    MediaFrame frame;
    DecodedFrame base;
    base.width = width_;
    base.height = 1;
    base.format = PixelFormat::kRGB888;
    base.pixels = std::make_shared<std::vector<uint8_t>>(3, static_cast<uint8_t>(width_));
    frame.base = base;
    return frame;
  }

 private:
  int width_ = 0;
};

// A layer with nothing new on a tick keeps its last contribution while another layer updates.
TEST(CompositePixelLayersTest, NonUpdatingLayerIsRetainedAcrossTicks) {
  CompositeMediaSource composite;
  auto a = std::make_unique<GatedSource>(10);
  auto b = std::make_unique<GatedSource>(20);
  auto* b_ptr = b.get();
  composite.addLayer(std::move(a), 1.0f);
  composite.addLayer(std::move(b), 1.0f);

  // Tick 1: both layers produce.
  composite.setTimestamp(1);
  auto first = composite.takeFrame();
  ASSERT_TRUE(first.has_value());
  ASSERT_EQ(first->pixel_layers.size(), 2u);

  // Tick 2: only layer A produces; layer B reports nothing new.
  b_ptr->produce = false;
  composite.setTimestamp(2);
  auto second = composite.takeFrame();

  ASSERT_TRUE(second.has_value());             // A updated -> the composite is re-emitted
  ASSERT_EQ(second->pixel_layers.size(), 2u);  // B retained, not dropped
  EXPECT_EQ(second->pixel_layers[0].frame.width, 10);
  EXPECT_EQ(second->pixel_layers[1].frame.width, 20);
}

// When no layer updates on a tick, there is nothing new to deliver.
TEST(CompositePixelLayersTest, NoLayerUpdateReturnsNullopt) {
  CompositeMediaSource composite;
  auto a = std::make_unique<GatedSource>(10);
  auto* a_ptr = a.get();
  composite.addLayer(std::move(a), 1.0f);

  composite.setTimestamp(1);
  ASSERT_TRUE(composite.takeFrame().has_value());

  a_ptr->produce = false;
  composite.setTimestamp(2);
  EXPECT_FALSE(composite.takeFrame().has_value());
}

}  // namespace
}  // namespace PJ
