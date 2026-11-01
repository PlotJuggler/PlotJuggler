// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/Timeline.h"

#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetricsF>
#include <QFrame>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsRectItem>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSplitter>
#include <QStyleOptionGraphicsItem>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include "TimelineItems.h"
#include "pj_widgets/Fnv1a.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/Hatch.h"
#include "pj_widgets/Scrollbar.h"
#include "pj_widgets/SvgButton.h"
using namespace Qt::StringLiterals;

namespace PJ {

// =============================================================================
// TimelineScene — Qt-free timing/geometry/alignment math.
// =============================================================================

namespace {
constexpr double kMinPxPerNs = 1e-12;
constexpr double kMaxPxPerNs = 1e-1;
constexpr int kMaxTicks = 10000;  // runaway guard

// Human-friendly tick intervals in ns: 100 us / 200 us / 500 us / 1/2/5/10/20/50 ms ... up to 1 h.
constexpr std::array<qint64, 23> kTickLadder = {
    100'000LL,         200'000LL,         500'000LL,         1'000'000LL,         2'000'000LL,        5'000'000LL,
    10'000'000LL,      20'000'000LL,      50'000'000LL,      100'000'000LL,       200'000'000LL,      500'000'000LL,
    1'000'000'000LL,   2'000'000'000LL,   5'000'000'000LL,   10'000'000'000LL,    30'000'000'000LL,   60'000'000'000LL,
    120'000'000'000LL, 300'000'000'000LL, 600'000'000'000LL, 1'800'000'000'000LL, 3'600'000'000'000LL};

qint64 chooseInterval(double px_per_ns, double target_px_per_tick) noexcept {
  const auto want = static_cast<qint64>(std::llround(target_px_per_tick / px_per_ns));
  for (const qint64 candidate : kTickLadder) {
    if (candidate >= want) {
      return candidate;
    }
  }
  return kTickLadder.back();
}

// Smallest multiple of `interval` that is >= value (handles negatives exactly,
// no double precision loss at epoch scale).
qint64 ceilToMultiple(qint64 value, qint64 interval) noexcept {
  const qint64 q = value / interval;  // truncates toward zero
  const qint64 floored = q * interval;
  if (floored == value) {
    return value;
  }
  return (value > 0) ? floored + interval : floored;
}
}  // namespace

// Shared time-strip theme helpers (TimelineItems.h), pulled to this scope so the
// widget code below keeps its unqualified call sites.
using timeline_detail::frameworkTheme;
using timeline_detail::timelineBackdrop;

void TimelineScene::setTracks(std::vector<TimelineSpanInput> tracks) {
  tracks_ = std::move(tracks);
}

TimeSpan TimelineScene::displayWindow(const TimelineSpanInput& track) noexcept {
  return {track.t_min_ns - track.offset_ns, track.t_max_ns - track.offset_ns};
}

TimeSpan TimelineScene::sceneExtent() const noexcept {
  std::optional<qint64> lo;
  std::optional<qint64> hi;
  for (const TimelineSpanInput& track : tracks_) {
    const auto w = displayWindow(track);
    lo = lo ? std::min(*lo, w.min) : w.min;
    hi = hi ? std::max(*hi, w.max) : w.max;
  }
  if (!lo) {
    return default_extent_;  // no tracks: the host-set empty span (defaults to 60 s)
  }
  return {*lo, *hi};
}

void TimelineScene::setDefaultExtent(qint64 min_ns, qint64 max_ns) noexcept {
  if (max_ns > min_ns) {
    default_extent_ = {min_ns, max_ns};
  }
}

double TimelineScene::nsToPx(qint64 display_ns, const TimelineViewport& vp) noexcept {
  return static_cast<double>(display_ns - vp.origin_ns) * vp.px_per_ns;
}

qint64 TimelineScene::pxToNs(double x_px, const TimelineViewport& vp) noexcept {
  return vp.origin_ns + static_cast<qint64>(std::llround(x_px / vp.px_per_ns));
}

PxSpan TimelineScene::barSpan(const TimelineSpanInput& track, const TimelineViewport& vp) noexcept {
  const auto w = displayWindow(track);
  const double x = nsToPx(w.min, vp);
  const double width = static_cast<double>(w.max - w.min) * vp.px_per_ns;
  return {x, width};
}

qint64 TimelineScene::pxDeltaToNs(double dx_px, const TimelineViewport& vp) noexcept {
  return static_cast<qint64>(std::llround(dx_px / vp.px_per_ns));
}

TimelineViewport TimelineScene::zoom(const TimelineViewport& vp, double factor, double anchor_x_px) noexcept {
  const double new_pp = std::clamp(vp.px_per_ns * factor, kMinPxPerNs, kMaxPxPerNs);
  const qint64 ns_at_anchor = pxToNs(anchor_x_px, vp);
  TimelineViewport out;
  out.px_per_ns = new_pp;
  out.origin_ns = ns_at_anchor - static_cast<qint64>(std::llround(anchor_x_px / new_pp));
  return out;
}

TimelineRuler TimelineScene::ruler(const TimelineViewport& vp, double width_px, double target_px_per_tick) {
  TimelineRuler out;
  out.interval_ns = chooseInterval(vp.px_per_ns, target_px_per_tick);
  const qint64 lo = vp.origin_ns;
  const qint64 hi = vp.origin_ns + static_cast<qint64>(std::llround(width_px / vp.px_per_ns));
  // Degenerate-extent guard: when the visible span is so large that even the widest
  // ladder interval (1 h) would pack thousands of ticks into the viewport — e.g. a
  // wall-clock-epoch streaming source unioned with relative-time file datasets, a span
  // of ~20,000 days — widen the interval so ticks stay at least kMinTickSpacingPx
  // apart. Without this the labels overpaint into a black smear and every repaint pays
  // for ~kMaxTicks labels + gridlines (the streaming "lags hard"). Round ladder
  // intervals are abandoned here (the labels are non-round), but that only happens in
  // the already-degenerate regime; normal zoom levels keep the ladder interval.
  constexpr double kMinTickSpacingPx = 48.0;
  if (hi > lo && out.interval_ns > 0 && width_px > 0.0) {
    const auto max_ticks_for_width = std::max<qint64>(1, static_cast<qint64>(width_px / kMinTickSpacingPx));
    const qint64 min_interval = (hi - lo) / max_ticks_for_width + 1;
    out.interval_ns = std::max(out.interval_ns, min_interval);
  }
  int count = 0;
  for (qint64 t = ceilToMultiple(lo, out.interval_ns); t <= hi && count < kMaxTicks; t += out.interval_ns, ++count) {
    out.ticks_ns.push_back(t);
  }
  return out;
}

std::vector<std::pair<TimelineSourceId, qint64>> TimelineScene::alignStartsToCommonOrigin() const {
  std::vector<std::pair<TimelineSourceId, qint64>> result;
  if (tracks_.empty()) {
    return result;
  }
  qint64 target = displayWindow(tracks_.front()).min;
  for (const TimelineSpanInput& track : tracks_) {
    target = std::min(target, displayWindow(track).min);
  }
  result.reserve(tracks_.size());
  for (const TimelineSpanInput& track : tracks_) {
    result.emplace_back(track.id, track.t_min_ns - target);
  }
  return result;
}

std::vector<std::pair<TimelineSourceId, qint64>> TimelineScene::alignCentersToCommonOrigin() const {
  std::vector<std::pair<TimelineSourceId, qint64>> result;
  if (tracks_.empty()) {
    return result;
  }
  // Displayed center of a track, overflow-safe: window.min + (window.max − window.min) / 2.
  const auto displayed_center = [](const TimelineSpanInput& track) -> qint64 {
    const TimeSpan w = displayWindow(track);
    return w.min + ((w.max - w.min) / 2);
  };
  qint64 target = displayed_center(tracks_.front());
  for (const TimelineSpanInput& track : tracks_) {
    target = std::min(target, displayed_center(track));
  }
  result.reserve(tracks_.size());
  for (const TimelineSpanInput& track : tracks_) {
    // We need raw_center − new_offset == target, and raw_center == displayed_center + offset.
    const qint64 raw_center = displayed_center(track) + track.offset_ns;
    result.emplace_back(track.id, raw_center - target);
  }
  return result;
}

std::vector<std::pair<TimelineSourceId, qint64>> TimelineScene::alignEndsToCommonOrigin() const {
  std::vector<std::pair<TimelineSourceId, qint64>> result;
  if (tracks_.empty()) {
    return result;
  }
  qint64 target = displayWindow(tracks_.front()).max;
  for (const TimelineSpanInput& track : tracks_) {
    target = std::max(target, displayWindow(track).max);
  }
  result.reserve(tracks_.size());
  for (const TimelineSpanInput& track : tracks_) {
    result.emplace_back(track.id, track.t_max_ns - target);
  }
  return result;
}

TimelineScene::EdgeSnap TimelineScene::snapToEdges(
    const std::vector<qint64>& dragged_edges, const std::vector<qint64>& candidate_edges, qint64 raw_delta_ns,
    qint64 threshold_ns) {
  EdgeSnap result{false, raw_delta_ns, 0};
  qint64 best_dist = threshold_ns + 1;
  for (const qint64 edge0 : dragged_edges) {
    for (const qint64 candidate : candidate_edges) {
      const qint64 align_delta = candidate - edge0;  // delta that lands edge0 on candidate
      const qint64 dist = std::llabs(align_delta - raw_delta_ns);
      if (dist < best_dist) {
        best_dist = dist;
        result.snapped = true;
        result.delta_ns = align_delta;
        result.edge_ns = candidate;
      }
    }
  }
  return result;
}

// =============================================================================
// Graphics items — bar / ruler / playhead. Module-private (timeline_detail).
// =============================================================================

namespace timeline_detail {

/// One source rendered as a draggable bar. Ported from the PJ3 prototype's
/// TopicItem and rebound: it no longer reads a model — the widget feeds it pixel
/// geometry (from TimelineScene::barSpan) plus the SourceId/label/color for
/// hit-testing and painting. The "ghost" dx is a live, view-only preview of an
/// in-flight horizontal drag; the authoritative offset is written by the host in
/// response to offsetChangeRequested, never by this item.
class TimelineBarItem : public QGraphicsRectItem {
 public:
  TimelineBarItem(TimelineSourceId id, QString label, const QColor& fill)
      : id_(id), label_(std::move(label)), fill_(fill) {
    setAcceptHoverEvents(true);
    setCursor(Qt::OpenHandCursor);
    setZValue(10);
  }

  [[nodiscard]] TimelineSourceId sourceId() const noexcept {
    return id_;
  }

  /// Current live drag preview offset in pixels (0 when not being dragged).
  [[nodiscard]] double ghostDx() const noexcept {
    return ghost_dx_px_;
  }

  /// Mark this bar as part of the current multi-selection (heavier accent border).
  void setHighlighted(bool on) {
    if (on == highlighted_) {
      return;
    }
    highlighted_ = on;
    update();
  }

  /// Preview a pending horizontal drag of `dx_px` pixels (does not commit).
  void setGhostDx(double dx_px) {
    // boundingRect() widens by |ghost_dx_px_|, so this is a geometry change: the
    // scene's spatial index must be told before the rect changes, or the old
    // (narrower) cached rect leaves repaint trails during a drag.
    prepareGeometryChange();
    ghost_dx_px_ = dx_px;
    update();
  }

