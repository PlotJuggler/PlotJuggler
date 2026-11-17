#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QList>
#include <QPoint>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <QtGlobal>
#include <set>
#include <utility>
#include <vector>

#include "pj_widgets/TimeFormat.h"

class QLabel;
class QPushButton;
class QToolButton;
class QGraphicsLineItem;
class QSplitter;

namespace PJ {

/// Stable per-source identity; one Timeline bar per source. A plain type, so pj_widgets stays free of any PJ-module
/// (`pj_base`) dependency: the host converts its DatasetId to and from this.
using TimelineSourceId = quint64;

/// One source rendered as a bar: its raw absolute time span (ns) plus the display shift the user applied (display_time
/// = raw_time - offset). The widget's public input, so Qt types (QString, QColor) are fine; the Qt-free math class
/// below sees only the numeric fields.
struct TimelineTrack {
  TimelineSourceId id = 0;
  QString name;
  qint64 t_min_ns = 0;       // raw absolute start (ns)
  qint64 t_max_ns = 0;       // raw absolute end (ns)
  qint64 offset_ns = 0;      // display = raw − offset
  QColor color = Qt::white;  // bar fill color
};

/// A pixel span (no Qt types); row y and height are the widget's concern.
struct PxSpan {
  double x = 0.0;
  double width = 0.0;
};

/// A closed time interval [min,max] in ns, so the math class carries no PJ-module type.
struct TimeSpan {
  qint64 min = 0;
  qint64 max = 0;
};

/// ns to pixel mapping: px = (display_ns - origin_ns) * px_per_ns. Computing relative to origin_ns keeps magnitudes
/// small, so the int64 to double cast stays exact even at epoch-scale timestamps (~1.7e18 ns).
struct TimelineViewport {
  double px_per_ns = 1e-7;  // pixels per nanosecond (zoom)
  qint64 origin_ns = 0;     // display-ns at scene x = 0
};

/// Ruler tick layout for a viewport: the chosen interval and the tick positions.
struct TimelineRuler {
  qint64 interval_ns = 0;
  std::vector<qint64> ticks_ns;
};

/// The numeric core of one track the math class operates on: id, raw span and offset only. Distinct from TimelineTrack,
/// so TimelineScene never sees a QString or QColor and stays Qt-free.
struct TimelineSpanInput {
  TimelineSourceId id = 0;
  qint64 t_min_ns = 0;
  qint64 t_max_ns = 0;
  qint64 offset_ns = 0;
};

/// Pure, Qt-free timing, geometry and alignment engine of the Source Timeline. Holds the numeric track list; the rest
/// is stateless math the widget renders. No Q* type appears in a method signature.
class TimelineScene {
 public:
  void setTracks(std::vector<TimelineSpanInput> tracks);
  [[nodiscard]] const std::vector<TimelineSpanInput>& tracks() const noexcept {
    return tracks_;
  }

  // --- stateless timing positions and differences (no track state needed) ---

  /// Displayed window of a track: [t_min - offset, t_max - offset].
  [[nodiscard]] static TimeSpan displayWindow(const TimelineSpanInput& track) noexcept;

  /// display-ns to pixel x under a viewport.
  [[nodiscard]] static double nsToPx(qint64 display_ns, const TimelineViewport& vp) noexcept;
  /// Pixel x to display-ns under a viewport (the inverse of nsToPx, rounded to the nearest ns).
  [[nodiscard]] static qint64 pxToNs(double x_px, const TimelineViewport& vp) noexcept;

  /// Pixel bar geometry of a track under a viewport (true width; the widget clamps to a minimum for visibility).
  [[nodiscard]] static PxSpan barSpan(const TimelineSpanInput& track, const TimelineViewport& vp) noexcept;

  /// Drag delta in pixels to ns (a pure difference, independent of origin).
  [[nodiscard]] static qint64 pxDeltaToNs(double dx_px, const TimelineViewport& vp) noexcept;

  /// Cursor-anchored zoom: scales px_per_ns by `factor` (clamped), keeping the ns under anchor_x_px fixed on screen.
  [[nodiscard]] static TimelineViewport zoom(const TimelineViewport& vp, double factor, double anchor_x_px) noexcept;

  /// Ruler ticks across the visible width, aiming at ~target_px_per_tick spacing.
  [[nodiscard]] static TimelineRuler ruler(
      const TimelineViewport& vp, double width_px, double target_px_per_tick = 80.0);

  // --- stateful (use the track list) ---

  /// Union of all tracks' displayed windows; the default extent (setDefaultExtent, 60 s initially) when there are no
  /// tracks.
  [[nodiscard]] TimeSpan sceneExtent() const noexcept;

