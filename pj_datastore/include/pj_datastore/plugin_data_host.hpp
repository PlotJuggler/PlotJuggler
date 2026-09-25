#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Host-side bridges that translate pj_plugins C-ABI write/read calls into
// DataWriter / DataEngine / ObjectStore operations. raw() yields the C vtable;
// flushPending() seals+commits. See docs/OBJECT_STORE_DESIGN.md (ABI bridge).

#include <memory>
#include <string_view>

#include "pj_base/plugin_data_api.h"
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

/// Receives ingest diagnostics that do not fail the write: today, a field value
/// stored as null because it does not fit its column's type exactly (e.g. 1.5
/// into an int64 column). Reported once per field per write host.
using IngestWarningHandler = void (*)(std::string_view message);

/// Install the process-wide IngestWarningHandler (nullptr, the default, discards).
/// Safe to call while ingest threads run.
void setIngestWarningHandler(IngestWarningHandler handler) noexcept;

/// Bridges the `pj.source_write` C ABI onto DataWriter for a DataSource session.
/// Owns a pimpl state; flushPending() seals+commits accumulated scalar rows.
/// setTarget() supports the streaming two-engine pause/resume swap.
class DatastoreSourceWriteHost {
 public:
  DatastoreSourceWriteHost(DataEngine& engine, PJ_data_source_handle_t source);
  ~DatastoreSourceWriteHost();

  DatastoreSourceWriteHost(const DatastoreSourceWriteHost&) = delete;
  DatastoreSourceWriteHost& operator=(const DatastoreSourceWriteHost&) = delete;
  DatastoreSourceWriteHost(DatastoreSourceWriteHost&&) noexcept;
  DatastoreSourceWriteHost& operator=(DatastoreSourceWriteHost&&) noexcept;

  [[nodiscard]] PJ_source_write_host_t raw() noexcept;
  void flushPending();

  // Atomically swap the destination DataEngine. Mirrors the parser write
  // host's setTarget: the streaming two-engine flow routes source-level
  // scalar pushes to a secondary DataEngine during pause and back to the
  // primary on resume. Pending rows are flushed to the current engine
  // before the switch. Does not take ownership.
  void setTarget(DataEngine* target);

  // Wire the secondary DataEngine for the streaming two-engine lockstep.
  // Every ensureTopic/ensureField — including the lazy column auto-create
  // inside append* — replays on the secondary using DataEngine::createTopic
  // and DataEngine::createTopicField with the SAME id, so a setTarget swap
  // never lands on an engine missing the topic/field a cached plugin handle
  // refers to. Pass nullptr to disable mirroring. Does not take ownership.
  void setSecondaryEngine(DataEngine* secondary);

 private:
  std::unique_ptr<DatastoreSourceWriteHostState> state_;
};

/// Host-side implementation of the scalar-peer object-write surface exposed
/// as `pj.source_object_write.v1`. Bridges the C ABI onto
/// `pj_datastore::ObjectStore`. One instance per DataSource session; the
/// `DatasetId` scopes newly-registered topics to the enclosing dataset.
class DatastoreSourceObjectWriteHost {
 public:
  DatastoreSourceObjectWriteHost(ObjectStore& store, DatasetId dataset_id);
  ~DatastoreSourceObjectWriteHost();

  DatastoreSourceObjectWriteHost(const DatastoreSourceObjectWriteHost&) = delete;
  DatastoreSourceObjectWriteHost& operator=(const DatastoreSourceObjectWriteHost&) = delete;
  DatastoreSourceObjectWriteHost(DatastoreSourceObjectWriteHost&&) noexcept;
  DatastoreSourceObjectWriteHost& operator=(DatastoreSourceObjectWriteHost&&) noexcept;

  [[nodiscard]] PJ_object_write_host_t raw() noexcept;

  // Atomically swap the destination store. Used by the streaming two-store
  // flow to route pushes to a secondary ObjectStore during pause and back to
  // the primary on resume. Must point to a live store; the host does not
  // take ownership and the caller must keep the target alive for as long as
  // the host can receive pushes against it.
  void setTarget(ObjectStore* target) noexcept;

 private:
  std::unique_ptr<DatastoreSourceObjectWriteHostState> state_;
};

/// Bridges the `pj.parser_write` C ABI onto DataWriter, bound to one host-chosen
/// topic (parsers never name topics). flushPending() seals+commits; setTarget()
/// is the streaming two-engine swap (bound topic must exist on the target).
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

  // Atomically swap the destination DataEngine. Mirrors the object write
  // host's setTarget: the streaming two-store flow routes scalar pushes to a
  // secondary DataEngine during pause and back to the primary on resume.
  // Pending rows are flushed to the current engine before the switch; the
  // bound topic must exist in `target` with the same TopicId (the streaming
  // manager registers it lockstep on both engines). Does not take ownership.
  void setTarget(DataEngine* target);

  // Wire the secondary DataEngine for streaming two-engine lockstep — see
  // DatastoreSourceWriteHost::setSecondaryEngine. Closes the latent
  // FieldHandle-stale bug for parser plugins that cache handles across
  // messages (parser_protobuf et al.) on pause/resume.
  void setSecondaryEngine(DataEngine* secondary);

 private:
  std::unique_ptr<DatastoreParserWriteHostState> state_;
};

/// Host-side implementation of the toolbox object-read surface exposed as
/// `pj.toolbox_object_read.v1`. Bridges the C ABI onto
/// `pj_datastore::ObjectStore`, allocating an owning handle per successful
/// `read_latest_at`. The handle keeps bytes alive independent of the
/// store's internal state, matching the `shared_ptr` model.
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

/// Host-side implementation of the parser-scoped object write surface
/// exposed as `pj.parser_object_write.v1`. The target ObjectTopic is bound
/// at construction time (matching the scalar `DatastoreParserWriteHost`
/// pattern); the parser never names topics.
///
/// @param topic_id the raw `ObjectTopicId::id` of the bound topic.
class DatastoreParserObjectWriteHost {
 public:
  DatastoreParserObjectWriteHost(ObjectStore& store, uint32_t topic_id);
  ~DatastoreParserObjectWriteHost();

  DatastoreParserObjectWriteHost(const DatastoreParserObjectWriteHost&) = delete;
  DatastoreParserObjectWriteHost& operator=(const DatastoreParserObjectWriteHost&) = delete;
  DatastoreParserObjectWriteHost(DatastoreParserObjectWriteHost&&) noexcept;
  DatastoreParserObjectWriteHost& operator=(DatastoreParserObjectWriteHost&&) noexcept;

  [[nodiscard]] PJ_parser_object_write_host_t raw() noexcept;

  // Atomically swap the destination store. Used by the streaming two-store
  // flow to route pushes to a secondary ObjectStore during pause and back to
  // the primary on resume. The bound topic id must exist in `target` (the
  // streaming manager ensures this via lockstep registerTopic on both
  // stores). The host does not take ownership of the target.
  void setTarget(ObjectStore* target) noexcept;

 private:
  std::unique_ptr<DatastoreParserObjectWriteHostState> state_;
};

/// Bridges the toolbox C ABI onto both a DataEngine (scalar/Arrow columns) and an
/// ObjectStore (media blobs); a toolbox plugin writes into either via one host
/// fat pointer. raw() yields the vtable; flushPending() seals+commits.
class DatastoreToolboxHost {
 public:
  /// Construct with both an engine (scalar/Arrow column writes) and an
  /// object store (canonical media payloads — images, point clouds,
  /// annotations). The two are independent storage backends; toolbox
  /// plugins write into one or both via the same host fat pointer.
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

}  // namespace PJ
