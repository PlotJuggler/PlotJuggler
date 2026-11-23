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
#include "pj_runtime/PlaybackRate.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/RealSlider.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_TimelineWidget.h"

namespace PJ {

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent), ui_(new Ui::TimelineWidget) {
  ui_->setupUi(this);
  ui_->playbackRate->setRange(kMinPlaybackRate, kMaxPlaybackRate);
  ui_->playbackRate->setSingleStep(kPlaybackRateStep);
  ui_->playbackRate->setDecimals(static_cast<int>(std::lround(-std::log10(kPlaybackRateStep))));

  applyIcons(currentTheme());

  ui_->displayTime->setText("0.000");
  ui_->displayTime->installEventFilter(this);

  // ~30 Hz window for slider seeks: a seek decodes images synchronously on the GUI thread, and without the window
  // each drag tick chains a fresh decode and the image lags arbitrarily far behind the slider.
  seek_throttle_timer_.setSingleShot(true);
  seek_throttle_timer_.setInterval(33);
  connect(&seek_throttle_timer_, &QTimer::timeout, this, &TimelineWidget::flushPendingSeek);

  connect(ui_->timeSlider, &RealSlider::realValueChanged, this, &TimelineWidget::onSliderValueChanged);
  connect(ui_->timeSlider, &QSlider::sliderReleased, this, &TimelineWidget::onSliderReleased);
  connect(ui_->buttonPlay, &QPushButton::toggled, this, &TimelineWidget::onPlayToggled);
  // Separate from onPlayToggled, so the icon also swaps while updating_from_engine_ is set.
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
  connect(engine_, &PlaybackEngine::coarseTimeChanged, this, &TimelineWidget::onEngineTimeChanged);
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
  // QLineEdit::sizeHint's recipe (text, its side margins, the style's frame and QSS padding) plus one character.
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
  // The app stylesheet lands after construction and may change the font or padding at any time.
  if (watched == ui_->displayTime && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)) {
    updateReadoutWidth();
  }
  return QWidget::eventFilter(watched, event);
}

void TimelineWidget::onEngineTimeChanged(double t) {
  // The coarse cursor, as the Source Timeline needle: the handle and the needle move together, and neither repaints
  // on every playback tick (docs/TECHNICAL_NOTES.md, "Raster repaints during playback").
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
  // Millisecond steps, computed in 64 bits and clamped: a stream's range is in absolute epoch seconds, and a span
  // polluted by a t≈0 sample overflows a 32-bit `(max-min)*1000`, collapsing the slider to its two ends.
  static constexpr int64_t kMaxSliderSteps = 10'000'000;
  const auto raw_steps = static_cast<int64_t>((max - min) * 1000.0);
  const int steps = static_cast<int>(std::clamp<int64_t>(raw_steps, 1, kMaxSliderSteps));
  updating_from_engine_ = true;
  ui_->timeSlider->setLimits(min, max, steps);
  applySliderStep();
  updating_from_engine_ = false;
  range_empty_ = !(max > min);
  updateTransportEnabled();
  // setLimits keeps the integer position, which maps to a different value in the new range: a range-only change (a
  // file load, an offset edit) emits no currentTimeChanged, so re-apply the current time.
  if (engine_ != nullptr) {
    applyEngineTime(toAxisDouble(engine_->currentTime()));
  }
}

void TimelineWidget::updateTransportEnabled() {
  const bool has_data = !range_empty_;
  // A live stream's first sample can be a single point (min == max): play stays enabled while playing, so it can
  // always be paused.
  const bool playing = engine_ != nullptr && engine_->isPlaying();
  ui_->buttonPlay->setEnabled(has_data || playing);
  ui_->playbackLoop->setEnabled(has_data);
  ui_->timeSlider->setEnabled(has_data && !seek_locked_);
}

void TimelineWidget::onEnginePlayingChanged(bool playing) {
  updating_from_engine_ = true;
  ui_->buttonPlay->setChecked(playing);
  updating_from_engine_ = false;
  // Play's enabled state depends on isPlaying().
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
    // flushPendingSeek delivers it when the window closes.
    pending_seek_value_ = value;
    has_pending_seek_ = true;
    return;
  }
  // Open the window before seeking: a long synchronous decode in setCurrentTime blocks the event loop, and the timer
  // fires as soon as control returns.
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
  // Replaces the .ui's fixed heights, so the row grows with the icons.
  ui_->displayTime->setMinimumHeight(button_extent);
  ui_->displayTime->setMaximumHeight(button_extent);
  ui_->timeSlider->setMinimumHeight(button_extent);
  ui_->timeSlider->setMaximumHeight(button_extent);
  ui_->frame->setMaximumHeight(button_extent);
  if (auto* layout = ui_->layoutTimescale) {
    layout->setContentsMargins(
        chrome_metrics_.layout_padding, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding,
        chrome_metrics_.layout_padding);
    layout->setSpacing(chrome_metrics_.layout_spacing);
  }
  // MainWindow.ui clamps this widget's slot to a fixed height; without this the taller row overflows it.
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
