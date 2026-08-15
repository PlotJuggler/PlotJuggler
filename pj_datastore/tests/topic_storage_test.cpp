// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/topic_storage.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_datastore/chunk.hpp"

namespace PJ {
namespace {

// ---------------------------------------------------------------------------
// Helper: build and seal a test chunk with given time range
// ---------------------------------------------------------------------------

TopicChunk makeTestChunk(TopicId topic_id, Timestamp t_start, Timestamp t_end, uint32_t num_rows) {
  std::vector<ColumnDescriptor> cols = {{0, PrimitiveType::kFloat32, "value"}};
  TopicChunkBuilder builder(topic_id, /*schema_id=*/1, cols, num_rows);
  Timestamp step = (num_rows > 1) ? (t_end - t_start) / static_cast<Timestamp>(num_rows - 1) : 0;
  for (uint32_t i = 0; i < num_rows; ++i) {
    builder.beginRow(t_start + static_cast<Timestamp>(i) * step);
    builder.set(0, static_cast<float>(i));
    builder.finishRow();
  }
  return builder.seal();
}

// ===========================================================================
// Test 1: Append chunks
// ===========================================================================

TEST(TopicStorageTest, AppendChunks) {
  TopicDescriptor desc;
  desc.name = "test_topic";
  desc.schema_id = 1;
  desc.dataset_id = 10;

  TopicStorage storage(/*topic_id=*/1, std::move(desc));

  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(1, 1000, 1900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(1, 2000, 2900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(1, 3000, 3900, 10)).has_value());

  EXPECT_EQ(storage.sealedChunks().size(), 3U);
}

// ===========================================================================
// Test 2: time_min / time_max
// ===========================================================================

TEST(TopicStorageTest, TimeMinMax) {
  TopicDescriptor desc;
  desc.name = "time_range_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/2, std::move(desc));

  // Empty storage returns 0
  EXPECT_EQ(storage.timeMin(), 0);
  EXPECT_EQ(storage.timeMax(), 0);

  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(2, 1000, 1900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(2, 2000, 2900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(2, 3000, 3900, 10)).has_value());

  EXPECT_EQ(storage.timeMin(), 1000);
  EXPECT_EQ(storage.timeMax(), 3900);
}

// ===========================================================================
// Test 3: Evict none
// ===========================================================================

TEST(TopicStorageTest, EvictNone) {
  TopicDescriptor desc;
  desc.name = "evict_none_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/3, std::move(desc));

  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(3, 1000, 1900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(3, 2000, 2900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(3, 3000, 3900, 10)).has_value());

  // Evict before the first chunk's t_min -- nothing should be removed
  storage.evictBefore(500);
  EXPECT_EQ(storage.sealedChunks().size(), 3U);

  // Evict at exactly the first chunk's t_min -- still nothing removed
  // because t_max (1900) is not < 1000
  storage.evictBefore(1000);
  EXPECT_EQ(storage.sealedChunks().size(), 3U);
}

// ===========================================================================
// Test 4: Evict some
// ===========================================================================

TEST(TopicStorageTest, EvictSome) {
  TopicDescriptor desc;
  desc.name = "evict_some_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/4, std::move(desc));

  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(4, 1000, 1900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(4, 2000, 2900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(4, 3000, 3900, 10)).has_value());

  // Evict chunks whose t_max < 2500
  // Chunk 1 (t_max=1900 < 2500) -> evicted
  // Chunk 2 (t_max=2900 >= 2500) -> kept
  // Chunk 3 (t_max=3900 >= 2500) -> kept
  storage.evictBefore(2500);
  EXPECT_EQ(storage.sealedChunks().size(), 2U);
  // Chunk 2 straddles the cutoff (t_min=2000 < 2500 <= t_max=2900): it survives
  // whole-chunk eviction, but the retention floor hides its rows below 2500, so
  // the visible minimum is the cutoff, not the chunk's physical t_min (2000).
  EXPECT_EQ(storage.retentionFloor(), 2500);
  EXPECT_EQ(storage.timeMin(), 2500);
  EXPECT_EQ(storage.timeMax(), 3900);
}

// ===========================================================================
// Test 5: Evict all
// ===========================================================================

TEST(TopicStorageTest, EvictAll) {
  TopicDescriptor desc;
  desc.name = "evict_all_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/5, std::move(desc));

  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(5, 1000, 1900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(5, 2000, 2900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(5, 3000, 3900, 10)).has_value());

  // Evict with t_keep_min beyond all chunks
  storage.evictBefore(5000);
  EXPECT_TRUE(storage.empty());
  EXPECT_EQ(storage.sealedChunks().size(), 0U);
  EXPECT_EQ(storage.timeMin(), 0);
  EXPECT_EQ(storage.timeMax(), 0);
}

// ===========================================================================
// Test 6: Metadata
// ===========================================================================

TEST(TopicStorageTest, Metadata) {
  TopicDescriptor desc;
  desc.name = "metadata_topic";
  desc.schema_id = 42;
  desc.dataset_id = 7;

  TopicStorage storage(/*topic_id=*/6, std::move(desc));

  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(6, 1000, 1900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(6, 2000, 2900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(6, 3000, 3900, 10)).has_value());

  TopicMetadata meta = storage.metadata();

  EXPECT_EQ(meta.topic_id, 6U);
  EXPECT_EQ(meta.name, "metadata_topic");
  EXPECT_EQ(meta.current_schema, 42U);
  EXPECT_EQ(meta.dataset_id, 7U);
  EXPECT_EQ(meta.time_range_min, 1000);
  EXPECT_EQ(meta.time_range_max, 3900);
  EXPECT_EQ(meta.total_row_count, 30U);
  EXPECT_GT(meta.total_byte_size, 0U);
}

// ===========================================================================
// Test 7: Empty
// ===========================================================================

TEST(TopicStorageTest, Empty) {
  TopicDescriptor desc;
  desc.name = "empty_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/7, std::move(desc));

  // Initially empty
  EXPECT_TRUE(storage.empty());

  // After appending, not empty
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(7, 1000, 1900, 10)).has_value());
  EXPECT_FALSE(storage.empty());

