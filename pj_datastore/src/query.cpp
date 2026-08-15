// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/query.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace PJ {
namespace {

[[nodiscard]] Range<Timestamp> normalized(Range<Timestamp> range) {
  if (range.max < range.min) {
    std::swap(range.min, range.max);
  }
  return range;
}

[[nodiscard]] bool isBoolColumn(const TopicChunk& chunk, std::size_t column_index) {
  return column_index < chunk.columns.size() && chunk.columns[column_index].descriptor &&
         chunk.columns[column_index].descriptor->logical_type == PrimitiveType::kBool;
}

[[nodiscard]] std::optional<double> readSeriesValue(
    const TopicChunk& chunk, std::size_t column_index, std::size_t row) {
  if (column_index >= chunk.columns.size() || row >= chunk.stats.row_count || chunk.isNull(column_index, row)) {
    return std::nullopt;
  }
  if (isBoolColumn(chunk, column_index)) {
    return chunk.readBool(column_index, row) ? 1.0 : 0.0;
  }
  return chunk.readNumericAsDouble(column_index, row);
}

[[nodiscard]] SeriesSample makeSeriesSample(const TopicChunk& chunk, std::size_t column_index, std::size_t row) {
  const auto value = readSeriesValue(chunk, column_index, row);
  assert(value.has_value());
  return SeriesSample{chunk.readTimestamp(row), *value, &chunk, row};
}

[[nodiscard]] Range<Timestamp> allTime() {
  return Range<Timestamp>{
      .min = std::numeric_limits<Timestamp>::min(),
      .max = std::numeric_limits<Timestamp>::max(),
  };
}

// Min-heap ordering for cursor frontiers: earliest (ts, chunk, row) on top.
[[nodiscard]] bool frontierAfter(const CursorFrontier& a, const CursorFrontier& b) {
  return std::tie(a.ts, a.chunk, a.row) > std::tie(b.ts, b.chunk, b.row);
}

// Replace the heap top with its in-chunk successor (has_next) or drop it.
// O(1) when the successor is still the global minimum — which is every
// advance when chunk ranges don't overlap; the heap only pays under overlap.
void replaceHeapTop(std::vector<CursorFrontier>& heap, const CursorFrontier& next, bool has_next) {
  if (has_next) {
    const std::size_t n = heap.size();
    const bool before_left = n <= 1 || !frontierAfter(next, heap[1]);
    const bool before_right = n <= 2 || !frontierAfter(next, heap[2]);
    if (before_left && before_right) {
      heap.front() = next;
      return;
    }
  }
  std::pop_heap(heap.begin(), heap.end(), frontierAfter);
  if (has_next) {
    heap.back() = next;
    std::push_heap(heap.begin(), heap.end(), frontierAfter);
  } else {
    heap.pop_back();
  }
}

}  // namespace

// ===========================================================================
// RangeCursor
// ===========================================================================

RangeCursor::RangeCursor(const std::deque<TopicChunk>& chunks, Timestamp t_min, Timestamp t_max)
    : chunks_(&chunks), t_min_(t_min), t_max_(t_max) {
  initFrontiers();
}

RangeCursor::RangeCursor(
    const std::deque<TopicChunk>& chunks, Timestamp t_min, Timestamp t_max, std::unique_lock<std::recursive_mutex> lock)
    : chunks_(&chunks), t_min_(t_min), t_max_(t_max), lock_(std::move(lock)) {  // adopts the read-lock for its lifetime
  initFrontiers();
}

bool RangeCursor::valid() const noexcept {
  return !frontiers_.empty();
}

SampleRow RangeCursor::current() const {
  assert(valid());
  const CursorFrontier& top = frontiers_.front();
  return SampleRow{top.ts, &(*chunks_)[top.chunk], top.row};
}

void RangeCursor::advance() {
  assert(valid());
  CursorFrontier next = frontiers_.front();
  const TopicChunk& chunk = (*chunks_)[next.chunk];
  ++next.row;
  bool has_next = next.row < chunk.stats.row_count;
  if (has_next) {
    next.ts = chunk.readTimestamp(next.row);
    has_next = next.ts <= t_max_;
  }
  replaceHeapTop(frontiers_, next, has_next);
}

