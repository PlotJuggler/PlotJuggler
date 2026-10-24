// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace PJ::scripting {

/// Largest script source an engine compiles: the compilers run on the host heap, outside a script's caps.
inline constexpr std::size_t kMaxSourceBytes = std::size_t{1} << 20;

/// Per-VM execution budget for an untrusted filter script. Filters run IN-PROCESS
/// on the commit thread, so a runaway loop or unbounded allocation would stall
/// ingest/render or exhaust memory — these limits make that a recoverable error
/// instead. Capabilities are handled separately by `luaL_sandbox` (read-only
/// globals) + Luau's already-curated stdlib (no io/loadstring/package by default).
struct BudgetLimits {
  /// Watchdog "fuel": one unit is spent per VM safepoint (loop back-edge,
  /// call/return). When it reaches zero mid-execution the script is aborted with
  /// a catchable error. Armed per protected region.
  std::uint64_t call_fuel = 5'000'000;    ///< budget for a single calculate()
  std::uint64_t setup_fuel = 20'000'000;  ///< budget for module-eval + create() (NOT luau_compile,
                                          ///< which runs on the host heap — that is bounded by source size)

  /// Peak live bytes a single VM may hold; the allocator returns null past this,
  /// which Luau surfaces as a catchable out-of-memory error.
  std::size_t mem_bytes = 64ull << 20;  ///< 64 MiB / VM

  /// Python: the wall clock of one on-demand run, and of a filter's module evaluation plus `create()`. The timeout
  /// lands at the next bytecode, never inside a native call.
  std::uint64_t python_watchdog_millis = 2000;
  /// Python filters: the wall clock of one `calculate()` or `reset()`, which runs on the GUI thread once per row.
  std::uint64_t python_call_millis = 250;
};

}  // namespace PJ::scripting
