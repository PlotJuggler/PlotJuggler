#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// PJ4's DISPLAY-relative time vocabulary: the app-level coordinate the Qwt axis
// and PlaybackEngine speak, layered on top of the absolute time spine in
// pj_base/time.hpp (PJ::Timepoint / PJ::Duration / fromRaw / toRaw). The spine
// stays int64 across storage/ABI/wire and its chrono mirror now lives in pj_base
// so every layer can name absolute time; the DISPLAY types below stay here
// because the per-dataset display offset is an app-presentation policy, not an
// SDK concern. Two concepts, kept un-mixable by the type system:
//   * DisplayOffset   — the per-dataset shift (a strong-named Duration).
//   * DisplaySeconds  — display-relative seconds (raw - display_offset) the Qwt
//                       axis / PlaybackEngine speak; a strong double, so absolute
//                       seconds can't masquerade as a display coordinate.
//
// DisplaySeconds wraps a double (not an opaque enum) so display-axis math stays
// natural; the safety lives at toDisplaySeconds()/rawToDisplaySeconds(), which
// DEMAND a DisplayOffset so the raw->display subtraction can't be skipped.

#include <chrono>
#include <cmath>
#include <compare>
#include <limits>
#include <optional>
#include <type_traits>

#include "pj_base/dataset.hpp"  // PJ::TimeDomain (display_offset)
#include "pj_base/time.hpp"     // PJ::Timepoint, PJ::Duration, fromRaw/toRaw (absolute spine)
#include "pj_base/types.hpp"    // PJ::Timestamp, PJ::Range

namespace PJ {

/// Seconds <-> nanoseconds conversion factor for the IDataWidget boundary
/// (tracker time travels as seconds in a bare double; storage timestamps are
/// int64 nanoseconds). Derived from std::chrono so the ratio can never drift
/// from the time types it converts between. Prefer converting through
/// std::chrono::duration directly where practical (it adds nothing at -O2); use
/// this constant where a plain multiplier/divisor reads better.
inline constexpr double kNanosecondsPerSecond = static_cast<double>(std::chrono::nanoseconds::period::den);

/// The per-dataset display shift carried as a strong-named Duration, so call
/// sites read "this is the subtrahend that turns a Timepoint into display time"
/// (TimeDomain::display_offset). It IS a Duration; the distinct name documents
/// intent without inventing a third arithmetic type.
struct DisplayOffset {
  Duration value{0};
};

/// DISPLAY-RELATIVE time in seconds: the Qwt-axis / PlaybackEngine coordinate.
/// Strong wrapper over double so it cannot be confused with an absolute-seconds
/// double. Arithmetic is intentionally narrow: a difference of two coordinates
/// is a bare seconds delta (no origin); a coordinate may be shifted by a delta.
struct DisplaySeconds {
  double value = 0.0;

  explicit constexpr DisplaySeconds(double seconds = 0.0) noexcept : value(seconds) {}

  friend constexpr bool operator==(const DisplaySeconds&, const DisplaySeconds&) = default;
  friend constexpr auto operator<=>(const DisplaySeconds&, const DisplaySeconds&) = default;

