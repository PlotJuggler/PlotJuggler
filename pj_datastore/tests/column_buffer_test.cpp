// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/column_buffer.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string_view>

namespace PJ {
namespace {

ColumnDescriptor makeDescriptor(PrimitiveType type, std::string path = "test_field") {
  return ColumnDescriptor{.field_id = 0, .logical_type = type, .field_path = std::move(path)};
}

TEST(TypedColumnBufferTest, Float32AppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kFloat32));
  buf.appendFloat32(1.5f);
  buf.appendFloat32(-3.14f);
  buf.appendFloat32(0.0f);

  EXPECT_EQ(buf.rowCount(), 3u);
  EXPECT_FLOAT_EQ(buf.readFloat32(0), 1.5f);
  EXPECT_FLOAT_EQ(buf.readFloat32(1), -3.14f);
  EXPECT_FLOAT_EQ(buf.readFloat32(2), 0.0f);
}

TEST(TypedColumnBufferTest, Float64AppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kFloat64));
  buf.appendFloat64(2.718281828459045);
  buf.appendFloat64(-1e308);

  EXPECT_EQ(buf.rowCount(), 2u);
  EXPECT_DOUBLE_EQ(buf.readFloat64(0), 2.718281828459045);
  EXPECT_DOUBLE_EQ(buf.readFloat64(1), -1e308);
}

TEST(TypedColumnBufferTest, Int32AppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kInt32));
  buf.appendInt32(42);
  buf.appendInt32(-100);
  buf.appendInt32(0);

  EXPECT_EQ(buf.rowCount(), 3u);
  EXPECT_EQ(buf.readInt32(0), 42);
  EXPECT_EQ(buf.readInt32(1), -100);
  EXPECT_EQ(buf.readInt32(2), 0);
}

TEST(TypedColumnBufferTest, Int64AppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kInt64));
  buf.appendInt64(INT64_MAX);
  buf.appendInt64(INT64_MIN);
  buf.appendInt64(0);

  EXPECT_EQ(buf.rowCount(), 3u);
  EXPECT_EQ(buf.readInt64(0), INT64_MAX);
  EXPECT_EQ(buf.readInt64(1), INT64_MIN);
  EXPECT_EQ(buf.readInt64(2), 0);
}

TEST(TypedColumnBufferTest, Uint64AppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kUint64));
  buf.appendUint64(255);
  buf.appendUint64(0);
  buf.appendUint64(18000000000000000000ULL);

  EXPECT_EQ(buf.rowCount(), 3u);
  EXPECT_EQ(buf.readUint64(0), 255U);
  EXPECT_EQ(buf.readUint64(1), 0U);
  EXPECT_EQ(buf.readUint64(2), 18000000000000000000ULL);
}

TEST(TypedColumnBufferTest, BoolAppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kBool));
  buf.appendBool(true);
  buf.appendBool(false);
  buf.appendBool(true);

  EXPECT_EQ(buf.rowCount(), 3u);
  EXPECT_TRUE(buf.readBool(0));
  EXPECT_FALSE(buf.readBool(1));
  EXPECT_TRUE(buf.readBool(2));
}

TEST(TypedColumnBufferTest, StringAppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kString));
  buf.appendString("hello");
  buf.appendString("world");
  buf.appendString("test");

  EXPECT_EQ(buf.rowCount(), 3u);
  EXPECT_EQ(buf.readString(0), "hello");
  EXPECT_EQ(buf.readString(1), "world");
  EXPECT_EQ(buf.readString(2), "test");
}

TEST(TypedColumnBufferTest, NullHandling) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kFloat32));
  buf.appendFloat32(1.0f);
  buf.appendNull();
  buf.appendFloat32(3.0f);

  EXPECT_EQ(buf.rowCount(), 3u);
  EXPECT_FALSE(buf.isNull(0));
  EXPECT_TRUE(buf.isNull(1));
  EXPECT_FALSE(buf.isNull(2));

  EXPECT_FLOAT_EQ(buf.readFloat32(0), 1.0f);
  EXPECT_FLOAT_EQ(buf.readFloat32(2), 3.0f);
}

TEST(TypedColumnBufferTest, HasNulls) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kInt32));
  EXPECT_FALSE(buf.hasNulls());

  buf.appendInt32(10);
  EXPECT_FALSE(buf.hasNulls());

  buf.appendNull();
  EXPECT_TRUE(buf.hasNulls());
}

TEST(TypedColumnBufferTest, RowCountIncrements) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kFloat64));
  EXPECT_EQ(buf.rowCount(), 0u);

  buf.appendFloat64(1.0);
  EXPECT_EQ(buf.rowCount(), 1u);

  buf.appendFloat64(2.0);
  EXPECT_EQ(buf.rowCount(), 2u);

  buf.appendNull();
  EXPECT_EQ(buf.rowCount(), 3u);

  buf.appendFloat64(4.0);
  EXPECT_EQ(buf.rowCount(), 4u);
}

