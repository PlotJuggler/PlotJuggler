// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow.hpp"
#include "nanoarrow/nanoarrow_ipc.h"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"

namespace PJ {
namespace {

using namespace PJ::sdk;

std::vector<uint8_t> serializeToIpc(ArrowSchema* schema, ArrowArray* array) {
  ArrowBuffer out_buf;
  ArrowBufferInit(&out_buf);

  ArrowIpcOutputStream out_stream;
  EXPECT_EQ(ArrowIpcOutputStreamInitBuffer(&out_stream, &out_buf), NANOARROW_OK);

  ArrowIpcWriter writer;
  EXPECT_EQ(ArrowIpcWriterInit(&writer, &out_stream), NANOARROW_OK);

  ArrowError error;
  EXPECT_EQ(ArrowIpcWriterWriteSchema(&writer, schema, &error), NANOARROW_OK) << error.message;

  nanoarrow::UniqueArrayView view;
  EXPECT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema, nullptr), NANOARROW_OK);
  EXPECT_EQ(ArrowArrayViewSetArray(view.get(), array, nullptr), NANOARROW_OK);
  EXPECT_EQ(ArrowIpcWriterWriteArrayView(&writer, view.get(), &error), NANOARROW_OK) << error.message;
  EXPECT_EQ(ArrowIpcWriterWriteArrayView(&writer, nullptr, &error), NANOARROW_OK);

  ArrowIpcWriterReset(&writer);

  std::vector<uint8_t> result(static_cast<std::size_t>(out_buf.size_bytes));
  std::memcpy(result.data(), out_buf.data, result.size());
  ArrowBufferReset(&out_buf);
  return result;
}

struct Fixture {
  DataEngine engine;
  ObjectStore object_store;
  DatastoreToolboxHost toolbox_impl{engine, object_store};
  ToolboxHostView toolbox{toolbox_impl.raw()};
};

TEST(PluginDataHostWriteTest, SourceHostWritesWithinBoundDataSource) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("imu");
  const auto field = *writer.ensureField(topic, "ax", PrimitiveType::kFloat32);
  EXPECT_EQ(field.topic.id, topic.id);

  const std::vector<NamedFieldValue> fields = {{.name = "ax", .value = 1.25F}};
  ASSERT_TRUE(writer.appendRecord(topic, 10, fields).has_value());
  source_impl.flushPending();

  auto series_or = f.toolbox.readSeries(field);
  ASSERT_TRUE(series_or.has_value());
  auto series = std::move(series_or.value());
  ASSERT_EQ(series.type(), PrimitiveType::kFloat32);
  ASSERT_EQ(series.timestamps().size(), 1U);
  EXPECT_EQ(series.timestamps()[0], 10);
  EXPECT_FLOAT_EQ(series.raw().values.as_float32[0], 1.25F);
}

TEST(PluginDataHostWriteTest, AppendRecordRejectsTypeMismatch) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("imu");
  ASSERT_TRUE(writer.ensureField(topic, "ax", PrimitiveType::kInt32).has_value());

  const std::vector<NamedFieldValue> fields = {{.name = "ax", .value = int16_t{7}}};
  const auto status = writer.appendRecord(topic, 1, fields);
  EXPECT_FALSE(status.has_value());
}

TEST(PluginDataHostWriteTest, AppendRecordFastRejectsUnknownFieldHandle) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("imu");
  ASSERT_TRUE(writer.ensureField(topic, "ax", PrimitiveType::kFloat64).has_value());

  const FieldHandle bad_field{.topic = topic, .id = 999};
  const std::vector<BoundFieldValue> fields = {{.field = bad_field, .value = 1.0}};
  const auto status = writer.appendBoundRecord(topic, 1, fields);
  EXPECT_FALSE(status.has_value());
}

TEST(PluginDataHostWriteTest, ParserHostIsBoundToSingleTopic) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");
  const auto topic = *f.toolbox.ensureTopic(source, "packets");
  const auto other_topic = *f.toolbox.ensureTopic(source, "other");
  const auto foreign_field = *f.toolbox.ensureField(other_topic, "count", PrimitiveType::kInt32);

  DatastoreParserWriteHost parser_impl(f.engine, topic);
  ParserWriteHostView parser(parser_impl.raw());

  const auto count_field = *parser.ensureField("count", PrimitiveType::kInt32);
  const std::vector<NamedFieldValue> good_fields = {{.name = "count", .value = int32_t{42}}};
  ASSERT_TRUE(parser.appendRecord(100, good_fields).has_value());

  const std::vector<BoundFieldValue> bad_fields = {{.field = foreign_field, .value = int32_t{9}}};
  const auto status = parser.appendBoundRecord(101, bad_fields);
  EXPECT_FALSE(status.has_value());

  parser_impl.flushPending();
  auto series_or = f.toolbox.readSeries(count_field);
  ASSERT_TRUE(series_or.has_value());
  auto series = std::move(series_or.value());
  ASSERT_EQ(series.timestamps().size(), 1U);
  EXPECT_EQ(series.raw().values.as_int32[0], 42);
}

