// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QMetaObject>
#include <QSignalSpy>
#include <cmath>
#include <limits>
#include <vector>

#include "pj_runtime/PlaybackEngine.h"

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

// A non-finite rate (typed "nan" into the speed scrubber) must not be stored: next tick would turn the cursor into NaN
// and playback would run forever with no time.
TEST(PlaybackEngineTest, NonFiniteRateIsIgnored) {
  PJ::PlaybackEngine engine;
  engine.setPlaybackRate(2.0);
  QSignalSpy rate_spy(&engine, &PJ::PlaybackEngine::playbackRateChanged);

  for (const double v :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    engine.setPlaybackRate(v);
    EXPECT_DOUBLE_EQ(engine.playbackRate(), 2.0);
  }
  EXPECT_EQ(rate_spy.count(), 0);
}

TEST(PlaybackEngineTest, TickAfterNonFiniteRateKeepsTimeFinite) {
  PJ::PlaybackEngine engine;
  engine.setRange(PJ::displayRange(0.0, 10.0));
  engine.setCurrentTime(PJ::displaySeconds(3.0));
  engine.setPlaybackRate(std::numeric_limits<double>::quiet_NaN());

  engine.play();
  QMetaObject::invokeMethod(&engine, "onTick", Qt::DirectConnection);
  engine.pause();

  EXPECT_TRUE(std::isfinite(engine.currentTime().value));
}

}  // namespace
