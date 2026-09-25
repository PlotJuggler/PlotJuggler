// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/chunk.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace PJ {
namespace {

// ---------------------------------------------------------------------------
// Helper: create a vector of ColumnDescriptors
// ---------------------------------------------------------------------------

ColumnDescriptor makeCol(FieldId id, PrimitiveType type, std::string path) {
  return ColumnDescriptor{id, type, std::move(path)};
}

// ===========================================================================
// Test 1: Build and seal float32 chunk
// ===========================================================================

TEST(ChunkTest, BuildAndSealFloat32Chunk) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "x"),
      makeCol(2, PrimitiveType::kFloat32, "y"),
      makeCol(3, PrimitiveType::kFloat32, "z"),
  };
  TopicChunkBuilder builder(/*topic_id=*/10, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  // Add 5 rows
  for (uint32_t i = 0; i < 5; ++i) {
    Timestamp ts = 1000 + static_cast<Timestamp>(i) * 100;
    builder.beginRow(ts);
    builder.set(0, static_cast<float>(i) * 1.0F);
    builder.set(1, static_cast<float>(i) * 2.0F);
    builder.set(2, static_cast<float>(i) * 3.0F);
    builder.finishRow();
  }

  EXPECT_EQ(builder.rowCount(), 5U);
  EXPECT_FALSE(builder.isFull());

  const auto& stats = builder.stats();
  EXPECT_EQ(stats.t_min, 1000);
  EXPECT_EQ(stats.t_max, 1400);
  EXPECT_EQ(stats.row_count, 5U);

  // Column 0 (x): values 0, 1, 2, 3, 4
  EXPECT_DOUBLE_EQ(*stats.column_stats[0].min_value, 0.0);
  EXPECT_DOUBLE_EQ(*stats.column_stats[0].max_value, 4.0);

  // Column 1 (y): values 0, 2, 4, 6, 8
  EXPECT_DOUBLE_EQ(*stats.column_stats[1].min_value, 0.0);
  EXPECT_DOUBLE_EQ(*stats.column_stats[1].max_value, 8.0);

  // Column 2 (z): values 0, 3, 6, 9, 12
  EXPECT_DOUBLE_EQ(*stats.column_stats[2].min_value, 0.0);
  EXPECT_DOUBLE_EQ(*stats.column_stats[2].max_value, 12.0);

  TopicChunk chunk = builder.seal();
  EXPECT_NE(chunk.id, 0U);
  EXPECT_EQ(chunk.topic_id, 10U);
  EXPECT_EQ(chunk.schema_version, 1U);
  EXPECT_EQ(chunk.stats.row_count, 5U);
  EXPECT_EQ(chunk.columns.size(), 3U);
  for (std::size_t c = 0; c < 3; ++c) {
    // Whole-number floats with a narrow range: frame-of-reference.
    EXPECT_EQ(chunk.columnEncoding(c), EncodingType::kFrameOfReference);
  }
}

// ===========================================================================
// Test 2: Read back sealed values
// ===========================================================================

TEST(ChunkTest, ReadBackSealedValues) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "x"),
      makeCol(2, PrimitiveType::kFloat64, "y"),
      makeCol(3, PrimitiveType::kInt32, "z"),
  };
  TopicChunkBuilder builder(/*topic_id=*/20, /*schema_id=*/2, std::move(cols), /*max_rows=*/100);

  Timestamp timestamps[] = {1000, 1100, 1200, 1300, 1400};
  float x_vals[] = {1.5F, 2.5F, 3.5F, 4.5F, 5.5F};
  double y_vals[] = {10.0, 20.0, 30.0, 40.0, 50.0};
  int32_t z_vals[] = {-1, 0, 1, 2, 3};

  for (int i = 0; i < 5; ++i) {
    builder.beginRow(timestamps[i]);
    builder.set(0, x_vals[i]);
    builder.set(1, y_vals[i]);
    builder.set(2, z_vals[i]);
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();

  // Read back timestamps
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(chunk.readTimestamp(i), timestamps[i]) << "row " << i;
  }

  // Read back float32 column as double
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_FLOAT_EQ(static_cast<float>(chunk.readNumericAsDouble(0, i)), x_vals[i]) << "row " << i;
  }

  // Read back float64 column
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(1, i), y_vals[i]) << "row " << i;
  }

  // Read back int32 column as double (may be FOR-encoded since range is small)
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(2, i), static_cast<double>(z_vals[i])) << "row " << i;
  }
}

// ===========================================================================
// Test 3: is_full
// ===========================================================================

TEST(ChunkTest, IsFull) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/30, /*schema_id=*/1, std::move(cols), /*max_rows=*/3);

  EXPECT_FALSE(builder.isFull());
  EXPECT_EQ(builder.rowCount(), 0U);

  for (uint32_t i = 0; i < 3; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, static_cast<float>(i));
    builder.finishRow();
  }

  EXPECT_TRUE(builder.isFull());
  EXPECT_EQ(builder.rowCount(), 3U);
}

// ===========================================================================
// Test 4: String column
// ===========================================================================

TEST(ChunkTest, StringColumn) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kString, "label"),
  };
  TopicChunkBuilder builder(/*topic_id=*/40, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  std::string_view strings[] = {"hello", "world", "hello", "world"};
  for (int i = 0; i < 4; ++i) {
    builder.beginRow(static_cast<Timestamp>(i * 100));
    builder.set(0, strings[i]);
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();

  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kDictionary);
  const auto& dict = std::get<encoding::DictionaryEncoded>(chunk.columns[0].data);
  // 2 unique strings: "hello" and "world"
  EXPECT_EQ(dict.dictionary.size(), 2U);

  // Read back all strings
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(chunk.readString(0, i), strings[i]) << "row " << i;
  }
}

// ===========================================================================
// Test 5: Bool column
// ===========================================================================

TEST(ChunkTest, BoolColumn) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kBool, "flag"),
  };
  TopicChunkBuilder builder(/*topic_id=*/50, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  bool bools[] = {true, false, true, true, false};
  for (int i = 0; i < 5; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, bools[i]);
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();

  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kPackedBool);

  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(chunk.readBool(0, i), bools[i]) << "row " << i;
  }
}

// ===========================================================================
// Test 6: Null handling
// ===========================================================================

