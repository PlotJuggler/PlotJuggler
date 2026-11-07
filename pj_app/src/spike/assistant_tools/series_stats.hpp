// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace assistant_agent {

// Summary statistics over one numeric time series. Timestamps are the raw int64
// nanoseconds-since-epoch the datastore stores; durations/rate are derived in
// seconds for a model-friendly report.
struct SeriesStats {
  std::size_t count = 0;
  std::size_t invalid = 0;  // samples whose value is not finite (NaN, ±inf)
  // True once at least one finite value was seen. min/max/mean/stddev are
  // computed over finite samples only and stay at their defaults (never NaN)
  // when this is false.
  bool has_values = false;
  double min = 0.0;
  double max = 0.0;
  // Where the finite min/max first occur, in seconds relative to the first
  // sample (same axis as max_gap_at_s and bucket `t`). First occurrence: a
  // later sample tying the extreme does not move it. Only meaningful when
  // has_values.
  double min_at_s = 0.0;
  double max_at_s = 0.0;
  double mean = 0.0;
  double stddev = 0.0;
  std::int64_t t_start_ns = 0;
  std::int64_t t_end_ns = 0;
  double duration_s = 0.0;
  double rate_hz = 0.0;  // (count - 1) / duration_s, 0 when undefined
  // Largest spacing between consecutive samples, and where the gap starts (in
  // seconds relative to the first sample, matching the buckets' time axis).
  // Only defined with two or more samples — see has_gap. This is what makes
  // "are there gaps in this recording?" answerable: count, mean and rate all
  // survive a dropout untouched, so without it the question invites a guess.
  bool has_gap = false;
  double max_gap_s = 0.0;
  double max_gap_at_s = 0.0;
};

// One decimated bucket: a time window collapsed to its min/max/mean. `t_rel_s`
// is the bucket's left edge in seconds relative to the first sample, so the
// model sees a compact, human-readable time axis.
struct SeriesBucket {
  double t_rel_s = 0.0;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  std::size_t count = 0;    // finite samples in this bucket
  std::size_t invalid = 0;  // samples whose value is not finite (NaN, ±inf)
};

// Compute summary stats. `timestamps` and `values` must be equal length and
// timestamps non-decreasing (as the datastore guarantees). Returns a zeroed
// SeriesStats for an empty input.
[[nodiscard]] inline SeriesStats computeStats(
    std::span<const std::int64_t> timestamps, std::span<const double> values) {
  SeriesStats s;
  const std::size_t n = std::min(timestamps.size(), values.size());
  if (n == 0) {
    return s;
  }
  s.count = n;
  double sum = 0.0;
  std::size_t finite_count = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double v = values[i];
    if (!std::isfinite(v)) {
      ++s.invalid;
      continue;
    }
    const double t_rel_s = static_cast<double>(timestamps[i] - timestamps[0]) * 1e-9;
    if (!s.has_values) {
      s.min = v;
      s.max = v;
      s.min_at_s = t_rel_s;
      s.max_at_s = t_rel_s;
      s.has_values = true;
    } else {
      if (v < s.min) {
        s.min = v;
        s.min_at_s = t_rel_s;
      }
      if (v > s.max) {
        s.max = v;
        s.max_at_s = t_rel_s;
      }
    }
    sum += v;
    ++finite_count;
  }
  if (s.has_values) {
    s.mean = sum / static_cast<double>(finite_count);
    double sq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double v = values[i];
      if (!std::isfinite(v)) {
        continue;
      }
      const double d = v - s.mean;
      sq += d * d;
    }
    s.stddev = std::sqrt(sq / static_cast<double>(finite_count));
  }
  s.t_start_ns = timestamps[0];
  s.t_end_ns = timestamps[n - 1];
  s.duration_s = static_cast<double>(s.t_end_ns - s.t_start_ns) * 1e-9;
  if (s.duration_s > 0.0 && n > 1) {
    s.rate_hz = static_cast<double>(n - 1) / s.duration_s;
  }
  if (n > 1) {
    std::int64_t max_gap_ns = 0;
    std::int64_t gap_at_ns = timestamps[0];
    for (std::size_t i = 1; i < n; ++i) {
      const std::int64_t gap = timestamps[i] - timestamps[i - 1];
      if (gap > max_gap_ns) {
        max_gap_ns = gap;
        gap_at_ns = timestamps[i - 1];
      }
    }
    s.has_gap = true;
    s.max_gap_s = static_cast<double>(max_gap_ns) * 1e-9;
    s.max_gap_at_s = static_cast<double>(gap_at_ns - s.t_start_ns) * 1e-9;
  }
  return s;
}

// Longest run of consecutive samples holding exactly the same value, plus
// whether the series never changes at all. Computed on the WHOLE series the
// caller passes in — the executor must call this on the unsliced ts/vals
// from readOne, BEFORE any t_start_s/t_end_s window is applied, because
// "this channel never changes" and "this channel froze for 21s" are claims
// about the entire recording, not about whatever slice a later window keeps.
struct FlatRunSummary {
  // True only when every sample equals the first. Exact `==`, so a series
  // containing any non-finite sample can't be constant unless it has just
  // one sample (nothing to compare it against).
  bool constant = false;
  // True when the longest flat run covers at least 5% of the series
  // duration and the series is not `constant` (a constant series already
  // says everything a flat span would add). Left false — with the span
  // fields at 0 — for a noisy series, a single-sample series, or one where
  // every sample shares the same timestamp (zero duration).
  bool has_flat_span = false;
  double flat_span_s = 0.0;
  // Where the longest run starts, in seconds relative to the first sample —
  // same axis as min_at_s/max_at_s/max_gap_at_s.
  double flat_span_at_s = 0.0;
};

