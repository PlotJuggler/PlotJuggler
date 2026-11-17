#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_series_data.h>

#include <QPointF>
#include <QRectF>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_datastore/query.hpp"
#include "pj_plotting/RevisionedSeries.h"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/Time.h"

namespace PJ {

class SessionManager;

// Not final: FilteredCurveAdapter derives from it to ride the same samplesIngested -> onTopicCommitted() refresh as
// the raw curve.
class DatastoreCurveAdapter : public QwtSeriesData<QPointF>, public RevisionedSeries {
 public:
  DatastoreCurveAdapter(SessionManager* session, CurveDescriptor source);
  ~DatastoreCurveAdapter() override = default;

  std::size_t size() const override;
  QPointF sample(std::size_t index) const override;
  QRectF boundingRect() const override;
  void setRectOfInterest(const QRectF& rect) override;

  // Y extent over the display-x window, from the store's per-range bounds rather than the visible sample buffer: the Y
  // auto-fit path of PlotWidget. Virtual so FilteredCurveAdapter reports its own output range.
  [[nodiscard]] virtual std::optional<Range<double>> visibleYRange(Range<double> x_range_sec) const;

  // Invalidate the cached window and bounds so the next read re-queries the store. PlotWidget calls them from its
  // samplesIngested and dataset-replace handlers; a subclass refreshes its own derived cache too.
  virtual void onTopicCommitted();
  virtual void onDataCleared();

  /// Drops only the cached display offset (the X mapping moved, the samples did not), so nothing is re-indexed.
  /// Virtual: FilteredCurveAdapter bakes display X into its points.
  virtual void onDisplayOffsetChanged();

  [[nodiscard]] const CurveDescriptor& source() const noexcept {
    return source_;
  }
  [[nodiscard]] std::optional<QPointF> sampleFromTime(double display_time_sec) const;

 protected:
  // May be null (a series built without a session); callers check.
  [[nodiscard]] SessionManager* session() const noexcept {
    return session_;
  }

 private:
  // Plain values, no TopicChunk pointer: retention can free chunks without touching this cache.
  struct CachedSample {
    Timestamp timestamp;
    double value;
  };

  void ensureSampleWindow() const;
  // Absorbs the series delta since the build (trims the retention-evicted prefix, appends the strictly in-order new
  // samples) without a rebuild. Valid only when the reader's series generation equals built_series_generation_; false
  // when the cache emptied and a full rebuild is needed.
  [[nodiscard]] bool absorbInOrderAppends(const SeriesReader& series) const;
  // Recomputes window_offset_ / window_count_ over window_samples_ for the visible window (two binary searches, no
  // engine access); the built range must cover it.
  void narrowToVisibleWindow() const;
  [[nodiscard]] QPointF toDisplayPoint(Timestamp timestamp, double value) const;
  [[nodiscard]] QPointF readPoint(const SeriesSample& sample) const;
  [[nodiscard]] DisplayOffset displayOffsetNow() const;

  SessionManager* session_ = nullptr;
  CurveDescriptor source_;

  Timestamp visible_t_min_raw_ns_ = std::numeric_limits<Timestamp>::min();
  Timestamp visible_t_max_raw_ns_ = std::numeric_limits<Timestamp>::max();

  // Samples over [built_t_min_, built_t_max_]: the visible span widened by one span per side, so zoom and pan steps
  // inside it re-narrow by binary search instead of re-walking the engine (pj_plotting/docs/TECHNICAL_NOTES.md
  // "Window cache"), plus one sample strictly outside each end (when one exists) so any narrowed window can serve its
  // off-screen guard.
  // size()/sample() serve [window_offset_, window_offset_ + window_count_): the visible samples plus one guard per
  // side for line continuity.
  mutable std::vector<CachedSample> window_samples_;
  mutable Timestamp built_t_min_raw_ns_ = 0;
  mutable Timestamp built_t_max_raw_ns_ = 0;
  mutable std::uint64_t built_series_generation_ = 0;
  mutable std::size_t window_offset_ = 0;
  mutable std::size_t window_count_ = 0;
  // window_dirty_: the visible window moved (often servable from the built vector). data_dirty_: the content changed
  // (commit, clear, retention), so a full rebuild is needed.
  mutable bool window_dirty_ = true;
  mutable bool data_dirty_ = true;

  mutable QRectF cached_full_bounding_rect_;
  mutable bool full_bounding_rect_valid_ = false;

  // Resolved live on first use after an invalidation (onTopicCommitted, onDataCleared, onDisplayOffsetChanged). Without
  // it readPoint pays two DataEngine lookups per sample (pj_plotting/docs/TECHNICAL_NOTES.md "Window cache").
  mutable DisplayOffset cached_display_offset_;
  mutable bool cached_display_offset_valid_ = false;
};

}  // namespace PJ
