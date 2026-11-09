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

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

// ---------------------------------------------------------------------------
// Build a one-batch ArrowArrayStream with columns {timestamp: int64, value: double}
// ---------------------------------------------------------------------------

struct BuiltStream {
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
};

/// `nulls`, when given, marks the rows written as null instead of their value.
BuiltStream makeStream(
    const std::vector<int64_t>& timestamps, const std::vector<double>& values,
    ArrowType value_type = NANOARROW_TYPE_DOUBLE, const std::vector<bool>& nulls = {}) {
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
    const auto row = static_cast<std::size_t>(i);
    if (row < nulls.size() && nulls[row]) {
      EXPECT_EQ(ArrowArrayAppendNull(result.array->children[1], 1), NANOARROW_OK);
    } else {
      EXPECT_EQ(ArrowArrayAppendDouble(result.array->children[1], values[row]), NANOARROW_OK);
    }
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

// A native Arrow TIMESTAMP column (here in milliseconds) can be selected as the
// timestamp by name; the value-column mappings do not list it.
TEST(ArrowStreamRoundTripTest, NativeTimestampColumnIsSelectableByName) {
  DataEngine engine;
  auto td_id = engine.createTimeDomain("td");
  auto ds_id = engine.createDataset(DatasetDescriptor{.source_name = "src", .time_domain_id = *td_id});
  ASSERT_TRUE(ds_id.has_value());
  DatastoreSourceWriteHost write_host(engine, PJ_data_source_handle_t{static_cast<uint32_t>(*ds_id)});
  sdk::SourceWriteHostView writer(write_host.raw());
  const auto topic = writer.ensureTopic("stamped");
  ASSERT_TRUE(topic.has_value());

  BuiltStream built;
  ArrowSchemaInit(built.schema.get());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(built.schema.get(), 2), NANOARROW_OK);
  ASSERT_EQ(
      ArrowSchemaSetTypeDateTime(
          built.schema->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MILLI, nullptr),
      NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(built.schema->children[0], "time"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(built.schema->children[1], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(built.schema->children[1], "value"), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayInitFromSchema(built.array.get(), built.schema.get(), nullptr), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayStartAppending(built.array.get()), NANOARROW_OK);
  for (int64_t ms : {5, 6}) {
    ASSERT_EQ(ArrowArrayAppendInt(built.array->children[0], ms), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayAppendDouble(built.array->children[1], static_cast<double>(ms) * 10.0), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayFinishElement(built.array.get()), NANOARROW_OK);
  }
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(built.array.get(), nullptr), NANOARROW_OK);

  ArrowArrayStream stream{};
  initOneBatchStream(&stream, std::move(built));
  auto raw = write_host.raw();
  PJ_error_t err{};
  ASSERT_TRUE(raw.vtable->append_arrow_stream(raw.ctx, *topic, &stream, PJ_string_view_t{"time", 4}, &err))
      << err.message;
  write_host.flushPending();

  const auto lock = engine.lockEngine();
  const auto& chunks = engine.getTopicStorage(topic->id)->sealedChunks();
  ASSERT_EQ(chunks.size(), 1U);
  EXPECT_EQ(chunks.front().timestamps, (std::vector<Timestamp>{5'000'000, 6'000'000}));  // ms scaled to ns
}

// A topic whose chunks were committed out of time order (an on-demand series' backfill lands after its forward range)
// reads back through read_series_arrow in time order: the export merges the chunks, each value at its own timestamp.
TEST(ArrowStreamRoundTripTest, ReadSeriesArrowMergesChunksCommittedOutOfOrder) {
  DataEngine engine;
  ObjectStore object_store;
  DatastoreToolboxHost toolbox_impl(engine, object_store);
  sdk::ToolboxHostView toolbox(toolbox_impl.raw());
  const auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "src"});
  ASSERT_TRUE(dataset.has_value());
  DataWriter writer = engine.createWriter();
  const auto handle = writer.registerScalarSeries(*dataset, "data", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value());
  const auto commit = [&](int first, int last) {
    for (int t = first; t <= last; ++t) {
      writer.appendScalar(*handle, t, double(t) * 0.1);
    }
    ASSERT_FALSE(engine.commitChunks(writer.flushAll()).empty());
  };
  commit(100, 199);
  commit(0, 99);
  commit(200, 249);

  auto snapshot = toolbox.catalogSnapshot();
  ASSERT_TRUE(snapshot.has_value());
  ASSERT_EQ(snapshot->topics().size(), 1u);
  auto series = toolbox.readSeries(snapshot->fields()[snapshot->topics()[0].first_field].handle);
  ASSERT_TRUE(series.has_value()) << series.error();
  const auto timestamps = series->timestamps();
  ASSERT_EQ(timestamps.size(), 250u);
  const double* values = series->valuesAsFloat64();
  ASSERT_NE(values, nullptr);
  for (std::size_t row = 0; row < timestamps.size(); ++row) {
    ASSERT_EQ(timestamps[row], static_cast<int64_t>(row));
    ASSERT_DOUBLE_EQ(values[row], double(row) * 0.1);
  }
}

