// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/merge_result.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

DatasetId makeDataset(DataEngine& engine, const std::string& name) {
  auto id = engine.createDataset(DatasetDescriptor{.source_name = name, .time_domain_id = 0});
  EXPECT_TRUE(id.has_value()) << (id.has_value() ? "" : id.error());
  return *id;
}

// Single-column float64 scalar topic. Returns the topic id.
TopicId writeScalar(
    DataEngine& engine, DatasetId ds, const std::string& name, const std::vector<Timestamp>& ts,
    const std::vector<double>& vals) {
  DataWriter w = engine.createWriter();
  auto handle = w.registerScalarSeries(ds, name, NumericType::kFloat64);
  EXPECT_TRUE(handle.has_value()) << (handle.has_value() ? "" : handle.error());
  for (std::size_t i = 0; i < ts.size(); ++i) {
    w.appendScalar(*handle, ts[i], vals[i]);
  }
  engine.commitChunks(w.flushAll());
  return handle->topic_id;
}

// Multi-column schemaless topic. `columns` spans must reference vectors that
// outlive this call. Returns the topic id.
TopicId writeTyped(
    DataEngine& engine, DatasetId ds, const std::string& name,
    const std::vector<std::pair<std::string, PrimitiveType>>& fields, const std::vector<Timestamp>& ts,
    const std::vector<ColumnData>& columns) {
  DataWriter w = engine.createWriter();
  auto tid = w.registerTopic(ds, TopicDescriptor{.name = name, .schema_id = 0});
  EXPECT_TRUE(tid.has_value()) << (tid.has_value() ? "" : tid.error());
  for (const auto& [path, type] : fields) {
    EXPECT_TRUE(w.ensureColumn(*tid, path, type).has_value());
  }
  EXPECT_TRUE(
      w.appendColumns(
           *tid, Span<const Timestamp>(ts.data(), ts.size()), Span<const ColumnData>(columns.data(), columns.size()))
          .has_value());
  engine.commitChunks(w.flushAll());
  return *tid;
}

TopicId topicIdByName(const DataEngine& engine, DatasetId ds, const std::string& name) {
  for (const TopicId tid : engine.listTopics(ds)) {
    if (const TopicStorage* st = engine.getTopicStorage(tid); st != nullptr && st->descriptor().name == name) {
      return tid;
    }
  }
  return 0;
}

// All value-bearing (non-null) samples of one topic column, in series order.
std::vector<std::pair<Timestamp, double>> readColumn(const DataEngine& engine, TopicId tid, std::size_t col = 0) {
  std::vector<std::pair<Timestamp, double>> out;
  DataReader reader = engine.createReader();
  auto series = reader.series(tid, col);
  if (!series.has_value()) {
    return out;
  }
  for (std::size_t i = 0; i < series->size(); ++i) {
    if (auto sample = series->sampleAt(i)) {
      out.emplace_back(sample->timestamp, sample->value);
    }
  }
  return out;
}

bool datasetEmpty(const DataEngine& engine, DatasetId ds) {
  for (const TopicId tid : engine.listTopics(ds)) {
    if (const TopicStorage* st = engine.getTopicStorage(tid); st != nullptr && !st->empty()) {
      return false;
    }
  }
  return true;
}

// --- Tests --------------------------------------------------------------------

TEST(MergeTest, NonOverlapSameSchemaConcatenates) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  const TopicId a_s = writeScalar(engine, a, "s", {0, 1000, 2000}, {0.0, 1.0, 2.0});
  writeScalar(engine, b, "s", {0, 1000, 2000}, {10.0, 11.0, 12.0});

  auto report = engine.mergeDatasets(a, {{b, 5000}});
  ASSERT_TRUE(report.has_value()) << (report.has_value() ? "" : report.error());

  const auto rows = readColumn(engine, a_s);
  const std::vector<std::pair<Timestamp, double>> expected = {{0, 0.0},     {1000, 1.0},  {2000, 2.0},
                                                              {5000, 10.0}, {6000, 11.0}, {7000, 12.0}};
  EXPECT_EQ(rows, expected);
  EXPECT_TRUE(datasetEmpty(engine, b));
  EXPECT_EQ(report->consumed_datasets, std::vector<DatasetId>{b});
  EXPECT_EQ(report->modified_topics, std::vector<TopicId>{a_s});
}

TEST(MergeTest, TimeOverlapInterleavesAllSamples) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  const TopicId a_s = writeScalar(engine, a, "s", {0, 2000, 4000}, {0.0, 2.0, 4.0});
  writeScalar(engine, b, "s", {1000, 3000}, {10.0, 13.0});

  ASSERT_TRUE(engine.mergeDatasets(a, {{b, 0}}).has_value());

  const auto rows = readColumn(engine, a_s);
  const std::vector<std::pair<Timestamp, double>> expected = {
      {0, 0.0}, {1000, 10.0}, {2000, 2.0}, {3000, 13.0}, {4000, 4.0}};
  EXPECT_EQ(rows, expected);
}

