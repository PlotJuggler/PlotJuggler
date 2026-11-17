// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/StateTransitionsView.h"

#include <QDataStream>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFontMetricsF>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QIODevice>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <utility>

#include "TimelineItems.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/Scrollbar.h"
#include "pj_widgets/StateColors.h"
using namespace Qt::StringLiterals;

namespace PJ {

void StateTransitionsScene::setRows(std::vector<StateRow> rows) {
  rows_ = std::move(rows);
}

bool StateTransitionsScene::updateRow(StateRow row) {
  const int index = rowIndexOf(row.id);
  if (index < 0) {
    return false;
  }
  rows_[static_cast<std::size_t>(index)] = std::move(row);
  return true;
}

bool StateTransitionsScene::removeRow(quint64 id) {
  const int index = rowIndexOf(id);
  if (index < 0) {
    return false;
  }
  rows_.erase(rows_.begin() + index);
  return true;
}

int StateTransitionsScene::rowIndexOf(quint64 id) const noexcept {
  for (std::size_t index = 0; index < rows_.size(); ++index) {
    if (rows_[index].id == id) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

double StateTransitionsScene::contentHeight() const noexcept {
  return kTopPad + (rowHeight() * static_cast<double>(rows_.size()));
}

TimeSpan StateTransitionsScene::sceneExtent() const noexcept {
  std::optional<qint64> lo;
  std::optional<qint64> hi;
  for (const StateRow& row : rows_) {
    if (row.segments.empty()) {
      continue;
    }
    const qint64 row_min = row.segments.front().t_start_ns;
    const qint64 row_max = row.segments.back().t_end_ns;
    lo = lo ? std::min(*lo, row_min) : row_min;
    hi = hi ? std::max(*hi, row_max) : row_max;
  }
  if (!lo || !hi || *hi <= *lo) {
    return default_extent_;
  }
  return {.min = *lo, .max = *hi};
}

void StateTransitionsScene::setDefaultExtent(qint64 min_ns, qint64 max_ns) noexcept {
  if (max_ns > min_ns) {
    default_extent_ = {.min = min_ns, .max = max_ns};
  }
}

std::optional<StateTransitionsScene::HoverHit> StateTransitionsScene::hitTest(
    double x_px, double y_px, const TimelineViewport& viewport) const {
  const double y_rel = y_px - kTopPad;
  if (y_rel < 0.0) {
    return std::nullopt;
  }
  const int row_index = static_cast<int>(std::floor(y_rel / rowHeight()));
  if (row_index < 0 || row_index >= static_cast<int>(rows_.size())) {
    return std::nullopt;
  }
  constexpr RowMetrics kMetrics;
  const double within = y_rel - (rowHeight() * row_index);
  const double band_top = kMetrics.chip_h;
  if (within < band_top || within >= band_top + kMetrics.band_h) {
    return std::nullopt;
  }

  const std::vector<StateSegment>& segments = rows_[static_cast<std::size_t>(row_index)].segments;
  const qint64 time_ns = TimelineScene::pxToNs(x_px, viewport);
  // Last segment whose start is <= time_ns; containment is half-open so the
  // instant t_end_ns already belongs to the next segment.
  auto it = std::ranges::upper_bound(
      segments, time_ns, [](qint64 value, qint64 t_start) { return value < t_start; }, &StateSegment::t_start_ns);
  if (it == segments.begin()) {
    return std::nullopt;
  }
  --it;
  if (time_ns >= it->t_end_ns) {
    return std::nullopt;
  }
  return HoverHit{.row = row_index, .segment = static_cast<int>(std::distance(segments.begin(), it))};
}

namespace state_transitions_detail {

namespace {
constexpr double kNameLeftPad = 6.0;     // gap between the viewport edge and the series name
constexpr double kNameMaxTextW = 260.0;  // elide ceiling for long series names
constexpr double kLabelFontPx = 11.0;
constexpr double kNameFontPx = 13.0;
constexpr double kMinSegmentPx = 1.0;   // paint floor so brief states stay visible
constexpr double kBandFillAlpha = 0.8;  // same translucency as the Timeline's dataset bars

QFont labelFont() {
  QFont font;
  font.setPixelSize(static_cast<int>(kLabelFontPx));
  return font;
}

// Series-name line above the band: frameless text, a step larger and semi-bold
// so the row identity reads at a glance without chrome.
QFont nameFont() {
  QFont font;
  font.setPixelSize(static_cast<int>(kNameFontPx));
  font.setWeight(QFont::DemiBold);
  return font;
}
}  // namespace

/// One series row. The view feeds geometry: the item lives at (0, StateTransitionsScene::rowTop(index)) and spans the
/// scene width; the chip is painted at the current viewport-left x (setSticky), so it never pans away with the data.
/// Painting culls to the visible x window.
class StateRowItem : public QGraphicsItem {
 public:
  explicit StateRowItem(StateRow row) : row_(std::move(row)) {}

  [[nodiscard]] quint64 rowId() const noexcept {
    return row_.id;
  }

  void setRow(StateRow row) {
    row_ = std::move(row);
    update();
  }

  /// Scene width + ns<->px mapping for this rebuild.
  void setLayout(const TimelineViewport& viewport, double scene_width) {
    prepareGeometryChange();
    viewport_ = viewport;
    scene_width_ = scene_width;
    update();
  }

  /// Current visible x window (scene coords): chips pin to `left_x`, painting
  /// culls to [left_x, left_x + width].
  void setSticky(double left_x, double width) {
    if (qFuzzyCompare(left_x, sticky_left_x_) && qFuzzyCompare(width, sticky_width_)) {
      return;
    }
    sticky_left_x_ = left_x;
    sticky_width_ = width;
    update();
  }

  /// The x-span a segment's label may occupy: the segment clipped to the visible viewport. A label stays readable while
  /// any part of its segment is on screen: a segment wider than the view would otherwise carry its label far off-screen
  /// left under horizontal zoom and pan.
  [[nodiscard]] std::pair<double, double> labelSpan(double x0, double x1) const {
    if (sticky_width_ <= 0.0) {
      return {x0, x1};  // no viewport known yet (headless/pre-layout): full bar
    }
    return {std::max(x0, sticky_left_x_), std::min(x1, sticky_left_x_ + sticky_width_)};
  }

  /// Per-segment "wide enough to carry its state name inside" flags: the rule the shared bar painter applies
  /// (kBarMinLabelWidthPx) over the same viewport-clipped labelSpan the paint path uses; the seam behind
  /// StateTransitionsView::visibleLabelsForTest.
  [[nodiscard]] std::vector<bool> labelVisibility() const {
    std::vector<bool> visible;
    visible.reserve(row_.segments.size());
    for (const StateSegment& segment : row_.segments) {
      const double x0 = TimelineScene::nsToPx(segment.t_start_ns, viewport_);
      const double x1 = TimelineScene::nsToPx(segment.t_end_ns, viewport_);
      const auto [label_x0, label_x1] = labelSpan(x0, x1);
      visible.push_back((label_x1 - label_x0) > timeline_detail::kBarMinLabelWidthPx);
    }
    return visible;
  }

  [[nodiscard]] QRectF boundingRect() const override {
    constexpr StateTransitionsScene::RowMetrics kMetrics;
    return QRectF(0, 0, scene_width_, kMetrics.chip_h + kMetrics.band_h);
  }

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* /*option*/, QWidget* /*widget*/) override {
    constexpr StateTransitionsScene::RowMetrics kMetrics;
    const timeline_detail::TimelineColors col = timeline_detail::timelineColors();
    const auto fw_theme = timeline_detail::frameworkTheme();
    painter->setRenderHint(QPainter::Antialiasing, false);

    const double view_left = sticky_left_x_;
    const double view_right = sticky_left_x_ + sticky_width_;
    const qint64 left_ns = TimelineScene::pxToNs(view_left, viewport_);
    const qint64 right_ns = TimelineScene::pxToNs(view_right, viewport_);

    // Color band: visible segments only (binary search for the first candidate), each drawn through the shared timeline
    // bar painter (translucent fill, hairline border, the state name elided inside the rectangle), as the Source
    // Timeline's dataset bars.
    painter->setFont(labelFont());
    const double band_y = kMetrics.chip_h;
    auto first = std::ranges::lower_bound(
        row_.segments, left_ns, [](qint64 t_end, qint64 value) { return t_end <= value; }, &StateSegment::t_end_ns);
    for (auto it = first; it != row_.segments.end() && it->t_start_ns <= right_ns; ++it) {
      const double x0 = TimelineScene::nsToPx(it->t_start_ns, viewport_);
      const double x1 = TimelineScene::nsToPx(it->t_end_ns, viewport_);
      const QRectF rect(x0, band_y, std::max(x1 - x0, kMinSegmentPx), kMetrics.band_h);
      QColor fill = stateColor(it->value);
      fill.setAlphaF(kBandFillAlpha);
      // Label pinned to the segment's VISIBLE portion, not its (possibly
      // off-screen) left edge, and centered within it.
      const auto [label_x0, label_x1] = labelSpan(rect.left(), rect.right());
      const QRectF label_rect(label_x0, band_y, label_x1 - label_x0, kMetrics.band_h);
      timeline_detail::paintTimelineBar(
          painter, rect, fill, QPen(col.bar_border, 1.0), it->value, label_rect, Qt::AlignHCenter);
    }

    // --- series name, pinned to the viewport's left edge: frameless semi-bold
    // text (no background chrome).
    Q_UNUSED(fw_theme);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setFont(nameFont());
    const QFontMetricsF name_fm{nameFont()};
    const QString elided = name_fm.elidedText(row_.name, Qt::ElideMiddle, kNameMaxTextW);
    painter->setPen(col.text);
    constexpr StateTransitionsScene::RowMetrics kRowMetrics;
    painter->drawText(
        QRectF(sticky_left_x_ + kNameLeftPad, 0.0, kNameMaxTextW, kRowMetrics.chip_h), Qt::AlignVCenter | Qt::AlignLeft,
        elided);
  }

 private:
  StateRow row_;
  TimelineViewport viewport_;
  double scene_width_ = 0.0;
  double sticky_left_x_ = 0.0;
  double sticky_width_ = 0.0;
};

}  // namespace state_transitions_detail

namespace {
constexpr int kViewStateCommitDebounceMs = 250;
constexpr double kZoomInFactor = 1.25;
constexpr double kZoomOutFactor = 1.0 / 1.25;
constexpr double kNanosecondsPerSecond = static_cast<double>(std::chrono::nanoseconds::period::den);
const char* const kCurveDragMime = "curveslist/add_curve";

qint64 secondsToNs(double seconds) {
  return static_cast<qint64>(std::llround(seconds * kNanosecondsPerSecond));
}

/// Tooltip body for a hovered segment: value, start instant, duration.
QString segmentTooltip(const StateSegment& segment) {
  const double start_s = static_cast<double>(segment.t_start_ns) / kNanosecondsPerSecond;
  const double duration_s = static_cast<double>(segment.t_end_ns - segment.t_start_ns) / kNanosecondsPerSecond;
  return u"%1\n%2: %3 s\n%4: %5 s"_s.arg(
      segment.value, QObject::tr("start"), QString::number(start_s, 'f', 3), QObject::tr("duration"),
      QString::number(duration_s, 'f', 3));
}
}  // namespace

StateTransitionsView::StateTransitionsView(QWidget* parent) : QWidget(parent) {
  using timeline_detail::TimelineBackgroundItem;
  using timeline_detail::TimelineNeedleItem;
  using timeline_detail::TimelineRulerItem;

  view_state_commit_debounce_.setSingleShot(true);
  view_state_commit_debounce_.setInterval(kViewStateCommitDebounceMs);
  connect(&view_state_commit_debounce_, &QTimer::timeout, this, &StateTransitionsView::viewStateChangeCommitted);

  gscene_ = new QGraphicsScene(this);
  view_ = new QGraphicsView(gscene_, this);
  view_->setRenderHint(QPainter::Antialiasing, true);
  view_->setBackgroundBrush(timeline_detail::timelineBackdrop());
  view_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  view_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  view_->setTransformationAnchor(QGraphicsView::NoAnchor);
  view_->setFrameShape(QFrame::NoFrame);
  // Hover tooltips need move events without a button held.
  view_->viewport()->setMouseTracking(true);
  // One handler set for zoom/pan/hover/chip-close: filter the viewport and
  // re-dispatch (same seam Timeline uses).
  view_->viewport()->installEventFilter(this);
  // One scroll pill, vertical rows only: the time axis is navigated by zoom, pan and linked zoom, so no horizontal
  // scroller appears over the ruler. Attached after our filter (LIFO: its filter runs first), so a strip-drag never
  // reaches the pan handler.
  v_scrollbar_ = new Scrollbar(Qt::Vertical);
  v_scrollbar_->setClickToScroll(true);
  v_scrollbar_->attach(view_);
  connect(v_scrollbar_, &Scrollbar::scrollChangeCommitted, this, &StateTransitionsView::viewStateChangeCommitted);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(view_);

  background_item_ = new TimelineBackgroundItem();
  background_item_->setGridTop(0.0);  // bottom ruler: gridlines span the rows
  gscene_->addItem(background_item_);

  ruler_item_ = new TimelineRulerItem();
  ruler_item_->setBottomAligned(true);
  // Tick labels are always the numeric display-axis value in seconds, the number the plot X axis shows (small with the
  // relative frame on, epoch seconds when off), so the two cannot disagree. The Source Timeline's elapsed-vs-absolute
  // relabelling has no equivalent here.
  ruler_item_->setAbsoluteFormat(true);
  // Match the plot axis: same label size, and NO data-span tint on the band
  // (rebuild passes an empty span) so the ruler shares the rows' background.
  ruler_item_->setLabelPixelSize(13);
  gscene_->addItem(ruler_item_);

  // Playback needle in Highlight (magenta), the token the plots' tracker line uses, so the two cursors read as one.
  // Read-only in this strip (no click-to-seek): mouse-transparent, no hover lift, no drag cursor.
  const auto fw_theme = timeline_detail::frameworkTheme();
  playhead_item_ = new TimelineNeedleItem(
      theme::interaction(theme::Variant::Highlight, theme::State::Checked, fw_theme),
      theme::interaction(theme::Variant::Highlight, theme::State::CheckedHovered, fw_theme),
      theme::interaction(theme::Variant::Highlight, theme::State::CheckedPressed, fw_theme),
      theme::onFill(theme::Variant::Highlight, theme::State::CheckedPressed, fw_theme));
  playhead_item_->setBottomRuler(true);
  playhead_item_->setZValue(200);
  playhead_item_->setAcceptHoverEvents(false);
  playhead_item_->setAcceptedMouseButtons(Qt::NoButton);
  playhead_item_->unsetCursor();
  gscene_->addItem(playhead_item_);

  // Sticky geometry follows both scroll axes; only horizontal motion changes
  // the visible time window (the linked-zoom feed).
  connect(view_->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
    syncStickyGeometry();
    if (!applying_external_) {
      user_navigated_ = true;
      emitVisibleRangeChanged();
      view_state_commit_debounce_.start();
    }
  });
  connect(view_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { syncStickyGeometry(); });

  setAcceptDrops(true);
  // Drops land on this widget, not the view: a QGraphicsView swallows drag
  // events for its scene, and no scene item accepts them.
  view_->setAcceptDrops(false);
  view_->viewport()->setAcceptDrops(false);

  rebuild();
}

StateTransitionsView::~StateTransitionsView() {
  // The scrollbar connections are dropped before our members tear down. ~QWidget (the base, run after our members are
  // gone) deletes the child QGraphicsScene, which resets the view's scrollbars and emits valueChanged, firing the
  // sticky-sync lambdas into deleted graphics items. (The `this`-context auto-disconnect only runs in ~QObject, too
  // late.)
  if (view_ != nullptr) {
    view_->horizontalScrollBar()->disconnect(this);
    view_->verticalScrollBar()->disconnect(this);
  }
}

void StateTransitionsView::setDroppablePredicate(DroppablePredicate predicate) {
  droppable_ = std::move(predicate);
}

double StateTransitionsView::zoom() const noexcept {
  return viewport_.px_per_ns;
}

qint64 StateTransitionsView::viewportLeftDisplayNs() const {
  return TimelineScene::pxToNs(static_cast<double>(view_->horizontalScrollBar()->value()), viewport_);
}

int StateTransitionsView::viewportTopOffsetPx() const {
  return view_->verticalScrollBar()->value();
}

std::optional<std::pair<double, double>> StateTransitionsView::dataExtentSeconds() const {
  const bool any_data = std::ranges::any_of(scene_.rows(), [](const StateRow& row) { return !row.segments.empty(); });
  if (!any_data) {
    return std::nullopt;  // sceneExtent() would report the default extent, not data
  }
  const TimeSpan extent = scene_.sceneExtent();
  return std::pair<double, double>{
      static_cast<double>(extent.min) / kNanosecondsPerSecond, static_cast<double>(extent.max) / kNanosecondsPerSecond};
}

int StateTransitionsView::rowCountForTest() const {
  return static_cast<int>(row_items_.size());
}

StateRow StateTransitionsView::rowForTest(int row_index) const {
  if (row_index < 0 || row_index >= static_cast<int>(scene_.rows().size())) {
    return {};
  }
  return scene_.rows()[static_cast<std::size_t>(row_index)];
}

qint64 StateTransitionsView::playheadNsForTest() const noexcept {
  return playhead_ns_;
}

std::vector<bool> StateTransitionsView::visibleLabelsForTest(int row_index) const {
  if (row_index < 0 || row_index >= static_cast<int>(row_items_.size())) {
    return {};
  }
  return row_items_[static_cast<std::size_t>(row_index)]->labelVisibility();
}

void StateTransitionsView::dropKeysForTest(const QStringList& catalog_keys) {
  handleDrop(catalog_keys);
}

bool StateTransitionsView::wouldAcceptDropForTest(const QStringList& catalog_keys) const {
  return anyKeyDroppable(catalog_keys);
}

void StateTransitionsView::wheelZoomForTest(double factor, double anchor_viewport_x) {
  applyWheelZoom(factor, anchor_viewport_x);
}

double StateTransitionsView::rulerViewportYForTest() const {
  return ruler_item_->pos().y() - view_->verticalScrollBar()->value();
}

int StateTransitionsView::verticalScrollForTest() const {
  return view_->verticalScrollBar()->value();
}

void StateTransitionsView::setVerticalScrollForTest(int value) {
  view_->verticalScrollBar()->setValue(value);
}

void StateTransitionsView::setRows(const std::vector<StateRow>& rows) {
  scene_.setRows(rows);
  if (!rows.empty() && !user_navigated_) {
    fit_pending_ = true;
  }
  rebuild();
}

void StateTransitionsView::updateRow(const StateRow& row) {
  if (!scene_.updateRow(row)) {
    return;
  }
  const int index = scene_.rowIndexOf(row.id);
  // A data refresh can grow the extent (live edge) — cheap full rebuild only
  // when the scene span actually changed; otherwise refresh the one item.
  const TimeSpan extent = scene_.sceneExtent();
  const bool extent_grew =
      TimelineScene::nsToPx(extent.max, viewport_) > (gscene_ != nullptr ? gscene_->sceneRect().right() : 0.0);
  if (extent_grew || TimelineScene::nsToPx(extent.min, viewport_) < 0.0) {
    rebuild();
    return;
  }
  if (index >= 0 && index < static_cast<int>(row_items_.size())) {
    row_items_[static_cast<std::size_t>(index)]->setRow(row);
  }
}

void StateTransitionsView::removeRow(quint64 id) {
  if (scene_.removeRow(id)) {
    rebuild();
  }
}

void StateTransitionsView::setPlayhead(double display_seconds) {
  playhead_ns_ = secondsToNs(display_seconds);
  repositionPlayhead();
}

void StateTransitionsView::setDisplayRange(double lo_seconds, double hi_seconds) {
  range_min_ns_ = secondsToNs(lo_seconds);
  range_max_ns_ = secondsToNs(hi_seconds);
  scene_.setDefaultExtent(range_min_ns_, range_max_ns_);
  rebuild();
}

void StateTransitionsView::setVisibleRange(double t_min_seconds, double t_max_seconds) {
  const qint64 min_ns = secondsToNs(t_min_seconds);
  const qint64 max_ns = secondsToNs(t_max_seconds);
  if (max_ns <= min_ns) {
    return;
  }
  const double vp_w = viewportWidthPx();
  if (vp_w <= 0.0) {
    return;
  }
  applying_external_ = true;
  user_navigated_ = true;  // an externally-driven window is still a chosen window
  viewport_.px_per_ns = std::clamp(vp_w / static_cast<double>(max_ns - min_ns), 1e-12, 1e-1);
  rebuild();
  view_->horizontalScrollBar()->setValue(static_cast<int>(std::llround(TimelineScene::nsToPx(min_ns, viewport_))));
  applying_external_ = false;
}

void StateTransitionsView::setZoom(double px_per_ns) {
  if (!(px_per_ns > 0.0)) {
    return;  // ignore non-positive / NaN
  }
  applying_external_ = true;
  user_navigated_ = true;
  viewport_.px_per_ns = std::clamp(px_per_ns, 1e-12, 1e-1);
  rebuild();
  applying_external_ = false;
}

void StateTransitionsView::setViewportLeftDisplayNs(qint64 display_ns) {
  applying_external_ = true;
  view_->horizontalScrollBar()->setValue(static_cast<int>(std::llround(TimelineScene::nsToPx(display_ns, viewport_))));
  applying_external_ = false;
}

void StateTransitionsView::setViewportTopOffsetPx(int offset_px) {
  view_->verticalScrollBar()->setValue(std::max(0, offset_px));
}

void StateTransitionsView::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  applying_external_ = true;  // a resize is not a user navigation gesture
  rebuild();
  applying_external_ = false;
}