  /// The span sceneExtent() reports with no tracks. The host sets it to the playback range, so a data-less timeline is
  /// as long as the playback. No-op if max <= min.
  void setDefaultExtent(qint64 min_ns, qint64 max_ns) noexcept;

  /// New offset per track so every displayed start lands on the global earliest displayed start (the leftmost source is
  /// unmoved): new_offset = t_min - target. Idempotent. {id, new_offset} pairs; empty if there are no tracks.
  [[nodiscard]] std::vector<std::pair<TimelineSourceId, qint64>> alignStartsToCommonOrigin() const;

  /// New offset per track so every displayed center lands on the global earliest displayed center (the source with the
  /// leftmost center is unmoved). The center is min + (max - min) / 2, to avoid overflow on large timestamps.
  /// Idempotent. {id, new_offset} pairs; empty if there are no tracks.
  [[nodiscard]] std::vector<std::pair<TimelineSourceId, qint64>> alignCentersToCommonOrigin() const;

  /// New offset per track so every displayed end lands on the global latest displayed end (the source with the
  /// rightmost end is unmoved): new_offset = t_max - target. Idempotent. {id, new_offset} pairs; empty if there are no
  /// tracks.
  [[nodiscard]] std::vector<std::pair<TimelineSourceId, qint64>> alignEndsToCommonOrigin() const;

  /// Result of an edge-snap search (see snapToEdges).
  struct EdgeSnap {
    bool snapped = false;  // true if a candidate landed within threshold
    qint64 delta_ns = 0;   // the (snapped, or pass-through raw) common shift to apply
    qint64 edge_ns = 0;    // the aligned candidate edge (display-ns), for the guide line
  };
  /// Edge snap for a drag: among every pairing of a dragged edge (plus raw_delta) with a candidate edge, picks the
  /// alignment whose required delta is nearest raw_delta_ns and within threshold_ns. `dragged_edges` and
  /// `candidate_edges` are drag-start display positions (ns). {snapped=false, delta=raw_delta} when nothing is close
  /// enough, so dragging past the threshold releases the snap.
  [[nodiscard]] static EdgeSnap snapToEdges(
      const std::vector<qint64>& dragged_edges, const std::vector<qint64>& candidate_edges, qint64 raw_delta_ns,
      qint64 threshold_ns);

 private:
  std::vector<TimelineSpanInput> tracks_;
  TimeSpan default_extent_{0, 60'000'000'000};  // sceneExtent() when empty; see setDefaultExtent
};

class Scrollbar;

namespace timeline_detail {
class TimelineBarItem;
class TimelineRulerItem;
class TimelineNeedleItem;
class TimelineBackgroundItem;
class TimelineNamePanel;
}  // namespace timeline_detail

/// Reusable, runtime-agnostic multi-track timeline view. It knows nothing about pj_runtime: the host feeds it tracks,
/// playhead and range through slots and reacts to its intent signals. All timing and geometry math is TimelineScene's.
///
/// Each loaded source is a horizontal bar; dragging a bar adjusts only that source's display offset (display_time =
/// raw_time - offset). The Align button snaps all displayed starts to the global earliest displayed start.
///
/// The view never mutates its track offsets directly: a bar drag, align or seek emits an intent signal, the host writes
/// the authoritative state and feeds it back through setTracks / setPlayhead. (During a bar drag the bar is previewed
/// as a "ghost" offset for responsiveness, cleared on release.)
///
/// Edge snap (setSnapEnabled): while dragging, a bar's start or end snaps onto a neighbour's within a pixel threshold,
/// an orange guide line marks the alignment, and dragging past the threshold releases it (see snapToEdges).
///
/// Layout: a left column lists each track's dataset name (pinned, so the name stays visible when its bar scrolls off or
/// squashes thin; each name row aligns in height with its bar). Its right edge is a draggable splitter that resizes the
/// column. Dragging a name row reorders the tracks (tracksReordered; reordered optimistically). Name rows are also
/// multi-selectable (click selects one, Ctrl or Meta+click toggles, Shift+click extends a range), highlighting the row
/// and its bar; with 2 or more selected a "Merge selected datasets:" prompt with a merge button appears below the
/// column and emits mergeRequested. The scrollable view sits to its right. PJ::Scrollbar overlay pills (h_scrollbar_,
/// v_scrollbar_) replace the native scroll bars: they fade in on hover over their edge strip and scroll the view when
/// dragged.
///
/// Navigation: the mouse wheel zooms horizontally, anchored at the cursor; a left-drag on empty background pans (a
/// plain background click with no drag deselects). The scene keeps a +-1 min buffer around the data so small drags near
/// the edges do not resize it; it grows only when a bar is pushed past the buffer.
///
/// Two draggable marker needles share one item type: the pink playback playhead (setPlayhead / playheadSeeked) and the
/// blue reference line (setReferenceLine / referenceLineMoved, shown only when the host toggles it on). Each darkens
/// and shows a timestamp pill, aligned to the ruler numbers, while grabbed.
class Timeline : public QWidget {
  Q_OBJECT
 public:
  explicit Timeline(QWidget* parent = nullptr);
  ~Timeline() override;

