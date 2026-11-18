// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// marker_engine: the whole-series -> markers path of the Luau engine. Unlike a filter, a marker script reads whole
// series with random access (size/at/atTime), looks back and forward, and emits PlotMarkers (events, regions, value
// bands) instead of one sample per input. The caller supplies a SeriesProvider over its own data (live series, or a
// loaded file in the headless runner). It depends on pj_base and Luau only, never pj_datastore, so one engine links
// into the app and the SDK-only headless runner and a rule runs identically in both.

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <pj_base/builtin/plot_markers.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "pj_scripting/sandbox.h"

namespace PJ::scripting {

/// One sample handed to a marker script: timestamp in nanoseconds + value.
struct MarkerSample {
  double t;
  double v;
};

/// Read-only access to one float series' samples (timestamps in nanoseconds).
/// The script reaches this through the bound `series("name")` accessor.
struct SeriesView {
  std::vector<double> timestamps;
  std::vector<double> values;

  [[nodiscard]] std::size_t size() const {
    return values.size();
  }
  /// Sample at index, or nullopt if out of range.
  [[nodiscard]] std::optional<MarkerSample> at(std::size_t index) const;
  /// Linearly interpolated value at time `t` ns (clamped to the endpoints).
  [[nodiscard]] double atTime(double t) const;
};

/// How the bound `series("name")` resolves names to data. The caller supplies the
/// backing store (live host vs. loaded file); the engine stays agnostic.
struct SeriesProvider {
  std::vector<std::string> names;                            ///< for the bound GetSeriesNames()
  std::function<const SeriesView*(const std::string&)> get;  ///< name -> series or nullptr
};

/// The default ceiling of a rule's native marker output (`runMarkerScript`'s `max_output_bytes`).
inline constexpr std::size_t kMaxMarkerOutputBytes = std::size_t{16} << 20;

/// Runs a Luau rule once over `provider` and returns the emitted markers. The script runs in the sandboxed Luau VM
/// (`limits`) with these bound primitives:
///   series("topic/field")            -> a read accessor (:size()/:at(i)/:atTime(t))
///   startMarker(t) / closeMarker(t, opts?)   -- a time region
///   createMarker(x?, y?, opts?)       -- vline (x) | hline (y) | point (both)
///   createVerticalMarker(x, opts?)    -- a vertical line at x
///   createHorizontalMarker(y, opts?)  -- a horizontal line at y
///   createPointMarker(x, y, opts?)    -- a point at (x, y)
///   createBandMarker(low, high, opts?) -- a shaded value band [low, high]
///   bandPower(series, fLo, fHi)       -- summed FFT power in [fLo,fHi] Hz, DC-removed
///   GetSeriesNames()                  -- list of available series names
///   when(series, condition?)          -- `series` is a name or series(...); `condition` is above(x), below(x),
///                                        between(a, b), outside(a, b), equals(x), isTrue(), changes() or a function
///                                        (v, t, prev, prev_t) -> boolean. Its actions scan the series once:
///     :backgroundColor(color?, opts?)  -- a region per stretch where the condition holds (no color: the severity
///                                        color). Without a condition, `color` is a table value -> color or a
///                                        function (v, t) -> color | opts | nil, and a region spans each run of equal
///                                        results; the last region ends at the last sample. opts.minDuration (s)
///                                        drops shorter regions.
///     :event(opts?) / :verticalLine(opts?) -- a point / line where the condition starts to hold (each sample with
///                                        opts.every; each change for changes()); opts.minGap (s) skips a mark closer
///                                        to the previous one; opts.label may be a function (v, prev) -> string.
/// `opts` is an optional table: {label, color, severity, status, category, description}. A color is a string accepted
/// by parseMarkerColor; anything else fails the run. Native marker storage (the markers and their strings) is capped by
/// `max_output_bytes`, apart from the VM's memory cap; a script past it fails the run. A success sets `output_bytes`,
/// when given, to what the markers count against it. `cancelled` is polled from the VM's interrupt at every safepoint
/// on the calling thread, so it must be cheap and thread-safe: once it returns true the script stops with "script
/// cancelled", even in a loop that calls nothing native. On a compile or runtime error (or watchdog trip) returns an
/// empty vector and sets `*error` when non-null (cleared on success).
[[nodiscard]] std::vector<PJ::sdk::PlotMarker> runMarkerScript(
    const std::string& code, const SeriesProvider& provider, std::string* error, BudgetLimits limits = {},
    const std::function<bool()>& cancelled = {}, std::size_t max_output_bytes = kMaxMarkerOutputBytes,
    std::size_t* output_bytes = nullptr);

/// A rule's color: "#rrggbb", "#rgb" or one of Qt's color names (QColor::colorNames(), case-insensitive, spaces
/// ignored), except "transparent": alpha 0 is PlotMarker's "use the severity color". The names are a copy of Qt's
/// table, since this engine cannot link Qt; nullopt for anything else.
[[nodiscard]] std::optional<PJ::sdk::ColorRGBA> parseMarkerColor(std::string_view text);

/// Compiles and loads the module without running a rule: the syntax gate behind the `validate_data_processor_script`
/// service slot. False sets `*error` to the compiler or loader message; runtime errors are not caught here.
[[nodiscard]] bool validateScript(const std::string& code, std::string* error);

}  // namespace PJ::scripting