TEST(ChunkTest, NullHandling) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat64, "val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/60, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  // Row 0: 10.0, Row 1: null, Row 2: 30.0, Row 3: null, Row 4: 50.0
  builder.beginRow(100);
  builder.set(0, 10.0);
  builder.finishRow();

  builder.beginRow(200);
  builder.setNull(0);
  builder.finishRow();

  builder.beginRow(300);
  builder.set(0, 30.0);
  builder.finishRow();

  builder.beginRow(400);
  builder.setNull(0);
  builder.finishRow();

  builder.beginRow(500);
  builder.set(0, 50.0);
  builder.finishRow();

  const auto& stats = builder.stats();
  EXPECT_EQ(stats.column_stats[0].null_count, 2U);

  TopicChunk chunk = builder.seal();

  EXPECT_FALSE(chunk.isNull(0, 0));
  EXPECT_TRUE(chunk.isNull(0, 1));
  EXPECT_FALSE(chunk.isNull(0, 2));
  EXPECT_TRUE(chunk.isNull(0, 3));
  EXPECT_FALSE(chunk.isNull(0, 4));

  // Non-null values should read back correctly
  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(0, 0), 10.0);
  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(0, 2), 30.0);
  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(0, 4), 50.0);
}

// ===========================================================================
// Test 7: Mixed types
// ===========================================================================

TEST(ChunkTest, MixedTypes) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "position"),
      makeCol(2, PrimitiveType::kString, "label"),
      makeCol(3, PrimitiveType::kBool, "active"),
  };
  TopicChunkBuilder builder(/*topic_id=*/70, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  builder.beginRow(1000);
  builder.set(0, 1.5F);
  builder.set(1, std::string_view("alpha"));
  builder.set(2, true);
  builder.finishRow();

  builder.beginRow(2000);
  builder.set(0, 2.5F);
  builder.set(1, std::string_view("beta"));
  builder.set(2, false);
  builder.finishRow();

  builder.beginRow(3000);
  builder.set(0, 3.5F);
  builder.set(1, std::string_view("alpha"));
  builder.set(2, true);
  builder.finishRow();

  TopicChunk chunk = builder.seal();

  // Check encodings
  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kRaw);
  EXPECT_EQ(chunk.columnEncoding(1), EncodingType::kDictionary);
  EXPECT_EQ(chunk.columnEncoding(2), EncodingType::kPackedBool);

  // Read back all values
  EXPECT_FLOAT_EQ(static_cast<float>(chunk.readNumericAsDouble(0, 0)), 1.5F);
  EXPECT_FLOAT_EQ(static_cast<float>(chunk.readNumericAsDouble(0, 1)), 2.5F);
  EXPECT_FLOAT_EQ(static_cast<float>(chunk.readNumericAsDouble(0, 2)), 3.5F);

  EXPECT_EQ(chunk.readString(1, 0), "alpha");
  EXPECT_EQ(chunk.readString(1, 1), "beta");
  EXPECT_EQ(chunk.readString(1, 2), "alpha");

  EXPECT_TRUE(chunk.readBool(2, 0));
  EXPECT_FALSE(chunk.readBool(2, 1));
  EXPECT_TRUE(chunk.readBool(2, 2));

  // Timestamps
  EXPECT_EQ(chunk.readTimestamp(0), 1000);
  EXPECT_EQ(chunk.readTimestamp(1), 2000);
  EXPECT_EQ(chunk.readTimestamp(2), 3000);
}

// ===========================================================================
// Test 8: Column stats (min/max, is_constant, run_count)
// ===========================================================================

TEST(ChunkTest, ColumnStatsNumeric) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat64, "varying"),
      makeCol(2, PrimitiveType::kFloat64, "constant"),
  };
  TopicChunkBuilder builder(/*topic_id=*/80, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  // varying: -5, 0, 10, 3, 10
  // constant: 42, 42, 42, 42, 42
  double varying[] = {-5.0, 0.0, 10.0, 3.0, 10.0};
  for (int i = 0; i < 5; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, varying[i]);
    builder.set(1, 42.0);
    builder.finishRow();
  }

  const auto& stats = builder.stats();

  // Varying column
  EXPECT_DOUBLE_EQ(*stats.column_stats[0].min_value, -5.0);
  EXPECT_DOUBLE_EQ(*stats.column_stats[0].max_value, 10.0);
  EXPECT_FALSE(stats.column_stats[0].is_constant);
  // run_count: -5->0 (change), 0->10 (change), 10->3 (change), 3->10 (change) = 1 + 4 = 5
  EXPECT_EQ(stats.column_stats[0].run_count, 5U);

  // Constant column
  EXPECT_DOUBLE_EQ(*stats.column_stats[1].min_value, 42.0);
  EXPECT_DOUBLE_EQ(*stats.column_stats[1].max_value, 42.0);
  EXPECT_TRUE(stats.column_stats[1].is_constant);
  EXPECT_EQ(stats.column_stats[1].run_count, 1U);
}

// ===========================================================================
// Test: Unique chunk IDs
// ===========================================================================

TEST(ChunkTest, UniqueChunkIds) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "val"),
  };

  TopicChunkBuilder builder1(1, 1, cols, 10);
  builder1.beginRow(100);
  builder1.set(0, 1.0F);
  builder1.finishRow();
  TopicChunk c1 = builder1.seal();

  TopicChunkBuilder builder2(1, 1, cols, 10);
  builder2.beginRow(200);
  builder2.set(0, 2.0F);
  builder2.finishRow();
  TopicChunk c2 = builder2.seal();

  EXPECT_NE(c1.id, c2.id);
  EXPECT_NE(c1.id, kInvalidChunkId);
  EXPECT_NE(c2.id, kInvalidChunkId);
}

// ===========================================================================
// Test: Integer types round-trip
// ===========================================================================

