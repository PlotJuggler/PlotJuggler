#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Host side of the pj_plugins C-ABI data surfaces: each class turns one surface's calls into DataWriter, DataEngine
// or ObjectStore operations. raw() is the vtable handed to the plugin; flushPending() commits. Design:
// docs/ARCHITECTURE.md "Plugin host bridges".

#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "pj_base/expected.hpp"
#include "pj_base/plugin_data_api.h"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"

namespace PJ {

class DataEngine;
class ObjectStore;
struct DatastoreSourceWriteHostState;
struct DatastoreSourceObjectWriteHostState;
struct DatastoreParserWriteHostState;
struct DatastoreParserObjectWriteHostState;
struct DatastoreToolboxHostState;
struct DatastoreToolboxObjectReadHostState;

/// Ingest problems that do not fail the write: today, a value stored as null because it does not fit its column's
/// type exactly (1.5 into an int64 column). Reported once per field per write host.
using IngestWarningHandler = void (*)(std::string_view message);

/// Process-wide; nullptr (the default) discards. Safe while ingest threads run.
void setIngestWarningHandler(IngestWarningHandler handler) noexcept;

/// The name the scalar write hosts store a topic under ("//" collapsed to "/"). Another ingest path that finds or
/// creates scalar topics by name must use it too, or one source name maps to two topics.
[[nodiscard]] std::string normalizeTopicName(std::string_view name);

/// `pj.source_write` for one DataSource session.
class DatastoreSourceWriteHost {
 public:
  DatastoreSourceWriteHost(DataEngine& engine, PJ_data_source_handle_t source);
  ~DatastoreSourceWriteHost();

  DatastoreSourceWriteHost(const DatastoreSourceWriteHost&) = delete;
  DatastoreSourceWriteHost& operator=(const DatastoreSourceWriteHost&) = delete;
  DatastoreSourceWriteHost(DatastoreSourceWriteHost&&) noexcept;
  DatastoreSourceWriteHost& operator=(DatastoreSourceWriteHost&&) noexcept;

  [[nodiscard]] PJ_source_write_host_t raw() noexcept;
  /// Commits the rows written so far: readers see them, and no chunk is cut (DataWriter::commitRows). The other hosts'
  /// flushPending() does the same.
  void flushPending();
  /// flushPending(), then re-encodes the last chunk of every topic this host wrote (TopicStorage::compactLastChunk):
  /// for when the source ends. The same on DatastoreParserWriteHost.
  void finish();

  // Flushes pending rows into the current engine, then routes later writes to `target` (the streaming pause/resume
  // swap). Not owning. Takes both engine locks, so it waits for a worker call in progress (an Arrow import holds them
  // for the whole stream).
  void setTarget(DataEngine* target);

  // Every topic and field this host creates (including the columns append* creates on the fly) is also created on
  // `secondary` with the same id, so after a setTarget() swap a plugin's cached handles still resolve. nullptr stops
  // mirroring. Not owning.
  void setSecondaryEngine(DataEngine* secondary);

 private:
  std::unique_ptr<DatastoreSourceWriteHostState> state_;
};

/// `pj.source_object_write.v1` for one DataSource session; new topics go under `dataset_id`.
class DatastoreSourceObjectWriteHost {
 public:
  DatastoreSourceObjectWriteHost(ObjectStore& store, DatasetId dataset_id);
  ~DatastoreSourceObjectWriteHost();

  DatastoreSourceObjectWriteHost(const DatastoreSourceObjectWriteHost&) = delete;
  DatastoreSourceObjectWriteHost& operator=(const DatastoreSourceObjectWriteHost&) = delete;
  DatastoreSourceObjectWriteHost(DatastoreSourceObjectWriteHost&&) noexcept;
  DatastoreSourceObjectWriteHost& operator=(DatastoreSourceObjectWriteHost&&) noexcept;

  [[nodiscard]] PJ_object_write_host_t raw() noexcept;

  // Routes later pushes to `target` (the streaming pause/resume swap). Not owning: keep it alive while the host can
  // still receive pushes.
  void setTarget(ObjectStore* target) noexcept;

 private:
  std::unique_ptr<DatastoreSourceObjectWriteHostState> state_;
};

/// `pj.parser_write`, bound to one host-chosen topic (parsers never name topics).
class DatastoreParserWriteHost {
 public:
  DatastoreParserWriteHost(DataEngine& engine, PJ_topic_handle_t topic);
  ~DatastoreParserWriteHost();

  DatastoreParserWriteHost(const DatastoreParserWriteHost&) = delete;
  DatastoreParserWriteHost& operator=(const DatastoreParserWriteHost&) = delete;
  DatastoreParserWriteHost(DatastoreParserWriteHost&&) noexcept;
  DatastoreParserWriteHost& operator=(DatastoreParserWriteHost&&) noexcept;

  [[nodiscard]] PJ_parser_write_host_t raw() noexcept;
  void flushPending();
  void finish();

  // As DatastoreSourceWriteHost::setTarget(); the bound topic must exist in `target` with the same TopicId (the
  // streaming manager registers it on both engines).
  void setTarget(DataEngine* target);

  // As DatastoreSourceWriteHost::setSecondaryEngine(): parsers that cache FieldHandles across messages keep
  // working across pause/resume.
  void setSecondaryEngine(DataEngine* secondary);

