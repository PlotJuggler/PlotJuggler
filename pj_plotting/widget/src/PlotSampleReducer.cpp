// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotSampleReducer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace PJ {
namespace {

struct FiniteRun {
  std::size_t begin = 0;
  std::size_t end = 0;

  [[nodiscard]] std::size_t size() const noexcept {
    return end - begin;
  }

  [[nodiscard]] std::size_t endpointCost() const noexcept {
    return size() == 1 ? 1 : 2;
  }

  [[nodiscard]] std::size_t interiorSize() const noexcept {
    return size() > 2 ? size() - 2 : 0;
  }
};

[[nodiscard]] bool finitePoint(const QPointF& point) {
  return std::isfinite(point.x()) && std::isfinite(point.y());
}

template <typename Accessor>
void appendRunEndpoints(PlotSampleReduction& result, const FiniteRun& run, const Accessor& sample_at) {
  result.samples.push_back({.point = sample_at(run.begin), .source_index = run.begin, .starts_new_run = true});
  if (run.size() > 1) {
    result.samples.push_back({.point = sample_at(run.end - 1), .source_index = run.end - 1, .starts_new_run = false});
  }
  ++result.retained_runs;
}

[[nodiscard]] std::vector<std::size_t> allocateBuckets(const std::vector<FiniteRun>& runs, std::size_t bucket_budget) {
  std::vector<std::size_t> allocation(runs.size(), 0);
  const std::size_t total_interior = std::accumulate(
      runs.begin(), runs.end(), std::size_t{0},
      [](std::size_t total, const FiniteRun& run) { return total + run.interiorSize(); });
  if (bucket_budget == 0 || total_interior == 0) {
    return allocation;
  }

  struct Remainder {
    std::size_t run = 0;
    std::uint64_t numerator = 0;
  };
  std::vector<Remainder> remainders;
  remainders.reserve(runs.size());
  std::size_t assigned = 0;
  for (std::size_t index = 0; index < runs.size(); ++index) {
    const std::size_t interior = runs[index].interiorSize();
    // WASM size_t is 32-bit. A normal multi-million-sample series overflows if
    // this apportionment multiplies in size_t, so widen both operands first.
    const std::uint64_t numerator = static_cast<std::uint64_t>(bucket_budget) * static_cast<std::uint64_t>(interior);
    allocation[index] =
        std::min(interior, static_cast<std::size_t>(numerator / static_cast<std::uint64_t>(total_interior)));
    assigned += allocation[index];
    remainders.push_back({index, numerator % static_cast<std::uint64_t>(total_interior)});
  }
  std::stable_sort(remainders.begin(), remainders.end(), [](const Remainder& lhs, const Remainder& rhs) {
    return lhs.numerator > rhs.numerator;
  });
  for (const Remainder& remainder : remainders) {
    if (assigned >= bucket_budget) {
      break;
    }
    if (allocation[remainder.run] < runs[remainder.run].interiorSize()) {
      ++allocation[remainder.run];
      ++assigned;
    }
  }
  return allocation;
}

template <typename Accessor>
void appendReducedRun(
    PlotSampleReduction& result, const FiniteRun& run, std::size_t bucket_count, const Accessor& sample_at) {
  std::vector<std::size_t> selected;
  selected.reserve(2 + bucket_count * 4);
  selected.push_back(run.begin);

  const std::size_t interior_begin = run.begin + 1;
  const std::size_t interior_size = run.interiorSize();
  for (std::size_t bucket = 0; bucket < bucket_count; ++bucket) {
    const std::size_t begin = interior_begin + static_cast<std::size_t>(
                                                   (static_cast<std::uint64_t>(interior_size) * bucket) /
                                                   static_cast<std::uint64_t>(bucket_count));
    const std::size_t end = interior_begin + static_cast<std::size_t>(
                                                 (static_cast<std::uint64_t>(interior_size) * (bucket + 1)) /
                                                 static_cast<std::uint64_t>(bucket_count));
    if (begin >= end) {
      continue;
    }
    std::size_t min_x = begin;
    std::size_t max_x = begin;
    std::size_t min_y = begin;
    std::size_t max_y = begin;
    QPointF min_x_point = sample_at(begin);
    QPointF max_x_point = min_x_point;
    QPointF min_y_point = min_x_point;
    QPointF max_y_point = min_x_point;
    for (std::size_t index = begin + 1; index < end; ++index) {
      const QPointF point = sample_at(index);
      if (point.x() < min_x_point.x()) {
        min_x = index;
        min_x_point = point;
      }
      if (point.x() > max_x_point.x()) {
        max_x = index;
        max_x_point = point;
      }
      if (point.y() < min_y_point.y()) {
        min_y = index;
        min_y_point = point;
      }
      if (point.y() > max_y_point.y()) {
        max_y = index;
        max_y_point = point;
      }
    }
    selected.push_back(min_x);
    selected.push_back(max_x);
    selected.push_back(min_y);
    selected.push_back(max_y);
  }
  if (run.size() > 1) {
    selected.push_back(run.end - 1);
  }
  std::sort(selected.begin(), selected.end());
  selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
  for (std::size_t index : selected) {
    result.samples.push_back({.point = sample_at(index), .source_index = index, .starts_new_run = index == run.begin});
  }
  ++result.retained_runs;
}

// One instantiation per accessor type, so a plain lambda over a vector is
// inlined into the scans instead of paying a std::function call per sample.
template <typename Accessor>
PlotSampleReduction reduceSamples(
    std::size_t sample_count, std::size_t sample_budget, std::size_t horizontal_bucket_count,
    const Accessor& sample_at) {
  PlotSampleReduction result;
  result.input_samples = sample_count;
  if (sample_count == 0) {
    result.reduced = sample_count > 0;
    return result;
  }

  std::size_t endpoint_cost = 0;
  std::size_t total_interior = 0;
  std::size_t current_run_size = 0;
  const auto finish_run = [&]() {
    if (current_run_size == 0) {
      return;
    }
    ++result.input_runs;
    endpoint_cost += current_run_size == 1 ? 1 : 2;
    total_interior += current_run_size > 2 ? current_run_size - 2 : 0;
    current_run_size = 0;
  };
  for (std::size_t index = 0; index < sample_count; ++index) {
    const bool finite = finitePoint(sample_at(index));
    if (finite) {
      ++result.finite_samples;
      ++current_run_size;
    } else {
      finish_run();
    }
  }
  finish_run();
  if (result.input_runs == 0) {
    result.reduced = sample_count > 0;
    return result;
  }

  if (result.finite_samples <= sample_budget) {
    result.samples.reserve(result.finite_samples);
    bool in_run = false;
    for (std::size_t index = 0; index < sample_count; ++index) {
      const QPointF point = sample_at(index);
      if (finitePoint(point)) {
        result.samples.push_back({.point = point, .source_index = index, .starts_new_run = !in_run});
        in_run = true;
      } else {
        in_run = false;
      }
    }
    result.retained_runs = result.input_runs;
    return result;
  }
  result.reduced = true;

  if (endpoint_cost > sample_budget) {
    result.samples.reserve(sample_budget);
    std::size_t remaining = sample_budget;
    bool budget_exhausted = false;
    std::size_t run_begin = 0;
    std::size_t run_size = 0;
    const auto emit_run = [&]() {
      if (run_size == 0 || budget_exhausted) {
        run_size = 0;
        return;
      }
      const FiniteRun run{run_begin, run_begin + run_size};
      if (run.endpointCost() > remaining) {
        // Keep a prefix of complete runs. Once one run cannot fit, all later
        // runs are dropped as documented; no small later singleton is packed
        // into the leftover slot.
        budget_exhausted = true;
        run_size = 0;
        return;
      }
      appendRunEndpoints(result, run, sample_at);
      remaining -= run.endpointCost();
      run_size = 0;
    };
    for (std::size_t index = 0; index < sample_count && !budget_exhausted; ++index) {
      if (finitePoint(sample_at(index))) {
        if (run_size == 0) {
          run_begin = index;
        }
        ++run_size;
      } else {
        emit_run();
      }
    }
    emit_run();
    result.dropped_runs = result.input_runs - result.retained_runs;
    return result;
  }

  // Only materialize run descriptors after proving their endpoint cost fits
  // the output budget. This bounds auxiliary run memory by O(sample_budget)
  // even for alternating finite/NaN input with millions of gaps.
  std::vector<FiniteRun> runs;
  runs.reserve(result.input_runs);
  bool in_run = false;
  for (std::size_t index = 0; index < sample_count; ++index) {
    if (finitePoint(sample_at(index))) {
      if (!in_run) {
        runs.push_back({index, index + 1});
        in_run = true;
      } else {
        runs.back().end = index + 1;
      }
    } else {
      in_run = false;
    }
  }
  const std::size_t extrema_slots = sample_budget - endpoint_cost;
  const std::size_t bucket_budget = std::min({horizontal_bucket_count, extrema_slots / 4, total_interior});
  const std::vector<std::size_t> buckets = allocateBuckets(runs, bucket_budget);
  result.samples.reserve(endpoint_cost + bucket_budget * 4);
  for (std::size_t index = 0; index < runs.size(); ++index) {
    appendReducedRun(result, runs[index], buckets[index], sample_at);
  }
  return result;
}

}  // namespace

