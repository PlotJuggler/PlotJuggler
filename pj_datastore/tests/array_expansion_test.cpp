// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"

using namespace PJ;

namespace {

TEST(ArrayExpansionTest, FixedSizeArray_Primitive_ProducesNColumns) {
  // Schema: struct msg { float32[3] accel }
  // Expected: 3 columns named accel[0], accel[1], accel[2]
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto accel = PJ::makeArray("accel", PJ::makePrimitive("", PJ::PrimitiveType::kFloat32), 3u);
  auto root = PJ::makeStruct("msg", {accel});
  auto sid = *writer.registerSchema("msg_fixed", root);

  TopicDescriptor desc;
  desc.name = "imu";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto handle = *writer.bindTopicWriter(topic_id);
  ASSERT_EQ(handle.field_ids.size(), 3u);

  EXPECT_EQ(*writer.resolveField(topic_id, "accel[0]"), 0u);
  EXPECT_EQ(*writer.resolveField(topic_id, "accel[1]"), 1u);
  EXPECT_EQ(*writer.resolveField(topic_id, "accel[2]"), 2u);
}

TEST(ArrayExpansionTest, FixedSizeArray_StructElement_ProducesNxMColumns) {
  // Schema: struct msg { Pose[2] poses } where Pose = struct { float32 x, y, z }
  // Expected: 6 columns: poses[0].x, poses[0].y, poses[0].z, poses[1].x, poses[1].y, poses[1].z
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  // Element type: anonymous struct with x, y, z
  auto pose_elem = PJ::makeStruct(
      "", {
              PJ::makePrimitive("x", PJ::PrimitiveType::kFloat32),
              PJ::makePrimitive("y", PJ::PrimitiveType::kFloat32),
              PJ::makePrimitive("z", PJ::PrimitiveType::kFloat32),
          });
  auto poses = PJ::makeArray("poses", pose_elem, 2u);
  auto root = PJ::makeStruct("msg", {poses});
  auto sid = *writer.registerSchema("msg_struct_arr", root);

  TopicDescriptor desc;
  desc.name = "poses_topic";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto handle = *writer.bindTopicWriter(topic_id);
  ASSERT_EQ(handle.field_ids.size(), 6u);

  EXPECT_EQ(*writer.resolveField(topic_id, "poses[0].x"), 0u);
  EXPECT_EQ(*writer.resolveField(topic_id, "poses[0].y"), 1u);
  EXPECT_EQ(*writer.resolveField(topic_id, "poses[0].z"), 2u);
  EXPECT_EQ(*writer.resolveField(topic_id, "poses[1].x"), 3u);
  EXPECT_EQ(*writer.resolveField(topic_id, "poses[1].y"), 4u);
  EXPECT_EQ(*writer.resolveField(topic_id, "poses[1].z"), 5u);
}

TEST(ArrayExpansionTest, VarLenArray_InitiallyZeroColumns) {
  // Schema: struct msg { float64[] data }
  // Variable-length: no fixed_size → 0 columns until expandArray() is called
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer.registerSchema("msg_varlen", root);

  TopicDescriptor desc;
  desc.name = "varlen_topic";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto handle = *writer.bindTopicWriter(topic_id);
  EXPECT_EQ(handle.field_ids.size(), 0u);  // 0 columns initially
}

TEST(ArrayExpansionTest, FixedSizeArray_WriteAndRead) {
  // Schema: struct msg { float32[3] accel }
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto accel = PJ::makeArray("accel", PJ::makePrimitive("", PJ::PrimitiveType::kFloat32), 3u);
  auto root = PJ::makeStruct("msg", {accel});
  auto sid = *writer.registerSchema("msg_wr", root);

  TopicDescriptor desc;
  desc.name = "imu_wr";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(writer.beginRow(topic_id, PJ::Timestamp(i) * 1000).has_value());
    writer.set(topic_id, 0, static_cast<float>(i) * 1.0f);
    writer.set(topic_id, 1, static_cast<float>(i) * 2.0f);
    writer.set(topic_id, 2, static_cast<float>(i) * 3.0f);
    ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  }
  engine.commitChunks(writer.flushAll());

  DataReader reader = engine.createReader();
  auto cursor = *reader.rangeQuery(QueryRange{.topic_id = topic_id, .t_min = 0, .t_max = 4000});
  std::size_t count = 0;
  cursor.forEach([&](const SampleRow& row) {
    EXPECT_NEAR(row.chunk->readNumericAsDouble(0, row.row_index), count * 1.0, 1e-4);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(1, row.row_index), count * 2.0, 1e-4);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(2, row.row_index), count * 3.0, 1e-4);
    ++count;
  });
  EXPECT_EQ(count, 4u);
}

TEST(ArrayExpansionTest, VarLenArray_ExpandArray_AddsColumns) {
  // expandArray("data", 3) on a float64[] topic → 3 columns: data[0], data[1], data[2]
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer.registerSchema("exp_test", root);

  TopicDescriptor desc;
  desc.name = "exp_topic";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  EXPECT_EQ(writer.bindTopicWriter(topic_id)->field_ids.size(), 0u);

  auto result = writer.expandArray(topic_id, "data", 3u);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(*result, 3u);

  auto handle = *writer.bindTopicWriter(topic_id);
  ASSERT_EQ(handle.field_ids.size(), 3u);
  EXPECT_EQ(*writer.resolveField(topic_id, "data[0]"), 0u);
  EXPECT_EQ(*writer.resolveField(topic_id, "data[1]"), 1u);
  EXPECT_EQ(*writer.resolveField(topic_id, "data[2]"), 2u);
}

