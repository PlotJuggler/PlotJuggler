// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/reader.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/type_registry.hpp"

namespace PJ {
namespace {

[[nodiscard]] bool isSeriesValueType(PrimitiveType type) noexcept {
  switch (type) {
    case PrimitiveType::kFloat32:
    case PrimitiveType::kFloat64:
    case PrimitiveType::kInt8:
    case PrimitiveType::kInt16:
    case PrimitiveType::kInt32:
    case PrimitiveType::kInt64:
    case PrimitiveType::kUint8:
    case PrimitiveType::kUint16:
    case PrimitiveType::kUint32:
    case PrimitiveType::kUint64:
    case PrimitiveType::kBool:
      return true;
    case PrimitiveType::kString:
    case PrimitiveType::kUnspecified:
      return false;
  }
  return false;
}

void flattenColumns(
    const TypeTreeNode& node, std::string_view prefix, FieldId& next_id, std::vector<ColumnDescriptor>& out) {
  const std::string path = prefix.empty() ? node.name : std::string(prefix) + "." + node.name;

  if (node.kind == TypeKind::kStruct) {
    for (const auto& child : node.children) {
      flattenColumns(*child, path, next_id, out);
    }
    return;
  }

  if (node.kind == TypeKind::kArray) {
    if (!node.element_type || !node.fixed_array_size.has_value()) {
      return;
    }
    for (uint32_t i = 0; i < *node.fixed_array_size; ++i) {
      const std::string element_path = path + "[" + std::to_string(i) + "]";
      if (node.element_type->kind == TypeKind::kStruct) {
        for (const auto& child : node.element_type->children) {
          flattenColumns(*child, element_path, next_id, out);
        }
      } else {
        out.push_back(
            ColumnDescriptor{
                .field_id = next_id++,
                .logical_type = node.element_type->primitive_type.value_or(PrimitiveType::kFloat64),
                .field_path = element_path,
            });
      }
    }
    return;
  }

  out.push_back(
      ColumnDescriptor{
          .field_id = next_id++,
          .logical_type = node.primitive_type.value_or(PrimitiveType::kFloat64),
          .field_path = path,
      });
}

[[nodiscard]] std::vector<ColumnDescriptor> columnsForTopic(const DataEngine& engine, const TopicStorage& storage) {
  if (!storage.columnDescriptors().empty()) {
    return storage.columnDescriptors();
  }

  const auto& chunks = storage.sealedChunks();
  if (!chunks.empty()) {
    std::vector<ColumnDescriptor> columns;
    columns.reserve(chunks.front().columns.size());
    for (const auto& column : chunks.front().columns) {
      if (column.descriptor) {
        columns.push_back(*column.descriptor);
      }
    }
    return columns;
  }

  const TypeTreeNode* type_tree = engine.typeRegistry().lookup(storage.descriptor().schema_id);
  if (type_tree == nullptr) {
    return {};
  }

  std::vector<ColumnDescriptor> columns;
  FieldId next_id = 0;
  if (type_tree->kind == TypeKind::kStruct) {
    for (const auto& child : type_tree->children) {
      flattenColumns(*child, "", next_id, columns);
    }
  } else {
    flattenColumns(*type_tree, "", next_id, columns);
  }
  return columns;
}

}  // namespace

DataReader::DataReader(const DataEngine& engine) : engine_(engine) {}

std::vector<DatasetId> DataReader::listDatasets() const {
  return engine_.listDatasets();
}

std::vector<TopicId> DataReader::listTopics(DatasetId dataset_id) const {
  return engine_.listTopics(dataset_id);
}

const TypeTreeNode* DataReader::getTypeTree(TopicId topic_id) const {
  auto lock = engine_.lockEngine();
  const TopicStorage* storage = engine_.getTopicStorage(topic_id);
  if (storage == nullptr) {
    return nullptr;
  }
  SchemaId schema_id = storage->descriptor().schema_id;
  return engine_.typeRegistry().lookup(schema_id);
}

std::optional<std::string> DataReader::getSchemaName(TopicId topic_id) const {
  auto lock = engine_.lockEngine();
  const TopicStorage* storage = engine_.getTopicStorage(topic_id);
  if (storage == nullptr) {
    return std::nullopt;
  }
  return engine_.typeRegistry().schemaName(storage->descriptor().schema_id);
}

std::optional<TopicMetadata> DataReader::getMetadata(TopicId topic_id) const {
  auto lock = engine_.lockEngine();
  const TopicStorage* storage = engine_.getTopicStorage(topic_id);
  if (storage == nullptr) {
    return std::nullopt;
  }
  return storage->metadata();  // copied out under the lock
}

Expected<RangeCursor> DataReader::rangeQuery(const QueryRange& range) const {
  // Take the read-lock BEFORE the lookup (a concurrent createTopic rehash could
  // otherwise move storages mid-lookup) and MOVE it into the cursor, which holds
  // it for its lazy-iteration lifetime.
  auto lock = engine_.lockEngine();
  const TopicStorage* storage = engine_.getTopicStorage(range.topic_id);
  if (storage == nullptr) {
    return PJ::unexpected(fmt::format("Topic {} not found", range.topic_id));
  }
  // Raise the lower bound to the topic's retention floor so a straddling chunk's
  // logically-evicted rows are never returned (RangeCursor skips rows < t_min,
  // and yields nothing when t_min > t_max — i.e. a window entirely below floor).
  const Timestamp t_min = std::max(range.t_min, storage->retentionFloor());
  return RangeCursor(storage->sealedChunks(), t_min, range.t_max, std::move(lock));
}

PJ::Expected<std::optional<MaterializedSample>> DataReader::latestAt(const QueryPoint& point) const {
  auto lock = engine_.lockEngine();
  const TopicStorage* storage = engine_.getTopicStorage(point.topic_id);
  if (storage == nullptr) {
    return PJ::unexpected(fmt::format("Topic {} not found", point.topic_id));
  }
  const std::optional<SampleRow> row = PJ::latestAt(storage->sealedChunks(), point.t, storage->retentionFloor());
  if (!row.has_value()) {
    return std::optional<MaterializedSample>{};
  }
  // Read the row's column values WHILE the lock is held so the raw TopicChunk*
  // never escapes. QueryPoint carries no column index; we materialize every
  // column of the row (column N -> values[N]; a scalar topic uses values[0]).
  std::vector<double> values;
  values.reserve(row->chunk->columns.size());
  for (std::size_t col = 0; col < row->chunk->columns.size(); ++col) {
    values.push_back(row->chunk->readNumericAsDouble(col, row->row_index));
  }
  return std::optional<MaterializedSample>{MaterializedSample{row->timestamp, std::move(values)}};
}

PJ::Expected<std::optional<double>> DataReader::latestNumericAt(
    const QueryPoint& point, std::size_t column_index) const {
  auto lock = engine_.lockEngine();
  const TopicStorage* storage = engine_.getTopicStorage(point.topic_id);
  if (storage == nullptr) {
    return PJ::unexpected(fmt::format("Topic {} not found", point.topic_id));
  }
  const std::optional<SampleRow> row = PJ::latestAt(storage->sealedChunks(), point.t, storage->retentionFloor());
  if (!row.has_value()) {
    return std::optional<double>{};
  }
  // Guard the column index (a chunk committed before a mid-stream column add has
  // fewer columns) and null cells — readNumericAsDouble reads a null as 0.0, so
  // an explicit isNull check is what lets the Value column show "-" rather than a
  // fabricated "0.000" for a sparsely-populated field.
  if (column_index >= row->chunk->columns.size() || row->chunk->isNull(column_index, row->row_index)) {
    return std::optional<double>{};
  }
  return std::optional<double>{row->chunk->readNumericAsDouble(column_index, row->row_index)};
}

PJ::Expected<std::optional<std::string>> DataReader::latestStringAt(
    const QueryPoint& point, std::size_t column_index) const {
  auto lock = engine_.lockEngine();
  const TopicStorage* storage = engine_.getTopicStorage(point.topic_id);
  if (storage == nullptr) {
    return PJ::unexpected(fmt::format("Topic {} not found", point.topic_id));
  }
  const std::optional<SampleRow> row = PJ::latestAt(storage->sealedChunks(), point.t, storage->retentionFloor());
  if (!row.has_value()) {
    return std::optional<std::string>{};
  }
  // Guard the column index (chunks before a mid-stream column add have fewer
  // columns), the string-ness of the column (readString is UB on a numeric
  // column), and nulls — then copy the value out WHILE the lock is held, since
  // readString views chunk-internal dictionary memory that must not escape.
  if (column_index >= row->chunk->columns.size()) {
    return std::optional<std::string>{};
  }
  const auto& descriptor = row->chunk->columns[column_index].descriptor;
  if (descriptor == nullptr || descriptor->logical_type != PrimitiveType::kString ||
      row->chunk->isNull(column_index, row->row_index)) {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>{std::string(row->chunk->readString(column_index, row->row_index))};
}

Expected<SeriesReader> DataReader::series(TopicId topic_id, std::size_t column_index) const {
  auto lock = engine_.lockEngine();
  const TopicStorage* storage = engine_.getTopicStorage(topic_id);
  if (storage == nullptr) {
    return PJ::unexpected(fmt::format("Topic {} not found", topic_id));
  }

  const std::vector<ColumnDescriptor> columns = columnsForTopic(engine_, *storage);
  if (column_index >= columns.size()) {
    return PJ::unexpected(fmt::format("Column {} not found in topic {}", column_index, topic_id));
  }

  if (!isSeriesValueType(columns[column_index].logical_type)) {
    return PJ::unexpected(fmt::format("Column {} in topic {} is not a numeric series", column_index, topic_id));
  }

  SeriesReader series(storage->sealedChunks(), column_index, storage->retentionFloor(), std::move(lock));
  series.setSeriesGeneration(storage->seriesGeneration());
  return series;
}

}  // namespace PJ
