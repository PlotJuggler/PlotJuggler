#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPointF>
#include <QRectF>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "pj_datastore/data_processor.hpp"
#include "pj_plotting/DatastoreCurveAdapter.h"
#include "pj_runtime/CurveDescriptor.h"

namespace PJ {

class SessionManager;

/// A lazy, datastore-backed curve serving the output of a `proc::DataProcessor` run over an input column: the "after"
/// curve of the Filter Editor preview.
///
/// It derives from `DatastoreCurveAdapter` and reports the input topic as its `source()`, so `PlotWidget`'s
/// `samplesIngested` handler (which calls `onTopicCommitted()` on every `DatastoreCurveAdapter`) refreshes it on the
/// same signal as the raw curve: the panel needs no connection or throttle timer, and the lazy recompute happens once
/// per paint, coalescing bursts of commits.
///
/// The filter is rebuilt fresh on every recompute through `factory_` (processors are stateful, so an accumulator must
/// not carry over) and runs over the whole input column. x is in display seconds through the same
/// `SessionManager::displayOffset` seam as the raw curve, so the two stay aligned across the "Use time offset" toggle.
class FilteredCurveAdapter : public DatastoreCurveAdapter {
 public:
  /// Builds a fresh processor for one recompute, or nullptr for "No Transform" (an empty curve). Called on each lazy
  /// refresh, so it reads the current parameter configuration; the owner it captures must outlive the adapter (the
  /// panel owns the plot that owns this).
  using ProcessorFactory = std::function<std::unique_ptr<proc::DataProcessor>()>;

  /// `input` is the column to filter; its `topic_id` is what the commit handler matches, so it must be the real input
  /// topic.
  FilteredCurveAdapter(SessionManager* session, CurveDescriptor input, ProcessorFactory factory);

  std::size_t size() const override;
  QPointF sample(std::size_t index) const override;
  QRectF boundingRect() const override;

  // Y extent of the filtered output: the base version would report the raw input range and mis-fit the axis for a
  // filter that changes amplitude. O(1) (the cache is built once per recompute); the x window is ignored, the whole
  // series being previewed.
  [[nodiscard]] std::optional<Range<double>> visibleYRange(Range<double> x_range_sec) const override;

  // Recompute is lazy: these only mark the cache stale.
  void onTopicCommitted() override;
  void onDataCleared() override;

  // filtered_ bakes display X into its points, so an offset move invalidates it (the base class refreshes only its
  // raw-timestamp caches).
  void onDisplayOffsetChanged() override;

  /// Marks the cache stale because the filter changed (a parameter edit) with no new samples; the recompute is deferred
  /// to the next read, as for `onTopicCommitted()`.
  void invalidate();

 private:
  // Rebuilds `filtered_` and its cached bounds (rect, Y range) through a fresh processor when dirty. const because it
  // feeds the const Qwt accessors.
  void ensureFiltered() const;

  ProcessorFactory factory_;
  mutable std::vector<QPointF> filtered_;
  mutable QRectF cached_bounding_rect_;
  mutable Range<double> filtered_y_range_;  // valid iff !filtered_.empty()
  mutable bool filtered_dirty_ = true;
};

}  // namespace PJ
