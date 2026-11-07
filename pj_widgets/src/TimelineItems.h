#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Internal (src/-private) QGraphicsItems and theme helpers shared by the
// time-strip widget family: the Source Timeline (`Timeline`) and the State
// Transitions view (`StateTransitionsView`). Everything here is painted against
// the same TimelineViewport ns<->px mapping, so the two widgets can never
// disagree on tick positions or needle geometry.

#include <QColor>
#include <QFontMetricsF>
#include <QGraphicsItem>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QString>
#include <QStyleOptionGraphicsItem>
#include <QtGlobal>
#include <chrono>
#include <cmath>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/Hatch.h"
#include "pj_widgets/TimeFormat.h"
#include "pj_widgets/Timeline.h"

namespace PJ::timeline_detail {

using namespace Qt::StringLiterals;

// Chrono-derived (no drift from the unit it measures); kSecondNs == 1e9.
inline constexpr qint64 kSecondNs = std::chrono::nanoseconds::period::den;
inline constexpr qint64 kMillisecondNs = kSecondNs / 1000;
inline constexpr qint64 kMinuteNs = 60 * kSecondNs;
inline constexpr qint64 kHourNs = 60 * kMinuteNs;

/// The framework theme matching the live application palette (light vs dark by
/// window lightness) — the seam every self-painting time-strip item resolves its
/// colors through.
inline theme::Theme frameworkTheme() {
  const QColor window = QGuiApplication::palette().color(QPalette::Window);
  return theme::themeFor(window.lightness() >= 128);
}

/// The shared data-backdrop fill behind time-strip rows and rulers.
inline QColor timelineBackdrop() {
  return theme::surface(theme::Surface::DataBackdrop, frameworkTheme());
}

// Absolute Unix-timestamp label (seconds) for a display-ns value read as epoch
// ns — the SAME value the plot axis shows in absolute mode while the app's
// date-time toggle is off (formatAbsoluteLabel picks the wall clock when on). The
// value is ROUNDED to the nearest millisecond — matching the playback readout's
// QString::number('f', 3) rounding, NOT truncated (truncation read a ms low and
// made the timeline disagree with playback by 0.001 s). Integer math (with carry
// into seconds) keeps it exact at epoch scale. `fixed_ms` forces a 3-decimal
// fraction even on a whole second (the marker pills, so they match the playback
// readout character-for-character); the ruler leaves it off for clean tick labels.
inline QString formatAbsoluteSeconds(qint64 epoch_ns, bool fixed_ms = false) {
  const qint64 half_ms = kMillisecondNs / 2;
  const qint64 total_ms = (epoch_ns + (epoch_ns >= 0 ? half_ms : -half_ms)) / kMillisecondNs;
  const qint64 sec = total_ms / 1000;
  const qint64 ms = (total_ms < 0 ? -total_ms : total_ms) % 1000;
  if (ms == 0 && !fixed_ms) {
    return QString::number(sec);
  }
  return u"%1.%2"_s.arg(sec).arg(ms, 3, 10, QChar('0'));
}

// Absolute label for a display-ns value read as epoch ns: the wall-clock time of
// day when `format` is a clock format (milliseconds when `fixed_ms`, else as many
// digits as the tick spacing `step_ns` needs), else the Unix timestamp above.
inline QString formatAbsoluteLabel(qint64 epoch_ns, TimeLabelFormat format, bool fixed_ms, qint64 step_ns = 0) {
  if (!isWallClock(format)) {
    return formatAbsoluteSeconds(epoch_ns, fixed_ms);
  }
  const int decimals =
      fixed_ms ? 3 : clockDecimalsForStep(static_cast<double>(step_ns) / static_cast<double>(kSecondNs));
  return formatTimeLabel(static_cast<double>(epoch_ns) / static_cast<double>(kSecondNs), format, decimals);
}

// Theme-derived timeline colours. A self-painting data view reads the same
// framework data-backdrop/outline/text tokens used by stylesheet-driven views.
struct TimelineColors {
  QColor bg;          // rows + ruler background
  QColor ruler_data;  // subtle tint over the data (OR) span
  QColor grid_line;   // faint full-height tick gridlines
  // (the empty-area diagonal hatch ink comes from the shared PJ::appHatchColor())
  QColor text;          // frame numbers + bar labels — "like any other text"
  QColor ruler_border;  // ruler bottom separator
  QColor bar_border;    // bar outline
};

inline TimelineColors timelineColors() {
  const auto fw_theme = frameworkTheme();
  const QColor bg = theme::surface(theme::Surface::DataBackdrop, fw_theme);
  const QColor text = theme::onSurface(theme::Surface::DataBackdrop, theme::Emphasis::Default, fw_theme);
  return {
      .bg = bg,
      .ruler_data = theme::overlay(theme::Overlay::Selected, fw_theme),
      .grid_line = theme::surface(PJ::theme::Surface::Separation, fw_theme),
      .text = text,
      .ruler_border = theme::surface(PJ::theme::Surface::Separation, fw_theme),
      .bar_border = theme::surface(PJ::theme::Surface::Separation, fw_theme),
  };
}

/// Below this width a bar/segment paints as pure color — no room for text.
inline constexpr double kBarMinLabelWidthPx = 8.0;

/// The ONE bar painter the time-strip family shares: `fill` (caller pre-applies
/// any alpha), `border` outline, and `label` elided INSIDE the rect — drawn only
/// when the label's rect is wider than kBarMinLabelWidthPx. Used by the Source
/// Timeline's dataset bars and the State Transitions segments, so both render
/// text-in-rectangle identically. A caller whose bars can extend beyond the
/// visible viewport passes `label_rect` (the bar clipped to the viewport) so the
/// label pins into view instead of scrolling away with the bar's left edge;
/// null (default) anchors the label to the full rect. `h_align` sets the
/// horizontal text alignment inside that rect (the Timeline's dataset bars
/// left-align; the state strip centers its value labels).
inline void paintTimelineBar(
    QPainter* painter, const QRectF& rect, const QColor& fill, const QPen& border, const QString& label,
    const QRectF& label_rect = QRectF(), Qt::Alignment h_align = Qt::AlignLeft) {
  painter->setBrush(fill);
  painter->setPen(border);
  painter->drawRect(rect);
  const QRectF text_rect = label_rect.isNull() ? rect : label_rect;
  if (text_rect.width() > kBarMinLabelWidthPx && !label.isEmpty()) {
    const QFontMetricsF fm(painter->font());
    const QString text = fm.elidedText(label, Qt::ElideRight, text_rect.width() - 6.0);
    painter->setPen(timelineColors().text);
    painter->drawText(text_rect.adjusted(4, 0, -4, 0), Qt::AlignVCenter | h_align, text);
  }
}

/// Time ruler row across the top of the timeline. Ported from the prototype's
/// TimeRulerItem, rebound to render a core-computed TimelineRuler (tick positions
/// + the chosen interval) rather than recomputing the tick ladder itself. Tick
/// x's are mapped through the same viewport the bars use, so labels line up.
class TimelineRulerItem : public QGraphicsItem {
 public:
  static constexpr double kRulerHeight = 18.0;  // compact header (Blender-style)