TEST(ArrayExpansionTest, VarLenArray_ExpandIsIdempotent_ShrinkIsNoop) {
  // expand(3) then expand(2) → no-op; still 3 columns
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer.registerSchema("idem_test", root);
  TopicDescriptor desc;
  desc.name = "idem_topic";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  ASSERT_TRUE(writer.expandArray(topic_id, "data", 3u).has_value());

  // Try to shrink: must be no-op, return current count (3)
  auto result = writer.expandArray(topic_id, "data", 2u);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 3u);

  EXPECT_EQ(writer.bindTopicWriter(topic_id)->field_ids.size(), 3u);
}

TEST(ArrayExpansionTest, VarLenArray_WriteAndRead_BasicValues) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer.registerSchema("varlen_wr", root);
  TopicDescriptor desc;
  desc.name = "varlen_wr";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  ASSERT_TRUE(writer.expandArray(topic_id, "data", 3u).has_value());

  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(writer.beginRow(topic_id, PJ::Timestamp(i) * 1000).has_value());
    writer.set(topic_id, 0, i * 1.0);
    writer.set(topic_id, 1, i * 2.0);
    writer.set(topic_id, 2, i * 3.0);
    ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  }
  engine.commitChunks(writer.flushAll());

  DataReader reader = engine.createReader();
  auto cursor = *reader.rangeQuery(QueryRange{.topic_id = topic_id, .t_min = 0, .t_max = 4000});
  std::size_t count = 0;
  cursor.forEach([&](const SampleRow& row) {
    EXPECT_NEAR(row.chunk->readNumericAsDouble(0, row.row_index), count * 1.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(1, row.row_index), count * 2.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(2, row.row_index), count * 3.0, 1e-9);
    ++count;
  });
  EXPECT_EQ(count, 4u);
}

TEST(ArrayExpansionTest, VarLenArray_ExpandUnknownTopic_ReturnsError) {
  DataEngine engine;
  DataWriter writer = engine.createWriter();
  auto result = writer.expandArray(/*topic_id=*/999u, "data", 3u);
  EXPECT_FALSE(result.has_value());
}

TEST(ArrayExpansionTest, VarLenArray_ExpandNonArrayField_ReturnsError) {
  // Schema: struct { float64 value } — "value" is not an array
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto root = PJ::makeStruct("msg", {PJ::makePrimitive("value", PJ::PrimitiveType::kFloat64)});
  auto sid = *writer.registerSchema("non_arr", root);
  TopicDescriptor desc;
  desc.name = "non_arr_topic";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto result = writer.expandArray(topic_id, "value", 3u);
  EXPECT_FALSE(result.has_value());
}

TEST(ArrayExpansionTest, ExpansionLimit_ClampsColumns) {
  // array_expansion_limit = 4; expand to 10 → actual = 4
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer.registerSchema("limit_test", root);

  TopicDescriptor desc;
  desc.name = "limited";
  desc.schema_id = sid;
  desc.array_expansion_limit = 4;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto result = writer.expandArray(topic_id, "data", 10u);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(*result, 4u);  // clamped to limit

  EXPECT_EQ(writer.bindTopicWriter(topic_id)->field_ids.size(), 4u);
}

TEST(ArrayExpansionTest, MaxObservedArrayLength_TrackedInMetadata) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer.registerSchema("obs_test", root);
  TopicDescriptor desc;
  desc.name = "observed";
  desc.schema_id = sid;
  desc.array_expansion_limit = 10;
  auto topic_id = *writer.registerTopic(ds, desc);

  ASSERT_TRUE(writer.expandArray(topic_id, "data", 3u).has_value());

  const TopicStorage* storage = engine.getTopicStorage(topic_id);
  ASSERT_NE(storage, nullptr);
  EXPECT_EQ(storage->maxObservedArrayLength(), 3u);

  ASSERT_TRUE(writer.expandArray(topic_id, "data", 5u).has_value());
  EXPECT_EQ(storage->maxObservedArrayLength(), 5u);

  ASSERT_TRUE(writer.expandArray(topic_id, "data", 8u).has_value());
  EXPECT_EQ(storage->maxObservedArrayLength(), 8u);
}

TEST(ArrayExpansionTest, TruncatedSampleCount_TrackedOnClamping) {
  // array_expansion_limit = 3. expand(10) → 1 truncation. expand(2) → no truncation.
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer.registerSchema("trunc_test", root);
  TopicDescriptor desc;
  desc.name = "truncated";
  desc.schema_id = sid;
  desc.array_expansion_limit = 3;
  auto topic_id = *writer.registerTopic(ds, desc);

  const TopicStorage* storage = engine.getTopicStorage(topic_id);

  // expand to 10 exceeds limit=3 → 1 truncation
  ASSERT_TRUE(writer.expandArray(topic_id, "data", 10u).has_value());
  EXPECT_EQ(storage->truncatedSampleCount(), 1u);

  // expand to 2 — no-op (current=3 >= 2); no new truncation
  ASSERT_TRUE(writer.expandArray(topic_id, "data", 2u).has_value());
  EXPECT_EQ(storage->truncatedSampleCount(), 1u);

  // expand to 20 — actual=3 (same as current), but still truncated (20 > limit)
  ASSERT_TRUE(writer.expandArray(topic_id, "data", 20u).has_value());
  EXPECT_EQ(storage->truncatedSampleCount(), 2u);
}

TEST(ArrayExpansionTest, Metadata_ExposedViaTopicMetadata) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer.registerSchema("meta_test", root);
  TopicDescriptor desc;
  desc.name = "meta_topic";
  desc.schema_id = sid;
  desc.array_expansion_limit = 4;
  auto topic_id = *writer.registerTopic(ds, desc);

  ASSERT_TRUE(writer.expandArray(topic_id, "data", 10u).has_value());  // clamped to 4; 1 truncation; max_observed=10

  ASSERT_TRUE(writer.beginRow(topic_id, 1000).has_value());
  writer.set(topic_id, 0, 1.0);
  writer.set(topic_id, 1, 2.0);
  writer.set(topic_id, 2, 3.0);
  writer.set(topic_id, 3, 4.0);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  DataReader reader = engine.createReader();
  auto meta = reader.getMetadata(topic_id);
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->max_observed_array_length, 10u);
  EXPECT_EQ(meta->truncated_sample_count, 1u);
}