  // After evicting all, empty again
  storage.evictBefore(5000);
  EXPECT_TRUE(storage.empty());
}

// ===========================================================================
// Test 8: Update schema
// ===========================================================================

TEST(TopicStorageTest, UpdateSchema) {
  TopicDescriptor desc;
  desc.name = "schema_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/8, std::move(desc));

  EXPECT_EQ(storage.descriptor().schema_id, 1U);

  storage.updateSchema(42);
  EXPECT_EQ(storage.descriptor().schema_id, 42U);

  // Metadata should reflect the updated schema
  TopicMetadata meta = storage.metadata();
  EXPECT_EQ(meta.current_schema, 42U);
}

// ===========================================================================
// Test 9: Accept out-of-order chunk (lossless out-of-order ingest)
// ===========================================================================

TEST(TopicStorageTest, AcceptsOutOfOrderChunk) {
  TopicDescriptor desc;
  desc.name = "order_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/9, std::move(desc));

  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(9, 2000, 2900, 10)).has_value());

  // A chunk entirely before the previous one is retained — rejecting it
  // silently lost late-committed data (multi-publisher recordings).
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(9, 1000, 1900, 10)).has_value());

  EXPECT_EQ(storage.sealedChunks().size(), 2U);
  // Topic extrema scan all chunks instead of trusting front/back order.
  EXPECT_EQ(storage.timeMin(), 1000);
  EXPECT_EQ(storage.timeMax(), 2900);
}

// ===========================================================================
// Test 10: Accept overlapping chunk, including equal t_min
// ===========================================================================

TEST(TopicStorageTest, AcceptsOverlappingChunk_SameTMin) {
  // A chunk whose t_min falls inside the previous chunk's [t_min, t_max] is
  // retained; queries merge across overlapping chunks.
  TopicDescriptor desc;
  desc.name = "overlap_tmin_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/10, std::move(desc));

  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(10, 1000, 1900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(10, 1000, 1500, 5)).has_value());

  EXPECT_EQ(storage.sealedChunks().size(), 2U);
  EXPECT_EQ(storage.metadata().total_row_count, 15U);
}

// ===========================================================================
// Test 11: Retention floor raised by a straddling eviction
// ===========================================================================