  [[nodiscard]] QRectF boundingRect() const override {
    // Widen by the ghost offset so a previewed drag isn't clipped to the old rect.
    const QRectF r = QGraphicsRectItem::boundingRect();
    const double extra = std::abs(ghost_dx_px_);
    return r.adjusted(-extra, 0, extra, 0);
  }

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* /*option*/, QWidget* /*widget*/) override {
    const TimelineColors col = timelineColors();
    const QRectF r = rect().translated(ghost_dx_px_, 0);

    QColor fill = fill_;
    // Dim the fill while a drag preview is active so the original position stays
    // legible underneath the moving ghost.
    fill.setAlphaF(std::abs(ghost_dx_px_) < 0.001 ? 0.8f : 0.5f);

    // Selected/grouped bars get a slightly heavier accent border as a hint.
    QPen border(highlighted_ ? theme::surface(PJ::theme::Surface::Separation, frameworkTheme()) : col.bar_border);
    border.setWidth(highlighted_ ? 2 : 1);
    paintTimelineBar(painter, r, fill, border, label_);
  }

 private:
  TimelineSourceId id_;
  QString label_;
  QColor fill_;
  double ghost_dx_px_ = 0.0;
  bool highlighted_ = false;
};

/// One name-column row: where to paint it (viewport-local y/height, supplied by
/// the Timeline so it matches the bar row exactly) plus the label and bar color.
struct TimelineNameRow {
  double y_top = 0.0;
  double height = 0.0;
  QString name;
  QColor color;
  quint64 id = 0;         // source id for this row (matches a TimelineTrack::id)
  bool selected = false;  // painted with a selection background when set
};

/// Left track-name column. One row per timeline track, pinned on the left so a
/// dataset's name stays visible even when its bar is scrolled out horizontally or
/// squashed thin. The Timeline owns the geometry: it feeds rows in viewport-local
/// y (synced to vertical scroll) so each name aligns exactly with its bar's row.
class TimelineNamePanel : public QWidget {
 public:
  static constexpr int kDefaultWidth = 140;  // initial column width (px); user-resizable
  static constexpr int kMinWidth = 70;       // floor so names never collapse to nothing
  static constexpr int kHeaderHeight = 28;   // top band for the "Datasets" filter; == kRowsTopOffset

  explicit TimelineNamePanel(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(kMinWidth);
    setMouseTracking(true);  // hover (no button) drives the grab cursor

    // Header band aligned with the ruler on the right: the "Datasets" title on the
    // left and a label-less merge button on the right (where the kebab used to be).
    // The band tone + 2-px title indent come from shared QSS keyed on these object
    // names. WA_StyledBackground so the band's QSS fill paints; opaque so rows
    // scrolled up never bleed into the header.
    header_ = new QWidget(this);
    header_->setObjectName(u"timelineDatasetsHeader"_s);
    header_->setAttribute(Qt::WA_StyledBackground, true);
    auto* row = new QHBoxLayout(header_);
    row->setContentsMargins(
        theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
        theme::space(theme::Space::None));
    row->setSpacing(theme::space(theme::Space::None));

    header_label_ = new QLabel(tr("Datasets"), header_);
    header_label_->setObjectName(u"timelineDatasetsLabel"_s);

    // Merge button (merge icon, no label; 24-px to match the app chrome).
    // An SvgButton, so it re-tints itself on theme change — no applyHeaderTheme hook.
    // The Timeline enables it only when ≥2 sources are selected and wires its click.
    merge_button_ = new SvgButton(u":/resources/svg/merge.svg"_s, SvgButton::Size::kDefault, header_);
    merge_button_->setObjectName(u"timelineMergeButton"_s);
    merge_button_->setToolTip(tr("Merge the selected datasets"));
    merge_button_->setEnabled(false);

    row->addWidget(header_label_);
    row->addStretch(1);
    row->addWidget(merge_button_);
  }

  /// The header merge button, so the Timeline can wire its click and toggle its
  /// enabled state from the selection. Never null after construction.
  [[nodiscard]] QToolButton* mergeButton() const {
    return merge_button_;
  }

  void setRows(std::vector<TimelineNameRow> rows) {
    rows_ = std::move(rows);
    update();
  }

  /// Drag feedback while a row is being reordered: which row is lifted, the
  /// cursor y the floating copy follows, and the insertion index (0..N).
  void setDrag(int grabbed, double drag_y, int drop_index) {
    drag_grabbed_ = grabbed;
    drag_y_ = drag_y;
    drop_index_ = drop_index;
    update();
  }
  void clearDrag() {
    drag_grabbed_ = -1;
    drop_index_ = -1;
    update();
  }

  [[nodiscard]] int rowCountForTest() const {
    return static_cast<int>(rows_.size());
  }
  [[nodiscard]] double rowTopForTest(int index) const {
    return rows_.at(static_cast<std::size_t>(index)).y_top;
  }

 protected:
  void paintEvent(QPaintEvent* /*event*/) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor window = timelineBackdrop();
    painter.fillRect(rect(), window);
    // The right divider is drawn by the splitter handle (TimelineSplitterHandle).
    const QFontMetricsF fm(font());

    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
      const TimelineNameRow& row = rows_[static_cast<std::size_t>(i)];
      if (row.y_top + row.height < 0.0 || row.y_top > height()) {
        continue;  // row scrolled out of view
      }
      paintRow(painter, row, row.y_top, fm, i == drag_grabbed_ ? 0.3 : 1.0);
    }

    // Opaque header band so rows scrolled up never bleed into the filter header
    // (the label/search/edit child widgets paint on top of this).
    painter.fillRect(QRectF(0.0, 0.0, width(), kHeaderHeight), window);

    if (drag_grabbed_ < 0 || drag_grabbed_ >= static_cast<int>(rows_.size())) {
      return;
    }
    // Insertion indicator at the drop boundary.
    const QColor accent = theme::interaction(theme::Variant::Accent, theme::State::Nominal, frameworkTheme());
    painter.fillRect(QRectF(0.0, dropLineY() - 1.0, width(), 2.0), accent);
    // Floating copy of the grabbed row, opaque, following the cursor.
    const TimelineNameRow& grabbed = rows_[static_cast<std::size_t>(drag_grabbed_)];
    const double fy = drag_y_ - (grabbed.height / 2.0);
    painter.fillRect(QRectF(0.0, fy, width(), grabbed.height), window);
    paintRow(painter, grabbed, fy, fm, 1.0);
    painter.setPen(QPen(accent, theme::stroke(theme::Stroke::Hairline, frameworkTheme())));
    painter.drawRect(QRectF(0.5, fy + 0.5, width() - 1.0, grabbed.height - 1.0));
  }

  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    header_->setGeometry(0, 0, width(), kHeaderHeight);  // pin the header band to the top
  }

 private:
  void paintRow(QPainter& painter, const TimelineNameRow& row, double y_top, const QFontMetricsF& fm, double opacity) {
    constexpr double kAccentWidth = 4.0;  // color strip tying a name to its bar
    constexpr double kTextPad = 8.0;
    const TimelineColors col = timelineColors();
    painter.setOpacity(opacity);
    if (row.selected) {
      painter.fillRect(
          QRectF(0.0, y_top, width(), row.height), theme::overlay(theme::Overlay::Selected, frameworkTheme()));
    }
    painter.fillRect(QRectF(0.0, y_top, kAccentWidth, row.height), row.color);
    const double text_x = kAccentWidth + kTextPad;
    const double text_w = width() - text_x - kTextPad;
    const QString label = fm.elidedText(row.name, Qt::ElideRight, text_w);
    painter.setPen(col.text);
    painter.drawText(QRectF(text_x, y_top, text_w, row.height), Qt::AlignVCenter | Qt::AlignLeft, label);
    painter.setOpacity(1.0);
  }

  // y of the insertion line for the current drop_index_ (gap midpoint / ends).
  [[nodiscard]] double dropLineY() const {
    if (rows_.empty()) {
      return 0.0;
    }
    if (drop_index_ <= 0) {
      return rows_.front().y_top;
    }
    if (drop_index_ >= static_cast<int>(rows_.size())) {
      return rows_.back().y_top + rows_.back().height;
    }
    const TimelineNameRow& above = rows_[static_cast<std::size_t>(drop_index_) - 1];
    const TimelineNameRow& below = rows_[static_cast<std::size_t>(drop_index_)];
    return ((above.y_top + above.height) + below.y_top) / 2.0;
  }

  std::vector<TimelineNameRow> rows_;
  int drag_grabbed_ = -1;  // lifted row index, -1 = no drag
  double drag_y_ = 0.0;    // cursor y the floating copy tracks
  int drop_index_ = -1;    // insertion index (0..N)
  // Header band widgets (see ctor): the "Datasets" title and the label-less merge
  // button on the right (enabled by the Timeline when ≥2 sources are selected).
  QWidget* header_ = nullptr;
  QLabel* header_label_ = nullptr;
  SvgButton* merge_button_ = nullptr;
};

}  // namespace timeline_detail

// =============================================================================
// Timeline — the QWidget (consolidated SourceTimelineWidget).
// =============================================================================

namespace {
constexpr double kRowHeight = 24.0;
constexpr double kRowGap = 2.0;
// Vertical offset of the first dataset row — the blank header band above it (the
// view's ruler is drawn within it on the view side; sized for a standard icon-row
// of breathing room).
constexpr double kRowsTopOffset = 28.0;  // first-dataset vertical offset (px)
constexpr double kMinBarWidthPx = 2.0;
constexpr double kSnapThresholdPx = 8.0;     // catch distance for edge-snap during a drag
constexpr double kSnapReleaseExtraPx = 6.0;  // extra hysteresis band before an active snap releases
// Chrono-derived so the seconds<->ns factor can never drift from the unit it
// converts (mirrors PJ::kNanosecondsPerSecond in pj_runtime/Time.h, which
// pj_widgets cannot include per its dependency rule). The widget's public
// playhead/reference/range slots speak display-seconds — the fleet-wide
// IDataWidget/PlaybackEngine double contract — and convert to the ns-native
// interior here.
constexpr double kNanosecondsPerSecond = static_cast<double>(std::chrono::nanoseconds::period::den);
constexpr double kNanosecondsPerMillisecond =
    static_cast<double>(std::chrono::nanoseconds::period::den) / std::chrono::milliseconds::period::den;
constexpr double kZoomInFactor = 1.25;
constexpr double kZoomOutFactor = 1.0 / 1.25;
// Padding added to each side of the content so a source can be dragged a little
// near the edges without the scrollable scene immediately growing. The scene only
// grows past this when a bar is pushed beyond the buffer (see computeBufferedExtent).
constexpr qint64 kBufferNs = 60'000'000'000LL;  // ±1 minute

/// Format a display-relative ns offset as a precise H:MM:SS.mmm-ish timestamp for
/// the marker pills (playhead / reference), more precise than the ruler labels.
QString formatMarkerTime(qint64 rel_ns) {
  const bool neg = rel_ns < 0;
  const qint64 total_ms = std::llround(std::abs(static_cast<double>(rel_ns)) / kNanosecondsPerMillisecond);
  const qint64 ms = total_ms % 1000;
  const qint64 sec = (total_ms / 1000) % 60;
  const qint64 min = (total_ms / 1000) / 60;
  QString body;
  if (min > 0) {
    body = u"%1:%2.%3"_s.arg(min).arg(sec, 2, 10, QChar('0')).arg(ms, 3, 10, QChar('0'));
  } else {
    body = u"%1.%2 s"_s.arg(sec).arg(ms, 3, 10, QChar('0'));
  }
  return (neg ? u"-"_s : QString()) + body;
}
}  // namespace