PlotSampleReduction reducePlotSamples(
    std::size_t sample_count, std::size_t sample_budget, std::size_t horizontal_bucket_count,
    const PlotSampleAccessor& sample_at) {
  if (!sample_at) {
    PlotSampleReduction result;
    result.input_samples = sample_count;
    result.reduced = sample_count > 0;
    return result;
  }
  return reduceSamples(sample_count, sample_budget, horizontal_bucket_count, sample_at);
}

PlotSampleReduction reducePlotSamples(
    std::span<const QPointF> samples, std::size_t sample_budget, std::size_t horizontal_bucket_count) {
  return reduceSamples(
      samples.size(), sample_budget, horizontal_bucket_count, [samples](std::size_t index) { return samples[index]; });
}

void annotatePlotSamplePathOffsets(
    PlotSampleReduction& reduction, const PlotSampleAccessor& sample_at, const PlotSegmentLength& segment_length) {
  if (reduction.samples.empty() || !sample_at || !segment_length) {
    return;
  }
  std::size_t retained_index = 0;
  QPointF previous;
  qreal path_offset = 0.0;
  bool in_run = false;
  for (std::size_t source_index = 0;
       source_index < reduction.input_samples && retained_index < reduction.samples.size(); ++source_index) {
    const QPointF point = sample_at(source_index);
    if (!finitePoint(point)) {
      in_run = false;
      path_offset = 0.0;
      continue;
    }
    if (!in_run) {
      path_offset = 0.0;
      in_run = true;
    } else {
      const qreal distance = segment_length(previous, point);
      if (std::isfinite(distance) && distance > 0.0) {
        path_offset += distance;
      }
    }
    while (retained_index < reduction.samples.size() &&
           reduction.samples[retained_index].source_index == source_index) {
      reduction.samples[retained_index].source_path_offset = path_offset;
      ++retained_index;
    }
    previous = point;
  }
}

}  // namespace PJ