TEST(ChunkTest, IntegerTypesRoundTrip) {
  // int8/int16 logical types widen to int64 storage; int32 has its own storage;
  // uint8/uint16/uint32 widen to uint64 storage.
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kInt8, "i8"),    makeCol(2, PrimitiveType::kInt16, "i16"),
      makeCol(3, PrimitiveType::kInt32, "i32"),  makeCol(4, PrimitiveType::kInt64, "i64"),
      makeCol(5, PrimitiveType::kUint8, "u8"),   makeCol(6, PrimitiveType::kUint16, "u16"),
      makeCol(7, PrimitiveType::kUint32, "u32"), makeCol(8, PrimitiveType::kUint64, "u64"),
  };
  TopicChunkBuilder builder(/*topic_id=*/90, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  builder.beginRow(1000);
  builder.set(0, static_cast<int64_t>(-42));    // int8 → int64 storage
  builder.set(1, static_cast<int64_t>(-1000));  // int16 → int64 storage
  builder.set(2, -999999);                      // int32 → int32 storage
  builder.set(3, static_cast<int64_t>(123456789012345LL));
  builder.set(4, static_cast<uint64_t>(255));          // uint8 → uint64 storage
  builder.set(5, static_cast<uint64_t>(65535));        // uint16 → uint64 storage
  builder.set(6, static_cast<uint64_t>(4000000000U));  // uint32 → uint64 storage
  builder.set(7, static_cast<uint64_t>(18000000000000000000ULL));
  builder.finishRow();

  TopicChunk chunk = builder.seal();

  // Single-row chunks will be constant-encoded, but readback should be the same
  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(0, 0), -42.0);
  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(1, 0), -1000.0);
  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(2, 0), -999999.0);
  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(3, 0), 123456789012345.0);
  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(4, 0), 255.0);
  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(5, 0), 65535.0);
  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(6, 0), 4000000000.0);
  // uint64 large values may lose precision in double, so just check close
  EXPECT_NEAR(chunk.readNumericAsDouble(7, 0), 1.8e19, 1e4);
}

// ===========================================================================
// Test: No nulls means is_null always returns false
// ===========================================================================

TEST(ChunkTest, NoNullsIsNullReturnsFalse) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/100, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  for (int i = 0; i < 3; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, static_cast<float>(i));
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();

  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_FALSE(chunk.isNull(0, i)) << "row " << i;
  }
}

// ===========================================================================
// Test: String column is_constant and run_count
// ===========================================================================

TEST(ChunkTest, StringColumnStats) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kString, "tag"),
  };
  TopicChunkBuilder builder(/*topic_id=*/110, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  // All same string -> is_constant = true, run_count = 1
  for (int i = 0; i < 4; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, std::string_view("same"));
    builder.finishRow();
  }

  const auto& stats = builder.stats();
  EXPECT_TRUE(stats.column_stats[0].is_constant);
  EXPECT_EQ(stats.column_stats[0].run_count, 1U);
  // String columns should not have numeric min/max
  EXPECT_FALSE(stats.column_stats[0].min_value.has_value());
  EXPECT_FALSE(stats.column_stats[0].max_value.has_value());
}

// ===========================================================================
// Test: Empty chunk (0 rows)
// ===========================================================================

TEST(ChunkTest, EmptyChunk) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/120, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  EXPECT_EQ(builder.rowCount(), 0U);
  EXPECT_FALSE(builder.isFull());

  TopicChunk chunk = builder.seal();
  EXPECT_EQ(chunk.stats.row_count, 0U);
  EXPECT_TRUE(chunk.timestamps.empty());
}

// ===========================================================================
// Test: Bulk read column as doubles (float32)
// ===========================================================================

TEST(ChunkTest, BulkReadFloat32) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "x"),
  };
  TopicChunkBuilder builder(/*topic_id=*/130, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  constexpr int kRows = 10;
  for (int i = 0; i < kRows; ++i) {
    builder.beginRow(static_cast<Timestamp>(i * 100));
    builder.set(0, static_cast<float>(i) * 1.5F);
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();

  // Read all rows
  std::vector<double> out(kRows);
  chunk.readColumnAsDoubles(0, Span<double>(out), 0);
  for (int i = 0; i < kRows; ++i) {
    EXPECT_FLOAT_EQ(static_cast<float>(out[static_cast<std::size_t>(i)]), static_cast<float>(i) * 1.5F) << "row " << i;
  }

  // Read a sub-range [3, 7)
  std::vector<double> sub(4);
  chunk.readColumnAsDoubles(0, Span<double>(sub), 3);
  for (int i = 0; i < 4; ++i) {
    EXPECT_FLOAT_EQ(static_cast<float>(sub[static_cast<std::size_t>(i)]), static_cast<float>(i + 3) * 1.5F)
        << "sub row " << i;
  }
}

// ===========================================================================
// Test: Bulk read column as doubles (int64)
// ===========================================================================

TEST(ChunkTest, BulkReadInt64) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kInt64, "val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/140, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  constexpr int kRows = 5;
  int64_t values[] = {-100, 0, 42, 999, -1};
  for (int i = 0; i < kRows; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, values[i]);
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();

  std::vector<double> out(kRows);
  chunk.readColumnAsDoubles(0, Span<double>(out), 0);
  for (int i = 0; i < kRows; ++i) {
    EXPECT_DOUBLE_EQ(out[static_cast<std::size_t>(i)], static_cast<double>(values[i])) << "row " << i;
  }
}

// ===========================================================================
// Test: Bulk read bool/string columns returns NaN
// ===========================================================================

TEST(ChunkTest, BulkReadBoolStringReturnsNaN) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kBool, "flag"),
      makeCol(2, PrimitiveType::kString, "label"),
  };
  TopicChunkBuilder builder(/*topic_id=*/150, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  builder.beginRow(100);
  builder.set(0, true);
  builder.set(1, std::string_view("hello"));
  builder.finishRow();

  builder.beginRow(200);
  builder.set(0, false);
  builder.set(1, std::string_view("world"));
  builder.finishRow();

  TopicChunk chunk = builder.seal();

  std::vector<double> out(2);

  chunk.readColumnAsDoubles(0, Span<double>(out.data(), 2), 0);
  EXPECT_TRUE(std::isnan(out[0]));
  EXPECT_TRUE(std::isnan(out[1]));

  chunk.readColumnAsDoubles(1, Span<double>(out.data(), 2), 0);
  EXPECT_TRUE(std::isnan(out[0]));
  EXPECT_TRUE(std::isnan(out[1]));
}

// ===========================================================================
// Test: Bulk read zero rows (boundary)
// ===========================================================================

TEST(ChunkTest, BulkReadZeroRows) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat64, "x"),
  };
  TopicChunkBuilder builder(/*topic_id=*/160, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  builder.beginRow(100);
  builder.set(0, 1.0);
  builder.finishRow();

  TopicChunk chunk = builder.seal();

  // Should not crash when reading 0 rows
  double dummy = 0.0;
  chunk.readColumnAsDoubles(0, Span<double>(&dummy, 0), 0);
  EXPECT_DOUBLE_EQ(dummy, 0.0);  // untouched
}

