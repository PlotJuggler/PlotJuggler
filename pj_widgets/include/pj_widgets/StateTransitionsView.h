#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <QtGlobal>
#include <functional>
#include <optional>
#include <vector>

#include "pj_widgets/Timeline.h"

class QGraphicsScene;
class QGraphicsView;
class QMimeData;

namespace PJ {

/// One contiguous run of an unchanged state value: the half-open interval [t_start_ns, t_end_ns) in display-frame
/// nanoseconds. The host maps raw store timestamps into the playback-display frame before feeding the view, so this
/// widget never sees per-dataset offsets.
struct StateSegment {
  qint64 t_start_ns = 0;
  qint64 t_end_ns = 0;
  QString value;  // the state string; also the color key (see StateColors.h)
};

/// One series as a stacked row: name chip, transition labels and the colored band built from `segments` (sorted by
/// t_start_ns, non-overlapping; gaps between consecutive segments render as empty band).
struct StateRow {
  quint64 id = 0;  // stable row identity, assigned by the host
  QString name;    // series name shown in the chip
  std::vector<StateSegment> segments;
};

/// Display-free math and model core of the State Transitions strip: holds the rows and answers the geometry and hit
/// questions the view paints from. QtCore types only (QString state values are intrinsic), never QtWidgets or QtGui, so
/// every method is testable headless. Pixel mapping delegates to the public TimelineScene statics: the strip and the
/// Source Timeline share one TimelineViewport convention.
class StateTransitionsScene {
 public:
  /// Vertical layout of one row, top to bottom: name chip, the color band (state names paint inside their segments),
  /// then `gap` to the next row. All px, compact so many series stack in a short dock (the name line fits the 13 px
  /// DemiBold name, the band the 11 px value labels).
  struct RowMetrics {
    double chip_h = 17.0;
    double band_h = 22.0;
    double gap = 6.0;
  };
  /// Padding above the first row's chip (px).
  static constexpr double kTopPad = 4.0;

  [[nodiscard]] static constexpr RowMetrics rowMetrics() noexcept {
    return {};
  }
  /// Full height of one row including its trailing gap.
  [[nodiscard]] static constexpr double rowHeight() noexcept {
    constexpr RowMetrics kMetrics;
    return kMetrics.chip_h + kMetrics.band_h + kMetrics.gap;
  }
  /// Scene-y of a row's top (its chip line).
  [[nodiscard]] static constexpr double rowTop(int index) noexcept {
    return kTopPad + (rowHeight() * index);
  }
  /// Scene-y of a row's color band top.
  [[nodiscard]] static constexpr double bandTop(int index) noexcept {
    constexpr RowMetrics kMetrics;
    return rowTop(index) + kMetrics.chip_h;
  }

  void setRows(std::vector<StateRow> rows);
  [[nodiscard]] const std::vector<StateRow>& rows() const noexcept {
    return rows_;
  }
  /// Replaces the row with the same id in place (order preserved). False if the id is unknown: the caller decides
  /// whether that means setRows.
  bool updateRow(StateRow row);
  bool removeRow(quint64 id);
  /// Index of the row with this id in rows(), or -1.
  [[nodiscard]] int rowIndexOf(quint64 id) const noexcept;

  /// Total content height (rows and top pad) in px; kTopPad when empty.
  [[nodiscard]] double contentHeight() const noexcept;

  /// Union of every row's covered span [first segment start, last segment end]; the default extent when no row has
  /// segments.
  [[nodiscard]] TimeSpan sceneExtent() const noexcept;
  /// The span sceneExtent() reports with no data. The host sets it to the playback range, so an empty strip is as long
  /// as the playback. No-op if max <= min.
  void setDefaultExtent(qint64 min_ns, qint64 max_ns) noexcept;

  /// A hovered band segment: indices into rows() and segments.
  struct HoverHit {
    int row = -1;
    int segment = -1;
  };
  /// The segment whose color band contains the scene point (x_px, y_px), or nullopt. Only the band strip is hit, not
  /// chips or labels. Time containment is half-open: the instant t_end_ns belongs to the next segment.
  [[nodiscard]] std::optional<HoverHit> hitTest(double x_px, double y_px, const TimelineViewport& viewport) const;