[[nodiscard]] inline FlatRunSummary flatRunSummary(
    std::span<const std::int64_t> timestamps, std::span<const double> values) {
  FlatRunSummary out;
  const std::size_t n = std::min(timestamps.size(), values.size());
  if (n == 0) {
    return out;
  }
  if (n == 1) {
    out.constant = true;
    return out;
  }

  bool constant = true;
  std::size_t run_start = 0;
  bool run_open = false;
  double best_len_s = 0.0;
  double best_start_s = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double v = values[i];
    if (!std::isfinite(v)) {
      // A non-finite sample can't equal anything (NaN) and can't be trusted
      // as a repeated constant (inf) either: it breaks any run in progress
      // and rules out the whole series being constant.
      run_open = false;
      constant = false;
      continue;
    }
    if (i > 0 && !(v == values[i - 1])) {
      constant = false;
    }
    if (run_open && v == values[run_start]) {
      // extends the open run
    } else {
      run_start = i;
      run_open = true;
    }
    const double len_s = static_cast<double>(timestamps[i] - timestamps[run_start]) * 1e-9;
    if (len_s > best_len_s) {
      best_len_s = len_s;
      best_start_s = static_cast<double>(timestamps[run_start] - timestamps[0]) * 1e-9;
    }
  }

  if (constant) {
    out.constant = true;
    return out;
  }

  const double duration_s = static_cast<double>(timestamps[n - 1] - timestamps[0]) * 1e-9;
  if (duration_s > 0.0 && best_len_s >= 0.05 * duration_s) {
    out.has_flat_span = true;
    out.flat_span_s = best_len_s;
    out.flat_span_at_s = best_start_s;
  }
  return out;
}

// Decimate into at most `max_points` equal-width time buckets, preserving the
// per-bucket min and max so spikes survive the downsample (a plain stride would
// alias them away). Buckets with no samples at all are omitted; a bucket whose
// samples are all non-finite (NaN/inf) is kept with count == 0 and its invalid
// count set, so a window of bad data is reported rather than silently dropped.
// When the series already has <= max_points samples, each sample becomes its
// own bucket.
[[nodiscard]] inline std::vector<SeriesBucket> bucketize(
    std::span<const std::int64_t> timestamps, std::span<const double> values, std::size_t max_points) {
  std::vector<SeriesBucket> out;
  const std::size_t n = std::min(timestamps.size(), values.size());
  if (n == 0 || max_points == 0) {
    return out;
  }
  const std::int64_t t0 = timestamps[0];
  const std::int64_t t1 = timestamps[n - 1];
  const std::int64_t span_ns = t1 - t0;

  // Degenerate time span (all samples share a timestamp) or few enough samples:
  // emit one bucket per sample, capped at max_points.
  if (span_ns <= 0 || n <= max_points) {
    const std::size_t limit = std::min(n, max_points);
    out.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
      const double v = values[i];
      const double t_rel_s = static_cast<double>(timestamps[i] - t0) * 1e-9;
      if (std::isfinite(v)) {
        out.push_back({t_rel_s, v, v, v, 1, 0});
      } else {
        out.push_back({t_rel_s, 0.0, 0.0, 0.0, 0, 1});
      }
    }
    return out;
  }

  out.reserve(max_points);
  const double bucket_ns = static_cast<double>(span_ns) / static_cast<double>(max_points);
  std::size_t idx = 0;
  for (std::size_t b = 0; b < max_points && idx < n; ++b) {
    // Right edge of bucket b (last bucket absorbs the final sample via <= t1).
    const std::int64_t edge =
        (b + 1 == max_points) ? t1 : t0 + static_cast<std::int64_t>(static_cast<double>(b + 1) * bucket_ns);
    SeriesBucket bucket;
    bucket.t_rel_s =
        static_cast<double>(t0 + static_cast<std::int64_t>(static_cast<double>(b) * bucket_ns) - t0) * 1e-9;
    double sum = 0.0;
    bool first = true;
    while (idx < n && (timestamps[idx] <= edge || b + 1 == max_points)) {
      const double v = values[idx];
      if (!std::isfinite(v)) {
        ++bucket.invalid;
        ++idx;
        continue;
      }
      if (first) {
        bucket.min = v;
        bucket.max = v;
        first = false;
      } else {
        bucket.min = std::min(bucket.min, v);
        bucket.max = std::max(bucket.max, v);
      }
      sum += v;
      ++bucket.count;
      ++idx;
    }
    if (bucket.count > 0) {
      bucket.mean = sum / static_cast<double>(bucket.count);
      out.push_back(bucket);
    } else if (bucket.invalid > 0) {
      // All samples in this window were non-finite: keep the bucket (with its
      // invalid count) instead of silently dropping the window, but leave
      // min/max/mean at their defaults rather than emitting NaN.
      out.push_back(bucket);
    }
  }
  return out;
}

}  // namespace assistant_agent