using timeline_detail::TimelineBackgroundItem;
using timeline_detail::TimelineBarItem;
using timeline_detail::TimelineNeedleItem;
using timeline_detail::TimelineRulerItem;

namespace {
// Wheel zoom and name-column drags fire per input event; one
// viewStateChangeCommitted publishes after the gesture goes quiet.
constexpr int kViewStateCommitDebounceMs = 200;
}  // namespace

Timeline::Timeline(QWidget* parent) : QWidget(parent) {
  view_state_commit_debounce_.setSingleShot(true);
  view_state_commit_debounce_.setInterval(kViewStateCommitDebounceMs);
  connect(&view_state_commit_debounce_, &QTimer::timeout, this, &Timeline::viewStateChangeCommitted);
  gscene_ = new QGraphicsScene(this);
  view_ = new QGraphicsView(gscene_, this);
  view_->setRenderHint(QPainter::Antialiasing, true);
  // The background item paints the themed base over the whole scene; this only
  // shows in any viewport area beyond the scene rect. Kept in step via changeEvent.
  view_->setBackgroundBrush(timelineBackdrop());
  view_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  // The native bars are hidden in favour of PJ::Scrollbar overlay pills
  // (h_scrollbar_ / v_scrollbar_). AlwaysOff hides the widget but KEEPS each
  // scrollbar's range + value live, so panning still flows through setValue().
  // Scrollbar::attach() re-applies AlwaysOff, so the lines below are belt-and-
  // suspenders and harmless if called before attach().
  view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  view_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  view_->setTransformationAnchor(QGraphicsView::NoAnchor);
  // No frame: the viewport's top edge then coincides with the view widget's top,
  // so the left name column (top-aligned beside it) lines up row-for-row.
  view_->setFrameShape(QFrame::NoFrame);
  // Mouse tracking so items get hover events without a button held (drives the
  // per-item cursor: open-hand over bars, resize over the needles).
  view_->viewport()->setMouseTracking(true);
  // Forward the view's mouse/wheel events to this widget's overrides so a single
  // set of handlers drives bar drag, playhead seek, scroll, and zoom.
  view_->viewport()->installEventFilter(this);
  // Attach PJ::Scrollbar overlays AFTER Timeline's own viewport event filter so
  // each Scrollbar's filter is installed last (LIFO → called first), letting it
  // consume strip-drag/hover events before Timeline's pan/zoom handler sees them.
  // The Timeline owns its whole viewport (no foreign clickable content under the
  // strips), so opt into click-to-scroll: a press anywhere in the strip grabs and
  // jumps the handle, matching the pre-extraction TimelineScrollPill behavior.
  h_scrollbar_ = new Scrollbar(Qt::Horizontal);
  h_scrollbar_->setClickToScroll(true);
  h_scrollbar_->attach(view_);
  connect(h_scrollbar_, &Scrollbar::scrollChangeCommitted, this, &Timeline::viewStateChangeCommitted);
  v_scrollbar_ = new Scrollbar(Qt::Vertical);
  v_scrollbar_->setClickToScroll(true);
  v_scrollbar_->attach(view_);
  connect(v_scrollbar_, &Scrollbar::scrollChangeCommitted, this, &Timeline::viewStateChangeCommitted);

  align_button_ = new QPushButton(tr("Align"), this);
  align_button_->setObjectName(u"timelineAlignButton"_s);
  align_button_->setToolTip(tr("Shift every source so their starts line up at the earliest start"));
  connect(align_button_, &QPushButton::clicked, this, &Timeline::alignRequested);

  auto* toolbar = new QHBoxLayout;
  toolbar->setContentsMargins(
      theme::space(theme::Space::Snug), theme::space(theme::Space::Snug), theme::space(theme::Space::Snug),
      theme::space(theme::Space::None));
  toolbar->addWidget(align_button_);
  toolbar->addStretch(1);
  // Host the toolbar in its own widget so it can be hidden as a unit. Hidden for
  // now (the Align action stays wired via alignRequested for when it returns); a
  // hidden widget is skipped by the layout, so no empty row is left above the view.
  auto* toolbar_row = new QWidget(this);
  toolbar_row->setLayout(toolbar);
  toolbar_row->hide();

  // Left name column beside the view, in a splitter so its right edge is a
  // draggable separator the user can pull to widen/narrow the column. The panel
  // paints one row per track aligned to the bars; the view scrolls/zooms while the
  // names stay pinned.
  name_panel_ = new timeline_detail::TimelineNamePanel(this);
  name_panel_->installEventFilter(this);  // central handling of row drag-to-reorder
  // The header's merge button (merge icon, no label) lives in the name
  // column's "Datasets" band. The Timeline enables it only when ≥2 sources are
  // selected (see updateMergeButton); a click merges the visible selection.
  connect(name_panel_->mergeButton(), &QToolButton::clicked, this, [this]() {
    const QList<quint64> ids = visibleSelectedIdsList();
    if (!interaction_locked_ && ids.size() >= 2) {
      emit mergeRequested(ids);
    }
  });

  // Plain QSplitter so its handle is the app's standard separator: the global
  // QSS styles QSplitter::handle as a 1-px separator line that turns the
  // Highlight (pressed) accent while grabbed, identical to every other splitter.
  name_splitter_ = new QSplitter(Qt::Horizontal, this);
  name_splitter_->setObjectName(u"timelineNameSplitter"_s);
  name_splitter_->setChildrenCollapsible(false);
  name_splitter_->setHandleWidth(1);
  name_splitter_->addWidget(name_panel_);
  name_splitter_->addWidget(view_);
  name_splitter_->setStretchFactor(0, 0);  // the column keeps its width
  name_splitter_->setStretchFactor(1, 1);  // the view absorbs extra space
  name_splitter_->setSizes({timeline_detail::TimelineNamePanel::kDefaultWidth, 1 << 16});
  // Resizing the column shifts the view, so re-place the lock overlay; and
  // tell the host the user's chosen width so it sticks across rebuilds + persists.
  connect(name_splitter_, &QSplitter::splitterMoved, this, [this](int, int) {
    updateLockOverlayGeometry();
    emit nameColumnWidthChanged(nameColumnWidth());
    view_state_commit_debounce_.start();
  });

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(
      theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
      theme::space(theme::Space::None));
  layout->setSpacing(theme::space(theme::Space::Tight));
  layout->addWidget(toolbar_row);
  layout->addWidget(name_splitter_);

  background_item_ = new TimelineBackgroundItem();
  gscene_->addItem(background_item_);

  ruler_item_ = new TimelineRulerItem();
  gscene_->addItem(ruler_item_);

  // Playback needle: Accent (blue). Reference needle: Highlight (magenta), so the
  // two markers stay visually distinct. Both darken to the pressed state on grab.
  const auto fw_theme = frameworkTheme();
  playhead_item_ = new TimelineNeedleItem(
      theme::interaction(theme::Variant::Accent, theme::State::Checked, fw_theme),
      theme::interaction(theme::Variant::Accent, theme::State::CheckedHovered, fw_theme),
      theme::interaction(theme::Variant::Accent, theme::State::CheckedPressed, fw_theme),
      theme::onFill(theme::Variant::Accent, theme::State::CheckedPressed, fw_theme));
  playhead_item_->setZValue(200);
  gscene_->addItem(playhead_item_);

  reference_item_ = new TimelineNeedleItem(
      theme::interaction(theme::Variant::Highlight, theme::State::Checked, fw_theme),
      theme::interaction(theme::Variant::Highlight, theme::State::CheckedHovered, fw_theme),
      theme::interaction(theme::Variant::Highlight, theme::State::CheckedPressed, fw_theme),
      theme::onFill(theme::Variant::Highlight, theme::State::CheckedPressed, fw_theme));
  reference_item_->setZValue(190);  // just under the playback needle
  reference_item_->setVisible(false);
  gscene_->addItem(reference_item_);

  // Edge-snap alignment guide: a thin vertical line shown while a dragged bar's
  // edge snaps to a neighbour's edge. Above the bars, below the playback needle.
  snap_line_item_ = new QGraphicsLineItem();
  snap_line_item_->setZValue(180);
  snap_line_item_->setPen(QPen(theme::interaction(theme::Variant::Emphasis, theme::State::Nominal, fw_theme), 1.0));
  snap_line_item_->setVisible(false);
  gscene_->addItem(snap_line_item_);

  // "Frozen" overlay: a centered pill shown over the scene while the timeline is
  // interaction-locked (live streaming + playing), telling the user how to interact.
  // Purely informational — WA_TransparentForMouseEvents so it never changes event
  // routing (the press/wheel gates already block interaction). Hidden until locked.
  lock_overlay_ = new QLabel(tr("Streaming: pause playback to interact with the timeline"), this);
  lock_overlay_->setObjectName(u"timelineLockOverlay"_s);
  lock_overlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
  lock_overlay_->setAlignment(Qt::AlignCenter);
  lock_overlay_->setWordWrap(true);
  // Rounded grey pill centered over the scene (geometry set in
  // updateLockOverlayGeometry): sized to the text + padding, NOT a full-width band —
  // prominent enough to read at a glance while leaving the streaming bars visible.
  lock_overlay_->setStyleSheet(
      QStringLiteral(
          "QLabel#timelineLockOverlay { background-color: %1; color: %2; "
          "border: 1px solid %3; border-radius: %4px; padding: %5px %6px; "
          "font-size: 18px; font-weight: 600; }")
          .arg(theme::overlay(theme::Overlay::Hud, fw_theme).name(QColor::HexArgb))
          .arg(theme::onOverlayHud(fw_theme).name(QColor::HexArgb))
          .arg(theme::outline(theme::OutlineRole::Default, theme::OutlineState::Rest, fw_theme).name(QColor::HexArgb))
          .arg(theme::radius(theme::Radius::Dialog))
          .arg(theme::space(theme::Space::Section))
          .arg(theme::space(theme::Space::Section)));
  lock_overlay_->hide();

  // Recenter the lock overlay on scroll / range changes; the PJ::Scrollbar overlays
  // maintain their own geometry via internal connections inside attach().
  connect(view_->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { updateLockOverlayGeometry(); });
  connect(
      view_->horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) { updateLockOverlayGeometry(); });
  connect(view_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
    updateNamePanel();
    syncStickyHeader();  // keep the number line pinned to the top as the rows scroll
    updateLockOverlayGeometry();
  });
  connect(
      view_->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) { updateLockOverlayGeometry(); });

  rebuild();
}

Timeline::~Timeline() {
  // Drop the scrollbar connections before our members tear down. ~QWidget (the
  // base, run after our members are gone) deletes the child QGraphicsScene, which
  // resets the view's scrollbars and emits valueChanged — that would otherwise
  // fire the lambdas into updateNamePanel()/syncStickyHeader() after scene_ et al.
  // are already destroyed. (The `this`-context auto-disconnect only kicks in once
  // ~QObject runs, which is too late.)
  if (view_ != nullptr) {
    view_->horizontalScrollBar()->disconnect(this);
    view_->verticalScrollBar()->disconnect(this);
  }
}

