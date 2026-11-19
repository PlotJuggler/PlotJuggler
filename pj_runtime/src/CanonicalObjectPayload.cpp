// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/CanonicalObjectPayload.h"

#include <utility>

#include "ObjectDecode.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_datastore/saturating_time.hpp"
#include "pj_runtime/SessionManager.h"

namespace PJ {

namespace {

// ponytail: only kLog is shifted (what the Log panel and the log widget read); shift other types when a view reads
// their stamp.
bool shiftsStamp(sdk::BuiltinObjectType type, const ResolvedObjectEntry& entry) {
  return type == sdk::BuiltinObjectType::kLog && entry.payload_stamp_shift != 0;
}

/// The canonical bytes of one stored object entry of `type` on `topic` (see canonicalObjectEncoder).
Expected<sdk::PayloadView> canonicalObjectPayload(
    const SessionManager& session, ObjectTopicId topic, sdk::BuiltinObjectType type, const ResolvedObjectEntry& entry) {
  if (!session.parserBindingForObjectTopic(topic) && !shiftsStamp(type, entry)) {
    return entry.payload;
  }
  auto object = decodeStoredObject(session, topic, type, entry);
  if (!object) {
    return unexpected(std::move(object).error());
  }
  auto bytes = serializeBuiltinObject(*object);
  if (!bytes) {
    return unexpected(std::move(bytes).error());
  }
  return sdk::makePayloadView(std::move(*bytes));
}

}  // namespace

Expected<sdk::BuiltinObject> decodeStoredObject(
    const SessionManager& session, ObjectTopicId topic, sdk::BuiltinObjectType type, const ResolvedObjectEntry& entry) {
  auto object = decodeObjectEntry(session.parserBindingForObjectTopic(topic), type, entry.timestamp, entry.payload);
  if (!object) {
    return object;
  }
  // A time-shifted merge moves the entry, not the stamp inside its payload.
  if (const sdk::Log* log = object->get<sdk::Log>();
      shiftsStamp(type, entry) && log != nullptr && log->timestamp_ns != 0) {
    sdk::Log shifted = *log;  // a parser may still share the decoded object
    shifted.timestamp_ns = saturatingAdd(shifted.timestamp_ns, entry.payload_stamp_shift);
    *object = sdk::BuiltinObject(std::move(shifted));
  }
  return object;
}

ObjectPayloadEncoder canonicalObjectEncoder(const SessionManager& session) {
  return [&session](const WireTarget& target, const ResolvedObjectEntry& entry) {
    return canonicalObjectPayload(session, target.object_topic, target.object_type, entry);
  };
}

std::optional<ResolvedObjectEntry> resolveEncodedObject(
    const ObjectStore& objects, const WireTarget& target, SequentialUID uid, const ObjectPayloadEncoder& encoder) {
  auto entry = objects.at(target.object_topic, uid);
  if (!entry || entry->fetch_failed) {
    return std::nullopt;
  }
  if (encoder) {
    auto payload = encoder(target, *entry);
    if (!payload) {
      return std::nullopt;
    }
    entry->payload = std::move(*payload);
  }
  return entry;
}

}  // namespace PJ
