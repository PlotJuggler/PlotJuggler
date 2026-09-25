#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/plugin_data_api.h"  // for ArrowArrayStream forward-declared in the Arrow C Data Interface block
#include "pj_base/span.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ::arrow_import {

/// Describes how an Arrow column maps to a PJ topic column.
struct ArrowColumnMapping {
  /// Source column index in Arrow batch/table schema.
  int arrow_column_index;
  /// Destination column index in PJ flattened schema.
  std::size_t pj_column_index;
  /// Target primitive type used by PJ storage.
  PrimitiveType pj_type;
  /// Source field name.
  std::string field_name;
};

/// Parse schema from Arrow IPC stream bytes (reads first message).
/// Returns a TypeTreeNode and column mappings for supported types.
/// Unsupported Arrow types are skipped.
[[nodiscard]] PJ::Expected<std::pair<std::shared_ptr<PJ::TypeTreeNode>, std::vector<ArrowColumnMapping>>> schemaFromIpc(
    PJ::Span<const uint8_t> ipc_stream);

/// Import all record batches from Arrow IPC stream bytes into a DataWriter topic.
///
/// timestamp_column: which Arrow column contains timestamps (as int64).
///   If -1, row indices (0, 1, 2, ...) are used as timestamps.
[[nodiscard]] PJ::Status importIpcStream(
    DataWriter& writer, TopicId topic_id, PJ::Span<const uint8_t> ipc_stream,
    const std::vector<ArrowColumnMapping>& mappings, int timestamp_column = -1);

/// Import all record batches from a live Arrow C Data Interface stream.
/// This is the v4 in-memory path — no IPC parse, plugin hands the stream.
///
/// Ownership: on success, the caller retains responsibility for releasing
/// @p stream (the importer does NOT call stream->release). This lets the
/// caller enforce the ownership contract on the ABI boundary: host-side
/// code that got the stream from a plugin releases on success, retains on
/// failure, all at the outermost ABI frame.
///
/// The mappings vector must match the stream's schema (same columns in
/// the same order). @p timestamp_column is an index into the stream's
/// schema, or -1 for synthetic sequential timestamps.
[[nodiscard]] PJ::Status importArrowStream(
    DataWriter& writer, TopicId topic_id, struct ::ArrowArrayStream* stream,
    const std::vector<ArrowColumnMapping>& mappings, int timestamp_column = -1);

/// Parse schema from a live Arrow C Data Interface stream (reads schema only;
/// does not consume batches). Caller retains ownership of @p stream.
[[nodiscard]] PJ::Expected<std::pair<std::shared_ptr<PJ::TypeTreeNode>, std::vector<ArrowColumnMapping>>>
schemaFromArrowStream(struct ::ArrowArrayStream* stream);

/// Index of the top-level field named @p name in the stream's schema, or -1.
/// Covers every Arrow type — including a native TIMESTAMP column, which the
/// value-column mappings of schemaFromArrowStream() leave out.
[[nodiscard]] int columnIndexByName(struct ::ArrowArrayStream* stream, std::string_view name);

}  // namespace PJ::arrow_import