TEST(ArrayExpansionTest, CrossChunkExpansion_OldChunksHaveFewerColumns) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer.registerSchema("cross_chunk", root);
  TopicDescriptor desc;
  desc.name = "cc";
  desc.schema_id = sid;
  desc.max_chunk_rows = 8;
  auto topic_id = *writer.registerTopic(ds, desc);

  // Expand to 2, write 10 rows (8 auto-sealed + 2 in builder)
  ASSERT_TRUE(writer.expandArray(topic_id, "data", 2u).has_value());
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(writer.beginRow(topic_id, PJ::Timestamp(i) * 1000).has_value());
    writer.set(topic_id, 0, i * 1.0);
    writer.set(topic_id, 1, i * 2.0);
    ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  }

  // Expand to 4: seals the in-progress builder (rows 8–9) and creates the new 4-column layout
  ASSERT_TRUE(writer.expandArray(topic_id, "data", 4u).has_value());

  for (int i = 10; i < 15; ++i) {
    ASSERT_TRUE(writer.beginRow(topic_id, PJ::Timestamp(i) * 1000).has_value());
    writer.set(topic_id, 0, i * 1.0);
    writer.set(topic_id, 1, i * 2.0);
    writer.set(topic_id, 2, i * 3.0);
    writer.set(topic_id, 3, i * 4.0);
    ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  }
  engine.commitChunks(writer.flushAll());

  const TopicStorage* storage = engine.getTopicStorage(topic_id);
  ASSERT_NE(storage, nullptr);
  for (const auto& chunk : storage->sealedChunks()) {
    if (chunk.stats.t_max < PJ::Timestamp(10) * 1000) {
      EXPECT_EQ(chunk.columns.size(), 2u) << "pre-expansion chunk must have 2 columns";
    } else {
      EXPECT_EQ(chunk.columns.size(), 4u) << "post-expansion chunk must have 4 columns";
    }
  }

  DataReader reader = engine.createReader();
  auto cursor = *reader.rangeQuery(QueryRange{.topic_id = topic_id, .t_min = 0, .t_max = 15000});
  std::size_t count = 0;
  cursor.forEach([&](const SampleRow& row) {
    int i = static_cast<int>(count);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(0, row.row_index), i * 1.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(1, row.row_index), i * 2.0, 1e-9);
    if (i >= 10) {
      EXPECT_NEAR(row.chunk->readNumericAsDouble(2, row.row_index), i * 3.0, 1e-9);
      EXPECT_NEAR(row.chunk->readNumericAsDouble(3, row.row_index), i * 4.0, 1e-9);
    }
    ++count;
  });
  EXPECT_EQ(count, 15u);
}

TEST(ArrayExpansionTest, UnsetElementsAutoNullFilled) {
  // expand(4), write rows setting only elements [0] and [1].
  // Elements [2] and [3] must be null (auto-filled by finish_row).
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer.registerSchema("partial_set", root);
  TopicDescriptor desc;
  desc.name = "partial";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  ASSERT_TRUE(writer.expandArray(topic_id, "data", 4u).has_value());

  // Write row with only first 2 elements; cols 2 and 3 are unset
  ASSERT_TRUE(writer.beginRow(topic_id, 1000).has_value());
  writer.set(topic_id, 0, 1.0);
  writer.set(topic_id, 1, 2.0);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  DataReader reader = engine.createReader();
  auto cursor = *reader.rangeQuery(QueryRange{.topic_id = topic_id, .t_min = 0, .t_max = 2000});
  bool visited = false;
  cursor.forEach([&](const SampleRow& row) {
    EXPECT_NEAR(row.chunk->readNumericAsDouble(0, row.row_index), 1.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(1, row.row_index), 2.0, 1e-9);
    EXPECT_TRUE(row.chunk->isNull(2, row.row_index));
    EXPECT_TRUE(row.chunk->isNull(3, row.row_index));
    visited = true;
  });
  EXPECT_TRUE(visited);
}

TEST(ArrayExpansionTest, VarLenStructArray_ExpandAndWrite) {
  // Schema: struct msg { Pose[] poses } where Pose = struct { float32 x, y }
  // expandArray("poses", 2) → 4 columns: poses[0].x, poses[0].y, poses[1].x, poses[1].y
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto pose_elem = PJ::makeStruct(
      "", {
              PJ::makePrimitive("x", PJ::PrimitiveType::kFloat32),
              PJ::makePrimitive("y", PJ::PrimitiveType::kFloat32),
          });
  auto poses_arr = PJ::makeArray("poses", pose_elem, std::nullopt);
  auto root = PJ::makeStruct("msg", {poses_arr});
  auto sid = *writer.registerSchema("struct_arr_var", root);

  TopicDescriptor desc;
  desc.name = "poses";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  EXPECT_EQ(writer.bindTopicWriter(topic_id)->field_ids.size(), 0u);

  auto result = writer.expandArray(topic_id, "poses", 2u);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(*result, 2u);

  auto handle = *writer.bindTopicWriter(topic_id);
  ASSERT_EQ(handle.field_ids.size(), 4u);
  EXPECT_EQ(*writer.resolveField(topic_id, "poses[0].x"), 0u);
  EXPECT_EQ(*writer.resolveField(topic_id, "poses[0].y"), 1u);
  EXPECT_EQ(*writer.resolveField(topic_id, "poses[1].x"), 2u);
  EXPECT_EQ(*writer.resolveField(topic_id, "poses[1].y"), 3u);

  ASSERT_TRUE(writer.beginRow(topic_id, 1000).has_value());
  writer.set(topic_id, 0, 1.0f);
  writer.set(topic_id, 1, 2.0f);
  writer.set(topic_id, 2, 3.0f);
  writer.set(topic_id, 3, 4.0f);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  DataReader reader = engine.createReader();
  auto cursor = *reader.rangeQuery(QueryRange{.topic_id = topic_id, .t_min = 0, .t_max = 2000});
  bool visited = false;
  cursor.forEach([&](const SampleRow& row) {
    EXPECT_NEAR(row.chunk->readNumericAsDouble(0, row.row_index), 1.0, 1e-4);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(1, row.row_index), 2.0, 1e-4);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(2, row.row_index), 3.0, 1e-4);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(3, row.row_index), 4.0, 1e-4);
    visited = true;
  });
  EXPECT_TRUE(visited);
}