TEST(PluginDataHostWriteTest, ToolboxCanWriteIntoExistingDataSource) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");
  const auto topic = *f.toolbox.ensureTopic(source, "labels");
  const auto field = *f.toolbox.ensureField(topic, "name", PrimitiveType::kString);

  const std::vector<NamedFieldValue> fields = {{.name = "name", .value = std::string_view("hello")}};
  ASSERT_TRUE(f.toolbox.appendRecord(topic, 5, fields).has_value());
  f.toolbox_impl.flushPending();

  auto series_or = f.toolbox.readSeries(field);
  ASSERT_TRUE(series_or.has_value());
  auto series = std::move(series_or.value());
  ASSERT_EQ(series.type(), PrimitiveType::kString);
  ASSERT_EQ(series.raw().values.as_string.offset_count, 2U);
  const auto bytes = std::string_view(series.raw().values.as_string.bytes, series.raw().values.as_string.byte_count);
  EXPECT_EQ(bytes, "hello");
}

TEST(PluginDataHostWriteTest, ArrowIpcPreservesExactNarrowPrimitiveTypes) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());
  const auto topic = *writer.ensureTopic("narrow");

  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_STRUCT), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaAllocateChildren(schema.get(), 4), NANOARROW_OK);
  ArrowSchemaInit(schema->children[0]);
  ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "_timestamp"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ArrowSchemaInit(schema->children[1]);
  ASSERT_EQ(ArrowSchemaSetName(schema->children[1], "i8"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_INT8), NANOARROW_OK);
  ArrowSchemaInit(schema->children[2]);
  ASSERT_EQ(ArrowSchemaSetName(schema->children[2], "u16"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[2], NANOARROW_TYPE_UINT16), NANOARROW_OK);
  ArrowSchemaInit(schema->children[3]);
  ASSERT_EQ(ArrowSchemaSetName(schema->children[3], "u32"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[3], NANOARROW_TYPE_UINT32), NANOARROW_OK);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayStartAppending(array.get()), NANOARROW_OK);

  for (int64_t i = 0; i < 3; ++i) {
    ASSERT_EQ(ArrowArrayAppendInt(array->children[0], 100 + i), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayAppendInt(array->children[1], -5 + i), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayAppendUInt(array->children[2], 1000 + static_cast<uint64_t>(i)), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayAppendUInt(array->children[3], 70000 + static_cast<uint64_t>(i)), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayFinishElement(array.get()), NANOARROW_OK);
  }
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), NANOARROW_OK);

  const auto ipc = serializeToIpc(schema.get(), array.get());
  ASSERT_TRUE(writer.appendArrowIpc(topic, ipc).has_value());
  source_impl.flushPending();

  auto snapshot_or = f.toolbox.catalogSnapshot();
  ASSERT_TRUE(snapshot_or.has_value());
  auto snapshot = std::move(snapshot_or.value());
  PrimitiveType i8_type = PrimitiveType::kFloat64;
  PrimitiveType u16_type = PrimitiveType::kFloat64;
  PrimitiveType u32_type = PrimitiveType::kFloat64;
  for (const auto& field : snapshot.fields()) {
    const auto name = toStringView(field.name);
    if (name == "i8") {
      i8_type = fromAbiType(field.type);
    } else if (name == "u16") {
      u16_type = fromAbiType(field.type);
    } else if (name == "u32") {
      u32_type = fromAbiType(field.type);
    }
  }

  EXPECT_EQ(i8_type, PrimitiveType::kInt8);
  EXPECT_EQ(u16_type, PrimitiveType::kUint16);
  EXPECT_EQ(u32_type, PrimitiveType::kUint32);
}

TEST(PluginDataHostWriteTest, CreateDataSourceReturnsDistinctHandles) {
  Fixture f;
  const auto source_a = *f.toolbox.createDataSource("alpha");
  const auto source_b = *f.toolbox.createDataSource("beta");
  EXPECT_NE(source_a.id, source_b.id);
}

