// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file arrow_stream_round_trip_test.cpp
 * @brief End-to-end round trip through the v4 Arrow C Data Interface path.
 *
 * Writes known small time series into the datastore via
 * DatastoreSourceWriteHost::append_arrow_stream and
 * DatastoreParserWriteHost::append_arrow_stream (the v4 ABI slots), then
 * reads them back via DatastoreToolboxHost::read_series_arrow.
 *
 * This exercises the Phase 1b host-side implementation without going through
 * a dlopen'd plugin — all ABI calls are made directly on the C vtable.
 */
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow.hpp"
#include "pj_base/dataset.hpp"
#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_datastore/topic_storage.hpp"

namespace PJ {
namespace {

// ---------------------------------------------------------------------------
// Build a one-batch ArrowArrayStream with columns {timestamp: int64, value: double}
// ---------------------------------------------------------------------------

struct BuiltStream {
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
};

BuiltStream makeStream(
    const std::vector<int64_t>& timestamps, const std::vector<double>& values,
    ArrowType value_type = NANOARROW_TYPE_DOUBLE) {
  EXPECT_EQ(timestamps.size(), values.size());
  const int64_t n = static_cast<int64_t>(timestamps.size());

  BuiltStream result;
  ArrowSchemaInit(result.schema.get());
  EXPECT_EQ(ArrowSchemaSetTypeStruct(result.schema.get(), 2), NANOARROW_OK);
  ArrowSchemaInit(result.schema->children[0]);
  EXPECT_EQ(ArrowSchemaSetType(result.schema->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  EXPECT_EQ(ArrowSchemaSetName(result.schema->children[0], "ts_col"), NANOARROW_OK);
  ArrowSchemaInit(result.schema->children[1]);
  EXPECT_EQ(ArrowSchemaSetType(result.schema->children[1], value_type), NANOARROW_OK);
  EXPECT_EQ(ArrowSchemaSetName(result.schema->children[1], "value"), NANOARROW_OK);

  ArrowError err;
  EXPECT_EQ(ArrowArrayInitFromSchema(result.array.get(), result.schema.get(), &err), NANOARROW_OK) << err.message;
  EXPECT_EQ(ArrowArrayStartAppending(result.array.get()), NANOARROW_OK);
  for (int64_t i = 0; i < n; ++i) {
    EXPECT_EQ(ArrowArrayAppendInt(result.array->children[0], timestamps[static_cast<std::size_t>(i)]), NANOARROW_OK);
    EXPECT_EQ(ArrowArrayAppendDouble(result.array->children[1], values[static_cast<std::size_t>(i)]), NANOARROW_OK);
    EXPECT_EQ(ArrowArrayFinishElement(result.array.get()), NANOARROW_OK);
  }
  EXPECT_EQ(ArrowArrayFinishBuildingDefault(result.array.get(), &err), NANOARROW_OK) << err.message;
  return result;
}

/// Stream producer that yields one batch then end-of-stream.
struct OneBatchStreamState {
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  bool exhausted = false;
  std::string last_error_buf;
};

int onebatchGetSchema(ArrowArrayStream* stream, ArrowSchema* out) {
  auto* s = static_cast<OneBatchStreamState*>(stream->private_data);
  return ArrowSchemaDeepCopy(s->schema.get(), out);
}

int onebatchGetNext(ArrowArrayStream* stream, ArrowArray* out) {
  auto* s = static_cast<OneBatchStreamState*>(stream->private_data);
  if (s->exhausted) {
    out->release = nullptr;  // sentinel for end-of-stream per Arrow spec
    return NANOARROW_OK;
  }
  ArrowArrayMove(s->array.get(), out);
  s->exhausted = true;
  return NANOARROW_OK;
}

const char* onebatchGetLastError(ArrowArrayStream* stream) {
  auto* s = static_cast<OneBatchStreamState*>(stream->private_data);
  return s->last_error_buf.empty() ? nullptr : s->last_error_buf.c_str();
}

void onebatchRelease(ArrowArrayStream* stream) {
  delete static_cast<OneBatchStreamState*>(stream->private_data);
  stream->private_data = nullptr;
  stream->release = nullptr;
}

void initOneBatchStream(ArrowArrayStream* out_stream, BuiltStream built) {
  auto* state = new OneBatchStreamState{std::move(built.schema), std::move(built.array), false, {}};
  out_stream->get_schema = onebatchGetSchema;
  out_stream->get_next = onebatchGetNext;
  out_stream->get_last_error = onebatchGetLastError;
  out_stream->release = onebatchRelease;
  out_stream->private_data = state;
}

// ---------------------------------------------------------------------------
// Round-trip test
// ---------------------------------------------------------------------------

// Writes `values` (at timestamps 1000, 2000, ...) through append_arrow_stream,
// reads them back through read_series_arrow, and checks the round trip. Reports
// the encoding of the stored value column.
void roundTripThroughArrow(
    const std::vector<double>& values, EncodingType* stored_encoding, ArrowType value_type = NANOARROW_TYPE_DOUBLE) {
  // Set up engine + dataset.
  DataEngine engine;
  auto td_id = engine.createTimeDomain("test_td");
  ASSERT_TRUE(td_id.has_value()) << td_id.error();
  auto ds_id = engine.createDataset(DatasetDescriptor{.source_name = "test", .time_domain_id = *td_id});
  ASSERT_TRUE(ds_id.has_value()) << ds_id.error();

  // Write host bound to that dataset.
  DatastoreSourceWriteHost write_host(engine, PJ_data_source_handle_t{static_cast<uint32_t>(*ds_id)});
  auto write_vtable = write_host.raw();

  // Ensure a topic named "metric" up-front (matches the stream's later schema).
  PJ_topic_handle_t topic{};
  PJ_error_t err{};
  PJ_string_view_t topic_name{"metric", 6};
  ASSERT_TRUE(write_vtable.vtable->ensure_topic(write_vtable.ctx, topic_name, &topic, &err)) << err.message;

  // Build a stream with {timestamp, value} and feed it through append_arrow_stream.
  std::vector<int64_t> timestamps;
  for (std::size_t i = 0; i < values.size(); ++i) {
    timestamps.push_back(static_cast<int64_t>(i + 1) * 1000);
  }
  auto built = makeStream(timestamps, values, value_type);

  ArrowArrayStream stream{};
  initOneBatchStream(&stream, std::move(built));

  PJ_string_view_t ts_col_name{"ts_col", 6};
  ASSERT_TRUE(write_vtable.vtable->append_arrow_stream(write_vtable.ctx, topic, &stream, ts_col_name, &err))
      << err.message;

  // append_arrow_stream ABI: on success, the host takes ownership of the
  // stream and releases it before returning. Our local `stream` must now
  // have a null release pointer (it was zeroed by the release callback).
  EXPECT_EQ(stream.release, nullptr);

  write_host.flushPending();
  {
    const auto lock = engine.lockEngine();
    const auto& chunks = engine.getTopicStorage(topic.id)->sealedChunks();
    ASSERT_EQ(chunks.size(), 1U);
    for (std::size_t c = 0; c < chunks.front().columns.size(); ++c) {
      if (chunks.front().columns[c].descriptor->field_path == "value") {
        *stored_encoding = chunks.front().columnEncoding(c);
      }
    }
  }

  // Catalog snapshot — look up the field handle for "value".
  ObjectStore object_store;
  DatastoreToolboxHost tb_host(engine, object_store);
  auto tb_vtable = tb_host.raw();

  PJ_catalog_snapshot_t snapshot{};
  ASSERT_TRUE(tb_vtable.vtable->acquire_catalog_snapshot(tb_vtable.ctx, &snapshot, &err)) << err.message;

  PJ_field_handle_t value_field{};
  bool value_found = false;
  for (std::size_t i = 0; i < snapshot.field_count; ++i) {
    const auto& f = snapshot.fields[i];
    if (std::string(f.name.data, f.name.size).find("value") != std::string::npos) {
      value_field = f.handle;
      value_found = true;
      break;
    }
  }
  snapshot.release(snapshot.release_ctx);
  ASSERT_TRUE(value_found) << "field 'value' missing from catalog";

  // Read it back via read_series_arrow.
  ArrowSchema out_schema{};
  ArrowArray out_array{};
  ASSERT_TRUE(tb_vtable.vtable->read_series_arrow(tb_vtable.ctx, value_field, &out_schema, &out_array, &err))
      << err.message;
  ASSERT_NE(out_schema.release, nullptr);
  ASSERT_NE(out_array.release, nullptr);

  // Schema: struct { timestamp: int64, <field_name>: double }
  EXPECT_EQ(std::string(out_schema.format), "+s");
  ASSERT_EQ(out_schema.n_children, 2);
  EXPECT_EQ(std::string(out_schema.children[0]->name), "timestamp");
  EXPECT_EQ(std::string(out_schema.children[0]->format), "l");  // int64
  EXPECT_EQ(std::string(out_schema.children[1]->format), value_type == NANOARROW_TYPE_FLOAT ? "f" : "g");

  // Array layout matches.
  ASSERT_EQ(out_array.length, static_cast<int64_t>(timestamps.size()));
  ASSERT_EQ(out_array.n_children, 2);

  // Walk via ArrowArrayView to extract the values.
  nanoarrow::UniqueArrayView view;
  ArrowError vf_err;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), &out_schema, &vf_err), NANOARROW_OK) << vf_err.message;
  ASSERT_EQ(ArrowArrayViewSetArray(view.get(), &out_array, &vf_err), NANOARROW_OK) << vf_err.message;