TEST(TopicStorageTest, RetentionFloorRaisedByStraddlingEvict) {
  TopicDescriptor desc;
  desc.name = "floor_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/11, std::move(desc));

  // A small, fully-old chunk plus one BIG chunk that spans the whole tail.
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(11, 1000, 1900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(11, 2000, 5000, 31)).has_value());

  // Evict before 3000 — INSIDE the big chunk (t_min=2000 < 3000 <= t_max=5000).
  storage.evictBefore(3000);

  // The small chunk (t_max=1900 < 3000) is physically dropped. The big chunk is
  // kept ENTIRELY (whole-chunk eviction can't trim it), but the floor rises to
  // the cutoff and hides everything below it.
  EXPECT_EQ(storage.sealedChunks().size(), 1U);
  EXPECT_EQ(storage.retentionFloor(), 3000);
  EXPECT_EQ(storage.timeMin(), 3000);                  // clamped to floor, NOT physical t_min (2000)
  EXPECT_EQ(storage.metadata().time_range_min, 3000);  // metadata clamps too
  EXPECT_EQ(storage.timeMax(), 5000);                  // unchanged
  EXPECT_EQ(storage.metadata().time_range_max, 5000);
}

// ===========================================================================
// Test 12: Retention floor is monotonic
// ===========================================================================

TEST(TopicStorageTest, RetentionFloorIsMonotonic) {
  TopicDescriptor desc;
  desc.name = "floor_mono_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/12, std::move(desc));
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(12, 1000, 5000, 41)).has_value());

  storage.evictBefore(3000);
  EXPECT_EQ(storage.retentionFloor(), 3000);

  // A later eviction with an EARLIER cutoff must not lower the floor.
  storage.evictBefore(2000);
  EXPECT_EQ(storage.retentionFloor(), 3000);
  EXPECT_EQ(storage.timeMin(), 3000);
}

// ===========================================================================
// Test 13: clearChunks() resets the retention floor
// ===========================================================================

TEST(TopicStorageTest, ClearChunksResetsRetentionFloor) {
  TopicDescriptor desc;
  desc.name = "floor_clear_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/13, std::move(desc));
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(13, 1000, 5000, 41)).has_value());
  storage.evictBefore(3000);
  ASSERT_EQ(storage.retentionFloor(), 3000);

  // A full replace/reload (replaceDatasetFrom -> clearChunks) has no retained
  // data, so it must carry no stale floor.
  storage.clearChunks();
  EXPECT_EQ(storage.retentionFloor(), kNoRetentionFloor);
}

// ===========================================================================
// Test: a default (un-evicted) topic must not clamp negative timestamps
// ===========================================================================

TEST(TopicStorageTest, NoFloorDoesNotClampNegativeTimestamps) {
  // Timestamp is signed: a topic with negative timestamps and NO eviction must
  // report its true (negative) minimum. The "no floor" sentinel is
  // numeric_limits<Timestamp>::min(), NOT 0 (0 is a valid stamp).
  TopicDescriptor desc;
  desc.name = "neg_ts_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/15, std::move(desc));
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(15, -1000, -100, 10)).has_value());

  EXPECT_EQ(storage.retentionFloor(), kNoRetentionFloor);
  EXPECT_EQ(storage.timeMin(), -1000);
  EXPECT_EQ(storage.metadata().time_range_min, -1000);
}

// ===========================================================================
// Test 14: Two straddling chunks under out-of-order ingest
// ===========================================================================

TEST(TopicStorageTest, RetentionFloorTwoStraddlingChunksOutOfOrder) {
  // Out-of-order ingest can leave more than one chunk straddling the cutoff.
  // The floor (a single clamp) handles them uniformly.
  TopicDescriptor desc;
  desc.name = "floor_ooo_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/14, std::move(desc));
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(14, 2000, 4000, 21)).has_value());  // straddles 3000
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(14, 1000, 3500, 26)).has_value());  // OOO, also straddles 3000

  storage.evictBefore(3000);

  // Neither chunk has t_max < 3000, so both are physically retained...
  EXPECT_EQ(storage.sealedChunks().size(), 2U);
  // ...but the floor hides everything below 3000.
  EXPECT_EQ(storage.retentionFloor(), 3000);
  EXPECT_EQ(storage.timeMin(), 3000);
  EXPECT_EQ(storage.metadata().time_range_min, 3000);
}

// ===========================================================================
// Test 15: detachChunks() empties the storage exactly like clearChunks()
// ===========================================================================

