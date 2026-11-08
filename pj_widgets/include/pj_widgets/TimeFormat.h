#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <cmath>
#include <cstdint>

namespace PJ {

/// How a time value (seconds on the shared display axis) is labeled. One value
/// drives every time label of the app (plot X axis, playback readout, Source
/// Timeline) so they never disagree.
/// The clock formats print a value below kMinWallClockSeconds as an elapsed
/// duration `H:mm:ss` (0:00:00 at the origin, no date, no zone).
enum class TimeLabelFormat : std::uint8_t {
  kSeconds,     ///< Plain decimal seconds, the value as-is.
  kLocalClock,  ///< Wall-clock time of day in the system time zone; the value is Unix epoch seconds.
  kUtcClock,    ///< Wall-clock time of day in UTC; the value is Unix epoch seconds.
};

/// Wall-clock formats read values below this (one year after the epoch) as an
/// elapsed duration from 0:00:00, since no real instant lands there: data with
/// the time offset removed or aligned to start at 0, and time-since-boot or sim
/// clocks (ULog, many ROS bags), do. A 1970 time of day shifted by the local zone
/// would read as nonsense.
inline constexpr double kMinWallClockSeconds = 365.0 * 86400.0;

/// True when a clock format prints `seconds` as a duration (see kMinWallClockSeconds).
[[nodiscard]] inline bool readsAsDuration(double seconds) noexcept {
  return std::abs(seconds) < kMinWallClockSeconds;
}

/// True for the two wall-clock formats (the value is read as an epoch instant).
[[nodiscard]] constexpr bool isWallClock(TimeLabelFormat format) noexcept {
  return format == TimeLabelFormat::kLocalClock || format == TimeLabelFormat::kUtcClock;
}

/// The format for the app-wide "date time" toggle: off is plain seconds; on is the
/// wall clock in local time or UTC. The VALUE then decides instant vs duration
/// (kMinWallClockSeconds), not the "use time offset" flag: per-dataset display
/// offsets (alignment, a restored layout) can make values relative or absolute
/// regardless of that flag, so only the value is reliable.
[[nodiscard]] TimeLabelFormat clockFormatFor(bool date_time, bool utc) noexcept;

/// Formats `seconds` with `decimals` fractional digits (clamped to 0..6),
/// rounded to the nearest unit, never truncated, so a label and the playback
/// readout cannot disagree by one in the last digit. `with_date` prefixes
/// `yyyy-MM-dd ` on a wall-clock instant only, never on a duration. Values too
/// large for exact integer rounding (or non-finite) fall back to plain seconds.
[[nodiscard]] QString formatTimeLabel(double seconds, TimeLabelFormat format, int decimals, bool with_date = false);

/// Fractional digits needed to tell apart labels `step_seconds` apart: 0 for
/// whole seconds, 1 for 0.1 s, ... up to 6. Tolerates the float noise of a step
/// computed as a tick difference (0.0999999 needs 1 digit, not 2).
[[nodiscard]] int clockDecimalsForStep(double step_seconds) noexcept;

}  // namespace PJ