// A second DataWriter for a typed variable-length array topic sees the expanded layout stored in TopicStorage
// (rebuilding from the type tree would yield 0 columns for variable-length arrays).
TEST(ArrayExpansionTest, SecondWriter_PicksUpExpandedLayout) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});

  DataWriter writer_a = engine.createWriter();
  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer_a.registerSchema("second_writer_schema", root);

  TopicDescriptor desc;
  desc.name = "second_writer_topic";
  desc.schema_id = sid;
  auto topic_id = *writer_a.registerTopic(ds, desc);

  auto expand_result = writer_a.expandArray(topic_id, "data", 3u);
  ASSERT_TRUE(expand_result.has_value()) << expand_result.error();
  ASSERT_EQ(*expand_result, 3u);

  ASSERT_TRUE(writer_a.beginRow(topic_id, 1000).has_value());
  writer_a.set(topic_id, 0, 10.0);
  writer_a.set(topic_id, 1, 20.0);
  writer_a.set(topic_id, 2, 30.0);
  ASSERT_TRUE(writer_a.finishRow(topic_id).has_value());
  engine.commitChunks(writer_a.flushAll());

  // Writer B: a brand new DataWriter on the same engine/topic.
  // It must see the 3-column layout that Writer A established.
  DataWriter writer_b = engine.createWriter();

  auto handle = writer_b.bindTopicWriter(topic_id);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  EXPECT_EQ(handle->field_ids.size(), 3u) << "Writer B must inherit the 3-column layout expanded by Writer A";

  EXPECT_EQ(*writer_b.resolveField(topic_id, "data[0]"), 0u);
  EXPECT_EQ(*writer_b.resolveField(topic_id, "data[1]"), 1u);
  EXPECT_EQ(*writer_b.resolveField(topic_id, "data[2]"), 2u);
}

// expandArray() while a row is in progress (between beginRow and finishRow) returns an error and leaves the
// previously completed rows intact in the pending chunks; it must not silently discard the in-progress row.
TEST(ArrayExpansionTest, ExpandArray_WhileRowInProgress_ReturnsError) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer.registerSchema("in_progress_schema", root);

  TopicDescriptor desc;
  desc.name = "in_progress_topic";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto init_expand = writer.expandArray(topic_id, "data", 2u);
  ASSERT_TRUE(init_expand.has_value()) << init_expand.error();

  ASSERT_TRUE(writer.beginRow(topic_id, 1000).has_value());
  writer.set(topic_id, 0, 1.0);
  writer.set(topic_id, 1, 2.0);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());

  ASSERT_TRUE(writer.beginRow(topic_id, 2000).has_value());
  writer.set(topic_id, 0, 3.0);
  writer.set(topic_id, 1, 4.0);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());

  // Row 3: begin_row but do NOT call finish_row before expand_array.
  // expand_array must return an error — it must not silently drop the row.
  ASSERT_TRUE(writer.beginRow(topic_id, 3000).has_value());
  writer.set(topic_id, 0, 5.0);
  writer.set(topic_id, 1, 6.0);
  auto expand_result = writer.expandArray(topic_id, "data", 3u);
  EXPECT_FALSE(expand_result.has_value())
      << "expand_array must fail when a row is in progress; got success unexpectedly";

  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  // All 3 rows must be readable — the failed expand did not corrupt data
  DataReader reader = engine.createReader();
  auto cursor = *reader.rangeQuery(QueryRange{.topic_id = topic_id, .t_min = 0, .t_max = 4000});
  std::size_t count = 0;
  cursor.forEach([&](const SampleRow&) { ++count; });
  EXPECT_EQ(count, 3u) << "All 3 rows must survive; the failed expand must not corrupt the builder";
}

// Re-expanding an array field from a fresh DataWriter must not duplicate existing indices: the per-writer
// expansion state starts at 0 while the persisted columns already exist, so expanding "data" from 3 to 5 yields
// data[0..4], not 8 columns.
TEST(ArrayExpansionTest, SecondWriter_ReExpandSameField_DoesNotDuplicateColumns) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});

  DataWriter writer_a = engine.createWriter();
  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer_a.registerSchema("reexpand_schema", root);

  TopicDescriptor desc;
  desc.name = "reexpand_topic";
  desc.schema_id = sid;
  auto topic_id = *writer_a.registerTopic(ds, desc);

  auto r1 = writer_a.expandArray(topic_id, "data", 3u);
  ASSERT_TRUE(r1.has_value()) << r1.error();
  ASSERT_EQ(*r1, 3u);

  DataWriter writer_b = engine.createWriter();
  auto before = writer_b.bindTopicWriter(topic_id);
  ASSERT_TRUE(before.has_value()) << before.error();
  ASSERT_EQ(before->field_ids.size(), 3u);

  auto r2 = writer_b.expandArray(topic_id, "data", 5u);
  ASSERT_TRUE(r2.has_value()) << r2.error();
  EXPECT_EQ(*r2, 5u);

  auto after = writer_b.bindTopicWriter(topic_id);
  ASSERT_TRUE(after.has_value()) << after.error();
  EXPECT_EQ(after->field_ids.size(), 5u)
      << "Re-expanding in a new writer must append only new indices [3..4], not duplicate [0..2]";
  EXPECT_EQ(*writer_b.resolveField(topic_id, "data[0]"), 0u);
  EXPECT_EQ(*writer_b.resolveField(topic_id, "data[1]"), 1u);
  EXPECT_EQ(*writer_b.resolveField(topic_id, "data[2]"), 2u);
  EXPECT_EQ(*writer_b.resolveField(topic_id, "data[3]"), 3u);
  EXPECT_EQ(*writer_b.resolveField(topic_id, "data[4]"), 4u);
}

