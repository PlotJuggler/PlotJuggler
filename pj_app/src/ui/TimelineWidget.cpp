// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/TimelineWidget.h"

#include <QByteArray>
#include <QEvent>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QSvgRenderer>
#include <Qt>
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "pj_runtime/PlaybackEngine.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/RealSlider.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_TimelineWidget.h"

namespace PJ {

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent), ui_(new Ui::TimelineWidget) {
  ui_->setupUi(this);

  applyIcons(currentTheme());

  ui_->displayTime->setText("0.000");
  ui_->displayTime->installEventFilter(this);

  // 30 Hz throttle window for slider-driven seeks. Synchronous image decode on
  // the GUI thread takes ~30-120 ms per topic; without this window each slider
  // tick during a drag would chain a fresh decode and the visible image lags
  // arbitrarily far behind the slider.
  seek_throttle_timer_.setSingleShot(true);
  seek_throttle_timer_.setInterval(33);
  connect(&seek_throttle_timer_, &QTimer::timeout, this, &TimelineWidget::flushPendingSeek);

  connect(ui_->timeSlider, &RealSlider::realValueChanged, this, &TimelineWidget::onSliderValueChanged);
  connect(ui_->timeSlider, &QSlider::sliderReleased, this, &TimelineWidget::onSliderReleased);
  connect(ui_->buttonPlay, &QPushButton::toggled, this, &TimelineWidget::onPlayToggled);
  // Icon-only sync: swap play_arrow ↔ pause whenever the toggle flips,
  // regardless of whether the change came from the user or from
  // onEnginePlayingChanged below. Kept separate from onPlayToggled so
  // the visual swap runs even while updating_from_engine_ is set.
  connect(ui_->buttonPlay, &QPushButton::toggled, this, [this]() { applyPlayPauseIcon(currentTheme()); });
  connect(ui_->playbackLoop, &QPushButton::toggled, this, &TimelineWidget::onLoopToggled);
  // clicked, not toggled: the host's silent setters must not echo back to it.
  connect(ui_->buttonUseDateTime, &QPushButton::clicked, this, &TimelineWidget::useDateTimeToggled);
  connect(ui_->buttonTimeOffset, &QPushButton::clicked, this, &TimelineWidget::useTimeOffsetToggled);
  connect(ui_->playbackRate, &DoubleScrubber::valueChanged, this, &TimelineWidget::onRateChanged);
  connect(ui_->playbackStep, &DoubleScrubber::valueChanged, this, &TimelineWidget::onStepChanged);
  auto* playback_shortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
  playback_shortcut->setAutoRepeat(false);
  connect(playback_shortcut, &QShortcut::activated, ui_->buttonPlay, &QPushButton::click);
}

TimelineWidget::~TimelineWidget() {
  delete ui_;
}

void TimelineWidget::setPlaybackEngine(PlaybackEngine* engine) {
  if (engine_) {
    disconnect(engine_, nullptr, this, nullptr);
  }
  engine_ = engine;
  if (!engine_) {
    return;
  }
  connect(engine_, &PlaybackEngine::currentTimeChanged, this, &TimelineWidget::onEngineTimeChanged);
  connect(engine_, &PlaybackEngine::rangeChanged, this, &TimelineWidget::onEngineRangeChanged);
  connect(engine_, &PlaybackEngine::playingChanged, this, &TimelineWidget::onEnginePlayingChanged);
  connect(engine_, &PlaybackEngine::playbackRateChanged, this, &TimelineWidget::onEngineRateChanged);

  onEngineRangeChanged(toAxisDouble(engine_->rangeMin()), toAxisDouble(engine_->rangeMax()));
  onEngineTimeChanged(toAxisDouble(engine_->currentTime()));
  onEnginePlayingChanged(engine_->isPlaying());
  onEngineRateChanged(engine_->playbackRate());
}

void TimelineWidget::setSeekLocked(bool locked) {
  if (seek_locked_ == locked) {
    return;
  }
  seek_locked_ = locked;
  updateTransportEnabled();
  if (locked) {
    has_pending_seek_ = false;
    seek_throttle_timer_.stop();
    if (engine_ != nullptr) {
      applyEngineTime(toAxisDouble(engine_->currentTime()));
    }
  }
}