void RangeCursor::forEach(std::function<void(const SampleRow&)> callback) {
  while (valid()) {
    callback(current());
    advance();
  }
}

void RangeCursor::forEachChunk(std::function<void(const ChunkRowRange&)> callback) {
  for (const TopicChunk& chunk : *chunks_) {
    // No early break: chunk ranges may overlap, so a later chunk can still
    // intersect the query range even after one that lies past it.
    if (chunk.stats.row_count == 0 || chunk.stats.t_max < t_min_ || chunk.stats.t_min > t_max_) {
      continue;
    }
    const auto ts_begin = chunk.timestamps.begin();
    const auto ts_end = ts_begin + static_cast<std::ptrdiff_t>(chunk.stats.row_count);
    const auto first_it = std::lower_bound(ts_begin, ts_end, t_min_);
    const auto end_it = std::upper_bound(first_it, ts_end, t_max_);
    if (first_it != end_it) {
      callback(
          ChunkRowRange{
              &chunk, static_cast<std::size_t>(first_it - ts_begin), static_cast<std::size_t>(end_it - ts_begin)});
    }
  }
  // Mark cursor exhausted
  frontiers_.clear();
}

void RangeCursor::initFrontiers() {
  const auto& chunks = *chunks_;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    const TopicChunk& chunk = chunks[i];
    if (chunk.stats.row_count == 0 || chunk.stats.t_max < t_min_ || chunk.stats.t_min > t_max_) {
      continue;
    }
    // First row with timestamp >= t_min_ (chunks are internally sorted).
    const auto ts_begin = chunk.timestamps.begin();
    const auto ts_end = ts_begin + static_cast<std::ptrdiff_t>(chunk.stats.row_count);
    const auto row_it = std::lower_bound(ts_begin, ts_end, t_min_);
    if (row_it == ts_end || *row_it > t_max_) {
      continue;
    }
    frontiers_.push_back(CursorFrontier{*row_it, i, static_cast<std::size_t>(row_it - ts_begin)});
  }
  std::make_heap(frontiers_.begin(), frontiers_.end(), frontierAfter);
}

// ===========================================================================
// latest_at
// ===========================================================================

std::optional<SampleRow> latestAt(const std::deque<TopicChunk>& chunks, Timestamp t, Timestamp retention_floor) {
  // Chunk ranges may overlap (out-of-order ingest), so candidate chunks are
  // scanned rather than binary-searched; each candidate contributes its last
  // row with timestamp <= t (per-chunk binary search — chunks are internally
  // sorted). Later-committed chunks win timestamp ties, matching the
  // pre-overlap behaviour at shared chunk boundaries.
  std::optional<SampleRow> best;
  for (const TopicChunk& chunk : chunks) {
    // Skip empty chunks, chunks beginning after t, and chunks entirely below the
    // retention floor (all their rows are logically evicted).
    if (chunk.stats.row_count == 0 || chunk.stats.t_min > t || chunk.stats.t_max < retention_floor) {
      continue;
    }
    const auto ts_begin = chunk.timestamps.begin();
    const auto ts_end = ts_begin + static_cast<std::ptrdiff_t>(chunk.stats.row_count);
    const auto row_after = std::upper_bound(ts_begin, ts_end, t);
    if (row_after == ts_begin) {
      continue;  // unreachable for committed chunks (row 0 ts == t_min <= t)
    }
    const std::size_t row = static_cast<std::size_t>((row_after - 1) - ts_begin);
    const Timestamp ts = chunk.readTimestamp(row);
    if (ts < retention_floor) {
      continue;  // the latest sample at-or-before t is logically evicted
    }
    if (!best.has_value() || ts >= best->timestamp) {
      best = SampleRow{ts, &chunk, row};
    }
  }
  return best;
}

// ===========================================================================
// range_query
// ===========================================================================

RangeCursor rangeQuery(const std::deque<TopicChunk>& chunks, Timestamp t_min, Timestamp t_max) {
  return RangeCursor(chunks, t_min, t_max);
}