// Mixed-schema topic (scalar + variable-length array): expandArray appends the element columns after the
// existing scalar columns with the correct FieldIds, and the scalar column stays readable.
TEST(ArrayExpansionTest, MixedSchema_ScalarPlusVarLenArray_CorrectColumnOrder) {
  // Schema: struct msg { float64 timestamp_sec; float64[] values }
  // After expandArray("values", 3):
  //   col 0 → timestamp_sec  (field_id 0)
  //   col 1 → values[0]      (field_id 1)
  //   col 2 → values[1]      (field_id 2)
  //   col 3 → values[2]      (field_id 3)
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto ts_field = PJ::makePrimitive("timestamp_sec", PJ::PrimitiveType::kFloat64);
  auto values_arr = PJ::makeArray("values", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {ts_field, values_arr});
  auto sid = *writer.registerSchema("mixed_schema", root);

  TopicDescriptor desc;
  desc.name = "mixed_topic";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto handle_before = *writer.bindTopicWriter(topic_id);
  ASSERT_EQ(handle_before.field_ids.size(), 1u);
  EXPECT_EQ(*writer.resolveField(topic_id, "timestamp_sec"), 0u);

  auto result = writer.expandArray(topic_id, "values", 3u);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(*result, 3u);

  auto handle_after = *writer.bindTopicWriter(topic_id);
  ASSERT_EQ(handle_after.field_ids.size(), 4u);

  EXPECT_EQ(*writer.resolveField(topic_id, "timestamp_sec"), 0u);
  EXPECT_EQ(*writer.resolveField(topic_id, "values[0]"), 1u);
  EXPECT_EQ(*writer.resolveField(topic_id, "values[1]"), 2u);
  EXPECT_EQ(*writer.resolveField(topic_id, "values[2]"), 3u);

  ASSERT_TRUE(writer.beginRow(topic_id, 1000).has_value());
  writer.set(topic_id, 0, 100.0);  // timestamp_sec
  writer.set(topic_id, 1, 1.0);    // values[0]
  writer.set(topic_id, 2, 2.0);    // values[1]
  writer.set(topic_id, 3, 3.0);    // values[2]
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  DataReader reader = engine.createReader();
  auto cursor = *reader.rangeQuery(QueryRange{.topic_id = topic_id, .t_min = 0, .t_max = 2000});
  bool visited = false;
  cursor.forEach([&](const SampleRow& row) {
    EXPECT_NEAR(row.chunk->readNumericAsDouble(0, row.row_index), 100.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(1, row.row_index), 1.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(2, row.row_index), 2.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(3, row.row_index), 3.0, 1e-9);
    visited = true;
  });
  EXPECT_TRUE(visited);
}