void TimelineWidget::setUseDateTime(bool checked) {
  ui_->buttonUseDateTime->setChecked(checked);
}

void TimelineWidget::setUseTimeOffset(bool checked) {
  ui_->buttonTimeOffset->setChecked(checked);
}

void TimelineWidget::setTimeLabelFormat(TimeLabelFormat format) {
  if (time_label_format_ == format) {
    return;
  }
  time_label_format_ = format;
  updateReadoutWidth();
  if (engine_ != nullptr) {
    applyEngineTime(toAxisDouble(engine_->currentTime()));
  }
}

void TimelineWidget::updateReadoutWidth() {
  QLineEdit* readout = ui_->displayTime;
  // A dated wall-clock instant (kMinWallClockSeconds would print a duration);
  // for the other formats a long negative duration is the widest label.
  const double sample = isWallClock(time_label_format_) ? 2.0e9 : -359999.999;
  const QString widest = formatTimeLabel(sample, time_label_format_, 3, /*with_date=*/true);
  const QFontMetrics fm = readout->fontMetrics();
  // Same recipe as QLineEdit::sizeHint (text + its 2-px side margins, then the
  // style's frame and QSS padding), plus one character of slack.
  const QMargins text_margins = readout->textMargins();
  const QMargins contents_margins = readout->contentsMargins();
  const int text_width = fm.horizontalAdvance(widest) + fm.averageCharWidth() + 4 + text_margins.left() +
                         text_margins.right() + contents_margins.left() + contents_margins.right();
  QStyleOptionFrame option;
  option.initFrom(readout);
  option.lineWidth =
      readout->hasFrame() ? readout->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, readout) : 0;
  const int width =
      readout->style()->sizeFromContents(QStyle::CT_LineEdit, &option, QSize(text_width, fm.height()), readout).width();
  readout->setMinimumWidth(std::max(80, width));
  readout->setMaximumWidth(std::max(200, width));
}

bool TimelineWidget::eventFilter(QObject* watched, QEvent* event) {
  // The app stylesheet lands after construction and can change the readout's font
  // or padding at any time: re-measure whenever either changes.
  if (watched == ui_->displayTime && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)) {
    updateReadoutWidth();
  }
  return QWidget::eventFilter(watched, event);
}

void TimelineWidget::onEngineTimeChanged(double t) {
  // Apply every engine tick immediately — NO display throttle. The Source Timeline
  // needle is also driven directly off currentTimeChanged, so throttling only this
  // widget made the handle lag the needle (and vice versa) by up to one window. The
  // slider/label repaint is cheap; the expensive per-cursor work (plot replot, scene
  // decode) is coalesced separately on MainWindow's tracker-broadcast path, so the
  // handle and needle now move in lockstep off the same source.
  applyEngineTime(t);
}

void TimelineWidget::applyEngineTime(double t) {
  updating_from_engine_ = true;
  ui_->timeSlider->setRealValue(t);
  const QString formatted = formatTimeLabel(t, time_label_format_, 3, /*with_date=*/true);
  if (formatted != ui_->displayTime->text()) {
    ui_->displayTime->setText(formatted);
  }
  updating_from_engine_ = false;
}