  /// Number of bar items in the scene (test and inspection aid).
  [[nodiscard]] int barCount() const;

  // --- view-state accessors (for layout persistence) ---
  // The host reads these to save the timeline's view into a layout and writes them back through the matching slots on
  // reload. All are view chrome, independent of the per-source offsets and order, which round-trip separately.

  /// Current zoom in pixels per nanosecond (Ctrl+wheel adjusts it).
  [[nodiscard]] double zoom() const noexcept;
  /// Display-ns at the left edge of the visible viewport: the horizontal pan in data terms, so it survives a zoom
  /// change and re-maps correctly when the same data reloads. 0 when there is no data.
  [[nodiscard]] qint64 viewportLeftDisplayNs() const;
  /// Vertical row-scroll position in pixels.
  [[nodiscard]] int viewportTopOffsetPx() const;
  /// Current width (px) of the left name column (the splitter's first pane).
  [[nodiscard]] int nameColumnWidth() const;

  /// Whether auto-zoom is on: loading new data (or an explicit fitToContents()) zooms the view so the largest extent
  /// (the union of all bars) fills it. The empty timeline always fits the playback range.
  [[nodiscard]] bool isAutoZoomEnabled() const noexcept;

  /// Test seam: runs the delta-to-offset path a horizontal bar drag uses and emits offsetChangeRequested, without
  /// synthesizing mouse events.
  void applyBarDragForTest(TimelineSourceId id, double dx_px);

  /// Test seams: run the needle-drag path (scene-x, clamp, emit) the playhead and reference-line drags use, so the
  /// data-extent clamp is testable without synthetic mouse events. Returns the needle position (display seconds) after
  /// clamping.
  [[nodiscard]] double seekToSceneXForTest(double scene_x);
  [[nodiscard]] double moveReferenceToSceneXForTest(double scene_x);

  /// The current data extent (bar union, or the empty-state span) in display ns: what the needle clamps to. Lets a test
  /// verify the bars track data growth.
  [[nodiscard]] TimeSpan sceneExtentForTest() const;

  /// The playhead's internal (bar-frame) position in ns: pins that a playback-frame setPlayhead lands bit-exact under a
  /// large time-frame offset.
  [[nodiscard]] qint64 playheadNsForTest() const;

  /// The marker-pill label for a bar-frame ns position, per the current label mode: pins the absolute formatter's
  /// millisecond rounding (vs playback).
  [[nodiscard]] QString markerLabelForTest(qint64 ns) const;

