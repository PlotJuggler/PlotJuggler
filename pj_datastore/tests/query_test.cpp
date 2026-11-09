// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/query.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <limits>
#include <utility>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"

namespace PJ {
namespace {

// Helper: build a test chunk with sequential timestamps.
TopicChunk makeTestChunk(Timestamp t_start, uint32_t num_rows, Timestamp step) {
  std::vector<ColumnDescriptor> cols = {{0, PrimitiveType::kFloat32, "value"}};
  TopicChunkBuilder builder(1, 1, cols, num_rows);
  for (uint32_t i = 0; i < num_rows; ++i) {
    Timestamp t = t_start + static_cast<Timestamp>(i) * step;
    builder.beginRow(t);
    builder.set(0, static_cast<float>(i) * 1.0f);
    builder.finishRow();
  }
  return builder.seal();
}

// Helper: build a chunk from an explicit (possibly non-uniform / duplicated)
// timestamp list. The column value equals the row index, so a returned
// row_index can be cross-checked against value.
TopicChunk makeChunkFromTimestamps(const std::vector<Timestamp>& ts) {
  std::vector<ColumnDescriptor> cols = {{0, PrimitiveType::kFloat32, "value"}};
  TopicChunkBuilder builder(1, 1, cols, static_cast<uint32_t>(ts.size()));
  for (std::size_t i = 0; i < ts.size(); ++i) {
    builder.beginRow(ts[i]);
    builder.set(0, static_cast<float>(i));
    builder.finishRow();
  }
  return builder.seal();
}

// Build the standard 5-chunk test fixture:
//   Chunk 0: t=[0,   90],  step=10
//   Chunk 1: t=[100, 190], step=10
//   Chunk 2: t=[200, 290], step=10
//   Chunk 3: t=[300, 390], step=10
//   Chunk 4: t=[400, 490], step=10
std::deque<TopicChunk> makeStandardChunks() {
  std::deque<TopicChunk> chunks;
  for (int i = 0; i < 5; ++i) {
    chunks.push_back(makeTestChunk(static_cast<Timestamp>(i) * 100, 10, 10));
  }
  return chunks;
}

// =========================================================================
// Range query tests
// =========================================================================

TEST(QueryTest, RangeQuerySpanningTwoChunks) {
  auto chunks = makeStandardChunks();
  auto cursor = rangeQuery(chunks, 150, 250);

  std::vector<Timestamp> timestamps;
  while (cursor.valid()) {
    timestamps.push_back(cursor.current().timestamp);
    cursor.advance();
  }

  // Expected: 150, 160, 170, 180, 190, 200, 210, 220, 230, 240, 250
  ASSERT_EQ(timestamps.size(), 11u);
  EXPECT_EQ(timestamps.front(), 150);
  EXPECT_EQ(timestamps.back(), 250);
  for (std::size_t i = 1; i < timestamps.size(); ++i) {
    EXPECT_EQ(timestamps[i] - timestamps[i - 1], 10);
  }
}

TEST(QueryTest, RangeQueryWithinSingleChunk) {
  auto chunks = makeStandardChunks();
  auto cursor = rangeQuery(chunks, 100, 190);

  std::size_t count = 0;
  while (cursor.valid()) {
    count++;
    cursor.advance();
  }
  EXPECT_EQ(count, 10u);
}

TEST(QueryTest, RangeQueryHittingNoChunks) {
  auto chunks = makeStandardChunks();
  auto cursor = rangeQuery(chunks, 500, 600);
  EXPECT_FALSE(cursor.valid());
}

TEST(QueryTest, RangeQueryAllData) {
  auto chunks = makeStandardChunks();
  auto cursor = rangeQuery(chunks, 0, 490);

  std::size_t count = 0;
  while (cursor.valid()) {
    count++;
    cursor.advance();
  }
  EXPECT_EQ(count, 50u);
}

TEST(QueryTest, RangeQueryExactChunkBoundary) {
  auto chunks = makeStandardChunks();
  // query [100, 199] should return only samples from chunk 1: 100..190
  auto cursor = rangeQuery(chunks, 100, 199);

  std::vector<Timestamp> timestamps;
  while (cursor.valid()) {
    timestamps.push_back(cursor.current().timestamp);
    cursor.advance();
  }

  // Chunk 1 has t = 100, 110, ..., 190. All 10 are in [100, 199].
  ASSERT_EQ(timestamps.size(), 10u);
  EXPECT_EQ(timestamps.front(), 100);
  EXPECT_EQ(timestamps.back(), 190);
}

TEST(QueryTest, ForEachCallback) {
  auto chunks = makeStandardChunks();
  auto cursor = rangeQuery(chunks, 200, 390);

  std::size_t count = 0;
  cursor.forEach([&count](const SampleRow& /*row*/) { ++count; });
  EXPECT_EQ(count, 20u);  // chunks 2 and 3, 10 rows each
}

// =========================================================================
// latest_at tests
// =========================================================================

TEST(QueryTest, LatestAtInMiddleOfChunk) {
  auto chunks = makeStandardChunks();
  auto result = latestAt(chunks, 155);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->timestamp, 150);
}

TEST(QueryTest, LatestAtExactTimestamp) {
  auto chunks = makeStandardChunks();
  auto result = latestAt(chunks, 200);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->timestamp, 200);
}