  for (int64_t i = 0; i < out_array.length; ++i) {
    EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view->children[0], i), timestamps[static_cast<std::size_t>(i)]);
    EXPECT_EQ(ArrowArrayViewGetDoubleUnsafe(view->children[1], i), values[static_cast<std::size_t>(i)]);
  }

  // Release the host-owned structs as per the ABI contract.
  out_schema.release(&out_schema);
  out_array.release(&out_array);
  EXPECT_EQ(out_schema.release, nullptr);
  EXPECT_EQ(out_array.release, nullptr);
}

TEST(ArrowStreamRoundTripTest, WriteViaAppendArrowStreamReadViaReadSeriesArrow) {
  EncodingType encoding{};
  roundTripThroughArrow({1.5, 2.5, 3.5, 4.5, 5.5}, &encoding);
  EXPECT_EQ(encoding, EncodingType::kRaw);
}

// Whole-number doubles are stored frame-of-reference; read_series_arrow must
// still hand back the exact float64 values.
TEST(ArrowStreamRoundTripTest, WholeNumberDoublesRoundTripThroughFrameOfReference) {
  EncodingType encoding{};
  roundTripThroughArrow({-20.0, 0.0, 15.0, 1000.0, -3.0}, &encoding);
  EXPECT_EQ(encoding, EncodingType::kFrameOfReference);
}