void StateTransitionsView::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange ||
      event->type() == QEvent::PaletteChange) {
    const auto fw_theme = timeline_detail::frameworkTheme();
    view_->setBackgroundBrush(timeline_detail::timelineBackdrop());
    playhead_item_->setColors(
        theme::interaction(theme::Variant::Highlight, theme::State::Checked, fw_theme),
        theme::interaction(theme::Variant::Highlight, theme::State::CheckedHovered, fw_theme),
        theme::interaction(theme::Variant::Highlight, theme::State::CheckedPressed, fw_theme),
        theme::onFill(theme::Variant::Highlight, theme::State::CheckedPressed, fw_theme));
    gscene_->update();
  }
}

void StateTransitionsView::dragEnterEvent(QDragEnterEvent* event) {
  if (event->mimeData()->hasFormat(kCurveDragMime) && anyKeyDroppable(decodeCurveKeys(event->mimeData()))) {
    event->acceptProposedAction();
  } else {
    event->ignore();
  }
}

void StateTransitionsView::dragMoveEvent(QDragMoveEvent* event) {
  event->acceptProposedAction();
}

void StateTransitionsView::dropEvent(QDropEvent* event) {
  const QStringList keys = decodeCurveKeys(event->mimeData());
  if (!anyKeyDroppable(keys)) {
    event->ignore();
    return;
  }
  event->acceptProposedAction();
  handleDrop(keys);
}