int Timeline::barCount() const {
  return static_cast<int>(bar_items_.size());
}

void Timeline::setTracks(const std::vector<TimelineTrack>& tracks) {
  all_tracks_ = tracks;
  // Hash the RAW data (id + raw bounds, NOT offsets) of the FULL list to tell a
  // genuine data change (load/remove/reload) from an offset-only update (a drag).
  // Computing it from the full list — not the filtered subset — means the dataset
  // filter never resets the extent or triggers auto-zoom. The combine is order-
  // INDEPENDENT (XOR of per-track hashes) so a pure reorder leaves it unchanged.
  quint64 sig = 0;
  for (const TimelineTrack& t : all_tracks_) {
    std::uint64_t h = kFnv1aOffsetBasis;
    h = fnv1aMix(h, t.id);
    h = fnv1aMix(h, static_cast<std::uint64_t>(t.t_min_ns));
    h = fnv1aMix(h, static_cast<std::uint64_t>(t.t_max_ns));
    sig ^= h;
  }
  if (sig != raw_data_sig_) {
    raw_data_sig_ = sig;
    extent_needs_reset_ = true;  // re-pad the scene from the new data bounds
    selected_ids_.clear();       // a real data change invalidates the old selection
    selection_anchor_id_ = 0;    // ...and its Shift-range anchor
  }
  applyDatasetFilter();
}

void Timeline::setDatasetFilter(const QString& text) {
  if (text == dataset_filter_) {
    return;
  }
  dataset_filter_ = text;
  // A filter change re-selects which tracks are shown but is NOT a data change, so
  // it doesn't touch raw_data_sig_/extent_needs_reset_ — the view stays put.
  applyDatasetFilter();
}

void Timeline::applyDatasetFilter() {
  // Feed only the filter-matching tracks into the scene; the bars, name rows,
  // extent, and reorder all operate on this filtered subset, so the rest of the
  // widget needs no filter awareness. Case-insensitive substring match on the
  // dataset name; an empty filter shows everything.
  std::vector<TimelineSpanInput> spans;
  bar_colors_.clear();
  bar_labels_.clear();
  for (const TimelineTrack& t : all_tracks_) {
    if (!dataset_filter_.isEmpty() && !t.name.contains(dataset_filter_, Qt::CaseInsensitive)) {
      continue;
    }
    spans.push_back(
        TimelineSpanInput{.id = t.id, .t_min_ns = t.t_min_ns, .t_max_ns = t.t_max_ns, .offset_ns = t.offset_ns});
    bar_colors_.push_back(t.color);
    bar_labels_.push_back(t.name);
  }
  scene_.setTracks(std::move(spans));
  rebuild();
}

void Timeline::setPlayhead(double display_seconds) {
  // `display_seconds` arrives in the host's external (playback) frame; shift into
  // the bars' internal frame by adding the offset in INTEGER ns (never via an
  // epoch-scale double add, which would lose ~one ulp ≈ 240 ns of precision).
  playhead_ns_ = static_cast<qint64>(std::llround(display_seconds * kNanosecondsPerSecond)) + time_frame_offset_ns_;
  repositionPlayhead();
  // The needle is a pure slave (seekToSceneX no longer sets the pill), so update the
  // grabbed time pill here from the echoed engine value while a drag is in progress.
  if (dragging_playhead_) {
    playhead_item_->setLabel(markerLabel(playhead_ns_));
  }
}

QString Timeline::markerLabel(qint64 display_ns) const {
  // fixed_ms=true so the pill matches the playback readout (always 3 decimals).
  return absolute_time_labels_ ? timeline_detail::formatAbsoluteSeconds(display_ns, /*fixed_ms=*/true)
                               : formatMarkerTime(display_ns - ruler_epoch_ns_);
}

void Timeline::setAbsoluteTimeLabels(bool absolute) {
  if (absolute_time_labels_ == absolute) {
    return;
  }
  absolute_time_labels_ = absolute;
  ruler_item_->setAbsoluteFormat(absolute);
  // Refresh the (lazily-shown) pill labels so the next grab reads the new format.
  playhead_item_->setLabel(markerLabel(playhead_ns_));
  reference_item_->setLabel(markerLabel(reference_ns_));
}

void Timeline::setTimeFrameOffsetNs(qint64 offset_ns) {
  // The ns gap between the host's external (playback) frame — in which the playhead,
  // range, and reference are supplied/emitted — and the bars' internal frame. Adding
  // it in integer ns (here and in the setters) keeps the needle bit-exact with the
  // playback instant; doing the same shift in epoch-scale double seconds would round
  // to ~240 ns and make the timeline and playback readouts disagree on the last digit.
  // Bars are fed in the internal frame directly (setTracks), so they're unaffected.
  const qint64 delta = offset_ns - time_frame_offset_ns_;
  if (delta == 0) {
    return;
  }
  time_frame_offset_ns_ = offset_ns;
  // The playhead/reference hold a fixed playback INSTANT; re-express it in the new bar
  // frame so the needle doesn't strand at the old position when the offset moves (a
  // load/toggle changing the global reference). The host often does NOT re-push
  // currentTime on a load (it may be unchanged), so this self-correction is required.
  // setPlayhead/setReferenceLine still overwrite absolutely, so this never double-applies.
  playhead_ns_ += delta;
  reference_ns_ += delta;
  repositionPlayhead();
  repositionReference();
}

void Timeline::setDisplayRange(double lo_seconds, double hi_seconds) {
  // When tracks are present the displayed extent comes from their data; this
  // range only governs the EMPTY timeline, which we make exactly as long as the
  // playback by adopting [lo, hi] as the empty-state span (rebuild() then fits it
  // to the view width — see the empty-fit block there). Stored on the scene so
  // every sceneExtent() caller (extent, ruler epoch) sees the same empty span.
  scene_.setDefaultExtent(
      static_cast<qint64>(std::llround(lo_seconds * kNanosecondsPerSecond)) + time_frame_offset_ns_,
      static_cast<qint64>(std::llround(hi_seconds * kNanosecondsPerSecond)) + time_frame_offset_ns_);
  rebuild();
}

bool Timeline::isAutoZoomEnabled() const noexcept {
  return auto_zoom_;
}

void Timeline::setAutoZoomEnabled(bool enabled) {
  if (auto_zoom_ == enabled) {
    return;
  }
  auto_zoom_ = enabled;
  if (enabled) {
    fitToContents();  // re-frame the current extent immediately on enable
  }
}

void Timeline::fitToContents() {
  // Request a fit on the next rebuild; rebuild() honors it only when auto-zoom is
  // on and there is data (the empty timeline always fits the playback range).
  fit_pending_ = true;
  // Re-pad the scene around the CURRENT content. computeBufferedExtent() only ever
  // GROWS the extent on an offset-only update, which is right for a drag (small,
  // and the scene must not resize under the cursor) but wrong for the callers of
  // this function: an alignment moves sources by the whole inter-source gap, then
  // collapses the content. Without a reset the scene keeps the width of the
  // pre-alignment spread, so scene_w = (that huge span) x (the zoom just fitted to
  // the small content) — and the full-scene-width ruler and background items grow
  // past what Qt will render, leaving a timeline that draws its bars but loses its
  // ruler, gridlines and hatch.
  extent_needs_reset_ = true;
  rebuild();
}

void Timeline::zoomToFit() {
  // Explicit, one-shot "zoom out horizontally" — fits the largest extent into the
  // view regardless of the auto-zoom preference.
  force_fit_pending_ = true;
  rebuild();
}

double Timeline::zoom() const noexcept {
  return viewport_.px_per_ns;
}

void Timeline::setZoom(double px_per_ns) {
  if (!(px_per_ns > 0.0)) {
    return;  // ignore non-positive / NaN
  }
  viewport_.px_per_ns = std::clamp(px_per_ns, kMinPxPerNs, kMaxPxPerNs);
  rebuild();  // scene width + scrollbar range track the zoom
}

qint64 Timeline::viewportLeftDisplayNs() const {
  // The horizontal scrollbar value is the scene-x of the visible left edge; map
  // it back to display-ns under the current viewport (origin pinned at scene x=0).
  if (view_ == nullptr || view_->horizontalScrollBar() == nullptr) {
    return 0;
  }
  return TimelineScene::pxToNs(static_cast<double>(view_->horizontalScrollBar()->value()), viewport_);
}

int Timeline::viewportTopOffsetPx() const {
  return (view_ != nullptr && view_->verticalScrollBar() != nullptr) ? view_->verticalScrollBar()->value() : 0;
}

void Timeline::setViewportLeftDisplayNs(qint64 display_ns) {
  if (view_ == nullptr || view_->horizontalScrollBar() == nullptr) {
    return;
  }
  // QScrollBar::setValue clamps to [min,max], so an out-of-range position (e.g.
  // a wider window than at save time) lands at the nearest reachable edge.
  const double scene_x = TimelineScene::nsToPx(display_ns, viewport_);
  view_->horizontalScrollBar()->setValue(static_cast<int>(std::llround(scene_x)));
}

void Timeline::setViewportTopOffsetPx(int offset_px) {
  if (view_ != nullptr && view_->verticalScrollBar() != nullptr) {
    view_->verticalScrollBar()->setValue(std::max(0, offset_px));
  }
}

int Timeline::nameColumnWidth() const {
  if (name_splitter_ == nullptr) {
    return 0;
  }
  const QList<int> sizes = name_splitter_->sizes();
  return sizes.isEmpty() ? 0 : sizes.front();
}

void Timeline::setNameColumnWidth(int width_px) {
  if (name_panel_ == nullptr || width_px <= 0) {
    return;
  }
  // Floor the column at this width (it can be dragged wider, never narrower) and
  // pin the splitter so the separator lands exactly at width_px by default.
  name_panel_->setMinimumWidth(width_px);
  resizeNameColumn(width_px);
}

void Timeline::resizeNameColumn(int width_px) {
  if (name_splitter_ == nullptr || width_px <= 0) {
    return;
  }
  const QList<int> sizes = name_splitter_->sizes();
  if (sizes.size() == 2) {
    const int total = sizes[0] + sizes[1];
    name_splitter_->setSizes({width_px, std::max(0, total - width_px)});
  }
}

double Timeline::sceneHeight() const {
  const double rows_h = static_cast<double>(scene_.tracks().size()) * (kRowHeight + kRowGap);
  const double vp_h =
      (view_->viewport() && view_->viewport()->height() > 0) ? static_cast<double>(view_->viewport()->height()) : 100.0;
  return std::max(vp_h, kRowsTopOffset + rows_h);
}