// ===========================================================================
// SeriesCursor
// ===========================================================================

SeriesCursor::SeriesCursor(
    const std::deque<TopicChunk>& chunks, std::size_t column_index, Range<Timestamp> time_range,
    Timestamp retention_floor)
    : chunks_(&chunks), column_index_(column_index), time_range_(normalized(time_range)) {
  // Raise the lower bound to the retention floor AFTER normalization so an
  // entirely-below-floor window cannot be swapped back into visibility. From
  // here the floor is just the cursor's lower time bound: initFrontiers seeks to
  // it and nextSample rejects rows below it, so an inverted [floor, max] window
  // naturally yields nothing.
  time_range_.min = std::max(time_range_.min, retention_floor);
  initFrontiers();
}

bool SeriesCursor::valid() const noexcept {
  return !frontiers_.empty();
}

SeriesSample SeriesCursor::current() const {
  assert(valid());
  const CursorFrontier& top = frontiers_.front();
  return makeSeriesSample((*chunks_)[top.chunk], column_index_, top.row);
}

void SeriesCursor::advance() {
  assert(valid());
  CursorFrontier next = frontiers_.front();
  ++next.row;
  replaceHeapTop(frontiers_, next, nextSample(next));
}

void SeriesCursor::forEach(std::function<void(const SeriesSample&)> callback) {
  while (valid()) {
    callback(current());
    advance();
  }
}

bool SeriesCursor::nextSample(CursorFrontier& frontier) const {
  const TopicChunk& chunk = (*chunks_)[frontier.chunk];
  while (frontier.row < chunk.stats.row_count) {
    const Timestamp ts = chunk.readTimestamp(frontier.row);
    if (ts > time_range_.max) {
      return false;
    }
    if (ts >= time_range_.min && readSeriesValue(chunk, column_index_, frontier.row).has_value()) {
      frontier.ts = ts;
      return true;
    }
    ++frontier.row;
  }
  return false;
}

void SeriesCursor::initFrontiers() {
  const auto& chunks = *chunks_;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    const TopicChunk& chunk = chunks[i];
    // column_index_ bound check: earlier chunks may predate a mid-stream
    // column addition. No early break — chunk ranges may overlap.
    if (chunk.stats.row_count == 0 || chunk.stats.t_max < time_range_.min || chunk.stats.t_min > time_range_.max ||
        column_index_ >= chunk.columns.size()) {
      continue;
    }
    const auto ts_begin = chunk.timestamps.begin();
    const auto ts_end = ts_begin + static_cast<std::ptrdiff_t>(chunk.stats.row_count);
    const auto row_it = std::lower_bound(ts_begin, ts_end, time_range_.min);
    CursorFrontier frontier{0, i, static_cast<std::size_t>(row_it - ts_begin)};
    if (nextSample(frontier)) {
      frontiers_.push_back(frontier);
    }
  }
  std::make_heap(frontiers_.begin(), frontiers_.end(), frontierAfter);
}

// ===========================================================================
// SeriesReader
// ===========================================================================

SeriesReader::SeriesReader(const std::deque<TopicChunk>& chunks, std::size_t column_index, Timestamp retention_floor)
    : chunks_(&chunks), column_index_(column_index), retention_floor_(retention_floor) {}

SeriesReader::SeriesReader(
    const std::deque<TopicChunk>& chunks, std::size_t column_index, Timestamp retention_floor,
    std::unique_lock<std::recursive_mutex> lock)
    : chunks_(&chunks), column_index_(column_index), retention_floor_(retention_floor), lock_(std::move(lock)) {}

std::size_t SeriesReader::size() const {
  std::size_t count = 0;
  for (const TopicChunk& chunk : *chunks_) {
    // Skip a chunk entirely below the floor (all its rows are logically evicted).
    if (column_index_ >= chunk.columns.size() || chunk.stats.t_max < retention_floor_) {
      continue;
    }
    for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
      if (chunk.readTimestamp(row) < retention_floor_) {
        continue;  // below the retention floor: logically evicted
      }
      if (readSeriesValue(chunk, column_index_, row).has_value()) {
        ++count;
      }
    }
  }
  return count;
}

