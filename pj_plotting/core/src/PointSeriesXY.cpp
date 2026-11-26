// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PointSeriesXY.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

#include "pj_datastore/chunk.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {
namespace {

[[nodiscard]] QRectF invalidRect() {
  return {1.0, 1.0, -2.0, -2.0};
}

[[nodiscard]] QPointF invalidPoint() {
  return {0.0, std::numeric_limits<double>::quiet_NaN()};
}

[[nodiscard]] bool isBoolColumn(const TopicChunk& chunk, std::size_t column_index) {
  return column_index < chunk.columns.size() && chunk.columns[column_index].descriptor &&
         chunk.columns[column_index].descriptor->logical_type == PrimitiveType::kBool;
}

[[nodiscard]] double readY(const TopicChunk& chunk, std::size_t column_index, std::size_t row) {
  if (column_index >= chunk.columns.size() || chunk.isNull(column_index, row)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (isBoolColumn(chunk, column_index)) {
    return chunk.readBool(column_index, row) ? 1.0 : 0.0;
  }
  return chunk.readNumericAsDouble(column_index, row);
}

void updateFiniteRange(double value, double& min_value, double& max_value, bool& found_value) {
  if (!std::isfinite(value)) {
    return;
  }
  min_value = found_value ? std::min(min_value, value) : value;
  max_value = found_value ? std::max(max_value, value) : value;
  found_value = true;
}

}  // namespace

PointSeriesXY::PointSeriesXY(SessionManager* session, CurveDescriptor x_source, CurveDescriptor y_source)
    : session_(session),
      x_source_(std::move(x_source)),
      y_source_(std::move(y_source)),
      cached_bounding_rect_(invalidRect()) {}

std::size_t PointSeriesXY::size() const {
  ensureAlignmentIndex();
  return pairs_.size();
}

QPointF PointSeriesXY::sample(std::size_t index) const {
  ensureAlignmentIndex();
  if (index >= pairs_.size()) {
    return invalidPoint();
  }
  return pairs_[index].point;
}

std::optional<QPointF> PointSeriesXY::sampleFromTime(double display_time_sec) const {
  ensureAlignmentIndex();
  if (pairs_.empty() || session_ == nullptr) {
    return std::nullopt;
  }

  // The tracker speaks display seconds and pairs_ carries raw timestamps: map back through the Y source's offset (X and
  // Y share a time base) so the key is in the stored timestamps' domain.
  const DisplayOffset offset = session_->displayOffset(y_source_.dataset_id);
  const Timestamp raw_time = displaySecondsToRaw(fromAxisDouble(display_time_sec), offset);

  // pairs_ is time-ordered: the last pair at or before raw_time, as the time-series tracker does (the marker holds the
  // latest sample, it does not interpolate).
  const auto after = std::upper_bound(
      pairs_.begin(), pairs_.end(), raw_time, [](Timestamp time, const PairSlot& slot) { return time < slot.time; });
  if (after == pairs_.begin()) {
    return std::nullopt;  // cursor is before the first sample
  }
  return std::prev(after)->point;
}

QRectF PointSeriesXY::boundingRect() const {
  ensureAlignmentIndex();
  if (bounding_rect_valid_) {
    return cached_bounding_rect_;
  }

  double x_min = 0.0;
  double x_max = 0.0;
  double y_min = 0.0;
  double y_max = 0.0;
  bool found_x = false;
  bool found_y = false;

  for (const PairSlot& slot : pairs_) {
    updateFiniteRange(slot.point.x(), x_min, x_max, found_x);
    updateFiniteRange(slot.point.y(), y_min, y_max, found_y);
  }

  cached_bounding_rect_ =
      found_x && found_y ? QRectF(QPointF(x_min, y_min), QPointF(x_max, y_max)).normalized() : invalidRect();
  bounding_rect_valid_ = true;
  return cached_bounding_rect_;
}

void PointSeriesXY::setRectOfInterest(const QRectF& /*rect*/) {}

void PointSeriesXY::onTopicCommitted() {
  bumpContentRevision();
  alignment_dirty_ = true;
  bounding_rect_valid_ = false;
}

void PointSeriesXY::onDataCleared() {
  bumpContentRevision();
  pairs_.clear();
  alignment_dirty_ = true;
  bounding_rect_valid_ = false;
  cached_bounding_rect_ = invalidRect();
}

void PointSeriesXY::ensureAlignmentIndex() const {
  if (!alignment_dirty_) {
    return;
  }

  pairs_.clear();
  bounding_rect_valid_ = false;

  if (session_ == nullptr) {
    alignment_dirty_ = false;
    return;
  }

  // Hold the engine lock across the build: the index builders read chunks through the raw getTopicStorage accessor.
  // pairs_ keeps values, so nothing of the store is read once the lock is released.
  const auto lock = session_->dataEngine().lockEngine();

  if (x_source_.topic_id == y_source_.topic_id) {
    buildSameTopicIndex();
  } else {
    buildDifferentTopicIndex();
  }

  alignment_dirty_ = false;
}

void PointSeriesXY::buildSameTopicIndex() const {
  const TopicStorage* storage = session_->dataEngine().getTopicStorage(x_source_.topic_id);
  if (storage == nullptr) {
    return;
  }

  const Timestamp retention_floor = storage->retentionFloor();
  for (const TopicChunk& chunk : storage->sealedChunks()) {
    const std::size_t row_count = chunk.stats.row_count;
    for (std::size_t row = 0; row < row_count; ++row) {
      if (chunk.readTimestamp(row) < retention_floor) {
        continue;  // logically evicted by retention — never expose pre-floor rows
      }
      const double x_value = readY(chunk, x_source_.column_index, row);
      const double y_value = readY(chunk, y_source_.column_index, row);
      if (!std::isfinite(x_value) || !std::isfinite(y_value)) {
        continue;
      }
      pairs_.push_back(PairSlot{.time = chunk.readTimestamp(row), .point = QPointF(x_value, y_value)});
    }
  }
}

void PointSeriesXY::buildDifferentTopicIndex() const {
  const auto x_rows = rowsFor(x_source_.topic_id);
  const auto y_rows = rowsFor(y_source_.topic_id);

  std::size_t x_index = 0;
  std::size_t y_index = 0;
  while (x_index < x_rows.size() && y_index < y_rows.size()) {
    const Timestamp x_time = x_rows[x_index].chunk->readTimestamp(x_rows[x_index].row);
    const Timestamp y_time = y_rows[y_index].chunk->readTimestamp(y_rows[y_index].row);
    if (x_time < y_time) {
      ++x_index;
      continue;
    }
    if (y_time < x_time) {
      ++y_index;
      continue;
    }

    const double x_value = readY(*x_rows[x_index].chunk, x_source_.column_index, x_rows[x_index].row);
    const double y_value = readY(*y_rows[y_index].chunk, y_source_.column_index, y_rows[y_index].row);
    if (std::isfinite(x_value) && std::isfinite(y_value)) {
      pairs_.push_back(PairSlot{.time = y_time, .point = QPointF(x_value, y_value)});
    }
    ++x_index;
    ++y_index;
  }
}

std::vector<PointSeriesXY::RowRef> PointSeriesXY::rowsFor(TopicId topic_id) const {
  std::vector<RowRef> rows;
  const TopicStorage* storage = session_->dataEngine().getTopicStorage(topic_id);
  if (storage == nullptr) {
    return rows;
  }

  const Timestamp retention_floor = storage->retentionFloor();
  const auto& chunks = storage->sealedChunks();
  std::size_t row_count = 0;
  for (const TopicChunk& chunk : chunks) {
    row_count += chunk.stats.row_count;
  }
  rows.reserve(row_count);

  for (const TopicChunk& chunk : chunks) {
    for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
      if (chunk.readTimestamp(row) < retention_floor) {
        continue;  // logically evicted by retention
      }
      rows.push_back(RowRef{.chunk = &chunk, .row = row});
    }
  }
  return rows;
}

}  // namespace PJ