// ---------------------------------------------------------------------------
// readSeriesArrowWindow
// ---------------------------------------------------------------------------

constexpr Timestamp kEver = std::numeric_limits<Timestamp>::min();
constexpr Timestamp kNever = std::numeric_limits<Timestamp>::max();

/// One read through readSeriesArrowWindow, row by row; a null value reads as nullopt.
struct WindowRead {
  std::vector<int64_t> ts;
  std::vector<std::optional<double>> values;
  std::vector<int64_t> ints;  ///< the values read as integers, exact for an integer column
};

WindowRead readWindow(DataEngine& engine, PJ_field_handle_t field, Timestamp first, Timestamp last) {
  WindowRead out;
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  const Status status = readSeriesArrowWindow(engine, field, first, last, schema.get(), array.get());
  EXPECT_TRUE(status) << status.error();
  if (!status) {
    return out;
  }
  nanoarrow::UniqueArrayView view;
  ArrowError err;
  EXPECT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &err), NANOARROW_OK) << err.message;
  EXPECT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), &err), NANOARROW_OK) << err.message;
  for (int64_t i = 0; i < array->length; ++i) {
    out.ts.push_back(ArrowArrayViewGetIntUnsafe(view->children[0], i));
    const bool is_null = ArrowArrayViewIsNull(view->children[1], i);
    out.values.push_back(is_null ? std::nullopt : std::optional(ArrowArrayViewGetDoubleUnsafe(view->children[1], i)));
    out.ints.push_back(is_null ? 0 : ArrowArrayViewGetIntUnsafe(view->children[1], i));
  }
  return out;
}

/// The handle of the first field whose name contains `name_part` in `engine`'s toolbox catalog (the first field of the
/// first topic for an empty `name_part`).
PJ_field_handle_t fieldHandleOf(DataEngine& engine, std::string_view name_part = {}) {
  ObjectStore object_store;
  DatastoreToolboxHost toolbox_impl(engine, object_store);
  auto snapshot = sdk::ToolboxHostView(toolbox_impl.raw()).catalogSnapshot();
  EXPECT_TRUE(snapshot.has_value());
  if (name_part.empty()) {
    return snapshot->fields()[snapshot->topics()[0].first_field].handle;
  }
  for (const auto& f : snapshot->fields()) {
    if (std::string_view(f.name.data, f.name.size).find(name_part) != std::string_view::npos) {
      return f.handle;
    }
  }
  ADD_FAILURE() << "no field named like " << name_part;
  return {};
}

/// A scalar topic with one chunk per [first, last] batch of integer timestamps, committed in the given order, holding
/// `value(t)` at each t. Returns the field's handle.
PJ_field_handle_t writeBatches(
    DataEngine& engine, const std::vector<std::pair<int, int>>& batches,
    const std::function<NumericValue(int)>& value = [](int t) { return NumericValue(double(t) * 0.1); },
    NumericType type = NumericType::kFloat64) {
  const auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "src"});
  EXPECT_TRUE(dataset.has_value());
  DataWriter writer = engine.createWriter();
  const auto handle = writer.registerScalarSeries(*dataset, "data", type);
  EXPECT_TRUE(handle.has_value());
  for (const auto& [first, last] : batches) {
    for (int t = first; t <= last; ++t) {
      writer.appendScalar(*handle, t, value(t));
    }
    EXPECT_FALSE(engine.commitChunks(writer.flushAll()).empty());
  }
  return fieldHandleOf(engine);
}

