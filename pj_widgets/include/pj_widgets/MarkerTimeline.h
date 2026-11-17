#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
// Editable multi-marker timeline: a horizontal time strip holding any number of marks of two kinds: Region (a resizable
// [start,end] span with two draggable purple edges and a blue fill) and Event (a single movable purple handle). Plugin
// .ui files (loaded by the host's QUiLoader) declare it by the class name "MarkerTimeline"; the mark set and the
// marksChanged event flow through the WidgetData protocol (widget_binding.cpp). The widget is a dumb editor: the owner
// supplies marks with setMarks() and reacts to marksChanged(); placement policy (where a freshly added mark lands)
// lives with the owner.
//
// Visuals mirror RangeSlider, so the two read as one family: PJLightBlue range fill, PJLightPurple handles at rest,
// PJPurple when hovered or dragged. Time labels (ns) go through an injectable formatter.

#include <QVector>
#include <QWidget>
#include <functional>

class QMouseEvent;
class QPaintEvent;

namespace PJ {

class MarkerTimeline : public QWidget {
  Q_OBJECT

 public:
  enum class Kind { kRegion, kEvent };

  // One mark in the integer step domain [minimum, maximum]. For an Event `end` is ignored (kept equal to `start`).
  struct Mark {
    int id = 0;
    Kind kind = Kind::kRegion;
    int start = 0;
    int end = 0;
    bool operator==(const Mark&) const = default;
  };

  explicit MarkerTimeline(QWidget* parent = nullptr);

  QSize minimumSizeHint() const override;

  // The step domain the marks live in ([0, 1000]); matches the slider quantization the producer plugin uses. Existing
  // marks are clamped into the new range.
  void setRange(int minimum, int maximum);
  int minimum() const {
    return minimum_;
  }
  int maximum() const {
    return maximum_;
  }

  // The absolute ns span the step domain maps onto, used only to format the hover tooltip and labels; unset shows raw
  // step values.
  void setTimeSpan(qint64 min_ns, qint64 max_ns);
  void setLabelFormatter(std::function<QString(qint64)> formatter);

  // Replaces the whole set (last writer wins, as the producer's republish model). Does not emit marksChanged: it
  // reflects an owner-driven update, not a user edit.
  void setMarks(const QVector<Mark>& marks);
  const QVector<Mark>& marks() const {
    return marks_;
  }

  // Mutators used by the mouse handlers and direct callers (tests); each emits marksChanged(). `start` and `end` are
  // clamped into range; regions keep start <= end.
  int addMark(Kind kind, int start, int end);  // returns the assigned id
  void deleteMark(int id);
  void moveMark(int id, int new_start);                   // preserves width (region) / position (event)
  void resizeRegion(int id, int new_start, int new_end);  // region edges; no-op for events
  void clearMarks();

 signals:
  // Emitted on any user edit (drag-move, edge-resize, right-click delete) and by the direct mutators above; the owner
  // reads marks() for the new set.
  void marksChanged();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void contextMenuEvent(QContextMenuEvent* event) override;
  void leaveEvent(QEvent* event) override;
  // Finalizes an in-flight drag if the widget is hidden or closed before mouse-release (no release event fires then),
  // so dragging_id_ never wedges setMarks() shut.
  void hideEvent(QHideEvent* event) override;

 private:
  enum class Grab { kNone, kMoveRegion, kResizeStart, kResizeEnd, kMoveEvent };

  // Geometry: the center-x pixel of a step value and its inverse, inside the track's usable span (handles stay fully
  // visible at the extremes).
  double pxForValue(int value) const;
  int valueForPx(double px) const;
  double usableLength() const;

  // Commits an in-flight drag: clears the drag state and emits marksChanged once. Shared by mouseReleaseEvent and
  // hideEvent.
  void finalizeDrag();
  Mark* markById(int id);
  // The topmost mark under `pos` (last drawn wins) and which part was grabbed.
  std::pair<int, Grab> hitTest(const QPoint& pos) const;
  int clampValue(int v) const;
  // Clamps a mark's endpoints into range; an event collapses end onto start, a region with end < start is swapped.
  void normalize(Mark& m) const;
  QString tooltipFor(const Mark& mark) const;

  QVector<Mark> marks_;
  int minimum_ = 0;
  int maximum_ = 1000;
  int next_id_ = 1;

  qint64 min_ns_ = 0;
  qint64 max_ns_ = 0;
  bool has_time_span_ = false;
  std::function<QString(qint64)> label_formatter_;

  int dragging_id_ = 0;
  Grab grab_ = Grab::kNone;
  double grab_offset_px_ = 0.0;  // cursor.x - pxForValue(anchor) at grab time
  int hovered_id_ = 0;
};

}  // namespace PJ