// ===========================================================================
// NEW: Constant int column gets constant encoding
// ===========================================================================

TEST(ChunkTest, ConstantIntColumnGetsConstantEncoding) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kInt32, "const_val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/200, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);

  for (int i = 0; i < 100; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, 42);
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();

  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kConstant);

  // Read back every row
  for (std::size_t i = 0; i < 100; ++i) {
    EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(0, i), 42.0) << "row " << i;
  }

  // Bulk read
  std::vector<double> out(100);
  chunk.readColumnAsDoubles(0, Span<double>(out), 0);
  for (std::size_t i = 0; i < 100; ++i) {
    EXPECT_DOUBLE_EQ(out[i], 42.0) << "bulk row " << i;
  }
}

// ===========================================================================
// NEW: Constant float column gets constant encoding
// ===========================================================================

TEST(ChunkTest, ConstantFloatColumnGetsConstantEncoding) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat64, "const_f64"),
  };
  TopicChunkBuilder builder(/*topic_id=*/201, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);

  for (int i = 0; i < 50; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, 3.14);
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();

  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kConstant);

  for (std::size_t i = 0; i < 50; ++i) {
    EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(0, i), 3.14) << "row " << i;
  }
}

// ===========================================================================
// NEW: Narrow range int column gets FOR encoding
// ===========================================================================

TEST(ChunkTest, NarrowRangeIntColumnGetsFOR) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kInt32, "narrow"),
  };
  TopicChunkBuilder builder(/*topic_id=*/202, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);

  // Values in [1000, 1100] — range=100, fits in uint8 (1 byte vs 4 native)
  for (int i = 0; i < 101; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, 1000 + static_cast<int32_t>(i));
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();

  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kFrameOfReference);
  const auto& for_enc = std::get<encoding::FrameOfReferenceEncoded>(chunk.columns[0].data);
  EXPECT_EQ(for_enc.offset_bytes, 1);
  EXPECT_EQ(for_enc.reference, 1000);

  // Per-row read
  for (std::size_t i = 0; i < 101; ++i) {
    EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(0, i), 1000.0 + static_cast<double>(i)) << "row " << i;
  }

  // Bulk read
  std::vector<double> out(101);
  chunk.readColumnAsDoubles(0, Span<double>(out), 0);
  for (std::size_t i = 0; i < 101; ++i) {
    EXPECT_DOUBLE_EQ(out[i], 1000.0 + static_cast<double>(i)) << "bulk row " << i;
  }
}

// ===========================================================================
// NEW: Wide range int column stays raw
// ===========================================================================

TEST(ChunkTest, WideRangeIntColumnStaysRaw) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kInt32, "wide"),
  };
  TopicChunkBuilder builder(/*topic_id=*/203, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);

  // Range that spans full int32 — FOR can't narrow below 4 bytes
  builder.beginRow(0);
  builder.set(0, -2000000000);
  builder.finishRow();

  builder.beginRow(1);
  builder.set(0, 2000000000);
  builder.finishRow();

  TopicChunk chunk = builder.seal();

  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kRaw);

  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(0, 0), -2000000000.0);
  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(0, 1), 2000000000.0);
}

// ===========================================================================
// NEW: Float column with fractional values stays raw (never gets FOR)
// ===========================================================================

TEST(ChunkTest, FractionalFloatColumnStaysRaw) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "f32"),
      makeCol(2, PrimitiveType::kFloat64, "f64"),
  };
  TopicChunkBuilder builder(/*topic_id=*/204, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);

  // Varying float values
  for (int i = 0; i < 10; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, static_cast<float>(i) * 0.1F);
    builder.set(1, static_cast<double>(i) * 0.1);
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();

  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kRaw);
  EXPECT_EQ(chunk.columnEncoding(1), EncodingType::kRaw);
}

// ===========================================================================
// Float columns whose chunk values are all whole numbers get FOR
// ===========================================================================

TEST(ChunkTest, IntegralFloat64ChunkGetsFOR) {
  std::vector<ColumnDescriptor> cols = {makeCol(1, PrimitiveType::kFloat64, "current_mA")};
  TopicChunkBuilder builder(/*topic_id=*/205, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);
  for (int i = 0; i < 100; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, static_cast<double>(i - 60));  // -60..39: range 99 -> 1-byte offsets
    builder.finishRow();
  }
  TopicChunk chunk = builder.seal();

  ASSERT_EQ(chunk.columnEncoding(0), EncodingType::kFrameOfReference);
  const auto& for_enc = std::get<encoding::FrameOfReferenceEncoded>(chunk.columns[0].data);
  EXPECT_EQ(for_enc.offset_bytes, 1);
  EXPECT_EQ(for_enc.reference, -60);
  EXPECT_EQ(chunk.columns[0].descriptor->logical_type, PrimitiveType::kFloat64);  // declared type unchanged

  std::vector<double> out(100);
  chunk.readColumnAsDoubles(0, Span<double>(out), 0);
  for (std::size_t i = 0; i < 100; ++i) {
    const double expected = static_cast<double>(i) - 60.0;
    EXPECT_EQ(chunk.readNumericAsDouble(0, i), expected) << "row " << i;
    EXPECT_EQ(out[i], expected) << "bulk row " << i;
  }
}

TEST(ChunkTest, IntegralFloat32ChunkGetsFOROnlyWhenNarrower) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "narrow"),  // range 100 -> 1 byte < 4
      makeCol(2, PrimitiveType::kFloat32, "wide"),    // range ~2^24 -> 4 bytes, not < 4
  };
  TopicChunkBuilder builder(/*topic_id=*/206, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);
  for (int i = 0; i <= 100; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, 16777216.0F + static_cast<float>(2 * i));  // above 2^24: still whole, exact floats
    builder.set(1, i == 0 ? 0.0F : 16777216.0F);
    builder.finishRow();
  }
  TopicChunk chunk = builder.seal();

  ASSERT_EQ(chunk.columnEncoding(0), EncodingType::kFrameOfReference);
  EXPECT_EQ(chunk.columnEncoding(1), EncodingType::kRaw);
  for (std::size_t i = 0; i <= 100; ++i) {
    EXPECT_EQ(chunk.readNumericAsDouble(0, i), 16777216.0 + 2.0 * static_cast<double>(i)) << "row " << i;
  }
}

