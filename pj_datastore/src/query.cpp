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

// Greater-than, so the std heap functions keep the earliest (ts, chunk, row) on top.
[[nodiscard]] bool frontierAfter(const CursorFrontier& a, const CursorFrontier& b) {
  return std::tie(a.ts, a.chunk, a.row) > std::tie(b.ts, b.chunk, b.row);
}

// O(1) while the successor stays the minimum, which is every advance when chunks do not overlap.
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

RangeCursor::RangeCursor(const std::deque<TopicChunk>& chunks, Timestamp t_min, Timestamp t_max)
    : chunks_(&chunks), t_min_(t_min), t_max_(t_max) {
  initFrontiers();
}

RangeCursor::RangeCursor(
    const std::deque<TopicChunk>& chunks, Timestamp t_min, Timestamp t_max, std::unique_lock<std::recursive_mutex> lock)
    : chunks_(&chunks), t_min_(t_min), t_max_(t_max), lock_(std::move(lock)) {  // holds the engine lock while alive
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
    // No early break: chunks may overlap, so a later one can still intersect the range.
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

void RangeCursor::forEachOrderedRun(const std::function<void(const ChunkRowRange&)>& callback) {
  while (valid()) {
    std::pop_heap(frontiers_.begin(), frontiers_.end(), frontierAfter);
    const CursorFrontier top = frontiers_.back();
    frontiers_.pop_back();
    const TopicChunk& chunk = (*chunks_)[top.chunk];
    const auto ts_begin = chunk.timestamps.begin();
    const auto run_begin = ts_begin + static_cast<std::ptrdiff_t>(top.row);
    auto run_end = std::upper_bound(run_begin, ts_begin + static_cast<std::ptrdiff_t>(chunk.stats.row_count), t_max_);
    const bool more = !frontiers_.empty();
    if (more) {
      // The rows that come before the next frontier in (ts, chunk) order: a tie goes to the lower chunk index.
      const CursorFrontier& next = frontiers_.front();
      run_end = top.chunk < next.chunk ? std::upper_bound(run_begin, run_end, next.ts)
                                       : std::lower_bound(run_begin, run_end, next.ts);
    }
    const auto row_end = static_cast<std::size_t>(run_end - ts_begin);
    assert(row_end > top.row);
    callback(ChunkRowRange{&chunk, top.row, row_end});
    if (more && row_end < chunk.stats.row_count && *run_end <= t_max_) {
      frontiers_.push_back(CursorFrontier{*run_end, top.chunk, row_end});
      std::push_heap(frontiers_.begin(), frontiers_.end(), frontierAfter);
    }
  }
}

void RangeCursor::initFrontiers() {
  const auto& chunks = *chunks_;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    const TopicChunk& chunk = chunks[i];
    if (chunk.stats.row_count == 0 || chunk.stats.t_max < t_min_ || chunk.stats.t_min > t_max_) {
      continue;
    }
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

std::optional<SampleRow> latestAt(const std::deque<TopicChunk>& chunks, Timestamp t, Timestamp retention_floor) {
  return latestAt(chunks, t, retention_floor, nullptr);
}

std::optional<SampleRow> latestAt(
    const std::deque<TopicChunk>& chunks, Timestamp t, Timestamp retention_floor,
    const std::function<bool(const TopicChunk&)>& include) {
  // Chunks may overlap, so every candidate is scanned; later commits win timestamp ties.
  std::optional<SampleRow> best;
  for (const TopicChunk& chunk : chunks) {
    if (chunk.stats.row_count == 0 || chunk.stats.t_min > t || chunk.stats.t_max < retention_floor ||
        (include && !include(chunk))) {
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
      continue;  // evicted, like every earlier row of the chunk
    }
    if (!best.has_value() || ts >= best->timestamp) {
      best = SampleRow{ts, &chunk, row};
    }
  }
  return best;
}

RangeCursor rangeQuery(const std::deque<TopicChunk>& chunks, Timestamp t_min, Timestamp t_max) {
  return RangeCursor(chunks, t_min, t_max);
}

SeriesCursor::SeriesCursor(
    const std::deque<TopicChunk>& chunks, std::size_t column_index, Range<Timestamp> time_range,
    Timestamp retention_floor)
    : chunks_(&chunks), column_index_(column_index), time_range_(normalized(time_range)) {
  // Clamped after normalized(), whose swap would turn a window below the floor back into a visible one; an inverted
  // [floor, max] window yields nothing.
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
    // Earlier chunks may predate a mid-stream column add. No early break: chunks may overlap.
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

SeriesReader::SeriesReader(const std::deque<TopicChunk>& chunks, std::size_t column_index, Timestamp retention_floor)
    : chunks_(&chunks), column_index_(column_index), retention_floor_(retention_floor) {}

SeriesReader::SeriesReader(
    const std::deque<TopicChunk>& chunks, std::size_t column_index, Timestamp retention_floor,
    std::unique_lock<std::recursive_mutex> lock)
    : chunks_(&chunks), column_index_(column_index), retention_floor_(retention_floor), lock_(std::move(lock)) {}

std::size_t SeriesReader::size() const {
  std::size_t count = 0;
  for (const TopicChunk& chunk : *chunks_) {
    if (column_index_ >= chunk.columns.size() || chunk.stats.t_max < retention_floor_) {
      continue;
    }
    for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
      if (chunk.readTimestamp(row) < retention_floor_) {
        continue;
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
        continue;
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
        continue;
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
          continue;
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
        continue;
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

bool columnAllNull(const TopicChunk& chunk, std::size_t column_index) {
  return column_index < chunk.stats.column_stats.size() &&
         chunk.stats.column_stats[column_index].null_count >= chunk.stats.row_count;
}

// Latest sample with ts <= t (ts < t when strict). Chunks may overlap, so each one is a candidate. Newest commit
// first: once a sample is found, a chunk ending at or before it is skipped, and equal timestamps resolve to the later
// commit, as rows inside a chunk do.
std::optional<SeriesSample> bestSampleBefore(
    const std::deque<TopicChunk>& chunks, std::size_t column_index, Timestamp retention_floor, Timestamp t,
    bool strict) {
  std::optional<SeriesSample> best;
  for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
    const TopicChunk& chunk = *it;
    if (chunk.stats.row_count == 0 || column_index >= chunk.columns.size() || chunk.stats.t_max < retention_floor ||
        columnAllNull(chunk, column_index)) {
      continue;
    }
    if (strict ? chunk.stats.t_min >= t : chunk.stats.t_min > t) {
      continue;
    }
    if (best.has_value() && chunk.stats.t_max <= best->timestamp) {
      continue;  // an older commit cannot beat the sample found, even on a tie
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
        break;  // the earlier rows are evicted too
      }
      if (best.has_value() && ts <= best->timestamp) {
        break;  // rows below can only be older; this chunk cannot improve
      }
      if (!readSeriesValue(chunk, column_index, row).has_value()) {
        continue;  // null: try the next-older row
      }
      best = makeSeriesSample(chunk, column_index, row);
      break;
    }
  }
  return best;
}

// First sample with ts >= t (ts > t when strict); the mirror of bestSampleBefore().
std::optional<SeriesSample> bestSampleAfter(
    const std::deque<TopicChunk>& chunks, std::size_t column_index, Timestamp retention_floor, Timestamp t,
    bool strict) {
  std::optional<SeriesSample> best;
  for (const TopicChunk& chunk : chunks) {
    if (chunk.stats.row_count == 0 || column_index >= chunk.columns.size() || chunk.stats.t_max < retention_floor ||
        columnAllNull(chunk, column_index)) {
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
        continue;  // evicted; a newer row may not be
      }
      if (best.has_value() && ts >= best->timestamp) {
        break;  // rows above can only be newer; this chunk cannot improve
      }
      if (!readSeriesValue(chunk, column_index, row).has_value()) {
        continue;  // null: try the next-newer row
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
  return SeriesCursor(*chunks_, column_index_, time_range, retention_floor_);
}

std::optional<SeriesBounds> SeriesReader::bounds() const {
  return bounds(allTime());
}

std::optional<SeriesBounds> SeriesReader::bounds(Range<Timestamp> time_range) const {
  // Min and max ignore order, so no cursor merge: a chunk wholly inside the window answers from its statistics, any
  // other chunk is scanned over its rows in the window. The statistics stand in only when they equal that scan: no
  // nulls (a null row is no sample, and t_min/t_max count it) and a finite min <= max (the statistics keep infinities,
  // and a chunk of nothing but NaNs has no extremes).
  Range<Timestamp> window = normalized(time_range);
  window.min = std::max(window.min, retention_floor_);
  SeriesBounds result{
      .time = {.min = std::numeric_limits<Timestamp>::max(), .max = std::numeric_limits<Timestamp>::min()},
      .value = {.min = std::numeric_limits<double>::infinity(), .max = -std::numeric_limits<double>::infinity()},
  };
  const auto add_time = [&result](Timestamp t_min, Timestamp t_max) {
    result.time.min = std::min(result.time.min, t_min);
    result.time.max = std::max(result.time.max, t_max);
  };
  const auto add_value = [&result](double min, double max) {
    result.value.min = std::min(result.value.min, min);
    result.value.max = std::max(result.value.max, max);
  };
  for (const TopicChunk& chunk : *chunks_) {
    if (chunk.stats.row_count == 0 || column_index_ >= chunk.columns.size() || chunk.stats.t_max < window.min ||
        chunk.stats.t_min > window.max) {
      continue;
    }
    if (chunk.stats.t_min >= window.min && chunk.stats.t_max <= window.max &&
        column_index_ < chunk.stats.column_stats.size()) {
      const ColumnStats& column = chunk.stats.column_stats[column_index_];
      if (column.null_count == 0 && column.min_value.has_value() && column.max_value.has_value() &&
          std::isfinite(*column.min_value) && std::isfinite(*column.max_value) &&
          *column.min_value <= *column.max_value) {
        add_time(chunk.stats.t_min, chunk.stats.t_max);
        add_value(*column.min_value, *column.max_value);
        result.sample_count += chunk.stats.row_count;
        continue;
      }
    }
    const auto ts_begin = chunk.timestamps.begin();
    const auto ts_end = ts_begin + static_cast<std::ptrdiff_t>(chunk.stats.row_count);
    const auto first = std::lower_bound(ts_begin, ts_end, window.min);
    const auto last = std::upper_bound(first, ts_end, window.max);
    for (auto it = first; it != last; ++it) {
      const auto row = static_cast<std::size_t>(it - ts_begin);
      const std::optional<double> value = readSeriesValue(chunk, column_index_, row);
      if (!value.has_value()) {
        continue;
      }
      add_time(*it, *it);
      if (std::isfinite(*value)) {
        add_value(*value, *value);
      }
      ++result.sample_count;
    }
  }
  if (result.sample_count == 0 || result.value.min > result.value.max) {
    return std::nullopt;
  }
  return result;
}

}  // namespace PJ