void Timeline::rebuild() {
  // Tearing down items mid-drag would dangle the drag-group items and crash the
  // next move. The host's setTracks() after release triggers a normal rebuild.
  if (!drag_group_.empty()) {
    return;
  }

  for (TimelineBarItem* item : bar_items_) {
    gscene_->removeItem(item);
    delete item;
  }
  bar_items_.clear();

  // Stable across this rebuild; compute the union extent once and reuse it below.
  const TimeSpan scene_extent = scene_.sceneExtent();

  const bool did_reset = extent_needs_reset_;
  buffered_extent_ = computeBufferedExtent();
  extent_needs_reset_ = false;
  // The horizontal scrollbar owns panning, so the origin (display-ns at scene
  // x=0) is pinned to the scene's left edge here — never moved by zoom/scroll.
  viewport_.origin_ns = buffered_extent_.min;
  // Anchor the ruler's 0:00 reference ONCE, when the data changes — not on a
  // drag. This keeps the timing headers stationary while a dataset is moved (the
  // axis is an absolute frame; only the dragged bar moves).
  if (did_reset) {
    ruler_epoch_ns_ = scene_extent.min;
  }

  const double vp_w =
      (view_->viewport() && view_->viewport()->width() > 0) ? static_cast<double>(view_->viewport()->width()) : 100.0;
  // Auto-fit the zoom to the view width. Both cases are gated on isVisible() so
  // they never run during the constructor's pre-show rebuild (whose fallback
  // width would bake a bogus zoom into viewport_ that the first dataset inherits):
  //   * Empty timeline: ALWAYS fit the playback range (buffered_extent_), so the
  //     data-less timeline is exactly as long as the playback.
  //   * Data timeline: fit the largest extent (the union of all bars) when
  //     auto-zoom is on AND new data just arrived (did_reset) or a fit was asked
  //     for (fit_pending_, e.g. after an alignment). Otherwise it keeps the user's
  //     zoom.
  // Re-runs on resize (resizeEvent -> rebuild) so the empty fit stays snug.
  const bool empty = scene_.tracks().empty();
  // force_fit_pending_ is an explicit user "zoom to fit" (the align rail's
  // zoom-out-horizontally button); it fits regardless of the auto-zoom preference.
  const bool do_fit =
      view_->isVisible() && (empty || force_fit_pending_ || (auto_zoom_ && (did_reset || fit_pending_)));
  if (do_fit && vp_w > 0.0) {
    const TimeSpan fit_span = empty ? buffered_extent_ : scene_extent;
    const qint64 span_ns = fit_span.max - fit_span.min;
    if (span_ns > 0) {
      // Clamp to the same floor/ceiling every other zoom path enforces (zoom(),
      // setZoom()); a degenerate (huge) span would otherwise store a sub-floor zoom.
      viewport_.px_per_ns = std::clamp(vp_w / static_cast<double>(span_ns), kMinPxPerNs, kMaxPxPerNs);
    }
  }
  fit_pending_ = false;
  force_fit_pending_ = false;
  const double extent_px = static_cast<double>(buffered_extent_.max - buffered_extent_.min) * viewport_.px_per_ns;
  const double scene_w = std::max(vp_w, extent_px);
  const double scene_h = sceneHeight();
  gscene_->setSceneRect(0, 0, scene_w, scene_h);

  // Compute the ruler tick layout ONCE per rebuild and feed it to both the
  // background gridlines and the ruler numbers, so the two share a single tick
  // list (no per-paint recompute, no comment-only "same positions" contract).
  const TimelineRuler ruler = TimelineScene::ruler(viewport_, scene_w);
  // The data union span both tints the ruler and bounds the empty-area hatch.
  const TimeSpan data_span = scene_.tracks().empty() ? TimeSpan{0, 0} : scene_extent;
  background_item_->setLayout(scene_w, scene_h, viewport_, ruler, data_span.min, data_span.max);

  double y = kRowsTopOffset;
  const std::vector<TimelineSpanInput>& tracks = scene_.tracks();
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    const TimelineSpanInput& track = tracks[i];
    const PxSpan span = TimelineScene::barSpan(track, viewport_);
    const double width = std::max(span.width, kMinBarWidthPx);
    const QColor color = (i < bar_colors_.size()) ? bar_colors_[i] : QColor(Qt::white);
    const QString label = (i < bar_labels_.size()) ? bar_labels_[i] : QString();
    auto* item = new TimelineBarItem(track.id, label, color);
    item->setRect(0, 0, width, kRowHeight);
    item->setPos(span.x, y);
    item->setHighlighted(selected_ids_.count(track.id) != 0);
    gscene_->addItem(item);
    bar_items_.push_back(item);
    y += kRowHeight + kRowGap;
  }

  rebuildRuler(ruler, data_span);
  playhead_item_->setHeight(scene_h);
  reference_item_->setHeight(scene_h);
  repositionPlayhead();
  repositionReference();

  // Whenever we (re-)fit — fresh data, an empty timeline, or an explicit
  // fitToContents (alignment) — scroll so the earliest content sits at the left
  // edge: data fills the view from its start (±1 min buffer reachable by scrolling
  // left), and the empty timeline opens with 0:00 flush at the left edge (under
  // the playback track start). Only on a fit, so it never fights user scrolling.
  if (did_reset || do_fit) {
    const qint64 content_min = scene_extent.min;
    view_->horizontalScrollBar()->setValue(
        static_cast<int>(std::llround(TimelineScene::nsToPx(content_min, viewport_))));
  }

  updateNamePanel();          // names follow the same row layout the bars just got
  syncStickyHeader();         // pin the ruler + needle pills to the current viewport top
  applySelectionHighlight();  // freshly-built bars start unhighlighted; restore selection
  updateMergeButton();        // selection may have been pruned by a data change
}

void Timeline::syncStickyHeader() {
  if (ruler_item_ == nullptr) {
    return;
  }
  // The viewport top maps to scene-y == verticalScrollBar value (no vertical
  // scaling). Keep the ruler (and the needles' header anchor) there so the rows
  // scroll underneath a stationary number line.
  const auto vscroll = static_cast<double>(view_->verticalScrollBar()->value());
  ruler_item_->setPos(0, vscroll);
  if (playhead_item_ != nullptr) {
    playhead_item_->setHeaderTop(vscroll);
  }
  if (reference_item_ != nullptr) {
    reference_item_->setHeaderTop(vscroll);
  }
}

TimeSpan Timeline::computeBufferedExtent() const {
  const TimeSpan content = scene_.sceneExtent();
  // Empty timeline: span EXACTLY the default extent (the playback range), with no
  // buffer and no sticky growth — so it is always exactly as long as the playback
  // (tracking setDisplayRange even when extent_needs_reset_ is false) and the
  // empty-fit in rebuild() makes that span fill the view, 0ms flush at the left
  // edge. The sticky-buffer logic below is only meaningful while bars exist.
  if (scene_.tracks().empty()) {
    return content;
  }
  if (extent_needs_reset_) {
    // Fresh data: pad ±1 min around the content.
    return {content.min - kBufferNs, content.max + kBufferNs};
  }
  // Offset-only update (a drag): keep the current padded extent so small drags
  // inside the buffer don't resize the scene. Re-pad only the side a bar was
  // pushed past, so the extent grows (never shrinks) when dragged beyond.
  TimeSpan out = buffered_extent_;
  if (content.min < out.min) {
    out.min = content.min - kBufferNs;
  }
  if (content.max > out.max) {
    out.max = content.max + kBufferNs;
  }
  return out;
}

void Timeline::updateLockOverlayGeometry() {
  if (lock_overlay_ == nullptr) {
    return;
  }
  const QWidget* vp = view_->viewport();
  // Centered rounded pill sized to its text. adjustSize() alone mis-sizes a
  // word-wrapped QLabel into a tall narrow box, so the width is set explicitly (the
  // single-line natural width, capped to the viewport so a narrow timeline wraps
  // instead of overflowing) and adjustSize only derives the matching height.
  constexpr int kMargin = 24;
  const int max_w = std::max(120, vp->width() - 2 * kMargin);
  lock_overlay_->setWordWrap(false);
  const int natural_w = lock_overlay_->sizeHint().width();
  lock_overlay_->setWordWrap(natural_w > max_w);
  lock_overlay_->setFixedWidth(std::min(natural_w, max_w));
  lock_overlay_->adjustSize();
  const int x = (vp->width() - lock_overlay_->width()) / 2;
  const int y = (vp->height() - lock_overlay_->height()) / 2;
  lock_overlay_->move(vp->mapTo(this, QPoint(std::max(0, x), std::max(0, y))));
}

void Timeline::updateNamePanel() {
  if (name_panel_ == nullptr) {
    return;
  }
  // Each row's scene y is the bar row's y; subtract the vertical scroll so the
  // panel (top-aligned with the frameless viewport) lines up row-for-row.
  const double vscroll = static_cast<double>(view_->verticalScrollBar()->value());
  const std::vector<TimelineSpanInput>& tracks = scene_.tracks();
  std::vector<timeline_detail::TimelineNameRow> rows;
  rows.reserve(tracks.size());
  double y = kRowsTopOffset;
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    timeline_detail::TimelineNameRow row;
    row.y_top = y - vscroll;
    row.height = kRowHeight;
    row.name = (i < bar_labels_.size()) ? bar_labels_[i] : QString();
    row.color = (i < bar_colors_.size()) ? bar_colors_[i] : QColor(Qt::white);
    row.id = tracks[i].id;
    row.selected = selected_ids_.count(tracks[i].id) != 0;
    rows.push_back(std::move(row));
    y += kRowHeight + kRowGap;
  }
  name_panel_->setRows(std::move(rows));
}

int Timeline::nameRowAt(double panel_y) const {
  const double vscroll = static_cast<double>(view_->verticalScrollBar()->value());
  const int n = static_cast<int>(scene_.tracks().size());
  for (int i = 0; i < n; ++i) {
    const double top = (kRowsTopOffset + (i * (kRowHeight + kRowGap))) - vscroll;
    if (panel_y >= top && panel_y < top + kRowHeight) {
      return i;
    }
  }
  return -1;
}

int Timeline::nameDropIndex(double panel_y) const {
  const double vscroll = static_cast<double>(view_->verticalScrollBar()->value());
  const int n = static_cast<int>(scene_.tracks().size());
  for (int i = 0; i < n; ++i) {
    const double center = (kRowsTopOffset + (i * (kRowHeight + kRowGap)) + (kRowHeight / 2.0)) - vscroll;
    if (panel_y < center) {
      return i;  // insert before row i
    }
  }
  return n;  // after the last row
}

void Timeline::namePanelPress(const QPoint& panel_pos, Qt::KeyboardModifiers mods) {
  if (interaction_locked_) {
    name_drag_index_ = -1;  // Locked: no row reorder / selection.
    return;
  }
  const int row = nameRowAt(panel_pos.y());
  selectNameRow(row, mods);
  // A plain press on a real row primes a potential drag-to-reorder (needs ≥2
  // tracks). A modifier press only edits the selection — never starts a reorder.
  const bool plain = (mods & (Qt::ControlModifier | Qt::ShiftModifier | Qt::MetaModifier)) == 0;
  name_drag_index_ = (plain && row >= 0 && scene_.tracks().size() >= 2) ? row : -1;
  name_dragging_ = false;
  name_drag_press_y_ = panel_pos.y();
}

void Timeline::namePanelMove(const QPoint& panel_pos, bool button_down) {
  if (!button_down || name_drag_index_ < 0) {
    // Hover: show the grab cursor over a row when reordering is possible.
    const bool grabbable = scene_.tracks().size() >= 2 && nameRowAt(panel_pos.y()) >= 0;
    name_panel_->setCursor(grabbable ? Qt::OpenHandCursor : Qt::ArrowCursor);
    return;
  }
  constexpr double kDragThresholdPx = 4.0;
  if (!name_dragging_ && std::abs(panel_pos.y() - name_drag_press_y_) < kDragThresholdPx) {
    return;  // not enough movement to start a drag yet
  }
  name_dragging_ = true;
  name_panel_->setCursor(Qt::ClosedHandCursor);
  name_panel_->setDrag(name_drag_index_, panel_pos.y(), nameDropIndex(panel_pos.y()));
}

