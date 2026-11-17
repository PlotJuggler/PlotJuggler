// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/plugin_data_host.hpp"

#include <fmt/format.h>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow.hpp"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/dataset.hpp"
#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/object_topic_metadata.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_datastore/arrow_import.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/encoding.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

using DataSourceHandle = PJ_data_source_handle_t;
using TopicHandle = PJ_topic_handle_t;
using FieldHandle = PJ_field_handle_t;

[[nodiscard]] std::string_view toStringView(PJ_string_view_t view) {
  return std::string_view(view.data == nullptr ? "" : view.data, view.size);
}

// The write hosts collapse "//" to "/" in topic and field names, so the stored key, the catalog and the by-name
// transform resolver agree. Callers check needsSlashNormalization() first, so the common name allocates nothing.
[[nodiscard]] bool needsSlashNormalization(std::string_view name) {
  return name.find("//") != std::string_view::npos;
}

// A field's leading '/' would double up at the "topic/field" junction.
[[nodiscard]] bool needsFieldNormalization(std::string_view name) {
  return needsSlashNormalization(name) || (!name.empty() && name.front() == '/');
}

[[nodiscard]] std::string normalizeSlashes(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  bool prev_slash = false;
  for (const char c : name) {
    if (c == '/' && prev_slash) {
      continue;
    }
    prev_slash = (c == '/');
    out.push_back(c);
  }
  return out;
}

[[nodiscard]] Expected<PrimitiveType> fromAbiType(PJ_primitive_type_t type) {
  const auto raw = static_cast<uint32_t>(type);
  if (raw > static_cast<uint32_t>(PrimitiveType::kString)) {
    return unexpected(fmt::format("unsupported primitive type value {}", raw));
  }
  return static_cast<PrimitiveType>(type);
}

std::atomic<IngestWarningHandler> g_ingest_warning_handler{nullptr};

[[nodiscard]] std::string_view primitiveTypeName(PrimitiveType type) {
  static constexpr std::array<std::string_view, 12> kNames = {
      "float32", "float64", "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "bool", "string"};
  const auto index = static_cast<std::size_t>(type);
  return index < kNames.size() ? kNames[index] : "unspecified";
}

// A numeric scalar in the widest carrier of its kind; every source value is exact in it.
using ExactNumber = std::variant<int64_t, uint64_t, double>;

[[nodiscard]] std::optional<ExactNumber> exactNumber(const PJ_scalar_value_t& value) {
  switch (static_cast<PrimitiveType>(value.type)) {
    case PrimitiveType::kFloat32:
      return double{value.data.as_float32};
    case PrimitiveType::kFloat64:
      return value.data.as_float64;
    case PrimitiveType::kInt8:
      return int64_t{value.data.as_int8};
    case PrimitiveType::kInt16:
      return int64_t{value.data.as_int16};
    case PrimitiveType::kInt32:
      return int64_t{value.data.as_int32};
    case PrimitiveType::kInt64:
      return value.data.as_int64;
    case PrimitiveType::kUint8:
      return uint64_t{value.data.as_uint8};
    case PrimitiveType::kUint16:
      return uint64_t{value.data.as_uint16};
    case PrimitiveType::kUint32:
      return uint64_t{value.data.as_uint32};
    case PrimitiveType::kUint64:
      return value.data.as_uint64;
    default:
      return std::nullopt;  // bool and string are never converted
  }
}

// `number` as T, or nullopt unless it converts exactly (no wrap, truncation or rounding).
template <typename T>
[[nodiscard]] std::optional<T> exactAs(const ExactNumber& number) {
  return std::visit(
      [](auto x) -> std::optional<T> {
        using S = decltype(x);
        if constexpr (std::is_integral_v<T> && std::is_integral_v<S>) {
          return std::in_range<T>(x) ? std::optional<T>(static_cast<T>(x)) : std::nullopt;
        } else if constexpr (std::is_integral_v<T>) {
          // hi = 2^digits (= max + 1), exact in double for every integer width.
          constexpr double hi = static_cast<double>(std::numeric_limits<T>::max() / 2 + 1) * 2.0;
          constexpr double lo = std::is_signed_v<T> ? -hi : 0.0;
          const bool ok = x >= lo && x < hi && x == std::trunc(x);  // false for NaN
          return ok ? std::optional<T>(static_cast<T>(x)) : std::nullopt;
        } else if constexpr (std::is_integral_v<S>) {
          const T rounded = static_cast<T>(x);
          const auto back = exactAs<S>(ExactNumber{static_cast<double>(rounded)});
          return back == x ? std::optional<T>(rounded) : std::nullopt;
        } else if constexpr (std::is_same_v<T, double>) {
          return x;
        } else {
          if (std::isnan(x) || std::isinf(x)) {
            return static_cast<T>(x);
          }
          if (std::fabs(x) > static_cast<double>(std::numeric_limits<T>::max())) {
            return std::nullopt;
          }
          const T narrowed = static_cast<T>(x);
          return static_cast<double>(narrowed) == x ? std::optional<T>(narrowed) : std::nullopt;
        }
      },
      number);
}

// `value` re-encoded as `target` when it converts exactly; nullopt otherwise
// (lossy numeric conversions, and anything to/from bool or string).
[[nodiscard]] std::optional<PJ_scalar_value_t> coerceExact(const PJ_scalar_value_t& value, PrimitiveType target) {
  const auto number = exactNumber(value);
  if (!number.has_value()) {
    return std::nullopt;
  }
  PJ_scalar_value_t out{};
  out.type = static_cast<PJ_primitive_type_t>(target);
  auto store = [&]<typename T>(T& slot) {
    const auto converted = exactAs<T>(*number);
    if (converted.has_value()) {
      slot = *converted;
    }
    return converted.has_value();
  };
  bool ok = false;
  switch (target) {
    case PrimitiveType::kFloat32:
      ok = store(out.data.as_float32);
      break;
    case PrimitiveType::kFloat64:
      ok = store(out.data.as_float64);
      break;
    case PrimitiveType::kInt8:
      ok = store(out.data.as_int8);
      break;
    case PrimitiveType::kInt16:
      ok = store(out.data.as_int16);
      break;
    case PrimitiveType::kInt32:
      ok = store(out.data.as_int32);
      break;
    case PrimitiveType::kInt64:
      ok = store(out.data.as_int64);
      break;
    case PrimitiveType::kUint8:
      ok = store(out.data.as_uint8);
      break;
    case PrimitiveType::kUint16:
      ok = store(out.data.as_uint16);
      break;
    case PrimitiveType::kUint32:
      ok = store(out.data.as_uint32);
      break;
    case PrimitiveType::kUint64:
      ok = store(out.data.as_uint64);
      break;
    default:
      break;
  }
  return ok ? std::optional<PJ_scalar_value_t>(out) : std::nullopt;
}

template <typename T>
[[nodiscard]] T loadFromBytes(const uint8_t* data) {
  T value{};
  std::memcpy(&value, data, sizeof(T));
  return value;
}

[[nodiscard]] uint64_t readForOffset(const encoding::FrameOfReferenceEncoded& enc, std::size_t row) {
  const uint8_t* data = enc.offsets.data();
  switch (enc.offset_bytes) {
    case 1:
      return loadFromBytes<uint8_t>(data + row);
    case 2:
      return loadFromBytes<uint16_t>(data + row * 2);
    default:
      return loadFromBytes<uint32_t>(data + row * 4);
  }
}

template <typename T>
[[nodiscard]] T decodeNumericExact(const TopicChunk& chunk, std::size_t col_index, std::size_t row) {
  switch (chunk.columnEncoding(col_index)) {
    case EncodingType::kConstant: {
      const auto& enc = std::get<encoding::ConstantEncoded>(chunk.columns[col_index].data);
      return loadFromBytes<T>(enc.value_bytes.data());
    }
    case EncodingType::kFrameOfReference: {
      const auto& enc = std::get<encoding::FrameOfReferenceEncoded>(chunk.columns[col_index].data);
      const uint64_t offset = readForOffset(enc, row);
      return static_cast<T>(enc.reference + static_cast<int64_t>(offset));
    }
    case EncodingType::kRaw: {
      const StorageKind kind = storageKindOf(chunk.columns[col_index].descriptor->logical_type);
      const uint8_t* base = std::get<RawBuffer>(chunk.columns[col_index].data).data();
      switch (kind) {
        case StorageKind::kFloat32:
          return static_cast<T>(loadFromBytes<float>(base + row * sizeof(float)));
        case StorageKind::kFloat64:
          return static_cast<T>(loadFromBytes<double>(base + row * sizeof(double)));
        case StorageKind::kInt32:
          return static_cast<T>(loadFromBytes<int32_t>(base + row * sizeof(int32_t)));
        case StorageKind::kInt64:
          return static_cast<T>(loadFromBytes<int64_t>(base + row * sizeof(int64_t)));
        case StorageKind::kUint64:
          return static_cast<T>(loadFromBytes<uint64_t>(base + row * sizeof(uint64_t)));
        case StorageKind::kBool:
          return static_cast<T>(chunk.readBool(col_index, row));
        case StorageKind::kString:
          return T{};
      }
      return T{};
    }
    default:
      return T{};
  }
}

void flattenColumnsImpl(
    const TypeTreeNode& node, std::string_view prefix, FieldId& next_id, std::vector<ColumnDescriptor>& out) {
  std::string path = prefix.empty() ? node.name : fmt::format("{}.{}", prefix, node.name);
  switch (node.kind) {
    case TypeKind::kPrimitive: {
      ColumnDescriptor desc;
      desc.field_id = next_id++;
      desc.logical_type = node.primitive_type.value_or(PrimitiveType::kFloat64);
      desc.field_path = std::move(path);
      out.push_back(std::move(desc));
      return;
    }
    case TypeKind::kEnum: {
      ColumnDescriptor desc;
      desc.field_id = next_id++;
      desc.logical_type = node.primitive_type.value_or(PrimitiveType::kInt32);
      desc.field_path = std::move(path);
      out.push_back(std::move(desc));
      return;
    }
    case TypeKind::kStruct:
      for (const auto& child : node.children) {
        flattenColumnsImpl(*child, path, next_id, out);
      }
      return;
    case TypeKind::kArray:
      return;
  }
}

[[nodiscard]] std::vector<ColumnDescriptor> buildSchemaColumns(const TypeTreeNode& root) {
  std::vector<ColumnDescriptor> result;
  FieldId next_id = 0;
  if (root.kind == TypeKind::kStruct) {
    for (const auto& child : root.children) {
      flattenColumnsImpl(*child, "", next_id, result);
    }
  } else {
    flattenColumnsImpl(root, "", next_id, result);
  }
  return result;
}

[[nodiscard]] std::vector<ColumnDescriptor> effectiveColumns(const DataEngine& engine, const TopicStorage& storage) {
  const auto& stored = storage.columnDescriptors();
  if (!stored.empty()) {
    return stored;
  }
  if (const auto* type_tree = engine.typeRegistry().lookup(storage.descriptor().schema_id)) {
    return buildSchemaColumns(*type_tree);
  }
  const auto& chunks = storage.sealedChunks();
  if (!chunks.empty()) {
    std::vector<ColumnDescriptor> result;
    result.reserve(chunks.front().columns.size());
    for (const auto& col : chunks.front().columns) {
      result.push_back(*col.descriptor);
    }
    return result;
  }
  return {};
}

[[nodiscard]] const ColumnDescriptor* findFieldDescriptor(
    const std::vector<ColumnDescriptor>& columns, FieldId field_id) {
  for (const auto& col : columns) {
    if (col.field_id == field_id) {
      return &col;
    }
  }
  return nullptr;
}

/// The Arrow type a column of `type` reads back as through read_series_arrow.
[[nodiscard]] ArrowType arrowTypeOf(PrimitiveType type) {
  switch (type) {
    case PrimitiveType::kFloat32:
      return NANOARROW_TYPE_FLOAT;
    case PrimitiveType::kFloat64:
      return NANOARROW_TYPE_DOUBLE;
    case PrimitiveType::kInt8:
      return NANOARROW_TYPE_INT8;
    case PrimitiveType::kInt16:
      return NANOARROW_TYPE_INT16;
    case PrimitiveType::kInt32:
      return NANOARROW_TYPE_INT32;
    case PrimitiveType::kInt64:
      return NANOARROW_TYPE_INT64;
    case PrimitiveType::kUint8:
      return NANOARROW_TYPE_UINT8;
    case PrimitiveType::kUint16:
      return NANOARROW_TYPE_UINT16;
    case PrimitiveType::kUint32:
      return NANOARROW_TYPE_UINT32;
    case PrimitiveType::kUint64:
      return NANOARROW_TYPE_UINT64;
    case PrimitiveType::kBool:
      return NANOARROW_TYPE_BOOL;
    case PrimitiveType::kString:
      return NANOARROW_TYPE_STRING;
    case PrimitiveType::kUnspecified:
      return NANOARROW_TYPE_NA;
  }
  return NANOARROW_TYPE_NA;
}

/// The index of `field` among `chunk`'s columns; none for a chunk sealed before the field appeared.
[[nodiscard]] std::optional<std::size_t> columnIndexOf(const TopicChunk& chunk, FieldId field) {
  for (std::size_t i = 0; i < chunk.columns.size(); ++i) {
    if (chunk.columns[i].descriptor->field_id == field) {
      return i;
    }
  }
  return std::nullopt;
}

/// `fn(T{})` with the C++ type a numeric `type` is stored as; none for bool, string and unspecified.
template <typename Fn>
[[nodiscard]] std::optional<bool> visitNumeric(PrimitiveType type, Fn&& fn) {
  switch (type) {
    case PrimitiveType::kFloat32:
      return fn(float{});
    case PrimitiveType::kFloat64:
      return fn(double{});
    case PrimitiveType::kInt8:
      return fn(int8_t{});
    case PrimitiveType::kInt16:
      return fn(int16_t{});
    case PrimitiveType::kInt32:
      return fn(int32_t{});
    case PrimitiveType::kInt64:
      return fn(int64_t{});
    case PrimitiveType::kUint8:
      return fn(uint8_t{});
    case PrimitiveType::kUint16:
      return fn(uint16_t{});
    case PrimitiveType::kUint32:
      return fn(uint32_t{});
    case PrimitiveType::kUint64:
      return fn(uint64_t{});
    case PrimitiveType::kBool:
    case PrimitiveType::kString:
    case PrimitiveType::kUnspecified:
      break;
  }
  return std::nullopt;
}

template <typename T>
[[nodiscard]] ArrowErrorCode appendValue(ArrowArray* child, T value) {
  if constexpr (std::is_floating_point_v<T>) {
    return ArrowArrayAppendDouble(child, value);
  } else if constexpr (std::is_signed_v<T>) {
    return ArrowArrayAppendInt(child, value);
  } else {
    return ArrowArrayAppendUInt(child, value);
  }
}

/// Appends one row to the [timestamp, value] struct being built, as its own element: any type, nulls included.
[[nodiscard]] bool appendRow(
    ArrowArray* array, const TopicChunk& chunk, std::size_t col_index, std::size_t row, PrimitiveType type) {
  ArrowArray* val_child = array->children[1];
  if (ArrowArrayAppendInt(array->children[0], chunk.timestamps[row]) != NANOARROW_OK) {
    return false;
  }
  ArrowErrorCode rc = NANOARROW_OK;
  if (chunk.isNull(col_index, row)) {
    rc = ArrowArrayAppendNull(val_child, 1);
  } else if (const auto appended = visitNumeric(type, [&](auto tag) {
               return appendValue(val_child, decodeNumericExact<decltype(tag)>(chunk, col_index, row)) == NANOARROW_OK;
             })) {
    rc = *appended ? NANOARROW_OK : EINVAL;
  } else if (type == PrimitiveType::kBool) {
    rc = ArrowArrayAppendInt(val_child, chunk.readBool(col_index, row) ? 1 : 0);
  } else if (type == PrimitiveType::kString) {
    const auto text = chunk.readString(col_index, row);
    rc = ArrowArrayAppendString(val_child, ArrowStringView{text.data(), static_cast<int64_t>(text.size())});
  } else {
    rc = ArrowArrayAppendNull(val_child, 1);
  }
  return rc == NANOARROW_OK && ArrowArrayFinishElement(array) == NANOARROW_OK;
}

/// appendRow() over rows [begin, end) of a numeric column without nulls, as one block, for whole-series toolbox reads.
template <typename T>
[[nodiscard]] bool appendNumericRun(
    ArrowArray* array, const TopicChunk& chunk, std::size_t col, std::size_t begin, std::size_t end) {
  const std::size_t n = end - begin;
  const auto start = static_cast<std::size_t>(array->length);
  if (ArrowArrayAppendEmpty(array, static_cast<int64_t>(n)) != NANOARROW_OK) {
    return false;
  }
  std::memcpy(
      reinterpret_cast<int64_t*>(ArrowArrayBuffer(array->children[0], 1)->data) + start,
      chunk.timestamps.data() + begin, n * sizeof(Timestamp));
  T* out = reinterpret_cast<T*>(ArrowArrayBuffer(array->children[1], 1)->data) + start;
  if (chunk.columnEncoding(col) == EncodingType::kConstant) {
    std::fill_n(out, n, decodeNumericExact<T>(chunk, col, begin));
  } else {
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = decodeNumericExact<T>(chunk, col, begin + i);
    }
  }
  return true;
}