void TimelineWidget::onEngineRangeChanged(double min, double max) {
  // Step count = ~millisecond resolution over the range, computed in 64-bit and
  // clamped. Streaming seeds the engine range in absolute epoch seconds, so a
  // large (or t≈0-polluted) span makes `(max-min)*1000` overflow a 32-bit int,
  // wrapping to ≤0 and collapsing the RealSlider to two positions (start/end).
  // kMaxSliderSteps caps the count at ms resolution over any realistic span —
  // past sub-pixel granularity on any display — so a polluted span cannot
  // produce an absurd count.
  static constexpr int64_t kMaxSliderSteps = 10'000'000;
  const auto raw_steps = static_cast<int64_t>((max - min) * 1000.0);
  const int steps = static_cast<int>(std::clamp<int64_t>(raw_steps, 1, kMaxSliderSteps));
  updating_from_engine_ = true;
  ui_->timeSlider->setLimits(min, max, steps);
  applySliderStep();
  updating_from_engine_ = false;
  // A collapsed range (max <= min) means no scrubbable data: disable the whole
  // transport so a dead slider can't be dragged over nothing.
  range_empty_ = !(max > min);
  updateTransportEnabled();
  // setLimits keeps the slider's integer position, which now maps to a DIFFERENT
  // value under the new range — so a range-only change (no currentTimeChanged, e.g.
  // the union growing as files load, or an offset edit) would otherwise leave the
  // handle on a stale value. Re-apply the current time so it reflects currentTime in
  // the new range (and stays in lockstep with the needle).
  if (engine_ != nullptr) {
    applyEngineTime(toAxisDouble(engine_->currentTime()));
  }
}

void TimelineWidget::updateTransportEnabled() {
  const bool has_data = !range_empty_;
  // Play stays usable while the engine is already playing, so a live stream —
  // whose first sample can be a single point (min==max) — can always be paused
  // from the transport; loop + slider require a real (non-empty) range.
  const bool playing = engine_ != nullptr && engine_->isPlaying();
  ui_->buttonPlay->setEnabled(has_data || playing);
  ui_->playbackLoop->setEnabled(has_data);
  // The slider needs data AND an unlocked seek (streaming pause locks it).
  ui_->timeSlider->setEnabled(has_data && !seek_locked_);
}

void TimelineWidget::onEnginePlayingChanged(bool playing) {
  updating_from_engine_ = true;
  ui_->buttonPlay->setChecked(playing);
  updating_from_engine_ = false;
  // Play's enabled state depends on isPlaying() (a stream over a degenerate range
  // stays pausable), so re-evaluate whenever the play state flips.
  updateTransportEnabled();
}

void TimelineWidget::onEngineRateChanged(double rate) {
  updating_from_engine_ = true;
  ui_->playbackRate->setValue(rate);
  updating_from_engine_ = false;
}

void TimelineWidget::onSliderValueChanged(double value) {
  if (updating_from_engine_ || !engine_) {
    return;
  }
  if (seek_locked_) {
    applyEngineTime(toAxisDouble(engine_->currentTime()));
    return;
  }
  if (seek_throttle_timer_.isActive()) {
    // Inside the throttle window — buffer the latest value. The trailing-edge
    // flush below will deliver whatever sits here once the window closes.
    pending_seek_value_ = value;
    has_pending_seek_ = true;
    return;
  }
  // Leading edge: open the window first so a long synchronous decode inside
  // setCurrentTime keeps the timer alive (it cannot tick on a blocked event
  // loop, but it will fire as soon as control returns).
  has_pending_seek_ = false;
  seek_throttle_timer_.start();
  engine_->setCurrentTime(displaySeconds(value));
}

void TimelineWidget::onSliderReleased() {
  if (updating_from_engine_ || !engine_) {
    return;
  }
  if (seek_locked_) {
    has_pending_seek_ = false;
    seek_throttle_timer_.stop();
    applyEngineTime(toAxisDouble(engine_->currentTime()));
    return;
  }
  // Deliver the final drag position immediately rather than waiting out the
  // throttle window — the frame the user released on should appear at once, and
  // forward playback resumes from there with no extra latency.
  if (has_pending_seek_) {
    has_pending_seek_ = false;
    seek_throttle_timer_.stop();
    engine_->setCurrentTime(displaySeconds(pending_seek_value_));
  }
}

void TimelineWidget::flushPendingSeek() {
  if (!has_pending_seek_) {
    return;
  }
  if (!engine_) {
    has_pending_seek_ = false;
    return;
  }
  if (seek_locked_) {
    has_pending_seek_ = false;
    applyEngineTime(toAxisDouble(engine_->currentTime()));
    return;
  }
  const double v = pending_seek_value_;
  has_pending_seek_ = false;
  seek_throttle_timer_.start();  // Re-arm so a long drag stays rate-limited.
  engine_->setCurrentTime(displaySeconds(v));
}