bool SeriesReader::empty() const {
  return size() == 0;
}

std::optional<SeriesSample> SeriesReader::sampleAt(std::size_t index) const {
  std::size_t series_index = 0;
  for (const TopicChunk& chunk : *chunks_) {
    if (column_index_ >= chunk.columns.size() || chunk.stats.t_max < retention_floor_) {
      continue;
    }
    for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
      if (chunk.readTimestamp(row) < retention_floor_) {
        continue;  // logically evicted: not part of the virtual series
      }
      if (!readSeriesValue(chunk, column_index_, row).has_value()) {
        continue;
      }
      if (series_index == index) {
        return makeSeriesSample(chunk, column_index_, row);
      }
      ++series_index;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> SeriesReader::indexAtOrBeforeTime(Timestamp t) const {
  std::optional<std::size_t> latest;
  std::size_t series_index = 0;
  for (const TopicChunk& chunk : *chunks_) {
    if (chunk.stats.row_count == 0 || column_index_ >= chunk.columns.size() || chunk.stats.t_max < retention_floor_) {
      continue;
    }
    if (chunk.stats.t_min > t) {
      break;
    }
    for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
      const Timestamp ts = chunk.readTimestamp(row);
      if (ts > t) {
        return latest;
      }
      if (ts < retention_floor_) {
        continue;  // logically evicted: not part of the virtual series
      }
      if (readSeriesValue(chunk, column_index_, row).has_value()) {
        latest = series_index;
        ++series_index;
      }
    }
  }
  return latest;
}

std::optional<std::size_t> SeriesReader::indexAtOrAfterTime(Timestamp t) const {
  std::size_t series_index = 0;
  for (const TopicChunk& chunk : *chunks_) {
    if (chunk.stats.row_count == 0 || column_index_ >= chunk.columns.size() || chunk.stats.t_max < retention_floor_) {
      continue;
    }
    if (chunk.stats.t_max < t) {
      for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
        if (chunk.readTimestamp(row) < retention_floor_) {
          continue;  // logically evicted: does not advance the virtual index
        }
        if (readSeriesValue(chunk, column_index_, row).has_value()) {
          ++series_index;
        }
      }
      continue;
    }
    for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
      const Timestamp ts = chunk.readTimestamp(row);
      if (ts < retention_floor_) {
        continue;  // logically evicted
      }
      if (!readSeriesValue(chunk, column_index_, row).has_value()) {
        continue;
      }
      if (ts >= t) {
        return series_index;
      }
      ++series_index;
    }
  }
  return std::nullopt;
}