TEST(ArrowStreamRoundTripTest, WholeNumberFloat32RoundTripsThroughFrameOfReference) {
  EncodingType encoding{};
  // Around 2^24, where float32 spacing becomes 2: every value is still a whole,
  // exactly representable float, with a narrow (2-byte) range.
  roundTripThroughArrow({16777196.0, 16777216.0, 16777218.0, 16777236.0, 16778216.0}, &encoding, NANOARROW_TYPE_FLOAT);
  EXPECT_EQ(encoding, EncodingType::kFrameOfReference);
}

TEST(ArrowStreamRoundTripTest, ParserWriteHostAppendArrowStreamWritesBoundTopic) {
  DataEngine engine;
  auto td_id = engine.createTimeDomain("parser_td");
  ASSERT_TRUE(td_id.has_value()) << td_id.error();
  auto ds_id = engine.createDataset(DatasetDescriptor{.source_name = "parser", .time_domain_id = *td_id});
  ASSERT_TRUE(ds_id.has_value()) << ds_id.error();

  DatastoreSourceWriteHost source_write_host(engine, PJ_data_source_handle_t{static_cast<uint32_t>(*ds_id)});
  auto source_vtable = source_write_host.raw();

  PJ_topic_handle_t topic{};
  PJ_error_t err{};
  PJ_string_view_t topic_name{"parser_metric", 13};
  ASSERT_TRUE(source_vtable.vtable->ensure_topic(source_vtable.ctx, topic_name, &topic, &err)) << err.message;

  DatastoreParserWriteHost parser_write_host(engine, topic);
  auto parser_vtable = parser_write_host.raw();

  const std::vector<int64_t> timestamps = {10, 20, 30};
  const std::vector<double> values = {7.0, 8.5, 9.25};
  auto built = makeStream(timestamps, values);

  ArrowArrayStream stream{};
  initOneBatchStream(&stream, std::move(built));

  PJ_string_view_t ts_col_name{"ts_col", 6};
  ASSERT_TRUE(parser_vtable.vtable->append_arrow_stream(parser_vtable.ctx, &stream, ts_col_name, &err)) << err.message;
  EXPECT_EQ(stream.release, nullptr);

  parser_write_host.flushPending();

  ObjectStore object_store;
  DatastoreToolboxHost tb_host(engine, object_store);
  auto tb_vtable = tb_host.raw();

  PJ_catalog_snapshot_t snapshot{};
  ASSERT_TRUE(tb_vtable.vtable->acquire_catalog_snapshot(tb_vtable.ctx, &snapshot, &err)) << err.message;

  PJ_field_handle_t value_field{};
  bool value_found = false;
  for (std::size_t i = 0; i < snapshot.field_count; ++i) {
    const auto& f = snapshot.fields[i];
    if (std::string(f.name.data, f.name.size).find("value") != std::string::npos) {
      value_field = f.handle;
      value_found = true;
      break;
    }
  }
  snapshot.release(snapshot.release_ctx);
  ASSERT_TRUE(value_found) << "field 'value' missing from catalog";

  ArrowSchema out_schema{};
  ArrowArray out_array{};
  ASSERT_TRUE(tb_vtable.vtable->read_series_arrow(tb_vtable.ctx, value_field, &out_schema, &out_array, &err))
      << err.message;
  ASSERT_NE(out_schema.release, nullptr);
  ASSERT_NE(out_array.release, nullptr);

  nanoarrow::UniqueArrayView view;
  ArrowError vf_err;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), &out_schema, &vf_err), NANOARROW_OK) << vf_err.message;
  ASSERT_EQ(ArrowArrayViewSetArray(view.get(), &out_array, &vf_err), NANOARROW_OK) << vf_err.message;

  ASSERT_EQ(out_array.length, static_cast<int64_t>(timestamps.size()));
  for (int64_t i = 0; i < out_array.length; ++i) {
    EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view->children[0], i), timestamps[static_cast<std::size_t>(i)]);
    EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view->children[1], i), values[static_cast<std::size_t>(i)]);
  }

  out_schema.release(&out_schema);
  out_array.release(&out_array);
  EXPECT_EQ(out_schema.release, nullptr);
  EXPECT_EQ(out_array.release, nullptr);
}

