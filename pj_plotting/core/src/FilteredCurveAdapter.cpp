// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/FilteredCurveAdapter.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/sample.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {
namespace {

[[nodiscard]] QPointF invalidPoint() {
  return {0.0, std::numeric_limits<double>::quiet_NaN()};
}

// Empty/sentinel rect (negative size) — matches DatastoreCurveAdapter so Qwt
// treats an empty filtered curve identically to an empty raw one.
[[nodiscard]] QRectF invalidRect() {
  return {1.0, 1.0, -2.0, -2.0};
}

// VarValue (int64/uint64/double/string) -> double for plotting; strings plot as 0.
[[nodiscard]] double toDouble(const VarValue& value) {
  return std::visit(
      [](auto&& held) -> double {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return 0.0;
        } else {
          return static_cast<double>(held);
        }
      },
      value);
}

}  // namespace

FilteredCurveAdapter::FilteredCurveAdapter(SessionManager* session, CurveDescriptor input, ProcessorFactory factory)
    : DatastoreCurveAdapter(session, std::move(input)), factory_(std::move(factory)) {}

std::size_t FilteredCurveAdapter::size() const {
  ensureFiltered();
  return filtered_.size();
}

QPointF FilteredCurveAdapter::sample(std::size_t index) const {
  ensureFiltered();
  if (index >= filtered_.size()) {
    return invalidPoint();
  }
  return filtered_[index];
}

QRectF FilteredCurveAdapter::boundingRect() const {
  ensureFiltered();
  return cached_bounding_rect_;
}

std::optional<Range<double>> FilteredCurveAdapter::visibleYRange(Range<double> /*x_range_sec*/) const {
  ensureFiltered();
  // Gate on the bounds being valid, not merely on having points: an all-NaN filter
  // leaves points in filtered_ but no finite bounds (invalid rect), and filtered_y_range_
  // is only meaningful when finite bounds were found.
  if (!cached_bounding_rect_.isValid()) {
    return std::nullopt;
  }
  return filtered_y_range_;
}

void FilteredCurveAdapter::onTopicCommitted() {
  invalidate();
}

void FilteredCurveAdapter::onDataCleared() {
  invalidate();
  filtered_.clear();
  cached_bounding_rect_ = invalidRect();
}

void FilteredCurveAdapter::onDisplayOffsetChanged() {
  // Base first: it drops the cached offset that ensureFiltered() converts
  // through; then the baked display-X points must be rebuilt.
  DatastoreCurveAdapter::onDisplayOffsetChanged();
  invalidate();
}

void FilteredCurveAdapter::invalidate() {
  filtered_dirty_ = true;
}

void FilteredCurveAdapter::ensureFiltered() const {
  if (!filtered_dirty_) {
    return;
  }
  filtered_dirty_ = false;
  filtered_.clear();
  cached_bounding_rect_ = invalidRect();

  SessionManager* sess = session();
  if (sess == nullptr || !factory_) {
    return;
  }
  // Fresh processor per recompute: processors are stateful (integral/derivative),
  // so reusing one across recomputes would leak an accumulator into the preview.
  std::unique_ptr<proc::DataProcessor> processor = factory_();
  if (!processor) {
    return;  // "No Transform" -> empty overlay.
  }

  const CurveDescriptor& input = source();

  // Read the WHOLE input column through the SAME reader the ghost uses, so the
  // filtered "after" spans exactly the ghost's domain: identical sealed chunks,
  // retention floor, and null-skipping. Reading the raw chunks directly would let
  // the two curves disagree (e.g. the filtered tail lagging behind, or null rows
  // counted as 0). All-rows range; the processor covers the whole series.
  auto series_or = sess->createReader().series(input.topic_id, input.column_index);
  if (!series_or.has_value()) {
    return;
  }
  std::vector<proc::Sample> samples;
  series_or
      ->samples(
          Range<Timestamp>{.min = std::numeric_limits<Timestamp>::min(), .max = std::numeric_limits<Timestamp>::max()})
      .forEach([&](const SeriesSample& row) {
        samples.push_back(proc::Sample::scalar(row.timestamp, VarValue(row.value)));
      });
  if (samples.empty()) {
    return;
  }

  // x in display seconds via the SAME seam the ghost uses (SessionManager::
  // displayOffset -> TimeDomain shift + the per-dataset "Use time offset" shift),
  // so filtered and ghost stay aligned across the t0 toggle. Bounds are folded in
  // this one pass so boundingRect()/visibleYRange() stay O(1) on the auto-fit path.
  const DisplayOffset offset = sess->displayOffset(input.dataset_id);
  const std::vector<proc::Sample> out = processor->applyBatch(samples);
  filtered_.reserve(out.size());
  double x_min = std::numeric_limits<double>::max();
  double x_max = std::numeric_limits<double>::lowest();
  double y_min = std::numeric_limits<double>::max();
  double y_max = std::numeric_limits<double>::lowest();
  bool found_finite = false;  // did any sample contribute a finite (x,y) to the bounds?
  for (const proc::Sample& sample : out) {
    const double secs = toAxisDouble(rawToDisplaySeconds(sample.raw_ts_ns, offset));
    const double value = toDouble(sample.value());
    filtered_.push_back(QPointF(secs, value));
    // Only finite points define the bounds. NaN/inf are ignored by std::min/std::max
    // (comparisons are false), so folding them in would leave the sentinels untouched
    // and yield an inverted range (min > max) that corrupts auto-fit. Mirrors
    // PointSeriesXY::updateFiniteRange.
    if (std::isfinite(secs) && std::isfinite(value)) {
      x_min = std::min(x_min, secs);
      x_max = std::max(x_max, secs);
      y_min = std::min(y_min, value);
      y_max = std::max(y_max, value);
      found_finite = true;
    }
  }
  if (!found_finite) {
    return;  // no finite point (all rows suppressed or NaN): leave bounds invalid.
  }
  // Same (x_min,y_min)->(x_max,y_max) construction as DatastoreCurveAdapter, so
  // top()/bottom() carry the value min/max consistently across both curves.
  cached_bounding_rect_ = QRectF(QPointF(x_min, y_min), QPointF(x_max, y_max)).normalized();
  filtered_y_range_ = Range<double>{.min = y_min, .max = y_max};
}

}  // namespace PJ