  /// Test seams: synthesise viewport mouse events and route them through the full event-filter chain (Scrollbar overlay
  /// first, then the Timeline handler), so the scroll-pill and interaction logic is exercised without flaky synthetic
  /// OS events. `button_down` marks a drag move.
  void pressViewportForTest(QPoint viewport_pos);
  void moveViewportForTest(QPoint viewport_pos, bool button_down);
  void releaseViewportForTest(QPoint viewport_pos);
  /// Scroll-pill inspection seams: query the attached PJ::Scrollbar overlay.
  [[nodiscard]] bool isScrollPillShownForTest() const;
  [[nodiscard]] int scrollValueForTest() const;
  [[nodiscard]] bool horizontalScrollBarVisibleForTest() const;
  [[nodiscard]] int viewportHeightForTest() const;
  [[nodiscard]] int viewportWidthForTest() const;
  /// Vertical scroll-pill and sticky-ruler seams.
  [[nodiscard]] bool isVScrollPillShownForTest() const;
  [[nodiscard]] bool verticalScrollBarVisibleForTest() const;
  [[nodiscard]] int verticalScrollValueForTest() const;
  void setVerticalScrollForTest(int value);
  /// Scene-y of the ruler item: equals the vertical scroll offset when the ruler is pinned to the viewport top.
  [[nodiscard]] double rulerSceneYForTest() const;
  /// Name-column alignment seams: the per-row top y of the name panel vs. the bar, which must match.
  [[nodiscard]] int nameRowCountForTest() const;
  [[nodiscard]] double nameRowTopForTest(int index) const;
  [[nodiscard]] double barRowTopForTest(int index) const;
  /// Reorder seams: the current top-to-bottom track id order, and a direct call into the drag-reorder path (move `from`
  /// to insertion `drop_index`).
  [[nodiscard]] QList<quint64> trackOrderForTest() const;
  void reorderForTest(int from, int drop_index);
  /// Selection seams: drive the name-row click path (row index into the displayed order, with modifiers) and inspect
  /// the resulting state (whether the merge prompt is logically shown, which selected ids are filter-visible) without
  /// synthesizing mouse events or showing the widget.
  void selectNameRowForTest(int row, Qt::KeyboardModifiers mods);
  [[nodiscard]] bool mergeButtonEnabledForTest() const;
  [[nodiscard]] QList<quint64> visibleSelectedIdsForTest() const;
  /// Snap seams: run the bar-drag snap path for `id` by `dx_px` (leaving the guide line in its resulting state) and
  /// inspect whether the guide line is showing.
  void dragBarForSnapTest(TimelineSourceId id, double dx_px);
  [[nodiscard]] bool snapLineVisibleForTest() const;
  /// Runs a sequence of moves within one continuous drag (no reset between them) and returns whether each ended
  /// snapped: exercises the sticky hysteresis.
  [[nodiscard]] std::vector<bool> dragSnapSequenceForTest(TimelineSourceId id, const std::vector<double>& dxs);

 public slots:
  /// Replaces the displayed sources and rebuilds bars, ruler and extent.
  void setTracks(const std::vector<TimelineTrack>& tracks);
  /// Shows only the tracks whose dataset name contains `text` (case-insensitive; empty shows all). Driven by the name
  /// column's "Datasets" filter; a filter change re-selects which bars show but never resets the view (no auto-zoom).
  void setDatasetFilter(const QString& text);
  /// Moves the playhead to a display-seconds position.
  void setPlayhead(double display_seconds);
  /// Shows or moves (or hides) the blue reference line at a display-seconds position. Driven by the host's "reference
  /// point" toggle; draggable while shown.
  void setReferenceLine(double display_seconds, bool visible);
  /// Sets the ruler and scroll bounds (display seconds), kept in sync with playback.
  void setDisplayRange(double lo_seconds, double hi_seconds);
  /// The ns gap between the host's external (playback) frame, in which the playhead, range and reference are supplied
  /// to the double setters and emitted by the seek signals, and the bars' internal frame (`setTracks` feeds that frame
  /// directly). The setters add this offset in integer ns and the seeks subtract it, so the needle stays bit-exact with
  /// the playback instant: the same shift in epoch-scale double seconds would round to ~240 ns and desync the readouts.
  /// The host sets it to its global display reference (0 when "use time offset" is off); when it changes (a load or
  /// toggle moving the global reference) the existing playhead and reference are re-expressed in the new frame, so the
  /// needle never strands at the old position even if the host does not re-push an unchanged current time.
  void setTimeFrameOffsetNs(qint64 offset_ns);
  /// Chooses how ruler ticks and marker time-pills are labelled (positions are unaffected): false (default) is elapsed
  /// time relative to the ruler epoch (0:00, 0:10, 1.5s); true is the absolute Unix timestamp in seconds, read from the
  /// value as epoch ns (matching the plot axis in absolute mode). The host drives it from its "use time offset" toggle.
  /// Pure formatting: bars and needles never move.
  void setAbsoluteTimeLabels(bool absolute);
  /// How absolute labels (setAbsoluteTimeLabels) render: kLocalClock and kUtcClock show the wall-clock time of day, any
  /// other value keeps the Unix timestamp. Elapsed labels ignore it. Pure formatting.
  void setAbsoluteClockFormat(TimeLabelFormat format);
  /// Pins the left name column to an exact width (px) and makes it the column's minimum, so the separator can be
  /// aligned pixel-perfect under an external reference (the host aligns it to the playback bar's track start). The user
  /// can drag the separator wider, never narrower.
  void setNameColumnWidth(int width_px);
  /// Sets only the name column's current width (px), leaving the minimum pinned by setNameColumnWidth intact, so the
  /// host can restore a remembered width while the playback-aligned floor holds (the user can drag back down to it).
  /// Clamped to the splitter's minimum.
  void resizeNameColumn(int width_px);
  /// Enables or disables edge snapping while dragging a bar: a dragged bar's start or end snaps to a neighbouring bar's
  /// within a pixel threshold (a guide line marks it); dragging further releases the snap.
  void setSnapEnabled(bool enabled);
  [[nodiscard]] bool snapEnabled() const;
  /// Puts the timeline into a read-only "frozen" state. Locked, the view is inert: every manipulation gesture is
  /// suppressed (bar-offset drags, name-row reorder and selection, the Align button, the merge prompt), as are needle
  /// seeking (dragging the playhead or reference line) and view navigation (wheel scroll, Ctrl+wheel zoom, the scroll
  /// pills). Only host-driven slave updates stay live: the needles keep tracking playback through setPlayhead, and new
  /// data still rebuilds and auto-fits the view. The host locks it while a data source is live-streaming and playback
  /// is playing: the view is pinned to the growing live edge, so scrolling, editing or seeking it is meaningless;
  /// pausing unlocks it so the retained window can be scrubbed and realigned. Locking mid-gesture cancels any in-flight
  /// drag. Idempotent.
  void setInteractionLocked(bool locked);
  /// Restores the zoom (pixels per ns); clamped to the widget's zoom limits and followed by a rebuild. Call before
  /// setViewportLeftDisplayNs on restore, so the pan maps under the intended zoom.
  void setZoom(double px_per_ns);
  /// Scrolls so `display_ns` sits at the visible viewport's left edge (clamped to the scrollable range). Apply after
  /// setZoom and after the tracks are loaded.
  void setViewportLeftDisplayNs(qint64 display_ns);
  /// Restores vertical row scrolling after the track order is settled.
  void setViewportTopOffsetPx(int offset_px);
  /// Enables or disables auto-zoom (default on): the view re-fits the largest extent on new data and on
  /// fitToContents(); enabling it re-fits at once.
  void setAutoZoomEnabled(bool enabled);
  /// Zooms and scrolls so the current largest extent (the union of all bars) fills the view, earliest content at the
  /// left edge. No-op when auto-zoom is off or there is no data. The host calls it after an alignment, to re-frame the
  /// realigned extent.
  void fitToContents();
  /// Explicit one-shot "zoom out horizontally": fits the largest extent into the view whatever the auto-zoom
  /// preference. Wired to the align rail button.
  void zoomToFit();