  TimelineRulerItem() {
    setZValue(100);
  }

  /// Lay out for the current viewport: `width_px` visible width, `ruler` the
  /// core's tick layout, `epoch_ns` the display-ns subtracted from labels so they
  /// read 0:00 at the scene origin, and [data_min_ns, data_max_ns] the span
  /// covered by data (the union of all datasets) — tinted distinctly from the
  /// empty buffer. Pass data_max <= data_min for "no data" (no tint).
  void setLayout(
      const TimelineViewport& viewport, double width_px, const TimelineRuler& ruler, qint64 epoch_ns,
      qint64 data_min_ns, qint64 data_max_ns) {
    prepareGeometryChange();
    viewport_ = viewport;
    width_px_ = width_px;
    ruler_ = ruler;
    epoch_ns_ = epoch_ns;
    data_min_ns_ = data_min_ns;
    data_max_ns_ = data_max_ns;
    update();
  }

  /// Switch tick labels between elapsed (false) and absolute wall-clock (true).
  /// Pure relabel — tick positions/layout are unchanged, so just repaint.
  void setAbsoluteFormat(bool absolute) {
    if (absolute_ != absolute) {
      absolute_ = absolute;
      update();
    }
  }

  /// Absolute-mode rendering: wall-clock time of day or the Unix timestamp
  /// (see formatAbsoluteLabel). Pure relabel, like setAbsoluteFormat.
  void setClockFormat(TimeLabelFormat format) {
    if (clock_format_ != format) {
      clock_format_ = format;
      update();
    }
  }