TEST(ChunkTest, IntegralFloatChunkWithNullsGetsFOR) {
  std::vector<ColumnDescriptor> cols = {makeCol(1, PrimitiveType::kFloat64, "x")};
  TopicChunkBuilder builder(/*topic_id=*/207, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);
  for (int i = 0; i < 10; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    if (i % 3 == 0) {
      builder.setNull(0);
    } else {
      builder.set(0, 1000.0 + static_cast<double>(i));
    }
    builder.finishRow();
  }
  TopicChunk chunk = builder.seal();

  ASSERT_EQ(chunk.columnEncoding(0), EncodingType::kFrameOfReference);
  for (std::size_t i = 0; i < 10; ++i) {
    ASSERT_EQ(chunk.isNull(0, i), i % 3 == 0) << "row " << i;
    if (i % 3 != 0) {
      EXPECT_EQ(chunk.readNumericAsDouble(0, i), 1000.0 + static_cast<double>(i)) << "row " << i;
    }
  }
  // Bulk read starting mid-chunk: non-null rows 4, 5, 7, 8.
  std::vector<double> out(6);
  chunk.readColumnAsDoubles(0, Span<double>(out), 4);
  for (std::size_t k = 0; k < out.size(); ++k) {
    const std::size_t row = 4 + k;
    if (row % 3 != 0) {
      EXPECT_EQ(out[k], 1000.0 + static_cast<double>(row)) << "bulk row " << row;
    }
  }
}

TEST(ChunkTest, LargeMagnitudeIntegralFloat64KeepsExactValues) {
  // 1e15 is beyond float32 but well inside the exact-integer range of double.
  std::vector<ColumnDescriptor> cols = {makeCol(1, PrimitiveType::kFloat64, "big")};
  TopicChunkBuilder builder(/*topic_id=*/208, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);
  for (int i = 0; i < 20; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, 1e15 + static_cast<double>(i * 1000));
    builder.finishRow();
  }
  TopicChunk chunk = builder.seal();

  ASSERT_EQ(chunk.columnEncoding(0), EncodingType::kFrameOfReference);
  for (std::size_t i = 0; i < 20; ++i) {
    EXPECT_EQ(chunk.readNumericAsDouble(0, i), 1e15 + static_cast<double>(i) * 1000.0) << "row " << i;
  }
}

// Whole doubles beyond 2^53 and at both ends of the int64 range: FOR must
// reproduce every value bit for bit.
TEST(ChunkTest, WholeDoublesAtExtremeMagnitudesRoundTripBitExact) {
  const std::vector<std::pair<double, double>> series = {
      {0x1p60, 256.0},                // above 2^53: spacing 256
      {-0x1p63, 2048.0},              // int64 min upward: spacing 2048
      {0x1p63 - 1024.0 * 64, 1024.0}  // up to the largest double below 2^63
  };
  for (const auto& [start, step] : series) {
    std::vector<ColumnDescriptor> cols = {makeCol(1, PrimitiveType::kFloat64, "x")};
    TopicChunkBuilder builder(/*topic_id=*/210, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);
    std::vector<double> expected;
    for (int i = 0; i < 64; ++i) {
      expected.push_back(start + step * static_cast<double>(i));
      builder.beginRow(static_cast<Timestamp>(i));
      builder.set(0, expected.back());
      builder.finishRow();
    }
    TopicChunk chunk = builder.seal();

    ASSERT_EQ(chunk.columnEncoding(0), EncodingType::kFrameOfReference) << "start " << start;
    for (std::size_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(std::bit_cast<uint64_t>(chunk.readNumericAsDouble(0, i)), std::bit_cast<uint64_t>(expected[i]))
          << "start " << start << " row " << i;
    }
  }
}

TEST(ChunkTest, WholeFloatOffsetWidthFollowsRange) {
  const std::vector<std::pair<double, uint8_t>> cases = {
      {255.0, 1}, {256.0, 2}, {65535.0, 2}, {65536.0, 4}, {4294967295.0, 4}};
  for (const auto& [range, offset_bytes] : cases) {
    std::vector<ColumnDescriptor> cols = {makeCol(1, PrimitiveType::kFloat64, "x")};
    TopicChunkBuilder builder(/*topic_id=*/211, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);
    for (const double v : {-7.0, -7.0 + range, 3.0}) {
      builder.beginRow(0);
      builder.set(0, v);
      builder.finishRow();
    }
    TopicChunk chunk = builder.seal();

    ASSERT_EQ(chunk.columnEncoding(0), EncodingType::kFrameOfReference) << "range " << range;
    EXPECT_EQ(std::get<encoding::FrameOfReferenceEncoded>(chunk.columns[0].data).offset_bytes, offset_bytes)
        << "range " << range;
  }
}

TEST(ChunkTest, WholeDoublesWithRangeBeyondUint32StayRaw) {
  const std::vector<std::vector<double>> cases = {
      {0.0, 4294967296.0},        // range 2^32
      {-0x1p62, 0x1p62},          // opposite signs, range 2^63
      {-0x1p63, 0x1p63 - 1024.0}  // the full int64 span
  };
  for (const auto& values : cases) {
    std::vector<ColumnDescriptor> cols = {makeCol(1, PrimitiveType::kFloat64, "x")};
    TopicChunkBuilder builder(/*topic_id=*/212, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);
    for (const double v : values) {
      builder.beginRow(0);
      builder.set(0, v);
      builder.finishRow();
    }
    TopicChunk chunk = builder.seal();

    EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kRaw) << "last " << values.back();
    for (std::size_t i = 0; i < values.size(); ++i) {
      EXPECT_EQ(chunk.readNumericAsDouble(0, i), values[i]);
    }
  }
}

TEST(ChunkTest, FloatChunkWithNonIntegralOrSpecialValueStaysRaw) {
  const std::vector<double> specials = {0.5,  std::nan(""), std::numeric_limits<double>::infinity(),
                                        -0.0, 0x1p63,       -0x1p64};
  for (const double special : specials) {
    std::vector<ColumnDescriptor> cols = {makeCol(1, PrimitiveType::kFloat64, "x")};
    TopicChunkBuilder builder(/*topic_id=*/209, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);
    for (int i = 0; i < 10; ++i) {
      builder.beginRow(static_cast<Timestamp>(i));
      builder.set(0, i == 5 ? special : static_cast<double>(i));
      builder.finishRow();
    }
    TopicChunk chunk = builder.seal();

    EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kRaw) << "special " << special;
    const double read_back = chunk.readNumericAsDouble(0, 5);
    if (std::isnan(special)) {
      EXPECT_TRUE(std::isnan(read_back));
    } else {
      EXPECT_EQ(read_back, special);
      EXPECT_EQ(std::signbit(read_back), std::signbit(special)) << "special " << special;
    }
  }
}