/// Appends rows [begin, end) of `chunk`: as one block when the column is numeric without nulls in the chunk, row by
/// row otherwise.
[[nodiscard]] bool appendChunkRun(
    ArrowArray* array, const TopicChunk& chunk, std::size_t col, std::size_t begin, std::size_t end,
    PrimitiveType type) {
  if (!chunk.columns[col].validity_bitmap.has_value()) {
    if (const auto appended = visitNumeric(
            type, [&](auto tag) { return appendNumericRun<decltype(tag)>(array, chunk, col, begin, end); })) {
      return *appended;
    }
  }
  for (std::size_t row = begin; row < end; ++row) {
    if (!appendRow(array, chunk, col, row, type)) {
      return false;
    }
  }
  return true;
}

/// A field's storage and column descriptor, looked up under the caller's engine lock.
struct FieldColumn {
  const TopicStorage* storage = nullptr;
  ColumnDescriptor desc;
};

[[nodiscard]] Expected<FieldColumn> lookupFieldColumn(const DataEngine& engine, PJ_field_handle_t field) {
  const auto* storage = engine.getTopicStorage(field.topic.id);
  if (storage == nullptr) {
    return unexpected(fmt::format("topic {} not found", field.topic.id));
  }
  const auto columns = effectiveColumns(engine, *storage);
  const auto* desc = findFieldDescriptor(columns, field.id);
  if (desc == nullptr) {
    return unexpected(fmt::format("field {} not found in topic {}", field.id, field.topic.id));
  }
  return FieldColumn{storage, *desc};
}

/// `fn(chunk, column, begin, end)` over the rows of `field` stamped in [t_min, t_max], in time order, a run of one
/// chunk at a time (RangeCursor::forEachOrderedRun). Chunks sealed before the field appeared hold none of its rows.
/// Stops at the first `fn` returning false, and returns false then.
template <typename Fn>
[[nodiscard]] bool forEachFieldRun(
    const std::deque<TopicChunk>& chunks, FieldId field, Timestamp t_min, Timestamp t_max, Fn&& fn) {
  const TopicChunk* column_chunk = nullptr;
  std::optional<std::size_t> col;
  bool ok = true;
  RangeCursor(chunks, t_min, t_max).forEachOrderedRun([&](const ChunkRowRange& run) {
    if (run.chunk != column_chunk) {
      column_chunk = run.chunk;
      col = columnIndexOf(*column_chunk, field);
    }
    if (ok && col) {
      ok = fn(*run.chunk, *col, run.row_start, run.row_end);
    }
  });
  return ok;
}

}  // namespace

Status readSeriesArrowWindow(
    const DataEngine& engine, PJ_field_handle_t field, Timestamp first, Timestamp last, ArrowSchema* out_schema,
    ArrowArray* out_array) {
  if (out_schema == nullptr || out_array == nullptr) {
    return unexpected(std::string("readSeriesArrow: out_schema and out_array must be non-null"));
  }
  engine.awaitCompleteSeries(field.topic.id);  // a snapshot must not catch a derived series mid-replay
  auto lock = engine.lockEngine();

  auto column = lookupFieldColumn(engine, field);
  if (!column) {
    return unexpected(column.error());
  }
  const TopicStorage* storage = column->storage;
  const PrimitiveType type = column->desc.logical_type;

  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  if (ArrowSchemaSetTypeStruct(schema.get(), 2) != NANOARROW_OK) {
    return unexpected(std::string("readSeriesArrow: ArrowSchemaSetTypeStruct failed"));
  }
  ArrowSchemaInit(schema->children[0]);
  if (ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64) != NANOARROW_OK ||
      ArrowSchemaSetName(schema->children[0], "timestamp") != NANOARROW_OK) {
    return unexpected(std::string("readSeriesArrow: failed to set timestamp child schema"));
  }
  ArrowSchemaInit(schema->children[1]);
  if (ArrowSchemaSetType(schema->children[1], arrowTypeOf(type)) != NANOARROW_OK ||
      ArrowSchemaSetName(schema->children[1], column->desc.field_path.c_str()) != NANOARROW_OK) {
    return unexpected(std::string("readSeriesArrow: failed to set value child schema"));
  }
  nanoarrow::UniqueArray array;
  ArrowError arrow_err;
  if (ArrowArrayInitFromSchema(array.get(), schema.get(), &arrow_err) != NANOARROW_OK) {
    return unexpected(std::string("readSeriesArrow: ArrowArrayInitFromSchema failed: ") + arrow_err.message);
  }
  if (ArrowArrayStartAppending(array.get()) != NANOARROW_OK) {
    return unexpected(std::string("readSeriesArrow: ArrowArrayStartAppending failed"));
  }

  // Rows under the retention floor are evicted: a reader must not see them.
  const auto& chunks = storage->sealedChunks();
  const Timestamp floor = storage->retentionFloor();
  const Timestamp t_min = std::max(first, floor);
  if (t_min > std::numeric_limits<Timestamp>::min()) {
    const auto before = latestAt(chunks, t_min - 1, floor, [&field](const TopicChunk& chunk) {
      return columnIndexOf(chunk, field.id).has_value();
    });
    if (before &&
        !appendRow(array.get(), *before->chunk, *columnIndexOf(*before->chunk, field.id), before->row_index, type)) {
      return unexpected(std::string("readSeriesArrow: append failed"));
    }
  }
  if (!forEachFieldRun(
          chunks, field.id, t_min, last,
          [&](const TopicChunk& chunk, std::size_t col, std::size_t begin, std::size_t end) {
            return appendChunkRun(array.get(), chunk, col, begin, end, type);
          })) {
    return unexpected(std::string("readSeriesArrow: append failed"));
  }

  if (ArrowArrayFinishBuildingDefault(array.get(), &arrow_err) != NANOARROW_OK) {
    return unexpected(std::string("readSeriesArrow: finish building failed: ") + arrow_err.message);
  }
  ArrowSchemaMove(schema.get(), out_schema);
  ArrowArrayMove(array.get(), out_array);
  return okStatus();
}

