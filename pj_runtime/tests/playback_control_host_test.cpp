// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Drives PlaybackControlHost's raw C-ABI vtable through the SDK's
// PlaybackHostView against a real PlaybackEngine, so the test exercises the
// exact marshalling path a plugin uses.

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_runtime/PlaybackControlHost.h"
#include "pj_runtime/PlaybackEngine.h"

namespace {

PJ::PlaybackControlHost::TimeMapper fixedOffsetMapper(int64_t offset_ns) {
  return [offset_ns](std::string_view topic, int64_t absolute_ns) -> std::optional<double> {
    if (topic == "unknown/topic") {
      return std::nullopt;
    }
    return static_cast<double>(absolute_ns - offset_ns) * 1e-9;
  };
}

TEST(PlaybackControlHostTest, PlayPauseFlipEngineState) {
  PJ::PlaybackEngine engine;
  engine.setRange(PJ::displayRange(0.0, 10.0));
  PJ::PlaybackControlHost host(engine, {});
  PJ::sdk::PlaybackHostView view(host.raw());

  EXPECT_FALSE(engine.isPlaying());
  ASSERT_TRUE(view.play());
  EXPECT_TRUE(engine.isPlaying());
  ASSERT_TRUE(view.pause());
  EXPECT_FALSE(engine.isPlaying());
}

TEST(PlaybackControlHostTest, SeekClampsIntoRange) {
  PJ::PlaybackEngine engine;
  engine.setRange(PJ::displayRange(2.0, 10.0));
  PJ::PlaybackControlHost host(engine, {});
  PJ::sdk::PlaybackHostView view(host.raw());

  ASSERT_TRUE(view.seek(5.5));
  EXPECT_DOUBLE_EQ(engine.currentTime().value, 5.5);

  // Out-of-range seeks clamp (the engine's contract); the clamped value is
  // what a plugin reads back through get_state.
  ASSERT_TRUE(view.seek(99.0));
  EXPECT_DOUBLE_EQ(engine.currentTime().value, 10.0);
  ASSERT_TRUE(view.seek(-1.0));
  EXPECT_DOUBLE_EQ(engine.currentTime().value, 2.0);
}

TEST(PlaybackControlHostTest, SeekRejectsNonFinite) {
  PJ::PlaybackEngine engine;
  engine.setRange(PJ::displayRange(0.0, 10.0));
  engine.setCurrentTime(PJ::displaySeconds(3.0));
  PJ::PlaybackControlHost host(engine, {});
  PJ::sdk::PlaybackHostView view(host.raw());

  auto status = view.seek(std::numeric_limits<double>::quiet_NaN());
  EXPECT_FALSE(status);
  EXPECT_NE(status.error().find("finite"), std::string::npos);
  EXPECT_DOUBLE_EQ(engine.currentTime().value, 3.0);  // untouched
}

TEST(PlaybackControlHostTest, RateForwardsAndRejectsBadValues) {
  PJ::PlaybackEngine engine;
  PJ::PlaybackControlHost host(engine, {});
  PJ::sdk::PlaybackHostView view(host.raw());

  ASSERT_TRUE(view.setPlaybackRate(0.25));
  EXPECT_DOUBLE_EQ(engine.playbackRate(), 0.25);

  EXPECT_FALSE(view.setPlaybackRate(0.0));
  EXPECT_FALSE(view.setPlaybackRate(-2.0));
  EXPECT_FALSE(view.setPlaybackRate(std::numeric_limits<double>::infinity()));
  EXPECT_FALSE(view.setPlaybackRate(std::numeric_limits<double>::quiet_NaN()));
  EXPECT_DOUBLE_EQ(engine.playbackRate(), 0.25);  // untouched by the rejects

  // Host policy clamps absurd-but-finite multipliers.
  ASSERT_TRUE(view.setPlaybackRate(1e9));
  EXPECT_DOUBLE_EQ(engine.playbackRate(), 1000.0);
}

TEST(PlaybackControlHostTest, StateMatchesEngine) {
  PJ::PlaybackEngine engine;
  engine.setRange(PJ::displayRange(1.0, 20.0));
  engine.setCurrentTime(PJ::displaySeconds(7.5));
  engine.setPlaybackRate(2.0);
  PJ::PlaybackControlHost host(engine, {});
  PJ::sdk::PlaybackHostView view(host.raw());

  auto state = view.state();
  ASSERT_TRUE(state) << state.error();
  EXPECT_FALSE(state->is_playing);
  EXPECT_DOUBLE_EQ(state->current_time_s, 7.5);
  EXPECT_DOUBLE_EQ(state->range_min_s, 1.0);
  EXPECT_DOUBLE_EQ(state->range_max_s, 20.0);
  EXPECT_DOUBLE_EQ(state->playback_rate, 2.0);
}

TEST(PlaybackControlHostTest, ToDisplayTimeUsesMapper) {
  PJ::PlaybackEngine engine;
  PJ::PlaybackControlHost host(engine, fixedOffsetMapper(1'000'000'000));
  PJ::sdk::PlaybackHostView view(host.raw());

  auto display_s = view.toDisplayTime("imu/accel", 3'500'000'000);
  ASSERT_TRUE(display_s) << display_s.error();
  EXPECT_DOUBLE_EQ(*display_s, 2.5);
}

TEST(PlaybackControlHostTest, ToDisplayTimeUnknownTopicIsError) {
  PJ::PlaybackEngine engine;
  PJ::PlaybackControlHost host(engine, fixedOffsetMapper(0));
  PJ::sdk::PlaybackHostView view(host.raw());

  auto display_s = view.toDisplayTime("unknown/topic", 100);
  EXPECT_FALSE(display_s);
  EXPECT_NE(display_s.error().find("unknown or ambiguous topic"), std::string::npos);
}

TEST(PlaybackControlHostTest, ToDisplayTimeWithoutMapperIsError) {
  PJ::PlaybackEngine engine;
  PJ::PlaybackControlHost host(engine, {});
  PJ::sdk::PlaybackHostView view(host.raw());

  auto display_s = view.toDisplayTime("any", 0);
  EXPECT_FALSE(display_s);
  EXPECT_NE(display_s.error().find("does not support"), std::string::npos);
}

TEST(PlaybackControlHostTest, SourceConversionForwardsAndRejectsInvalidRequests) {
  PJ::PlaybackEngine engine;
  PJ_data_source_handle_t seen_source{};
  int64_t seen_ns = 0;
  PJ::PlaybackControlHost host(
      engine, {}, [&seen_source, &seen_ns](PJ_data_source_handle_t source, int64_t ns) -> std::optional<double> {
        seen_source = source;
        seen_ns = ns;
        if (source.id == 99) {
          throw std::runtime_error("conversion failure");
        }
        return source.id == 42 ? std::optional<double>(2.5) : std::nullopt;
      });
  PJ::sdk::PlaybackHostView view(host.raw());
  const auto result = view.toDisplayTimeForSource({42}, 3'500'000'000);
  ASSERT_TRUE(result);
  EXPECT_DOUBLE_EQ(*result, 2.5);
  EXPECT_EQ(seen_source.id, 42u);
  EXPECT_EQ(seen_ns, 3'500'000'000);

  EXPECT_FALSE(view.toDisplayTimeForSource({0}, 0));
  EXPECT_EQ(seen_source.id, 42u);  // invalid handle rejected before callback
  EXPECT_FALSE(view.toDisplayTimeForSource({43}, 0));
  const auto thrown = view.toDisplayTimeForSource({99}, 0);
  ASSERT_FALSE(thrown);
  EXPECT_NE(thrown.error().find("conversion failure"), std::string::npos);

  PJ_error_t error{};
  const auto raw = host.raw();
  EXPECT_FALSE(raw.vtable->to_display_time_for_source(raw.ctx, {42}, 0, nullptr, &error));
  PJ::PlaybackControlHost unsupported(engine, {});
  EXPECT_FALSE(PJ::sdk::PlaybackHostView(unsupported.raw()).toDisplayTimeForSource({42}, 0));
}

}  // namespace
