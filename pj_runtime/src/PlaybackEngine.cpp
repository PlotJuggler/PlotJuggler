// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/PlaybackEngine.h"

#include <algorithm>
#include <cmath>

namespace PJ {

namespace {
// Tick period, ~30 Hz. Nothing the cursor drives needs 60 Hz (video tops out ~30 Hz, ROS messages 10-20 Hz, the
// curve-list Value column is capped at 10 Hz); halving the tick halves the downstream replot/decode/composite work for
// every consumer at once, capped at the origin rather than throttled per widget. Playback speed is unaffected: onTick
// derives the cursor from elapsed wall-clock, not tick count.
constexpr int kTickIntervalMs = 33;  // ~30 Hz
}  // namespace

PlaybackEngine::PlaybackEngine(QObject* parent) : QObject(parent) {
  timer_.setInterval(kTickIntervalMs);
  connect(&timer_, &QTimer::timeout, this, &PlaybackEngine::onTick);
}

PlaybackEngine::~PlaybackEngine() = default;

void PlaybackEngine::setRange(DisplayRange range) {
  applyRange(range);

  const double clamped = clampedTime(current_time_);
  if (clamped != current_time_) {
    current_time_ = clamped;
    emit currentTimeChanged(current_time_);
  }
}

void PlaybackEngine::setRangeAndCurrentTime(DisplayRange range, DisplaySeconds t) {
  double min = range.min.value;
  double max = range.max.value;
  if (max < min) {
    std::swap(min, max);
  }
  const bool range_changed = range_min_ != min || range_max_ != max;
  range_min_ = min;
  range_max_ = max;
  const double clamped = clampedTime(t.value);
  const bool time_changed = clamped != current_time_;
  current_time_ = clamped;

  if (range_changed) {
    emit rangeChanged(range_min_, range_max_);
  }
  if (time_changed) {
    emit currentTimeChanged(current_time_);
  }
}

void PlaybackEngine::applyRange(DisplayRange range) {
  double min = range.min.value;
  double max = range.max.value;
  if (max < min) {
    std::swap(min, max);
  }
  if (range_min_ == min && range_max_ == max) {
    return;
  }
  range_min_ = min;
  range_max_ = max;
  emit rangeChanged(range_min_, range_max_);
}

void PlaybackEngine::setCurrentTime(DisplaySeconds t) {
  const double clamped = clampedTime(t.value);
  if (clamped == current_time_) {
    return;
  }
  current_time_ = clamped;
  emit currentTimeChanged(current_time_);
}

void PlaybackEngine::setPlaybackRate(double rate) {
  if (!std::isfinite(rate)) {
    return;
  }
  rate = snapPlaybackRate(rate);
  if (rate_ == rate) {
    return;
  }
  rate_ = rate;
  emit playbackRateChanged(rate_);
}

void PlaybackEngine::setStep(double step) {
  step_ = step;
}

void PlaybackEngine::setLooping(bool looping) {
  if (looping_ == looping) {
    return;
  }
  looping_ = looping;
  emit loopingChanged(looping_);
}

void PlaybackEngine::setHoldAtRangeMax(bool hold) {
  hold_at_range_max_ = hold;
}

double PlaybackEngine::clampTickTime(
    double next, double range_min, double range_max, bool hold_at_max, bool looping, bool* reached_end) {
  if (reached_end != nullptr) {
    *reached_end = false;
  }
  // Live streaming: the cursor is pinned to the live tip (range_max); it does NOT free-run forward by wall-clock
  // between ingests, since advancing only to clamp back made the needle overshoot the data and snap back. The loop
  // toggle is IGNORED so a stream never wraps to the start.
  if (hold_at_max) {
    return range_max;
  }
  if (next > range_max) {
    if (looping) {
      const double span = range_max - range_min;
      return span > 0.0 ? range_min + std::fmod(next - range_min, span) : range_min;
    }
    if (reached_end != nullptr) {
      *reached_end = true;  // hit the end with no repeat: the caller pauses
    }
    return range_max;
  }
  if (next < range_min) {
    return range_min;
  }
  return next;
}

void PlaybackEngine::play() {
  if (playing_) {
    return;
  }
  // Rewind-on-play: pressing Play with the cursor at (or past) the end of a finite range would otherwise do nothing:
  // the first onTick computes next > range_max, clampTickTime reports reached_end and onTick pauses again before the
  // cursor moves. Snap back to the start so Play replays from the beginning. Skipped while holding at the live tip
  // (streaming) and when looping (which wraps on its own).
  if (!hold_at_range_max_ && !looping_ && range_max_ > range_min_ && current_time_ >= range_max_) {
    current_time_ = range_min_;
    emit currentTimeChanged(current_time_);
  }
  playing_ = true;
  elapsed_.restart();
  timer_.start();
  emit playingChanged(playing_);
}

void PlaybackEngine::pause() {
  if (!playing_) {
    return;
  }
  playing_ = false;
  timer_.stop();
  emit playingChanged(playing_);
}

void PlaybackEngine::togglePlay() {
  if (playing_) {
    pause();
  } else {
    play();
  }
}

void PlaybackEngine::onTick() {
  const qint64 dt_ms = elapsed_.restart();
  const double dt = static_cast<double>(dt_ms) * 0.001;
  const double raw_next = current_time_ + dt * rate_;

  bool reached_end = false;
  const double next = clampTickTime(raw_next, range_min_, range_max_, hold_at_range_max_, looping_, &reached_end);
  if (reached_end) {
    pause();  // ran past the end with neither hold-at-tip nor loop
  }

  if (next != current_time_) {
    current_time_ = next;
    emit currentTimeChanged(current_time_);
  }
}

double PlaybackEngine::clampedTime(double t) const {
  return std::clamp(t, range_min_, range_max_);
}

}  // namespace PJ