Status scanSeriesValues(
    const DataEngine& engine, PJ_field_handle_t field, const SampleBlockCallback& on_block, Timestamp first) {
  engine.awaitCompleteSeries(field.topic.id);
  auto lock = engine.lockEngine();
  auto column = lookupFieldColumn(engine, field);
  if (!column) {
    return unexpected(column.error());
  }
  if (!visitNumeric(column->desc.logical_type, [](auto) { return true; })) {
    return okStatus();
  }
  std::vector<double> values;
  (void)forEachFieldRun(
      column->storage->sealedChunks(), field.id, std::max(first, column->storage->retentionFloor()),
      std::numeric_limits<Timestamp>::max(),
      [&](const TopicChunk& chunk, std::size_t col, std::size_t begin, std::size_t end) {
        const Span<const Timestamp> ts(chunk.timestamps.data() + begin, end - begin);
        const bool constant =
            chunk.columnEncoding(col) == EncodingType::kConstant && !chunk.columns[col].validity_bitmap.has_value();
        values.resize(constant ? 1 : ts.size());
        chunk.readColumnAsDoubles(col, Span<double>(values), begin);
        on_block(ts, values);
        return true;
      });
  return okStatus();
}

// Engine locks for a write-host operation (the worker writes while the GUI reads). With a staging engine armed it takes
// both with std::lock, as flushTo() and the parser-binding route do, so they cannot deadlock. Recursive, so nested
// calls such as ensureField() in appendRecord() re-acquire.
[[nodiscard]] inline EngineLockPair lockWriteEngines(DataEngine& live, DataEngine* staging) {
  return lockEnginePair(live, staging);
}

struct WriteCore {
  explicit WriteCore(DataEngine& data_engine) : engine(data_engine), writer(data_engine.createWriter()) {}

  DataEngine& engine;
  DataWriter writer;
  // The pause/resume staging engine, or null. Every topic and field ensureTopic() and ensureField() land is created
  // there with the same id (appendRecord() and appendArrowStream() create columns through ensureField() too), so a
  // plugin's cached handles still resolve after setTarget() swaps engines.
  DataEngine* secondary_engine = nullptr;
  std::string last_error;

  struct DatasetTopicKey {
    DatasetId dataset_id;
    std::string topic_name;

    friend bool operator==(const DatasetTopicKey& a, const DatasetTopicKey& b) {
      return a.dataset_id == b.dataset_id && a.topic_name == b.topic_name;
    }
  };

  struct DatasetTopicKeyHash {
    std::size_t operator()(const DatasetTopicKey& key) const noexcept {
      std::size_t h1 = std::hash<DatasetId>{}(key.dataset_id);
      std::size_t h2 = std::hash<std::string>{}(key.topic_name);
      return h1 ^ (h2 << 1);
    }
  };

  struct TopicFieldKey {
    TopicId topic_id;
    std::string field_name;

    friend bool operator==(const TopicFieldKey& a, const TopicFieldKey& b) {
      return a.topic_id == b.topic_id && a.field_name == b.field_name;
    }
  };

  // Borrowed key: the per-record hot path probes field_cache without allocating a std::string per field.
  struct TopicFieldKeyView {
    TopicId topic_id;
    std::string_view field_name;
  };

  struct TopicFieldKeyHash {
    using is_transparent = void;
    std::size_t operator()(const TopicFieldKey& key) const noexcept {
      return hash(key.topic_id, key.field_name);
    }
    std::size_t operator()(const TopicFieldKeyView& key) const noexcept {
      return hash(key.topic_id, key.field_name);
    }

   private:
    // Both overloads hash the name as a string_view, so an owning key and an equal view hash alike.
    static std::size_t hash(TopicId topic_id, std::string_view field_name) noexcept {
      std::size_t h1 = std::hash<TopicId>{}(topic_id);
      std::size_t h2 = std::hash<std::string_view>{}(field_name);
      return h1 ^ (h2 << 1);
    }
  };

  struct TopicFieldKeyEq {
    using is_transparent = void;
    template <typename A, typename B>
    bool operator()(const A& a, const B& b) const noexcept {
      return a.topic_id == b.topic_id && a.field_name == b.field_name;
    }
  };

  struct TopicFieldIdKey {
    TopicId topic_id;
    FieldId field_id;

    friend bool operator==(const TopicFieldIdKey& a, const TopicFieldIdKey& b) {
      return a.topic_id == b.topic_id && a.field_id == b.field_id;
    }
  };

  // Probed per field per appended record. std::hash of an integer is the identity (libstdc++, libc++) and robin_map
  // masks the low bits, so the small topic and field ids are mixed first.
  struct TopicFieldIdKeyHash {
    std::size_t operator()(const TopicFieldIdKey& key) const noexcept {
      uint64_t mixed = (static_cast<uint64_t>(key.topic_id) << 32) | key.field_id;
      mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;  // splitmix64 finalizer
      mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
      return static_cast<std::size_t>(mixed ^ (mixed >> 31));
    }
  };

  tsl::robin_map<DatasetTopicKey, TopicHandle, DatasetTopicKeyHash> topic_cache;
  tsl::robin_map<TopicFieldKey, FieldHandle, TopicFieldKeyHash, TopicFieldKeyEq> field_cache;
  tsl::robin_map<TopicFieldIdKey, PrimitiveType, TopicFieldIdKeyHash> field_types;
  // Fields already reported through the ingest-warning handler (once per field).
  tsl::robin_set<TopicFieldIdKey, TopicFieldIdKeyHash> warned_fields;

  void setError(std::string message) {
    last_error = std::move(message);
  }

  [[nodiscard]] const char* lastError() const {
    return last_error.empty() ? nullptr : last_error.c_str();
  }

  [[nodiscard]] bool createDataSource(std::string_view name, DataSourceHandle* out_source) {
    auto engine_locks = lockWriteEngines(engine, secondary_engine);
    // Each source gets its own time domain, which holds its display offset; the default domain (id 0) cannot carry
    // one.
    auto td_or = engine.createTimeDomain(std::string(name));
    if (!td_or.has_value()) {
      setError(td_or.error());
      return false;
    }
    // Mirrored before the dataset exists, so a failed mirror creates no dataset.
    if (secondary_engine != nullptr) {
      auto mirror_or = secondary_engine->createTimeDomain(std::string(name), *td_or);
      if (!mirror_or.has_value()) {
        setError(fmt::format("secondary mirror createTimeDomain failed: {}", mirror_or.error()));
        return false;
      }
    }
    auto id_or = engine.createDataset(DatasetDescriptor{.source_name = std::string(name), .time_domain_id = *td_or});
    if (!id_or.has_value()) {
      setError(id_or.error());
      return false;
    }
    *out_source = DataSourceHandle{.id = *id_or};
    last_error.clear();
    return true;
  }

  // Runs on every successful ensureTopic() path, cache hits included, so a failed mirror is retried.
  [[nodiscard]] bool mirrorTopicToSecondary(DatasetId dataset_id, TopicId topic_id, std::string_view topic_name) {
    if (secondary_engine == nullptr) {
      return true;
    }
    if (secondary_engine->getTopicStorage(topic_id) != nullptr) {
      return true;  // already mirrored
    }
    TopicDescriptor desc;
    desc.name = std::string(topic_name);
    desc.schema_id = 0;
    auto mirror_or = secondary_engine->createTopic(dataset_id, std::move(desc), topic_id);
    if (!mirror_or.has_value()) {
      setError(fmt::format("secondary mirror createTopic failed: {}", mirror_or.error()));
      return false;
    }
    return true;
  }

  [[nodiscard]] bool ensureTopic(DataSourceHandle source, std::string_view topic_name, TopicHandle* out_topic) {
    std::string topic_name_normalized;
    if (needsSlashNormalization(topic_name)) {
      topic_name_normalized = normalizeSlashes(topic_name);
      topic_name = topic_name_normalized;
    }
    auto engine_locks = lockWriteEngines(engine, secondary_engine);
    const auto* dataset = engine.getDataset(source.id);
    if (dataset == nullptr) {
      setError(fmt::format("data source {} not found", source.id));
      return false;
    }

    DatasetTopicKey key{.dataset_id = source.id, .topic_name = std::string(topic_name)};
    if (auto it = topic_cache.find(key); it != topic_cache.end()) {
      if (!mirrorTopicToSecondary(source.id, it->second.id, topic_name)) {
        return false;
      }
      *out_topic = it->second;
      last_error.clear();
      return true;
    }

    auto topic_ids = engine.listTopics(source.id);
    std::sort(topic_ids.begin(), topic_ids.end());
    for (TopicId tid : topic_ids) {
      const auto* storage = engine.getTopicStorage(tid);
      if (storage != nullptr && storage->descriptor().name == topic_name) {
        if (!mirrorTopicToSecondary(source.id, tid, topic_name)) {
          return false;
        }
        *out_topic = TopicHandle{.id = tid};
        topic_cache.emplace(std::move(key), *out_topic);
        last_error.clear();
        return true;
      }
    }

    TopicDescriptor desc;
    desc.name = std::string(topic_name);
    desc.schema_id = 0;
    auto tid_or = writer.registerTopic(source.id, std::move(desc));
    if (!tid_or.has_value()) {
      setError(tid_or.error());
      return false;
    }

    if (!mirrorTopicToSecondary(source.id, *tid_or, topic_name)) {
      return false;
    }

    *out_topic = TopicHandle{.id = *tid_or};
    topic_cache.emplace(std::move(key), *out_topic);
    last_error.clear();
    return true;
  }

  [[nodiscard]] bool lookupFieldType(TopicHandle topic, FieldId field_id, PrimitiveType* out_type) {
    const TopicFieldIdKey key{.topic_id = topic.id, .field_id = field_id};
    if (auto it = field_types.find(key); it != field_types.end()) {
      *out_type = it->second;
      return true;
    }

    const auto column = lookupFieldColumn(engine, FieldHandle{.topic = topic, .id = field_id});
    if (!column) {
      setError(column.error());
      return false;
    }

    *out_type = column->desc.logical_type;
    field_types[key] = column->desc.logical_type;
    field_cache[{.topic_id = topic.id, .field_name = column->desc.field_path}] =
        FieldHandle{.topic = topic, .id = field_id};
    return true;
  }

