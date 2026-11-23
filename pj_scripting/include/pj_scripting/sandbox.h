// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace PJ::scripting {

/// Largest script source an engine compiles: the compilers run on the host heap, outside a script's caps.
inline constexpr std::size_t kMaxSourceBytes = std::size_t{1} << 20;

/// Per-VM execution budget of a script. Filters run in-process on the GUI thread, so a runaway loop or unbounded
/// allocation would stall the app: these limits turn that into a recoverable error.
struct BudgetLimits {
  /// Luau watchdog fuel: one unit per VM safepoint (loop back-edge, call/return), armed per protected region. At zero
  /// the script is aborted with an error a script `pcall` can catch, which is why the host also checks the sticky trip.
  std::uint64_t call_fuel = 5'000'000;    ///< a single calculate()
  std::uint64_t setup_fuel = 20'000'000;  ///< module evaluation + create(); luau_compile is bounded by kMaxSourceBytes
  /// Marker rules: fuel added per sample of each distinct series a rule opens, once per series, on top of
  /// `setup_fuel`, so a rule's budget grows with the data it scans and a runaway still ends. Sized from the measured
  /// per-sample cost of a scan (TECHNICAL_NOTES "Marker rule fuel").
  std::uint64_t series_sample_fuel = 8;

  /// Peak live bytes of one Luau VM; past it the allocator fails and Luau raises an out-of-memory error.
  std::size_t mem_bytes = 64ull << 20;

  /// Python: the wall clock of one on-demand run, and of a filter's module evaluation plus `create()`. The timeout
  /// lands at the next bytecode, never inside a native call.
  std::uint64_t python_watchdog_millis = 2000;
  /// Python filters: the wall clock of one `calculate()` or `reset()`, which runs on the GUI thread once per row.
  std::uint64_t python_call_millis = 250;
};

}  // namespace PJ::scripting