TEST(PluginDataHostWriteTest, EnsureTopicIsIdempotent) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic1 = *writer.ensureTopic("imu");
  const auto topic2 = *writer.ensureTopic("imu");
  EXPECT_EQ(topic1.id, topic2.id);
}

TEST(PluginDataHostWriteTest, EnsureTopicDifferentNamesYieldDifferentIds) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic_a = *writer.ensureTopic("imu");
  const auto topic_b = *writer.ensureTopic("gps");
  EXPECT_NE(topic_a.id, topic_b.id);
}

TEST(PluginDataHostWriteTest, EnsureFieldIsIdempotentForSameType) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("imu");
  const auto field1 = *writer.ensureField(topic, "ax", PrimitiveType::kFloat64);
  const auto field2 = *writer.ensureField(topic, "ax", PrimitiveType::kFloat64);
  EXPECT_EQ(field1.id, field2.id);
  EXPECT_EQ(field1.topic.id, field2.topic.id);
}

TEST(PluginDataHostWriteTest, EnsureFieldRejectsTypeMismatchOnExistingField) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("imu");
  ASSERT_TRUE(writer.ensureField(topic, "x", PrimitiveType::kFloat64).has_value());
  const auto result = writer.ensureField(topic, "x", PrimitiveType::kInt32);
  EXPECT_FALSE(result.has_value());
}

TEST(PluginDataHostWriteTest, AppendRecordRejectsDuplicateFieldNames) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("imu");
  ASSERT_TRUE(writer.ensureField(topic, "x", PrimitiveType::kInt32).has_value());

  const std::vector<NamedFieldValue> fields = {
      {.name = "x", .value = int32_t{1}},
      {.name = "x", .value = int32_t{2}},
  };
  const auto status = writer.appendRecord(topic, 1, fields);
  EXPECT_FALSE(status.has_value());
}

TEST(PluginDataHostWriteTest, AppendRecordRejectsInvalidTopicHandle) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const TopicHandle bad_topic{.id = 999};
  const std::vector<NamedFieldValue> fields = {{.name = "x", .value = int32_t{1}}};
  const auto status = writer.appendRecord(bad_topic, 1, fields);
  EXPECT_FALSE(status.has_value());
}

TEST(PluginDataHostWriteTest, AppendRecordSparseFieldsProduceNullsForMissingColumns) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("imu");
  const auto field_a = *writer.ensureField(topic, "a", PrimitiveType::kFloat64);
  const auto field_b = *writer.ensureField(topic, "b", PrimitiveType::kFloat64);

  // Append only field "a", leaving "b" missing.
  const std::vector<NamedFieldValue> fields = {{.name = "a", .value = double{3.14}}};
  ASSERT_TRUE(writer.appendRecord(topic, 100, fields).has_value());
  source_impl.flushPending();

  auto series_b = std::move(*f.toolbox.readSeries(field_b));
  ASSERT_EQ(series_b.timestamps().size(), 1U);
  ASSERT_GE(series_b.validityBits().size(), 1U);
  EXPECT_EQ(series_b.validityBits()[0] & 0x01, 0) << "Expected null for missing field b";

  auto series_a = std::move(*f.toolbox.readSeries(field_a));
  ASSERT_EQ(series_a.timestamps().size(), 1U);
  EXPECT_DOUBLE_EQ(series_a.raw().values.as_float64[0], 3.14);
}

TEST(PluginDataHostWriteTest, AppendRecordTimestampOnlyRow) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("heartbeat");
  const std::vector<NamedFieldValue> fields = {};
  const auto status = writer.appendRecord(topic, 42, fields);
  // Just verify it does not crash. Accept either success or error.
  (void)status;
}

TEST(PluginDataHostWriteTest, AppendRecordFastHappyPath) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("imu");
  const auto field = *writer.ensureField(topic, "ax", PrimitiveType::kFloat64);

  const std::vector<BoundFieldValue> fields = {{.field = field, .value = double{2.5}}};
  ASSERT_TRUE(writer.appendBoundRecord(topic, 10, fields).has_value());
  source_impl.flushPending();

  auto series = std::move(*f.toolbox.readSeries(field));
  ASSERT_EQ(series.timestamps().size(), 1U);
  EXPECT_EQ(series.timestamps()[0], 10);
  EXPECT_DOUBLE_EQ(series.raw().values.as_float64[0], 2.5);
}

