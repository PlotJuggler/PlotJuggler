#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <functional>
#include <memory>

#include "pj_base/buffer_anchor.hpp"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"

namespace PJ {

/// Synchronous object-byte consumer invoked on the source's pushing thread.
/// The payload is valid only for the call; a consumer must decode or copy any
/// state it needs before returning.
using ObjectIngestTapFn = std::function<void(ObjectTopicId, DatasetId, Timestamp store_ts, sdk::PayloadView bytes)>;

/// Session-scoped single-consumer routing for builtin object payloads.
///
/// Claims are wired during application startup, before any runtime host exists.
/// Invocation is nevertheless mutex-protected so registration and teardown do
/// not rely on unchecked map access from ingest workers.
class ObjectIngestTapRegistry {
 private:
  struct State;

 public:
  /// Move-only ownership token that releases its object-kind claim on teardown.
  class TapLease {
   public:
    TapLease() = default;
    ~TapLease();

    TapLease(TapLease&& other) noexcept;
    TapLease& operator=(TapLease&& other) noexcept;

    TapLease(const TapLease&) = delete;
    TapLease& operator=(const TapLease&) = delete;

   private:
    friend class ObjectIngestTapRegistry;
    TapLease(std::shared_ptr<State> state, sdk::BuiltinObjectType kind);
    void release();

    std::shared_ptr<State> state_;
    sdk::BuiltinObjectType kind_ = sdk::BuiltinObjectType::kNone;
  };

  ObjectIngestTapRegistry();
  ~ObjectIngestTapRegistry();

  ObjectIngestTapRegistry(const ObjectIngestTapRegistry&) = delete;
  ObjectIngestTapRegistry& operator=(const ObjectIngestTapRegistry&) = delete;

  /// Claims the sole tap for `kind`; a second owner receives an error.
  [[nodiscard]] Expected<TapLease> claimExclusive(sdk::BuiltinObjectType kind, ObjectIngestTapFn fn);

  /// Whether `kind` currently has a consumer.
  [[nodiscard]] bool claimed(sdk::BuiltinObjectType kind) const;

  /// Invokes the current consumer synchronously on the caller's thread, returning
  /// true after it runs or false when `kind` is unclaimed. Payload bytes are valid
  /// only for the call. Callback exceptions propagate; runtime hosts contain and
  /// count them so object registration can still complete.
  [[nodiscard]] bool invoke(
      sdk::BuiltinObjectType kind, ObjectTopicId topic_id, DatasetId dataset_id, Timestamp store_ts,
      sdk::PayloadView bytes) const;

 private:
  std::shared_ptr<State> state_;
};

}  // namespace PJ