  /// Difference of two display coordinates: a span in seconds (no origin), so a
  /// plain double — never a DisplaySeconds.
  friend constexpr double operator-(DisplaySeconds a, DisplaySeconds b) noexcept {
    return a.value - b.value;
  }
  /// Shift a display coordinate by a seconds delta (e.g. a tracker step).
  friend constexpr DisplaySeconds operator+(DisplaySeconds a, double delta_sec) noexcept {
    return DisplaySeconds{a.value + delta_sec};
  }
  friend constexpr DisplaySeconds operator-(DisplaySeconds a, double delta_sec) noexcept {
    return DisplaySeconds{a.value - delta_sec};
  }
};

/// Display interval the PlaybackEngine range speaks (reuses PJ::Range, not pair).
using DisplayRange = PJ::Range<DisplaySeconds>;

/// Wrap a display-relative seconds value without implying it came from Qwt.
[[nodiscard]] constexpr DisplaySeconds displaySeconds(double seconds) noexcept {
  return DisplaySeconds{seconds};
}

/// Build a display-relative seconds interval without spelling out the wrapper at
/// each app/UI call site.
[[nodiscard]] constexpr DisplayRange displayRange(double min_seconds, double max_seconds) noexcept {
  return DisplayRange{displaySeconds(min_seconds), displaySeconds(max_seconds)};
}

/// Signed difference of absolute nanosecond timestamps, converted only after
/// integer subtraction so a small epoch-scale frame change stays exact.
[[nodiscard]] constexpr double timestampDifferenceSeconds(Timestamp lhs, Timestamp rhs) noexcept {
  using UnsignedTimestamp = std::make_unsigned_t<Timestamp>;
  if (lhs >= rhs) {
    const UnsignedTimestamp delta = static_cast<UnsignedTimestamp>(lhs) - static_cast<UnsignedTimestamp>(rhs);
    return static_cast<double>(delta) / kNanosecondsPerSecond;
  }
  const UnsignedTimestamp delta = static_cast<UnsignedTimestamp>(rhs) - static_cast<UnsignedTimestamp>(lhs);
  return -static_cast<double>(delta) / kNanosecondsPerSecond;
}

/// Whether subtracting an offset from an absolute nanosecond timestamp stays
/// inside the signed timestamp range.
[[nodiscard]] constexpr bool timelineDifferenceFits(Timestamp raw_ns, Timestamp offset_ns) noexcept {
  if (offset_ns > 0) {
    return raw_ns >= std::numeric_limits<Timestamp>::min() + offset_ns;
  }
  if (offset_ns < 0) {
    return raw_ns <= std::numeric_limits<Timestamp>::max() + offset_ns;
  }
  return true;
}

/// Subtract a display offset without signed overflow; nullopt means the
/// requested display coordinate is not representable.
[[nodiscard]] constexpr std::optional<Timestamp> checkedTimelineDifference(
    Timestamp raw_ns, Timestamp offset_ns) noexcept {
  if (!timelineDifferenceFits(raw_ns, offset_ns)) {
    return std::nullopt;
  }
  return raw_ns - offset_ns;
}

// --- display seam: the offset is MANDATORY, so raw->display can't be skipped ---

/// Build the per-dataset display shift from the engine's TimeDomain.
[[nodiscard]] constexpr DisplayOffset offsetOf(const TimeDomain& domain) noexcept {
  return DisplayOffset{Duration{domain.display_offset}};
}

/// Absolute Timepoint -> display-relative seconds. THE single named place the
/// display_offset subtraction happens (display_time = raw_time - display_offset).
/// The DisplayOffset argument is required, so a caller cannot forget it.
[[nodiscard]] constexpr DisplaySeconds toDisplaySeconds(Timepoint absolute, DisplayOffset offset) noexcept {
  const Duration display_ns = absolute.time_since_epoch() - offset.value;
  return DisplaySeconds{std::chrono::duration<double>(display_ns).count()};
}

/// Display-relative seconds -> absolute Timepoint (inverse of toDisplaySeconds).
/// Rounds to the nearest nanosecond (not truncate) so the round-trip is stable.
[[nodiscard]] constexpr Timepoint toAbsolute(DisplaySeconds seconds, DisplayOffset offset) noexcept {
  const auto display_ns = std::chrono::round<Duration>(std::chrono::duration<double>(seconds.value));
  return Timepoint{display_ns + offset.value};
}

/// Raw int64-ns -> display-relative seconds, for call sites holding a Timestamp
/// + TimeDomain. Centralizes the (raw - offset)/1e9 idiom in one place.
[[nodiscard]] constexpr DisplaySeconds rawToDisplaySeconds(Timestamp raw_ns, DisplayOffset offset) noexcept {
  return toDisplaySeconds(fromRaw(raw_ns), offset);
}

/// Display-relative seconds -> raw int64-ns (inverse of rawToDisplaySeconds).
[[nodiscard]] constexpr Timestamp displaySecondsToRaw(DisplaySeconds seconds, DisplayOffset offset) noexcept {
  return toRaw(toAbsolute(seconds, offset));
}

/// rawToDisplaySeconds raised to the first double that displaySecondsToRaw maps back to `raw_ns` or later.
/// Far from the display origin a double does not hold every nanosecond (near 1.7e9 s it steps ~238 ns),
/// and the nearest one can convert back to an instant before `raw_ns`: a seek to that time would show
/// the sample before. Used for every time a seek may come back with: a sample handed to a Widget plugin,
/// pj.playback.v1's to_display_time, and the end of a playback range (the engine clamps a seek into the
/// range, so a nearest-rounded end would clamp a seek to the last sample onto the one before it).
[[nodiscard]] inline DisplaySeconds rawToDisplaySecondsAtOrAfter(Timestamp raw_ns, DisplayOffset offset) noexcept {
  DisplaySeconds seconds = rawToDisplaySeconds(raw_ns, offset);
  // Within a millisecond of the int64 limits (an "end of time" sentinel) a raised double no longer converts
  // back to a Timestamp: keep the nearest time there.
  constexpr Timestamp kLimitMargin = 1'000'000;
  if (raw_ns > std::numeric_limits<Timestamp>::max() - kLimitMargin ||
      raw_ns < std::numeric_limits<Timestamp>::min() + kLimitMargin) {
    return seconds;
  }
  // Each step moves the converted instant by one double step; two cover the rounding of both conversions.
  for (int step = 0; step < 4 && displaySecondsToRaw(seconds, offset) < raw_ns; ++step) {
    seconds.value = std::nextafter(seconds.value, std::numeric_limits<double>::infinity());
  }
  return seconds;
}

// --- Qwt / IDataWidget edge: DisplaySeconds <-> bare double (axis stays double) ---

/// Unwrap to the bare display-seconds double the Qwt axis / IDataWidget expect.
[[nodiscard]] constexpr double toAxisDouble(DisplaySeconds seconds) noexcept {
  return seconds.value;
}

/// Wrap a display-seconds double arriving from the Qwt/IDataWidget edge — the
/// only legal door for an axis double into the typed world, so it can never be
/// mistaken for an absolute-seconds value.
[[nodiscard]] constexpr DisplaySeconds fromAxisDouble(double display_sec) noexcept {
  return displaySeconds(display_sec);
}

}  // namespace PJ
