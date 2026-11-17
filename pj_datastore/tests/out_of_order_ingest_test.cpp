// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Lossless out-of-order ingest: arrival order is not timestamp order (publishers' embedded timestamps
// disagree), and the engine retains every row regardless of timestamp regressions. Each sealed chunk is
// internally sorted (stable for duplicate timestamps); sealed chunks of one topic may overlap in time;
// queries (rangeQuery / latestAt / metadata extrema) stay correct over overlapping chunks.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

// Collects (timestamp, first-column double) pairs over the full time range.
std::vector<std::pair<Timestamp, double>> readAll(DataEngine& engine, TopicId topic_id) {
  std::vector<std::pair<Timestamp, double>> out;
  DataReader reader = engine.createReader();
  auto cursor_or = reader.rangeQuery(
      QueryRange{
          .topic_id = topic_id,
          .t_min = std::numeric_limits<Timestamp>::min(),
          .t_max = std::numeric_limits<Timestamp>::max()});
  EXPECT_TRUE(cursor_or.has_value());
  if (!cursor_or.has_value()) {
    return out;
  }
  cursor_or->forEach([&out](const SampleRow& row) {
    out.emplace_back(row.timestamp, row.chunk->readNumericAsDouble(0, row.row_index));
  });
  return out;
}

TEST(OutOfOrderIngestTest, AppendScalarToleratesOutOfOrder) {
  DataEngine engine;
  auto dataset_id = engine.createDataset(DatasetDescriptor{.source_name = "ooo", .time_domain_id = 0});
  ASSERT_TRUE(dataset_id.has_value());
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(*dataset_id, "scalar", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value());

  // Arrival order with two regressions (50 after 100, 25 after 150).
  writer.appendScalar(*handle, 100, 1.0);
  writer.appendScalar(*handle, 50, 2.0);
  writer.appendScalar(*handle, 150, 3.0);
  writer.appendScalar(*handle, 25, 4.0);
  engine.commitChunks(writer.flushAll());

  const auto rows = readAll(engine, handle->topic_id);
  const std::vector<std::pair<Timestamp, double>> expected = {{25, 4.0}, {50, 2.0}, {100, 1.0}, {150, 3.0}};
  EXPECT_EQ(rows, expected);

  DataReader reader = engine.createReader();
  auto latest = reader.latestAt(QueryPoint{.topic_id = handle->topic_id, .t = 60});
  ASSERT_TRUE(latest.has_value());
  ASSERT_TRUE(latest->has_value());
  EXPECT_EQ((*latest)->timestamp, 50);
  EXPECT_DOUBLE_EQ((*latest)->values[0], 2.0);

  auto meta = reader.getMetadata(handle->topic_id);
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->total_row_count, 4U);
  EXPECT_EQ(meta->time_range_min, 25);
  EXPECT_EQ(meta->time_range_max, 150);
}

TEST(OutOfOrderIngestTest, RowAppendAcceptsInterleavedPublisherTimestamps) {
  // Publisher A stamps "now", publisher B future-dates by 9.9 s.
  // Interleaved arrival makes every other row a timestamp regression.
  constexpr Timestamp kSkew = 9'900'000'000;
  constexpr std::size_t kPerPublisher = 300;

  DataEngine engine;
  auto dataset_id = engine.createDataset(DatasetDescriptor{.source_name = "tf", .time_domain_id = 0});
  ASSERT_TRUE(dataset_id.has_value());
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(*dataset_id, "tf_x", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value());

  for (std::size_t i = 0; i < kPerPublisher; ++i) {
    const auto base = static_cast<Timestamp>(i) * 1'000'000;
    writer.appendScalar(*handle, base + kSkew, 1.0);  // future-dated publisher first
    writer.appendScalar(*handle, base, 0.0);          // regression on every odom sample
  }
  engine.commitChunks(writer.flushAll());

  const auto rows = readAll(engine, handle->topic_id);
  ASSERT_EQ(rows.size(), 2 * kPerPublisher) << "out-of-order rows must not be dropped";
  for (std::size_t i = 1; i < rows.size(); ++i) {
    EXPECT_LE(rows[i - 1].first, rows[i].first) << "query must yield ascending timestamps at row " << i;
  }
}

