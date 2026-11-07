// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// marker_engine — the whole-series → markers path of the Luau ScriptEngine.
//
// Distinct from the SISO filter path (lua_siso_transform): a marker script reads
// the WHOLE series at once (random access: size/at/atTime), can look back AND
// forward, and emits PlotMarkers (events, regions, value bands) instead of a
// derived sample per input. This is what the Anomaly Detector needs and what the
// per-sample IncrementalEngine model cannot express (FFT, regions, lookahead).
//
// The engine is host-agnostic: the caller supplies a SeriesProvider over its own
// data (the GUI toolbox's live series, or the headless runner's loaded file), and
// gets back the emitted markers. It depends only on pj_base (PlotMarker) + Luau —
// NOT pj_datastore — so the same engine links into both the app and the SDK-only
// headless runner, preserving the "one engine, GUI == headless" guarantee.

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <pj_base/builtin/plot_markers.hpp>
#include <string>
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

/// Run a Luau detection rule once over `provider`, returning the emitted markers.
/// The script environment is the sandboxed Luau VM (memory cap + instruction-budget
/// watchdog from `limits`) plus these bound primitives:
///   series("topic/field")            -> a read accessor (:size()/:at(i)/:atTime(t))
///   startMarker(t) / closeMarker(t, opts?)   -- a time region
///   createMarker(x?, y?, opts?)       -- vline (x) | hline (y) | point (both)
///   createVerticalMarker(x, opts?)    -- a vertical line at x
///   createHorizontalMarker(y, opts?)  -- a horizontal line at y
///   createPointMarker(x, y, opts?)    -- a point at (x, y)
///   createBandMarker(low, high, opts?) -- a shaded value band [low, high]
///   bandPower(series, fLo, fHi)       -- summed FFT power in [fLo,fHi] Hz, DC-removed
///   GetSeriesNames()                  -- list of available series names
/// `opts` is an optional table: {label, color="#rrggbb", severity, status, category, description}.
/// Native marker storage (the markers and their strings) is bounded by `max_output_bytes`,
/// independently of the VM's memory cap; a script that exceeds it fails the run. A success
/// sets `output_bytes`, when given, to what its markers count against that ceiling.
/// `cancelled`, when set, is polled from the VM's interrupt (every safepoint, on the
/// calling thread, so it must be cheap and thread-safe): once it returns true the script
/// stops with a "script cancelled" error, even inside a loop that makes no native call.
/// On a compile/runtime error (or watchdog trip) returns an empty vector and, if
/// `error` is non-null, sets it to the message (cleared on success).
[[nodiscard]] std::vector<PJ::sdk::PlotMarker> runMarkerScript(
    const std::string& code, const SeriesProvider& provider, std::string* error, BudgetLimits limits = {},
    const std::function<bool()>& cancelled = {}, std::size_t max_output_bytes = kMaxMarkerOutputBytes,
    std::size_t* output_bytes = nullptr);

/// Compile + module-load ONLY (no execution, no inputs, no side effects): the cheap
/// syntax gate behind the `validate_data_processor_script` service slot. Returns true if the script
/// compiles and loads; on false sets `*error` to the compiler/loader message.
/// Runtime / empty-output errors are NOT caught here.
[[nodiscard]] bool validateScript(const std::string& code, std::string* error);

}  // namespace PJ::scripting