TEST(QueryTest, LatestAtBeforeAllData) {
  auto chunks = makeStandardChunks();
  auto result = latestAt(chunks, -10);
  EXPECT_FALSE(result.has_value());
}

TEST(QueryTest, LatestAtAfterAllData) {
  auto chunks = makeStandardChunks();
  auto result = latestAt(chunks, 1000);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->timestamp, 490);
}

TEST(QueryTest, LatestAtBetweenChunks) {
  auto chunks = makeStandardChunks();
  // t=95 is between chunk 0 (t_max=90) and chunk 1 (t_min=100)
  auto result = latestAt(chunks, 95);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->timestamp, 90);
}

// =========================================================================
// Binary-search edge cases (duplicate timestamps, shared chunk boundaries)
// =========================================================================

TEST(QueryTest, LatestAtWithDuplicateTimestampsReturnsLastDuplicate) {
  std::deque<TopicChunk> chunks;
  // Rows:           0    1    2    3    4
  chunks.push_back(makeChunkFromTimestamps({10, 20, 20, 20, 30}));

  auto result = latestAt(chunks, 20);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->timestamp, 20);
  // upper_bound semantics: the last row with ts <= 20 is row index 3.
  EXPECT_EQ(result->row_index, 3u);
}

TEST(QueryTest, RangeQueryWithDuplicateTimestampsStartsAtFirstDuplicate) {
  std::deque<TopicChunk> chunks;
  // Rows:           0    1    2    3    4
  chunks.push_back(makeChunkFromTimestamps({10, 20, 20, 20, 30}));

  auto cursor = rangeQuery(chunks, 20, 20);
  std::vector<std::size_t> rows;
  cursor.forEach([&](const SampleRow& row) { rows.push_back(row.row_index); });

  // lower_bound semantics: starts at the first ts >= 20 (row 1) and includes
  // every row with ts <= 20 (rows 1, 2, 3).
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows.front(), 1u);
  EXPECT_EQ(rows.back(), 3u);
}

TEST(QueryTest, LatestAtAtSharedChunkBoundarySelectsLaterChunk) {
  std::deque<TopicChunk> chunks;
  chunks.push_back(makeChunkFromTimestamps({70, 80, 90}));    // chunk A, t_max=90
  chunks.push_back(makeChunkFromTimestamps({90, 100, 110}));  // chunk B, t_min=90

  auto result = latestAt(chunks, 90);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->timestamp, 90);
  // The boundary value 90 exists in both chunks; the later chunk (B, row 0) wins.
  EXPECT_EQ(result->chunk, &chunks[1]);
  EXPECT_EQ(result->row_index, 0u);
}

TEST(QueryTest, RangeQuerySingleTimestampPoint) {
  auto chunks = makeStandardChunks();
  // Degenerate inclusive range [200, 200] hits exactly one row.
  auto cursor = rangeQuery(chunks, 200, 200);
  std::vector<Timestamp> timestamps;
  cursor.forEach([&](const SampleRow& row) { timestamps.push_back(row.timestamp); });
  ASSERT_EQ(timestamps.size(), 1u);
  EXPECT_EQ(timestamps.front(), 200);
}