// ===========================================================================
// NEW: Constant bool column gets constant encoding
// ===========================================================================

TEST(ChunkTest, ConstantBoolGetsConstantEncoding) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kBool, "always_true"),
  };
  TopicChunkBuilder builder(/*topic_id=*/205, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);

  for (int i = 0; i < 50; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, true);
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();

  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kConstant);

  for (std::size_t i = 0; i < 50; ++i) {
    EXPECT_TRUE(chunk.readBool(0, i)) << "row " << i;
  }
}

// ===========================================================================
// Bulk append: basic float32
// ===========================================================================

TEST(ChunkTest, BulkAppendFloat32) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "x"),
      makeCol(2, PrimitiveType::kFloat32, "y"),
  };
  TopicChunkBuilder builder(/*topic_id=*/500, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);

  constexpr std::size_t kN = 100;
  std::vector<Timestamp> ts(kN);
  std::vector<float> x_vals(kN), y_vals(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    ts[i] = static_cast<Timestamp>(i) * 10;
    x_vals[i] = static_cast<float>(i) * 1.0F;
    y_vals[i] = static_cast<float>(i) * 2.0F;
  }

  builder.appendTimestamps(ts);
  builder.appendColumn<float>(0, x_vals);
  builder.appendColumn<float>(1, y_vals);
  builder.finishBulkAppend();

  EXPECT_EQ(builder.rowCount(), 100U);
  EXPECT_EQ(builder.lastTimestamp(), 990);

  TopicChunk chunk = builder.seal();
  EXPECT_EQ(chunk.stats.row_count, 100U);
  EXPECT_EQ(chunk.stats.t_min, 0);
  EXPECT_EQ(chunk.stats.t_max, 990);

  // Verify round-trip
  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(chunk.readTimestamp(i), static_cast<Timestamp>(i) * 10);
    EXPECT_FLOAT_EQ(static_cast<float>(chunk.readNumericAsDouble(0, i)), x_vals[i]);
    EXPECT_FLOAT_EQ(static_cast<float>(chunk.readNumericAsDouble(1, i)), y_vals[i]);
  }
}

// ===========================================================================
// Bulk append: stats are correct
// ===========================================================================

TEST(ChunkTest, BulkAppendStats) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat64, "val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/501, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);

  const double data[] = {3.0, 1.0, 4.0, 1.0, 5.0};
  const Timestamp ts[] = {10, 20, 30, 40, 50};

  builder.appendTimestamps(Span<const Timestamp>(ts, 5));
  builder.appendColumn(0, Span<const double>(data, 5));
  builder.finishBulkAppend();

  const auto& cs = builder.stats().column_stats[0];
  EXPECT_DOUBLE_EQ(*cs.min_value, 1.0);
  EXPECT_DOUBLE_EQ(*cs.max_value, 5.0);
  EXPECT_FALSE(cs.is_constant);
  EXPECT_GT(cs.run_count, 1U);
}

// ===========================================================================
// Bulk append: constant column
// ===========================================================================

TEST(ChunkTest, BulkAppendConstantColumn) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kInt32, "const"),
  };
  TopicChunkBuilder builder(/*topic_id=*/502, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);

  constexpr std::size_t kN = 50;
  std::vector<Timestamp> ts(kN);
  std::vector<int32_t> vals(kN, 42);
  for (std::size_t i = 0; i < kN; ++i) {
    ts[i] = static_cast<Timestamp>(i);
  }

  builder.appendTimestamps(ts);
  builder.appendColumn<int32_t>(0, vals);
  builder.finishBulkAppend();

  const auto& cs = builder.stats().column_stats[0];
  EXPECT_TRUE(cs.is_constant);
  EXPECT_DOUBLE_EQ(*cs.min_value, 42.0);
  EXPECT_DOUBLE_EQ(*cs.max_value, 42.0);

  TopicChunk chunk = builder.seal();
  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kConstant);
  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(0, i), 42.0);
  }
}

// ===========================================================================
// Bulk append: string column
// ===========================================================================

TEST(ChunkTest, BulkAppendStrings) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kString, "name"),
  };
  TopicChunkBuilder builder(/*topic_id=*/503, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);

  const char string_data[] = "alphaBravoCharlie";
  const uint32_t offsets[] = {0, 5, 10, 17};
  const Timestamp ts[] = {10, 20, 30};

  builder.appendTimestamps(Span<const Timestamp>(ts, 3));
  builder.appendColumnStrings(0, Span<const uint32_t>(offsets, 4), Span<const char>(string_data, 17));
  builder.finishBulkAppend();

  EXPECT_EQ(builder.rowCount(), 3U);

  TopicChunk chunk = builder.seal();
  EXPECT_EQ(chunk.readString(0, 0), "alpha");
  EXPECT_EQ(chunk.readString(0, 1), "Bravo");
  EXPECT_EQ(chunk.readString(0, 2), "Charlie");
}

// ===========================================================================
// Bulk append: remaining_capacity
// ===========================================================================

TEST(ChunkTest, BulkRemainingCapacity) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "x"),
  };
  TopicChunkBuilder builder(/*topic_id=*/504, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  EXPECT_EQ(builder.remainingCapacity(), 100U);

  const Timestamp ts[] = {1, 2, 3};
  const float vals[] = {1.0F, 2.0F, 3.0F};
  builder.appendTimestamps(Span<const Timestamp>(ts, 3));
  builder.appendColumn(0, Span<const float>(vals, 3));
  builder.finishBulkAppend();

  EXPECT_EQ(builder.remainingCapacity(), 97U);
}

// ===========================================================================
// Bulk append: with validity bitmap
// ===========================================================================