  /// Tick-label pixel size. The Source Timeline keeps the compact default; the
  /// State Transitions strip raises it to match the plot axis labels.
  void setLabelPixelSize(int pixel_size) {
    if (label_pixel_size_ != pixel_size && pixel_size > 0) {
      label_pixel_size_ = pixel_size;
      update();
    }
  }

  /// Bottom-of-strip placement: draw the separator along the TOP edge (the rows
  /// sit above the band) instead of the bottom. Number layout is unchanged.
  void setBottomAligned(bool bottom) {
    if (bottom_aligned_ != bottom) {
      bottom_aligned_ = bottom;
      update();
    }
  }

  /// Update only the tinted data span (display-ns), keeping the rest of the
  /// layout — used to track the band live during a bar drag without a rebuild.
  void setDataSpan(qint64 data_min_ns, qint64 data_max_ns) {
    data_min_ns_ = data_min_ns;
    data_max_ns_ = data_max_ns;
    update();
  }

  [[nodiscard]] QRectF boundingRect() const override {
    return QRectF(0, 0, width_px_, kRulerHeight);
  }

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* /*option*/, QWidget* /*widget*/) override {
    const TimelineColors col = timelineColors();
    painter->fillRect(boundingRect(), col.bg);
    // Subtle tint over the data-covered span (earliest start .. latest end — the
    // union/"OR" region) so the ±buffer padding still reads as empty up here too.
    if (data_max_ns_ > data_min_ns_) {
      const double x0 = TimelineScene::nsToPx(data_min_ns_, viewport_);
      const double x1 = TimelineScene::nsToPx(data_max_ns_, viewport_);
      painter->fillRect(QRectF(x0, 0, x1 - x0, kRulerHeight), col.ruler_data);
    }
    // Thin separator on the row-facing edge; the full-height tick gridlines are
    // drawn by the background item, so the band itself stays uncluttered.
    painter->setPen(col.ruler_border);
    const double separator_y = bottom_aligned_ ? 0.5 : kRulerHeight - 0.5;
    painter->drawLine(QLineF(0, separator_y, width_px_, separator_y));

    // Frame numbers centered over each tick's gridline, vertically centered in
    // the band (compact Blender-style by default; see setLabelPixelSize).
    QFont font = painter->font();
    font.setPixelSize(label_pixel_size_);
    painter->setFont(font);
    const QFontMetricsF fm(font);
    const double baseline = (kRulerHeight + fm.ascent() - fm.descent()) / 2.0;
    for (const qint64 tick : ruler_.ticks_ns) {
      const double x = TimelineScene::nsToPx(tick, viewport_);
      // The full-height gridline (drawn by the background item, starting flush
      // under this separator) is the tick — the ruler shows only the number,
      // horizontally centered on the gridline (no nub poking up through the
      // separator line). Absolute mode reads the tick as epoch ns (clock_format_).
      const QString label = absolute_ ? formatAbsoluteLabel(tick, clock_format_, /*fixed_ms=*/false, ruler_.interval_ns)
                                      : formatLabel(tick - epoch_ns_);
      const double tw = fm.horizontalAdvance(label);
      painter->setPen(col.text);
      painter->drawText(QPointF(x - (tw / 2.0), baseline), label);
    }
  }