 private:
  std::vector<StateRow> rows_;
  TimeSpan default_extent_{.min = 0, .max = 60'000'000'000};  // sceneExtent() when empty (see setDefaultExtent)
};

class Scrollbar;

namespace timeline_detail {
class TimelineRulerItem;
class TimelineNeedleItem;
class TimelineBackgroundItem;
}  // namespace timeline_detail

namespace state_transitions_detail {
class StateRowItem;
}  // namespace state_transitions_detail

/// State Transitions strip: each discrete series is a stacked row (a name-chip label pinned to the viewport's left edge
/// above a color band of contiguous-state segments, each carrying its state name inside the rectangle) over one shared
/// bottom time ruler with a playback needle. Runtime-agnostic like `Timeline`: the host feeds display-frame rows,
/// playhead and range through slots and reacts to intent signals; the view never mutates host state.
///
/// The whole widget lives in the playback-display frame (the frame IDataWidget::onTrackerTime and the plots' linked
/// zoom speak), so there is no internal time-frame offset. Rendering reuses the shared timeline items (ruler, needle
/// and background in bottom-ruler mode) and TimelineScene's TimelineViewport ns/px mapping.
///
/// Navigation: the wheel zooms horizontally (cursor-anchored), Shift+wheel (or the right-strip scroll pill) scrolls
/// rows vertically, and a left-drag on the background pans. User zoom and pan emit visibleRangeChanged at once (the
/// linked-zoom feed); applying setVisibleRange from outside never re-emits. The ruler and needle stay pinned to the
/// viewport bottom while rows scroll.
///
/// Drops: accepts the curve-list drag ("curveslist/add_curve") when the host-injected droppable predicate passes for at
/// least one dragged key, and emits all dragged keys through seriesDropped; the host filters again and adds rows.
/// Without a predicate every drag is refused (a bare view is inert).
class StateTransitionsView : public QWidget {
  Q_OBJECT
 public:
  explicit StateTransitionsView(QWidget* parent = nullptr);
  ~StateTransitionsView() override;

  /// Host-injected gate deciding whether a dragged catalog key can become a row (the host checks "is this a discrete
  /// series"). Called on drag-enter and drag-move, so it must be cheap.
  using DroppablePredicate = std::function<bool(const QString&)>;
  void setDroppablePredicate(DroppablePredicate predicate);

  // --- view-state accessors (layout persistence) ---
  /// Current zoom in pixels per nanosecond.
  [[nodiscard]] double zoom() const noexcept;
  /// Display-ns at the left edge of the visible viewport (the pan position in data terms, so it survives zoom changes
  /// and window resizes).
  [[nodiscard]] qint64 viewportLeftDisplayNs() const;
  /// Vertical row-scroll position in pixels.
  [[nodiscard]] int viewportTopOffsetPx() const;
  /// Union of the rows' covered display span in seconds: the strip's contribution to a linked "zoom out" union; nullopt
  /// while no row has data.
  [[nodiscard]] std::optional<std::pair<double, double>> dataExtentSeconds() const;

  // --- test seams: the exact production code paths, no synthetic OS events ---
  [[nodiscard]] int rowCountForTest() const;
  /// The model row at this index (display-frame segments as fed by the host).
  [[nodiscard]] StateRow rowForTest(int row_index) const;
  [[nodiscard]] qint64 playheadNsForTest() const noexcept;
  /// The label-visibility flags the row's paint pass would use now.
  [[nodiscard]] std::vector<bool> visibleLabelsForTest(int row_index) const;
  /// Runs the drop path with these keys, as if a curve drag landed.
  void dropKeysForTest(const QStringList& catalog_keys);
  /// Whether a drag carrying these keys would be accepted by dragEnter.
  [[nodiscard]] bool wouldAcceptDropForTest(const QStringList& catalog_keys) const;
  /// A cursor-anchored zoom step through the production wheel path.
  void wheelZoomForTest(double factor, double anchor_viewport_x);
  /// Viewport-y of the ruler band's top: viewport_height minus ruler height whatever the vertical scroll (the
  /// sticky-bottom contract).
  [[nodiscard]] double rulerViewportYForTest() const;
  [[nodiscard]] int verticalScrollForTest() const;
  void setVerticalScrollForTest(int value);