  // With `out_column_type` null (the C-ABI register path) a type mismatch with an existing column is an error;
  // otherwise (appendRecord()'s lazy path) the existing column and its type come back, and the caller converts the
  // value.
  [[nodiscard]] bool ensureField(
      TopicHandle topic, std::string_view field_name, PJ_primitive_type_t abi_type, FieldHandle* out_field,
      PrimitiveType* out_column_type = nullptr) {
    // A field path is relative to its topic: a leading '/' (a CSV header such as "/robot/pose/x") is dropped, or the
    // joined "topic/field" name would double the slash.
    std::string field_name_normalized;
    if (needsFieldNormalization(field_name)) {
      field_name_normalized = normalizeSlashes(field_name);
      if (!field_name_normalized.empty() && field_name_normalized.front() == '/') {
        field_name_normalized.erase(0, 1);
      }
      field_name = field_name_normalized;
    }
    auto engine_locks = lockWriteEngines(engine, secondary_engine);
    const auto* storage = engine.getTopicStorage(topic.id);
    if (storage == nullptr) {
      setError(fmt::format("topic {} not found", topic.id));
      return false;
    }

    auto type_or = fromAbiType(abi_type);
    if (!type_or.has_value()) {
      setError(type_or.error());
      return false;
    }
    const PrimitiveType type = *type_or;

    if (auto it = field_cache.find(TopicFieldKeyView{.topic_id = topic.id, .field_name = field_name});
        it != field_cache.end()) {
      PrimitiveType existing{};
      if (!lookupFieldType(topic, it->second.id, &existing)) {
        return false;
      }
      if (existing != type && out_column_type == nullptr) {
        setError(fmt::format("field '{}' already exists with a different type", field_name));
        return false;
      }
      // Re-mirrored on a cache hit, so a failed mirror is retried; createTopicField() returns the existing FieldId when
      // name and type match.
      if (secondary_engine != nullptr) {
        auto mirror_or =
            secondary_engine->createTopicField(topic.id, field_name, existing, std::optional<FieldId>{it->second.id});
        if (!mirror_or.has_value()) {
          setError(fmt::format("secondary mirror createTopicField failed: {}", mirror_or.error()));
          return false;
        }
      }
      *out_field = it->second;
      if (out_column_type != nullptr) {
        *out_column_type = existing;
      }
      last_error.clear();
      return true;
    }

    // On a cache miss the column may exist with another type (a fresh core after a setTarget() swap, or another
    // writer's column): adopt its type, so ensureColumn() resolves it rather than rejecting the mismatch.
    PrimitiveType column_type = type;
    if (out_column_type != nullptr) {
      const auto& columns = storage->columnDescriptors();
      if (auto col = std::ranges::find(columns, field_name, &ColumnDescriptor::field_path); col != columns.end()) {
        column_type = col->logical_type;
      }
    }

    auto field_id_or = writer.ensureColumn(topic.id, field_name, column_type);
    if (!field_id_or.has_value()) {
      setError(field_id_or.error());
      return false;
    }

    // The requested id is an optional because FieldId 0, a topic's first field, must be forced too.
    if (secondary_engine != nullptr) {
      auto mirror_or =
          secondary_engine->createTopicField(topic.id, field_name, column_type, std::optional<FieldId>{*field_id_or});
      if (!mirror_or.has_value()) {
        setError(fmt::format("secondary mirror createTopicField failed: {}", mirror_or.error()));
        return false;
      }
    }

    *out_field = FieldHandle{.topic = topic, .id = *field_id_or};
    field_cache.emplace(TopicFieldKey{.topic_id = topic.id, .field_name = std::string(field_name)}, *out_field);
    field_types[{.topic_id = topic.id, .field_id = *field_id_or}] = column_type;
    if (out_column_type != nullptr) {
      *out_column_type = column_type;
    }
    last_error.clear();
    return true;
  }

  // A value of the wrong type for its column: converted when that is exact, else nullopt after reporting the field
  // once; the caller stores null for that field and keeps the rest of the record.
  [[nodiscard]] std::optional<PJ_scalar_value_t> convertForColumn(
      TopicId topic_id, FieldId field_id, const PJ_scalar_value_t& value, PrimitiveType column_type) {
    auto converted = coerceExact(value, column_type);
    if (converted.has_value() || !warned_fields.insert({.topic_id = topic_id, .field_id = field_id}).second) {
      return converted;
    }
    if (auto handler = g_ingest_warning_handler.load(std::memory_order_acquire)) {
      std::string_view topic_name = "?";
      std::string_view field_name = "?";
      if (const auto* storage = engine.getTopicStorage(topic_id)) {
        topic_name = storage->descriptor().name;
        const auto& columns = storage->columnDescriptors();
        if (auto col = std::ranges::find(columns, field_id, &ColumnDescriptor::field_id); col != columns.end()) {
          field_name = col->field_path;
        }
      }
      handler(
          fmt::format(
              "{}/{}: {} value does not fit the field's {} type exactly; stored as null (reported once per field)",
              topic_name, field_name, primitiveTypeName(static_cast<PrimitiveType>(value.type)),
              primitiveTypeName(column_type)));
    }
    return std::nullopt;
  }

  void setFieldValue(
      TopicId topic_id, std::size_t col_index, PrimitiveType logical_type, const PJ_scalar_value_t& value) {
    switch (logical_type) {
      case PrimitiveType::kFloat32:
        writer.set(topic_id, col_index, value.data.as_float32);
        break;
      case PrimitiveType::kFloat64:
        writer.set(topic_id, col_index, value.data.as_float64);
        break;
      case PrimitiveType::kInt8:
        writer.set(topic_id, col_index, static_cast<int64_t>(value.data.as_int8));
        break;
      case PrimitiveType::kInt16:
        writer.set(topic_id, col_index, static_cast<int64_t>(value.data.as_int16));
        break;
      case PrimitiveType::kInt32:
        writer.set(topic_id, col_index, value.data.as_int32);
        break;
      case PrimitiveType::kInt64:
        writer.set(topic_id, col_index, value.data.as_int64);
        break;
      case PrimitiveType::kUint8:
        writer.set(topic_id, col_index, static_cast<uint64_t>(value.data.as_uint8));
        break;
      case PrimitiveType::kUint16:
        writer.set(topic_id, col_index, static_cast<uint64_t>(value.data.as_uint16));
        break;
      case PrimitiveType::kUint32:
        writer.set(topic_id, col_index, static_cast<uint64_t>(value.data.as_uint32));
        break;
      case PrimitiveType::kUint64:
        writer.set(topic_id, col_index, value.data.as_uint64);
        break;
      case PrimitiveType::kBool:
        writer.set(topic_id, col_index, value.data.as_bool != 0);
        break;
      case PrimitiveType::kString:
        writer.set(topic_id, col_index, toStringView(value.data.as_string));
        break;
      case PrimitiveType::kUnspecified:
        break;
    }
  }

  [[nodiscard]] bool appendRecord(
      TopicHandle topic, Timestamp timestamp, const PJ_named_field_value_t* fields, std::size_t field_count) {
    auto engine_locks = lockWriteEngines(engine, secondary_engine);
    if (engine.getTopicStorage(topic.id) == nullptr) {
      setError(fmt::format("topic {} not found", topic.id));
      return false;
    }

    tsl::robin_set<std::string_view> seen_names;
    struct ResolvedField {
      FieldHandle handle;
      PrimitiveType type;
      PJ_scalar_value_t value;
      bool is_null;
    };
    std::vector<ResolvedField> resolved;
    resolved.reserve(field_count);
    // Normalized names: a deque keeps their addresses stable, so the string_views into it (seen_names too) stay valid
    // for the whole record.
    std::deque<std::string> normalized_names;
    for (std::size_t i = 0; i < field_count; ++i) {
      const auto& field = fields[i];
      auto name = toStringView(field.name);
      if (needsFieldNormalization(name)) {
        std::string norm = normalizeSlashes(name);
        if (!norm.empty() && norm.front() == '/') {
          norm.erase(0, 1);
        }
        normalized_names.push_back(std::move(norm));
        name = normalized_names.back();
      }
      if (!seen_names.insert(name).second) {
        setError(fmt::format("duplicate field name '{}'", name));
        return false;
      }
      if (field.is_null) {
        auto it = field_cache.find(TopicFieldKeyView{.topic_id = topic.id, .field_name = name});
        if (it == field_cache.end()) {
          // A typed null (the ABI keeps value.type when is_null is set) creates the column now; an untyped one
          // (PJ_PRIMITIVE_TYPE_UNSPECIFIED) is skipped until a value arrives.
          auto type_or = fromAbiType(field.value.type);
          if (type_or.has_value()) {
            FieldHandle handle{};
            if (!ensureField(topic, name, field.value.type, &handle)) {
              return false;
            }
            resolved.push_back({handle, *type_or, field.value, true});
          }
          continue;
        }
        PrimitiveType existing{};
        if (!lookupFieldType(topic, it->second.id, &existing)) {
          return false;
        }
        resolved.push_back({it->second, existing, field.value, true});
      } else {
        auto type_or = fromAbiType(field.value.type);
        if (!type_or.has_value()) {
          setError(type_or.error());
          return false;
        }
        FieldHandle handle{};
        PrimitiveType column_type{};
        if (!ensureField(topic, name, field.value.type, &handle, &column_type)) {
          return false;
        }
        if (column_type == *type_or) {
          resolved.push_back({handle, column_type, field.value, false});
        } else {
          auto converted = convertForColumn(topic.id, handle.id, field.value, column_type);
          resolved.push_back({handle, column_type, converted.value_or(field.value), !converted.has_value()});
        }
      }
    }

    auto begin_status = writer.beginRow(topic.id, timestamp);
    if (!begin_status.has_value()) {
      setError(begin_status.error());
      return false;
    }
    for (const auto& field : resolved) {
      if (field.is_null) {
        writer.setNull(topic.id, static_cast<std::size_t>(field.handle.id));
      } else {
        setFieldValue(topic.id, static_cast<std::size_t>(field.handle.id), field.type, field.value);
      }
    }
    auto finish_status = writer.finishRow(topic.id);
    if (!finish_status.has_value()) {
      setError(finish_status.error());
      return false;
    }
    last_error.clear();
    return true;
  }

  [[nodiscard]] bool appendBoundRecord(
      TopicHandle topic, Timestamp timestamp, const PJ_bound_field_value_t* fields, std::size_t field_count) {
    auto engine_locks = lockWriteEngines(engine, secondary_engine);
    if (engine.getTopicStorage(topic.id) == nullptr) {
      setError(fmt::format("topic {} not found", topic.id));
      return false;
    }

    tsl::robin_set<FieldId> seen_ids;
    struct ResolvedField {
      FieldId id;
      PrimitiveType type;
      PJ_scalar_value_t value;
      bool is_null;
    };
    std::vector<ResolvedField> resolved;
    resolved.reserve(field_count);
    for (std::size_t i = 0; i < field_count; ++i) {
      const auto& field = fields[i];
      if (field.field.topic.id != topic.id) {
        setError("field handle does not belong to the target topic");
        return false;
      }
      if (!seen_ids.insert(field.field.id).second) {
        setError(fmt::format("duplicate field id {}", field.field.id));
        return false;
      }
      PrimitiveType type{};
      if (!lookupFieldType(topic, field.field.id, &type)) {
        return false;
      }
      if (field.is_null || field.value.type == static_cast<PJ_primitive_type_t>(type)) {
        resolved.push_back({field.field.id, type, field.value, field.is_null});
        continue;
      }
      if (auto value_type_or = fromAbiType(field.value.type); !value_type_or.has_value()) {
        setError(value_type_or.error());
        return false;
      }
      auto converted = convertForColumn(topic.id, field.field.id, field.value, type);
      resolved.push_back({field.field.id, type, converted.value_or(field.value), !converted.has_value()});
    }

    auto begin_status = writer.beginRow(topic.id, timestamp);
    if (!begin_status.has_value()) {
      setError(begin_status.error());
      return false;
    }
    for (const auto& field : resolved) {
      if (field.is_null) {
        writer.setNull(topic.id, static_cast<std::size_t>(field.id));
      } else {
        setFieldValue(topic.id, static_cast<std::size_t>(field.id), field.type, field.value);
      }
    }
    auto finish_status = writer.finishRow(topic.id);
    if (!finish_status.has_value()) {
      setError(finish_status.error());
      return false;
    }
    last_error.clear();
    return true;
  }