 signals:
  /// Live during a bar drag: the new absolute display offset (ns) for a source.
  void offsetChangeRequested(quint64 id, qint64 offset_ns);
  /// A bar-offset drag ended after changing at least one source.
  void offsetChangeCommitted();
  /// The Align button was pressed.
  void alignRequested();
  /// The user dragged the playhead or clicked the ruler to a new display-seconds.
  void playheadSeeked(double display_seconds);
  /// The user dragged the reference line to a new display-seconds position.
  void referenceLineMoved(double display_seconds);
  /// The merge-prompt button (shown below the name column with 2 or more sources selected) was clicked. `ids` are the
  /// selected source ids; the widget stays agnostic: the host (pj_app's SourceTimelineController) confirms and performs
  /// the destructive dataset merge through the runtime.
  void mergeRequested(const QList<quint64>& ids);
  /// The user drag-reordered the track rows in the name column. `ordered_ids` is the new top-to-bottom order; the host
  /// adopts it (so it survives later setTracks rebuilds). The widget also reorders optimistically.
  void tracksReordered(const QList<quint64>& ordered_ids);
  /// The user dragged the name-column separator to a new width (px). The host remembers it so the column keeps that
  /// width across rebuilds and panel toggles (it is otherwise re-pinned to the playback-aligned floor), and persists
  /// it.
  void nameColumnWidthChanged(int width_px);
  /// A user zoom, pan or scroll gesture ended with changed view chrome.
  void viewStateChangeCommitted();

