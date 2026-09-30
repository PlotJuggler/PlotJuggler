#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPointF>
#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace PJ {

// A retained finite sample. starts_new_run is true after an input NaN/Inf gap
// (and for the first retained sample), so renderers never infer a line across a
// missing-data interval.
struct ReducedPlotSample {
  QPointF point;
  std::size_t source_index = 0;
  // Optional cumulative distance along the original mapped finite run. The
  // reducer itself leaves this at zero; annotatePlotSamplePathOffsets() fills
  // it when a renderer needs phase-stable path semantics.
  qreal source_path_offset = 0.0;
  bool starts_new_run = false;
};

struct PlotSampleReduction {
  std::vector<ReducedPlotSample> samples;
  std::size_t input_samples = 0;
  std::size_t finite_samples = 0;
  std::size_t input_runs = 0;
  std::size_t retained_runs = 0;
  std::size_t dropped_runs = 0;
  bool reduced = false;
};

using PlotSampleAccessor = std::function<QPointF(std::size_t)>;
using PlotSegmentLength = std::function<qreal(const QPointF&, const QPointF&)>;

// Reduces already-mapped (screen-space) samples without materializing the
// complete input series. Every retained run keeps its first/last point, while
// each bucket keeps its X and Y extrema in original source order.
//
// The finite-sample budget is a hard ceiling. If the endpoints of every input
// run cannot fit, a prefix of complete runs is retained while that run and all
// later runs are dropped wholesale. Partial runs are never emitted, so
// reduction cannot create a false connection across a missing-data gap.
//
// sample_at must be pure and stable for the duration of the call: the
// implementation scans the series in several independent passes and assumes
// they agree on which indices are finite and on each point's value. A
// non-deterministic accessor would desynchronize run boundaries.
[[nodiscard]] PlotSampleReduction reducePlotSamples(
    std::size_t sample_count, std::size_t sample_budget, std::size_t horizontal_bucket_count,
    const PlotSampleAccessor& sample_at);

// The same reduction over samples already mapped into memory (desktop canvases
// map each curve once and reduce the visible range), without a type-erased call
// per sample and scan.
[[nodiscard]] PlotSampleReduction reducePlotSamples(
    std::span<const QPointF> samples, std::size_t sample_budget, std::size_t horizontal_bucket_count);

// Annotates retained samples with cumulative distance measured over every
// original mapped sample, not over the reduced chords. Finite runs restart at
// zero. segment_length selects the source path metric: Euclidean for Lines and
// Manhattan for Steps. This is intentionally separate from reduction so solid
// curves do not pay another full input scan.
void annotatePlotSamplePathOffsets(
    PlotSampleReduction& reduction, const PlotSampleAccessor& sample_at, const PlotSegmentLength& segment_length);

}  // namespace PJ