 private:
  static QString formatLabel(qint64 ns_relative) {
    // Pick the format from the value's magnitude (not the tick interval) so labels
    // stay readable whether the visible span is milliseconds or hours.
    const qint64 abs_ns = std::abs(ns_relative);
    if (abs_ns >= kHourNs) {
      const qint64 sec = ns_relative / kSecondNs;
      const qint64 hh = sec / 3600;
      const qint64 mm = (sec / 60) % 60;
      return QString("%1:%2").arg(hh).arg(mm, 2, 10, QChar('0'));
    }
    if (abs_ns >= kMinuteNs) {
      const qint64 sec = ns_relative / kSecondNs;
      const qint64 mm = sec / 60;
      const qint64 ss = sec % 60;
      return QString("%1:%2").arg(mm).arg(ss, 2, 10, QChar('0'));
    }
    if (abs_ns >= kSecondNs) {
      return QString::number(static_cast<double>(ns_relative) / static_cast<double>(kSecondNs), 'f', 1) + "s";
    }
    return QString::number(static_cast<double>(ns_relative) / static_cast<double>(kMillisecondNs), 'f', 1) + "ms";
  }

  TimelineViewport viewport_;
  double width_px_ = 0.0;
  TimelineRuler ruler_;
  qint64 epoch_ns_ = 0;
  qint64 data_min_ns_ = 0;  // data-covered span (display-ns); data_max_<=data_min_ means "no data"
  qint64 data_max_ns_ = 0;
  bool absolute_ = false;  // tick labels: false = elapsed (epoch_ns_-relative), true = wall-clock
  TimeLabelFormat clock_format_ = TimeLabelFormat::kSeconds;  // absolute labels: wall clock vs unix seconds
  bool bottom_aligned_ = false;  // separator on the top edge (ruler pinned at the strip bottom)
  int label_pixel_size_ = 11;    // tick-label font (see setLabelPixelSize)
};

/// Vertical marker needle, used for both the playhead and the reference line. No
/// top handle/arrow: the line starts at the header bottom (just below the frame
/// numbers — never overlapping the text) and runs down through the rows. Idle it
/// paints `idle_color_`, under the cursor it lifts to `hovered_color_`, and while
/// grabbed it darkens to `grabbed_color_` and shows a current-time pill in the
/// header, on the SAME baseline/font as the ruler numbers (so the number lines
/// up). A ±6px column stays grabbable for seek-drags.
class TimelineNeedleItem : public QGraphicsItem {
 public:
  static constexpr double kGrabHalfWidth = 6.0;
  static constexpr double kPillHalfWidth = 52.0;  // paint room for the timestamp pill

  TimelineNeedleItem(
      const QColor& idle_color, const QColor& hovered_color, const QColor& grabbed_color,
      const QColor& grabbed_text_color)
      : idle_color_(idle_color),
        hovered_color_(hovered_color),
        grabbed_color_(grabbed_color),
        grabbed_text_color_(grabbed_text_color) {
    setFlag(ItemIsSelectable, false);
    setCursor(Qt::SizeHorCursor);
    setAcceptHoverEvents(true);
  }

  // Re-resolve on a live theme switch (Timeline::changeEvent) — the colors are
  // captured at construction and would otherwise keep the previous theme's ink.
  void setColors(
      const QColor& idle_color, const QColor& hovered_color, const QColor& grabbed_color,
      const QColor& grabbed_text_color) {
    idle_color_ = idle_color;
    hovered_color_ = hovered_color;
    grabbed_color_ = grabbed_color;
    grabbed_text_color_ = grabbed_text_color;
    update();
  }

  void setHeight(double height) {
    if (qFuzzyCompare(height, height_)) {
      return;
    }
    prepareGeometryChange();
    height_ = height;
  }

  /// Scene-y of the (sticky) header's top — the timeline keeps it at the visible
  /// viewport top as the rows scroll, so the time pill and the needle's start
  /// follow the ruler instead of scrolling away. Within [0, height_].
  void setHeaderTop(double scene_y) {
    if (qFuzzyCompare(scene_y, header_top_)) {
      return;
    }
    header_top_ = scene_y;
    update();
  }

  /// Bottom-of-strip ruler: the needle line runs from the scene top DOWN to the
  /// ruler band (instead of from below a top band to the scene bottom). The
  /// grabbed-time pill stays inside the band either way.
  void setBottomRuler(bool bottom) {
    if (bottom_ruler_ != bottom) {
      bottom_ruler_ = bottom;
      update();
    }
  }

