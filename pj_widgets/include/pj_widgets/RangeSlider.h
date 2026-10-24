#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Two-handle range slider with floating value/duration labels, so plugin .ui
// files (loaded by the host's custom QUiLoader) can declare a real range
// slider by class name "RangeSlider". Data + the rangeChanged event flow
// through the WidgetData protocol; see widget_binding.cpp.

#include <QMouseEvent>
#include <QPainter>
#include <QString>
#include <QToolTip>
#include <QWidget>
#include <functional>
#include <vector>

namespace PJ {

class RangeSlider : public QWidget {
  Q_OBJECT

 public:
  enum Option { kNoHandle = 0x0, kLeftHandle = 0x1, kRightHandle = 0x2, kDoubleHandles = kLeftHandle | kRightHandle };
  Q_DECLARE_FLAGS(Options, Option)

  explicit RangeSlider(QWidget* parent = nullptr);
  RangeSlider(Qt::Orientation ori, Options t = kDoubleHandles, QWidget* parent = nullptr);

  QSize minimumSizeHint() const override;

  int getMinimun() const;
  int getMaximun() const;
  int getLowerValue() const;
  int getUpperValue() const;
  void setRange(int a_minimum, int a_maximum);

  void setOptions(Options t);

  // Boundary segments: one box per marker covering [start, end] (in slider
  // units) drawn at its TRUE extent — so disjoint selections leave blank slider
  // space between boxes — with an optional label centered inside and a tint over
  // the boxes overlapping the current [lower, upper] selection. Lets the slider
  // double as a segment ("which chunk falls in the range") indicator. Empty clears.
  struct Marker {
    int start = 0;
    int end = 0;
    QString label;
  };
  void setMarkers(std::vector<Marker> markers);
  void setShowHandleValueTooltip(bool on);
  bool showHandleValueTooltip() const;

  // Floating labels — painted above handles during drag.
  void setFloatingLabelsVisible(bool on);
  bool floatingLabelsVisible() const;

  // Custom formatters for floating labels (default: decimal number).
  void setLabelFormatter(std::function<QString(double)> formatter);
  void setCenterLabelFormatter(std::function<QString(double, double)> formatter);

  void setRangeReal(double min_v, double max_v, int decimals);
  void setLowerValueReal(double v);
  void setUpperValueReal(double v);
  double lowerValueReal() const;
  double upperValueReal() const;
  int decimals() const;
  int toInt(double v) const;
  double toReal(int v) const;

 protected:
  void paintEvent(QPaintEvent* a_event) override;
  void mousePressEvent(QMouseEvent* a_event) override;
  void mouseMoveEvent(QMouseEvent* a_event) override;
  void mouseReleaseEvent(QMouseEvent* a_event) override;
  void changeEvent(QEvent* a_event) override;
  void leaveEvent(QEvent* e) override;

  QRectF firstHandleRect() const;
  QRectF secondHandleRect() const;
  QRectF handleRect(int a_value) const;

 signals:
  void lowerValueChanged(int a_lower_value);
  void upperValueChanged(int a_upper_value);
  void rangeChanged(int a_min, int a_max);

 public slots:
  void setLowerValue(int a_lower_value);
  void setUpperValue(int a_upper_value);
  void setMinimum(int a_minimum);
  void setMaximum(int a_maximum);

 private:
  Q_DISABLE_COPY(RangeSlider)
  int validLength() const;

  // Y of the track's top (horizontal orientation). With floating labels the
  // per-handle labels occupy a row ABOVE the track, so the track sits just below
  // that row; without them the track is vertically centered.
  int trackTop() const;

  // Height of the groove + handles (horizontal orientation). Defaults to
  // kScTrackHeight but GROWS to fill the widget when it is given more vertical
  // room than its minimum needs (e.g. stretched to match a taller neighbour), so
  // the visible slider area — not just its bounding box — gets taller. At the
  // natural minimum height this equals kScTrackHeight, so short sliders are
  // unchanged.
  int trackHeight() const;

  int minimum_ = 0;
  int maximum_ = 100;
  int lower_value_ = 0;
  int upper_value_ = 100;
  bool first_handle_pressed_ = false;
  bool second_handle_pressed_ = false;
  bool range_drag_active_ = false;
  int hovered_handle_ = 0;  // 0 none, 1 first, 2 second — drives the PJPurple hover tint (timeSlider parity)
  int range_drag_start_pos_ = 0;
  int range_drag_lower_start_ = 0;
  int range_drag_upper_start_ = 0;
  int interval_ = 100;
  int delta_ = 0;
  Qt::Orientation orientation_ = Qt::Horizontal;
  Options type_ = kDoubleHandles;

  void drawMarkers(QPainter& painter, const QRectF& background_rect);
  std::vector<Marker> markers_;

  bool show_handle_value_tooltip_ = true;
  bool tooltip_visible_ = false;

  bool floating_labels_ = false;
  std::function<QString(double)> label_formatter_;
  std::function<QString(double, double)> center_label_formatter_;

  QRect lower_label_rect_;
  QRect upper_label_rect_;
  QRect center_label_rect_;

  void drawFloatingLabels(QPainter& painter);
  QString formatHandleValue(double value) const;

  void maybeShowHandleTooltip(const QPoint& global_pos, const QPoint& local_pos);
  QString handleValueText(bool left) const;
};

}  // namespace PJ

Q_DECLARE_OPERATORS_FOR_FLAGS(PJ::RangeSlider::Options)
