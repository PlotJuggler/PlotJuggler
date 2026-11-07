// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QDateTime>
#include <QTimeZone>

#include "pj_widgets/TimeFormat.h"

namespace PJ {
namespace {

// 2026-11-07 15:51:23 UTC.
constexpr double kEpoch = 1794066683.0;

TEST(TimeFormat, ClockFormatFollowsToggleAndZone) {
  EXPECT_EQ(clockFormatFor(false, false), TimeLabelFormat::kSeconds);
  EXPECT_EQ(clockFormatFor(false, true), TimeLabelFormat::kSeconds);
  EXPECT_EQ(clockFormatFor(true, false), TimeLabelFormat::kLocalClock);
  EXPECT_EQ(clockFormatFor(true, true), TimeLabelFormat::kUtcClock);
}

TEST(TimeFormat, SecondsKeepsPlainNumber) {
  EXPECT_EQ(formatTimeLabel(12.3456, TimeLabelFormat::kSeconds, 3), QStringLiteral("12.346"));
}

TEST(TimeFormat, UtcClockIsTimeOfDayWithRoundedFraction) {
  EXPECT_EQ(formatTimeLabel(kEpoch + 0.25, TimeLabelFormat::kUtcClock, 3), QStringLiteral("15:51:23.250"));
  EXPECT_EQ(formatTimeLabel(kEpoch, TimeLabelFormat::kUtcClock, 0), QStringLiteral("15:51:23"));
  EXPECT_EQ(
      formatTimeLabel(kEpoch + 0.5, TimeLabelFormat::kUtcClock, 3, /*with_date=*/true),
      QStringLiteral("2026-11-07 15:51:23.500"));
}

TEST(TimeFormat, RoundingCarriesIntoTheSecond) {
  // 0.9996 at 3 digits must read :24.000, not :23.1000 or a truncated :23.999.
  EXPECT_EQ(formatTimeLabel(kEpoch + 0.9996, TimeLabelFormat::kUtcClock, 3), QStringLiteral("15:51:24.000"));
}

TEST(TimeFormat, LocalClockMatchesSystemZone) {
  const QString expected =
      QDateTime::fromSecsSinceEpoch(static_cast<qint64>(kEpoch), QTimeZone(QTimeZone::LocalTime)).toString(u"HH:mm:ss");
  EXPECT_EQ(formatTimeLabel(kEpoch, TimeLabelFormat::kLocalClock, 0), expected);
}

// Small values (relative to an offset) read as a signed duration in any zone.
TEST(TimeFormat, SmallValuesAreSignedDurations) {
  EXPECT_EQ(formatTimeLabel(3723.5, TimeLabelFormat::kUtcClock, 1), QStringLiteral("1:02:03.5"));
  EXPECT_EQ(formatTimeLabel(-1.5, TimeLabelFormat::kUtcClock, 3), QStringLiteral("-0:00:01.500"));
  EXPECT_EQ(formatTimeLabel(0.0, TimeLabelFormat::kUtcClock, 0), QStringLiteral("0:00:00"));
  // Durations past a day keep counting hours instead of wrapping.
  EXPECT_EQ(formatTimeLabel(100.0 * 3600.0, TimeLabelFormat::kUtcClock, 0), QStringLiteral("100:00:00"));
}

// Time-since-boot / sim-time values (or data aligned to start at 0) are not
// instants: the wall-clock formats render them as a zone-free duration.
TEST(TimeFormat, WallClockBelowEpochScaleReadsAsDuration) {
  EXPECT_EQ(
      formatTimeLabel(566.676, TimeLabelFormat::kLocalClock, 3, /*with_date=*/true), QStringLiteral("0:09:26.676"));
  EXPECT_EQ(formatTimeLabel(566.676, TimeLabelFormat::kUtcClock, 0), QStringLiteral("0:09:27"));
}

TEST(TimeFormat, DecimalsForStep) {
  EXPECT_EQ(clockDecimalsForStep(60.0), 0);
  EXPECT_EQ(clockDecimalsForStep(1.0), 0);
  EXPECT_EQ(clockDecimalsForStep(0.5), 1);
  EXPECT_EQ(clockDecimalsForStep(0.0999999999), 1);
  EXPECT_EQ(clockDecimalsForStep(0.002), 3);
  EXPECT_EQ(clockDecimalsForStep(1e-9), 6);
  EXPECT_EQ(clockDecimalsForStep(0.0), 3);
}

}  // namespace
}  // namespace PJ