TEST(ChunkTest, BulkAppendWithValidity) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat64, "val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/505, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);

  const double data[] = {1.0, 0.0, 3.0, 0.0};
  const Timestamp ts[] = {10, 20, 30, 40};

  builder.appendTimestamps(Span<const Timestamp>(ts, 4));
  builder.appendColumn(0, Span<const double>(data, 4));
  // validity: bits [1, 0, 1, 0] = 0b0101 = 0x05
  const uint8_t bitmap[] = {0x05};
  builder.appendColumnValidity(0, BitSpan{Span<const uint8_t>(bitmap, 1), 0, 4});
  builder.finishBulkAppend();

  TopicChunk chunk = builder.seal();
  EXPECT_FALSE(chunk.isNull(0, 0));
  EXPECT_TRUE(chunk.isNull(0, 1));
  EXPECT_FALSE(chunk.isNull(0, 2));
  EXPECT_TRUE(chunk.isNull(0, 3));
  EXPECT_EQ(chunk.stats.column_stats[0].null_count, 2U);
}

// ===========================================================================
// Bulk append: mixed types
// ===========================================================================

TEST(ChunkTest, BulkAppendMixedTypes) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kFloat32, "f32"),
      makeCol(2, PrimitiveType::kInt64, "i64"),
      makeCol(3, PrimitiveType::kBool, "b"),
  };
  TopicChunkBuilder builder(/*topic_id=*/506, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);

  const Timestamp ts[] = {100, 200, 300};
  const float f32[] = {1.0F, 2.0F, 3.0F};
  const int64_t i64[] = {10, 20, 30};
  const uint8_t bools[] = {1, 0, 1};

  builder.appendTimestamps(Span<const Timestamp>(ts, 3));
  builder.appendColumn(0, Span<const float>(f32, 3));
  builder.appendColumn(1, Span<const int64_t>(i64, 3));
  builder.appendColumn(2, Span<const uint8_t>(bools, 3));
  builder.finishBulkAppend();

  TopicChunk chunk = builder.seal();
  EXPECT_EQ(chunk.stats.row_count, 3U);
  EXPECT_FLOAT_EQ(static_cast<float>(chunk.readNumericAsDouble(0, 1)), 2.0F);
  EXPECT_DOUBLE_EQ(chunk.readNumericAsDouble(1, 2), 30.0);
  EXPECT_TRUE(chunk.readBool(2, 0));
  EXPECT_FALSE(chunk.readBool(2, 1));
  EXPECT_TRUE(chunk.readBool(2, 2));
}

// ===========================================================================
// BUG-3: readNumericAsInt64 precision loss through double for constant/FOR
// ===========================================================================

TEST(ChunkTest, ReadInt64PrecisionConstant) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kInt64, "big"),
  };
  TopicChunkBuilder builder(/*topic_id=*/700, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  // Value that exceeds 2^53 — not exactly representable as double
  constexpr int64_t kBig = (int64_t{1} << 53) + 1;  // 9007199254740993

  // All same value → constant encoding
  for (int i = 0; i < 3; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, kBig);
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();
  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kConstant);

  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(chunk.readNumericAsInt64(0, i), kBig) << "row " << i;
  }
}

TEST(ChunkTest, ReadInt64PrecisionFOR) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kInt64, "big"),
  };
  TopicChunkBuilder builder(/*topic_id=*/701, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  // Two values > 2^53 with small range → FOR encoding
  constexpr int64_t kBase = (int64_t{1} << 53) + 1;
  constexpr int64_t kValues[] = {kBase, kBase + 100, kBase + 50};

  for (int i = 0; i < 3; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, kValues[i]);
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();
  // Range is 100, fits in 1 byte offset vs 8 byte native → FOR
  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kFrameOfReference);

  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(chunk.readNumericAsInt64(0, i), kValues[i]) << "row " << i;
  }
}

// ===========================================================================
// BUG-4: readNumericAsUint64 precision loss through double for constant/FOR
// ===========================================================================

TEST(ChunkTest, ReadUint64PrecisionConstant) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kUint64, "big_u"),
  };
  TopicChunkBuilder builder(/*topic_id=*/702, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  constexpr uint64_t kBig = (uint64_t{1} << 53) + 1;

  for (int i = 0; i < 3; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    builder.set(0, kBig);
    builder.finishRow();
  }

  TopicChunk chunk = builder.seal();
  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kConstant);

  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(chunk.readNumericAsUint64(0, i), kBig) << "row " << i;
  }
}

// ===========================================================================
// BUG-1: Stats tracking loses int64 precision via double cast
// BUG-2: FOR reference computed from lossy double min/max
// ===========================================================================

TEST(ChunkTest, Int64StatsPreservePrecision) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kInt64, "precise"),
  };
  TopicChunkBuilder builder(/*topic_id=*/703, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  // Two int64 values beyond 2^53 that map to the same double
  constexpr int64_t kA = (int64_t{1} << 54) - 1;  // rounds to 2^54
  constexpr int64_t kB = (int64_t{1} << 54) + 1;  // also rounds to 2^54
  // These must NOT be detected as constant (even though static_cast<double>(kA) == static_cast<double>(kB))
  static_assert(
      static_cast<double>(kA) == static_cast<double>(kB),
      "test premise: kA and kB must have the same double representation");

  builder.beginRow(0);
  builder.set(0, kA);
  builder.finishRow();

  builder.beginRow(1);
  builder.set(0, kB);
  builder.finishRow();

  TopicChunk chunk = builder.seal();

  // The column must NOT be constant-encoded since kA != kB
  EXPECT_NE(chunk.columnEncoding(0), EncodingType::kConstant);

  // Values must round-trip exactly
  EXPECT_EQ(chunk.readNumericAsInt64(0, 0), kA);
  EXPECT_EQ(chunk.readNumericAsInt64(0, 1), kB);
}

// ===========================================================================
// BUG-8: FOR encode offset truncation (offset_bytes default case)
// ===========================================================================