bool StateTransitionsView::eventFilter(QObject* watched, QEvent* event) {
  if (watched != view_->viewport()) {
    return QWidget::eventFilter(watched, event);
  }
  switch (event->type()) {
    case QEvent::Resize: {
      // The outer widget's resizeEvent can run before the child layout settles the viewport geometry (LayoutRequest is
      // deferred), so an empty strip would keep a stale bottom-ruler position: the viewport's own resize is the
      // authoritative moment to re-pin.
      applying_external_ = true;  // geometry change, not a user navigation
      rebuild();
      applying_external_ = false;
      return false;  // QGraphicsView still needs the event
    }
    case QEvent::Wheel: {
      auto* wheel = static_cast<QWheelEvent*>(event);
      const int angle = wheel->angleDelta().y() != 0 ? wheel->angleDelta().y() : wheel->angleDelta().x();
      if (angle == 0) {
        return false;
      }
      if ((wheel->modifiers() & Qt::ShiftModifier) != 0) {
        // Shift+wheel scrolls rows vertically.
        QScrollBar* vbar = view_->verticalScrollBar();
        vbar->setValue(vbar->value() - (angle / 2));
        return true;
      }
      applyWheelZoom(angle > 0 ? kZoomInFactor : kZoomOutFactor, wheel->position().x());
      return true;
    }
    case QEvent::MouseButtonPress: {
      auto* mouse = static_cast<QMouseEvent*>(event);
      if (mouse->button() != Qt::LeftButton) {
        return false;
      }
      panning_ = true;
      pan_start_global_x_ = mouse->globalPosition().x();
      pan_start_scroll_value_ = view_->horizontalScrollBar()->value();
      return true;
    }
    case QEvent::MouseMove: {
      auto* mouse = static_cast<QMouseEvent*>(event);
      if (panning_ && (mouse->buttons() & Qt::LeftButton) != 0) {
        const double dx = mouse->globalPosition().x() - pan_start_global_x_;
        view_->horizontalScrollBar()->setValue(pan_start_scroll_value_ - static_cast<int>(std::llround(dx)));
        return true;
      }
      if (mouse->buttons() == Qt::NoButton) {
        showHoverTooltip(view_->mapToScene(mouse->position().toPoint()), mouse->globalPosition().toPoint());
      }
      return false;
    }
    case QEvent::MouseButtonRelease: {
      if (panning_) {
        panning_ = false;
        view_state_commit_debounce_.start();
        return true;
      }
      return false;
    }
    case QEvent::Leave:
      QToolTip::hideText();
      return false;
    default:
      return false;
  }
}