namespace {

// Latest sample with ts <= t (ts < t when strict), across ALL chunks. Chunk
// time ranges may overlap after out-of-order ingest, so every chunk is
// inspected; rows are time-sorted within a chunk, so each chunk contributes
// via one binary search plus a short backward walk over null/evicted rows.
std::optional<SeriesSample> bestSampleBefore(
    const std::deque<TopicChunk>& chunks, std::size_t column_index, Timestamp retention_floor, Timestamp t,
    bool strict) {
  std::optional<SeriesSample> best;
  for (const TopicChunk& chunk : chunks) {
    if (chunk.stats.row_count == 0 || column_index >= chunk.columns.size() || chunk.stats.t_max < retention_floor) {
      continue;
    }
    if (strict ? chunk.stats.t_min >= t : chunk.stats.t_min > t) {
      continue;
    }
    // First row past the cut; candidates are the rows before it, newest first.
    std::size_t lo = 0;
    std::size_t hi = chunk.stats.row_count;
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      const Timestamp ts = chunk.readTimestamp(mid);
      if (strict ? ts < t : ts <= t) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    for (std::size_t row = lo; row-- > 0;) {
      const Timestamp ts = chunk.readTimestamp(row);
      if (ts < retention_floor) {
        break;  // rows below are older still: all logically evicted
      }
      if (best.has_value() && ts <= best->timestamp) {
        break;  // rows below can only be older; this chunk cannot improve
      }
      if (!readSeriesValue(chunk, column_index, row).has_value()) {
        continue;  // null row: not part of the series, try the next-older one
      }
      best = makeSeriesSample(chunk, column_index, row);
      break;
    }
  }
  return best;
}

// First sample with ts >= t (ts > t when strict), across ALL chunks; the
// mirror of bestSampleBefore.
std::optional<SeriesSample> bestSampleAfter(
    const std::deque<TopicChunk>& chunks, std::size_t column_index, Timestamp retention_floor, Timestamp t,
    bool strict) {
  std::optional<SeriesSample> best;
  for (const TopicChunk& chunk : chunks) {
    if (chunk.stats.row_count == 0 || column_index >= chunk.columns.size() || chunk.stats.t_max < retention_floor) {
      continue;
    }
    if (strict ? chunk.stats.t_max <= t : chunk.stats.t_max < t) {
      continue;
    }
    // First row at/past the cut; candidates run forward from it, oldest first.
    std::size_t lo = 0;
    std::size_t hi = chunk.stats.row_count;
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      const Timestamp ts = chunk.readTimestamp(mid);
      if (strict ? ts <= t : ts < t) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    for (std::size_t row = lo; row < chunk.stats.row_count; ++row) {
      const Timestamp ts = chunk.readTimestamp(row);
      if (ts < retention_floor) {
        continue;  // logically evicted: keep looking at newer rows
      }
      if (best.has_value() && ts >= best->timestamp) {
        break;  // rows above can only be newer; this chunk cannot improve
      }
      if (!readSeriesValue(chunk, column_index, row).has_value()) {
        continue;  // null row: try the next-newer one
      }
      best = makeSeriesSample(chunk, column_index, row);
      break;
    }
  }
  return best;
}

}  // namespace

std::optional<SeriesSample> SeriesReader::sampleAtOrBeforeTime(Timestamp t) const {
  return bestSampleBefore(*chunks_, column_index_, retention_floor_, t, /*strict=*/false);
}

std::optional<SeriesSample> SeriesReader::sampleAtOrAfterTime(Timestamp t) const {
  return bestSampleAfter(*chunks_, column_index_, retention_floor_, t, /*strict=*/false);
}

std::optional<SeriesSample> SeriesReader::sampleBeforeTime(Timestamp t) const {
  return bestSampleBefore(*chunks_, column_index_, retention_floor_, t, /*strict=*/true);
}

std::optional<SeriesSample> SeriesReader::sampleAfterTime(Timestamp t) const {
  return bestSampleAfter(*chunks_, column_index_, retention_floor_, t, /*strict=*/true);
}

SeriesCursor SeriesReader::samples(Range<Timestamp> time_range) const {
  // The cursor raises its own lower bound to the retention floor, so logically-
  // evicted rows are never yielded (and a window entirely below the floor yields
  // nothing).
  return SeriesCursor(*chunks_, column_index_, time_range, retention_floor_);
}

std::optional<SeriesBounds> SeriesReader::bounds() const {
  return bounds(allTime());
}

std::optional<SeriesBounds> SeriesReader::bounds(Range<Timestamp> time_range) const {
  SeriesBounds result;
  bool found_time = false;
  bool found_value = false;
  auto cursor = samples(time_range);
  cursor.forEach([&](const SeriesSample& sample) {
    if (!found_time) {
      result.time.min = sample.timestamp;
      result.time.max = sample.timestamp;
      found_time = true;
    } else {
      result.time.min = std::min(result.time.min, sample.timestamp);
      result.time.max = std::max(result.time.max, sample.timestamp);
    }

    if (std::isfinite(sample.value)) {
      if (!found_value) {
        result.value.min = sample.value;
        result.value.max = sample.value;
        found_value = true;
      } else {
        result.value.min = std::min(result.value.min, sample.value);
        result.value.max = std::max(result.value.max, sample.value);
      }
    }
    ++result.sample_count;
  });

  if (!found_time || !found_value) {
    return std::nullopt;
  }
  return result;
}

}  // namespace PJ