std::vector<int64_t> stamps(int64_t first, int64_t last) {
  std::vector<int64_t> out;
  for (int64_t t = first; t <= last; ++t) {
    out.push_back(t);
  }
  return out;
}

// The window's samples plus the one before it, whose value holds at the window's start.
TEST(ReadSeriesArrowWindowTest, ReturnsTheWindowPlusThePrecedingSample) {
  DataEngine engine;
  const auto field = writeBatches(engine, {{0, 99}, {100, 199}, {200, 249}});
  const WindowRead read = readWindow(engine, field, 150, 160);
  EXPECT_EQ(read.ts, stamps(149, 160));
  ASSERT_EQ(read.values.size(), read.ts.size());
  for (std::size_t i = 0; i < read.ts.size(); ++i) {
    ASSERT_TRUE(read.values[i].has_value());
    EXPECT_DOUBLE_EQ(*read.values[i], double(read.ts[i]) * 0.1);
  }
}

TEST(ReadSeriesArrowWindowTest, AWindowBeforeTheSeriesHasNoPrecedingSample) {
  DataEngine engine;
  const auto field = writeBatches(engine, {{0, 99}, {100, 199}});
  EXPECT_EQ(readWindow(engine, field, -10, 5).ts, stamps(0, 5));
}

// Past the end, the last sample is the value that still holds.
TEST(ReadSeriesArrowWindowTest, AWindowPastTheEndReturnsTheLastSample) {
  DataEngine engine;
  const auto field = writeBatches(engine, {{0, 99}, {100, 249}});
  EXPECT_EQ(readWindow(engine, field, 300, 400).ts, (std::vector<int64_t>{249}));
}

TEST(ReadSeriesArrowWindowTest, TheUnboundedWindowIsTheWholeSeries) {
  DataEngine engine;
  const auto field = writeBatches(engine, {{0, 99}, {100, 199}, {200, 249}});
  const WindowRead read = readWindow(engine, field, kEver, kNever);
  EXPECT_EQ(read.ts, stamps(0, 249));
  ASSERT_EQ(read.values.size(), 250u);
  EXPECT_DOUBLE_EQ(*read.values[249], 24.9);
}

// Chunks committed out of time order (an on-demand series' backfill) still read back in time order, in a window and
// whole.
TEST(ReadSeriesArrowWindowTest, ChunksCommittedOutOfOrderReadBackInTimeOrder) {
  DataEngine engine;
  const auto field = writeBatches(engine, {{100, 199}, {0, 99}, {200, 249}});
  EXPECT_EQ(readWindow(engine, field, 95, 105).ts, stamps(94, 105));
  const WindowRead whole = readWindow(engine, field, kEver, kNever);
  EXPECT_EQ(whole.ts, stamps(0, 249));
  for (std::size_t i = 0; i < whole.ts.size(); ++i) {
    ASSERT_DOUBLE_EQ(*whole.values[i], double(whole.ts[i]) * 0.1);
  }
}

// A chunk whose values are all equal is stored constant-encoded; it reads back like any other.
TEST(ReadSeriesArrowWindowTest, AConstantChunkReadsBackItsValue) {
  DataEngine engine;
  const auto field =
      writeBatches(engine, {{0, 99}, {100, 199}}, [](int t) { return NumericValue(t < 100 ? 7.5 : double(t)); });
  {
    const auto lock = engine.lockEngine();
    const auto& chunks = engine.getTopicStorage(field.topic.id)->sealedChunks();
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks.front().columnEncoding(0), EncodingType::kConstant);
  }
  const WindowRead read = readWindow(engine, field, kEver, kNever);
  ASSERT_EQ(read.values.size(), 200u);
  EXPECT_DOUBLE_EQ(*read.values[0], 7.5);
  EXPECT_DOUBLE_EQ(*read.values[99], 7.5);
  EXPECT_DOUBLE_EQ(*read.values[100], 100.0);
}

