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

/// How one Arrow column maps to a topic column.
struct ArrowColumnMapping {
  int arrow_column_index;
  std::size_t pj_column_index;
  PrimitiveType pj_type;
  std::string field_name;
};

/// Reads the first message only. Columns of unsupported Arrow types are left out of the mappings; fails when none
/// remain.
[[nodiscard]] PJ::Expected<std::pair<std::shared_ptr<PJ::TypeTreeNode>, std::vector<ArrowColumnMapping>>> schemaFromIpc(
    PJ::Span<const uint8_t> ipc_stream);

/// Imports every record batch of the IPC stream into a DataWriter topic.
///
/// `timestamp_column` indexes the Arrow schema: an int64, uint64 or int32 column taken as nanoseconds, or a TIMESTAMP
/// column scaled from its unit to nanoseconds (a value that overflows int64 nanoseconds is an error). -1 uses row
/// indices 0, 1, 2, ...; a column of any other type is an error, raised before any row is written.
[[nodiscard]] PJ::Status importIpcStream(
    DataWriter& writer, TopicId topic_id, PJ::Span<const uint8_t> ipc_stream,
    const std::vector<ArrowColumnMapping>& mappings, int timestamp_column = -1);

/// Imports every record batch of a live Arrow C Data Interface stream; `mappings` and `timestamp_column` are as for
/// importIpcStream() and refer to the stream's schema.
///
/// Never calls stream->release, on success or failure: the caller decides. The host's ABI wrapper releases on success
/// and keeps the stream on failure.
[[nodiscard]] PJ::Status importArrowStream(
    DataWriter& writer, TopicId topic_id, struct ::ArrowArrayStream* stream,
    const std::vector<ArrowColumnMapping>& mappings, int timestamp_column = -1);

/// Reads the schema only, consuming no batches; the caller keeps ownership of `stream`.
[[nodiscard]] PJ::Expected<std::pair<std::shared_ptr<PJ::TypeTreeNode>, std::vector<ArrowColumnMapping>>>
schemaFromArrowStream(struct ::ArrowArrayStream* stream);

/// Index of the top-level field named `name`, or -1. Unlike the mappings, it finds columns of any Arrow type,
/// including a TIMESTAMP column.
[[nodiscard]] int columnIndexByName(struct ::ArrowArrayStream* stream, std::string_view name);

}  // namespace PJ::arrow_import