TEST(PluginDataHostWriteTest, AppendRecordFastRejectsWrongTopic) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic_a = *writer.ensureTopic("imu");
  const auto topic_b = *writer.ensureTopic("gps");
  const auto field_a = *writer.ensureField(topic_a, "ax", PrimitiveType::kFloat64);

  // Use field from topic_a with topic_b.
  const std::vector<BoundFieldValue> fields = {{.field = field_a, .value = double{1.0}}};
  const auto status = writer.appendBoundRecord(topic_b, 1, fields);
  EXPECT_FALSE(status.has_value());
}

TEST(PluginDataHostWriteTest, AppendRecordFastRejectsDuplicateFieldIds) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("imu");
  const auto field = *writer.ensureField(topic, "ax", PrimitiveType::kFloat64);

  const std::vector<BoundFieldValue> fields = {
      {.field = field, .value = double{1.0}},
      {.field = field, .value = double{2.0}},
  };
  const auto status = writer.appendBoundRecord(topic, 1, fields);
  EXPECT_FALSE(status.has_value());
}

TEST(PluginDataHostWriteTest, AppendRecordFastRejectsValueTypeMismatch) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("imu");
  const auto field = *writer.ensureField(topic, "ax", PrimitiveType::kFloat64);

  // Pass int32_t value for a kFloat64 field.
  const std::vector<BoundFieldValue> fields = {{.field = field, .value = int32_t{42}}};
  const auto status = writer.appendBoundRecord(topic, 1, fields);
  EXPECT_FALSE(status.has_value());
}

TEST(PluginDataHostWriteTest, AppendRecordNullValueViaIsNull) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("imu");
  const auto field = *writer.ensureField(topic, "ax", PrimitiveType::kFloat64);

  const std::vector<NamedFieldValue> fields = {{.name = "ax", .value = PJ::kNull}};
  ASSERT_TRUE(writer.appendRecord(topic, 10, fields).has_value());
  source_impl.flushPending();

  auto series = std::move(*f.toolbox.readSeries(field));
  ASSERT_EQ(series.timestamps().size(), 1U);
  ASSERT_GE(series.validityBits().size(), 1U);
  EXPECT_EQ(series.validityBits()[0] & 0x01, 0) << "Expected null for NullValue field";
}

TEST(PluginDataHostWriteTest, AppendRecordFastNullValue) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());

  const auto topic = *writer.ensureTopic("imu");
  const auto field = *writer.ensureField(topic, "ax", PrimitiveType::kFloat64);

  const std::vector<BoundFieldValue> fields = {{.field = field, .value = PJ::kNull}};
  ASSERT_TRUE(writer.appendBoundRecord(topic, 10, fields).has_value());
  source_impl.flushPending();

  auto series = std::move(*f.toolbox.readSeries(field));
  ASSERT_EQ(series.timestamps().size(), 1U);
  ASSERT_GE(series.validityBits().size(), 1U);
  EXPECT_EQ(series.validityBits()[0] & 0x01, 0) << "Expected null for NullValue field via fast path";
}

TEST(PluginDataHostWriteTest, ArrowIpcFloat64RoundTrip) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());
  const auto topic = *writer.ensureTopic("data");

  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_STRUCT), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaAllocateChildren(schema.get(), 2), NANOARROW_OK);
  ArrowSchemaInit(schema->children[0]);
  ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "_timestamp"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ArrowSchemaInit(schema->children[1]);
  ASSERT_EQ(ArrowSchemaSetName(schema->children[1], "value"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayStartAppending(array.get()), NANOARROW_OK);
  const int64_t timestamps[] = {100, 200, 300};
  const double values[] = {1.5, 2.5, 3.5};
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(ArrowArrayAppendInt(array->children[0], timestamps[i]), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayAppendDouble(array->children[1], values[i]), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayFinishElement(array.get()), NANOARROW_OK);
  }
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), NANOARROW_OK);

  const auto ipc = serializeToIpc(schema.get(), array.get());
  ASSERT_TRUE(writer.appendArrowIpc(topic, Span<const uint8_t>(ipc.data(), ipc.size())).has_value());
  source_impl.flushPending();

  auto snapshot = std::move(*f.toolbox.catalogSnapshot());
  FieldHandle value_field{};
  bool found = false;
  for (const auto& fi : snapshot.fields()) {
    if (toStringView(fi.name) == "value") {
      value_field = fi.handle;
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found) << "Field 'value' not found in catalog";

  auto series = std::move(*f.toolbox.readSeries(value_field));
  ASSERT_EQ(series.type(), PrimitiveType::kFloat64);
  ASSERT_EQ(series.timestamps().size(), 3U);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(series.timestamps()[static_cast<size_t>(i)], timestamps[i]);
    EXPECT_DOUBLE_EQ(series.raw().values.as_float64[i], values[i]);
  }
}