TEST(TypedColumnBufferTest, ReadAsDoubleFloat32) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kFloat32));
  buf.appendFloat32(1.5f);

  EXPECT_DOUBLE_EQ(buf.readAsDouble(0), 1.5);
}

TEST(TypedColumnBufferTest, ReadAsDoubleInt32) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kInt32));
  buf.appendInt32(42);

  EXPECT_DOUBLE_EQ(buf.readAsDouble(0), 42.0);
}

TEST(TypedColumnBufferTest, MultipleStringsVaryingLengths) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kString));
  buf.appendString("a");
  buf.appendString("bb");
  buf.appendString("ccc");
  buf.appendString("dddd");
  buf.appendString("eeeee");

  EXPECT_EQ(buf.rowCount(), 5u);
  EXPECT_EQ(buf.readString(0), "a");
  EXPECT_EQ(buf.readString(1), "bb");
  EXPECT_EQ(buf.readString(2), "ccc");
  EXPECT_EQ(buf.readString(3), "dddd");
  EXPECT_EQ(buf.readString(4), "eeeee");
}

TEST(TypedColumnBufferTest, EmptyString) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kString));
  buf.appendString("");

  EXPECT_EQ(buf.rowCount(), 1u);
  EXPECT_EQ(buf.readString(0), "");
  EXPECT_TRUE(buf.readString(0).empty());
}

TEST(TypedColumnBufferTest, DescriptorAccessor) {
  auto desc = makeDescriptor(PrimitiveType::kFloat64, "position.x");
  TypedColumnBuffer buf(desc);

  EXPECT_EQ(buf.descriptor().logical_type, PrimitiveType::kFloat64);
  EXPECT_EQ(buf.descriptor().field_path, "position.x");
}

TEST(TypedColumnBufferTest, ReadAsDoubleBool) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kBool));
  buf.appendBool(true);
  buf.appendBool(false);

  EXPECT_DOUBLE_EQ(buf.readAsDouble(0), 1.0);
  EXPECT_DOUBLE_EQ(buf.readAsDouble(1), 0.0);
}

TEST(TypedColumnBufferTest, ReadAsDoubleStringReturnsNaN) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kString));
  buf.appendString("hello");

  EXPECT_TRUE(std::isnan(buf.readAsDouble(0)));
}

TEST(TypedColumnBufferTest, NullInStringColumn) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kString));
  buf.appendString("hello");
  buf.appendNull();
  buf.appendString("world");

  EXPECT_EQ(buf.rowCount(), 3u);
  EXPECT_FALSE(buf.isNull(0));
  EXPECT_TRUE(buf.isNull(1));
  EXPECT_FALSE(buf.isNull(2));
  EXPECT_EQ(buf.readString(0), "hello");
  EXPECT_EQ(buf.readString(1), "");  // null string reads as empty
  EXPECT_EQ(buf.readString(2), "world");
}

TEST(TypedColumnBufferTest, ValueBufferNonEmpty) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kInt32));
  EXPECT_TRUE(buf.valueBuffer().empty());

  buf.appendInt32(10);
  EXPECT_FALSE(buf.valueBuffer().empty());
  EXPECT_EQ(buf.valueBuffer().size(), sizeof(int32_t));
}

TEST(TypedColumnBufferTest, OffsetsBufferForStrings) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kString));
  EXPECT_TRUE(buf.offsetsBuffer().empty());

  buf.appendString("hi");
  // offsets: [0, 2] => 2 * sizeof(uint32_t) = 8
  EXPECT_EQ(buf.offsetsBuffer().size(), 2 * sizeof(uint32_t));
}

TEST(TypedColumnBufferTest, BulkFloat32AppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kFloat32));
  const float data[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  buf.appendFloat32Bulk(Span<const float>(data, 5));

  EXPECT_EQ(buf.rowCount(), 5u);
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_FLOAT_EQ(buf.readFloat32(i), data[i]);
  }
}

TEST(TypedColumnBufferTest, BulkFloat64AppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kFloat64));
  const double data[] = {1.1, 2.2, 3.3};
  buf.appendFloat64Bulk(Span<const double>(data, 3));

  EXPECT_EQ(buf.rowCount(), 3u);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_DOUBLE_EQ(buf.readFloat64(i), data[i]);
  }
}

TEST(TypedColumnBufferTest, BulkInt32AppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kInt32));
  const int32_t data[] = {-100, 0, 42, 999};
  buf.appendInt32Bulk(Span<const int32_t>(data, 4));

  EXPECT_EQ(buf.rowCount(), 4u);
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(buf.readInt32(i), data[i]);
  }
}

TEST(TypedColumnBufferTest, BulkInt64AppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kInt64));
  const int64_t data[] = {INT64_MIN, 0, INT64_MAX};
  buf.appendInt64Bulk(Span<const int64_t>(data, 3));

  EXPECT_EQ(buf.rowCount(), 3u);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(buf.readInt64(i), data[i]);
  }
}

