#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <memory>

#include "pj_runtime/Time.h"

namespace PJ {

// Authoritative tracker time + play/pause/loop state. Drives a QTimer while
// playing; every widget family subscribes to currentTimeChanged to stay in
// sync. Time is display-relative seconds (DisplaySeconds); the change SIGNALS
// stay bare double — they are a moc/vtable contract shared by every widget
// family's onTrackerTime(double), so they are deliberately not typed.
class PlaybackEngine : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<PlaybackEngine>;

  explicit PlaybackEngine(QObject* parent = nullptr);
  ~PlaybackEngine() override;

  PlaybackEngine(const PlaybackEngine&) = delete;
  PlaybackEngine& operator=(const PlaybackEngine&) = delete;

  [[nodiscard]] DisplaySeconds currentTime() const {
    return DisplaySeconds{current_time_};
  }
  [[nodiscard]] DisplaySeconds rangeMin() const {
    return DisplaySeconds{range_min_};
  }
  [[nodiscard]] DisplaySeconds rangeMax() const {
    return DisplaySeconds{range_max_};
  }
  double playbackRate() const {
    return rate_;
  }
  double step() const {
    return step_;
  }
  bool isPlaying() const {
    return playing_;
  }
  bool isLooping() const {
    return looping_;
  }
  /// The cursor parks at the range's end while playing (live streaming follows its newest sample).
  bool holdsAtRangeMax() const {
    return hold_at_range_max_;
  }

  // Compute the clamped cursor time for one playback tick. Pure + static so the
  // branch precedence is unit-testable. While `hold_at_max` is set (live streaming),
  // parking at `range_max` takes precedence over `looping`, so a live stream stays
  // glued to the tip even if the loop toggle is on. Returns the clamped time; sets
  // *reached_end true only when neither hold nor loop applies and the cursor ran past
  // range_max (the caller then pauses). `reached_end` may be null.
  [[nodiscard]] static double clampTickTime(
      double next, double range_min, double range_max, bool hold_at_max, bool looping, bool* reached_end);

 public slots:
  void setRange(DisplayRange range);
  void setRangeAndCurrentTime(DisplayRange range, DisplaySeconds t);
  void setCurrentTime(DisplaySeconds t);
  void setPlaybackRate(double rate);
  void setStep(double step);
  void setLooping(bool looping);
  void setHoldAtRangeMax(bool hold);
  void play();
  void pause();
  void togglePlay();

 signals:
  void currentTimeChanged(double t);
  void rangeChanged(double min, double max);
  void playbackRateChanged(double rate);
  void playingChanged(bool playing);
  void loopingChanged(bool looping);

 private slots:
  void onTick();

 private:
  double clampedTime(double t) const;
  void applyRange(DisplayRange range);

  double current_time_ = 0.0;
  double range_min_ = 0.0;
  double range_max_ = 1.0;
  double rate_ = 1.0;
  double step_ = 0.0;
  bool playing_ = false;
  bool looping_ = false;
  bool hold_at_range_max_ = false;

  QTimer timer_;
  QElapsedTimer elapsed_;
};

}  // namespace PJ