TEST(OutOfOrderIngestTest, BatchAppendAcceptsUnsortedTimestamps) {
  DataEngine engine;
  auto dataset_id = engine.createDataset(DatasetDescriptor{.source_name = "batch", .time_domain_id = 0});
  ASSERT_TRUE(dataset_id.has_value());
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(*dataset_id, "batch", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value());

  const std::vector<Timestamp> timestamps = {300, 100, 200};
  const std::vector<double> values = {3.0, 1.0, 2.0};
  const ColumnData column = ColumnData::float64(0, Span<const double>(values.data(), values.size()));
  ASSERT_TRUE(writer
                  .appendColumns(
                      handle->topic_id, Span<const Timestamp>(timestamps.data(), timestamps.size()),
                      Span<const ColumnData>(&column, 1))
                  .has_value());
  engine.commitChunks(writer.flushAll());

  const auto rows = readAll(engine, handle->topic_id);
  const std::vector<std::pair<Timestamp, double>> expected = {{100, 1.0}, {200, 2.0}, {300, 3.0}};
  EXPECT_EQ(rows, expected);
}

TEST(OutOfOrderIngestTest, OutOfOrderAcrossChunkSealBoundaryRetainsAllRows) {
  // Fill one full chunk (auto-seal at 1024 rows), then append rows that
  // regress into the sealed chunk's range — the new chunk overlaps the old.
  DataEngine engine;
  auto dataset_id = engine.createDataset(DatasetDescriptor{.source_name = "seal", .time_domain_id = 0});
  ASSERT_TRUE(dataset_id.has_value());
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(*dataset_id, "seal", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value());

  constexpr std::size_t kChunkRows = 1024;
  for (std::size_t i = 0; i < kChunkRows; ++i) {
    const auto ts = static_cast<Timestamp>(2000 + i);
    writer.appendScalar(*handle, ts, static_cast<double>(ts));
  }
  // Late rows regress below the sealed chunk's t_min and interleave its range.
  writer.appendScalar(*handle, 1500, 1500.0);
  writer.appendScalar(*handle, 3500, 3500.0);
  writer.appendScalar(*handle, 1600, 1600.0);
  engine.commitChunks(writer.flushAll());

  const auto rows = readAll(engine, handle->topic_id);
  ASSERT_EQ(rows.size(), kChunkRows + 3) << "late rows must survive the seal boundary";
  for (std::size_t i = 1; i < rows.size(); ++i) {
    EXPECT_LE(rows[i - 1].first, rows[i].first);
  }

  DataReader reader = engine.createReader();
  auto latest = reader.latestAt(QueryPoint{.topic_id = handle->topic_id, .t = 1999});
  ASSERT_TRUE(latest.has_value());
  ASSERT_TRUE(latest->has_value());
  EXPECT_EQ((*latest)->timestamp, 1600) << "latestAt must consider overlapping chunks";

  auto meta = reader.getMetadata(handle->topic_id);
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->time_range_min, 1500);
  EXPECT_EQ(meta->time_range_max, 3500);
}

TEST(OutOfOrderIngestTest, StableOrderForDuplicateTimestamps) {
  DataEngine engine;
  auto dataset_id = engine.createDataset(DatasetDescriptor{.source_name = "dup", .time_domain_id = 0});
  ASSERT_TRUE(dataset_id.has_value());
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(*dataset_id, "dup", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value());
  const TopicId topic_id = handle->topic_id;

  ASSERT_TRUE(writer.beginRow(topic_id, 100).has_value());
  writer.set(topic_id, 0, 1.0);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  ASSERT_TRUE(writer.beginRow(topic_id, 50).has_value()) << "regressed row must be accepted";
  writer.set(topic_id, 0, 2.0);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  ASSERT_TRUE(writer.beginRow(topic_id, 100).has_value());
  writer.set(topic_id, 0, 3.0);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  // Stable sort: rows sharing t=100 keep arrival order (1.0 before 3.0).
  const auto rows = readAll(engine, topic_id);
  const std::vector<std::pair<Timestamp, double>> expected = {{50, 2.0}, {100, 1.0}, {100, 3.0}};
  EXPECT_EQ(rows, expected);
}