 private:
  std::unique_ptr<DatastoreParserWriteHostState> state_;
};

/// `pj.toolbox_object_read.v1`. Each successful `read_latest_at` gets its own owning handle, so the bytes outlive any
/// later change to the store.
class DatastoreToolboxObjectReadHost {
 public:
  explicit DatastoreToolboxObjectReadHost(ObjectStore& store);
  ~DatastoreToolboxObjectReadHost();

  DatastoreToolboxObjectReadHost(const DatastoreToolboxObjectReadHost&) = delete;
  DatastoreToolboxObjectReadHost& operator=(const DatastoreToolboxObjectReadHost&) = delete;
  DatastoreToolboxObjectReadHost(DatastoreToolboxObjectReadHost&&) noexcept;
  DatastoreToolboxObjectReadHost& operator=(DatastoreToolboxObjectReadHost&&) noexcept;

  [[nodiscard]] PJ_object_read_host_t raw() noexcept;

 private:
  std::unique_ptr<DatastoreToolboxObjectReadHostState> state_;
};

/// `pj.parser_object_write.v1`, bound at construction to one object topic (`topic_id` is its raw
/// ObjectTopicId::id); parsers never name topics.
class DatastoreParserObjectWriteHost {
 public:
  DatastoreParserObjectWriteHost(ObjectStore& store, uint32_t topic_id);
  ~DatastoreParserObjectWriteHost();

  DatastoreParserObjectWriteHost(const DatastoreParserObjectWriteHost&) = delete;
  DatastoreParserObjectWriteHost& operator=(const DatastoreParserObjectWriteHost&) = delete;
  DatastoreParserObjectWriteHost(DatastoreParserObjectWriteHost&&) noexcept;
  DatastoreParserObjectWriteHost& operator=(DatastoreParserObjectWriteHost&&) noexcept;

  [[nodiscard]] PJ_parser_object_write_host_t raw() noexcept;

  // Routes later pushes to `target` (the streaming pause/resume swap); the bound topic must exist there (the
  // streaming manager registers it on both stores). Not owning.
  void setTarget(ObjectStore* target) noexcept;

 private:
  std::unique_ptr<DatastoreParserObjectWriteHostState> state_;
};

/// The toolbox C ABI over both stores: scalar and Arrow columns into the DataEngine, media payloads into the
/// ObjectStore, through one host pointer.
class DatastoreToolboxHost {
 public:
  /// raw()'s vtable also serves `acquire_catalog_snapshot_v2`: v1's scalar catalog plus every object topic, with its
  /// dataset as `source`, the canonical `PJ::sdk::name()` of its `builtin_object_type` ("" when unknown), its whole
  /// metadata, and the entry count and raw time range it retains now (a derived topic's are its ring's). The scalar
  /// half is read under the engine lock and the object half after it is released, since the two locks never nest;
  /// the two halves are therefore not one atomic snapshot.
  DatastoreToolboxHost(DataEngine& engine, ObjectStore& object_store);
  ~DatastoreToolboxHost();

  DatastoreToolboxHost(const DatastoreToolboxHost&) = delete;
  DatastoreToolboxHost& operator=(const DatastoreToolboxHost&) = delete;
  DatastoreToolboxHost(DatastoreToolboxHost&&) noexcept;
  DatastoreToolboxHost& operator=(DatastoreToolboxHost&&) noexcept;

  [[nodiscard]] PJ_toolbox_host_t raw() noexcept;
  void flushPending();

 private:
  std::unique_ptr<DatastoreToolboxHostState> state_;
};

/// `read_series_arrow` limited to the samples stamped in [first, last] (raw ns), plus the last one before `first`,
/// whose value holds at the window's start (none when the series starts inside the window or that sample is below
/// the retention floor). [min, max] reads the whole series. Decodes only the chunks the window touches. Same layout
/// and ownership as `read_series_arrow`: a struct of ["timestamp" int64, <field path>], both structs released by
/// the caller. Finishes a derived series' pending replay first (DataEngine::awaitCompleteSeries(), effective only
/// for a GUI-thread caller).
[[nodiscard]] Status readSeriesArrowWindow(
    const DataEngine& engine, PJ_field_handle_t field, Timestamp first, Timestamp last, struct ArrowSchema* out_schema,
    struct ArrowArray* out_array);

/// Consecutive samples: `values` holds one double per sample (NaN at a null), or a single value they all share.
using SampleBlockCallback = std::function<void(Span<const Timestamp> timestamps, Span<const double> values)>;

/// The samples `read_series_arrow` returns for `field`, stamped at or after `first`, in time order, one chunk per
/// block and without building an Arrow array: the cheap pass for whole-series facts. A constant-encoded chunk is one
/// block of a single value; a non-numeric field has none. Waits for a derived series as readSeriesArrowWindow()
/// does. `on_block` runs under the engine lock, must not call the engine, and its spans are valid only during the
/// call.
[[nodiscard]] Status scanSeriesValues(
    const DataEngine& engine, PJ_field_handle_t field, const SampleBlockCallback& on_block,
    Timestamp first = std::numeric_limits<Timestamp>::min());

}  // namespace PJ
