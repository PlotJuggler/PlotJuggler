#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot_marker.h>

#include <QColor>
#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <optional>
#include <vector>

class QwtPlot;
class QwtPlotCurve;

namespace PJ {

std::optional<QPointF> curvePointAt(const QwtPlotCurve* curve, double x);

/// One row of the tracker's value box.
struct ValueBoxRow {
  QColor ink;
  QString text;
};

class CurveTracker : public QObject {
  Q_OBJECT
 public:
  enum Parameter { kLineOnly, kValue, kValueName };

  CurveTracker(QwtPlot* plot, QColor color);
  ~CurveTracker() override;

  [[nodiscard]] QPointF actualPosition() const noexcept {
    return previous_tracker_point_;
  }
  [[nodiscard]] bool isEnabled() const noexcept {
    return visible_;
  }
  [[nodiscard]] Parameter parameter() const noexcept {
    return parameter_;
  }
  // Whether the floating value box ("time : … / curve values") is currently
  // shown. Always hidden in kLineOnly mode.
  [[nodiscard]] bool valueBoxVisible() const noexcept {
    return text_marker_ != nullptr && text_marker_->isVisible();
  }
  // The value box's rows: the time, then one per curve sampled at the last position (also kept while the box is
  // hidden). valueBoxText() is their text, one line each.
  [[nodiscard]] const std::vector<ValueBoxRow>& valueBoxRows() const noexcept {
    return value_box_rows_;
  }
  [[nodiscard]] QString valueBoxText() const;
  // The value box's vertical anchor in plot coordinates, where it is drawn now (it glides toward the curves' mid value
  // at kLabelSpeedPxPerSec, so it can lag the last position).
  [[nodiscard]] double valueBoxY() const;
  // Whether the value box is still moving toward its value (it keeps moving after positions stop arriving).
  [[nodiscard]] bool valueBoxGliding() const noexcept {
    return label_timer_.isActive();
  }

 signals:
  // The value box moved by itself (a glide step with no new position): the owner repaints its overlays.
  void valueBoxMoved();

 public slots:
  void setPosition(const QPointF& pos);
  void setReferencePosition(std::optional<QPointF> reference_pos);
  void setParameter(Parameter parameter);
  void setEnabled(bool enable);
  void redraw() {
    setPosition(previous_tracker_point_);
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  // The display parameter that permits a value box at all (kLineOnly never does).
  // Single home for the rule shared by setEnabled() and setPosition().
  [[nodiscard]] bool valueBoxAllowed() const noexcept {
    return parameter_ != kLineOnly;
  }
  // Moves the value box toward label_target_y_ by the distance allowed since the last step; stops the glide on arrival.
  void stepValueBox();
  // label_value_y_, moved just enough that a box `box_height_px` tall centred on it stays inside the canvas.
  [[nodiscard]] double boxAnchorY(double box_height_px) const;

  QPointF previous_tracker_point_;
  std::optional<QPointF> reference_pos_;
  std::vector<QwtPlotMarker*> point_markers_;
  QwtPlotMarker* line_marker_ = nullptr;
  QwtPlotMarker* text_marker_ = nullptr;
  std::vector<ValueBoxRow> value_box_rows_;
  QwtPlot* plot_ = nullptr;
  Parameter parameter_ = kValue;
  bool visible_ = true;
  // Glide of the value box: a box that was hidden, or a seek (a large time jump), snaps to its target; otherwise it
  // moves at a capped pixel speed so playback over noisy curves does not make it jump every frame.
  double label_target_y_ = 0.0;
  // The curves' mid value at the last position. label_target_y_ is derived from it in canvas pixels, so it is
  // recomputed when the canvas resizes (a new dock is laid out small first).
  double label_value_y_ = 0.0;
  bool label_shown_ = false;
  QPointer<QWidget> watched_canvas_;  // the canvas this filters resizes of; PlotWidgetBase can replace it
  QTimer label_timer_;
  QElapsedTimer label_clock_;
};

}  // namespace PJ