void StateTransitionsView::rebuild() {
  for (state_transitions_detail::StateRowItem* item : row_items_) {
    gscene_->removeItem(item);
    delete item;
  }
  row_items_.clear();

  const TimeSpan extent = fullExtent();
  viewport_.origin_ns = extent.min;

  const double vp_w = viewportWidthPx();
  // A chosen window (user gesture, restored zoom, linked range) wins over a fit
  // still pending from construction: a layout restores before the first show.
  if (fit_pending_ && !user_navigated_ && view_->isVisible() && vp_w > 0.0) {
    const qint64 span_ns = extent.max - extent.min;
    if (span_ns > 0) {
      viewport_.px_per_ns = std::clamp(vp_w / static_cast<double>(span_ns), 1e-12, 1e-1);
    }
    fit_pending_ = false;
  }

  const double extent_px = static_cast<double>(extent.max - extent.min) * viewport_.px_per_ns;
  const double scene_w = std::max(vp_w, extent_px);
  const double ruler_h = timeline_detail::TimelineRulerItem::kRulerHeight;
  const double scene_h = std::max(viewportHeightPx(), scene_.contentHeight() + ruler_h);
  gscene_->setSceneRect(0, 0, scene_w, scene_h);

  const TimelineRuler ruler = TimelineScene::ruler(viewport_, scene_w);
  const TimeSpan data_span = scene_.rows().empty() ? TimeSpan{.min = 0, .max = 0} : scene_.sceneExtent();
  background_item_->setLayout(scene_w, scene_h, viewport_, ruler, data_span.min, data_span.max);
  // Labels read the display-axis VALUE (epoch 0), matching the plot X axis; the
  // empty data span keeps the band untinted (the hatch already marks non-data).
  ruler_item_->setLayout(viewport_, scene_w, ruler, /*epoch_ns=*/0, /*data_min_ns=*/0, /*data_max_ns=*/0);

  const std::vector<StateRow>& rows = scene_.rows();
  row_items_.reserve(rows.size());
  for (std::size_t index = 0; index < rows.size(); ++index) {
    auto* item = new state_transitions_detail::StateRowItem(rows[index]);
    item->setLayout(viewport_, scene_w);
    item->setPos(0, StateTransitionsScene::rowTop(static_cast<int>(index)));
    gscene_->addItem(item);
    row_items_.push_back(item);
  }

  repositionPlayhead();
  syncStickyGeometry();
}