// Two independent expansions of different fields on the same topic: the second appends after the first's
// columns, with the right field_id sequence and readable data.
TEST(ArrayExpansionTest, TwoDistinctArrayFields_ExpandBoth_CorrectLayout) {
  // Schema: struct msg { float64[] positions; float64[] velocities }
  // Expand positions to 2, then velocities to 3.
  // Expected columns:
  //   positions[0] (id 0), positions[1] (id 1),
  //   velocities[0] (id 2), velocities[1] (id 3), velocities[2] (id 4)
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto pos_arr = PJ::makeArray("positions", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto vel_arr = PJ::makeArray("velocities", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {pos_arr, vel_arr});
  auto sid = *writer.registerSchema("two_arrays_schema", root);

  TopicDescriptor desc;
  desc.name = "two_arrays_topic";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  EXPECT_EQ(writer.bindTopicWriter(topic_id)->field_ids.size(), 0u);

  auto r1 = writer.expandArray(topic_id, "positions", 2u);
  ASSERT_TRUE(r1.has_value()) << r1.error();
  EXPECT_EQ(*r1, 2u);
  EXPECT_EQ(writer.bindTopicWriter(topic_id)->field_ids.size(), 2u);

  auto r2 = writer.expandArray(topic_id, "velocities", 3u);
  ASSERT_TRUE(r2.has_value()) << r2.error();
  EXPECT_EQ(*r2, 3u);
  EXPECT_EQ(writer.bindTopicWriter(topic_id)->field_ids.size(), 5u);

  EXPECT_EQ(*writer.resolveField(topic_id, "positions[0]"), 0u);
  EXPECT_EQ(*writer.resolveField(topic_id, "positions[1]"), 1u);
  EXPECT_EQ(*writer.resolveField(topic_id, "velocities[0]"), 2u);
  EXPECT_EQ(*writer.resolveField(topic_id, "velocities[1]"), 3u);
  EXPECT_EQ(*writer.resolveField(topic_id, "velocities[2]"), 4u);

  ASSERT_TRUE(writer.beginRow(topic_id, 1000).has_value());
  writer.set(topic_id, 0, 1.0);
  writer.set(topic_id, 1, 2.0);
  writer.set(topic_id, 2, 10.0);
  writer.set(topic_id, 3, 20.0);
  writer.set(topic_id, 4, 30.0);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  DataReader reader = engine.createReader();
  auto cursor = *reader.rangeQuery(QueryRange{.topic_id = topic_id, .t_min = 0, .t_max = 2000});
  bool visited = false;
  cursor.forEach([&](const SampleRow& row) {
    EXPECT_NEAR(row.chunk->readNumericAsDouble(0, row.row_index), 1.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(1, row.row_index), 2.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(2, row.row_index), 10.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(3, row.row_index), 20.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(4, row.row_index), 30.0, 1e-9);
    visited = true;
  });
  EXPECT_TRUE(visited);
}

TEST(ArrayExpansionTest, Schemaless_EnsureColumn_AddsColumn) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  TopicDescriptor desc;
  desc.name = "sl_ec_add";
  desc.schema_id = 0;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto fid = writer.ensureColumn(topic_id, "value", PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(fid.has_value()) << fid.error();
  EXPECT_EQ(*fid, 0u);

  auto handle = *writer.bindTopicWriter(topic_id);
  ASSERT_EQ(handle.field_ids.size(), 1u);

  ASSERT_TRUE(writer.beginRow(topic_id, 1000).has_value());
  writer.set(topic_id, 0, 42.0);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  DataReader reader = engine.createReader();
  auto cursor = *reader.rangeQuery(QueryRange{.topic_id = topic_id, .t_min = 0, .t_max = 2000});
  bool visited = false;
  cursor.forEach([&](const SampleRow& row) {
    EXPECT_NEAR(row.chunk->readNumericAsDouble(0, row.row_index), 42.0, 1e-9);
    visited = true;
  });
  EXPECT_TRUE(visited);
}

TEST(ArrayExpansionTest, Schemaless_EnsureColumn_Idempotent) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  TopicDescriptor desc;
  desc.name = "sl_ec_idem";
  desc.schema_id = 0;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto fid1 = writer.ensureColumn(topic_id, "x", PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(fid1.has_value());
  EXPECT_EQ(*fid1, 0u);

  auto fid2 = writer.ensureColumn(topic_id, "x", PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(fid2.has_value());
  EXPECT_EQ(*fid2, 0u);  // same field id — idempotent

  auto handle = *writer.bindTopicWriter(topic_id);
  EXPECT_EQ(handle.field_ids.size(), 1u);  // no duplicate
}

TEST(ArrayExpansionTest, Schemaless_EnsureColumn_MultipleColumns_WriteAndRead) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  TopicDescriptor desc;
  desc.name = "sl_ec_multi";
  desc.schema_id = 0;
  auto topic_id = *writer.registerTopic(ds, desc);

  EXPECT_EQ(*writer.ensureColumn(topic_id, "x", PJ::PrimitiveType::kFloat64), 0u);
  EXPECT_EQ(*writer.ensureColumn(topic_id, "y", PJ::PrimitiveType::kFloat64), 1u);
  EXPECT_EQ(*writer.ensureColumn(topic_id, "z", PJ::PrimitiveType::kFloat32), 2u);

  auto handle = *writer.bindTopicWriter(topic_id);
  ASSERT_EQ(handle.field_ids.size(), 3u);

  ASSERT_TRUE(writer.beginRow(topic_id, 1000).has_value());
  writer.set(topic_id, 0, 1.0);
  writer.set(topic_id, 1, 2.0);
  writer.set(topic_id, 2, 3.0f);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  DataReader reader = engine.createReader();
  auto cursor = *reader.rangeQuery(QueryRange{.topic_id = topic_id, .t_min = 0, .t_max = 2000});
  bool visited = false;
  cursor.forEach([&](const SampleRow& row) {
    EXPECT_NEAR(row.chunk->readNumericAsDouble(0, row.row_index), 1.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(1, row.row_index), 2.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(2, row.row_index), 3.0, 1e-4);
    visited = true;
  });
  EXPECT_TRUE(visited);
}

TEST(ArrayExpansionTest, Schemaless_EnsureColumn_SecondWriter_PicksUpLayout) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer_a = engine.createWriter();

  TopicDescriptor desc;
  desc.name = "sl_ec_second_writer";
  desc.schema_id = 0;
  auto topic_id = *writer_a.registerTopic(ds, desc);

  ASSERT_TRUE(writer_a.ensureColumn(topic_id, "v", PJ::PrimitiveType::kFloat64).has_value());
  ASSERT_TRUE(writer_a.beginRow(topic_id, 1000).has_value());
  writer_a.set(topic_id, 0, 99.0);
  ASSERT_TRUE(writer_a.finishRow(topic_id).has_value());
  engine.commitChunks(writer_a.flushAll());

  DataWriter writer_b = engine.createWriter();
  auto handle = writer_b.bindTopicWriter(topic_id);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  ASSERT_EQ(handle->field_ids.size(), 1u);
  EXPECT_EQ(*writer_b.resolveField(topic_id, "v"), 0u);
}

// The stored layout ends up as the writer's, whether it was already in step (grown one column at a time) or the
// writer's columns came from the schema and never reached the storage.
TEST(ArrayExpansionTest, EnsureColumn_StoredLayoutMatchesWriter) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  TopicDescriptor schemaless;
  schemaless.name = "grows";
  const auto grows = *writer.registerTopic(ds, schemaless);
  for (int i = 0; i < 50; ++i) {
    ASSERT_EQ(*writer.ensureColumn(grows, "c" + std::to_string(i), PJ::PrimitiveType::kFloat64), FieldId(i));
  }

  auto root = PJ::makeStruct("msg", {PJ::makeArray("accel", PJ::makePrimitive("", PJ::PrimitiveType::kFloat32), 3u)});
  TopicDescriptor typed;
  typed.name = "typed";
  typed.schema_id = *writer.registerSchema("msg_typed_extra", root);
  const auto typed_id = *writer.registerTopic(ds, typed);
  ASSERT_EQ(*writer.ensureColumn(typed_id, "extra", PJ::PrimitiveType::kFloat64), 3u);

  const auto lock = engine.lockEngine();
  const auto& grown = engine.getTopicStorage(grows)->columnDescriptors();
  ASSERT_EQ(grown.size(), 50u);
  for (std::size_t i = 0; i < grown.size(); ++i) {
    EXPECT_EQ(grown[i].field_id, FieldId(i));
    EXPECT_EQ(grown[i].field_path, "c" + std::to_string(i));
  }
  const auto& typed_cols = engine.getTopicStorage(typed_id)->columnDescriptors();
  ASSERT_EQ(typed_cols.size(), 4u);
  EXPECT_EQ(typed_cols[0].field_path, "accel[0]");
  EXPECT_EQ(typed_cols[3].field_path, "extra");
  EXPECT_EQ(typed_cols[3].field_id, 3u);
}

