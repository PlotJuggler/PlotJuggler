// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/arrow_import.hpp"

#include <fmt/format.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow.hpp"
#include "nanoarrow/nanoarrow_ipc.h"
#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ::arrow_import {
namespace {

// ---------------------------------------------------------------------------
// Non-owning IPC input stream from PJ::Span<const uint8_t>
// ---------------------------------------------------------------------------

struct SpanInputStreamData {
  const uint8_t* data;
  int64_t size;
  int64_t offset;
};

ArrowErrorCode spanInputStreamRead(
    ArrowIpcInputStream* stream, uint8_t* buf, int64_t buf_size_bytes, int64_t* size_read_out, ArrowError* /*error*/) {
  auto* s = static_cast<SpanInputStreamData*>(stream->private_data);
  const int64_t available = s->size - s->offset;
  const int64_t to_read = std::min(buf_size_bytes, available);
  if (to_read > 0) {
    std::memcpy(buf, s->data + s->offset, static_cast<std::size_t>(to_read));
    s->offset += to_read;
  }
  *size_read_out = to_read;
  return NANOARROW_OK;
}

void spanInputStreamRelease(ArrowIpcInputStream* stream) {
  delete static_cast<SpanInputStreamData*>(stream->private_data);
  stream->private_data = nullptr;
  stream->release = nullptr;
}

void initSpanInputStream(ArrowIpcInputStream* stream, PJ::Span<const uint8_t> span) {
  stream->read = spanInputStreamRead;
  stream->release = spanInputStreamRelease;
  stream->private_data = new SpanInputStreamData{span.data(), static_cast<int64_t>(span.size()), 0};
}

// ---------------------------------------------------------------------------
// nanoarrow ArrowType → PrimitiveType
// ---------------------------------------------------------------------------

std::optional<PrimitiveType> nanoarrowTypeToPrimitive(ArrowType type) {
  switch (type) {
    case NANOARROW_TYPE_INT8:
      return PrimitiveType::kInt8;
    case NANOARROW_TYPE_INT16:
      return PrimitiveType::kInt16;
    case NANOARROW_TYPE_INT32:
      return PrimitiveType::kInt32;
    case NANOARROW_TYPE_INT64:
      return PrimitiveType::kInt64;
    case NANOARROW_TYPE_UINT8:
      return PrimitiveType::kUint8;
    case NANOARROW_TYPE_UINT16:
      return PrimitiveType::kUint16;
    case NANOARROW_TYPE_UINT32:
      return PrimitiveType::kUint32;
    case NANOARROW_TYPE_UINT64:
      return PrimitiveType::kUint64;
    case NANOARROW_TYPE_FLOAT:
      return PrimitiveType::kFloat32;
    case NANOARROW_TYPE_DOUBLE:
      return PrimitiveType::kFloat64;
    case NANOARROW_TYPE_BOOL:
      return PrimitiveType::kBool;
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_LARGE_STRING:
      return PrimitiveType::kString;
    default:
      return std::nullopt;
  }
}

// ---------------------------------------------------------------------------
// Helpers: extract raw data from nanoarrow ArrowArrayView children
// ---------------------------------------------------------------------------

struct ColumnDataWithBuffer {
  ColumnData col_data;
  std::vector<int64_t> int64_buf;
  std::vector<uint64_t> uint64_buf;
  std::vector<uint32_t> offset_buf;
};

PJ::Expected<ColumnDataWithBuffer> makeColumnDataNanoarrow(
    const ArrowArrayView* child, const ArrowColumnMapping& mapping, int64_t length) {
  ColumnDataWithBuffer result;
  const auto sk = storageKindOf(mapping.pj_type);
  const auto n = static_cast<std::size_t>(length);

  // Validity bitmap
  BitSpan validity_view;
  if (child->null_count > 0 && child->buffer_views[0].data.as_uint8 != nullptr) {
    const std::size_t validity_offset = static_cast<std::size_t>(child->offset);
    const std::size_t validity_bytes = (validity_offset + n + 7) / 8;
    validity_view =
        BitSpan{Span<const uint8_t>(child->buffer_views[0].data.as_uint8, validity_bytes), validity_offset, n};
  }

  switch (sk) {
    case StorageKind::kFloat32: {
      result.col_data = ColumnData::float32(
          mapping.pj_column_index, Span<const float>(child->buffer_views[1].data.as_float + child->offset, n),
          validity_view);
      break;
    }
    case StorageKind::kFloat64: {
      result.col_data = ColumnData::float64(
          mapping.pj_column_index, Span<const double>(child->buffer_views[1].data.as_double + child->offset, n),
          validity_view);
      break;
    }
    case StorageKind::kInt32: {
      result.col_data = ColumnData::int32(
          mapping.pj_column_index, Span<const int32_t>(child->buffer_views[1].data.as_int32 + child->offset, n),
          validity_view);
      break;
    }
    case StorageKind::kInt64: {
      switch (mapping.pj_type) {
        case PrimitiveType::kInt8:
        case PrimitiveType::kInt16: {
          result.int64_buf.resize(n);
          for (int64_t i = 0; i < length; ++i) {
            result.int64_buf[static_cast<std::size_t>(i)] = ArrowArrayViewGetIntUnsafe(child, i);
          }
          result.col_data = ColumnData::int64(
              mapping.pj_column_index, Span<const int64_t>(result.int64_buf.data(), n), validity_view);
          break;
        }
        case PrimitiveType::kInt64: {
          result.col_data = ColumnData::int64(
              mapping.pj_column_index, Span<const int64_t>(child->buffer_views[1].data.as_int64 + child->offset, n),
              validity_view);
          break;
        }
        default:
          break;
      }
      break;
    }
    case StorageKind::kUint64: {
      switch (mapping.pj_type) {
        case PrimitiveType::kUint8:
        case PrimitiveType::kUint16:
        case PrimitiveType::kUint32: {
          result.uint64_buf.resize(n);
          for (int64_t i = 0; i < length; ++i) {
            result.uint64_buf[static_cast<std::size_t>(i)] = ArrowArrayViewGetUIntUnsafe(child, i);
          }
          result.col_data = ColumnData::uint64(
              mapping.pj_column_index, Span<const uint64_t>(result.uint64_buf.data(), n), validity_view);
          break;
        }
        case PrimitiveType::kUint64: {
          result.col_data = ColumnData::uint64(
              mapping.pj_column_index, Span<const uint64_t>(child->buffer_views[1].data.as_uint64 + child->offset, n),
              validity_view);
          break;
        }
        default:
          break;
      }
      break;
    }
    case StorageKind::kBool: {
      // Arrow stores bools as packed bits; we need unpacked uint8_t.
      std::vector<uint8_t> bool_buf(n);
      for (int64_t i = 0; i < length; ++i) {
        bool_buf[static_cast<std::size_t>(i)] = ArrowArrayViewGetIntUnsafe(child, i) != 0 ? uint8_t{1} : uint8_t{0};
      }
      // Store in uint64_buf as raw bytes
      result.uint64_buf.resize((n + sizeof(uint64_t) - 1) / sizeof(uint64_t));
      std::memcpy(result.uint64_buf.data(), bool_buf.data(), n);
      result.col_data = ColumnData::boolean(
          mapping.pj_column_index, Span<const uint8_t>(reinterpret_cast<const uint8_t*>(result.uint64_buf.data()), n),
          validity_view);
      break;
    }
    case StorageKind::kString: {
      // Arrow STRING has int32 offsets, LARGE_STRING int64; PJ stores uint32.
      result.offset_buf.resize(n + 1);
      if (child->storage_type == NANOARROW_TYPE_LARGE_STRING) {
        const auto* offsets_ptr = child->buffer_views[1].data.as_int64 + child->offset;
        if (offsets_ptr[n] > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
          return PJ::unexpected(
              fmt::format(
                  "Arrow LARGE_STRING column '{}': {} bytes of string data in one batch exceed the 4 GiB limit",
                  mapping.field_name, offsets_ptr[n]));
        }
        for (std::size_t i = 0; i <= n; ++i) {
          result.offset_buf[i] = static_cast<uint32_t>(offsets_ptr[i]);
        }
      } else {
        const auto* offsets_ptr = child->buffer_views[1].data.as_int32 + child->offset;
        for (std::size_t i = 0; i <= n; ++i) {
          result.offset_buf[i] = static_cast<uint32_t>(offsets_ptr[i]);
        }
      }
      result.col_data = ColumnData::string(
          mapping.pj_column_index, Span<const uint32_t>(result.offset_buf.data(), n + 1),
          Span<const char>(
              child->buffer_views[2].data.as_char, static_cast<std::size_t>(child->buffer_views[2].size_bytes)),
          validity_view);
      break;
    }
  }

  return result;
}

/// Scale extracted integer ticks in place to nanoseconds for an Arrow TIMESTAMP
/// unit. Plain integer timestamp columns pass NANOARROW_TIME_UNIT_NANO (identity),
/// so existing int64-ns data is untouched. ns-per-tick is derived from std::chrono
/// so the factors can't drift.
[[nodiscard]] const char* timeUnitName(ArrowTimeUnit unit) noexcept {
  switch (unit) {
    case NANOARROW_TIME_UNIT_SECOND:
      return "s";
    case NANOARROW_TIME_UNIT_MILLI:
      return "ms";
    case NANOARROW_TIME_UNIT_MICRO:
      return "us";
    case NANOARROW_TIME_UNIT_NANO:
      return "ns";
  }
  return "unknown";
}

[[nodiscard]] bool multiplyWouldOverflow(Timestamp value, Timestamp multiplier) noexcept {
  if (multiplier <= 0) {
    return true;
  }
  if (value > 0) {
    return value > std::numeric_limits<Timestamp>::max() / multiplier;
  }
  if (value < 0) {
    return value < std::numeric_limits<Timestamp>::min() / multiplier;
  }
  return false;
}

[[nodiscard]] Status rescaleToNanoseconds(std::vector<Timestamp>& values, ArrowTimeUnit unit) {
  Timestamp ns_per_tick = 1;
  switch (unit) {
    case NANOARROW_TIME_UNIT_SECOND:
      ns_per_tick = std::chrono::nanoseconds{std::chrono::seconds{1}}.count();
      break;
    case NANOARROW_TIME_UNIT_MILLI:
      ns_per_tick = std::chrono::nanoseconds{std::chrono::milliseconds{1}}.count();
      break;
    case NANOARROW_TIME_UNIT_MICRO:
      ns_per_tick = std::chrono::nanoseconds{std::chrono::microseconds{1}}.count();
      break;
    case NANOARROW_TIME_UNIT_NANO:
      return okStatus();  // already nanoseconds
  }
  for (Timestamp& v : values) {
    if (multiplyWouldOverflow(v, ns_per_tick)) {
      return unexpected(
          fmt::format(
              "Arrow TIMESTAMP[{}] value {} cannot be represented as int64 nanoseconds", timeUnitName(unit), v));
    }
    v *= ns_per_tick;
  }
  return okStatus();
}

/// Extract timestamps (as int64 ns) from an ArrowArrayView child column. @p unit
/// is the column's Arrow time unit (s/ms/us/ns) when it is a TIMESTAMP; raw
/// integer columns pass NANOARROW_TIME_UNIT_NANO and are taken as ns verbatim.
Expected<std::vector<Timestamp>> extractTimestampsNanoarrow(
    const ArrowArrayView* view, int64_t length, ArrowTimeUnit unit) {
  const auto n = static_cast<std::size_t>(length);
  std::vector<Timestamp> result(n);

  if (view->storage_type == NANOARROW_TYPE_INT64) {
    const auto* raw = view->buffer_views[1].data.as_int64 + view->offset;
    std::memcpy(result.data(), raw, n * sizeof(Timestamp));
  } else if (view->storage_type == NANOARROW_TYPE_UINT64) {
    const auto* raw = view->buffer_views[1].data.as_uint64 + view->offset;
    std::memcpy(result.data(), raw, n * sizeof(Timestamp));
  } else if (view->storage_type == NANOARROW_TYPE_INT32) {
    const auto* raw = view->buffer_views[1].data.as_int32 + view->offset;
    for (int64_t i = 0; i < length; ++i) {
      result[static_cast<std::size_t>(i)] = static_cast<Timestamp>(raw[i]);
    }
  } else {
    // Unsupported storage: synthesize row indices (not a real time → no scaling).
    for (int64_t i = 0; i < length; ++i) {
      result[static_cast<std::size_t>(i)] = i;
    }
    return result;
  }

  Status scale_status = rescaleToNanoseconds(result, unit);
  if (!scale_status.has_value()) {
    return unexpected(scale_status.error());
  }
  return result;
}

// Row indices [first, first + length) as timestamps, for streams without a
// timestamp column. `first` carries over between batches so indices stay unique.
std::vector<Timestamp> generateSequentialTimestamps(int64_t first, int64_t length) {
  const auto n = static_cast<std::size_t>(length);
  std::vector<Timestamp> result(n);
  for (int64_t i = 0; i < length; ++i) {
    result[static_cast<std::size_t>(i)] = first + i;
  }
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// schema_from_ipc
// ---------------------------------------------------------------------------

namespace {

// Derive column mappings + type tree from an already-populated nanoarrow
// schema. Shared between schemaFromIpc and schemaFromArrowStream.
PJ::Expected<std::pair<std::shared_ptr<PJ::TypeTreeNode>, std::vector<ArrowColumnMapping>>> mappingsFromSchema(
    const ArrowSchema* schema) {
  std::vector<ArrowColumnMapping> mappings;
  std::vector<std::shared_ptr<PJ::TypeTreeNode>> children;

  for (int64_t i = 0; i < schema->n_children; ++i) {
    ArrowSchemaView view;
    ArrowError error;
    const int rc = ArrowSchemaViewInit(&view, schema->children[i], &error);
    if (rc != NANOARROW_OK) {
      continue;  // skip unrecognized types
    }

    auto pj_type = nanoarrowTypeToPrimitive(view.type);
    if (!pj_type.has_value()) {
      continue;  // skip unsupported types
    }

    ArrowColumnMapping m;
    m.arrow_column_index = static_cast<int>(i);
    m.pj_column_index = mappings.size();
    m.pj_type = *pj_type;
    m.field_name = schema->children[i]->name != nullptr ? schema->children[i]->name : "";

    children.push_back(PJ::makePrimitive(m.field_name, *pj_type));
    mappings.push_back(std::move(m));
  }

  if (mappings.empty()) {
    return PJ::unexpected("No supported columns found in Arrow schema");
  }

  auto type_tree = PJ::makeStruct("arrow_row", std::move(children));
  return std::make_pair(std::move(type_tree), std::move(mappings));
}

// Pull record batches from an ArrowArrayStream* and feed them into the
// writer. The stream's schema must already be known (caller passes it in).
// Ownership: the caller retains ownership of @p stream; this helper does
// NOT call stream->release.
PJ::Status ingestBatchesFromStream(
    DataWriter& writer, TopicId topic_id, ArrowArrayStream* stream, const ArrowSchema* schema,
    const std::vector<ArrowColumnMapping>& mappings, int timestamp_column) {
  nanoarrow::UniqueArrayView array_view;
  int rc = ArrowArrayViewInitFromSchema(array_view.get(), const_cast<ArrowSchema*>(schema), nullptr);
  if (rc != NANOARROW_OK) {
    return PJ::unexpected("Failed to initialize ArrowArrayView from schema");
  }

  // Resolve the timestamp column's Arrow time unit once. A plain integer column
  // (or none) defaults to nanoseconds, preserving raw int64-ns timestamps; a
  // TIMESTAMP column carries its s/ms/us/ns unit, which extract_timestamps scales.
  ArrowTimeUnit timestamp_unit = NANOARROW_TIME_UNIT_NANO;
  if (timestamp_column >= 0 && timestamp_column < schema->n_children) {
    ArrowSchemaView ts_view;
    ArrowError ts_error;
    if (ArrowSchemaViewInit(&ts_view, schema->children[timestamp_column], &ts_error) == NANOARROW_OK &&
        ts_view.type == NANOARROW_TYPE_TIMESTAMP) {
      timestamp_unit = ts_view.time_unit;
    }
  }

  int64_t synthetic_first_row = 0;  // next row index when the stream has no timestamp column
  nanoarrow::UniqueArray batch;
  while (true) {
    batch.reset();
    rc = stream->get_next(stream, batch.get());
    if (rc != NANOARROW_OK) {
      const char* err = stream->get_last_error != nullptr ? stream->get_last_error(stream) : nullptr;
      return PJ::unexpected(fmt::format("Failed to read next batch: {}", err != nullptr ? err : "unknown"));
    }
    if (batch->release == nullptr) {
      break;  // end of stream
    }

    const int64_t num_rows = batch->length;
    if (num_rows == 0) {
      continue;
    }

    rc = ArrowArrayViewSetArray(array_view.get(), batch.get(), nullptr);
    if (rc != NANOARROW_OK) {
      return PJ::unexpected("Failed to set array on ArrowArrayView");
    }

    std::vector<Timestamp> timestamps;
    if (timestamp_column >= 0) {
      if (timestamp_column >= static_cast<int>(array_view->n_children)) {
        return PJ::unexpected(
            fmt::format("timestamp_column {} out of range ({} children)", timestamp_column, array_view->n_children));
      }
      auto timestamps_or = extractTimestampsNanoarrow(array_view->children[timestamp_column], num_rows, timestamp_unit);
      if (!timestamps_or.has_value()) {
        return unexpected(timestamps_or.error());
      }
      timestamps = std::move(*timestamps_or);
    } else {
      timestamps = generateSequentialTimestamps(synthetic_first_row, num_rows);
      synthetic_first_row += num_rows;
    }

    std::vector<ColumnDataWithBuffer> col_buffers;
    col_buffers.reserve(mappings.size());
    for (const auto& mapping : mappings) {
      if (mapping.arrow_column_index >= static_cast<int>(array_view->n_children)) {
        return PJ::unexpected(fmt::format("Arrow column index {} out of range", mapping.arrow_column_index));
      }
      auto column_or = makeColumnDataNanoarrow(array_view->children[mapping.arrow_column_index], mapping, num_rows);
      if (!column_or.has_value()) {
        return PJ::unexpected(column_or.error());
      }
      col_buffers.push_back(std::move(*column_or));
    }

    std::vector<ColumnData> col_data_vec;
    col_data_vec.reserve(col_buffers.size());
    for (auto& cb : col_buffers) {
      col_data_vec.push_back(cb.col_data);
    }

    auto status = writer.appendColumns(topic_id, timestamps, col_data_vec);
    if (!status.has_value()) {
      return status;
    }
  }

  return PJ::okStatus();
}

}  // namespace

// ---------------------------------------------------------------------------
// schemaFromIpc
// ---------------------------------------------------------------------------

PJ::Expected<std::pair<std::shared_ptr<PJ::TypeTreeNode>, std::vector<ArrowColumnMapping>>> schemaFromIpc(
    PJ::Span<const uint8_t> ipc_stream) {
  ArrowIpcInputStream input;
  initSpanInputStream(&input, ipc_stream);

  nanoarrow::UniqueArrayStream stream;
  int rc = ArrowIpcArrayStreamReaderInit(stream.get(), &input, nullptr);
  if (rc != NANOARROW_OK) {
    return PJ::unexpected("Failed to initialize IPC stream reader");
  }

  nanoarrow::UniqueSchema schema;
  rc = stream->get_schema(stream.get(), schema.get());
  if (rc != NANOARROW_OK) {
    return PJ::unexpected("Failed to read schema from IPC stream");
  }

  return mappingsFromSchema(schema.get());
}

// ---------------------------------------------------------------------------
// columnIndexByName
// ---------------------------------------------------------------------------

int columnIndexByName(ArrowArrayStream* stream, std::string_view name) {
  if (stream == nullptr || stream->get_schema == nullptr || name.empty()) {
    return -1;
  }
  nanoarrow::UniqueSchema schema;
  if (stream->get_schema(stream, schema.get()) != NANOARROW_OK) {
    return -1;
  }
  for (int64_t i = 0; i < schema->n_children; ++i) {
    if (schema->children[i]->name != nullptr && name == schema->children[i]->name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// schemaFromArrowStream
// ---------------------------------------------------------------------------

PJ::Expected<std::pair<std::shared_ptr<PJ::TypeTreeNode>, std::vector<ArrowColumnMapping>>> schemaFromArrowStream(
    ArrowArrayStream* stream) {
  if (stream == nullptr || stream->get_schema == nullptr) {
    return PJ::unexpected("null ArrowArrayStream or missing get_schema");
  }

  nanoarrow::UniqueSchema schema;
  const int rc = stream->get_schema(stream, schema.get());
  if (rc != NANOARROW_OK) {
    const char* err = stream->get_last_error != nullptr ? stream->get_last_error(stream) : nullptr;
    return PJ::unexpected(fmt::format("Failed to read schema from ArrowArrayStream: {}", err != nullptr ? err : ""));
  }

  return mappingsFromSchema(schema.get());
}

// ---------------------------------------------------------------------------
// importIpcStream
// ---------------------------------------------------------------------------

PJ::Status importIpcStream(
    DataWriter& writer, TopicId topic_id, PJ::Span<const uint8_t> ipc_stream,
    const std::vector<ArrowColumnMapping>& mappings, int timestamp_column) {
  ArrowIpcInputStream input;
  initSpanInputStream(&input, ipc_stream);

  nanoarrow::UniqueArrayStream stream;
  int rc = ArrowIpcArrayStreamReaderInit(stream.get(), &input, nullptr);
  if (rc != NANOARROW_OK) {
    return PJ::unexpected("Failed to initialize IPC stream reader");
  }

  nanoarrow::UniqueSchema schema;
  rc = stream->get_schema(stream.get(), schema.get());
  if (rc != NANOARROW_OK) {
    return PJ::unexpected("Failed to read schema from IPC stream");
  }

  return ingestBatchesFromStream(writer, topic_id, stream.get(), schema.get(), mappings, timestamp_column);
}

// ---------------------------------------------------------------------------
// importArrowStream  (v4 Arrow C Data Interface path)
// ---------------------------------------------------------------------------

PJ::Status importArrowStream(
    DataWriter& writer, TopicId topic_id, ArrowArrayStream* stream, const std::vector<ArrowColumnMapping>& mappings,
    int timestamp_column) {
  if (stream == nullptr || stream->get_schema == nullptr || stream->get_next == nullptr) {
    return PJ::unexpected("null ArrowArrayStream or missing callbacks");
  }

  nanoarrow::UniqueSchema schema;
  int rc = stream->get_schema(stream, schema.get());
  if (rc != NANOARROW_OK) {
    const char* err = stream->get_last_error != nullptr ? stream->get_last_error(stream) : nullptr;
    return PJ::unexpected(fmt::format("Failed to read schema from ArrowArrayStream: {}", err != nullptr ? err : ""));
  }

  return ingestBatchesFromStream(writer, topic_id, stream, schema.get(), mappings, timestamp_column);
}

}  // namespace PJ::arrow_import
