// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "pj_base/type_tree.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

TopicChunk makeChunkWithRange(TopicId tid, Timestamp t_start, Timestamp t_end, uint32_t num_rows) {
  std::vector<ColumnDescriptor> cols = {{0, PrimitiveType::kFloat32, "v"}};
  TopicChunkBuilder b(tid, /*schema_id=*/1, cols, num_rows);
  Timestamp step = (num_rows > 1) ? (t_end - t_start) / static_cast<Timestamp>(num_rows - 1) : 0;
  for (uint32_t i = 0; i < num_rows; ++i) {
    b.beginRow(t_start + static_cast<Timestamp>(i) * step);
    b.set(0, static_cast<float>(i));
    b.finishRow();
  }
  return b.seal();
}

// flush() seals only committed rows: a row begun but not finished must not leak its timestamp into the sealed
// chunk's stats (t_min / t_max).
TEST(RegressionTest, Bug1_FlushWithRowInProgress_CorruptsChunkStats) {
  DataEngine engine;
  auto ds = *engine.createDataset(DatasetDescriptor{.source_name = "test", .time_domain_id = 0});
  DataWriter writer = engine.createWriter();

  auto schema = makePrimitive("v", PrimitiveType::kFloat64);
  auto sid = *writer.registerSchema("s", schema);
  auto tid = *writer.registerTopic(ds, TopicDescriptor{.name = "t", .schema_id = sid});

  // Commit one complete row at t=100.
  ASSERT_TRUE(writer.beginRow(tid, 100).has_value());
  writer.set(tid, 0, 1.0);
  ASSERT_TRUE(writer.finishRow(tid).has_value());

  // Start a second row at t=200 but do NOT call finishRow().
  ASSERT_TRUE(writer.beginRow(tid, 200).has_value());
  writer.set(tid, 0, 2.0);

  auto chunks = writer.flush(tid);

  ASSERT_EQ(chunks.size(), 1u);
  EXPECT_EQ(chunks[0].stats.row_count, 1u);
  // The in-progress row's timestamp (200) must not leak into the stats.
  EXPECT_EQ(chunks[0].stats.t_max, 100);
  EXPECT_EQ(chunks[0].timestamps.size(), 1u);
}

// finishBulkAppend() must fail loudly when a column holds fewer rows than the pending timestamps (N timestamps,
// N-1 values): otherwise the unsigned row-count subtraction wraps and the statistics loop reads out of bounds.
TEST(RegressionTest, Bug2_FinishBulkAppend_ColumnRowCountMismatch_TriggersUB) {
  // Declare test data outside the EXPECT_THROW block: the C preprocessor does
  // not track {} when splitting macro arguments, so vector-initializer commas
  // inside the block would be misinterpreted as extra macro arguments.
  std::vector<ColumnDescriptor> cols = {{1, PrimitiveType::kFloat32, "x"}};
  std::vector<Timestamp> ts;
  ts.push_back(100);
  ts.push_back(200);
  ts.push_back(300);
  std::vector<float> vals;
  vals.push_back(1.0f);
  vals.push_back(2.0f);  // one short: 2 values for 3 timestamps

  // PJ_ASSERT behaviour differs by build configuration:
  //   Debug/ASAN  (PJ_ASSERT_THROWS=ON, no NDEBUG): throws std::runtime_error
  //   Release (no PJ_ASSERT_THROWS, NDEBUG):         assert() compiled away → silent UB
  // The assertion is only verifiable in debug builds.
#ifdef PJ_ASSERT_THROWS
  EXPECT_THROW(
      {
        TopicChunkBuilder builder(/*topic_id=*/1, /*schema_id=*/1, cols, /*max_rows=*/100);
        builder.appendTimestamps(ts);
        builder.appendColumn<float>(0, vals);  // 2 rows appended, pending=3
        builder.finishBulkAppend();            // PJ_ASSERT fires → throws
      },
      std::exception);
#else
  // Release: NDEBUG disables assert(), so the check cannot be observed.
  GTEST_SKIP() << "Bug #2 assertion not verifiable in Release (NDEBUG disables assert())";
#endif
}