// =========================================================================
// Empty deque tests
// =========================================================================

TEST(QueryTest, EmptyDequeRangeQuery) {
  std::deque<TopicChunk> empty;
  auto cursor = rangeQuery(empty, 0, 100);
  EXPECT_FALSE(cursor.valid());
}

TEST(QueryTest, EmptyDequeLatestAt) {
  std::deque<TopicChunk> empty;
  auto result = latestAt(empty, 50);
  EXPECT_FALSE(result.has_value());
}

// =========================================================================
// for_each_chunk tests
// =========================================================================

TEST(QueryTest, ForEachChunkMatchesForEach) {
  auto chunks = makeStandardChunks();

  // Collect per-row results via for_each
  auto cursor1 = rangeQuery(chunks, 150, 350);
  std::vector<Timestamp> per_row_ts;
  cursor1.forEach([&](const SampleRow& row) { per_row_ts.push_back(row.timestamp); });

  // Collect per-chunk results via for_each_chunk
  auto cursor2 = rangeQuery(chunks, 150, 350);
  std::vector<Timestamp> chunk_ts;
  cursor2.forEachChunk([&](const ChunkRowRange& range) {
    for (std::size_t r = range.row_start; r < range.row_end; ++r) {
      chunk_ts.push_back(range.chunk->readTimestamp(r));
    }
  });

  ASSERT_EQ(per_row_ts.size(), chunk_ts.size());
  for (std::size_t i = 0; i < per_row_ts.size(); ++i) {
    EXPECT_EQ(per_row_ts[i], chunk_ts[i]) << "mismatch at index " << i;
  }
}

TEST(QueryTest, ForEachChunkAllData) {
  auto chunks = makeStandardChunks();
  auto cursor = rangeQuery(chunks, 0, 490);

  std::size_t total_rows = 0;
  std::size_t chunk_count = 0;
  cursor.forEachChunk([&](const ChunkRowRange& range) {
    ++chunk_count;
    total_rows += range.row_end - range.row_start;
  });

  EXPECT_EQ(total_rows, 50u);
  EXPECT_EQ(chunk_count, 5u);
}

TEST(QueryTest, ForEachChunkNoResults) {
  auto chunks = makeStandardChunks();
  auto cursor = rangeQuery(chunks, 500, 600);

  std::size_t count = 0;
  cursor.forEachChunk([&](const ChunkRowRange& /*range*/) { ++count; });
  EXPECT_EQ(count, 0u);
}

// =========================================================================
// SeriesReader retention-floor tests
//
// A SeriesReader constructed with floor F must behave as if rows with
// timestamp < F do not exist — through EVERY method (the retention-window
// contract). The straddling chunk still physically holds those rows.
// All cases share one chunk t=[0,490] step 10 (value == row index) that
// straddles a floor of 200.
// =========================================================================

class SeriesReaderFloorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    chunks_.push_back(makeTestChunk(0, 50, 10));
  }
  std::deque<TopicChunk> chunks_;
};

TEST_F(SeriesReaderFloorTest, HidesSubFloorRows) {
  const SeriesReader series(chunks_, /*column_index=*/0, /*retention_floor=*/200);

  EXPECT_EQ(series.size(), 30u);  // rows 20..49
  EXPECT_FALSE(series.empty());

  const auto first = series.sampleAt(0);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->timestamp, 200);
  EXPECT_DOUBLE_EQ(first->value, 20.0);

  const auto last = series.sampleAt(29);
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->timestamp, 490);
}

TEST_F(SeriesReaderFloorTest, BoundsExcludeSubFloor) {
  const SeriesReader series(chunks_, 0, /*retention_floor=*/200);

  // The default (open) bounds must still respect the floor.
  const auto bounds = series.bounds();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_EQ(bounds->time.min, 200);
  EXPECT_EQ(bounds->time.max, 490);
  EXPECT_DOUBLE_EQ(bounds->value.min, 20.0);
  EXPECT_DOUBLE_EQ(bounds->value.max, 49.0);
  EXPECT_EQ(bounds->sample_count, 30u);
}

