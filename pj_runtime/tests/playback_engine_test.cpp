// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <vector>

#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/PlaybackRate.h"

namespace {

TEST(PlaybackEngineTest, SetRangeAndCurrentTimeSkipsIntermediateClamp) {
  PJ::PlaybackEngine engine;
  engine.setRange(PJ::displayRange(0.0, 10.0));
  engine.setCurrentTime(PJ::displaySeconds(5.0));

  std::vector<double> current_times;
  std::vector<double> current_times_seen_by_range;
  QObject::connect(
      &engine, &PJ::PlaybackEngine::rangeChanged, &engine, [&engine, &current_times_seen_by_range](double, double) {
        current_times_seen_by_range.push_back(engine.currentTime().value);
      });
  QObject::connect(&engine, &PJ::PlaybackEngine::currentTimeChanged, &engine, [&current_times](double time) {
    current_times.push_back(time);
  });

  engine.setRangeAndCurrentTime(PJ::displayRange(20.0, 30.0), PJ::displaySeconds(30.0));

  ASSERT_EQ(current_times.size(), 1u);
  EXPECT_DOUBLE_EQ(current_times.front(), 30.0);
  ASSERT_EQ(current_times_seen_by_range.size(), 1u);
  EXPECT_DOUBLE_EQ(current_times_seen_by_range.front(), 30.0);
  EXPECT_DOUBLE_EQ(engine.rangeMin().value, 20.0);
  EXPECT_DOUBLE_EQ(engine.rangeMax().value, 30.0);
  EXPECT_DOUBLE_EQ(engine.currentTime().value, 30.0);
}

// Per-tick clamp precedence: while a stream is live (hold-at-tip), the cursor parks
// at the live edge and IGNORES the loop toggle, so a live stream never wraps to the
// start. Loop only wraps when not holding; with neither, it flags end-of-range so
// the engine pauses.
TEST(PlaybackEngineTest, ClampTickTimeHoldAtTipBeatsLoop) {
  bool reached_end = true;

  EXPECT_DOUBLE_EQ(
      PJ::PlaybackEngine::clampTickTime(15.0, 0.0, 10.0, /*hold_at_max=*/true, /*looping=*/true, &reached_end), 10.0);
  EXPECT_FALSE(reached_end);

  EXPECT_DOUBLE_EQ(
      PJ::PlaybackEngine::clampTickTime(12.0, 0.0, 10.0, /*hold_at_max=*/false, /*looping=*/true, &reached_end), 2.0);
  EXPECT_FALSE(reached_end);

  reached_end = false;
  EXPECT_DOUBLE_EQ(
      PJ::PlaybackEngine::clampTickTime(12.0, 0.0, 10.0, /*hold_at_max=*/false, /*looping=*/false, &reached_end), 10.0);
  EXPECT_TRUE(reached_end);

  // hold-at-tip pins to range_max even when the tick would land below it: the cursor must not free-run forward between
  // ingests.
  EXPECT_DOUBLE_EQ(
      PJ::PlaybackEngine::clampTickTime(5.0, 0.0, 10.0, /*hold_at_max=*/true, /*looping=*/false, nullptr), 10.0);

  EXPECT_DOUBLE_EQ(PJ::PlaybackEngine::clampTickTime(5.0, 0.0, 10.0, false, true, nullptr), 5.0);
  EXPECT_DOUBLE_EQ(PJ::PlaybackEngine::clampTickTime(-3.0, 0.0, 10.0, false, false, nullptr), 0.0);
}

// Whatever asks for a rate (the scrubber, a plugin, the MCP tool), the engine plays at one inside the range the speed
// scrubber can show, so the two never disagree.
TEST(PlaybackEngineTest, RateIsClampedToTheSharedRange) {
  PJ::PlaybackEngine engine;

  engine.setPlaybackRate(PJ::kMaxPlaybackRate * 2.0);
  EXPECT_DOUBLE_EQ(engine.playbackRate(), PJ::kMaxPlaybackRate);

  engine.setPlaybackRate(PJ::kMinPlaybackRate / 2.0);
  EXPECT_DOUBLE_EQ(engine.playbackRate(), PJ::kMinPlaybackRate);

  engine.setPlaybackRate(2.5);
  EXPECT_DOUBLE_EQ(engine.playbackRate(), 2.5);
}

// The speed scrubber shows one decimal and commits what it shows, so the engine plays at a multiple of its step.
TEST(PlaybackEngineTest, RateIsRoundedToTheScrubberStep) {
  PJ::PlaybackEngine engine;

  engine.setPlaybackRate(0.25);
  EXPECT_DOUBLE_EQ(engine.playbackRate(), 0.3);

  engine.setPlaybackRate(2.34);
  EXPECT_DOUBLE_EQ(engine.playbackRate(), 2.3);

  engine.setPlaybackRate(0.04);  // rounds to 0, then clamps up to the minimum
  EXPECT_DOUBLE_EQ(engine.playbackRate(), PJ::kMinPlaybackRate);
}

// Pressing Play with the cursor already at the end of a finite range rewinds to the start and replays instead of a
// no-op (the first tick would pause again before moving).
TEST(PlaybackEngineTest, PlayAtEndRewindsToStart) {
  PJ::PlaybackEngine engine;
  engine.setRange(PJ::displayRange(0.0, 10.0));
  engine.setCurrentTime(PJ::displaySeconds(10.0));  // cursor parked at the end

  engine.play();

  EXPECT_TRUE(engine.isPlaying()) << "play() must actually start playback";
  EXPECT_DOUBLE_EQ(engine.currentTime().value, 0.0) << "play() at the end must rewind to range_min";
}

// Looping wraps on its own, so play() must not force a rewind; clampTickTime handles the wrap.
TEST(PlaybackEngineTest, PlayAtEndDoesNotRewindWhenLooping) {
  PJ::PlaybackEngine engine;
  engine.setRange(PJ::displayRange(0.0, 10.0));
  engine.setLooping(true);
  engine.setCurrentTime(PJ::displaySeconds(10.0));

  engine.play();

  EXPECT_TRUE(engine.isPlaying());
  EXPECT_DOUBLE_EQ(engine.currentTime().value, 10.0) << "looping play() must not force a rewind";
}

// Play from the middle of the range leaves the cursor untouched (only the at-end
// case rewinds).
TEST(PlaybackEngineTest, PlayFromMiddleDoesNotRewind) {
  PJ::PlaybackEngine engine;
  engine.setRange(PJ::displayRange(0.0, 10.0));
  engine.setCurrentTime(PJ::displaySeconds(4.0));

  engine.play();

  EXPECT_DOUBLE_EQ(engine.currentTime().value, 4.0) << "play() mid-range must not move the cursor";
}

}  // namespace