TEST(OutOfOrderIngestTest, ReorderPreservesStringsAndNulls) {
  DataEngine engine;
  auto value_node = makePrimitive("value", PrimitiveType::kFloat64);
  auto label_node = makePrimitive("label", PrimitiveType::kString);
  auto root = makeStruct("sample", {value_node, label_node});

  auto dataset_id = engine.createDataset(DatasetDescriptor{.source_name = "strs", .time_domain_id = 0});
  ASSERT_TRUE(dataset_id.has_value());
  DataWriter writer = engine.createWriter();
  auto schema_id = writer.registerSchema("sample", root);
  ASSERT_TRUE(schema_id.has_value());
  TopicDescriptor topic_desc;
  topic_desc.name = "samples";
  topic_desc.schema_id = *schema_id;
  auto topic_id = writer.registerTopic(*dataset_id, topic_desc);
  ASSERT_TRUE(topic_id.has_value());
  ASSERT_TRUE(writer.bindTopicWriter(*topic_id).has_value());

  // Arrival order 300, 100, 200; the t=200 row has a null value column.
  ASSERT_TRUE(writer.beginRow(*topic_id, 300).has_value());
  writer.set(*topic_id, 0, 3.0);
  writer.set(*topic_id, 1, std::string_view("c"));
  ASSERT_TRUE(writer.finishRow(*topic_id).has_value());
  ASSERT_TRUE(writer.beginRow(*topic_id, 100).has_value()) << "regressed row must be accepted";
  writer.set(*topic_id, 0, 1.0);
  writer.set(*topic_id, 1, std::string_view("a"));
  ASSERT_TRUE(writer.finishRow(*topic_id).has_value());
  ASSERT_TRUE(writer.beginRow(*topic_id, 200).has_value());
  writer.setNull(*topic_id, 0);
  writer.set(*topic_id, 1, std::string_view("b"));
  ASSERT_TRUE(writer.finishRow(*topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  // Each row's columns must travel together through the reorder.
  DataReader reader = engine.createReader();
  auto cursor_or = reader.rangeQuery(QueryRange{.topic_id = *topic_id, .t_min = 0, .t_max = 400});
  ASSERT_TRUE(cursor_or.has_value());
  std::vector<SampleRow> rows;
  cursor_or->forEach([&rows](const SampleRow& row) { rows.push_back(row); });
  ASSERT_EQ(rows.size(), 3U);

  EXPECT_EQ(rows[0].timestamp, 100);
  EXPECT_DOUBLE_EQ(rows[0].chunk->readNumericAsDouble(0, rows[0].row_index), 1.0);
  EXPECT_EQ(rows[0].chunk->readString(1, rows[0].row_index), "a");

  EXPECT_EQ(rows[1].timestamp, 200);
  EXPECT_EQ(rows[1].chunk->readString(1, rows[1].row_index), "b");
  std::vector<double> values(rows[1].chunk->timestamps.size());
  rows[1].chunk->readColumnAsDoubles(0, Span<double>(values.data(), values.size()), 0);
  EXPECT_TRUE(std::isnan(values[rows[1].row_index])) << "null must travel with its reordered row";

  EXPECT_EQ(rows[2].timestamp, 300);
  EXPECT_DOUBLE_EQ(rows[2].chunk->readNumericAsDouble(0, rows[2].row_index), 3.0);
  EXPECT_EQ(rows[2].chunk->readString(1, rows[2].row_index), "c");
}

}  // namespace
}  // namespace PJ
