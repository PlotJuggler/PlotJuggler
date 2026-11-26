// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/writer.hpp"

#include <fmt/format.h>
#include <tsl/robin_map.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "pj_base/assert.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/type_registry.hpp"

namespace PJ {

struct DataWriter::Impl {
  explicit Impl(DataEngine& eng) : engine(eng) {}
  DataEngine& engine;
  tsl::robin_map<PJ::TopicId, TopicChunkBuilder> builders;
  // A topic's rows set aside, oldest first, when their builder filled or before a layout change. A builder that
  // holds a whole chunk is sealed at once, so a long import between two commits holds encoded chunks and not raw rows;
  // any other is kept as it is, for TopicStorage::appendRows() to put in the topic's last chunk.
  struct Pending {
    std::vector<std::variant<TopicChunkBuilder, TopicChunk>> batches;
    uint64_t rows = 0;
  };
  tsl::robin_map<PJ::TopicId, Pending> pending_batches;
  tsl::robin_map<PJ::TopicId, std::vector<ColumnDescriptor>> topic_columns;
};

namespace {

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace

std::size_t ColumnData::rowCount() const {
  return std::visit(
      overloaded{
          [](const StringData& s) -> std::size_t { return s.offsets.empty() ? 0 : s.offsets.size() - 1; },
          [](const auto& span) -> std::size_t { return span.size(); },
      },
      data);
}

StorageKind ColumnData::kind() const {
  static constexpr StorageKind kInds[] = {
      StorageKind::kFloat32, StorageKind::kFloat64, StorageKind::kInt32,  StorageKind::kInt64,
      StorageKind::kUint64,  StorageKind::kBool,    StorageKind::kString,
  };
  return kInds[data.index()];
}

namespace {

constexpr PrimitiveType numericToPrimitive(NumericType nt) noexcept {
  switch (nt) {
    case NumericType::kFloat32:
      return PrimitiveType::kFloat32;
    case NumericType::kFloat64:
      return PrimitiveType::kFloat64;
    case NumericType::kInt8:
      return PrimitiveType::kInt8;
    case NumericType::kInt16:
      return PrimitiveType::kInt16;
    case NumericType::kInt32:
      return PrimitiveType::kInt32;
    case NumericType::kInt64:
      return PrimitiveType::kInt64;
    case NumericType::kUint8:
      return PrimitiveType::kUint8;
    case NumericType::kUint16:
      return PrimitiveType::kUint16;
    case NumericType::kUint32:
      return PrimitiveType::kUint32;
    case NumericType::kUint64:
      return PrimitiveType::kUint64;
  }
  return PrimitiveType::kFloat64;  // unreachable
}

void flattenColumnsImpl(
    const TypeTreeNode& node, std::string_view prefix, FieldId& next_field_id, std::vector<ColumnDescriptor>& out);

void flattenArrayElementImpl(
    const TypeTreeNode& element_type, std::string_view element_path, FieldId& next_field_id,
    std::vector<ColumnDescriptor>& out) {
  if (element_type.kind == TypeKind::kStruct) {
    for (const auto& child : element_type.children) {
      flattenColumnsImpl(*child, element_path, next_field_id, out);
    }
  } else {
    ColumnDescriptor desc;
    desc.field_id = next_field_id++;
    desc.field_path = std::string(element_path);
    desc.logical_type = element_type.primitive_type.value_or(PrimitiveType::kFloat64);
    out.push_back(std::move(desc));
  }
}

void flattenColumnsImpl(
    const TypeTreeNode& node, std::string_view prefix, FieldId& next_field_id, std::vector<ColumnDescriptor>& out) {
  std::string current_path = prefix.empty() ? node.name : fmt::format("{}.{}", prefix, node.name);

  if (node.kind == TypeKind::kStruct) {
    for (const auto& child : node.children) {
      flattenColumnsImpl(*child, current_path, next_field_id, out);
    }
    return;
  }

  if (node.kind == TypeKind::kArray) {
    if (node.fixed_array_size.has_value()) {
      for (uint32_t i = 0; i < *node.fixed_array_size; ++i) {
        std::string elem_path = fmt::format("{}[{}]", current_path, i);
        flattenArrayElementImpl(*node.element_type, elem_path, next_field_id, out);
      }
    }
    // A variable-length array gets its columns from expandArray().
    return;
  }

  ColumnDescriptor desc;
  desc.field_id = next_field_id++;
  desc.field_path = std::move(current_path);
  desc.logical_type = node.primitive_type.value_or(PrimitiveType::kFloat64);
  out.push_back(std::move(desc));
}

// `path` is dotted and excludes the root's name, e.g. "body.poses".
const TypeTreeNode* findChildAtPath(const TypeTreeNode& root, std::string_view path) {
  std::string_view remaining = path;
  const TypeTreeNode* cur = &root;
  while (!remaining.empty()) {
    size_t dot = remaining.find('.');
    std::string_view segment = remaining.substr(0, dot);
    remaining = (dot == std::string_view::npos) ? std::string_view{} : remaining.substr(dot + 1);
    if (cur->kind != TypeKind::kStruct) {
      return nullptr;
    }
    const TypeTreeNode* found = nullptr;
    for (const auto& child : cur->children) {
      if (child->name == segment) {
        found = child.get();
        break;
      }
    }
    if (!found) {
      return nullptr;
    }
    cur = found;
  }
  return cur;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction / move
// ---------------------------------------------------------------------------

DataWriter::DataWriter(DataEngine& engine) : impl_(std::make_unique<Impl>(engine)) {}

DataWriter::~DataWriter() = default;
DataWriter::DataWriter(DataWriter&&) noexcept = default;
DataWriter& DataWriter::operator=(DataWriter&&) noexcept = default;

// ---------------------------------------------------------------------------
// Schema registration
// ---------------------------------------------------------------------------

Expected<SchemaId> DataWriter::registerSchema(std::string schema_name, std::shared_ptr<TypeTreeNode> type_tree) {
  return impl_->engine.typeRegistry().registerSchema(std::move(schema_name), std::move(type_tree));
}

// ---------------------------------------------------------------------------
// Topic registration
// ---------------------------------------------------------------------------

Expected<TopicId> DataWriter::registerTopic(DatasetId dataset_id, TopicDescriptor descriptor) {
  return impl_->engine.createTopic(dataset_id, std::move(descriptor));
}

// ---------------------------------------------------------------------------
// Bind for fast-path access
// ---------------------------------------------------------------------------

Expected<TopicWriteHandle> DataWriter::bindTopicWriter(TopicId topic_id) {
  const auto* storage = impl_->engine.getTopicStorage(topic_id);
  if (storage == nullptr) {
    return PJ::unexpected(fmt::format("Topic {} not found", topic_id));
  }

  auto& builder = getOrCreateBuilder(topic_id);
  (void)builder;  // only its side effect: the columns get cached

  const auto& columns = impl_->topic_columns.at(topic_id);
  TopicWriteHandle handle;
  handle.topic_id = topic_id;
  handle.field_ids.reserve(columns.size());
  for (const auto& col : columns) {
    handle.field_ids.push_back(col.field_id);
  }
  return handle;
}

// ---------------------------------------------------------------------------
// Field resolution
// ---------------------------------------------------------------------------

Expected<FieldId> DataWriter::resolveField(TopicId topic_id, std::string_view field_path) {
  // Creating the builder caches the columns.
  auto& builder = getOrCreateBuilder(topic_id);
  (void)builder;

  auto col_it = impl_->topic_columns.find(topic_id);
  if (col_it == impl_->topic_columns.end()) {
    return PJ::unexpected(fmt::format("Topic {} not found", topic_id));
  }

  for (const auto& col : col_it->second) {
    if (col.field_path == field_path) {
      return col.field_id;
    }
  }
  return PJ::unexpected(fmt::format("Field '{}' not found in topic {}", field_path, topic_id));
}

// ---------------------------------------------------------------------------
// Row-at-a-time append
// ---------------------------------------------------------------------------

PJ::Status DataWriter::beginRow(TopicId topic_id, Timestamp t) {
  auto* storage = impl_->engine.getTopicStorage(topic_id);
  if (storage == nullptr) {
    return PJ::unexpected(fmt::format("Topic {} not found", topic_id));
  }
  auto& builder = getOrCreateBuilder(topic_id, storage);
  builder.beginRow(t);
  return PJ::okStatus();
}

PJ::Status DataWriter::finishRow(PJ::TopicId topic_id) {
  auto it = impl_->builders.find(topic_id);
  if (it == impl_->builders.end()) {
    return PJ::unexpected(fmt::format("finish_row: no active row for topic {}", topic_id));
  }
  it.value().finishRow();

  if (it->second.isFull()) {
    setBuilderAside(topic_id);
  }
  return PJ::okStatus();
}

template <typename T>
void DataWriter::set(TopicId topic_id, std::size_t col_index, T value) {
  auto it = impl_->builders.find(topic_id);
  PJ_ASSERT(it != impl_->builders.end(), "set: no builder for topic");
  if (it != impl_->builders.end()) {
    it.value().set<T>(col_index, value);
  }
}

template void DataWriter::set<float>(TopicId, std::size_t, float);
template void DataWriter::set<double>(TopicId, std::size_t, double);
template void DataWriter::set<int32_t>(TopicId, std::size_t, int32_t);
template void DataWriter::set<int64_t>(TopicId, std::size_t, int64_t);
template void DataWriter::set<uint64_t>(TopicId, std::size_t, uint64_t);
template void DataWriter::set<bool>(TopicId, std::size_t, bool);
template void DataWriter::set<std::string_view>(TopicId, std::size_t, std::string_view);

void DataWriter::setNull(TopicId topic_id, std::size_t col_index) {
  auto it = impl_->builders.find(topic_id);
  PJ_ASSERT(it != impl_->builders.end(), "set_null: no builder for topic");
  if (it != impl_->builders.end()) {
    it.value().setNull(col_index);
  }
}

// ---------------------------------------------------------------------------
// Bulk column append
// ---------------------------------------------------------------------------

namespace {

void appendSingleColumnToBuilder(
    TopicChunkBuilder& builder, const ColumnData& col, std::size_t offset, std::size_t batch_size) {
  std::visit(
      overloaded{
          [&](Span<const float> d) { builder.appendColumn(col.col_index, d.subspan(offset, batch_size)); },
          [&](Span<const double> d) { builder.appendColumn(col.col_index, d.subspan(offset, batch_size)); },
          [&](Span<const int32_t> d) { builder.appendColumn(col.col_index, d.subspan(offset, batch_size)); },
          [&](Span<const int64_t> d) { builder.appendColumn(col.col_index, d.subspan(offset, batch_size)); },
          [&](Span<const uint64_t> d) { builder.appendColumn(col.col_index, d.subspan(offset, batch_size)); },
          [&](Span<const uint8_t> d) { builder.appendColumn(col.col_index, d.subspan(offset, batch_size)); },
          [&](const ColumnData::StringData& s) {
            builder.appendColumnStrings(col.col_index, s.offsets.subspan(offset, batch_size + 1), s.values);
          },
      },
      col.data);

  if (!col.validity.empty()) {
    builder.appendColumnValidity(col.col_index, col.validity.subspan(offset, batch_size));
  }
}

}  // namespace

PJ::Status DataWriter::appendColumns(
    TopicId topic_id, Span<const Timestamp> timestamps, Span<const ColumnData> columns) {
  auto* storage = impl_->engine.getTopicStorage(topic_id);
  if (storage == nullptr) {
    return PJ::unexpected(fmt::format("Topic {} not found", topic_id));
  }

  for (const auto& col : columns) {
    const std::size_t n = col.rowCount();
    if (n != timestamps.size()) {
      return PJ::unexpected(
          fmt::format("Column {} has {} rows but {} timestamps provided", col.col_index, n, timestamps.size()));
    }

    if (!col.validity.empty()) {
      if (col.validity.bit_length != n) {
        return PJ::unexpected(fmt::format("Column {} validity bit_length mismatch", col.col_index));
      }
      const std::size_t available_bits = col.validity.bytes.size() * 8;
      if (col.validity.bit_offset + col.validity.bit_length > available_bits) {
        return PJ::unexpected(fmt::format("Column {} validity range out of bounds", col.col_index));
      }
    }
  }

  if (timestamps.empty()) {
    return PJ::okStatus();
  }

  std::size_t offset = 0;
  const std::size_t total = timestamps.size();

  while (offset < total) {
    auto& b = getOrCreateBuilder(topic_id);
    const std::size_t batch_size = std::min(total - offset, static_cast<std::size_t>(b.remainingCapacity()));

    b.appendTimestamps(timestamps.subspan(offset, batch_size));
    for (const auto& col : columns) {
      appendSingleColumnToBuilder(b, col, offset, batch_size);
    }
    b.finishBulkAppend();

    if (b.isFull()) {
      setBuilderAside(topic_id);
    }

    offset += batch_size;
  }

  return PJ::okStatus();
}

// ---------------------------------------------------------------------------
// Scalar convenience API
// ---------------------------------------------------------------------------

Expected<ScalarSeriesHandle> DataWriter::registerScalarSeries(
    DatasetId dataset_id, std::string_view topic_name, NumericType value_type) {
  TopicDescriptor desc;
  desc.name = std::string(topic_name);
  desc.schema_id = 0;

  auto topic_id_or = impl_->engine.createTopic(dataset_id, std::move(desc));
  if (!topic_id_or.has_value()) {
    return PJ::unexpected(topic_id_or.error());
  }
  TopicId topic_id = *topic_id_or;

  ColumnDescriptor col_desc;
  col_desc.field_id = 0;
  col_desc.logical_type = numericToPrimitive(value_type);
  col_desc.field_path = "value";

  std::vector<ColumnDescriptor> columns;
  columns.push_back(std::move(col_desc));
  impl_->topic_columns[topic_id] = columns;

  // Stored on the topic so other writers and the derived engine resolve it before any chunk is committed.
  if (auto* storage = impl_->engine.getTopicStorage(topic_id)) {
    storage->setColumnDescriptors(std::move(columns));
  }

  ScalarSeriesHandle handle{topic_id, 0};
  return handle;
}

void DataWriter::appendScalar(const ScalarSeriesHandle& handle, Timestamp t, NumericValue value) {
  auto& builder = getOrCreateBuilder(handle.topic_id);
  builder.beginRow(t);

  const auto col = static_cast<std::size_t>(handle.value_field);
  std::visit(
      [&builder, col](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, float>) {
          builder.set(col, v);
        } else if constexpr (std::is_same_v<T, double>) {
          builder.set(col, v);
        } else if constexpr (std::is_same_v<T, int32_t>) {
          builder.set(col, v);
        } else if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, int16_t> || std::is_same_v<T, int64_t>) {
          builder.set(col, static_cast<int64_t>(v));
        } else if constexpr (
            std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> || std::is_same_v<T, uint32_t> ||
            std::is_same_v<T, uint64_t>) {
          builder.set(col, static_cast<uint64_t>(v));
        }
      },
      value);