  /// Does not release `stream`: the ABI trampoline releases it on success only.
  [[nodiscard]] bool appendArrowStream(
      TopicHandle topic, struct ArrowArrayStream* stream, PJ_string_view_t timestamp_column) {
    auto engine_locks = lockWriteEngines(engine, secondary_engine);
    if (stream == nullptr) {
      setError("append_arrow_stream: null stream");
      return false;
    }
    if (engine.getTopicStorage(topic.id) == nullptr) {
      setError(fmt::format("topic {} not found", topic.id));
      return false;
    }

    auto schema_or = arrow_import::schemaFromArrowStream(stream);
    if (!schema_or.has_value()) {
      setError(schema_or.error());
      return false;
    }

    const std::string_view timestamp_name = toStringView(timestamp_column);
    int ts_arrow_col = -1;
    std::vector<arrow_import::ArrowColumnMapping> mappings;
    for (const auto& mapping : schema_or->second) {
      if (!timestamp_name.empty() && mapping.field_name == timestamp_name) {
        ts_arrow_col = mapping.arrow_column_index;
        continue;
      }

      FieldHandle field{};
      if (!ensureField(topic, mapping.field_name, static_cast<PJ_primitive_type_t>(mapping.pj_type), &field)) {
        return false;
      }
      auto adjusted = mapping;
      adjusted.pj_column_index = static_cast<std::size_t>(field.id);
      mappings.push_back(std::move(adjusted));
    }

    if (!timestamp_name.empty() && ts_arrow_col < 0) {
      // Not a value-typed column: a native Arrow TIMESTAMP (with its s/ms/us/ns
      // unit, which the importer scales) is looked up in the full schema.
      ts_arrow_col = arrow_import::columnIndexByName(stream, timestamp_name);
    }
    if (!timestamp_name.empty() && ts_arrow_col < 0) {
      setError(fmt::format("timestamp column '{}' not found in stream schema", timestamp_name));
      return false;
    }

    auto status = arrow_import::importArrowStream(writer, topic.id, stream, mappings, ts_arrow_col);
    if (!status.has_value()) {
      setError(status.error());
      return false;
    }
    last_error.clear();
    return true;
  }

  void flushPending() {
    auto lock = engine.lockEngine();
    auto flushed = writer.flushAll();
    if (!flushed.empty()) {
      engine.commitChunks(std::move(flushed));
    }
  }
};

struct CatalogSnapshotState {
  std::deque<std::string> names;
  std::vector<PJ_data_source_info_t> data_sources;
  std::vector<PJ_topic_info_t> topics;
  std::vector<PJ_field_info_t> fields;
  std::vector<PJ_object_topic_info_t> object_topics;  // v2 only
};

void releaseCatalogSnapshot(void* ctx) {
  delete static_cast<CatalogSnapshotState*>(ctx);
}

PJ_string_view_t storeString(CatalogSnapshotState& state, std::string_view value) {
  state.names.emplace_back(value);
  const auto& stored = state.names.back();
  return PJ_string_view_t{stored.data(), stored.size()};
}

// A topic's `builtin_object_type` as its static PJ::sdk::name(), or "" when the metadata
// names no type this SDK knows (kNone, a non-string, metadata that is not an object).
[[nodiscard]] std::string_view canonicalObjectType(std::string_view metadata_json) {
  const auto doc = nlohmann::json::parse(metadata_json.begin(), metadata_json.end(), nullptr, false);
  if (!doc.is_object()) {
    return {};
  }
  const auto field = doc.find(std::string(sdk::kBuiltinObjectTypeMetadataKey));
  if (field == doc.end() || !field->is_string()) {
    return {};
  }
  const auto type = sdk::parseBuiltinObjectType(field->get_ref<const std::string&>());
  if (!type || *type == sdk::BuiltinObjectType::kNone) {
    return {};
  }
  return sdk::name(*type);
}

// v2 only: every object topic of every dataset, derived and marker topics included, sorted by (dataset, name). Called
// with the engine lock released: the engine and ObjectStore locks never nest. The strings are copied under the store's
// shared lock, and each type is read from the copy afterwards.
void fillObjectTopics(CatalogSnapshotState* state, const ObjectStore& object_store) {
  object_store.forEachTopicRetained(
      [state](ObjectTopicId id, const ObjectTopicDescriptor& desc, const ObjectStore::RetainedRange& retained) {
        state->object_topics.push_back(
            PJ_object_topic_info_t{
                .handle = PJ_object_topic_handle_t{.id = id.id},
                .source = DataSourceHandle{.id = desc.dataset_id},
                .name = storeString(*state, desc.topic_name),
                .builtin_object_type = {},
                .metadata_json = storeString(*state, desc.metadata_json),
                .entry_count = retained.entry_count,
                .time_min_ns = retained.first,
                .time_max_ns = retained.last,
                .reserved = {0, 0},
            });
      });
  for (auto& info : state->object_topics) {
    const std::string_view type = canonicalObjectType(toStringView(info.metadata_json));
    info.builtin_object_type = PJ_string_view_t{type.data(), type.size()};
  }
  std::sort(
      state->object_topics.begin(), state->object_topics.end(),
      [](const PJ_object_topic_info_t& lhs, const PJ_object_topic_info_t& rhs) {
        if (lhs.source.id != rhs.source.id) {
          return lhs.source.id < rhs.source.id;
        }
        return toStringView(lhs.name) < toStringView(rhs.name);
      });
}

struct ToolboxCore {
  explicit ToolboxCore(DataEngine& data_engine) : write(data_engine), engine(data_engine) {}

  WriteCore write;
  DataEngine& engine;

  // The scalar half of the catalog, shared by v1 and v2. The caller holds lockEngine(): every read is a live view into
  // engine state.
  void fillScalarCatalog(CatalogSnapshotState* state) {
    auto dataset_ids = engine.listDatasets();
    std::sort(dataset_ids.begin(), dataset_ids.end());
    state->data_sources.reserve(dataset_ids.size());

    for (DatasetId ds_id : dataset_ids) {
      const auto* dataset = engine.getDataset(ds_id);
      if (dataset == nullptr) {
        continue;
      }

      const uint32_t first_topic = static_cast<uint32_t>(state->topics.size());
      auto topic_ids = engine.listTopics(ds_id);
      std::sort(topic_ids.begin(), topic_ids.end());
      for (TopicId tid : topic_ids) {
        const auto* storage = engine.getTopicStorage(tid);
        if (storage == nullptr) {
          continue;
        }
        const uint32_t first_field = static_cast<uint32_t>(state->fields.size());
        const auto columns = effectiveColumns(engine, *storage);
        for (const auto& col : columns) {
          state->fields.push_back(
              PJ_field_info_t{
                  .handle = FieldHandle{.topic = TopicHandle{.id = tid}, .id = col.field_id},
                  .name = storeString(*state, col.field_path),
                  .type = static_cast<PJ_primitive_type_t>(col.logical_type),
              });
        }
        state->topics.push_back(
            PJ_topic_info_t{
                .handle = TopicHandle{.id = tid},
                .source = DataSourceHandle{.id = ds_id},
                .name = storeString(*state, storage->descriptor().name),
                .first_field = first_field,
                .field_count = static_cast<uint32_t>(state->fields.size()) - first_field,
            });
      }

      state->data_sources.push_back(
          PJ_data_source_info_t{
              .handle = DataSourceHandle{.id = ds_id},
              .name = storeString(*state, dataset->source_name),
              .first_topic = first_topic,
              .topic_count = static_cast<uint32_t>(state->topics.size()) - first_topic,
          });
    }
  }

  [[nodiscard]] bool acquireCatalogSnapshot(PJ_catalog_snapshot_t* out_snapshot) {
    auto lock = engine.lockEngine();  // held through the deep-copy below
    auto* state = new CatalogSnapshotState{};
    fillScalarCatalog(state);

    *out_snapshot = PJ_catalog_snapshot_t{
        .data_sources = state->data_sources.data(),
        .data_source_count = state->data_sources.size(),
        .topics = state->topics.data(),
        .topic_count = state->topics.size(),
        .fields = state->fields.data(),
        .field_count = state->fields.size(),
        .release_ctx = state,
        .release = releaseCatalogSnapshot,
    };
    write.last_error.clear();
    return true;
  }

  // v2: the scalar half under the engine lock, then the object half once it is released.
  // sdk::storeCatalogSnapshotV2 applies the caller-size rule and releases the snapshot
  // itself when it refuses the caller's struct.
  [[nodiscard]] bool acquireCatalogSnapshotV2(PJ_catalog_snapshot_v2_t* out_snapshot, const ObjectStore& object_store) {
    auto state = std::make_unique<CatalogSnapshotState>();
    {
      auto lock = engine.lockEngine();
      fillScalarCatalog(state.get());
    }
    fillObjectTopics(state.get(), object_store);

    PJ_catalog_snapshot_v2_t filled{};
    filled.data_sources = state->data_sources.data();
    filled.data_source_count = state->data_sources.size();
    filled.topics = state->topics.data();
    filled.topic_count = state->topics.size();
    filled.fields = state->fields.data();
    filled.field_count = state->fields.size();
    filled.object_topics = state->object_topics.data();
    filled.object_topic_count = state->object_topics.size();
    filled.release = releaseCatalogSnapshot;
    filled.release_ctx = state.release();
    if (auto stored = sdk::storeCatalogSnapshotV2(out_snapshot, filled); !stored) {
      write.setError(stored.error());
      return false;
    }
    write.last_error.clear();
    return true;
  }

  // v4: one field's whole time series as host-owned Arrow structs (readSeriesArrowWindow, unbounded).
  [[nodiscard]] bool readSeriesArrow(FieldHandle field, struct ArrowSchema* out_schema, struct ArrowArray* out_array) {
    if (auto status = readSeriesArrowWindow(
            engine, field, std::numeric_limits<Timestamp>::min(), std::numeric_limits<Timestamp>::max(), out_schema,
            out_array);
        !status) {
      write.setError(status.error());
      return false;
    }
    write.last_error.clear();
    return true;
  }
};

// A mutex-backed std::atomic<std::shared_ptr<T>>, which libstdc++ ships only from GCC 12 (CI builds with GCC 11). Like
// std::atomic, neither copyable nor movable.
template <typename T>
class AtomicSharedPtr {
 public:
  AtomicSharedPtr() = default;
  explicit AtomicSharedPtr(std::shared_ptr<T> ptr) : ptr_(std::move(ptr)) {}

  std::shared_ptr<T> load(std::memory_order = std::memory_order_seq_cst) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ptr_;
  }
  void store(std::shared_ptr<T> ptr, std::memory_order = std::memory_order_seq_cst) {
    std::lock_guard<std::mutex> lock(mutex_);
    ptr_ = std::move(ptr);
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<T> ptr_;
};

struct DatastoreSourceWriteHostState {
  DatastoreSourceWriteHostState(DataEngine& engine, DataSourceHandle source_handle)
      : core(std::make_shared<WriteCore>(engine)), source(source_handle), primary_engine(&engine) {}
  // setTarget() replaces the core on the GUI thread while the ingest worker may be inside the old one: each worker call
  // holds a strong reference, so the old core lives until the call returns.
  AtomicSharedPtr<WriteCore> core;
  DataSourceHandle source;
  // Not owned (the streaming manager outlives the host). setTarget() points the new core's mirror at the other one.
  DataEngine* primary_engine = nullptr;
  DataEngine* secondary_engine = nullptr;
};

struct DatastoreParserWriteHostState {
  DatastoreParserWriteHostState(DataEngine& engine, TopicHandle topic_handle)
      : core(std::make_shared<WriteCore>(engine)), topic(topic_handle), primary_engine(&engine) {}
  // As DatastoreSourceWriteHostState::core.
  AtomicSharedPtr<WriteCore> core;
  TopicHandle topic;
  // As in DatastoreSourceWriteHostState: parsers cache FieldHandles across pause/resume.
  DataEngine* primary_engine = nullptr;
  DataEngine* secondary_engine = nullptr;
};