TEST(TopicStorageTest, DetachChunksLeavesStorageEmptyLikeClear) {
  TopicDescriptor desc;
  desc.name = "detach_empty_topic";
  desc.schema_id = 1;

  TopicStorage storage(/*topic_id=*/16, std::move(desc));
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(16, 1000, 5000, 41)).has_value());
  storage.evictBefore(3000);
  ASSERT_EQ(storage.retentionFloor(), 3000);

  TopicStorage::DetachedState detached = storage.detachChunks();

  // Same observable state a clearChunks() would leave: no rows anywhere, no stale floor.
  EXPECT_TRUE(storage.empty());
  EXPECT_TRUE(storage.sealedChunks().empty());
  EXPECT_EQ(storage.retentionFloor(), kNoRetentionFloor);
  EXPECT_EQ(storage.timeMin(), 0);
  EXPECT_EQ(storage.timeMax(), 0);
  EXPECT_EQ(storage.metadata().total_row_count, 0U);
  // The rows moved OUT, they were not dropped.
  EXPECT_EQ(detached.chunks.size(), 1U);
}

// ===========================================================================
// Test 16: restoreChunks() puts back every field detachChunks() captured
// ===========================================================================

TEST(TopicStorageTest, DetachRestoreRoundTripsChunksAndMutableMetadata) {
  TopicDescriptor desc;
  desc.name = "detach_restore_topic";
  desc.schema_id = 1;
  desc.dataset_id = 3;

  TopicStorage storage(/*topic_id=*/17, std::move(desc));
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(17, 1000, 1900, 10)).has_value());
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(17, 2000, 5000, 31)).has_value());
  storage.setColumnDescriptors({{0, PrimitiveType::kFloat32, "value"}});
  storage.evictBefore(2500);  // drops the first chunk, floors the straddling second one
  storage.updateMaxObservedArrayLength(7);
  storage.incrementTruncatedSampleCount();
  storage.incrementTruncatedSampleCount();
  storage.setArrayExpansionCount("points", 5);

  const std::size_t chunk_count = storage.sealedChunks().size();
  const Timestamp t_min = storage.timeMin();
  const Timestamp t_max = storage.timeMax();
  const uint64_t row_count = storage.metadata().total_row_count;
  ASSERT_EQ(chunk_count, 1U);

  TopicStorage::DetachedState detached = storage.detachChunks();

  // Mutate every restorable field, including the ones clearChunks() leaves alone — a failed
  // write may have rewritten the column layout, the ratchets, or even the active schema id.
  ASSERT_TRUE(storage.appendSealedChunk(makeTestChunk(17, 9000, 9900, 4)).has_value());
  storage.setColumnDescriptors({{0, PrimitiveType::kInt64, "other"}, {1, PrimitiveType::kInt64, "extra"}});
  storage.evictBefore(9500);
  storage.updateSchema(42);
  storage.updateMaxObservedArrayLength(99);
  storage.incrementTruncatedSampleCount();
  storage.setArrayExpansionCount("points", 12);
  storage.setArrayExpansionCount("added_later", 3);

  storage.restoreChunks(std::move(detached));

  EXPECT_EQ(storage.sealedChunks().size(), chunk_count);
  EXPECT_EQ(storage.timeMin(), t_min);
  EXPECT_EQ(storage.timeMax(), t_max);
  EXPECT_EQ(storage.metadata().total_row_count, row_count);
  EXPECT_EQ(storage.metadata().time_range_min, t_min);
  EXPECT_EQ(storage.retentionFloor(), 2500);
  EXPECT_EQ(storage.descriptor().schema_id, 1U);  // the updateSchema(42) is undone too
  EXPECT_EQ(storage.metadata().current_schema, 1U);
  ASSERT_EQ(storage.columnDescriptors().size(), 1U);
  EXPECT_EQ(storage.columnDescriptors()[0].field_path, "value");
  EXPECT_EQ(storage.maxObservedArrayLength(), 7U);  // ratchet restored DOWN from 99
  EXPECT_EQ(storage.truncatedSampleCount(), 2U);
  EXPECT_EQ(storage.arrayExpansionCount("points"), 5U);
  EXPECT_EQ(storage.arrayExpansionCount("added_later"), 0U);  // entries added after the detach are gone

  // The restored rows are readable, not just counted.
  const TopicChunk& restored = storage.sealedChunks().front();
  EXPECT_EQ(restored.stats.t_min, 2000);
  EXPECT_EQ(restored.stats.t_max, 5000);
  EXPECT_EQ(restored.readNumericAsDouble(0, 0), 0.0);
  EXPECT_EQ(restored.readNumericAsDouble(0, 30), 30.0);
}

}  // namespace
}  // namespace PJ
