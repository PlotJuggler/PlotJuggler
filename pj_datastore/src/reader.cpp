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
  // The cursor keeps the engine lock for its whole lazy iteration.
  auto lock = engine_.lockEngine();
  const TopicStorage* storage = engine_.getTopicStorage(range.topic_id);
  if (storage == nullptr) {
    return PJ::unexpected(fmt::format("Topic {} not found", range.topic_id));
  }
  // Rows of a straddling chunk under the retention floor are evicted; t_min > t_max yields nothing.
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
  // Read under the lock so no TopicChunk* escapes.
  std::vector<double> values;
  values.reserve(row->chunk->columns.size());
  for (std::size_t col = 0; col < row->chunk->columns.size(); ++col) {
    values.push_back(row->chunk->readNumericAsDouble(col, row->row_index));
  }
  return std::optional<MaterializedSample>{MaterializedSample{row->timestamp, std::move(values)}};
}

PJ::Expected<std::optional<StringSample>> DataReader::latestStringAt(
    const QueryPoint& point, std::size_t column_index) const {
  auto lock = engine_.lockEngine();
  const TopicStorage* storage = engine_.getTopicStorage(point.topic_id);
  if (storage == nullptr) {
    return PJ::unexpected(fmt::format("Topic {} not found", point.topic_id));
  }
  const std::optional<SampleRow> row = PJ::latestAt(storage->sealedChunks(), point.t, storage->retentionFloor());
  if (!row.has_value()) {
    return std::optional<StringSample>{};
  }
  // Chunks from before a mid-stream column add have fewer columns, and readString() on a numeric column is UB. The
  // value is copied under the lock: readString() views chunk memory.
  if (column_index >= row->chunk->columns.size()) {
    return std::optional<StringSample>{};
  }
  const auto& descriptor = row->chunk->columns[column_index].descriptor;
  if (descriptor == nullptr || descriptor->logical_type != PrimitiveType::kString) {
    return std::optional<StringSample>{};
  }
  StringSample sample{
      .timestamp = row->timestamp, .value = std::nullopt, .chunk_id = row->chunk->id, .row_index = row->row_index};
  if (!row->chunk->isNull(column_index, row->row_index)) {
    sample.value = std::string(row->chunk->readString(column_index, row->row_index));
  }
  return std::optional<StringSample>{std::move(sample)};
}

std::optional<PrimitiveType> DataReader::columnType(TopicId topic_id, std::size_t column_index) const {
  const auto lock = engine_.lockEngine();
  const TopicStorage* storage = engine_.getTopicStorage(topic_id);
  if (storage == nullptr) {
    return std::nullopt;
  }
  const std::vector<ColumnDescriptor> columns = columnsForTopic(engine_, *storage);
  if (column_index >= columns.size()) {
    return std::nullopt;
  }
  return columns[column_index].logical_type;
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
