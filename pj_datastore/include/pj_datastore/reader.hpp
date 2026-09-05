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

/// Read-only facade over committed DataEngine storage.
/// Provides listing, metadata, type-tree lookup, range queries,
/// and latest-at point queries.
class DataReader {
 public:
  /// Create a read-only facade bound to `engine`.
  explicit DataReader(const DataEngine& engine);

  /// List all dataset ids known by the engine.
  [[nodiscard]] std::vector<PJ::DatasetId> listDatasets() const;

  /// List topic ids for one dataset.
  [[nodiscard]] std::vector<PJ::TopicId> listTopics(PJ::DatasetId dataset_id) const;

  /// Lookup schema tree for a topic (nullptr if unknown).
  [[nodiscard]] const PJ::TypeTreeNode* getTypeTree(PJ::TopicId topic_id) const;

  /// Return topic metadata if topic exists.
  [[nodiscard]] std::optional<TopicMetadata> getMetadata(PJ::TopicId topic_id) const;

  /// Registered schema name of a topic (e.g. "sensor_msgs/msg/Imu"); nullopt when
  /// the topic is unknown or was created without a named schema (schema_id 0).
  [[nodiscard]] std::optional<std::string> getSchemaName(PJ::TopicId topic_id) const;

  /// Create range cursor over [t_min, t_max].
  [[nodiscard]] PJ::Expected<RangeCursor> rangeQuery(const QueryRange& range) const;

  /// Return the latest scalar sample at or before query time; nullopt payload if
  /// no row exists. The value is materialized (read out under the engine lock)
  /// into a MaterializedSample, so no raw TopicChunk* escapes — `value` is the
  /// topic's scalar column (index 0).
  [[nodiscard]] PJ::Expected<std::optional<MaterializedSample>> latestAt(const QueryPoint& point) const;

  /// Latest numeric value at or before query time for column `column_index`
  /// (zero-order hold), as a double. Unlike latestAt (which materializes the
  /// whole row via readNumericAsDouble and reads a null cell as 0.0), this
  /// null-checks: nullopt payload when no row exists at or before `point.t`, the
  /// column index is out of range, or the latest cell is null — so a missing
  /// value is distinguishable from a real 0.
  [[nodiscard]] PJ::Expected<std::optional<double>> latestNumericAt(
      const QueryPoint& point, std::size_t column_index) const;

  /// Latest string value at or before query time for column `column_index`
  /// (zero-order hold), copied into a std::string under the engine lock so no
  /// chunk-internal dictionary memory escapes. nullopt payload when no row exists
  /// at or before `point.t`, the column index is out of range, the value is null,
  /// or the column is not a string column.
  [[nodiscard]] PJ::Expected<std::optional<std::string>> latestStringAt(
      const QueryPoint& point, std::size_t column_index) const;

  /// Create a series view over one numeric/bool topic column. The returned
  /// reader exposes only value-bearing samples; null rows are skipped.
  [[nodiscard]] PJ::Expected<SeriesReader> series(PJ::TopicId topic_id, std::size_t column_index) const;

 private:
  const DataEngine& engine_;
};

}  // namespace PJ