TEST(PluginDataHostWriteTest, ArrowIpcCustomTimestampColumnName) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());
  const auto topic = *writer.ensureTopic("custom_ts");

  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_STRUCT), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaAllocateChildren(schema.get(), 2), NANOARROW_OK);
  ArrowSchemaInit(schema->children[0]);
  ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "ts"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ArrowSchemaInit(schema->children[1]);
  ASSERT_EQ(ArrowSchemaSetName(schema->children[1], "val"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_FLOAT), NANOARROW_OK);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayStartAppending(array.get()), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayAppendInt(array->children[0], 10), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayAppendDouble(array->children[1], 1.0), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayFinishElement(array.get()), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayAppendInt(array->children[0], 20), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayAppendDouble(array->children[1], 2.0), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayFinishElement(array.get()), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), NANOARROW_OK);

  const auto ipc = serializeToIpc(schema.get(), array.get());
  ASSERT_TRUE(writer.appendArrowIpc(topic, Span<const uint8_t>(ipc.data(), ipc.size()), "ts").has_value());
  source_impl.flushPending();

  auto snapshot = std::move(*f.toolbox.catalogSnapshot());
  FieldHandle val_field{};
  bool found = false;
  for (const auto& fi : snapshot.fields()) {
    if (toStringView(fi.name) == "val") {
      val_field = fi.handle;
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found) << "Field 'val' not found in catalog";

  auto series = std::move(*f.toolbox.readSeries(val_field));
  ASSERT_EQ(series.type(), PrimitiveType::kFloat32);
  ASSERT_EQ(series.timestamps().size(), 2U);
  EXPECT_EQ(series.timestamps()[0], 10);
  EXPECT_EQ(series.timestamps()[1], 20);
  EXPECT_FLOAT_EQ(series.raw().values.as_float32[0], 1.0F);
  EXPECT_FLOAT_EQ(series.raw().values.as_float32[1], 2.0F);
}

TEST(PluginDataHostWriteTest, ArrowIpcRejectsMissingTimestampColumn) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());
  const auto topic = *writer.ensureTopic("no_ts");

  // Build a struct schema with only a data column (no "_timestamp").
  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_STRUCT), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaAllocateChildren(schema.get(), 1), NANOARROW_OK);
  ArrowSchemaInit(schema->children[0]);
  ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "data"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32), NANOARROW_OK);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayStartAppending(array.get()), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayAppendInt(array->children[0], 42), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayFinishElement(array.get()), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), NANOARROW_OK);

  const auto ipc = serializeToIpc(schema.get(), array.get());
  const auto status = writer.appendArrowIpc(topic, Span<const uint8_t>(ipc.data(), ipc.size()));
  EXPECT_FALSE(status.has_value()) << "Expected error when timestamp column is missing";
}

TEST(PluginDataHostWriteTest, LateColumnAddition) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());
  const auto topic = *writer.ensureTopic("json");

  const std::vector<NamedFieldValue> row1 = {{.name = "x", .value = 1.0}};
  ASSERT_TRUE(writer.appendRecord(topic, 10, row1).has_value());

  // Row 2: "x" plus new field "y" — triggers auto-seal of chunk 1
  const std::vector<NamedFieldValue> row2 = {
      {.name = "x", .value = 2.0},
      {.name = "y", .value = 3.0},
  };
  ASSERT_TRUE(writer.appendRecord(topic, 20, row2).has_value());
  source_impl.flushPending();

  auto snapshot = std::move(*f.toolbox.catalogSnapshot());
  FieldHandle x_field{}, y_field{};
  for (const auto& fi : snapshot.fields()) {
    auto name = std::string_view(fi.name.data, fi.name.size);
    if (name == "x") {
      x_field = fi.handle;
    }
    if (name == "y") {
      y_field = fi.handle;
    }
  }

  auto x_series = std::move(*f.toolbox.readSeries(x_field));
  ASSERT_EQ(x_series.timestamps().size(), 2U);
  EXPECT_DOUBLE_EQ(x_series.raw().values.as_float64[0], 1.0);
  EXPECT_DOUBLE_EQ(x_series.raw().values.as_float64[1], 2.0);

  auto y_series = std::move(*f.toolbox.readSeries(y_field));
  ASSERT_EQ(y_series.timestamps().size(), 1U);
  EXPECT_EQ(y_series.timestamps()[0], 20);
  EXPECT_DOUBLE_EQ(y_series.raw().values.as_float64[0], 3.0);
}