TEST(MergeTest, DuplicateTimestampsCoexist) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  const TopicId a_s = writeScalar(engine, a, "s", {0, 2000}, {0.0, 2.0});
  writeScalar(engine, b, "s", {2000}, {99.0});

  ASSERT_TRUE(engine.mergeDatasets(a, {{b, 0}}).has_value());

  const auto rows = readColumn(engine, a_s);
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(rows[0], (std::pair<Timestamp, double>{0, 0.0}));
  // Both samples at t=2000 survive; anchor first (stable order).
  EXPECT_EQ(rows[1], (std::pair<Timestamp, double>{2000, 2.0}));
  EXPECT_EQ(rows[2], (std::pair<Timestamp, double>{2000, 99.0}));
}

TEST(MergeTest, DisjointTopicsUnion) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  const TopicId a_only = writeScalar(engine, a, "only_a", {0, 1000}, {1.0, 2.0});
  writeScalar(engine, b, "only_b", {0, 1000}, {3.0, 4.0});

  auto report = engine.mergeDatasets(a, {{b, 0}});
  ASSERT_TRUE(report.has_value());

  const TopicId merged_b = topicIdByName(engine, a, "only_b");
  ASSERT_NE(merged_b, 0U);
  EXPECT_EQ(report->added_topics, std::vector<TopicId>{merged_b});
  EXPECT_EQ(readColumn(engine, a_only).size(), 2U);    // anchor's own topic untouched
  EXPECT_EQ(readColumn(engine, merged_b).size(), 2U);  // source topic carried over
  EXPECT_EQ(readColumn(engine, merged_b)[1], (std::pair<Timestamp, double>{1000, 4.0}));
}

TEST(MergeTest, AdditiveColumnUnionNullFills) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  const std::vector<double> ax = {1.0, 2.0};
  const std::vector<double> bx = {3.0, 4.0};
  const std::vector<double> by = {30.0, 40.0};
  const TopicId a_m = writeTyped(
      engine, a, "m", {{"x", PrimitiveType::kFloat64}}, {0, 1000},
      {ColumnData::float64(0, Span<const double>(ax.data(), ax.size()))});
  writeTyped(
      engine, b, "m", {{"x", PrimitiveType::kFloat64}, {"y", PrimitiveType::kFloat64}}, {0, 1000},
      {ColumnData::float64(0, Span<const double>(bx.data(), bx.size())),
       ColumnData::float64(1, Span<const double>(by.data(), by.size()))});

  ASSERT_TRUE(engine.mergeDatasets(a, {{b, 2000}}).has_value());

  // Column x: all four rows (A then shifted B).
  const auto col_x = readColumn(engine, a_m, 0);
  const std::vector<std::pair<Timestamp, double>> expected_x = {{0, 1.0}, {1000, 2.0}, {2000, 3.0}, {3000, 4.0}};
  EXPECT_EQ(col_x, expected_x);
  // Column y: only the two B rows (A's rows are null and skipped by SeriesReader).
  const auto col_y = readColumn(engine, a_m, 1);
  const std::vector<std::pair<Timestamp, double>> expected_y = {{2000, 30.0}, {3000, 40.0}};
  EXPECT_EQ(col_y, expected_y);
}

TEST(MergeTest, TypeConflictSkipsSourceContribution) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  const std::vector<double> av = {1.5, 2.5};
  const std::vector<int64_t> bv = {10, 20};
  const TopicId a_t = writeTyped(
      engine, a, "t", {{"v", PrimitiveType::kFloat64}}, {0, 1000},
      {ColumnData::float64(0, Span<const double>(av.data(), av.size()))});
  writeTyped(
      engine, b, "t", {{"v", PrimitiveType::kInt64}}, {0, 1000},
      {ColumnData::int64(0, Span<const int64_t>(bv.data(), bv.size()))});

  auto report = engine.mergeDatasets(a, {{b, 5000}});
  ASSERT_TRUE(report.has_value());

  EXPECT_EQ(report->skipped_topics, std::vector<std::string>{"t"});
  // Anchor's topic is untouched: only its two float rows remain.
  const auto rows = readColumn(engine, a_t);
  const std::vector<std::pair<Timestamp, double>> expected = {{0, 1.5}, {1000, 2.5}};
  EXPECT_EQ(rows, expected);
  EXPECT_TRUE(datasetEmpty(engine, b));  // source still consumed
}