// Integers beyond a double's 53-bit mantissa stay exact.
TEST(ReadSeriesArrowWindowTest, Int64ValuesStayExact) {
  DataEngine engine;
  constexpr int64_t kBase = (int64_t{1} << 62) + 1;
  const auto field = writeBatches(
      engine, {{0, 99}, {100, 199}}, [](int t) { return NumericValue(kBase + 2 * t); }, NumericType::kInt64);
  const WindowRead read = readWindow(engine, field, 50, 150);
  ASSERT_EQ(read.ts, stamps(49, 150));
  for (std::size_t i = 0; i < read.ts.size(); ++i) {
    ASSERT_EQ(read.ints[i], kBase + 2 * read.ts[i]);
  }
}

// Rows 1..8 holding 1..8, with rows 3 and 6 null.
const std::vector<int64_t> kNullableStamps = stamps(1, 8);
const std::vector<double> kNullableValues{1, 2, 3, 4, 5, 6, 7, 8};
const std::vector<bool> kNullRows{false, false, true, false, false, true, false, false};

/// The float64 field "metric/value" holding kNullableValues, written through the Arrow ingest path so the kNullRows
/// rows are stored null.
PJ_field_handle_t writeNullableMetric(DataEngine& engine) {
  const auto td_id = engine.createTimeDomain("td");
  const auto ds_id = engine.createDataset(DatasetDescriptor{.source_name = "src", .time_domain_id = *td_id});
  DatastoreSourceWriteHost write_host(engine, PJ_data_source_handle_t{static_cast<uint32_t>(*ds_id)});
  auto write_vtable = write_host.raw();
  PJ_topic_handle_t topic{};
  PJ_error_t err{};
  EXPECT_TRUE(write_vtable.vtable->ensure_topic(write_vtable.ctx, PJ_string_view_t{"metric", 6}, &topic, &err));
  ArrowArrayStream stream{};
  initOneBatchStream(&stream, makeStream(kNullableStamps, kNullableValues, NANOARROW_TYPE_DOUBLE, kNullRows));
  EXPECT_TRUE(
      write_vtable.vtable->append_arrow_stream(write_vtable.ctx, topic, &stream, PJ_string_view_t{"ts_col", 6}, &err))
      << err.message;
  write_host.flushPending();
  return fieldHandleOf(engine, "value");
}

// Null rows stay null, including the preceding sample.
TEST(ReadSeriesArrowWindowTest, NullRowsStayNull) {
  DataEngine engine;
  const auto field = writeNullableMetric(engine);
  const WindowRead whole = readWindow(engine, field, kEver, kNever);
  ASSERT_EQ(whole.ts, kNullableStamps);
  for (std::size_t i = 0; i < kNullRows.size(); ++i) {
    EXPECT_EQ(whole.values[i].has_value(), !kNullRows[i]) << "row " << i;
  }
  const WindowRead window = readWindow(engine, field, 4, 5);  // preceded by the null at 3
  ASSERT_EQ(window.ts, stamps(3, 5));
  EXPECT_FALSE(window.values[0].has_value());
  EXPECT_DOUBLE_EQ(*window.values[1], 4.0);
}

// Retention evicted the rows below its floor: the sample before the window is the first one kept, not an evicted one.
TEST(ReadSeriesArrowWindowTest, NoPrecedingSampleBelowTheRetentionFloor) {
  DataEngine engine;
  const auto field = writeBatches(engine, {{0, 99}, {100, 199}});
  engine.enforceRetention(50);  // floor 149: the first chunk goes, the second straddles it
  EXPECT_EQ(readWindow(engine, field, 0, 160).ts, stamps(149, 160));
}

