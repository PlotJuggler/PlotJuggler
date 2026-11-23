#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QTimer>
#include <QWidget>

#include "pj_widgets/ChromeMetrics.h"
#include "pj_widgets/TimeFormat.h"

namespace Ui {
class TimelineWidget;
}

namespace PJ {

class PlaybackEngine;

// The bottom time strip, a view over PlaybackEngine: gestures call into the engine, engine signals update the
// display.
class TimelineWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TimelineWidget(QWidget* parent = nullptr);
  ~TimelineWidget() override;

  void setPlaybackEngine(PlaybackEngine* engine);
  void setSeekLocked(bool locked);

  /// Does not emit useDateTimeToggled.
  void setUseDateTime(bool checked);
  /// Does not emit useTimeOffsetToggled.
  void setUseTimeOffset(bool checked);
  /// Wall-clock formats add the date, which the plot axis omits, so the readout pins the exact instant.
  void setTimeLabelFormat(TimeLabelFormat format);

 signals:
  /// The user flipped the date-time toggle; the host owns the app-wide state.
  void useDateTimeToggled(bool checked);
  /// The user flipped the time-offset toggle; the host owns the time frame.
  void useTimeOffsetToggled(bool checked);

 public slots:
  void onStylesheetChanged(QString theme);

  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 private slots:
  void onEngineTimeChanged(double t);
  void onEngineRangeChanged(double min, double max);
  void onEnginePlayingChanged(bool playing);
  void onEngineRateChanged(double rate);

  void onSliderValueChanged(double value);
  // Delivers a throttled final position at once, so the released frame appears without waiting out the window.
  void onSliderReleased();
  void onPlayToggled(bool checked);
  void onLoopToggled(bool checked);
  void onRateChanged(double value);
  void onStepChanged(double value);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  // Fits displayTime to the widest label of time_label_format_ in its styled font, so a dated readout never clips.
  void updateReadoutWidth();

  void applyIcons(QString theme);
  void applyPlayPauseIcon(const QString& theme);

  // Trailing edge of the slider-drag throttle: seeks are rate-limited (leading and trailing edge) because a seek can
  // decode images synchronously on the GUI thread, and a drag emits a valueChanged per millisecond step; the final
  // position is always committed, with no backlog.
  void flushPendingSeek();

  // Fed by PlaybackEngine::coarseTimeChanged, as the Source Timeline needle: both must move in lockstep.
  void applyEngineTime(double t);

  // Empty ranges are skipped; a step larger than the range reaches its endpoint.
  void applySliderStep();

  // An empty range disables loop and slider, and play unless the engine is playing (a live stream must stay
  // pausable); the slider also follows the seek lock.
  void updateTransportEnabled();

  Ui::TimelineWidget* ui_;
  PlaybackEngine* engine_ = nullptr;
  bool updating_from_engine_ = false;
  bool seek_locked_ = false;
  // Range collapsed to a point. Starts true, so the strip is disabled until data loads.
  bool range_empty_ = true;
  QTimer seek_throttle_timer_;
  double pending_seek_value_ = 0.0;
  bool has_pending_seek_ = false;

  TimeLabelFormat time_label_format_ = TimeLabelFormat::kSeconds;

  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