TEST(ArrayExpansionTest, Schemaless_EnsureColumn_WhileRowInProgress_ReturnsError) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  TopicDescriptor desc;
  desc.name = "sl_ec_row_progress";
  desc.schema_id = 0;
  auto topic_id = *writer.registerTopic(ds, desc);

  ASSERT_TRUE(writer.ensureColumn(topic_id, "x", PJ::PrimitiveType::kFloat64).has_value());
  ASSERT_TRUE(writer.beginRow(topic_id, 1000).has_value());

  auto result = writer.ensureColumn(topic_id, "y", PJ::PrimitiveType::kFloat64);
  EXPECT_FALSE(result.has_value()) << "ensure_column must fail while a row is in progress";

  ASSERT_TRUE(writer.finishRow(topic_id).has_value());

  // After finishing the row, ensure_column should auto-seal and succeed.
  auto result2 = writer.ensureColumn(topic_id, "y", PJ::PrimitiveType::kFloat64);
  EXPECT_TRUE(result2.has_value()) << "ensure_column should auto-seal and succeed after rows";
}

TEST(ArrayExpansionTest, Schemaless_ExpandArray_AddsColumns) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  TopicDescriptor desc;
  desc.name = "sl_ea_add";
  desc.schema_id = 0;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto result = writer.expandArray(topic_id, "data", 3u, PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(*result, 3u);

  auto handle = *writer.bindTopicWriter(topic_id);
  ASSERT_EQ(handle.field_ids.size(), 3u);
  EXPECT_EQ(*writer.resolveField(topic_id, "data[0]"), 0u);
  EXPECT_EQ(*writer.resolveField(topic_id, "data[1]"), 1u);
  EXPECT_EQ(*writer.resolveField(topic_id, "data[2]"), 2u);
}

TEST(ArrayExpansionTest, Schemaless_ExpandArray_IncrementalExpansion) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  TopicDescriptor desc;
  desc.name = "sl_ea_incremental";
  desc.schema_id = 0;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto r1 = writer.expandArray(topic_id, "data", 2u, PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(r1.has_value()) << r1.error();
  EXPECT_EQ(*r1, 2u);

  ASSERT_TRUE(writer.beginRow(topic_id, 1000).has_value());
  writer.set(topic_id, 0, 1.0);
  writer.set(topic_id, 1, 2.0);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  auto r2 = writer.expandArray(topic_id, "data", 5u, PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(r2.has_value()) << r2.error();
  EXPECT_EQ(*r2, 5u);

  ASSERT_TRUE(writer.beginRow(topic_id, 2000).has_value());
  writer.set(topic_id, 0, 10.0);
  writer.set(topic_id, 1, 20.0);
  writer.set(topic_id, 2, 30.0);
  writer.set(topic_id, 3, 40.0);
  writer.set(topic_id, 4, 50.0);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  const TopicStorage* storage = engine.getTopicStorage(topic_id);
  ASSERT_NE(storage, nullptr);
  ASSERT_GE(storage->sealedChunks().size(), 2u);
  EXPECT_EQ(storage->sealedChunks().back().columns.size(), 5u);
}

TEST(ArrayExpansionTest, Schemaless_ExpandArray_Idempotent_ShrinkIsNoop) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  TopicDescriptor desc;
  desc.name = "sl_ea_idem";
  desc.schema_id = 0;
  auto topic_id = *writer.registerTopic(ds, desc);

  EXPECT_EQ(*writer.expandArray(topic_id, "data", 3u, PJ::PrimitiveType::kFloat64), 3u);
  EXPECT_EQ(*writer.expandArray(topic_id, "data", 3u, PJ::PrimitiveType::kFloat64), 3u);
  EXPECT_EQ(*writer.expandArray(topic_id, "data", 1u, PJ::PrimitiveType::kFloat64), 3u);

  EXPECT_EQ(writer.bindTopicWriter(topic_id)->field_ids.size(), 3u);
}

TEST(ArrayExpansionTest, Schemaless_ExpandArray_DefaultType_IsFloat64) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  TopicDescriptor desc;
  desc.name = "sl_ea_default_type";
  desc.schema_id = 0;
  auto topic_id = *writer.registerTopic(ds, desc);

  // No element_type arg — default is kFloat64
  auto result = writer.expandArray(topic_id, "data", 2u);
  ASSERT_TRUE(result.has_value()) << result.error();

  const TopicStorage* storage = engine.getTopicStorage(topic_id);
  ASSERT_NE(storage, nullptr);
  const auto& cols = storage->columnDescriptors();
  ASSERT_EQ(cols.size(), 2u);
  EXPECT_EQ(cols[0].logical_type, PJ::PrimitiveType::kFloat64);
  EXPECT_EQ(cols[1].logical_type, PJ::PrimitiveType::kFloat64);
}