void Timeline::namePanelRelease(const QPoint& panel_pos) {
  const int from = name_drag_index_;
  const bool was_dragging = name_dragging_;
  name_drag_index_ = -1;
  name_dragging_ = false;
  name_panel_->clearDrag();
  name_panel_->setCursor(nameRowAt(panel_pos.y()) >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
  if (was_dragging && from >= 0) {
    applyTrackReorder(from, nameDropIndex(panel_pos.y()));
  }
}

void Timeline::applyTrackReorder(int from, int drop_index) {
  if (interaction_locked_) {
    return;  // Locked: track order is frozen.
  }
  const std::vector<TimelineSpanInput>& spans = scene_.tracks();
  const int n = static_cast<int>(spans.size());
  if (from < 0 || from >= n || n < 2) {
    return;
  }
  int ins = std::clamp(drop_index, 0, n);
  if (ins > from) {
    --ins;  // the removed row shifts later indices down by one
  }
  if (ins == from) {
    return;  // dropped back where it started
  }
  // New index order: all rows except `from`, with `from` re-inserted at `ins`.
  std::vector<int> order;
  order.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    if (i != from) {
      order.push_back(i);
    }
  }
  order.insert(order.begin() + ins, from);
  // Reconstruct the track list in the new order and re-feed via setTracks. The
  // data signature is order-independent, so this re-lays-out the rows without
  // resetting the extent/scroll. Then announce the order for the host to adopt.
  std::vector<TimelineTrack> reordered;
  reordered.reserve(static_cast<std::size_t>(n));
  QList<quint64> ids;
  for (int idx : order) {
    const TimelineSpanInput& s = spans[static_cast<std::size_t>(idx)];
    const auto u_idx = static_cast<std::size_t>(idx);
    TimelineTrack track;
    track.id = s.id;
    track.t_min_ns = s.t_min_ns;
    track.t_max_ns = s.t_max_ns;
    track.offset_ns = s.offset_ns;
    track.name = (u_idx < bar_labels_.size()) ? bar_labels_[u_idx] : QString();
    track.color = (u_idx < bar_colors_.size()) ? bar_colors_[u_idx] : QColor(Qt::white);
    reordered.push_back(std::move(track));
    ids.push_back(s.id);
  }
  setTracks(reordered);
  emit tracksReordered(ids);
}

void Timeline::rebuildRuler(const TimelineRuler& ruler, const TimeSpan& data) {
  const double width_px = gscene_->sceneRect().width();
  // `data` is the data-covered span (union of all datasets, or an empty span when
  // there are no tracks) rebuild() already computed — it tints the ruler distinctly
  // from the empty ±buffer.
  // Labels are relative to the FIXED epoch (set on data load), so the timing
  // headers stay put while a dataset is dragged.
  ruler_item_->setLayout(viewport_, width_px, ruler, ruler_epoch_ns_, data.min, data.max);
  // Keep the ruler at the current viewport top (sticky); rebuild() also calls
  // syncStickyHeader() after this, but positioning here keeps rebuildRuler correct
  // on its own.
  ruler_item_->setPos(0, static_cast<double>(view_->verticalScrollBar()->value()));
}

void Timeline::updateDataSpanFromItems() {
  if (bar_items_.empty()) {
    ruler_item_->setDataSpan(0, 0);
    background_item_->setDataSpan(0, 0);
    return;
  }
  // Union the bars' current VISUAL extents (item position + the in-flight drag's
  // ghost dx), so the ruler tint AND the empty-area hatch track the dragged
  // bar(s) live. Reading the items (not scene_) keeps this correct whether or
  // not a host has fed the offset back.
  double min_x = std::numeric_limits<double>::max();
  double max_x = std::numeric_limits<double>::lowest();
  for (const TimelineBarItem* bar : bar_items_) {
    const double x0 = bar->pos().x() + bar->ghostDx();
    const double x1 = x0 + bar->rect().width();
    min_x = std::min(min_x, x0);
    max_x = std::max(max_x, x1);
  }
  const qint64 lo = TimelineScene::pxToNs(min_x, viewport_);
  const qint64 hi = TimelineScene::pxToNs(max_x, viewport_);
  ruler_item_->setDataSpan(lo, hi);
  background_item_->setDataSpan(lo, hi);
}

void Timeline::repositionPlayhead() {
  const double x = TimelineScene::nsToPx(playhead_ns_, viewport_);
  playhead_item_->setPos(x, 0);
}

double Timeline::seekToSceneX(double scene_x) {
  if (interaction_locked_) {
    // Locked: the needle is view-only — report the current playhead (external
    // frame) without emitting a seek, so callers/test seams see no movement.
    return static_cast<double>(playhead_ns_ - time_frame_offset_ns_) / kNanosecondsPerSecond;
  }
  // Clamp to the data extent (union of the bars, or the playback range when empty)
  // so an out-of-range drag emits a sane value. The needle is a PURE SLAVE: we do
  // NOT move playhead_ns_ here. The host writes this time to the PlaybackEngine, and
  // its currentTimeChanged echo (setPlayhead, synchronous) is the SINGLE source that
  // repositions the needle — so it stays exactly in lockstep with the playback handle
  // (same source, same value) instead of holding a locally-set value the engine never
  // adopted.
  const TimeSpan extent = scene_.sceneExtent();
  const qint64 ns = std::clamp(TimelineScene::pxToNs(scene_x, viewport_), extent.min, extent.max);
  // Emit in the host's external (playback) frame: undo the frame offset in integer
  // ns first, then divide — the difference is small, so the double stays precise.
  const double seconds = static_cast<double>(ns - time_frame_offset_ns_) / kNanosecondsPerSecond;
  emit playheadSeeked(seconds);
  return seconds;
}

const TimelineSpanInput* Timeline::trackById(TimelineSourceId id) const {
  for (const TimelineSpanInput& track : scene_.tracks()) {
    if (track.id == id) {
      return &track;
    }
  }
  return nullptr;
}

qint64 Timeline::baseOffsetOf(TimelineSourceId id) const {
  const TimelineSpanInput* track = trackById(id);
  return track != nullptr ? track->offset_ns : 0;
}

qint64 Timeline::offsetForBarDelta(TimelineSourceId id, double dx_px) const {
  // display = raw - offset. Dragging RIGHT (+dx) moves the bar to a LATER display
  // time, which means the offset must DECREASE: offset_new = base - delta_ns.
  const qint64 delta_ns = TimelineScene::pxDeltaToNs(dx_px, viewport_);
  return baseOffsetOf(id) - delta_ns;
}

void Timeline::setSnapEnabled(bool enabled) {
  snap_enabled_ = enabled;
  if (!enabled) {
    hideSnapLine();
  }
}

bool Timeline::snapEnabled() const {
  return snap_enabled_;
}

void Timeline::setInteractionLocked(bool locked) {
  if (interaction_locked_ == locked) {
    return;
  }
  interaction_locked_ = locked;
  // The widget's own Align button greys out; the host also disables its external
  // align rail (the rail drives the controller's align slots directly).
  if (align_button_ != nullptr) {
    align_button_->setEnabled(!locked);
  }
  // Show/hide the "pause to interact" overlay over the scene.
  if (lock_overlay_ != nullptr) {
    if (locked) {
      updateLockOverlayGeometry();
      lock_overlay_->show();
      lock_overlay_->raise();
    } else {
      lock_overlay_->hide();
    }
  }
  if (locked) {
    // Cancel any in-flight gesture so a drag started just before the lock can't
    // dangle (the press gates below stop NEW gestures; this clears live ones).
    drag_group_.clear();
    dragging_playhead_ = false;
    dragging_reference_ = false;
    panning_ = false;
    name_drag_index_ = -1;
    name_dragging_ = false;
    hideSnapLine();
    if (view_ != nullptr) {
      view_->viewport()->unsetCursor();
    }
  }
  // The merge prompt is suppressed while locked, restored (if ≥2 still selected) on unlock.
  updateMergeButton();
  // Disable/enable scroll-pill drag on the overlay scrollbars. The Scrollbar
  // filter runs first (LIFO; installed after Timeline's own viewport filter), so
  // setInteractive(false) is required to stop it consuming strip-drag events while
  // the timeline is locked — returning early in our handlers is not sufficient.
  if (h_scrollbar_ != nullptr) {
    h_scrollbar_->setInteractive(!locked);
  }
  if (v_scrollbar_ != nullptr) {
    v_scrollbar_->setInteractive(!locked);
  }
}

Timeline::DragSnap Timeline::computeDragSnap(qint64 raw_delta_ns) {
  if (drag_group_.empty() || viewport_.px_per_ns <= 0.0) {
    snap_active_ = false;
    return {false, raw_delta_ns, 0};
  }
  // Dragged edges come from the DRAG-START display position (raw bounds − base
  // offset). The host may have fed the mid-drag offset back into scene_ during the
  // drag, so reading scene_ for the dragged bars would double-count raw_delta and
  // make the snap target drift/flicker. Candidates are the fixed neighbours.
  std::set<TimelineSourceId> dragged;
  std::vector<qint64> dragged_edges;
  for (const DragMember& m : drag_group_) {
    dragged.insert(m.id);
    if (const TimelineSpanInput* track = trackById(m.id)) {
      dragged_edges.push_back(track->t_min_ns - m.base_offset);
      dragged_edges.push_back(track->t_max_ns - m.base_offset);
    }
  }
  std::vector<qint64> candidate_edges;
  for (const TimelineSpanInput& track : scene_.tracks()) {
    if (dragged.count(track.id) != 0) {
      continue;
    }
    const TimeSpan w = TimelineScene::displayWindow(track);
    candidate_edges.push_back(w.min);
    candidate_edges.push_back(w.max);
  }

  const auto catch_ns = static_cast<qint64>(std::llround(kSnapThresholdPx / viewport_.px_per_ns));
  // Hysteresis: keep the active snap while the cursor stays within a wider release
  // band, so it doesn't chatter between two nearby candidates. "Keep dragging to
  // unsnap" = leave that band.
  const auto release_ns =
      static_cast<qint64>(std::llround((kSnapThresholdPx + kSnapReleaseExtraPx) / viewport_.px_per_ns));
  if (snap_active_ && std::llabs(snap_active_delta_ - raw_delta_ns) <= release_ns) {
    return {true, snap_active_delta_, snap_active_edge_};
  }
  const TimelineScene::EdgeSnap snap =
      TimelineScene::snapToEdges(dragged_edges, candidate_edges, raw_delta_ns, catch_ns);
  snap_active_ = snap.snapped;
  snap_active_delta_ = snap.delta_ns;
  snap_active_edge_ = snap.edge_ns;
  return {snap.snapped, snap.delta_ns, snap.edge_ns};
}

void Timeline::showSnapLine(qint64 display_ns) {
  const double x = TimelineScene::nsToPx(display_ns, viewport_);
  snap_line_item_->setLine(x, TimelineRulerItem::kRulerHeight, x, sceneHeight());
  snap_line_item_->setVisible(true);
}