void StateTransitionsView::syncStickyGeometry() {
  const double vscroll = view_->verticalScrollBar()->value();
  const double hscroll = view_->horizontalScrollBar()->value();
  const double vp_h = viewportHeightPx();
  const double vp_w = viewportWidthPx();
  const double ruler_h = timeline_detail::TimelineRulerItem::kRulerHeight;
  // Ruler pinned to the viewport bottom; the needle line ends at its top edge.
  const double ruler_top = vscroll + vp_h - ruler_h;
  ruler_item_->setPos(0, ruler_top);
  playhead_item_->setHeight(vscroll + vp_h);
  playhead_item_->setHeaderTop(ruler_top);
  for (state_transitions_detail::StateRowItem* item : row_items_) {
    item->setSticky(hscroll, vp_w);
  }
}

void StateTransitionsView::repositionPlayhead() {
  playhead_item_->setPos(TimelineScene::nsToPx(playhead_ns_, viewport_), 0);
}

TimeSpan StateTransitionsView::fullExtent() const {
  TimeSpan extent = scene_.sceneExtent();
  if (range_max_ns_ > range_min_ns_) {
    extent.min = std::min(extent.min, range_min_ns_);
    extent.max = std::max(extent.max, range_max_ns_);
  }
  return extent;
}

double StateTransitionsView::viewportWidthPx() const {
  const QWidget* vp = view_->viewport();
  return (vp != nullptr && vp->width() > 0) ? static_cast<double>(vp->width()) : 100.0;
}