TEST(ArrayExpansionTest, Mixed_EnsureColumnThenExpandArray_NoConflict) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  TopicDescriptor desc;
  desc.name = "mixed_ec_ea";
  desc.schema_id = 0;
  auto topic_id = *writer.registerTopic(ds, desc);

  // Pre-add data[0] via ensure_column
  auto fid0 = writer.ensureColumn(topic_id, "data[0]", PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(fid0.has_value());
  EXPECT_EQ(*fid0, 0u);

  // expand_array sees data[0] already exists, adds data[1] and data[2]
  auto result = writer.expandArray(topic_id, "data", 3u, PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(*result, 3u);

  auto handle = *writer.bindTopicWriter(topic_id);
  EXPECT_EQ(handle.field_ids.size(), 3u);  // exactly 3, no duplicates
  EXPECT_EQ(*writer.resolveField(topic_id, "data[0]"), 0u);
  EXPECT_EQ(*writer.resolveField(topic_id, "data[1]"), 1u);
  EXPECT_EQ(*writer.resolveField(topic_id, "data[2]"), 2u);
}

TEST(ArrayExpansionTest, Mixed_ExpandArrayThenEnsureColumn_NoConflict) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  TopicDescriptor desc;
  desc.name = "mixed_ea_ec";
  desc.schema_id = 0;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto r = writer.expandArray(topic_id, "data", 2u, PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(*r, 2u);

  // data[0] already exists — should be no-op, returns existing field id
  auto fid0 = writer.ensureColumn(topic_id, "data[0]", PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(fid0.has_value());
  EXPECT_EQ(*fid0, 0u);

  // extra column not part of array
  auto fid_extra = writer.ensureColumn(topic_id, "extra", PJ::PrimitiveType::kFloat32);
  ASSERT_TRUE(fid_extra.has_value());
  EXPECT_EQ(*fid_extra, 2u);  // new column at index 2

  auto handle = *writer.bindTopicWriter(topic_id);
  EXPECT_EQ(handle.field_ids.size(), 3u);  // data[0], data[1], extra
}

// A typed expandArray must not duplicate element columns that ensureColumn already added for an array-indexed path.
TEST(ArrayExpansionTest, Typed_EnsureColumnThenExpandArray_NoDuplicateColumns) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto data_arr = PJ::makeArray("data", PJ::makePrimitive("", PJ::PrimitiveType::kFloat64), std::nullopt);
  auto root = PJ::makeStruct("msg", {data_arr});
  auto sid = *writer.registerSchema("typed_ec_ea_dedup", root);

  TopicDescriptor desc;
  desc.name = "typed_ec_ea_dedup";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  // Manually add data[0] via ensureColumn (does not update array_expansion_count)
  auto fid0 = writer.ensureColumn(topic_id, "data[0]", PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(fid0.has_value()) << fid0.error();
  EXPECT_EQ(*fid0, 0u);

  // expand_array for the same typed field: must NOT duplicate data[0]
  auto result = writer.expandArray(topic_id, "data", 3u);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(*result, 3u);

  auto handle = *writer.bindTopicWriter(topic_id);
  EXPECT_EQ(handle.field_ids.size(), 3u) << "Must have exactly 3 columns, no duplicates";
  EXPECT_EQ(*writer.resolveField(topic_id, "data[0]"), 0u);
  EXPECT_EQ(*writer.resolveField(topic_id, "data[1]"), 1u);
  EXPECT_EQ(*writer.resolveField(topic_id, "data[2]"), 2u);
}

// ensureColumn returns an error when the path already exists with a different type.
TEST(ArrayExpansionTest, EnsureColumn_TypeMismatch_ReturnsError) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  TopicDescriptor desc;
  desc.name = "type_mismatch";
  desc.schema_id = 0;
  auto topic_id = *writer.registerTopic(ds, desc);

  auto fid = writer.ensureColumn(topic_id, "value", PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(fid.has_value());
  EXPECT_EQ(*fid, 0u);

  // Re-register same path with float32: must fail
  auto mismatch = writer.ensureColumn(topic_id, "value", PJ::PrimitiveType::kFloat32);
  EXPECT_FALSE(mismatch.has_value()) << "Type mismatch must return error, not silent success";

  // Re-register same path with same type: must succeed (idempotent)
  auto same_type = writer.ensureColumn(topic_id, "value", PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(same_type.has_value());
  EXPECT_EQ(*same_type, 0u);  // same field id
}

TEST(ArrayExpansionTest, Typed_EnsureColumn_AddsColumnOutsideSchema) {
  DataEngine engine;
  DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t"});
  DataWriter writer = engine.createWriter();

  auto root = PJ::makeStruct("msg", {PJ::makePrimitive("x", PJ::PrimitiveType::kFloat64)});
  auto sid = *writer.registerSchema("typed_ensure", root);

  TopicDescriptor desc;
  desc.name = "typed_ensure_topic";
  desc.schema_id = sid;
  auto topic_id = *writer.registerTopic(ds, desc);

  // Schema gives field "x" at id=0
  EXPECT_EQ(*writer.resolveField(topic_id, "x"), 0u);

  // Add an extra column outside the schema
  auto fid = writer.ensureColumn(topic_id, "debug_extra", PJ::PrimitiveType::kFloat64);
  ASSERT_TRUE(fid.has_value()) << fid.error();
  EXPECT_EQ(*fid, 1u);

  auto handle = *writer.bindTopicWriter(topic_id);
  ASSERT_EQ(handle.field_ids.size(), 2u);

  ASSERT_TRUE(writer.beginRow(topic_id, 1000).has_value());
  writer.set(topic_id, 0, 1.0);
  writer.set(topic_id, 1, 2.0);
  ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  engine.commitChunks(writer.flushAll());

  DataReader reader = engine.createReader();
  auto cursor = *reader.rangeQuery(QueryRange{.topic_id = topic_id, .t_min = 0, .t_max = 2000});
  bool visited = false;
  cursor.forEach([&](const SampleRow& row) {
    EXPECT_NEAR(row.chunk->readNumericAsDouble(0, row.row_index), 1.0, 1e-9);
    EXPECT_NEAR(row.chunk->readNumericAsDouble(1, row.row_index), 2.0, 1e-9);
    visited = true;
  });
  EXPECT_TRUE(visited);
}

}  // namespace
