#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_date_scale_engine.h>
#include <qwt_scale_draw.h>
#include <qwt_text.h>

#include "pj_widgets/TimeFormat.h"

namespace PJ {

/// Scale draw installed on every PJ4 plot axis: tick labels use fixed
/// notation (6 decimals, default locale) with trailing zeros stripped,
/// instead of Qwt's default shortest-form QLocale::toString(). Keep in sync
/// with the twin in pj_dialog_host/src/chart_preview_widget.cpp, which cannot
/// link this module.
class PlotScaleDraw : public QwtScaleDraw {
 public:
  [[nodiscard]] QwtText label(double value) const override;
};

/// X-axis scale draw of a time plot while the app's date-time toggle is on:
/// labels read as a clock (`HH:mm:ss`, see formatTimeLabel) with as many
/// fractional digits as the major-tick spacing needs. The date is never shown.
class PlotTimeScaleDraw : public QwtScaleDraw {
 public:
  explicit PlotTimeScaleDraw(TimeLabelFormat format) : format_(format) {}
  [[nodiscard]] QwtText label(double value) const override;

 private:
  TimeLabelFormat format_;
};

/// X-axis scale engine paired with PlotTimeScaleDraw: places ticks on clock
/// boundaries (whole seconds, minutes, hours; 1/2/5 steps below a second) via
/// QwtDateScaleEngine. That engine works in epoch MILLISECONDS while the plot axis
/// is in seconds, so divideScale converts on the way in and out. It always runs
/// in UTC: kLocalClock shifts the interval by the zone offset (looked up once per
/// call) instead, because Qwt's local-time mode converts every candidate tick
/// through the system zone on every pan/zoom step. An interval that reads
/// as a duration (readsAsDuration) is not shifted.
class PlotTimeScaleEngine : public QwtDateScaleEngine {
 public:
  explicit PlotTimeScaleEngine(TimeLabelFormat format);
  /// Linear autoscale in seconds; the date engine's version would read seconds as ms.
  void autoScale(int max_num_steps, double& x1, double& x2, double& step_size) const override;
  [[nodiscard]] QwtScaleDiv divideScale(
      double x1, double x2, int max_major_steps, int max_minor_steps, double step_size = 0.0) const override;

 private:
  bool local_;
};

}  // namespace PJ