double StateTransitionsView::viewportHeightPx() const {
  const QWidget* vp = view_->viewport();
  return (vp != nullptr && vp->height() > 0) ? static_cast<double>(vp->height()) : 100.0;
}

std::pair<double, double> StateTransitionsView::currentVisibleRange() const {
  const double left_px = view_->horizontalScrollBar()->value();
  const qint64 left_ns = TimelineScene::pxToNs(left_px, viewport_);
  const qint64 right_ns = TimelineScene::pxToNs(left_px + viewportWidthPx(), viewport_);
  return {static_cast<double>(left_ns) / kNanosecondsPerSecond, static_cast<double>(right_ns) / kNanosecondsPerSecond};
}

void StateTransitionsView::emitVisibleRangeChanged() {
  const auto [t_min, t_max] = currentVisibleRange();
  emit visibleRangeChanged(t_min, t_max);
}

void StateTransitionsView::applyWheelZoom(double factor, double anchor_viewport_x) {
  const double anchor_scene_x = view_->horizontalScrollBar()->value() + anchor_viewport_x;
  const qint64 anchor_ns = TimelineScene::pxToNs(anchor_scene_x, viewport_);
  double next_zoom = TimelineScene::zoom(viewport_, factor, anchor_scene_x).px_per_ns;
  // Zoom-out floor: the visible window never grows past the display range, the maximum-zoom-out limit the plots honor,
  // so a wheel gesture here cannot walk the (linked) view outside the data. A degenerate range (single sample, or none
  // yet) floors at a 1 s window instead of being unclamped.
  const qint64 range_span_ns = std::max(range_max_ns_ - range_min_ns_, timeline_detail::kSecondNs);
  next_zoom = std::max(next_zoom, viewportWidthPx() / static_cast<double>(range_span_ns));
  // Same absolute bounds the setZoom/setVisibleRange appliers enforce.
  viewport_.px_per_ns = std::clamp(next_zoom, 1e-12, 1e-1);
  user_navigated_ = true;
  applying_external_ = true;  // scroll adjustments below are part of ONE gesture
  rebuild();
  const double new_anchor_scene_x = TimelineScene::nsToPx(anchor_ns, viewport_);
  view_->horizontalScrollBar()->setValue(static_cast<int>(std::llround(new_anchor_scene_x - anchor_viewport_x)));
  applying_external_ = false;
  emitVisibleRangeChanged();
  view_state_commit_debounce_.start();
}

