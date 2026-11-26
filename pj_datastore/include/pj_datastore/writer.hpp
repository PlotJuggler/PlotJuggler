#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/topic_storage.hpp"

namespace PJ {

class DataEngine;

struct TopicWriteHandle {
  PJ::TopicId topic_id;
  /// In writer column order.
  std::vector<PJ::FieldId> field_ids;
};

/// A topic with one numeric column.
struct ScalarSeriesHandle {
  PJ::TopicId topic_id;
  PJ::FieldId value_field;
};

/// One column's data for appendColumns().
struct ColumnData {
  std::size_t col_index;

  /// Arrow layout: offsets plus concatenated bytes.
  struct StringData {
    PJ::Span<const uint32_t> offsets;  // (row_count + 1) entries
    PJ::Span<const char> values;
  };

  /// The alternative decides the StorageKind.
  using Data = std::variant<
      PJ::Span<const float>,     // kFloat32
      PJ::Span<const double>,    // kFloat64
      PJ::Span<const int32_t>,   // kInt32
      PJ::Span<const int64_t>,   // kInt64
      PJ::Span<const uint64_t>,  // kUint64
      PJ::Span<const uint8_t>,   // kBool (one byte per bool)
      StringData                 // kString
      >;

  Data data;

  /// Empty means every row is valid.
  PJ::BitSpan validity;

  [[nodiscard]] std::size_t rowCount() const;

  [[nodiscard]] StorageKind kind() const;

  static ColumnData float32(std::size_t col, PJ::Span<const float> values, PJ::BitSpan validity = {}) {
    return {col, Data{values}, validity};
  }
  static ColumnData float64(std::size_t col, PJ::Span<const double> values, PJ::BitSpan validity = {}) {
    return {col, Data{values}, validity};
  }
  static ColumnData int32(std::size_t col, PJ::Span<const int32_t> values, PJ::BitSpan validity = {}) {
    return {col, Data{values}, validity};
  }
  static ColumnData int64(std::size_t col, PJ::Span<const int64_t> values, PJ::BitSpan validity = {}) {
    return {col, Data{values}, validity};
  }
  static ColumnData uint64(std::size_t col, PJ::Span<const uint64_t> values, PJ::BitSpan validity = {}) {
    return {col, Data{values}, validity};
  }
  static ColumnData boolean(std::size_t col, PJ::Span<const uint8_t> values, PJ::BitSpan validity = {}) {
    return {col, Data{values}, validity};
  }
  static ColumnData string(
      std::size_t col, PJ::Span<const uint32_t> offsets, PJ::Span<const char> str_data, PJ::BitSpan validity = {}) {
    return {col, Data{StringData{offsets, str_data}}, validity};
  }
};

/// Builds rows into per-topic chunk builders; nothing is visible to readers until
/// commitRows() or `engine.commitChunks(writer.flushAll())`. Never takes the engine lock, though it reads and changes
/// TopicStorage: when another thread can see the engine, call it under lockEngine() (the C-ABI write hosts do).
class DataWriter {
 public:
  explicit DataWriter(DataEngine& engine);

  ~DataWriter();
  DataWriter(DataWriter&&) noexcept;
  DataWriter& operator=(DataWriter&&) noexcept;
  DataWriter(const DataWriter&) = delete;
  DataWriter& operator=(const DataWriter&) = delete;

  // ---- Schema registration (delegates to engine's TypeRegistry) ----
  [[nodiscard]] PJ::Expected<PJ::SchemaId> registerSchema(
      std::string schema_name, std::shared_ptr<PJ::TypeTreeNode> type_tree);

  // ---- Topic registration ----
  [[nodiscard]] PJ::Expected<PJ::TopicId> registerTopic(PJ::DatasetId dataset_id, TopicDescriptor descriptor);

  // ---- Bind for fast path ----
  /// Resolves and caches the topic's columns.
  [[nodiscard]] PJ::Expected<TopicWriteHandle> bindTopicWriter(PJ::TopicId topic_id);

