// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotScaleDraw.h"

#include <qwt_text.h>

#include <QLocale>
#include <cmath>
#include <limits>

namespace PJ {

QwtText PlotScaleDraw::label(double value) const {
  const QLocale locale;
  QString str = locale.toString(value, 'f', 6);
  // Strip trailing fractional zeros, then a bare decimal separator — using the
  // locale's own digits/separator so decimal-comma locales trim correctly. The
  // 'f' format guarantees a separator, which shields the integer digits.
  const QString zero = locale.zeroDigit();
  const QString point = locale.decimalPoint();
  while (str.endsWith(zero)) {
    str.chop(zero.size());
  }
  if (str.endsWith(point)) {
    str.chop(point.size());
  }
  return str;
}

QwtText PlotTimeScaleDraw::label(double value) const {
  const QList<double> ticks = scaleDiv().ticks(QwtScaleDiv::MajorTick);
  double step = std::numeric_limits<double>::infinity();
  for (qsizetype i = 1; i < ticks.size(); ++i) {
    step = std::min(step, std::abs(ticks[i] - ticks[i - 1]));
  }
  if (!std::isfinite(step)) {
    step = scaleDiv().range();
  }
  return formatTimeLabel(value, format_, clockDecimalsForStep(step));
}

PlotTimeScaleEngine::PlotTimeScaleEngine(TimeLabelFormat format)
    : QwtDateScaleEngine(format == TimeLabelFormat::kLocalClock ? Qt::LocalTime : Qt::UTC) {}

void PlotTimeScaleEngine::autoScale(int max_num_steps, double& x1, double& x2, double& step_size) const {
  QwtLinearScaleEngine::autoScale(max_num_steps, x1, x2, step_size);
}

QwtScaleDiv PlotTimeScaleEngine::divideScale(
    double x1, double x2, int max_major_steps, int max_minor_steps, double step_size) const {
  constexpr double kMsPerSecond = 1000.0;
  const bool duration = std::max(std::abs(x1), std::abs(x2)) < kMinWallClockSeconds;
  const QwtDateScaleEngine& engine = duration ? duration_engine_ : *this;
  // Qualified call: on *this a virtual divideScale would recurse into this override.
  const QwtScaleDiv ms_div = engine.QwtDateScaleEngine::divideScale(
      x1 * kMsPerSecond, x2 * kMsPerSecond, max_major_steps, max_minor_steps, step_size * kMsPerSecond);
  QList<double> ticks[QwtScaleDiv::NTickTypes];
  for (int type = 0; type < QwtScaleDiv::NTickTypes; ++type) {
    for (const double tick_ms : ms_div.ticks(type)) {
      ticks[type].append(tick_ms / kMsPerSecond);
    }
  }
  // Keep the caller's exact bounds: x * 1000 / 1000 need not round-trip, and the
  // axis must not drift by an ulp on every zoom.
  return QwtScaleDiv(x1, x2, ticks);
}

}  // namespace PJ