QStringList StateTransitionsView::decodeCurveKeys(const QMimeData* mime) {
  QStringList keys;
  if (mime == nullptr || !mime->hasFormat(QLatin1StringView(kCurveDragMime))) {
    return keys;
  }
  QByteArray payload = mime->data(QLatin1StringView(kCurveDragMime));
  QDataStream stream(&payload, QIODevice::ReadOnly);
  while (!stream.atEnd()) {
    QString key;
    stream >> key;
    if (!key.isEmpty()) {
      keys.append(key);
    }
  }
  return keys;
}

bool StateTransitionsView::anyKeyDroppable(const QStringList& keys) const {
  if (!droppable_ || keys.isEmpty()) {
    return false;  // no predicate → a bare view refuses every drag
  }
  return std::ranges::any_of(keys, droppable_);
}

void StateTransitionsView::handleDrop(const QStringList& keys) {
  if (!keys.isEmpty()) {
    emit seriesDropped(keys);
  }
}

void StateTransitionsView::showHoverTooltip(const QPointF& scene_pos, const QPoint& global_pos) {
  const auto hit = scene_.hitTest(scene_pos.x(), scene_pos.y(), viewport_);
  if (!hit.has_value()) {
    QToolTip::hideText();
    return;
  }
  const StateSegment& segment =
      scene_.rows()[static_cast<std::size_t>(hit->row)].segments[static_cast<std::size_t>(hit->segment)];
  QToolTip::showText(global_pos, segmentTooltip(segment), this);
}

}  // namespace PJ