void TimelineWidget::onPlayToggled(bool checked) {
  if (updating_from_engine_ || !engine_) {
    return;
  }
  if (checked) {
    engine_->play();
  } else {
    engine_->pause();
  }
}

void TimelineWidget::onLoopToggled(bool checked) {
  if (updating_from_engine_ || !engine_) {
    return;
  }
  engine_->setLooping(checked);
}

void TimelineWidget::onRateChanged(double value) {
  if (updating_from_engine_ || !engine_) {
    return;
  }
  engine_->setPlaybackRate(value);
}

void TimelineWidget::onStepChanged(double value) {
  if (updating_from_engine_ || !engine_) {
    return;
  }
  engine_->setStep(value);
  applySliderStep();
}

void TimelineWidget::applySliderStep() {
  const double range = ui_->timeSlider->getMaximum() - ui_->timeSlider->getMinimum();
  const double step = ui_->playbackStep->value();
  if (range > 0.0 && std::isfinite(step)) {
    ui_->timeSlider->setRealStepValue(std::min(step, range));
  }
}

void TimelineWidget::onStylesheetChanged(QString theme) {
  applyIcons(theme);
}

void TimelineWidget::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  applyIcons(currentTheme());
}

void TimelineWidget::applyIcons(QString theme) {
  const QSize icon_sz(chrome_metrics_.icon_size, chrome_metrics_.icon_size);
  const int button_extent = chrome_metrics_.icon_size + chrome_metrics_.icon_padding;
  const int band_extent = chrome_metrics_.bandHeight();
  ui_->buttonTimeOffset->setIcon(loadSvg(":/resources/svg/t0.svg", theme));
  ui_->buttonUseDateTime->setIcon(loadSvg(":/resources/svg/datetime.svg", theme));
  ui_->playbackLoop->setIcon(loadSvg(":/resources/svg/loop.svg", theme));
  applyPlayPauseIcon(theme);
  for (QPushButton* button : {ui_->buttonTimeOffset, ui_->buttonUseDateTime, ui_->playbackLoop, ui_->buttonPlay}) {
    button->setIconSize(icon_sz);
    button->setMinimumSize(button_extent, button_extent);
    button->setMaximumSize(button_extent, button_extent);
  }
  // Override .ui-baked 24-px height caps on the strip's non-button
  // controls so the row grows together when icons scale.
  ui_->displayTime->setMinimumHeight(button_extent);
  ui_->displayTime->setMaximumHeight(button_extent);
  ui_->timeSlider->setMinimumHeight(button_extent);
  ui_->timeSlider->setMaximumHeight(button_extent);
  ui_->frame->setMaximumHeight(button_extent);
  // Outer strip layout: padding goes here, so the playback widget
  // itself grows by 2 * layout_padding via sizeHint (the inner content
  // remains button_extent tall plus the margins).
  if (auto* layout = ui_->layoutTimescale) {
    layout->setContentsMargins(
        chrome_metrics_.layout_padding, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding,
        chrome_metrics_.layout_padding);
    layout->setSpacing(chrome_metrics_.layout_spacing);
  }
  // Grow the TimelineWidget itself to band_extent so the .ui-baked
  // 24-px clamp on the host slot in MainWindow.ui doesn't clip the
  // taller buttons / scrubbers when Chrome metrics scale up. Without
  // this the outer slot stays at 24 and the inner row overflows.
  setMinimumHeight(band_extent);
  setMaximumHeight(band_extent);
  const qreal dpr = devicePixelRatioF();
  ui_->labelSpeed->setPixmap(renderSvgPixmap(":/resources/svg/acute.svg", theme, icon_sz, dpr));
  ui_->labelStep->setPixmap(renderSvgPixmap(":/resources/svg/move_selection_right.svg", theme, icon_sz, dpr));
}

void TimelineWidget::applyPlayPauseIcon(const QString& theme) {
  const char* icon = ui_->buttonPlay->isChecked() ? ":/resources/svg/pause.svg" : ":/resources/svg/play_arrow.svg";
  ui_->buttonPlay->setIcon(loadSvg(QString::fromLatin1(icon), theme));
}

}  // namespace PJ
