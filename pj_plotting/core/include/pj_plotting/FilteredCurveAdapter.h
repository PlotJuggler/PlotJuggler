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

/// A lazy, datastore-backed curve that serves the OUTPUT of a `proc::DataProcessor`
/// run over an input column — the "after" curve in the Filter Editor preview.
///
/// Why a `DatastoreCurveAdapter` subclass rather than an in-memory point series:
/// it reports the INPUT topic as its `source()`, so `PlotWidget`'s existing
/// `samplesIngested` handler (which iterates every `DatastoreCurveAdapter` and
/// calls `onTopicCommitted()`) refreshes it on the SAME signal that already moves
/// the raw ghost. No panel-side `samplesIngested` connection and no throttle timer
/// are needed — the filtered curve tracks streaming data because it rides the
/// ghost's refresh path. The pull/lazy model also means the recompute happens once
/// per paint, coalescing bursts of commits for free.
///
/// The filter is rebuilt FRESH on every recompute via `factory_` (processors are
/// stateful, so a preview must not reuse an accumulator across recomputes) and run
/// over the WHOLE input column (matching Apply and the previous preview). x is in
/// display seconds via the same `SessionManager::displayOffset` seam the ghost uses,
/// so filtered and ghost stay aligned across the "Use time offset" toggle.
class FilteredCurveAdapter : public DatastoreCurveAdapter {
 public:
  /// Builds a fresh processor for one recompute, or nullptr for "No Transform"
  /// (-> an empty curve). Called on each lazy refresh, so it reads the current
  /// parameter configuration; capture by reference to the owner with care that
  /// the owner outlives the adapter (the panel owns the plot that owns this).
  using ProcessorFactory = std::function<std::unique_ptr<proc::DataProcessor>()>;

  /// `input` is the source column to filter; its `topic_id` is what the commit
  /// handler matches against, so it must be the REAL input topic.
  FilteredCurveAdapter(SessionManager* session, CurveDescriptor input, ProcessorFactory factory);

  std::size_t size() const override;
  QPointF sample(std::size_t index) const override;
  QRectF boundingRect() const override;

  // Y extent of the FILTERED output (PlotWidget's auto-fit path). The base version
  // would report the raw input range, mis-fitting the Y axis for a filter that
  // changes amplitude (e.g. a binary filter whose output reaches 1). The cache is
  // built once per recompute, so this is O(1); the x-window is ignored (the whole
  // in-memory series is previewed, matching the old in-memory curve's full-range fit).
  [[nodiscard]] std::optional<Range<double>> visibleYRange(Range<double> x_range_sec) const override;

  // Recompute is lazy: these just mark the cache stale; the next read rebuilds it.
  void onTopicCommitted() override;
  void onDataCleared() override;

  // filtered_ bakes display X into its points, so an offset move must
  // invalidate it (the base class only refreshes its raw-timestamp caches).
  void onDisplayOffsetChanged() override;

  /// Mark the filtered cache stale because the FILTER changed (e.g. the user edited
  /// a parameter), even though no new samples arrived. Distinct from
  /// `onTopicCommitted()` only in intent; both defer the recompute to the next read.
  void invalidate();

 private:
  // Rebuilds `filtered_` and its cached bounds (rect + Y range) from the input
  // column through a fresh processor, when dirty. const because it feeds the const
  // Qwt accessors. After it returns, the caches below match `filtered_`.
  void ensureFiltered() const;

  ProcessorFactory factory_;
  mutable std::vector<QPointF> filtered_;
  mutable QRectF cached_bounding_rect_;
  mutable Range<double> filtered_y_range_;  // valid iff !filtered_.empty()
  mutable bool filtered_dirty_ = true;
};

}  // namespace PJ
