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
///   colorRegions(series, mapping)     -- shade the background by value: `series` is a name or series(...); `mapping`
///                                        is a table value -> color or a function v -> color | {color=, label=, ...}
///                                        | nil (no region). Consecutive equal results merge into one region; the
///                                        last one ends at the last sample.
///   ranges({min, max, color}, ...)    -- a mapping for colorRegions. Ranges may overlap: v takes the first listed
///                                        range with min <= v < max; none -> no region. `inf` is math.huge.
///   lessThan(limit, color) / greaterThan(limit, color) -- mappings for colorRegions: `color` where v < limit
///                                        (v > limit), no region elsewhere. In ranges and these, `color` may also be
///                                        a {color=, label=, ...} table.
/// `opts` is an optional table: {label, color, severity, status, category, description}. A color is "#rrggbb", "#rgb"
/// or a Qt color name (case-insensitive); anything else fails the run.
/// Native marker storage (the markers and their strings) is capped by `max_output_bytes`, apart from the VM's memory
/// cap; a script past it fails the run. A success sets `output_bytes`, when given, to what the markers count against
/// it. `cancelled` is polled from the VM's interrupt at every safepoint on the calling thread, so it must be cheap and
/// thread-safe: once it returns true the script stops with "script cancelled", even in a loop that calls nothing
/// native. On a compile or runtime error (or watchdog trip) returns an empty vector and sets `*error` when non-null
/// (cleared on success).
[[nodiscard]] std::vector<PJ::sdk::PlotMarker> runMarkerScript(
    const std::string& code, const SeriesProvider& provider, std::string* error, BudgetLimits limits = {},
    const std::function<bool()>& cancelled = {}, std::size_t max_output_bytes = kMaxMarkerOutputBytes,
    std::size_t* output_bytes = nullptr);

/// A rule's color: "#rrggbb", "#rgb" or one of Qt's color names (QColor::colorNames(), case-insensitive; "transparent"
/// has alpha 0). The names are a copy of Qt's table, since this engine cannot link Qt; nullopt for anything else.
[[nodiscard]] std::optional<PJ::sdk::ColorRGBA> parseMarkerColor(std::string_view text);

/// Compiles and loads the module without running a rule: the syntax gate behind the `validate_data_processor_script`
/// service slot. False sets `*error` to the compiler or loader message; runtime errors are not caught here.
[[nodiscard]] bool validateScript(const std::string& code, std::string* error);

}  // namespace PJ::scripting