struct DatastoreToolboxHostState {
  DatastoreToolboxHostState(DataEngine& engine, ObjectStore& store) : core(engine), object_store(store) {}
  ToolboxCore core;
  ObjectStore& object_store;
  std::string object_last_error;

  void setObjectError(std::string msg) {
    object_last_error = std::move(msg);
  }
};

struct DatastoreSourceObjectWriteHostState {
  DatastoreSourceObjectWriteHostState(ObjectStore& s, DatasetId dataset) : target(&s), dataset_id(dataset) {}
  // Atomic: pause/resume retargets the host between the primary and the secondary ObjectStore while the worker pushes.
  std::atomic<ObjectStore*> target;
  DatasetId dataset_id;
  std::string last_error;

  void setError(std::string msg) {
    last_error = std::move(msg);
  }
};

struct DatastoreToolboxObjectReadHostState {
  explicit DatastoreToolboxObjectReadHostState(ObjectStore& s) : store(s) {}
  ObjectStore& store;
  std::string last_error;
  // Backs the const char* toolboxObjectTopicMetadata() returns: valid until the next call on this host.
  std::string last_metadata;

  void setError(std::string msg) {
    last_error = std::move(msg);
  }
};

struct DatastoreParserObjectWriteHostState {
  DatastoreParserObjectWriteHostState(ObjectStore& s, ObjectTopicId topic) : target(&s), bound_topic(topic) {}
  // Atomic: see DatastoreSourceObjectWriteHostState.
  std::atomic<ObjectStore*> target;
  ObjectTopicId bound_topic;
  std::string last_error;

  void setError(std::string msg) {
    last_error = std::move(msg);
  }
};

void propagateError(PJ_error_t* out_error, const char* msg) {
  sdk::fillError(out_error, 1, "datastore", msg != nullptr ? std::string_view(msg) : std::string_view{});
}

template <typename Fn>
bool guardHostCallback(PJ_error_t* out_error, Fn&& fn) noexcept {
  try {
    return fn();
  } catch (const std::exception& e) {
    propagateError(out_error, e.what());
  } catch (...) {
    propagateError(out_error, "unknown datastore host exception");
  }
  return false;
}