TEST(PluginDataHostWriteTest, UntypedNullForUnknownFieldIsSkipped) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());
  const auto topic = *writer.ensureTopic("sparse");

  // Row 1: "x" is non-null, "y" is untyped null (kNull) and never seen → skipped
  const std::vector<NamedFieldValue> row = {
      {.name = "x", .value = 1.0},
      {.name = "y", .value = PJ::kNull},
  };
  ASSERT_TRUE(writer.appendRecord(topic, 10, row).has_value());
  source_impl.flushPending();

  auto snapshot = std::move(*f.toolbox.catalogSnapshot());
  int field_count = 0;
  for (const auto& fi : snapshot.fields()) {
    auto name = std::string_view(fi.name.data, fi.name.size);
    EXPECT_EQ(name, "x") << "unexpected field: " << name;
    ++field_count;
  }
  EXPECT_EQ(field_count, 1);
}

TEST(PluginDataHostWriteTest, TypedNullForUnknownFieldCreatesColumn) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());
  const auto topic = *writer.ensureTopic("typed");

  // Row 1: "x" non-null, "y" is a typed null (type known but value absent)
  const std::vector<NamedFieldValue> row = {
      {.name = "x", .value = 1.0},
      {.name = "y", .value = TypedNull{PrimitiveType::kFloat64}},
  };
  ASSERT_TRUE(writer.appendRecord(topic, 10, row).has_value());
  source_impl.flushPending();

  auto snapshot = std::move(*f.toolbox.catalogSnapshot());
  FieldHandle x_field{}, y_field{};
  int field_count = 0;
  for (const auto& fi : snapshot.fields()) {
    auto name = std::string_view(fi.name.data, fi.name.size);
    if (name == "x") {
      x_field = fi.handle;
    }
    if (name == "y") {
      y_field = fi.handle;
    }
    ++field_count;
  }
  EXPECT_EQ(field_count, 2);

  auto y_series = std::move(*f.toolbox.readSeries(y_field));
  ASSERT_EQ(y_series.timestamps().size(), 1U);
  ASSERT_GE(y_series.validityBits().size(), 1U);
  EXPECT_EQ(y_series.validityBits()[0] & 0x01, 0) << "Expected null for TypedNull field";
}

TEST(PluginDataHostWriteTest, VariableLengthArraySimulation) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost source_impl(f.engine, source);
  SourceWriteHostView writer(source_impl.raw());
  const auto topic = *writer.ensureTopic("varlen");

  const std::vector<NamedFieldValue> row1 = {
      {.name = "data[0]", .value = 10.0},
      {.name = "data[1]", .value = 20.0},
  };
  ASSERT_TRUE(writer.appendRecord(topic, 100, row1).has_value());

  // Row 2: 4-element array — data[2] and data[3] are new columns
  const std::vector<NamedFieldValue> row2 = {
      {.name = "data[0]", .value = 11.0},
      {.name = "data[1]", .value = 21.0},
      {.name = "data[2]", .value = 31.0},
      {.name = "data[3]", .value = 41.0},
  };
  ASSERT_TRUE(writer.appendRecord(topic, 200, row2).has_value());
  source_impl.flushPending();

  auto snapshot = std::move(*f.toolbox.catalogSnapshot());
  FieldHandle d0{}, d2{};
  for (const auto& fi : snapshot.fields()) {
    auto name = std::string_view(fi.name.data, fi.name.size);
    if (name == "data[0]") {
      d0 = fi.handle;
    }
    if (name == "data[2]") {
      d2 = fi.handle;
    }
  }

  auto s0 = std::move(*f.toolbox.readSeries(d0));
  ASSERT_EQ(s0.timestamps().size(), 2U);
  EXPECT_DOUBLE_EQ(s0.raw().values.as_float64[0], 10.0);
  EXPECT_DOUBLE_EQ(s0.raw().values.as_float64[1], 11.0);

  auto s2 = std::move(*f.toolbox.readSeries(d2));
  ASSERT_EQ(s2.timestamps().size(), 1U);
  EXPECT_EQ(s2.timestamps()[0], 200);
  EXPECT_DOUBLE_EQ(s2.raw().values.as_float64[0], 31.0);
}

}  // namespace
}  // namespace PJ