TEST_F(SeriesReaderFloorTest, SamplesCursorSkipsSubFloor) {
  const SeriesReader series(chunks_, 0, /*retention_floor=*/200);

  // An explicitly-open sample window must be raised up to the floor.
  std::vector<Timestamp> seen;
  auto cursor = series.samples(
      Range<Timestamp>{.min = std::numeric_limits<Timestamp>::min(), .max = std::numeric_limits<Timestamp>::max()});
  cursor.forEach([&](const SeriesSample& s) { seen.push_back(s.timestamp); });
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.front(), 200);
  EXPECT_EQ(seen.size(), 30u);
}

TEST_F(SeriesReaderFloorTest, IndexLookupsRespectFloor) {
  const SeriesReader series(chunks_, 0, /*retention_floor=*/200);

  // A point below the floor has no sample at-or-before it.
  EXPECT_FALSE(series.indexAtOrBeforeTime(150).has_value());
  EXPECT_FALSE(series.sampleAtOrBeforeTime(150).has_value());

  // At-or-after a sub-floor time resolves to the first retained sample (ts=200).
  const auto after = series.indexAtOrAfterTime(150);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*after, 0u);
  const auto after_sample = series.sampleAtOrAfterTime(150);
  ASSERT_TRUE(after_sample.has_value());
  EXPECT_EQ(after_sample->timestamp, 200);

  // Within the retained range, indices count only retained samples:
  // ts 200,210,220,230,240,250 -> indices 0..5.
  const auto before = series.indexAtOrBeforeTime(250);
  ASSERT_TRUE(before.has_value());
  EXPECT_EQ(*before, 5u);
}

TEST_F(SeriesReaderFloorTest, WindowEntirelyBelowFloorIsEmpty) {
  const SeriesReader series(chunks_, 0, /*retention_floor=*/200);

  // A window entirely below the floor must yield NOTHING: raising the lower
  // bound up to the floor must not invert the range back into visibility.
  std::size_t n = 0;
  series.samples(Range<Timestamp>{.min = 0, .max = 150}).forEach([&](const SeriesSample&) { ++n; });
  EXPECT_EQ(n, 0u);
  EXPECT_FALSE(series.bounds(Range<Timestamp>{.min = 0, .max = 150}).has_value());
}

TEST_F(SeriesReaderFloorTest, DefaultIsNoFloor) {
  // No floor argument -> existing behavior (no rows hidden).
  const SeriesReader series(chunks_, 0);
  EXPECT_EQ(series.size(), 50u);
  const auto bounds = series.bounds();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_EQ(bounds->time.min, 0);
}

// forEachOrderedRun's runs concatenate to forEach's rows, ties included, and only an overlap splits a chunk.
TEST(QueryTest, OrderedRunsConcatenateToTheMergedRows) {
  std::deque<TopicChunk> chunks;
  chunks.push_back(makeChunkFromTimestamps({0, 2, 4, 6}));
  chunks.push_back(makeChunkFromTimestamps({1, 2, 3, 8}));
  chunks.push_back(makeChunkFromTimestamps({10, 11, 12}));
  using Row = std::pair<const TopicChunk*, std::size_t>;
  const std::vector<std::pair<Timestamp, Timestamp>> windows = {
      {std::numeric_limits<Timestamp>::min(), std::numeric_limits<Timestamp>::max()}, {2, 6}, {3, 3}, {7, 9}, {10, 12}};
  for (const auto& [t_min, t_max] : windows) {
    std::vector<Row> merged;
    RangeCursor(chunks, t_min, t_max).forEach([&](const SampleRow& row) {
      merged.emplace_back(row.chunk, row.row_index);
    });
    std::vector<Row> runs;
    std::size_t run_count = 0;
    RangeCursor(chunks, t_min, t_max).forEachOrderedRun([&](const ChunkRowRange& run) {
      ++run_count;
      for (std::size_t row = run.row_start; row < run.row_end; ++row) {
        runs.emplace_back(run.chunk, row);
      }
    });
    EXPECT_EQ(runs, merged) << "window [" << t_min << ", " << t_max << "]";
    if (t_min == 10) {
      EXPECT_EQ(run_count, 1u);
    }
  }
}

}  // namespace
}  // namespace PJ
