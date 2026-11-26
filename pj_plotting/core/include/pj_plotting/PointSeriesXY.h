#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_series_data.h>

#include <QPointF>
#include <QRectF>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_plotting/RevisionedSeries.h"
#include "pj_runtime/CurveDescriptor.h"

namespace PJ {

class SessionManager;
struct TopicChunk;

class PointSeriesXY final : public QwtSeriesData<QPointF>, public RevisionedSeries {
 public:
  PointSeriesXY(SessionManager* session, CurveDescriptor x_source, CurveDescriptor y_source);

  std::size_t size() const override;
  QPointF sample(std::size_t index) const override;
  QRectF boundingRect() const override;
  void setRectOfInterest(const QRectF& rect) override;

  void onTopicCommitted();
  void onDataCleared();

  [[nodiscard]] const CurveDescriptor& xSource() const noexcept {
    return x_source_;
  }
  [[nodiscard]] const CurveDescriptor& ySource() const noexcept {
    return y_source_;
  }

  /// The (x, y) sample at or before `display_time_sec`, for the marker that follows the playback cursor along an XY
  /// curve. The display time is mapped back to a raw timestamp through the Y source's display offset (both axes share
  /// a time base) and matched against the time-ordered alignment index. nullopt when the series is empty or the
  /// cursor is before the first sample.
  [[nodiscard]] std::optional<QPointF> sampleFromTime(double display_time_sec) const;

 private:
  // Values, not rows of the store: the index is built under the engine lock, and sample() runs at paint time without
  // it, where a writer thread may be changing the topic's chunks.
  struct PairSlot {
    Timestamp time = 0;  // raw, of the Y sample
    QPointF point;
  };

  struct RowRef {
    const TopicChunk* chunk = nullptr;
    std::size_t row = 0;
  };

  void ensureAlignmentIndex() const;
  void buildSameTopicIndex() const;
  void buildDifferentTopicIndex() const;
  // Valid only while the caller holds the engine lock.
  [[nodiscard]] std::vector<RowRef> rowsFor(TopicId topic_id) const;

  SessionManager* session_ = nullptr;
  CurveDescriptor x_source_;
  CurveDescriptor y_source_;

  mutable std::vector<PairSlot> pairs_;
  mutable QRectF cached_bounding_rect_;
  mutable bool alignment_dirty_ = true;
  mutable bool bounding_rect_valid_ = false;
};

}  // namespace PJ
