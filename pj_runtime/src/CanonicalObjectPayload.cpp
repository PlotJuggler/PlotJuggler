// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/CanonicalObjectPayload.h"

#include <utility>

#include "ObjectDecode.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_runtime/SessionManager.h"

namespace PJ {

Expected<sdk::PayloadView> canonicalObjectPayload(
    const SessionManager& session, ObjectTopicId topic, sdk::BuiltinObjectType type, const ResolvedObjectEntry& entry) {
  const SessionManager::ParserBinding parser = session.parserBindingForObjectTopic(topic);
  if (!parser) {
    return entry.payload;
  }
  auto object = decodeObjectEntry(parser, type, entry.timestamp, entry.payload);
  if (!object) {
    return unexpected(std::move(object).error());
  }
  auto bytes = serializeBuiltinObject(*object);
  if (!bytes) {
    return unexpected(std::move(bytes).error());
  }
  return sdk::makePayloadView(std::move(*bytes));
}

ObjectPayloadEncoder canonicalObjectEncoder(const SessionManager& session) {
  return [&session](const WireTarget& target, const ResolvedObjectEntry& entry) {
    return canonicalObjectPayload(session, target.object_topic, target.object_type, entry);
  };
}

}  // namespace PJ