// A field that appears mid-stream, backfilled before an older chunk's rows: the sample before the window is the
// field's own last one, not the topic's (whose chunk does not hold the field).
TEST(ReadSeriesArrowWindowTest, ThePrecedingSampleIsTheFieldsOwn) {
  DataEngine engine;
  const auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "src"});
  DataWriter writer = engine.createWriter();
  const auto topic = writer.registerTopic(*dataset, TopicDescriptor{.name = "multi", .dataset_id = *dataset});
  ASSERT_TRUE(topic.has_value()) << topic.error();
  ASSERT_TRUE(writer.ensureColumn(*topic, "a", PrimitiveType::kFloat64).has_value());
  ASSERT_TRUE(writer.beginRow(*topic, 20).has_value());
  writer.set<double>(*topic, 0, 1.0);
  ASSERT_TRUE(writer.finishRow(*topic).has_value());
  engine.commitChunks(writer.flushAll());  // a chunk without "b"

  DataWriter backfill = engine.createWriter();
  ASSERT_TRUE(backfill.ensureColumn(*topic, "b", PrimitiveType::kFloat64).has_value());
  ASSERT_TRUE(backfill.beginRow(*topic, 10).has_value());
  backfill.set<double>(*topic, 0, 2.0);
  backfill.set<double>(*topic, 1, 7.0);
  ASSERT_TRUE(backfill.finishRow(*topic).has_value());
  engine.commitChunks(backfill.flushAll());

  const WindowRead read = readWindow(engine, fieldHandleOf(engine, "b"), 21, 30);
  ASSERT_EQ(read.ts, (std::vector<int64_t>{10}));
  EXPECT_DOUBLE_EQ(*read.values[0], 7.0);
}

/// scanSeriesValues' blocks, a constant block expanded to one value per sample.
struct ScannedSeries {
  std::vector<Timestamp> ts;
  std::vector<double> values;
  std::size_t blocks = 0;
  std::size_t constant_blocks = 0;
};

ScannedSeries scan(const DataEngine& engine, PJ_field_handle_t field) {
  ScannedSeries out;
  const Status status = scanSeriesValues(engine, field, [&out](Span<const Timestamp> ts, Span<const double> values) {
    ++out.blocks;
    out.ts.insert(out.ts.end(), ts.begin(), ts.end());
    if (values.size() == 1 && ts.size() > 1) {
      ++out.constant_blocks;
      out.values.insert(out.values.end(), ts.size(), values[0]);
    } else {
      EXPECT_EQ(values.size(), ts.size());
      out.values.insert(out.values.end(), values.begin(), values.end());
    }
  });
  EXPECT_TRUE(status.has_value()) << status.error();
  return out;
}

// One block per chunk; a constant-encoded chunk is a single value.
TEST(ScanSeriesValuesTest, AConstantChunkIsOneValue) {
  DataEngine engine;
  const auto field =
      writeBatches(engine, {{0, 9}, {10, 19}}, [](int t) { return NumericValue(t < 10 ? 5.0 : double(t)); });
  const auto scanned = scan(engine, field);
  EXPECT_EQ(scanned.blocks, 2u);
  EXPECT_EQ(scanned.constant_blocks, 1u);
  EXPECT_EQ(scanned.ts, stamps(0, 19));
  for (std::size_t i = 0; i < scanned.values.size(); ++i) {
    EXPECT_DOUBLE_EQ(scanned.values[i], i < 10 ? 5.0 : double(i));
  }
}

TEST(ScanSeriesValuesTest, NullsAreNaN) {
  DataEngine engine;
  const auto scanned = scan(engine, writeNullableMetric(engine));
  ASSERT_EQ(scanned.ts, kNullableStamps);
  for (std::size_t i = 0; i < scanned.values.size(); ++i) {
    EXPECT_EQ(std::isnan(scanned.values[i]), static_cast<bool>(kNullRows[i])) << "row " << i;
  }
}

// A backfilled chunk still comes as whole blocks, in time order.
TEST(ScanSeriesValuesTest, ChunksCommittedOutOfOrderScanInTimeOrder) {
  DataEngine engine;
  const auto scanned = scan(engine, writeBatches(engine, {{100, 199}, {0, 99}, {200, 249}}));
  EXPECT_EQ(scanned.blocks, 3u);
  ASSERT_EQ(scanned.ts, stamps(0, 249));
  for (std::size_t i = 0; i < scanned.values.size(); ++i) {
    ASSERT_DOUBLE_EQ(scanned.values[i], double(i) * 0.1);
  }
}

TEST(ScanSeriesValuesTest, StartsAtTheRetentionFloor) {
  DataEngine engine;
  const auto field = writeBatches(engine, {{0, 99}, {100, 199}});
  engine.enforceRetention(50);  // floor 149
  EXPECT_EQ(scan(engine, field).ts, stamps(149, 199));
}

}  // namespace
}  // namespace PJ