  // ---- Field resolution ----
  [[nodiscard]] PJ::Expected<PJ::FieldId> resolveField(PJ::TopicId topic_id, std::string_view field_path);

  // ---- Row-at-a-time append ----
  [[nodiscard]] PJ::Status beginRow(PJ::TopicId topic_id, PJ::Timestamp t);

  /// Columns not set in the row become null. Errors when no row was begun.
  [[nodiscard]] PJ::Status finishRow(PJ::TopicId topic_id);

  /// T: float, double, int32_t, int64_t, uint64_t, bool, std::string_view.
  template <typename T>
  void set(PJ::TopicId topic_id, std::size_t col_index, T value);

  void setNull(PJ::TopicId topic_id, std::size_t col_index);

  // ---- Bulk column append ----
  /// Columns aligned with `timestamps`; splits into chunks as needed.
  [[nodiscard]] PJ::Status appendColumns(
      PJ::TopicId topic_id, PJ::Span<const PJ::Timestamp> timestamps, PJ::Span<const ColumnData> columns);

  // ---- Scalar convenience API ----
  /// Stores the column layout at once, so the topic can feed a transform before any data is committed.
  [[nodiscard]] PJ::Expected<ScalarSeriesHandle> registerScalarSeries(
      PJ::DatasetId dataset_id, std::string_view topic_name, PJ::NumericType value_type);
  void appendScalar(const ScalarSeriesHandle& handle, PJ::Timestamp t, PJ::NumericValue value);

  // ---- Dynamic column addition ----
  /// Returns the FieldId of `field_path`, adding the column if new, at the end of the topic's layout: the chunks that
  /// filled before have fewer columns. Errors for an existing path of another type, or a new column while a row is in
  /// progress. On a topic with a schema, an added column exists only in the physical layout, not in getTypeTree().
  [[nodiscard]] PJ::Expected<PJ::FieldId> ensureColumn(
      PJ::TopicId topic_id, std::string_view field_path, PJ::PrimitiveType type);

  // ---- Variable-length array expansion ----
  /// Grows the element columns of a variable-length array to at least `new_length`, never past the topic's
  /// array_expansion_limit (a clamp is counted as a truncation), and returns the count reached. Not inside a row.
  /// With a schema the field is validated and `element_type` ignored; without one any path is accepted with
  /// `element_type`.
  [[nodiscard]] PJ::Expected<uint32_t> expandArray(
      PJ::TopicId topic_id, std::string_view array_field_path, uint32_t new_length,
      PJ::PrimitiveType element_type = PJ::PrimitiveType::kFloat64);

  // ---- Flush ----
  /// Seals only finished rows.
  [[nodiscard]] std::vector<TopicChunk> flush(PJ::TopicId topic_id);

  [[nodiscard]] std::vector<std::pair<PJ::TopicId, TopicChunk>> flushAll();

  /// Commits every finished row to the engine and returns the sorted ids of the topics that took some, as
  /// DataEngine::commitChunks() does. Unlike `commitChunks(flushAll())`, the rows extend each topic's last chunk
  /// (TopicStorage::appendRows), so how often this is called does not decide where chunks end. Not inside a row.
  std::vector<PJ::TopicId> commitRows();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // `storage` is the topic's, when the caller has it already.
  TopicChunkBuilder& getOrCreateBuilder(PJ::TopicId topic_id, const TopicStorage* storage = nullptr);

  // The rows a new or emptied builder of the topic takes before it is set aside.
  [[nodiscard]] uint32_t builderRows(PJ::TopicId topic_id, const TopicStorage& storage) const;

  void ensureColsLoaded(PJ::TopicId topic_id, const TopicStorage& storage);

  static std::vector<ColumnDescriptor> buildColumnDescriptors(const PJ::TypeTreeNode& root);

  // Sets the topic's builder aside until the next flush or commitRows(), sealed if it holds a whole chunk; an empty
  // one is dropped.
  void setBuilderAside(PJ::TopicId topic_id);
};

}  // namespace PJ