  /// Grabbed state: darken to grabbed_color_ + show the timestamp pill.
  void setGrabbed(bool on) {
    if (on == grabbed_) {
      return;
    }
    grabbed_ = on;
    update();
  }

  /// Hover state: the needle answers the pointer before it is grabbed, so a
  /// draggable marker is discoverable without pressing first. Ignored while
  /// grabbed, where the pressed colour must win.
  void hoverEnterEvent(QGraphicsSceneHoverEvent* /*event*/) override {
    hovered_ = true;
    update();
  }

  void hoverLeaveEvent(QGraphicsSceneHoverEvent* /*event*/) override {
    hovered_ = false;
    update();
  }

  /// Current-time text for the pill (formatted by the widget).
  void setLabel(const QString& text) {
    if (text == label_) {
      return;
    }
    label_ = text;
    if (grabbed_) {
      update();
    }
  }

  [[nodiscard]] QRectF boundingRect() const override {
    // Wide enough to paint the centered pill; hit area is narrowed in shape().
    return QRectF(-kPillHalfWidth, 0, 2 * kPillHalfWidth, height_);
  }

  // Narrow hit area (just the needle column) so the wide pill region never
  // shadows bars underneath; the pill only appears mid-grab anyway.
  [[nodiscard]] QPainterPath shape() const override {
    QPainterPath path;
    path.addRect(-kGrabHalfWidth, 0, 2 * kGrabHalfWidth, height_);
    return path;
  }

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* /*option*/, QWidget* /*widget*/) override {
    const QColor color = grabbed_ ? grabbed_color_ : (hovered_ ? hovered_color_ : idle_color_);
    const double rh = TimelineRulerItem::kRulerHeight;
    // Start at the (sticky) header bottom so the needle reaches up to — never into —
    // the numbers, even when the rows are scrolled and the header floats down. Use a
    // flat cap so the 2px pen doesn't square-cap a pixel up past the ruler separator.
    QPen needle_pen(color, 2.0);
    needle_pen.setCapStyle(Qt::FlatCap);
    painter->setPen(needle_pen);
    if (bottom_ruler_) {
      painter->drawLine(QLineF(0, 0, 0, header_top_));
    } else {
      painter->drawLine(QLineF(0, header_top_ + rh, 0, height_));
    }

    if (!grabbed_ || label_.isEmpty()) {
      return;
    }
    // Current-time pill, centered on the needle, sitting in the header on the
    // EXACT same baseline + font as the ruler frame numbers (so it lines up).
    QFont font = painter->font();
    font.setPixelSize(11);
    painter->setFont(font);
    const QFontMetricsF fm(font);
    const double tw = fm.horizontalAdvance(label_);
    const QRectF pill(-tw / 2.0 - 5.0, header_top_ + 0.5, tw + 10.0, rh - 1.0);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    const qreal pill_radius = theme::radius(theme::Radius::Input);
    painter->drawRoundedRect(pill, pill_radius, pill_radius);
    const double baseline = header_top_ + ((rh + fm.ascent() - fm.descent()) / 2.0);
    painter->setPen(grabbed_text_color_);
    painter->drawText(QPointF(-tw / 2.0, baseline), label_);
  }

 private:
  double height_ = 200.0;
  double header_top_ = 0.0;    // scene-y of the sticky header top (see setHeaderTop)
  bool bottom_ruler_ = false;  // needle line above the ruler band instead of below it
  bool hovered_ = false;
  bool grabbed_ = false;
  QString label_;
  QColor idle_color_;
  QColor hovered_color_;
  QColor grabbed_color_;
  QColor grabbed_text_color_;
};

/// Full-height background that fills the EMPTY span (outside the data union) with
/// a diagonal stripe hatch, so the ±buffer padding reads as "nothing here" while
/// the data span stays clean. Sits below every other item; never hit-tested.
class TimelineBackgroundItem : public QGraphicsItem {
 public:
  TimelineBackgroundItem() {
    setZValue(-50);
  }

  void setLayout(
      double width, double height, const TimelineViewport& viewport, const TimelineRuler& ruler, qint64 data_min_ns,
      qint64 data_max_ns) {
    prepareGeometryChange();
    width_ = width;
    height_ = height;
    viewport_ = viewport;
    ruler_ = ruler;  // shared with the ruler item so gridlines and numbers can't desync
    data_min_ns_ = data_min_ns;
    data_max_ns_ = data_max_ns;
    update();
  }