TEST(MergeTest, ThreeWayFold) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  const DatasetId c = makeDataset(engine, "C");
  const TopicId a_s = writeScalar(engine, a, "s", {0}, {0.0});
  writeScalar(engine, b, "s", {0}, {10.0});
  writeScalar(engine, c, "s", {0}, {20.0});

  auto report = engine.mergeDatasets(a, {{b, 1000}, {c, 2000}});
  ASSERT_TRUE(report.has_value());

  const auto rows = readColumn(engine, a_s);
  const std::vector<std::pair<Timestamp, double>> expected = {{0, 0.0}, {1000, 10.0}, {2000, 20.0}};
  EXPECT_EQ(rows, expected);
  EXPECT_EQ(report->consumed_datasets.size(), 2U);
  EXPECT_TRUE(datasetEmpty(engine, b));
  EXPECT_TRUE(datasetEmpty(engine, c));
}

TEST(MergeTest, AnchorTopicIdPreserved) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  const TopicId a_s = writeScalar(engine, a, "s", {0, 1000}, {0.0, 1.0});
  writeScalar(engine, b, "s", {0}, {5.0});

  ASSERT_TRUE(engine.mergeDatasets(a, {{b, 2000}}).has_value());

  EXPECT_EQ(topicIdByName(engine, a, "s"), a_s);  // stable id => curve keys survive
  EXPECT_EQ(readColumn(engine, a_s).size(), 3U);
}

TEST(MergeTest, ValidationErrors) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  writeScalar(engine, a, "s", {0}, {0.0});

  EXPECT_FALSE(engine.mergeDatasets(999, {}).has_value());        // unknown anchor
  EXPECT_FALSE(engine.mergeDatasets(a, {{a, 0}}).has_value());    // source == anchor
  EXPECT_FALSE(engine.mergeDatasets(a, {{999, 0}}).has_value());  // unknown source
}

TEST(MergeTest, DuplicateSourceIdRejected) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  writeScalar(engine, a, "s", {0}, {0.0});
  writeScalar(engine, b, "s", {0}, {10.0});

  // Listing b twice would fold its samples twice; rejected at the boundary, with
  // nothing mutated (b keeps its data, a keeps only its single sample).
  EXPECT_FALSE(engine.mergeDatasets(a, {{b, 0}, {b, 1000}}).has_value());
  EXPECT_FALSE(datasetEmpty(engine, b));
  EXPECT_EQ(readColumn(engine, topicIdByName(engine, a, "s")).size(), 1U);
}

TEST(MergeTest, NegativeShiftInterleavesBeforeAnchorStaysAscending) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  // The production caller routinely emits NEGATIVE shifts (raw_shift =
  // anchorOffset - sourceOffset). Here source {1000,3000} shifts by -1500 ->
  // {-500,1500}, landing before the anchor's first sample — the case most able
  // to break the ascending/non-overlapping invariant the SeriesReader path needs.
  const TopicId a_s = writeScalar(engine, a, "s", {0, 2000, 4000}, {0.0, 2.0, 4.0});
  writeScalar(engine, b, "s", {1000, 3000}, {10.0, 13.0});

  ASSERT_TRUE(engine.mergeDatasets(a, {{b, -1500}}).has_value());

  const auto rows = readColumn(engine, a_s);
  const std::vector<std::pair<Timestamp, double>> expected = {
      {-500, 10.0}, {0, 0.0}, {1500, 13.0}, {2000, 2.0}, {4000, 4.0}};
  EXPECT_EQ(rows, expected);
  for (std::size_t i = 1; i < rows.size(); ++i) {
    EXPECT_LE(rows[i - 1].first, rows[i].first) << "merged column not ascending at row " << i;
  }
}