bool sourceEnsureTopic(void* ctx, PJ_string_view_t topic_name, TopicHandle* out_topic, PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreSourceWriteHostState*>(ctx);
    // Pinned for the whole call: a concurrent setTarget() cannot destroy it, and the op and its error read one core.
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->ensureTopic(impl->source, toStringView(topic_name), out_topic)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool sourceEnsureField(
    void* ctx, TopicHandle topic, PJ_string_view_t field_name, PJ_primitive_type_t type, FieldHandle* out_field,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreSourceWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->ensureField(topic, toStringView(field_name), type, out_field)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool sourceAppendRecord(
    void* ctx, TopicHandle topic, int64_t timestamp, const PJ_named_field_value_t* fields, uint64_t field_count,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreSourceWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->appendRecord(topic, timestamp, fields, field_count)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool sourceAppendBoundRecord(
    void* ctx, TopicHandle topic, int64_t timestamp, const PJ_bound_field_value_t* fields, uint64_t field_count,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreSourceWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->appendBoundRecord(topic, timestamp, fields, field_count)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool sourceAppendArrowStream(
    void* ctx, TopicHandle topic, struct ArrowArrayStream* stream, PJ_string_view_t timestamp_column,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreSourceWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->appendArrowStream(topic, stream, timestamp_column)) {
      // On failure the plugin keeps the stream.
      propagateError(out_error, core->lastError());
      return false;
    }
    // On success the host owns it.
    if (stream != nullptr && stream->release != nullptr) {
      stream->release(stream);
    }
    return true;
  });
}

bool parserEnsureField(
    void* ctx, PJ_string_view_t field_name, PJ_primitive_type_t type, FieldHandle* out_field,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreParserWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->ensureField(impl->topic, toStringView(field_name), type, out_field)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool parserAppendRecord(
    void* ctx, int64_t timestamp, const PJ_named_field_value_t* fields, uint64_t field_count,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreParserWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->appendRecord(impl->topic, timestamp, fields, field_count)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool parserAppendBoundRecord(
    void* ctx, int64_t timestamp, const PJ_bound_field_value_t* fields, uint64_t field_count,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreParserWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->appendBoundRecord(impl->topic, timestamp, fields, field_count)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool parserAppendArrowStream(
    void* ctx, struct ArrowArrayStream* stream, PJ_string_view_t timestamp_column, PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreParserWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->appendArrowStream(impl->topic, stream, timestamp_column)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    if (stream != nullptr && stream->release != nullptr) {
      stream->release(stream);
    }
    return true;
  });
}

bool toolboxCreateDataSource(
    void* ctx, PJ_string_view_t name, DataSourceHandle* out_source, PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.write.createDataSource(toStringView(name), out_source)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxEnsureTopic(
    void* ctx, DataSourceHandle source, PJ_string_view_t topic_name, TopicHandle* out_topic,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.write.ensureTopic(source, toStringView(topic_name), out_topic)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxEnsureField(
    void* ctx, TopicHandle topic, PJ_string_view_t field_name, PJ_primitive_type_t type, FieldHandle* out_field,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.write.ensureField(topic, toStringView(field_name), type, out_field)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxAppendRecord(
    void* ctx, TopicHandle topic, int64_t timestamp, const PJ_named_field_value_t* fields, uint64_t field_count,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.write.appendRecord(topic, timestamp, fields, field_count)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxAppendBoundRecord(
    void* ctx, TopicHandle topic, int64_t timestamp, const PJ_bound_field_value_t* fields, uint64_t field_count,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.write.appendBoundRecord(topic, timestamp, fields, field_count)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxAppendArrowStream(
    void* ctx, TopicHandle topic, struct ArrowArrayStream* stream, PJ_string_view_t timestamp_column,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.write.appendArrowStream(topic, stream, timestamp_column)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    if (stream != nullptr && stream->release != nullptr) {
      stream->release(stream);
    }
    return true;
  });
}

bool toolboxAcquireCatalogSnapshot(void* ctx, PJ_catalog_snapshot_t* out_snapshot, PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.acquireCatalogSnapshot(out_snapshot)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxAcquireCatalogSnapshotV2(
    void* ctx, PJ_catalog_snapshot_v2_t* out_snapshot, PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.acquireCatalogSnapshotV2(out_snapshot, impl->object_store)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxReadSeriesArrow(
    void* ctx, FieldHandle field, struct ArrowSchema* out_schema, struct ArrowArray* out_array,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.readSeriesArrow(field, out_schema, out_array)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxRegisterObjectTopic(
    void* ctx, DataSourceHandle source, PJ_string_view_t topic_name, PJ_string_view_t metadata_json,
    PJ_object_topic_handle_t* out_handle, PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
  if (out_handle == nullptr) {
    propagateError(out_error, "out_handle must not be null");
    return false;
  }
  // The engine lock covers this lookup only: the engine and ObjectStore locks never nest.
  {
    auto lock = impl->core.engine.lockEngine();
    if (impl->core.engine.getDataset(source.id) == nullptr) {
      impl->setObjectError(fmt::format("data source {} not found", source.id));
      propagateError(out_error, impl->object_last_error.c_str());
      return false;
    }
  }
  try {
    ObjectTopicDescriptor desc{};
    desc.dataset_id = source.id;
    desc.topic_name = std::string(toStringView(topic_name));
    desc.metadata_json = std::string(toStringView(metadata_json));
    auto result = impl->object_store.registerTopic(desc);
    if (!result) {
      impl->setObjectError(result.error());
      propagateError(out_error, impl->object_last_error.c_str());
      return false;
    }
    out_handle->id = result->id;
    impl->object_last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setObjectError(e.what());
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  } catch (...) {
    impl->setObjectError("registerObjectTopic: unknown exception");
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  }
}

bool toolboxRegisterObjectTopicOnDataset(
    void* ctx, uint32_t dataset_id, PJ_string_view_t topic_name, PJ_string_view_t metadata_json,
    PJ_object_topic_handle_t* out_handle, PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
  if (out_handle == nullptr) {
    propagateError(out_error, "out_handle must not be null");
    return false;
  }
  bool dataset_exists = false;
  {
    // The engine lock covers this lookup only, as above.
    const auto lock = impl->core.engine.lockEngine();
    dataset_exists = impl->core.engine.getDataset(dataset_id) != nullptr;
  }
  if (!dataset_exists) {
    impl->setObjectError(fmt::format("dataset {} not found", dataset_id));
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  }
  try {
    const std::string name(toStringView(topic_name));
    // Idempotent: a producer that republishes its marker set gets the same topic handle back.
    if (const auto existing = impl->object_store.findTopic(dataset_id, name)) {
      out_handle->id = existing->id;
      impl->object_last_error.clear();
      return true;
    }
    ObjectTopicDescriptor desc{};
    desc.dataset_id = dataset_id;
    desc.topic_name = name;
    desc.metadata_json = std::string(toStringView(metadata_json));
    auto result = impl->object_store.registerTopic(desc);
    if (!result) {
      impl->setObjectError(result.error());
      propagateError(out_error, impl->object_last_error.c_str());
      return false;
    }
    out_handle->id = result->id;
    impl->object_last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setObjectError(e.what());
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  } catch (...) {
    impl->setObjectError("registerObjectTopicOnDataset: unknown exception");
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  }
}

// Keeps a topic's last `max_entries` entries (0 = unlimited). A producer that republishes a whole set at a sentinel
// timestamp sets 1, so superseded sets are evicted.
bool toolboxSetObjectTopicRetention(
    void* ctx, PJ_object_topic_handle_t topic, uint64_t max_entries, PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
  try {
    RetentionBudget budget{};
    budget.max_entries = static_cast<size_t>(max_entries);
    impl->object_store.setRetentionBudget(ObjectTopicId{topic.id}, budget);
    impl->object_last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setObjectError(e.what());
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  } catch (...) {
    impl->setObjectError("setObjectTopicRetention: unknown exception");
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  }
}

bool toolboxPushOwnedObject(
    void* ctx, PJ_object_topic_handle_t topic, int64_t timestamp_ns, const uint8_t* data, uint64_t size,
    PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
  if (data == nullptr && size > 0) {
    impl->setObjectError("data must not be null when size > 0");
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  }
  try {
    std::vector<uint8_t> bytes;
    if (data != nullptr && size > 0) {
      bytes.assign(data, data + size);
    }
    auto result = impl->object_store.pushOwned(ObjectTopicId{topic.id}, timestamp_ns, std::move(bytes));
    if (!result) {
      impl->setObjectError(result.error());
      propagateError(out_error, impl->object_last_error.c_str());
      return false;
    }
    impl->object_last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setObjectError(e.what());
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  } catch (...) {
    impl->setObjectError("pushOwnedObject: unknown exception");
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  }
}

/// Owns the plugin's `fetch_ctx` and destroys it once, when the last copy of the store's closure goes. Shared, because
/// std::function needs a copyable target.
class PluginFetchCtx {
 public:
  PluginFetchCtx(PJ_lazy_fetch_fn_t fetch_fn, void* fetch_ctx, void (*destroy_fn)(void*)) noexcept
      : fetch_fn_(fetch_fn), ctx_(fetch_ctx), destroy_fn_(destroy_fn) {}

  ~PluginFetchCtx() {
    if (destroy_fn_ != nullptr) {
      destroy_fn_(ctx_);
    }
  }

  PluginFetchCtx(const PluginFetchCtx&) = delete;
  PluginFetchCtx& operator=(const PluginFetchCtx&) = delete;
  PluginFetchCtx(PluginFetchCtx&&) = delete;
  PluginFetchCtx& operator=(PluginFetchCtx&&) = delete;

  // Hands fetch_ctx back to the plugin (a rejected push). Valid only while this holder is the sole reference.
  void disarm() noexcept {
    destroy_fn_ = nullptr;
  }

  // nullopt when the plugin reports failure or promises bytes it does not hand over (fetch failed); an empty vector is
  // a legitimately empty payload.
  [[nodiscard]] std::optional<std::vector<uint8_t>> invoke() const {
    if (fetch_fn_ == nullptr) {
      return std::nullopt;
    }
    // The ABI keeps the buffer valid only until the next call on this fetch_ctx (the SDK's LazyBox reuses one vector),
    // and the store resolves with no lock held: the lock covers the fetch and the copy, or two readers race.
    const std::lock_guard lock(mutex_);
    const uint8_t* data = nullptr;
    uint64_t size = 0;  // matches PJ_lazy_fetch_fn_t out_size (uint64_t*)
    if (!fetch_fn_(ctx_, &data, &size)) {
      return std::nullopt;
    }
    if (data == nullptr && size > 0) {
      return std::nullopt;
    }
    if (size == 0) {
      return std::vector<uint8_t>{};
    }
    return std::vector<uint8_t>(data, data + size);
  }

 private:
  PJ_lazy_fetch_fn_t fetch_fn_;
  void* ctx_;
  void (*destroy_fn_)(void*);
  mutable std::mutex mutex_;
};

// Shared body of the source and parser push_lazy trampolines. fetch_ctx_destroy runs only for an entry the store
// accepted, when it is removed: on a rejected push the plugin keeps fetch_ctx, since the SDK's pushLazy wrappers delete
// their box when push_lazy returns false, and destroying it here too would double-free it.
Status pushPluginLazy(
    ObjectStore& store, ObjectTopicId topic, int64_t timestamp_ns, PJ_lazy_fetch_fn_t fetch_fn, void* fetch_ctx,
    void (*fetch_ctx_destroy)(void*)) {
  auto holder = std::make_shared<PluginFetchCtx>(fetch_fn, fetch_ctx, fetch_ctx_destroy);
  auto closure = [holder]() -> std::optional<sdk::PayloadView> {
    auto bytes = holder->invoke();
    if (!bytes.has_value()) {
      return std::nullopt;  // resolves as fetch_failed
    }
    return sdk::makePayloadView(std::move(*bytes));
  };
  try {
    auto status = store.pushLazy(topic, timestamp_ns, std::move(closure));
    if (!status) {
      holder->disarm();  // rejected: the store holds no copy, `holder` is the last one
    }
    return status;
  } catch (...) {
    holder->disarm();
    throw;
  }
}

bool sourceObjectRegisterTopic(
    void* ctx, PJ_string_view_t topic_name, PJ_string_view_t metadata_json, PJ_object_topic_handle_t* out_handle,
    PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreSourceObjectWriteHostState*>(ctx);
  if (out_handle == nullptr) {
    propagateError(out_error, "out_handle must not be null");
    return false;
  }
  auto* target = impl->target.load(std::memory_order_acquire);
  try {
    ObjectTopicDescriptor desc{};
    desc.dataset_id = impl->dataset_id;
    desc.topic_name = std::string(toStringView(topic_name));
    desc.metadata_json = std::string(toStringView(metadata_json));
    // Find-or-register: a reload's refill re-registers the topics the first ingest created and keeps their
    // ObjectTopicIds; a metadata mismatch fails.
    auto result = target->findOrRegisterTopic(desc);
    if (!result) {
      impl->setError(result.error());
      propagateError(out_error, impl->last_error.c_str());
      return false;
    }
    out_handle->id = result->id;
    impl->last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("registerTopic: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

bool sourceObjectPushOwned(
    void* ctx, PJ_object_topic_handle_t topic, int64_t timestamp_ns, const uint8_t* data, uint64_t size,
    PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreSourceObjectWriteHostState*>(ctx);
  if (data == nullptr && size > 0) {
    impl->setError("data must not be null when size > 0");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
  auto* target = impl->target.load(std::memory_order_acquire);
  try {
    std::vector<uint8_t> bytes;
    if (data != nullptr && size > 0) {
      bytes.assign(data, data + size);
    }
    auto result = target->pushOwned(ObjectTopicId{topic.id}, timestamp_ns, std::move(bytes));
    if (!result) {
      impl->setError(result.error());
      propagateError(out_error, impl->last_error.c_str());
      return false;
    }
    impl->last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("pushOwned: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

bool sourceObjectPushLazy(
    void* ctx, PJ_object_topic_handle_t topic, int64_t timestamp_ns, PJ_lazy_fetch_fn_t fetch_fn, void* fetch_ctx,
    void (*fetch_ctx_destroy)(void*), PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreSourceObjectWriteHostState*>(ctx);
  if (fetch_fn == nullptr) {
    propagateError(out_error, "fetch_fn must not be null");  // fetch_ctx stays the plugin's
    return false;
  }
  auto* target = impl->target.load(std::memory_order_acquire);
  try {
    auto result =
        pushPluginLazy(*target, ObjectTopicId{topic.id}, timestamp_ns, fetch_fn, fetch_ctx, fetch_ctx_destroy);
    if (!result) {
      impl->setError(result.error());
      propagateError(out_error, impl->last_error.c_str());
      return false;
    }
    impl->last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("pushLazy: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

void sourceObjectSetRetentionBudget(
    void* ctx, PJ_object_topic_handle_t topic, int64_t time_window_ns, uint64_t max_memory_bytes) noexcept {
  auto* impl = static_cast<DatastoreSourceObjectWriteHostState*>(ctx);
  auto* target = impl->target.load(std::memory_order_acquire);
  try {
    RetentionBudget budget{};
    budget.time_window_ns = time_window_ns;
    budget.max_memory_bytes = max_memory_bytes;
    target->setRetentionBudget(ObjectTopicId{topic.id}, budget);
  } catch (...) {
    // void in the ABI: nothing to report, and no exception may cross it.
  }
}

// ---------------------------------------------------------------------------
// Toolbox object read host trampolines
// ---------------------------------------------------------------------------

// PJ_object_bytes_handle_t is a heap-allocated sdk::PayloadView: its anchor keeps the buffer alive until the plugin
// calls release_bytes.
PJ_object_topic_handle_t toolboxObjectLookupTopic(void* ctx, PJ_string_view_t topic_name) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  try {
    const auto needle = toStringView(topic_name);
    for (const auto id : impl->store.listTopics()) {
      if (impl->store.descriptor(id).topic_name == needle) {
        return PJ_object_topic_handle_t{id.id};
      }
    }
  } catch (...) {
    // Fall through to invalid handle.
  }
  return PJ_object_topic_handle_t{0};
}

// Object-topic identity is (dataset, name): this resolves the topic of `dataset_id`, not the first name match.
PJ_object_topic_handle_t toolboxObjectLookupTopicOnDataset(
    void* ctx, uint32_t dataset_id, PJ_string_view_t topic_name) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  try {
    if (const auto id = impl->store.findTopic(dataset_id, toStringView(topic_name))) {
      return PJ_object_topic_handle_t{id->id};
    }
  } catch (...) {
    // Fall through to invalid handle.
  }
  return PJ_object_topic_handle_t{0};
}

bool toolboxObjectListTopics(
    void* ctx, PJ_object_topic_handle_t* out_buffer, uint64_t buffer_capacity, uint64_t* out_count,
    PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  if (out_count == nullptr) {
    propagateError(out_error, "out_count must not be null");
    return false;
  }
  try {
    const auto ids = impl->store.listTopics();
    *out_count = ids.size();
    if (out_buffer != nullptr) {
      const std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(buffer_capacity, ids.size()));
      for (std::size_t i = 0; i < n; ++i) {
        out_buffer[i] = PJ_object_topic_handle_t{ids[i].id};
      }
    }
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("listTopics: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

const char* toolboxObjectTopicMetadata(void* ctx, PJ_object_topic_handle_t topic) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  try {
    impl->last_metadata = impl->store.descriptor(ObjectTopicId{topic.id}).metadata_json;
    return impl->last_metadata.c_str();
  } catch (...) {
    return nullptr;
  }
}

bool toolboxObjectReadLatestAt(
    void* ctx, PJ_object_topic_handle_t topic, int64_t timestamp_ns, PJ_object_bytes_handle_t* out_handle,
    int64_t* out_timestamp, PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  if (out_handle == nullptr) {
    propagateError(out_error, "out_handle must not be null");
    return false;
  }
  *out_handle = nullptr;
  try {
    auto entry = impl->store.latestAt(ObjectTopicId{topic.id}, timestamp_ns);
    if (!entry.has_value() || entry->payload.anchor == nullptr) {
      impl->setError("no entry at-or-before timestamp");
      propagateError(out_error, impl->last_error.c_str());
      return false;
    }
    auto* payload_handle = new sdk::PayloadView(std::move(entry->payload));
    *out_handle = reinterpret_cast<PJ_object_bytes_handle_t>(payload_handle);
    if (out_timestamp != nullptr) {
      *out_timestamp = entry->timestamp;
    }
    impl->last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("readLatestAt: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

void toolboxObjectGetBytes(PJ_object_bytes_handle_t handle, const uint8_t** out_data, uint64_t* out_size) noexcept {
  if (out_data != nullptr) {
    *out_data = nullptr;
  }
  if (out_size != nullptr) {
    *out_size = 0;
  }
  if (handle == nullptr) {
    return;
  }
  const auto* payload = reinterpret_cast<const sdk::PayloadView*>(handle);
  if (payload->anchor == nullptr) {
    return;
  }
  if (out_data != nullptr) {
    *out_data = payload->bytes.data();
  }
  if (out_size != nullptr) {
    *out_size = payload->bytes.size();
  }
}

void toolboxObjectReleaseBytes(PJ_object_bytes_handle_t handle) noexcept {
  if (handle == nullptr) {
    return;
  }
  delete reinterpret_cast<sdk::PayloadView*>(handle);
}

uint64_t toolboxObjectEntryCount(void* ctx, PJ_object_topic_handle_t topic) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  try {
    return impl->store.entryCount(ObjectTopicId{topic.id});
  } catch (...) {
    return 0;
  }
}

bool toolboxObjectTimeRange(
    void* ctx, PJ_object_topic_handle_t topic, int64_t* out_min_ts, int64_t* out_max_ts) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  try {
    if (impl->store.entryCount(ObjectTopicId{topic.id}) == 0) {
      return false;
    }
    const auto range = impl->store.timeRange(ObjectTopicId{topic.id});
    if (out_min_ts != nullptr) {
      *out_min_ts = range.first;
    }
    if (out_max_ts != nullptr) {
      *out_max_ts = range.second;
    }
    return true;
  } catch (...) {
    return false;
  }
}

// ---------------------------------------------------------------------------
// Parser object write host trampolines — topic bound at service-create time.
// ---------------------------------------------------------------------------

bool parserObjectPushOwned(
    void* ctx, int64_t timestamp_ns, const uint8_t* data, uint64_t size, PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreParserObjectWriteHostState*>(ctx);
  if (data == nullptr && size > 0) {
    impl->setError("data must not be null when size > 0");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
  auto* target = impl->target.load(std::memory_order_acquire);
  try {
    std::vector<uint8_t> bytes;
    if (data != nullptr && size > 0) {
      bytes.assign(data, data + size);
    }
    auto result = target->pushOwned(impl->bound_topic, timestamp_ns, std::move(bytes));
    if (!result) {
      impl->setError(result.error());
      propagateError(out_error, impl->last_error.c_str());
      return false;
    }
    impl->last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("parser pushOwned: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

bool parserObjectPushLazy(
    void* ctx, int64_t timestamp_ns, PJ_lazy_fetch_fn_t fetch_fn, void* fetch_ctx, void (*fetch_ctx_destroy)(void*),
    PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreParserObjectWriteHostState*>(ctx);
  if (fetch_fn == nullptr) {
    propagateError(out_error, "fetch_fn must not be null");  // fetch_ctx stays the plugin's
    return false;
  }
  auto* target = impl->target.load(std::memory_order_acquire);
  try {
    auto result = pushPluginLazy(*target, impl->bound_topic, timestamp_ns, fetch_fn, fetch_ctx, fetch_ctx_destroy);
    if (!result) {
      impl->setError(result.error());
      propagateError(out_error, impl->last_error.c_str());
      return false;
    }
    impl->last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("parser pushLazy: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

const PJ_source_write_host_vtable_t kSourceWriteVTable = {
    PJ_PLUGIN_DATA_API_VERSION, sizeof(PJ_source_write_host_vtable_t),
    sourceEnsureTopic,          sourceEnsureField,
    sourceAppendRecord,         sourceAppendBoundRecord,
    sourceAppendArrowStream,
};

const PJ_parser_write_host_vtable_t kParserWriteVTable = {
    PJ_PLUGIN_DATA_API_VERSION, sizeof(PJ_parser_write_host_vtable_t),
    parserEnsureField,          parserAppendRecord,
    parserAppendBoundRecord,    parserAppendArrowStream,
};

const PJ_toolbox_host_vtable_t kToolboxVTable = {
    PJ_PLUGIN_DATA_API_VERSION,
    sizeof(PJ_toolbox_host_vtable_t),
    toolboxCreateDataSource,
    toolboxEnsureTopic,
    toolboxEnsureField,
    toolboxAppendRecord,
    toolboxAppendBoundRecord,
    toolboxAppendArrowStream,
    toolboxAcquireCatalogSnapshot,
    toolboxReadSeriesArrow,
    toolboxRegisterObjectTopic,
    toolboxPushOwnedObject,
    toolboxRegisterObjectTopicOnDataset,
    toolboxSetObjectTopicRetention,
    toolboxAcquireCatalogSnapshotV2,
};

const PJ_object_write_host_vtable_t kSourceObjectWriteVTable = {
    PJ_PLUGIN_DATA_API_VERSION, sizeof(PJ_object_write_host_vtable_t), sourceObjectRegisterTopic, sourceObjectPushOwned,
    sourceObjectPushLazy,       sourceObjectSetRetentionBudget,
};

const PJ_object_read_host_vtable_t kToolboxObjectReadVTable = {
    PJ_PLUGIN_DATA_API_VERSION,
    sizeof(PJ_object_read_host_vtable_t),
    toolboxObjectLookupTopic,
    toolboxObjectListTopics,
    toolboxObjectTopicMetadata,
    toolboxObjectReadLatestAt,
    toolboxObjectGetBytes,
    toolboxObjectReleaseBytes,
    toolboxObjectEntryCount,
    toolboxObjectTimeRange,
    toolboxObjectLookupTopicOnDataset,
};

const PJ_parser_object_write_host_vtable_t kParserObjectWriteVTable = {
    PJ_PLUGIN_DATA_API_VERSION,
    sizeof(PJ_parser_object_write_host_vtable_t),
    parserObjectPushOwned,
    parserObjectPushLazy,
};

DatastoreSourceWriteHost::DatastoreSourceWriteHost(DataEngine& engine, DataSourceHandle source)
    : state_(std::make_unique<DatastoreSourceWriteHostState>(engine, source)) {}
DatastoreSourceWriteHost::~DatastoreSourceWriteHost() = default;
DatastoreSourceWriteHost::DatastoreSourceWriteHost(DatastoreSourceWriteHost&&) noexcept = default;
DatastoreSourceWriteHost& DatastoreSourceWriteHost::operator=(DatastoreSourceWriteHost&&) noexcept = default;

PJ_source_write_host_t DatastoreSourceWriteHost::raw() noexcept {
  return PJ_source_write_host_t{.ctx = state_.get(), .vtable = &kSourceWriteVTable};
}

void DatastoreSourceWriteHost::flushPending() {
  state_->core.load(std::memory_order_acquire)->flushPending();
}

void DatastoreSourceWriteHost::setTarget(DataEngine* target) {
  // Commits the open chunks to the current engine, then binds a fresh WriteCore (writer and caches are per engine)
  // that mirrors to the other engine, so both keep the same TopicIds and FieldIds.
  state_->core.load(std::memory_order_acquire)->flushPending();
  // Fully wired before it is published, so the worker never sees a partial core. A worker call that loaded the old
  // core keeps it alive until it returns.
  auto next = std::make_shared<WriteCore>(*target);
  if (target == state_->primary_engine) {
    next->secondary_engine = state_->secondary_engine;
  } else if (target == state_->secondary_engine) {
    next->secondary_engine = state_->primary_engine;
  } else {
    // Unknown target (an in-place dataset replace): no mirror.
    next->secondary_engine = nullptr;
    state_->primary_engine = target;
  }
  state_->core.store(std::move(next), std::memory_order_release);
}

void DatastoreSourceWriteHost::setSecondaryEngine(DataEngine* secondary) {
  // Call once, before any ingest worker runs: the live core's mirror changes in place, unsynchronized. The active core
  // mirrors to whichever engine it does not write; nullptr disables mirroring.
  state_->secondary_engine = secondary;
  if (auto core = state_->core.load(std::memory_order_acquire)) {
    core->secondary_engine = (&core->engine == state_->primary_engine) ? secondary : state_->primary_engine;
  }
}

DatastoreParserWriteHost::DatastoreParserWriteHost(DataEngine& engine, TopicHandle topic)
    : state_(std::make_unique<DatastoreParserWriteHostState>(engine, topic)) {}
DatastoreParserWriteHost::~DatastoreParserWriteHost() = default;
DatastoreParserWriteHost::DatastoreParserWriteHost(DatastoreParserWriteHost&&) noexcept = default;
DatastoreParserWriteHost& DatastoreParserWriteHost::operator=(DatastoreParserWriteHost&&) noexcept = default;

PJ_parser_write_host_t DatastoreParserWriteHost::raw() noexcept {
  return PJ_parser_write_host_t{.ctx = state_.get(), .vtable = &kParserWriteVTable};
}

void DatastoreParserWriteHost::flushPending() {
  state_->core.load(std::memory_order_acquire)->flushPending();
}

void DatastoreParserWriteHost::setTarget(DataEngine* target) {
  // As DatastoreSourceWriteHost::setTarget(). The bound topic must already exist in `target` with the same TopicId.
  state_->core.load(std::memory_order_acquire)->flushPending();
  auto next = std::make_shared<WriteCore>(*target);
  if (target == state_->primary_engine) {
    next->secondary_engine = state_->secondary_engine;
  } else if (target == state_->secondary_engine) {
    next->secondary_engine = state_->primary_engine;
  } else {
    next->secondary_engine = nullptr;
    state_->primary_engine = target;
  }
  state_->core.store(std::move(next), std::memory_order_release);
}

void DatastoreParserWriteHost::setSecondaryEngine(DataEngine* secondary) {
  // As DatastoreSourceWriteHost::setSecondaryEngine().
  state_->secondary_engine = secondary;
  if (auto core = state_->core.load(std::memory_order_acquire)) {
    core->secondary_engine = (&core->engine == state_->primary_engine) ? secondary : state_->primary_engine;
  }
}

void setIngestWarningHandler(IngestWarningHandler handler) noexcept {
  g_ingest_warning_handler.store(handler, std::memory_order_release);
}

std::string normalizeTopicName(std::string_view name) {
  return needsSlashNormalization(name) ? normalizeSlashes(name) : std::string(name);
}

DatastoreToolboxHost::DatastoreToolboxHost(DataEngine& engine, ObjectStore& object_store)
    : state_(std::make_unique<DatastoreToolboxHostState>(engine, object_store)) {}
DatastoreToolboxHost::~DatastoreToolboxHost() = default;
DatastoreToolboxHost::DatastoreToolboxHost(DatastoreToolboxHost&&) noexcept = default;
DatastoreToolboxHost& DatastoreToolboxHost::operator=(DatastoreToolboxHost&&) noexcept = default;

PJ_toolbox_host_t DatastoreToolboxHost::raw() noexcept {
  return PJ_toolbox_host_t{.ctx = state_.get(), .vtable = &kToolboxVTable};
}

void DatastoreToolboxHost::flushPending() {
  state_->core.write.flushPending();
}

DatastoreSourceObjectWriteHost::DatastoreSourceObjectWriteHost(ObjectStore& store, DatasetId dataset_id)
    : state_(std::make_unique<DatastoreSourceObjectWriteHostState>(store, dataset_id)) {}
DatastoreSourceObjectWriteHost::~DatastoreSourceObjectWriteHost() = default;
DatastoreSourceObjectWriteHost::DatastoreSourceObjectWriteHost(DatastoreSourceObjectWriteHost&&) noexcept = default;
DatastoreSourceObjectWriteHost& DatastoreSourceObjectWriteHost::operator=(DatastoreSourceObjectWriteHost&&) noexcept =
    default;

PJ_object_write_host_t DatastoreSourceObjectWriteHost::raw() noexcept {
  return PJ_object_write_host_t{.ctx = state_.get(), .vtable = &kSourceObjectWriteVTable};
}

void DatastoreSourceObjectWriteHost::setTarget(ObjectStore* target) noexcept {
  state_->target.store(target, std::memory_order_release);
}

DatastoreToolboxObjectReadHost::DatastoreToolboxObjectReadHost(ObjectStore& store)
    : state_(std::make_unique<DatastoreToolboxObjectReadHostState>(store)) {}
DatastoreToolboxObjectReadHost::~DatastoreToolboxObjectReadHost() = default;
DatastoreToolboxObjectReadHost::DatastoreToolboxObjectReadHost(DatastoreToolboxObjectReadHost&&) noexcept = default;
DatastoreToolboxObjectReadHost& DatastoreToolboxObjectReadHost::operator=(DatastoreToolboxObjectReadHost&&) noexcept =
    default;

PJ_object_read_host_t DatastoreToolboxObjectReadHost::raw() noexcept {
  return PJ_object_read_host_t{.ctx = state_.get(), .vtable = &kToolboxObjectReadVTable};
}

DatastoreParserObjectWriteHost::DatastoreParserObjectWriteHost(ObjectStore& store, uint32_t topic_id)
    : state_(std::make_unique<DatastoreParserObjectWriteHostState>(store, ObjectTopicId{topic_id})) {}
DatastoreParserObjectWriteHost::~DatastoreParserObjectWriteHost() = default;
DatastoreParserObjectWriteHost::DatastoreParserObjectWriteHost(DatastoreParserObjectWriteHost&&) noexcept = default;
DatastoreParserObjectWriteHost& DatastoreParserObjectWriteHost::operator=(DatastoreParserObjectWriteHost&&) noexcept =
    default;

PJ_parser_object_write_host_t DatastoreParserObjectWriteHost::raw() noexcept {
  return PJ_parser_object_write_host_t{.ctx = state_.get(), .vtable = &kParserObjectWriteVTable};
}

void DatastoreParserObjectWriteHost::setTarget(ObjectStore* target) noexcept {
  state_->target.store(target, std::memory_order_release);
}

}  // namespace PJ