TEST(TypedColumnBufferTest, BulkUint64AppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kUint64));
  const uint64_t data[] = {0, 255, 18000000000000000000ULL};
  buf.appendUint64Bulk(Span<const uint64_t>(data, 3));

  EXPECT_EQ(buf.rowCount(), 3u);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(buf.readUint64(i), data[i]);
  }
}

TEST(TypedColumnBufferTest, BulkBoolAppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kBool));
  const uint8_t data[] = {1, 0, 1, 1, 0};
  buf.appendBoolBulk(Span<const uint8_t>(data, 5));

  EXPECT_EQ(buf.rowCount(), 5u);
  EXPECT_TRUE(buf.readBool(0));
  EXPECT_FALSE(buf.readBool(1));
  EXPECT_TRUE(buf.readBool(2));
  EXPECT_TRUE(buf.readBool(3));
  EXPECT_FALSE(buf.readBool(4));
}

TEST(TypedColumnBufferTest, BulkStringAppendRead) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kString));
  // "hello" "world" "!"
  const char string_data[] = "helloworld!";
  const uint32_t offsets[] = {0, 5, 10, 11};
  buf.appendStringsBulk(Span<const uint32_t>(offsets, 4), Span<const char>(string_data, 11));

  EXPECT_EQ(buf.rowCount(), 3u);
  EXPECT_EQ(buf.readString(0), "hello");
  EXPECT_EQ(buf.readString(1), "world");
  EXPECT_EQ(buf.readString(2), "!");
}

TEST(TypedColumnBufferTest, BulkValidityBitmap) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kFloat32));
  const float data[] = {1.0f, 0.0f, 3.0f, 0.0f};
  buf.appendFloat32Bulk(Span<const float>(data, 4));

  // Validity bitmap: bits [1,0,1,0] = 0b0101 = 0x05
  const uint8_t bitmap[] = {0x05};
  buf.appendValidityBulk(BitSpan{Span<const uint8_t>(bitmap, 1), 0, 4});

  EXPECT_FALSE(buf.isNull(0));
  EXPECT_TRUE(buf.isNull(1));
  EXPECT_FALSE(buf.isNull(2));
  EXPECT_TRUE(buf.isNull(3));
  EXPECT_TRUE(buf.hasNulls());
}

TEST(TypedColumnBufferTest, BulkAfterSingleAppend) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kFloat32));
  buf.appendFloat32(1.0f);
  buf.appendFloat32(2.0f);

  const float bulk[] = {3.0f, 4.0f, 5.0f};
  buf.appendFloat32Bulk(Span<const float>(bulk, 3));

  EXPECT_EQ(buf.rowCount(), 5u);
  EXPECT_FLOAT_EQ(buf.readFloat32(0), 1.0f);
  EXPECT_FLOAT_EQ(buf.readFloat32(1), 2.0f);
  EXPECT_FLOAT_EQ(buf.readFloat32(2), 3.0f);
  EXPECT_FLOAT_EQ(buf.readFloat32(3), 4.0f);
  EXPECT_FLOAT_EQ(buf.readFloat32(4), 5.0f);
}

TEST(TypedColumnBufferTest, BulkZeroCount) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kFloat32));
  buf.appendFloat32Bulk(Span<const float>());
  EXPECT_EQ(buf.rowCount(), 0u);
}

TEST(TypedColumnBufferTest, BulkStringsNonZeroBaseOffset) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kString));
  // Simulates Arrow-style offsets that don't start at 0
  const char string_data[] = "XXXXXhelloworld";
  const uint32_t offsets[] = {5, 10, 15};  // 2 strings: "hello", "world"
  buf.appendStringsBulk(Span<const uint32_t>(offsets, 3), Span<const char>(string_data, 15));

  EXPECT_EQ(buf.rowCount(), 2u);
  EXPECT_EQ(buf.readString(0), "hello");
  EXPECT_EQ(buf.readString(1), "world");
}

TEST(TypedColumnBufferTest, BulkValidityWithBitOffset) {
  TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kInt32));
  const int32_t data[] = {10, 20, 30};
  buf.appendInt32Bulk(Span<const int32_t>(data, 3));

  // bitmap = 0b01010000, bit_offset = 4, so bits 4,5,6 = 1,0,1
  // In Arrow's LSB-first layout: byte 0x50 = 0b01010000
  //   bit 4 = (0x50 >> 4) & 1 = 1 (valid)
  //   bit 5 = (0x50 >> 5) & 1 = 0 (null)
  //   bit 6 = (0x50 >> 6) & 1 = 1 (valid)
  const uint8_t bitmap[] = {0x50};
  buf.appendValidityBulk(BitSpan{Span<const uint8_t>(bitmap, 1), 4, 3});

  EXPECT_FALSE(buf.isNull(0));  // bit 4 = 1 -> valid
  EXPECT_TRUE(buf.isNull(1));   // bit 5 = 0 -> null
  EXPECT_FALSE(buf.isNull(2));  // bit 6 = 1 -> valid
}

}  // namespace
}  // namespace PJ
