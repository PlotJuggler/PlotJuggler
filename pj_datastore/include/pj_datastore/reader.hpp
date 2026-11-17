#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"

namespace PJ {

class DataEngine;

/// Read access to a DataEngine. Each call takes the engine lock; the RangeCursor and SeriesReader it returns keep it
/// until they are destroyed, so do not hold one across work that needs the engine from another thread.
class DataReader {
 public:
  explicit DataReader(const DataEngine& engine);

  [[nodiscard]] std::vector<PJ::DatasetId> listDatasets() const;

  [[nodiscard]] std::vector<PJ::TopicId> listTopics(PJ::DatasetId dataset_id) const;

  /// nullptr if unknown. Returned past the lock: safe because the registry never erases a schema, and only
  /// TypeRegistry::evolveSchema() (no production caller) replaces a tree.
  [[nodiscard]] const PJ::TypeTreeNode* getTypeTree(PJ::TopicId topic_id) const;

  [[nodiscard]] std::optional<TopicMetadata> getMetadata(PJ::TopicId topic_id) const;

  /// e.g. "sensor_msgs/msg/Imu"; nullopt for an unknown or schema-less topic.
  [[nodiscard]] std::optional<std::string> getSchemaName(PJ::TopicId topic_id) const;

  [[nodiscard]] PJ::Expected<RangeCursor> rangeQuery(const QueryRange& range) const;

  /// The row at or before `point.t`, every column read out under the lock (see MaterializedSample).
  [[nodiscard]] PJ::Expected<std::optional<MaterializedSample>> latestAt(const QueryPoint& point) const;

  /// Zero-order hold on a string column, copied out under the lock. nullopt for no row at or before `point.t`, a
  /// column out of range or not a string column. A null cell in the latest row is a sample with a null `value`; it
  /// does not walk back to an older row.
  [[nodiscard]] PJ::Expected<std::optional<StringSample>> latestStringAt(
      const QueryPoint& point, std::size_t column_index) const;

  /// Comes from the schema while no row is stored yet. nullopt if the topic or the column does not exist.
  [[nodiscard]] std::optional<PJ::PrimitiveType> columnType(PJ::TopicId topic_id, std::size_t column_index) const;

  /// A numeric or bool column; null rows are not samples.
  [[nodiscard]] PJ::Expected<SeriesReader> series(PJ::TopicId topic_id, std::size_t column_index) const;

 private:
  const DataEngine& engine_;
};

}  // namespace PJ