TEST(ChunkTest, FOREncodeLargeRange) {
  std::vector<ColumnDescriptor> cols = {
      makeCol(1, PrimitiveType::kInt64, "wide_for"),
  };
  TopicChunkBuilder builder(/*topic_id=*/704, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  // Range fits in uint32 (< 2^32) but needs 4-byte offsets
  constexpr int64_t kMin = 0;
  constexpr int64_t kMax = int64_t{1} << 31;  // 2^31, range fits in uint32

  builder.beginRow(0);
  builder.set(0, kMin);
  builder.finishRow();
  builder.beginRow(1);
  builder.set(0, kMax);
  builder.finishRow();
  builder.beginRow(2);
  builder.set(0, kMax / 2);  // mid-range value
  builder.finishRow();

  TopicChunk chunk = builder.seal();

  // Should be FOR encoded with 4-byte offsets (range 2^31 < 2^32 and < 8 byte native)
  EXPECT_EQ(chunk.columnEncoding(0), EncodingType::kFrameOfReference);

  EXPECT_EQ(chunk.readNumericAsInt64(0, 0), kMin);
  EXPECT_EQ(chunk.readNumericAsInt64(0, 1), kMax);
  EXPECT_EQ(chunk.readNumericAsInt64(0, 2), kMax / 2);
}

// ===========================================================================
// Death tests: debug asserts catch misuse
// Active in debug builds (assert) or when PJ_ASSERT_THROWS is defined.
// ===========================================================================

#if !defined(NDEBUG) || defined(PJ_ASSERT_THROWS)

#ifdef PJ_ASSERT_THROWS
#define PJ_EXPECT_ASSERT_FAIL(stmt, msg) EXPECT_THROW(stmt, std::runtime_error)
#else
#define PJ_EXPECT_ASSERT_FAIL(stmt, msg) ASSERT_DEATH(stmt, msg)
#endif

TEST(ChunkDeathTest, SetWithoutBeginRowAsserts) {
  std::vector<ColumnDescriptor> cols = {
      make_col(1, PrimitiveType::kFloat32, "val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/300, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  PJ_EXPECT_ASSERT_FAIL(builder.set(0, 1.0F), "set_float32 called without begin_row");
}

TEST(ChunkDeathTest, FinishRowWithoutBeginRowAsserts) {
  std::vector<ColumnDescriptor> cols = {
      make_col(1, PrimitiveType::kFloat32, "val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/301, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  PJ_EXPECT_ASSERT_FAIL(builder.finishRow(), "finish_row called without begin_row");
}

TEST(ChunkDeathTest, BeginRowWhileRowInProgressAsserts) {
  std::vector<ColumnDescriptor> cols = {
      make_col(1, PrimitiveType::kFloat32, "val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/302, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  builder.beginRow(100);
  PJ_EXPECT_ASSERT_FAIL(builder.beginRow(200), "begin_row called while row already in progress");
}

TEST(ChunkDeathTest, OutOfBoundsColIndexAsserts) {
  std::vector<ColumnDescriptor> cols = {
      make_col(1, PrimitiveType::kFloat32, "val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/303, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  builder.beginRow(100);
  PJ_EXPECT_ASSERT_FAIL(builder.set(5, 1.0F), "col_index out of bounds");
}

TEST(ChunkDeathTest, OutOfOrderTimestampAsserts) {
  std::vector<ColumnDescriptor> cols = {
      make_col(1, PrimitiveType::kFloat32, "val"),
  };
  TopicChunkBuilder builder(/*topic_id=*/304, /*schema_id=*/1, std::move(cols), /*max_rows=*/100);

  builder.beginRow(200);
  builder.set(0, 1.0F);
  builder.finishRow();

  PJ_EXPECT_ASSERT_FAIL(builder.beginRow(100), "timestamps must be monotonically non-decreasing");
}

#endif  // !defined(NDEBUG) || defined(PJ_ASSERT_THROWS)

// ===========================================================================
// ColumnStats::all_integral — whole-number detection, independent of encoding
// ===========================================================================

TEST(ChunkTest, AllIntegralFlagTracksWholeNumbersNotEncoding) {
  struct Case {
    const char* name;
    std::vector<double> values;
    bool all_integral;
    EncodingType encoding;
  };
  const std::vector<Case> cases = {
      {"narrow whole", {1.0, 2.0, 3.0}, true, EncodingType::kFrameOfReference},
      {"wide whole", {0.0, 4294967296.0}, true, EncodingType::kRaw},
      {"constant whole", {3.0, 3.0, 3.0}, true, EncodingType::kConstant},
      {"constant fractional", {0.5, 0.5}, false, EncodingType::kConstant},
      {"fractional", {1.0, 1.5}, false, EncodingType::kRaw},
  };
  for (const Case& c : cases) {
    std::vector<ColumnDescriptor> cols = {
        makeCol(1, PrimitiveType::kFloat64, "x"), makeCol(2, PrimitiveType::kInt64, "i")};
    TopicChunkBuilder builder(/*topic_id=*/213, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);
    for (const double v : c.values) {
      builder.beginRow(0);
      builder.set(0, v);
      builder.set(1, int64_t{1});
      builder.finishRow();
    }
    TopicChunk chunk = builder.seal();

    EXPECT_EQ(chunk.stats.column_stats[0].all_integral, c.all_integral) << c.name;
    EXPECT_EQ(chunk.columnEncoding(0), c.encoding) << c.name;
    EXPECT_FALSE(chunk.stats.column_stats[1].all_integral) << "integer columns never carry the flag";
  }
}

TEST(ChunkTest, AllIntegralIgnoresNullSlots) {
  std::vector<ColumnDescriptor> cols = {makeCol(1, PrimitiveType::kFloat64, "x")};
  TopicChunkBuilder builder(/*topic_id=*/214, /*schema_id=*/1, std::move(cols), /*max_rows=*/1000);
  // Bulk append with a NaN payload behind a null: the null slot must not count.
  const double values[] = {2.0, std::nan(""), 4.0};
  const Timestamp ts[] = {0, 1, 2};
  builder.appendTimestamps(Span<const Timestamp>(ts, 3));
  builder.appendColumn(0, Span<const double>(values, 3));
  const uint8_t bitmap[] = {0x05};  // bits [1, 0, 1]
  builder.appendColumnValidity(0, BitSpan{Span<const uint8_t>(bitmap, 1), 0, 3});
  builder.finishBulkAppend();
  TopicChunk chunk = builder.seal();

  EXPECT_TRUE(chunk.stats.column_stats[0].all_integral);
  ASSERT_EQ(chunk.columnEncoding(0), EncodingType::kFrameOfReference);
  EXPECT_TRUE(chunk.isNull(0, 1));
  EXPECT_EQ(chunk.readNumericAsDouble(0, 0), 2.0);
  EXPECT_EQ(chunk.readNumericAsDouble(0, 2), 4.0);
}

}  // namespace
}  // namespace PJ