void Timeline::hideSnapLine() {
  if (snap_line_item_ != nullptr) {
    snap_line_item_->setVisible(false);
  }
}

void Timeline::repositionReference() {
  reference_item_->setPos(TimelineScene::nsToPx(reference_ns_, viewport_), 0);
}

void Timeline::moveReferenceToSceneX(double scene_x) {
  if (interaction_locked_) {
    return;  // Locked: the reference line is view-only — no user drag.
  }
  // Clamp to the data extent, same as the playhead — a reference outside the data
  // is meaningless and would otherwise be snapped back by the host's clamp.
  const TimeSpan extent = scene_.sceneExtent();
  const qint64 ns = std::clamp(TimelineScene::pxToNs(scene_x, viewport_), extent.min, extent.max);
  reference_ns_ = ns;
  repositionReference();
  reference_item_->setLabel(markerLabel(ns));
  // Emit in the host's external (playback) frame — undo the offset in integer ns.
  emit referenceLineMoved(static_cast<double>(ns - time_frame_offset_ns_) / kNanosecondsPerSecond);
}

void Timeline::setReferenceLine(double display_seconds, bool visible) {
  reference_visible_ = visible;
  reference_item_->setVisible(visible);
  if (!visible) {
    return;
  }
  reference_ns_ = static_cast<qint64>(std::llround(display_seconds * kNanosecondsPerSecond)) + time_frame_offset_ns_;
  repositionReference();
  reference_item_->setLabel(markerLabel(reference_ns_));
}

void Timeline::applyBarDragForTest(TimelineSourceId id, double dx_px) {
  if (interaction_locked_) {
    return;  // Locked: bar-offset editing is suppressed (matches the real drag gate).
  }
  emit offsetChangeRequested(id, offsetForBarDelta(id, dx_px));
}

double Timeline::seekToSceneXForTest(double scene_x) {
  // Pure slave: seekToSceneX no longer moves playhead_ns_, it returns the clamped
  // display-seconds it emitted. That emitted (clamped) value is what the test pins.
  return seekToSceneX(scene_x);
}

double Timeline::moveReferenceToSceneXForTest(double scene_x) {
  moveReferenceToSceneX(scene_x);
  return static_cast<double>(reference_ns_) / kNanosecondsPerSecond;
}

TimeSpan Timeline::sceneExtentForTest() const {
  return scene_.sceneExtent();
}

qint64 Timeline::playheadNsForTest() const {
  return playhead_ns_;
}

QString Timeline::markerLabelForTest(qint64 ns) const {
  return markerLabel(ns);
}

void Timeline::pressViewportForTest(QPoint viewport_pos) {
  // Route through the full event-filter chain so the PJ::Scrollbar overlay (installed
  // last = called first in LIFO order) can consume strip events before Timeline sees them.
  const QPointF global = view_->viewport()->mapToGlobal(viewport_pos);
  QMouseEvent ev(
      QEvent::MouseButtonPress, QPointF(viewport_pos), global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(view_->viewport(), &ev);
}

void Timeline::moveViewportForTest(QPoint viewport_pos, bool button_down) {
  const QPointF global = view_->viewport()->mapToGlobal(viewport_pos);
  const Qt::MouseButtons buttons = button_down ? Qt::LeftButton : Qt::NoButton;
  QMouseEvent ev(QEvent::MouseMove, QPointF(viewport_pos), global, Qt::NoButton, buttons, Qt::NoModifier);
  QCoreApplication::sendEvent(view_->viewport(), &ev);
}

void Timeline::releaseViewportForTest(QPoint viewport_pos) {
  const QPointF global = view_->viewport()->mapToGlobal(viewport_pos);
  QMouseEvent ev(
      QEvent::MouseButtonRelease, QPointF(viewport_pos), global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(view_->viewport(), &ev);
}

bool Timeline::isScrollPillShownForTest() const {
  return h_scrollbar_ != nullptr && h_scrollbar_->isShown();
}

int Timeline::scrollValueForTest() const {
  return view_->horizontalScrollBar()->value();
}

bool Timeline::horizontalScrollBarVisibleForTest() const {
  return view_->horizontalScrollBar()->isVisible();
}

int Timeline::viewportHeightForTest() const {
  return view_->viewport()->height();
}

int Timeline::viewportWidthForTest() const {
  return view_->viewport()->width();
}

bool Timeline::isVScrollPillShownForTest() const {
  return v_scrollbar_ != nullptr && v_scrollbar_->isShown();
}

bool Timeline::verticalScrollBarVisibleForTest() const {
  return view_->verticalScrollBar()->isVisible();
}

int Timeline::verticalScrollValueForTest() const {
  return view_->verticalScrollBar()->value();
}

void Timeline::setVerticalScrollForTest(int value) {
  view_->verticalScrollBar()->setValue(value);
}

double Timeline::rulerSceneYForTest() const {
  return ruler_item_ != nullptr ? ruler_item_->y() : 0.0;
}

int Timeline::nameRowCountForTest() const {
  return name_panel_->rowCountForTest();
}

double Timeline::nameRowTopForTest(int index) const {
  return name_panel_->rowTopForTest(index);
}

double Timeline::barRowTopForTest(int index) const {
  return bar_items_.at(static_cast<std::size_t>(index))->pos().y();
}

QList<quint64> Timeline::trackOrderForTest() const {
  QList<quint64> ids;
  for (const TimelineSpanInput& s : scene_.tracks()) {
    ids.push_back(s.id);
  }
  return ids;
}

void Timeline::reorderForTest(int from, int drop_index) {
  applyTrackReorder(from, drop_index);
}

void Timeline::selectNameRowForTest(int row, Qt::KeyboardModifiers mods) {
  selectNameRow(row, mods);
}

bool Timeline::mergeButtonEnabledForTest() const {
  return name_panel_ != nullptr && name_panel_->mergeButton()->isEnabled();
}

QList<quint64> Timeline::visibleSelectedIdsForTest() const {
  return visibleSelectedIdsList();
}

bool Timeline::beginSyntheticDrag(TimelineSourceId id) {
  drag_group_.clear();
  snap_active_ = false;  // start the synthetic drag with no sticky snap held
  for (TimelineBarItem* bar : bar_items_) {
    if (bar->sourceId() == id) {
      drag_group_.push_back(DragMember{.item = bar, .id = id, .base_offset = baseOffsetOf(id)});
      return true;
    }
  }
  return false;
}

void Timeline::dragBarForSnapTest(TimelineSourceId id, double dx_px) {
  hideSnapLine();
  if (!beginSyntheticDrag(id)) {
    return;
  }
  const qint64 delta_ns = TimelineScene::pxDeltaToNs(dx_px, viewport_);
  if (snap_enabled_) {
    const DragSnap snap = computeDragSnap(delta_ns);
    if (snap.snapped) {
      showSnapLine(snap.line_display_ns);
    } else {
      hideSnapLine();
    }
  }
  drag_group_.clear();  // leave the guide-line state; end the synthetic drag
}

bool Timeline::snapLineVisibleForTest() const {
  return snap_line_item_ != nullptr && snap_line_item_->isVisible();
}

std::vector<bool> Timeline::dragSnapSequenceForTest(TimelineSourceId id, const std::vector<double>& dxs) {
  std::vector<bool> states;
  // One continuous drag for the whole sequence (beginSyntheticDrag resets the
  // sticky snap_active_ once, so hysteresis carries across the dxs).
  if (!beginSyntheticDrag(id)) {
    return states;
  }
  states.reserve(dxs.size());
  for (const double dx : dxs) {
    const qint64 delta_ns = TimelineScene::pxDeltaToNs(dx, viewport_);
    states.push_back(snap_enabled_ && computeDragSnap(delta_ns).snapped);
  }
  drag_group_.clear();
  return states;
}

void Timeline::selectNameRow(int row, Qt::KeyboardModifiers mods) {
  const std::vector<TimelineSpanInput>& tracks = scene_.tracks();
  const bool toggle = (mods & (Qt::ControlModifier | Qt::MetaModifier)) != 0;
  const bool range = (mods & Qt::ShiftModifier) != 0;

  if (row < 0 || row >= static_cast<int>(tracks.size())) {
    // Clicked below the rows: a plain click clears; a modifier click keeps the
    // current selection (matches a standard list view).
    if (!toggle && !range) {
      clearSelection();
    }
    return;
  }

  const TimelineSourceId id = tracks[static_cast<std::size_t>(row)].id;
  if (toggle) {
    if (selected_ids_.count(id) != 0) {
      selected_ids_.erase(id);
    } else {
      selected_ids_.insert(id);
    }
    selection_anchor_id_ = id;
  } else if (range && selection_anchor_id_ != 0) {
    // Select the contiguous run between the anchor and this row (display order).
    int anchor_row = -1;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
      if (tracks[static_cast<std::size_t>(i)].id == selection_anchor_id_) {
        anchor_row = i;
        break;
      }
    }
    if (anchor_row < 0) {  // anchor scrolled away / merged: treat as a fresh single pick
      selected_ids_ = {id};
      selection_anchor_id_ = id;
    } else {
      selected_ids_.clear();
      for (int i = std::min(anchor_row, row); i <= std::max(anchor_row, row); ++i) {
        selected_ids_.insert(tracks[static_cast<std::size_t>(i)].id);
      }
    }
  } else {
    selected_ids_ = {id};
    selection_anchor_id_ = id;
  }

  applySelectionHighlight();
  updateNamePanel();
  updateMergeButton();
}

void Timeline::clearSelection() {
  if (selected_ids_.empty() && selection_anchor_id_ == 0) {
    return;
  }
  selected_ids_.clear();
  selection_anchor_id_ = 0;
  applySelectionHighlight();
  updateNamePanel();
  updateMergeButton();
}

void Timeline::applySelectionHighlight() {
  for (TimelineBarItem* bar : bar_items_) {
    bar->setHighlighted(selected_ids_.count(bar->sourceId()) != 0);
  }
}

QList<quint64> Timeline::selectedIdsList() const {
  QList<quint64> ids;
  ids.reserve(static_cast<qsizetype>(selected_ids_.size()));
  for (const TimelineSourceId id : selected_ids_) {
    ids.push_back(id);
  }
  return ids;
}

QList<quint64> Timeline::visibleSelectedIdsList() const {
  // Selected ids that survive the current dataset filter (i.e. are in scene_),
  // in display order. The merge prompt + action operate on this, so the filter
  // can hide a selected source without the prompt offering to merge it.
  QList<quint64> ids;
  for (const TimelineSpanInput& t : scene_.tracks()) {
    if (selected_ids_.count(t.id) != 0) {
      ids.push_back(t.id);
    }
  }
  return ids;
}

void Timeline::updateMergeButton() {
  if (name_panel_ != nullptr) {
    // The header merge button is enabled only with ≥2 filter-visible selected
    // sources (and not while interaction-locked). Disabled otherwise.
    name_panel_->mergeButton()->setEnabled(!interaction_locked_ && visibleSelectedIdsList().size() >= 2);
  }
}

void Timeline::wheelEvent(QWheelEvent* event) {
  // Locked (e.g. live streaming + playing): the view is frozen at the live edge —
  // no zoom. Swallow the event so it can't navigate.
  if (interaction_locked_) {
    event->accept();
    return;
  }
  const int angle = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
  if (angle == 0) {
    QWidget::wheelEvent(event);
    return;
  }

  // The wheel zooms horizontally, anchored at the cursor: the instant under the
  // pointer stays fixed on screen (like the plot's X-axis wheel-zoom). The origin
  // stays pinned to the scene's left edge (rebuild owns it); we keep the anchored
  // ns under the cursor by adjusting the horizontal scrollbar after the rebuild.
  // Pan is the left-drag gesture (see mousePressEvent); the scroll pill remains.
  const double factor = angle > 0 ? kZoomInFactor : kZoomOutFactor;
  const double anchor_vp_x = event->position().x();
  const double anchor_scene_x = view_->mapToScene(event->position().toPoint()).x();
  const qint64 anchor_ns = TimelineScene::pxToNs(anchor_scene_x, viewport_);
  // zoom() only scales + clamps px_per_ns here; its origin output is ignored.
  viewport_.px_per_ns = TimelineScene::zoom(viewport_, factor, anchor_scene_x).px_per_ns;
  rebuild();
  const double new_anchor_scene_x = TimelineScene::nsToPx(anchor_ns, viewport_);
  view_->horizontalScrollBar()->setValue(static_cast<int>(std::llround(new_anchor_scene_x - anchor_vp_x)));
  view_state_commit_debounce_.start();
  event->accept();
}

void Timeline::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QWidget::mousePressEvent(event);
    return;
  }
  const QPoint view_pos = view_->viewport()->mapFromGlobal(event->globalPosition().toPoint());

  // Locked (e.g. live streaming + playing): the whole view is frozen — no needle
  // seek, or bar-offset drag. Scrolling/zooming is also blocked (see wheelEvent).
  // The needles still track playback via setPlayhead. The PJ::Scrollbar overlays
  // are also suppressed: their event filter runs FIRST (installed last, LIFO order),
  // and setInteractionLocked calls h_scrollbar_->setInteractive(false) /
  // v_scrollbar_->setInteractive(false) so their filter passes all events through
  // without starting or consuming a drag — this filter then runs and returns early.
  if (interaction_locked_) {
    event->accept();
    return;
  }

  QGraphicsItem* item = view_->itemAt(view_pos);
  const QPointF scene_pos = view_->mapToScene(view_pos);

  // Grabbing the (visible) reference needle: drag it to reposition the reference.
  if (reference_visible_ && item == reference_item_) {
    dragging_reference_ = true;
    reference_item_->setGrabbed(true);  // dark-blue needle + current-time pill
    moveReferenceToSceneX(scene_pos.x());
    event->accept();
    return;
  }

  // Playhead handle or a click within the ruler band: start a seek drag.
  if (item == playhead_item_ || (item == ruler_item_ && scene_pos.y() < TimelineRulerItem::kRulerHeight)) {
    dragging_playhead_ = true;
    playhead_item_->setGrabbed(true);  // dark-purple needle + current-time pill
    seekToSceneX(scene_pos.x());
    event->accept();
    return;
  }

  // Bar drag. If the pressed bar is part of the current selection, drag the
  // whole selection together (group move); otherwise drag just this bar.
  if (auto* bar = dynamic_cast<TimelineBarItem*>(item)) {
    const TimelineSourceId pressed_id = bar->sourceId();
    const bool group_move = selected_ids_.count(pressed_id) != 0;
    drag_group_.clear();
    bar_drag_changed_ = false;
    snap_active_ = false;  // fresh drag: start with no sticky snap held
    drag_start_scene_x_ = scene_pos.x();
    for (TimelineBarItem* candidate : bar_items_) {
      const TimelineSourceId cid = candidate->sourceId();
      const bool in_drag = group_move ? (selected_ids_.count(cid) != 0) : (cid == pressed_id);
      if (!in_drag) {
        continue;
      }
      drag_group_.push_back(DragMember{.item = candidate, .id = cid, .base_offset = baseOffsetOf(cid)});
    }
    view_->viewport()->setCursor(Qt::ClosedHandCursor);
    event->accept();
    return;
  }

  // Empty space (not a bar/needle): a left-drag pans the view; a plain click
  // (no movement past the threshold) clears the name-column selection. mouseMove
  // promotes this to a pan once the cursor moves; mouseRelease decides which it was.
  panning_ = true;
  pan_moved_ = false;
  pan_start_global_x_ = event->globalPosition().x();
  pan_start_scroll_value_ = view_->horizontalScrollBar()->value();
  event->accept();
}