 public slots:
  /// Replaces all rows and rebuilds. Fits the extent on the first non-empty set unless the user has navigated.
  void setRows(const std::vector<StateRow>& rows);
  /// Refreshes one row's data in place (no-op if the id is unknown).
  void updateRow(const StateRow& row);
  /// Removes one row (view-side only; the host's curves panel drives removal).
  void removeRow(quint64 id);
  /// Moves the playback needle (display-axis seconds, the onTrackerTime frame).
  void setPlayhead(double display_seconds);
  /// The authoritative playback range (display seconds): the ruler and scroll bounds, and the right edge trailing
  /// segments extend to.
  void setDisplayRange(double lo_seconds, double hi_seconds);
  /// Applies an externally driven visible time window (linked zoom); never re-emits visibleRangeChanged.
  void setVisibleRange(double t_min_seconds, double t_max_seconds);
  /// Restores zoom (px per ns; clamped); call before setViewportLeftDisplayNs.
  void setZoom(double px_per_ns);
  /// Scrolls so `display_ns` sits at the viewport's left edge.
  void setViewportLeftDisplayNs(qint64 display_ns);
  /// Restores the vertical row scroll (px).
  void setViewportTopOffsetPx(int offset_px);

 signals:
  /// A curve-list drag dropped here; `catalog_keys` is every dragged key (the host filters to discrete series and adds
  /// rows).
  void seriesDropped(const QStringList& catalog_keys);
  /// The visible time window changed through a user gesture (wheel zoom or background pan): the linked-zoom feed, in
  /// display-axis seconds.
  void visibleRangeChanged(double t_min_seconds, double t_max_seconds);
  /// A user gesture finished changing view chrome (debounced): layout-dirty.
  void viewStateChangeCommitted();

 protected:
  void resizeEvent(QResizeEvent* event) override;
  void changeEvent(QEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dragMoveEvent(QDragMoveEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  /// The QGraphicsView viewport consumes mouse and wheel events; they are filtered and re-dispatched, so zoom, pan,
  /// hover and chip-close live in one place.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void rebuild();             // re-lay-out rows/ruler/background for viewport_
  void syncStickyGeometry();  // pin ruler+needle to the viewport bottom, chips to the left
  void repositionPlayhead();
  [[nodiscard]] TimeSpan fullExtent() const;  // data ∪ display range
  [[nodiscard]] double viewportWidthPx() const;
  [[nodiscard]] double viewportHeightPx() const;
  /// The current visible window in display seconds (for the linked-zoom emit).
  [[nodiscard]] std::pair<double, double> currentVisibleRange() const;
  void emitVisibleRangeChanged();
  void applyWheelZoom(double factor, double anchor_viewport_x);
  /// Decodes the curve-drag payload ("curveslist/add_curve"): a QDataStream of QStrings; empty when the format is
  /// absent.
  [[nodiscard]] static QStringList decodeCurveKeys(const QMimeData* mime);
  [[nodiscard]] bool anyKeyDroppable(const QStringList& keys) const;
  void handleDrop(const QStringList& keys);
  void showHoverTooltip(const QPointF& scene_pos, const QPoint& global_pos);

  StateTransitionsScene scene_;  // display-free model/math
  TimelineViewport viewport_;
  DroppablePredicate droppable_;

  QGraphicsScene* gscene_ = nullptr;
  QGraphicsView* view_ = nullptr;
  timeline_detail::TimelineBackgroundItem* background_item_ = nullptr;
  timeline_detail::TimelineRulerItem* ruler_item_ = nullptr;
  timeline_detail::TimelineNeedleItem* playhead_item_ = nullptr;
  std::vector<state_transitions_detail::StateRowItem*> row_items_;  // index-aligned with scene_.rows()

  qint64 playhead_ns_ = 0;   // display-frame needle position
  qint64 range_min_ns_ = 0;  // authoritative playback range (setDisplayRange)
  qint64 range_max_ns_ = 0;
  bool user_navigated_ = false;     // a user zoom/pan happened; stop auto-fitting
  bool fit_pending_ = true;         // fit on the next rebuild (until user_navigated_)
  bool applying_external_ = false;  // guard: external range apply must not re-emit

  // --- background left-drag pan state ---
  bool panning_ = false;
  double pan_start_global_x_ = 0.0;
  int pan_start_scroll_value_ = 0;

  // Coalesces per-event view-chrome changes into one viewStateChangeCommitted.
  QTimer view_state_commit_debounce_;

  // The vertical rows pill only: the time axis has no scroller (zoom, pan and linked zoom own horizontal navigation;
  // the native horizontal scrollbar stays hidden but live as the pan state).
  Scrollbar* v_scrollbar_ = nullptr;
};

}  // namespace PJ