// Names containing "//" (empty path segments, e.g. a CSV column header) must be
// collapsed to a single '/' at the ingest boundary, so the stored topic/field name
// (which the UI catalog and the by-name transform resolver both read) has no "//".
TEST(ArrowStreamRoundTripTest, CollapsesDoubleSlashesInNames) {
  DataEngine engine;
  auto td_id = engine.createTimeDomain("td");
  ASSERT_TRUE(td_id.has_value()) << td_id.error();
  auto ds_id = engine.createDataset(DatasetDescriptor{.source_name = "csv", .time_domain_id = *td_id});
  ASSERT_TRUE(ds_id.has_value()) << ds_id.error();

  DatastoreSourceWriteHost write_host(engine, PJ_data_source_handle_t{static_cast<uint32_t>(*ds_id)});
  sdk::SourceWriteHostView writer(write_host.raw());

  // Mirror the CSV plugin: topic = file basename, column header carries "//".
  const auto topic = writer.ensureTopic("myfile");
  ASSERT_TRUE(topic.has_value()) << topic.error();
  ASSERT_TRUE(writer.ensureField(*topic, "/robot//pose/x", PrimitiveType::kFloat64).has_value());
  const std::vector<sdk::NamedFieldValue> fields = {{.name = "/robot//pose/x", .value = 1.0}};
  ASSERT_TRUE(writer.appendRecord(*topic, 10, fields).has_value());
  write_host.flushPending();

  const auto lock = engine.lockEngine();
  const TopicStorage* storage = engine.getTopicStorage(topic->id);
  ASSERT_NE(storage, nullptr);
  bool found_normalized = false;
  bool found_raw = false;
  for (const auto& col : storage->columnDescriptors()) {
    // Internal "//" collapses to "/" and the leading '/' is dropped (fields are
    // relative to their topic), so "/robot//pose/x" is stored as "robot/pose/x".
    found_normalized = found_normalized || col.field_path == "robot/pose/x";
    found_raw = found_raw || col.field_path == "/robot//pose/x" || col.field_path == "/robot/pose/x";
  }
  EXPECT_TRUE(found_normalized) << "field name was not normalized (collapse + strip leading '/')";
  EXPECT_FALSE(found_raw) << "raw or leading-slash field name survived ingest";
}

}  // namespace
}  // namespace PJ