  /// Scene-y where the tick gridlines start. Defaults to just under a top ruler
  /// band; a strip with a bottom ruler sets 0 so the gridlines span its rows.
  void setGridTop(double grid_top) {
    if (!qFuzzyCompare(grid_top, grid_top_)) {
      grid_top_ = grid_top;
      update();
    }
  }

  /// Live-update only the data span (during a drag; layout otherwise unchanged).
  void setDataSpan(qint64 data_min_ns, qint64 data_max_ns) {
    data_min_ns_ = data_min_ns;
    data_max_ns_ = data_max_ns;
    update();
  }

  [[nodiscard]] QRectF boundingRect() const override {
    return QRectF(0, 0, width_, height_);
  }

  [[nodiscard]] QPainterPath shape() const override {
    return {};  // decorative; never hit-tested
  }

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override {
    const TimelineColors col = timelineColors();
    // Everything below is bounded by the exposed slice rather than the item's full
    // width. The item spans the union of every source, so its width scales with how
    // far apart they sit on the clock, not with what is on screen. drawHatch() costs
    // one antialiased line per kHatchSpacing of rect WIDTH, so painting the whole
    // item is ~126k lines per repaint for two sources ~56 years apart, against ~123
    // for the visible strip.
    const QRectF visible = (option != nullptr && !option->exposedRect.isEmpty()) ? option->exposedRect : boundingRect();

    // 1) Solid theme background under everything (white in light theme).
    painter->fillRect(visible, col.bg);

    // 2) Hatch the empty area (outside the data span) with the shared app hatch
    // (PJ::drawHatch). Phased to this item's GLOBAL origin so its diagonal lines line
    // up with every other widget that paints the hatch (the Mosaico RangeSlider, ...)
    // — all are windows into one continuous hatch layer. Mapping item(0,0) through the
    // painter's world transform and the viewport widget gives that global origin. The
    // phase is derived from that global origin, so clipping a hatch rect to `visible`
    // shifts no line: the grid stays continuous across widgets.
    const QPointF hatch_origin =
        widget != nullptr ? widget->mapToGlobal(painter->worldTransform().map(QPointF(0, 0))) : QPointF(0, 0);
    const QColor hatch_ink = appHatchColor();
    const auto hatch = [&](const QRectF& area) {
      const QRectF clipped = area.intersected(visible);
      if (!clipped.isEmpty()) {
        PJ::drawHatch(*painter, clipped, hatch_origin, hatch_ink);
      }
    };
    if (data_max_ns_ <= data_min_ns_) {
      hatch(QRectF(0, 0, width_, height_));
    } else {
      const double x_lo = TimelineScene::nsToPx(data_min_ns_, viewport_);
      const double x_hi = TimelineScene::nsToPx(data_max_ns_, viewport_);
      if (x_lo > 0.0) {
        hatch(QRectF(0, 0, x_lo, height_));
      }
      if (x_hi < width_) {
        hatch(QRectF(x_hi, 0, width_ - x_hi, height_));
      }
    }

    // 3) Faint full-height vertical tick gridlines (the "ticks"), at the SAME
    // tick positions the ruler numbers use — the Timeline computes the ruler once
    // and feeds it to both items via setLayout, so the two can never desync (and
    // we don't re-run the tick ladder on this hot paint path).
    painter->setPen(col.grid_line);
    for (const qint64 tick : ruler_.ticks_ns) {
      const double x = TimelineScene::nsToPx(tick, viewport_);
      painter->drawLine(QLineF(x, grid_top_, x, height_));
    }
  }

 private:
  double width_ = 0.0;
  double height_ = 0.0;
  double grid_top_ = TimelineRulerItem::kRulerHeight;  // gridline start (see setGridTop)
  TimelineViewport viewport_;
  TimelineRuler ruler_;  // tick list shared with the ruler item (set via setLayout)
  qint64 data_min_ns_ = 0;
  qint64 data_max_ns_ = 0;
};

}  // namespace PJ::timeline_detail