TEST(MergeTest, EmptyAnchorTopicKeepsItsColumnOrder) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  // Anchor topic "m" exists but is EMPTY, schema'd with columns [x@0, y@1].
  {
    DataWriter w = engine.createWriter();
    auto tid = w.registerTopic(a, TopicDescriptor{.name = "m", .schema_id = 0});
    ASSERT_TRUE(tid.has_value());
    ASSERT_TRUE(w.ensureColumn(*tid, "x", PrimitiveType::kFloat64).has_value());
    ASSERT_TRUE(w.ensureColumn(*tid, "y", PrimitiveType::kFloat64).has_value());
    engine.commitChunks(w.flushAll());  // no rows -> empty topic, descriptors [x,y]
  }
  const TopicId a_m = topicIdByName(engine, a, "m");
  ASSERT_NE(a_m, 0U);
  // The source declares the SAME fields in the OPPOSITE order [y@0, x@1].
  const std::vector<double> sy = {30.0, 40.0};
  const std::vector<double> sx = {3.0, 4.0};
  writeTyped(
      engine, b, "m", {{"y", PrimitiveType::kFloat64}, {"x", PrimitiveType::kFloat64}}, {0, 1000},
      {ColumnData::float64(0, Span<const double>(sy.data(), sy.size())),
       ColumnData::float64(1, Span<const double>(sx.data(), sx.size()))});

  ASSERT_TRUE(engine.mergeDatasets(a, {{b, 0}}).has_value());

  // The anchor's advertised order wins (x stays column 0), so a curve keyed on
  // (a_m, 0) still reads x — not the source's y. The topic id is preserved too.
  EXPECT_EQ(topicIdByName(engine, a, "m"), a_m);
  const auto col_x = readColumn(engine, a_m, 0);
  const std::vector<std::pair<Timestamp, double>> expected_x = {{0, 3.0}, {1000, 4.0}};
  EXPECT_EQ(col_x, expected_x);
  const auto col_y = readColumn(engine, a_m, 1);
  const std::vector<std::pair<Timestamp, double>> expected_y = {{0, 30.0}, {1000, 40.0}};
  EXPECT_EQ(col_y, expected_y);
}

// Rows below a contributor's retention floor are logically evicted (their
// chunk may still physically straddle the floor): a merge must not revive them,
// neither from a source nor from the anchor it rebuilds.
TEST(MergeTest, RowsBelowRetentionFloorAreNotRevived) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  const TopicId a_s = writeScalar(engine, a, "s", {0, 1000, 2000}, {0.0, 1.0, 2.0});
  const TopicId b_s = writeScalar(engine, b, "s", {0, 1000, 2000}, {10.0, 11.0, 12.0});
  engine.getTopicStorage(a_s)->evictBefore(500);   // anchor keeps 1000, 2000
  engine.getTopicStorage(b_s)->evictBefore(1500);  // source keeps 2000
  ASSERT_EQ(engine.getTopicStorage(b_s)->sealedChunks().size(), 1U) << "the chunk straddles the floor";

  auto report = engine.mergeDatasets(a, {{b, 5000}});
  ASSERT_TRUE(report.has_value()) << (report.has_value() ? "" : report.error());

  const std::vector<std::pair<Timestamp, double>> expected = {{1000, 1.0}, {2000, 2.0}, {7000, 12.0}};
  EXPECT_EQ(readColumn(engine, a_s), expected);
}

// Timestamp arithmetic at the int64 limits clamps instead of overflowing (UB).
TEST(MergeTest, ShiftNearInt64MaxSaturates) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  const DatasetId b = makeDataset(engine, "B");
  constexpr Timestamp kMax = std::numeric_limits<Timestamp>::max();
  const TopicId a_s = writeScalar(engine, a, "s", {0}, {0.0});
  writeScalar(engine, b, "s", {kMax - 10}, {1.0});

  auto report = engine.mergeDatasets(a, {{b, 100}});
  ASSERT_TRUE(report.has_value()) << (report.has_value() ? "" : report.error());

  const std::vector<std::pair<Timestamp, double>> expected = {{0, 0.0}, {kMax, 1.0}};
  EXPECT_EQ(readColumn(engine, a_s), expected);
}

TEST(MergeTest, RetentionWindowWiderThanTheTimelineDoesNotOverflow) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  // t_max = -2, so t_max - INT64_MAX is one below INT64_MIN: unclamped it wraps
  // to INT64_MAX and evicts everything.
  const TopicId a_s = writeScalar(engine, a, "s", {-3, -2}, {1.0, 2.0});
  engine.enforceRetention(std::numeric_limits<Timestamp>::max());
  const std::vector<std::pair<Timestamp, double>> expected = {{-3, 1.0}, {-2, 2.0}};
  EXPECT_EQ(readColumn(engine, a_s), expected);
}

// evictTopicHistory() drops everything up to the newest sample; at INT64_MAX the
// "newest + 1" cutoff must not overflow.
TEST(MergeTest, EvictTopicHistoryAtInt64MaxDoesNotOverflow) {
  DataEngine engine;
  const DatasetId a = makeDataset(engine, "A");
  constexpr Timestamp kMax = std::numeric_limits<Timestamp>::max();
  const TopicId a_s = writeScalar(engine, a, "s", {0, kMax}, {1.0, 2.0});
  engine.evictTopicHistory(a_s);
  // The cutoff saturates at INT64_MAX: everything older is gone, and nothing
  // wraps around to resurrect the past.
  const auto rows = readColumn(engine, a_s);
  for (const auto& [ts, value] : rows) {
    EXPECT_EQ(ts, kMax) << "value " << value;
  }
}

}  // namespace
}  // namespace PJ