void Timeline::mouseMoveEvent(QMouseEvent* event) {
  const QPoint view_pos = view_->viewport()->mapFromGlobal(event->globalPosition().toPoint());
  const QPointF scene_pos = view_->mapToScene(view_pos);

  // Background pan: drag the view horizontally (content follows the cursor, so the
  // scrollbar moves opposite the delta). A move past the threshold commits it to a
  // pan, so the release no longer reads as a deselecting click.
  if (panning_) {
    constexpr double kPanThresholdPx = 3.0;
    const double dx = event->globalPosition().x() - pan_start_global_x_;
    if (!pan_moved_ && std::abs(dx) >= kPanThresholdPx) {
      pan_moved_ = true;
      view_->viewport()->setCursor(Qt::ClosedHandCursor);
    }
    if (pan_moved_) {
      view_->horizontalScrollBar()->setValue(pan_start_scroll_value_ - static_cast<int>(std::llround(dx)));
    }
    event->accept();
    return;
  }

  if (dragging_reference_) {
    moveReferenceToSceneX(scene_pos.x());
    event->accept();
    return;
  }
  if (dragging_playhead_) {
    seekToSceneX(scene_pos.x());
    event->accept();
    return;
  }
  if (!drag_group_.empty()) {
    const double dx_px = scene_pos.x() - drag_start_scene_x_;
    qint64 delta_ns = TimelineScene::pxDeltaToNs(dx_px, viewport_);
    bar_drag_changed_ = bar_drag_changed_ || delta_ns != 0;
    double applied_dx_px = dx_px;
    // Edge-snap: align a dragged start/end onto a neighbour's start/end when within
    // the threshold; a guide line marks it. Dragging past the threshold releases it.
    if (snap_enabled_) {
      const DragSnap snap = computeDragSnap(delta_ns);
      if (snap.snapped) {
        delta_ns = snap.delta_ns;
        applied_dx_px = static_cast<double>(delta_ns) * viewport_.px_per_ns;  // ghost follows the snap
        showSnapLine(snap.line_display_ns);
      } else {
        hideSnapLine();
      }
    }
    // Every group member shifts by the SAME (possibly snapped) delta (rigid move).
    for (const DragMember& m : drag_group_) {
      m.item->setGhostDx(applied_dx_px);
      // Live intent per member; the host applies each and feeds state back.
      emit offsetChangeRequested(m.id, m.base_offset - delta_ns);
    }
    // Keep the ruler tint AND the empty-area hatch tracking the moving group in
    // real time (a full rebuild is suppressed mid-drag, so update them directly).
    updateDataSpanFromItems();
    event->accept();
    return;
  }
  // No drag in progress: the PJ::Scrollbar overlays handle hover-reveal and
  // leave-hide via their own event filter on the viewport.
  QWidget::mouseMoveEvent(event);
}

void Timeline::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QWidget::mouseReleaseEvent(event);
    return;
  }
  if (panning_) {
    panning_ = false;
    if (pan_moved_) {
      view_->viewport()->unsetCursor();
      emit viewStateChangeCommitted();
    } else {
      clearSelection();  // a plain background click (no pan) deselects
    }
    event->accept();
    return;
  }
  if (dragging_reference_) {
    dragging_reference_ = false;
    reference_item_->setGrabbed(false);  // back to the idle light-blue needle
    event->accept();
    return;
  }
  if (dragging_playhead_) {
    dragging_playhead_ = false;
    playhead_item_->setGrabbed(false);  // back to the idle pink needle, pill hidden
    event->accept();
    return;
  }
  if (!drag_group_.empty()) {
    const bool changed = bar_drag_changed_;
    for (const DragMember& m : drag_group_) {
      m.item->setGhostDx(0.0);
    }
    drag_group_.clear();
    bar_drag_changed_ = false;
    hideSnapLine();
    view_->viewport()->unsetCursor();
    // A rebuild was suppressed during the drag; re-lay-out now from whatever the
    // host wrote back (or the original state if it ignored the intent).
    rebuild();
    if (changed) {
      emit offsetChangeCommitted();
    }
    event->accept();
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

void Timeline::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  rebuild();
  updateLockOverlayGeometry();  // re-overlay the lock overlay at the new size
}

void Timeline::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange ||
      event->type() == QEvent::StyleChange) {
    // Items read framework tokens each paint; refresh the view's own backing
    // brush and force a full repaint so the theme switch is immediate. The
    // needles captured their colors at construction — re-resolve them here.
    const auto fw_theme = frameworkTheme();
    if (playhead_item_ != nullptr) {
      playhead_item_->setColors(
          theme::interaction(theme::Variant::Accent, theme::State::Checked, fw_theme),
          theme::interaction(theme::Variant::Accent, theme::State::CheckedHovered, fw_theme),
          theme::interaction(theme::Variant::Accent, theme::State::CheckedPressed, fw_theme),
          theme::onFill(theme::Variant::Accent, theme::State::CheckedPressed, fw_theme));
    }
    if (reference_item_ != nullptr) {
      reference_item_->setColors(
          theme::interaction(theme::Variant::Highlight, theme::State::Checked, fw_theme),
          theme::interaction(theme::Variant::Highlight, theme::State::CheckedHovered, fw_theme),
          theme::interaction(theme::Variant::Highlight, theme::State::CheckedPressed, fw_theme),
          theme::onFill(theme::Variant::Highlight, theme::State::CheckedPressed, fw_theme));
    }
    view_->setBackgroundBrush(timelineBackdrop());
    gscene_->update();
    view_->viewport()->update();
    if (name_panel_ != nullptr) {
      name_panel_->update();  // re-reads framework tokens on repaint
      // The name panel re-tints its own header merge button via its changeEvent.
    }
  }
}

bool Timeline::eventFilter(QObject* watched, QEvent* event) {
  if (watched == name_panel_) {
    switch (event->type()) {
      case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
          namePanelPress(me->position().toPoint(), me->modifiers());
          return true;
        }
        break;
      }
      case QEvent::MouseMove:
        namePanelMove(
            static_cast<QMouseEvent*>(event)->position().toPoint(),
            (static_cast<QMouseEvent*>(event)->buttons() & Qt::LeftButton) != 0);
        return name_dragging_;
      case QEvent::MouseButtonRelease:
        if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
          namePanelRelease(static_cast<QMouseEvent*>(event)->position().toPoint());
          return true;
        }
        break;
      default:
        break;
    }
    return QWidget::eventFilter(watched, event);
  }
  if (watched == view_->viewport()) {
    switch (event->type()) {
      case QEvent::MouseButtonPress:
        mousePressEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();
      case QEvent::MouseMove:
        mouseMoveEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();
      case QEvent::MouseButtonRelease:
        mouseReleaseEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();
      case QEvent::Wheel:
        wheelEvent(static_cast<QWheelEvent*>(event));
        return event->isAccepted();
      default:
        break;
    }
  }
  return QWidget::eventFilter(watched, event);
}

}  // namespace PJ
