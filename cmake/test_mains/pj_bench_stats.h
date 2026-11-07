// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Qt-free helpers shared by every benchmark: the calling thread's CPU clock and
// sample statistics. pj_bench_probes.h includes this header; runners that must not
// link Qt Widgets (pj_runtime_tests) include it directly.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <ctime>
#endif

namespace pj_bench {

/// CPU time of the calling thread (the GUI thread in every benchmark), ms.
inline double threadCpuMs() {
#ifdef _WIN32
  FILETIME creation{};
  FILETIME exit{};
  FILETIME kernel{};
  FILETIME user{};
  GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user);
  const auto ticks = [](const FILETIME& t) { return (std::uint64_t{t.dwHighDateTime} << 32U) | t.dwLowDateTime; };
  return static_cast<double>(ticks(kernel) + ticks(user)) / 1.0e4;  // 100 ns ticks
#else
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<double>(now.tv_sec) * 1.0e3 + static_cast<double>(now.tv_nsec) / 1.0e6;
#endif
}

/// Median, min, max, 90th percentile (nearest rank) and 95th (the sorted value
/// at index floor(0.95 n)) of `values` (0 when empty).
struct Stats {
  double median = 0.0;
  double min = 0.0;
  double max = 0.0;
  double p90 = 0.0;
  double p95 = 0.0;
};
inline Stats stats(std::vector<double> values) {
  if (values.empty()) {
    return {};
  }
  std::sort(values.begin(), values.end());
  const std::size_t last = values.size() - 1;
  const std::size_t p90 = std::min(last, (values.size() * 9 + 9) / 10 - 1);
  return {
      values[values.size() / 2], values.front(), values.back(), values[p90],
      values[std::min(last, values.size() * 95 / 100)]};
}

}  // namespace pj_bench
