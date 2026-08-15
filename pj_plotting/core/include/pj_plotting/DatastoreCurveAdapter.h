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
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/Time.h"

namespace PJ {

class SessionManager;

// Not `final`: FilteredCurveAdapter derives from it so a filter-output preview
// rides the SAME samplesIngested -> onTopicCommitted() refresh path as the raw
// ghost (see FilteredCurveAdapter.h). The data accessors are already virtual via
// QwtSeriesData; the commit/clear hooks are made virtual for that subclass.
class DatastoreCurveAdapter : public QwtSeriesData<QPointF> {
 public:
  DatastoreCurveAdapter(SessionManager* session, CurveDescriptor source);
  ~DatastoreCurveAdapter() override = default;

  std::size_t size() const override;
  QPointF sample(std::size_t index) const override;
  QRectF boundingRect() const override;
  void setRectOfInterest(const QRectF& rect) override;

  // Y extent over the given display-x window — the fast path PlotWidget uses to
  // auto-fit the Y axis (it queries the store's per-range bounds rather than the
  // visible sample buffer). virtual so a derived curve (FilteredCurveAdapter)
  // reports its OWN output range instead of this raw input range.
  [[nodiscard]] virtual std::optional<Range<double>> visibleYRange(Range<double> x_range_sec) const;

  // Invalidate the cached sample index / bounds so the next read re-queries the
  // store. PlotWidget calls these from its samplesIngested / dataset-replace
  // handlers; a subclass overrides them to refresh its own derived cache too.
  virtual void onTopicCommitted();
  virtual void onDataCleared();

  /// Drop the cached display offset only (the X mapping moved; the samples did
  /// not). Unlike onTopicCommitted/onDataCleared, does NOT re-index samples.
  /// virtual: FilteredCurveAdapter bakes display X into its output points and
  /// must invalidate them on an offset move.
  virtual void onDisplayOffsetChanged();

  [[nodiscard]] const CurveDescriptor& source() const noexcept {
    return source_;
  }
  [[nodiscard]] std::optional<QPointF> sampleFromTime(double display_time_sec) const;

 protected:
  // The session a subclass needs to read the input column / resolve the display
  // offset. May be null (constructed without a session); callers null-check.
  [[nodiscard]] SessionManager* session() const noexcept {
    return session_;
  }

 private:
  // One servable point of the materialized window: plain values only — no
  // TopicChunk pointer, so streaming retention may free chunks with no
  // lifetime coupling to this cache.
  struct CachedSample {
    Timestamp timestamp;
    double value;
  };

  void ensureSampleWindow() const;
  // Absorb the series delta since the build into the cached window without a
  // rebuild: trim the retention-evicted prefix and append the strictly
  // in-order new samples. Valid ONLY when the reader's series generation
  // equals built_series_generation_. Returns false when the cache emptied and
  // a full rebuild is required after all.
  [[nodiscard]] bool absorbInOrderAppends(const SeriesReader& series) const;
  // Recompute window_offset_/window_count_ over the built window_samples_ for
  // the current visible window (two binary searches, no engine access). The
  // built range must cover the visible range.
  void narrowToVisibleWindow() const;
  [[nodiscard]] QPointF toDisplayPoint(Timestamp timestamp, double value) const;
  [[nodiscard]] QPointF readPoint(const SeriesSample& sample) const;
  [[nodiscard]] DisplayOffset displayOffsetNow() const;

  SessionManager* session_ = nullptr;
  CurveDescriptor source_;

  Timestamp visible_t_min_raw_ns_ = std::numeric_limits<Timestamp>::min();
  Timestamp visible_t_max_raw_ns_ = std::numeric_limits<Timestamp>::max();

  // Samples materialized over [built_t_min_, built_t_max_] — the visible span
  // widened by one span on each side, so zoom/pan steps that stay inside it
  // re-narrow with binary searches instead of re-walking the engine (that
  // rebuild was the top GUI-thread cost while zooming). The built range holds
  // one sample STRICTLY outside each end (when one exists), so every narrowed
  // window can serve its off-screen guard. size()/sample() serve the
  // [window_offset_, window_offset_+window_count_) slice: the visible samples
  // plus one guard per side for line continuity.
  mutable std::vector<CachedSample> window_samples_;
  mutable Timestamp built_t_min_raw_ns_ = 0;
  mutable Timestamp built_t_max_raw_ns_ = 0;
  mutable std::uint64_t built_series_generation_ = 0;
  mutable std::size_t window_offset_ = 0;
  mutable std::size_t window_count_ = 0;
  // window_dirty_: the visible window moved (often servable from the built
  // vector). data_dirty_: the series content changed (commit/clear/retention),
  // so the cached values may be stale and a full rebuild is required.
  mutable bool window_dirty_ = true;
  mutable bool data_dirty_ = true;

  mutable QRectF cached_full_bounding_rect_;
  mutable bool full_bounding_rect_valid_ = false;

  // Cached display-time offset. Resolved live on first use after an
  // invalidation; invalidated in onTopicCommitted/onDataCleared/onDisplayOffsetChanged
  // so it tracks time-domain reconfiguration through the same signals that drive
  // sample re-indexing (or, for a pure offset change, without re-indexing).
  // Removing this cache makes readPoint_() pay 2 DataEngine lookups per sample,
  // which dominates per-curve paint cost.
  mutable DisplayOffset cached_display_offset_;
  mutable bool cached_display_offset_valid_ = false;
};

}  // namespace PJ
