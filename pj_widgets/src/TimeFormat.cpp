// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/TimeFormat.h"

#include <QDateTime>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace PJ {

namespace {

constexpr int kMaxDecimals = 6;
// Largest |seconds * 10^decimals| rounded through an integer: 2^53, where a
// double still holds every integer exactly.
constexpr double kMaxExactScaled = 9007199254740992.0;

qint64 pow10(int exponent) noexcept {
  qint64 out = 1;
  for (int i = 0; i < exponent; ++i) {
    out *= 10;
  }
  return out;
}

}  // namespace

TimeLabelFormat clockFormatFor(bool date_time, bool utc) noexcept {
  if (!date_time) {
    return TimeLabelFormat::kSeconds;
  }
  return utc ? TimeLabelFormat::kUtcClock : TimeLabelFormat::kLocalClock;
}

QString formatTimeLabel(double seconds, TimeLabelFormat format, int decimals, bool with_date) {
  decimals = std::clamp(decimals, 0, kMaxDecimals);
  const bool duration = isWallClock(format) && readsAsDuration(seconds);
  const qint64 scale = pow10(decimals);
  const double scaled_value = seconds * static_cast<double>(scale);
  if (format == TimeLabelFormat::kSeconds || !std::isfinite(scaled_value) ||
      std::abs(scaled_value) >= kMaxExactScaled) {
    return QString::number(seconds, 'f', decimals);
  }
  // Round ONCE in integer units of the last digit, then split: rounding the
  // fraction alone would print 0.9996 as ":59.1000" instead of carrying.
  const qint64 scaled = std::llround(scaled_value);
  const bool negative = duration && scaled < 0;
  // Durations print a sign and a magnitude; wall-clock instants floor (an
  // instant before the epoch still lands in the right second).
  const qint64 magnitude = negative ? -scaled : scaled;
  qint64 whole = magnitude / scale;
  qint64 frac = magnitude % scale;
  if (frac < 0) {
    frac += scale;
    --whole;
  }

  QString text;
  if (duration) {
    text = QStringLiteral("%1%2:%3:%4")
               .arg(negative ? QStringLiteral("-") : QString())
               .arg(whole / 3600)
               .arg((whole / 60) % 60, 2, 10, QChar('0'))
               .arg(whole % 60, 2, 10, QChar('0'));
  } else {
    const QTimeZone zone(format == TimeLabelFormat::kUtcClock ? QTimeZone::UTC : QTimeZone::LocalTime);
    text = QDateTime::fromSecsSinceEpoch(whole, zone)
               .toString(with_date ? QStringLiteral("yyyy-MM-dd HH:mm:ss") : QStringLiteral("HH:mm:ss"));
  }
  if (decimals > 0) {
    text += QChar('.') + QStringLiteral("%1").arg(frac, decimals, 10, QChar('0'));
  }
  return text;
}

int clockDecimalsForStep(double step_seconds) noexcept {
  if (!(step_seconds > 0.0) || !std::isfinite(step_seconds)) {
    return 3;
  }
  int digits = 0;
  // 1e-6 slack absorbs the float noise of a step taken as a tick difference.
  while (step_seconds < 1.0 - 1e-6 && digits < kMaxDecimals) {
    step_seconds *= 10.0;
    ++digits;
  }
  return digits;
}

}  // namespace PJ