 protected:
  void wheelEvent(QWheelEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  /// Repaints with the new theme colours when the application palette or style rolls (the items read
  /// QGuiApplication::palette() each paint; this nudges them).
  void changeEvent(QEvent* event) override;
  /// QGraphicsView consumes mouse and wheel events internally, so its viewport is filtered and re-dispatched to this
  /// widget's handlers (one place for drag, seek and zoom). True swallows the events handled.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  /// Feeds only the filter-matching tracks (from all_tracks_) into the scene, then rebuilds. Bars, names, extent and
  /// reorder all run on this filtered subset, so nothing else needs filter awareness.
  void applyDatasetFilter();
  void rebuild();  // re-lay-out bars + ruler from scene_ + viewport_
  /// Refreshes ruler ticks and labels for the current viewport. `ruler` is the tick layout rebuild() computed once and
  /// also handed to the background gridlines, so the numbers and the gridlines share one tick list. `data` is the
  /// data-covered span (union of all bars, empty with no tracks), passed from rebuild() so it is not re-derived.
  void rebuildRuler(const TimelineRuler& ruler, const TimeSpan& data);
  void repositionPlayhead();                 // move the playhead line to playhead_ns_
  [[nodiscard]] double sceneHeight() const;  // total scene height for the current track count

  /// Seeks the playhead from a viewport scene-x: clamps the requested time to the data extent and emits playheadSeeked.
  /// The needle is a pure slave: it does not move itself here. The host writes the time to the PlaybackEngine and the
  /// currentTimeChanged echo (setPlayhead) is the one source that repositions it, keeping it in lockstep with the
  /// playback handle. Returns the clamped display-seconds it emitted (for the test seam).
  double seekToSceneX(double scene_x);
  /// Moves the reference line to a scene-x and emits referenceLineMoved (drag path).
  void moveReferenceToSceneX(double scene_x);
  /// Moves the reference needle to reference_ns_ for the current viewport.
  void repositionReference();
  /// Label of a marker pill (playhead or reference) at a display-ns position: absolute (absolute_clock_format_) when
  /// absolute_time_labels_, else elapsed time relative to ruler_epoch_ns_. Both needle labels go through it.
  [[nodiscard]] QString markerLabel(qint64 display_ns) const;
  /// The scene track with this id, or nullptr: the lookup the drag paths share.
  [[nodiscard]] const TimelineSpanInput* trackById(TimelineSourceId id) const;
  /// The track's display offset (ns), or 0 if it is not loaded.
  [[nodiscard]] qint64 baseOffsetOf(TimelineSourceId id) const;
  /// New offset for `id` given a horizontal pixel delta from drag start.
  [[nodiscard]] qint64 offsetForBarDelta(TimelineSourceId id, double dx_px) const;
  /// Seeds a single-bar drag group for the synthetic-drag test seams: clears the group and sticky-snap state, then
  /// pushes the bar item for `id`. False (group left empty) if no such bar exists. The real drag is built inline in
  /// mousePressEvent because it carries the whole multi-selection.
  bool beginSyntheticDrag(TimelineSourceId id);

  /// Edge-snap result for an in-flight bar drag. `delta_ns` is the (possibly snapped) common shift; when `snapped`,
  /// `line_display_ns` is the display-ns of the aligned neighbour edge the guide line marks.
  struct DragSnap {
    bool snapped = false;
    qint64 delta_ns = 0;
    qint64 line_display_ns = 0;
  };
  /// Given the raw drag delta, finds the nearest neighbour-edge alignment. Dragged edges come from the drag-start
  /// offsets (drag_group_ base offsets), not scene_, which the host may have updated mid-drag (reading it would
  /// double-count). Sticky: once snapped, the snap holds (no re-catch) until the cursor leaves a wider release band, so
  /// it does not flicker between candidates. Not const: it updates the sticky state.
  [[nodiscard]] DragSnap computeDragSnap(qint64 raw_delta_ns);
  /// Shows or hides the vertical alignment guide line at a display-ns position.
  void showSnapLine(qint64 display_ns);
  void hideSnapLine();
  /// Applies a name-row click to the selection per the held modifiers (Ctrl or Meta toggles, Shift extends from the
  /// anchor, a plain click selects one), then refreshes the row and bar highlight and the merge prompt. `row` is the
  /// index into the displayed track order; a negative row clears the selection.
  void selectNameRow(int row, Qt::KeyboardModifiers mods);
  /// Clears the selection and refreshes the highlight and merge button.
  void clearSelection();
  /// Re-applies the selected and grouped border to the current bar items.
  void applySelectionHighlight();
  /// Enables the header merge button iff 2 or more filter-visible sources are selected (and the view is not
  /// interaction-locked).
  void updateMergeButton();
  /// Recomputes the data span from the bars' current visual positions (including an in-flight drag's ghost) and pushes
  /// it to the ruler tint and the empty-area hatch, so both track drags live without a full rebuild (suppressed
  /// mid-drag).
  void updateDataSpanFromItems();
  /// The current selection as a list, for the intent signals.
  [[nodiscard]] QList<quint64> selectedIdsList() const;
  /// The selected ids that pass the current dataset filter (are in scene_), in display order. The merge prompt and
  /// merge action use it, so a filter-hidden source is neither counted nor merged.
  [[nodiscard]] QList<quint64> visibleSelectedIdsList() const;
  /// The padded, sticky scene extent: content +-1 min, grown (never shrunk on a small drag) only when a bar is pushed
  /// past the buffer. Recomputed in rebuild() from `scene_.sceneExtent()`; reset when the raw data changes.
  [[nodiscard]] TimeSpan computeBufferedExtent() const;

  /// Recenters the interaction-lock overlay (setInteractionLocked) over the scene viewport, capping its width so it
  /// wraps on a narrow timeline. Called on resize, zoom and splitter drag, and when the lock turns on. (The
  /// PJ::Scrollbar overlays recalculate their own geometry through scrollbar connections.)
  void updateLockOverlayGeometry();

  /// Refreshes the left name column: one row per track, positioned in viewport-local y (scene row y minus the vertical
  /// scroll offset) so each name lines up with its bar's row. Cheap; called from rebuild() and on vertical scroll.
  void updateNamePanel();

  /// Pins the header (ruler numbers and the needles' time pills) to the visible viewport top by re-placing the ruler
  /// item and the needles' header anchor at the current vertical scroll offset: vertical scrolling moves the rows under
  /// a stationary number line. Called from rebuild() and on vertical scroll.
  void syncStickyHeader();

  /// Name-column press: edits the selection per `mods` (see selectNameRow) and, on a plain press of a row, primes a
  /// possible drag-to-reorder. Driven from eventFilter() on name_panel_.
  void namePanelPress(const QPoint& panel_pos, Qt::KeyboardModifiers mods);
  void namePanelMove(const QPoint& panel_pos, bool button_down);
  void namePanelRelease(const QPoint& panel_pos);
  /// Track-row index whose band contains the panel-local y, or -1.
  [[nodiscard]] int nameRowAt(double panel_y) const;
  /// Insertion index (0..N) for a drop at the panel-local y.
  [[nodiscard]] int nameDropIndex(double panel_y) const;
  /// Moves track `from` to insertion `drop_index`, re-feeds via setTracks (the row order changes but the data signature
  /// does not, so the view does not reset), and emits tracksReordered with the new order.
  void applyTrackReorder(int from, int drop_index);

  TimelineScene scene_;  // the pure model/engine (holds the FILTER-matching subset)
  // Full track list as last set by the host; the name-column filter selects which are fed into scene_ (see
  // applyDatasetFilter). dataset_filter_ is the case-insensitive substring filter ("" = show all).
  std::vector<TimelineTrack> all_tracks_;
  QString dataset_filter_;
  TimelineViewport viewport_;
  TimeSpan buffered_extent_;         // sticky padded extent (see computeBufferedExtent)
  bool extent_needs_reset_ = true;   // true after a raw-data change → re-pad from scratch
  quint64 raw_data_sig_ = 0;         // hash of (id,t_min,t_max) to detect data vs offset-only changes
  std::vector<QColor> bar_colors_;   // index-aligned with scene_.tracks()
  std::vector<QString> bar_labels_;  // index-aligned with scene_.tracks()
  QGraphicsScene* gscene_ = nullptr;
  QGraphicsView* view_ = nullptr;
  // Left track-name column: one pinned row per track, so the dataset name stays visible when its bar scrolls off or
  // squashes thin; updateNamePanel() lines the rows up with the bar rows.
  timeline_detail::TimelineNamePanel* name_panel_ = nullptr;
  QSplitter* name_splitter_ = nullptr;  // [name column | view]; its handle is the separator
  // Name-row drag-to-reorder state (panel-local).
  int name_drag_index_ = -1;        // grabbed track row, -1 = none
  bool name_dragging_ = false;      // moved past the start threshold
  double name_drag_press_y_ = 0.0;  // panel-local y at grab
  QPushButton* align_button_ = nullptr;
  timeline_detail::TimelineBackgroundItem* background_item_ = nullptr;
  timeline_detail::TimelineRulerItem* ruler_item_ = nullptr;
  timeline_detail::TimelineNeedleItem* playhead_item_ = nullptr;   // pink/purple
  timeline_detail::TimelineNeedleItem* reference_item_ = nullptr;  // light/dark blue
  std::vector<timeline_detail::TimelineBarItem*> bar_items_;       // index-aligned with scene_.tracks()

  qint64 playhead_ns_ = 0;          // playhead position in display-ns
  qint64 reference_ns_ = 0;         // reference-line position in display-ns
  bool reference_visible_ = false;  // reference line shown (host toggle); also gates its grab
  // Fixed 0:00 reference of the ruler and marker-pill labels. Captured once when the data changes (load, remove), not
  // recomputed per drag: the time axis is a stationary absolute frame, and dragging a dataset moves only its bar, never
  // the timing headers.
  qint64 ruler_epoch_ns_ = 0;
  // When true, ruler ticks and marker pills read display-ns as epoch ns and show the absolute Unix timestamp in
  // seconds; false (default) shows elapsed time relative to ruler_epoch_ns_. A label choice only; driven by the host's
  // "use time offset" toggle through setAbsoluteTimeLabels.
  bool absolute_time_labels_ = false;
  // Rendering of the absolute labels: wall clock or Unix timestamp (setAbsoluteClockFormat).
  TimeLabelFormat absolute_clock_format_ = TimeLabelFormat::kSeconds;
  // ns added to playhead, range and reference values supplied through the double setters (the host's external playback
  // frame) to reach the bars' internal frame, and subtracted from emitted seeks. Integer ns, so the needle stays
  // bit-exact with the playback instant (setTimeFrameOffsetNs).
  qint64 time_frame_offset_ns_ = 0;

  // --- bar and playhead drag state ---
  // rebuild() is suppressed while a bar drag is in progress, so the dragged items cannot dangle; the host's setTracks()
  // response after release re-populates. The bar being dragged carries the whole multi-selection (group move): each
  // member shifts by the same pixel delta. Pressing an unselected bar drags just that one. Empty when no bar drag is
  // active.
  struct DragMember {
    timeline_detail::TimelineBarItem* item = nullptr;
    TimelineSourceId id = 0;
    qint64 base_offset = 0;  // member's display offset at drag start
  };
  std::vector<DragMember> drag_group_;
  double drag_start_scene_x_ = 0.0;
  bool bar_drag_changed_ = false;
  // Coalesces per-event view-chrome changes (wheel ticks, name-column drags) into one viewStateChangeCommitted after
  // the gesture goes quiet.
  QTimer view_state_commit_debounce_;
  bool auto_zoom_ = true;           // re-fit the largest extent on new data / fitToContents()
  bool fit_pending_ = false;        // a fitToContents()/enable requested a fit on the next rebuild
  bool force_fit_pending_ = false;  // zoomToFit() requested a fit ignoring the auto-zoom preference
  bool snap_enabled_ = true;        // edge-snap while dragging (host-toggled)
  // Read-only mode (host-driven, while live-streaming): suppresses all user manipulation and needle seeking; navigation
  // and slave needle updates stay live (setInteractionLocked).
  bool interaction_locked_ = false;
  QGraphicsLineItem* snap_line_item_ = nullptr;  // alignment guide shown during a snap
  // Sticky-snap state of the current drag (reset when a bar drag starts): once a dragged edge snaps to a neighbour, it
  // holds until the cursor leaves a wider release band, which prevents flicker between nearby candidates.
  bool snap_active_ = false;
  qint64 snap_active_delta_ = 0;  // the held common shift (ns)
  qint64 snap_active_edge_ = 0;   // the held neighbour edge (display-ns) for the guide
  bool dragging_playhead_ = false;
  bool dragging_reference_ = false;

  // --- background left-drag pan ---
  // Armed on a press over empty space; pan_moved_ flips once the cursor passes the threshold (a release without moving
  // reads as a deselecting click, not a pan). The horizontal scrollbar pans opposite the cursor delta, so content
  // follows the cursor.
  bool panning_ = false;
  bool pan_moved_ = false;
  double pan_start_global_x_ = 0.0;
  int pan_start_scroll_value_ = 0;

  // --- multi-selection (name-column click) ---
  std::set<TimelineSourceId> selected_ids_;  // sources highlighted/grouped
  // Anchor of Shift+click range selection, by source id so it survives a rebuild; 0 = none. Set on every plain or Ctrl
  // click. The merge button lives in the name panel's header (TimelineNamePanel::mergeButton()); the Timeline only
  // enables and disables it from the selection (updateMergeButton).
  TimelineSourceId selection_anchor_id_ = 0;

  // Centered "pause playback to interact" pill over the scene while interaction_locked_ (live streaming and playing);
  // mouse-transparent and informational (setInteractionLocked, updateLockOverlayGeometry).
  QLabel* lock_overlay_ = nullptr;

  // Overlay scroll pills replacing the native horizontal and vertical scroll bars. Attached to view_ in the
  // constructor, after Timeline's own viewport event filter, so the Scrollbar's filter runs first in the LIFO chain and
  // can consume strip-drag events before Timeline's pan and zoom logic sees them.
  Scrollbar* h_scrollbar_ = nullptr;  // bottom-strip overlay (horizontal)
  Scrollbar* v_scrollbar_ = nullptr;  // right-strip overlay (vertical)
};

}  // namespace PJ