// An out-of-order chunk is retained, so commitChunks reports its topic as changed.
TEST(RegressionTest, Bug3_CommitChunks_OutOfOrderChunkRetainedAndReported) {
  DataEngine engine;
  auto ds = *engine.createDataset(DatasetDescriptor{.source_name = "test", .time_domain_id = 0});
  DataWriter writer = engine.createWriter();

  auto schema = makePrimitive("v", PrimitiveType::kFloat32);
  auto sid = *writer.registerSchema("s", schema);
  auto tid = *writer.registerTopic(ds, TopicDescriptor{.name = "t", .schema_id = sid});

  // First commit: chunk at t=[100, 200]. Accepted.
  std::vector<std::pair<TopicId, TopicChunk>> batch1;
  batch1.emplace_back(tid, makeChunkWithRange(tid, 100, 200, 2));
  auto changed1 = engine.commitChunks(std::move(batch1));
  ASSERT_EQ(changed1.size(), 1u);

  // Second commit: chunk at t=[50, 150] — out of order. Retained and reported.
  std::vector<std::pair<TopicId, TopicChunk>> batch2;
  batch2.emplace_back(tid, makeChunkWithRange(tid, 50, 150, 2));
  std::vector<TopicId> changed2;
  ASSERT_NO_THROW(changed2 = engine.commitChunks(std::move(batch2)));
  EXPECT_EQ(changed2, (std::vector<TopicId>{tid}));

  const TopicStorage* storage = engine.getTopicStorage(tid);
  ASSERT_NE(storage, nullptr);
  EXPECT_EQ(storage->sealedChunks().size(), 2u);
  EXPECT_EQ(storage->timeMin(), 50);
  EXPECT_EQ(storage->timeMax(), 200);
}

// Chunk time ranges may overlap after out-of-order ingest: latestAt() scans candidate chunks (the
// later-committed chunk wins timestamp ties) and the row cursor merges chunks into one globally time-ordered
// stream.
TEST(RegressionTest, Bug4_QueriesStayCorrectOverOverlappingChunks) {
  TopicDescriptor desc;
  desc.name = "t";
  desc.schema_id = 1;
  desc.dataset_id = 1;
  TopicStorage storage(/*topic_id=*/1, std::move(desc));

  // Chunk1: rows at 100,200,300,400,500. Chunk2: rows at 400,500,600 —
  // overlapping [400, 500].
  ASSERT_TRUE(storage.appendSealedChunk(makeChunkWithRange(1, 100, 500, 5)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeChunkWithRange(1, 400, 600, 3)).has_value());

  // Row cursor: one ascending stream across both chunks, duplicates included.
  std::vector<Timestamp> timestamps;
  rangeQuery(storage.sealedChunks(), 0, 1000).forEach([&timestamps](const SampleRow& row) {
    timestamps.push_back(row.timestamp);
  });
  EXPECT_EQ(timestamps, (std::vector<Timestamp>{100, 200, 300, 400, 400, 500, 500, 600}));

  // latestAt inside the overlap: both chunks hold a row at t=400; the
  // later-committed chunk wins the tie (commit order).
  const auto at_450 = latestAt(storage.sealedChunks(), 450);
  ASSERT_TRUE(at_450.has_value());
  EXPECT_EQ(at_450->timestamp, 400);
  EXPECT_EQ(at_450->chunk, &storage.sealedChunks()[1]);

  // latestAt past everything returns the global maximum.
  const auto at_end = latestAt(storage.sealedChunks(), 1000);
  ASSERT_TRUE(at_end.has_value());
  EXPECT_EQ(at_end->timestamp, 600);
}

}  // namespace
}  // namespace PJ