  builder.finishRow();

  if (builder.isFull()) {
    setBuilderAside(handle.topic_id);
  }
}

// ---------------------------------------------------------------------------
// Flush
// ---------------------------------------------------------------------------

namespace {
// A batch set aside, as the chunk flush() and flushAll() return.
[[nodiscard]] TopicChunk sealed(std::variant<TopicChunkBuilder, TopicChunk>& batch) {
  if (auto* builder = std::get_if<TopicChunkBuilder>(&batch)) {
    return builder->seal();
  }
  return std::move(std::get<TopicChunk>(batch));
}
}  // namespace

std::vector<TopicChunk> DataWriter::flush(TopicId topic_id) {
  std::vector<TopicChunk> result;

  auto pending_it = impl_->pending_batches.find(topic_id);
  if (pending_it != impl_->pending_batches.end()) {
    for (auto& batch : pending_it.value().batches) {
      result.push_back(sealed(batch));
    }
    impl_->pending_batches.erase(pending_it);
  }

  auto builder_it = impl_->builders.find(topic_id);
  if (builder_it != impl_->builders.end() && builder_it->second.rowCount() > 0) {
    result.push_back(builder_it.value().seal());
    impl_->builders.erase(builder_it);
  }

  return result;
}

std::vector<std::pair<TopicId, TopicChunk>> DataWriter::flushAll() {
  std::vector<std::pair<TopicId, TopicChunk>> result;

  for (auto it = impl_->pending_batches.begin(); it != impl_->pending_batches.end(); ++it) {
    for (auto& batch : it.value().batches) {
      result.emplace_back(it->first, sealed(batch));
    }
  }
  impl_->pending_batches.clear();

  // Ids first: erasing while iterating would invalidate the iterator.
  std::vector<TopicId> builder_ids;
  builder_ids.reserve(impl_->builders.size());
  for (auto it = impl_->builders.begin(); it != impl_->builders.end(); ++it) {
    if (it->second.rowCount() > 0) {
      builder_ids.push_back(it->first);
    }
  }
  for (TopicId topic_id : builder_ids) {
    auto it = impl_->builders.find(topic_id);
    if (it != impl_->builders.end()) {
      result.emplace_back(topic_id, it.value().seal());
      impl_->builders.erase(it);
    }
  }

  return result;
}

std::vector<TopicId> DataWriter::commitRows() {
  const auto lock = impl_->engine.lockEngine();
  std::vector<TopicId> changed;
  // A topic's batches set aside come before its open builder: they hold its earlier rows.
  for (auto it = impl_->pending_batches.begin(); it != impl_->pending_batches.end(); ++it) {
    const TopicId topic_id = it->first;
    for (auto& batch : it.value().batches) {
      if (auto* builder = std::get_if<TopicChunkBuilder>(&batch)) {
        if (impl_->engine.commitRows(topic_id, *builder)) {
          changed.push_back(topic_id);
        }
        continue;
      }
      std::vector<std::pair<TopicId, TopicChunk>> whole;
      whole.emplace_back(topic_id, std::move(std::get<TopicChunk>(batch)));
      for (const TopicId id : impl_->engine.commitChunksLocked(std::move(whole))) {
        changed.push_back(id);
      }
    }
  }
  impl_->pending_batches.clear();

  std::vector<TopicId> stale;
  for (auto it = impl_->builders.begin(); it != impl_->builders.end(); ++it) {
    TopicChunkBuilder& builder = it.value();
    if (builder.rowCount() == 0) {
      continue;
    }
    const TopicId topic_id = it->first;
    if (impl_->engine.commitRows(topic_id, builder)) {
      changed.push_back(topic_id);
    }
    // The builder is kept and emptied: a topic of thousands of columns otherwise frees and reallocates each column's
    // buffers on every commit. Not when the topic's layout moved on without it (another writer added a column, which
    // ensureColumn() then hands out without replacing this builder): a new one takes the current layout.
    const TopicStorage* storage = impl_->engine.getTopicStorage(topic_id);
    const auto cols = impl_->topic_columns.find(topic_id);
    if (storage == nullptr || cols == impl_->topic_columns.end() || cols->second.size() != builder.columnCount() ||
        storage->columnDescriptors().size() > builder.columnCount()) {
      stale.push_back(topic_id);
    } else {
      builder.clearRows(storage->descriptor().max_chunk_rows);  // sized when its next row begins
    }
  }
  for (const TopicId topic_id : stale) {
    impl_->builders.erase(topic_id);
  }

  std::sort(changed.begin(), changed.end());
  changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
  return changed;
}

// ---------------------------------------------------------------------------
// Dynamic column addition
// ---------------------------------------------------------------------------

Expected<FieldId> DataWriter::ensureColumn(TopicId topic_id, std::string_view field_path, PrimitiveType type) {
  auto* storage = impl_->engine.getTopicStorage(topic_id);
  if (!storage) {
    return PJ::unexpected(fmt::format("ensure_column: topic {} not found", topic_id));
  }

  ensureColsLoaded(topic_id, *storage);
  auto& cols = impl_->topic_columns[topic_id];

  for (const auto& col : cols) {
    if (col.field_path == field_path) {
      if (col.logical_type != type) {
        return PJ::unexpected(
            fmt::format("ensure_column: field '{}' already exists with a different type", field_path));
      }
      return col.field_id;
    }
  }

  auto builder_it = impl_->builders.find(topic_id);
  if (builder_it != impl_->builders.end() && builder_it->second.isRowInProgress()) {
    return PJ::unexpected(
        fmt::format(
            "ensure_column: topic {} has a row in progress; call finishRow() before adding new columns", topic_id));
  }

  // A builder has one column layout: its rows are set aside before it changes.
  setBuilderAside(topic_id);

  // Field ids are dense from 0, so the next id is the column count.
  PJ_ASSERT(
      cols.empty() || cols.back().field_id == static_cast<FieldId>(cols.size() - 1),
      "ensure_column: field_id invariant broken — non-dense column ids detected");
  FieldId new_id = static_cast<FieldId>(cols.size());
  ColumnDescriptor desc;
  desc.field_id = new_id;
  desc.logical_type = type;
  desc.field_path = std::string(field_path);
  // In sync, append one: a topic growing column by column otherwise copies its whole layout per column (quadratic).
  // A layout this writer took from the schema or a sealed chunk, or one a reload replaced under it, is published whole.
  const auto& stored = storage->columnDescriptors();
  if (stored.size() == cols.size() && (cols.empty() || stored.back().field_path == cols.back().field_path)) {
    storage->addColumnDescriptor(desc);
    cols.push_back(std::move(desc));
  } else {
    cols.push_back(std::move(desc));
    storage->setColumnDescriptors(cols);
  }
  return new_id;
}

// ---------------------------------------------------------------------------
// Variable-length array expansion
// ---------------------------------------------------------------------------

PJ::Expected<uint32_t> DataWriter::expandArray(
    PJ::TopicId topic_id, std::string_view array_field_path, uint32_t new_length, PJ::PrimitiveType element_type) {
  TopicStorage* storage = impl_->engine.getTopicStorage(topic_id);
  if (!storage) {
    return PJ::unexpected(fmt::format("expand_array: topic {} not found", topic_id));
  }

  storage->updateMaxObservedArrayLength(new_length);

  // The count lives on the topic, shared by every DataWriter.
  std::string path_key(array_field_path);
  const uint32_t current = storage->arrayExpansionCount(path_key);

  if (new_length <= current) {
    return current;
  }

  SchemaId schema_id = storage->descriptor().schema_id;
  const TypeTreeNode* type_tree = impl_->engine.typeRegistry().lookup(schema_id);

  // Typed topics: validate the field against the schema before changing the layout.
  const TypeTreeNode* array_node = nullptr;
  if (type_tree) {
    array_node = findChildAtPath(*type_tree, array_field_path);
    if (!array_node) {
      return PJ::unexpected(fmt::format("expand_array: field '{}' not found in schema", array_field_path));
    }
    if (array_node->kind != TypeKind::kArray) {
      return PJ::unexpected(fmt::format("expand_array: field '{}' is not an array node", array_field_path));
    }
    if (array_node->fixed_array_size.has_value()) {
      return PJ::unexpected(
          fmt::format("expand_array: field '{}' is fixed-size; use schema declaration", array_field_path));
    }
  }

  uint32_t limit = storage->descriptor().array_expansion_limit;
  uint32_t actual = std::min(new_length, limit);
  if (new_length > limit) {
    storage->incrementTruncatedSampleCount();
  }
  if (actual <= current) {
    return current;
  }

  auto builder_it = impl_->builders.find(topic_id);
  if (builder_it != impl_->builders.end() && builder_it->second.isRowInProgress()) {
    return PJ::unexpected(
        fmt::format(
            "expand_array: topic {}"
            " has a row in progress; call finishRow() or abandon the row before calling expandArray()",
            topic_id));
  }

  setBuilderAside(topic_id);

  ensureColsLoaded(topic_id, *storage);
  auto& cols = impl_->topic_columns[topic_id];

  if (!type_tree) {
    // Schemaless: any path is accepted, typed by element_type.
    PJ_ASSERT(
        cols.empty() || cols.back().field_id == static_cast<FieldId>(cols.size() - 1),
        "expand_array: field_id invariant broken — non-dense column ids detected");
    FieldId next_field_id = static_cast<FieldId>(cols.size());
    for (uint32_t i = current; i < actual; ++i) {
      std::string elem_path = fmt::format("{}[{}]", array_field_path, i);
      // May already exist, added by ensureColumn().
      bool already_exists = false;
      for (const auto& col : cols) {
        if (col.field_path == elem_path) {
          already_exists = true;
          break;
        }
      }
      if (!already_exists) {
        ColumnDescriptor desc;
        desc.field_id = next_field_id++;
        desc.logical_type = element_type;
        desc.field_path = std::move(elem_path);
        cols.push_back(std::move(desc));
      }
    }
  } else {
    // Typed: columns from the schema's element type, skipping ones ensureColumn() already added.
    PJ_ASSERT(
        cols.empty() || cols.back().field_id == static_cast<FieldId>(cols.size() - 1),
        "expand_array: field_id invariant broken — non-dense column ids detected");
    FieldId next_field_id = static_cast<FieldId>(cols.size());
    for (uint32_t i = current; i < actual; ++i) {
      std::string elem_prefix = fmt::format("{}[{}]", array_field_path, i);
      // Exact match for a primitive element, prefix match for a struct element's fields.
      bool already_exists = false;
      for (const auto& col : cols) {
        if (col.field_path == elem_prefix || col.field_path.starts_with(elem_prefix + ".")) {
          already_exists = true;
          break;
        }
      }
      if (!already_exists) {
        flattenArrayElementImpl(*array_node->element_type, elem_prefix, next_field_id, cols);
      }
    }
  }

  storage->setColumnDescriptors(cols);
  storage->setArrayExpansionCount(path_key, actual);

  return actual;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void DataWriter::ensureColsLoaded(TopicId topic_id, const TopicStorage& storage) {
  auto& cols = impl_->topic_columns[topic_id];
  // The stored layout wins: it includes the columns expandArray() and ensureColumn() added, which a schema rebuild
  // would miss. Every writer on the topic grows it, so a longer one than this cache means another writer added
  // columns; without taking it, this writer's next column would reuse one of their ids.
  const auto& stored = storage.columnDescriptors();
  if (stored.size() > cols.size()) {
    cols = stored;
    return;
  }
  if (!cols.empty()) {
    return;
  }
  const auto* type_tree = impl_->engine.typeRegistry().lookup(storage.descriptor().schema_id);
  if (type_tree) {
    cols = buildColumnDescriptors(*type_tree);
    return;
  }
  const auto& chunks = storage.sealedChunks();
  if (!chunks.empty()) {
    cols.reserve(chunks[0].columns.size());
    for (const auto& col : chunks[0].columns) {
      cols.push_back(*col.descriptor);
    }
  }
  // A new schemaless topic starts with no columns.
}

TopicChunkBuilder& DataWriter::getOrCreateBuilder(TopicId topic_id, const TopicStorage* storage) {
  auto it = impl_->builders.find(topic_id);
  if (it != impl_->builders.end() && (it->second.rowCount() > 0 || it->second.isRowInProgress())) {
    return it.value();
  }

  // An empty builder, new or kept from the last commit, is fitted to the topic as it is now. That reads the topic's
  // chunks and layout, which other threads change under the engine lock: it is taken here, so a caller that locks only
  // its commits is safe too (the write hosts hold it already).
  const auto lock = impl_->engine.lockEngine();
  if (storage == nullptr) {
    storage = impl_->engine.getTopicStorage(topic_id);
  }
  PJ_ASSERT(storage != nullptr, "get_or_create_builder: topic storage not found");
  if (it != impl_->builders.end()) {
    // Kept across a commit, the builder can be older than a column another writer added since: ensureColumn() and
    // resolveField() hand that column's id out, and a write to it would run past this builder's columns.
    if (storage->columnDescriptors().size() <= it->second.columnCount()) {
      it.value().setMaxRows(builderRows(topic_id, *storage));
      return it.value();
    }
    impl_->builders.erase(it);
  }

  const auto& desc = storage->descriptor();
  ensureColsLoaded(topic_id, *storage);
  auto col_it = impl_->topic_columns.find(topic_id);

  auto [insert_it, inserted] = impl_->builders.emplace(
      std::piecewise_construct, std::forward_as_tuple(topic_id),
      std::forward_as_tuple(topic_id, desc.schema_id, col_it->second, builderRows(topic_id, *storage)));

  return insert_it.value();
}

uint32_t DataWriter::builderRows(TopicId topic_id, const TopicStorage& storage) const {
  // Sized to end where the topic's last chunk fills, counting the rows already set aside for it: the rows committed
  // then complete that chunk, and the builders after it are whole chunks, which TopicStorage::appendRows() encodes
  // straight from the builder. A wrong guess (the rows start a chunk of their own) only costs a copy.
  const uint32_t max_rows = storage.descriptor().max_chunk_rows;
  if (max_rows <= 1) {
    return max_rows;
  }
  uint64_t rows_ahead = 0;
  if (const auto& chunks = storage.sealedChunks(); !chunks.empty()) {
    rows_ahead = chunks.back().stats.row_count;
  }
  if (const auto pending = impl_->pending_batches.find(topic_id); pending != impl_->pending_batches.end()) {
    rows_ahead += pending->second.rows;
  }
  return max_rows - static_cast<uint32_t>(rows_ahead % max_rows);
}

std::vector<ColumnDescriptor> DataWriter::buildColumnDescriptors(const TypeTreeNode& root) {
  std::vector<ColumnDescriptor> result;
  FieldId next_id = 0;

  if (root.kind != TypeKind::kStruct) {
    ColumnDescriptor desc;
    desc.field_id = next_id++;
    desc.field_path = root.name;
    desc.logical_type = root.primitive_type.value_or(PrimitiveType::kFloat64);
    result.push_back(std::move(desc));
    return result;
  }

  for (const auto& child : root.children) {
    flattenColumnsImpl(*child, "", next_id, result);
  }
  return result;
}

void DataWriter::setBuilderAside(TopicId topic_id) {
  auto it = impl_->builders.find(topic_id);
  if (it == impl_->builders.end()) {
    return;
  }
  if (const uint32_t rows = it->second.rowCount(); rows > 0) {
    Impl::Pending& pending = impl_->pending_batches[topic_id];
    const TopicStorage* storage = impl_->engine.getTopicStorage(topic_id);
    if (storage != nullptr && rows >= storage->descriptor().max_chunk_rows) {
      pending.batches.emplace_back(it.value().seal());
    } else {
      pending.batches.emplace_back(std::move(it.value()));
    }
    pending.rows += rows;
  }
  impl_->builders.erase(it);
}

}  // namespace PJ
