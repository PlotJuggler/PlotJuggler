#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot_marker.h>

#include <QColor>
#include <QObject>
#include <QPointF>
#include <optional>
#include <vector>

class QwtPlot;
class QwtPlotCurve;

namespace PJ {

std::optional<QPointF> curvePointAt(const QwtPlotCurve* curve, double x);

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
  // The value box's HTML, one line per curve sampled at the last position (also kept while the box is hidden).
  [[nodiscard]] QString valueBoxText() const;

 public slots:
  void setPosition(const QPointF& pos);
  void setReferencePosition(std::optional<QPointF> reference_pos);
  void setParameter(Parameter parameter);
  void setEnabled(bool enable);
  void redraw() {
    setPosition(previous_tracker_point_);
  }

 private:
  // The display parameter that permits a value box at all (kLineOnly never does).
  // Single home for the rule shared by setEnabled() and setPosition().
  [[nodiscard]] bool valueBoxAllowed() const noexcept {
    return parameter_ != kLineOnly;
  }

  QPointF previous_tracker_point_;
  std::optional<QPointF> reference_pos_;
  std::vector<QwtPlotMarker*> point_markers_;
  QwtPlotMarker* line_marker_ = nullptr;
  QwtPlotMarker* text_marker_ = nullptr;
  QwtPlot* plot_ = nullptr;
  Parameter parameter_ = kValue;
  bool visible_ = true;
};

}  // namespace PJ
