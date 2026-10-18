// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/sdk/object_topic_metadata.hpp"
#include "pj_datastore/object_store.hpp"

// A derived (on-demand) object topic is marked in its ObjectTopicDescriptor's
// metadata_json, never by its name: a name convention would hide the topic from any
// consumer that reads metadata but not names, and collide with a plugin's own topic
// that contains the text (docs/derived_objects/ARCHITECTURE.md §13).

namespace PJ {

/// `metadata_json` an on-demand node's output topic of `type` is registered with:
/// `pj_derived` = `sdk::kDerivedOnDemandValue` (another processor kind, such as the
/// markers', has its own value, so logic for one kind never touches another's topics).
/// `findOrRegisterTopic` reuses a topic only for byte-identical metadata, so equal
/// `type`s must give equal bytes (the builder fixes the key order). A complete
/// SceneEntities / ImageAnnotations snapshot is published on every evaluation (D27), so
/// those two types are marked as snapshot streams and a stateless consumer renders each
/// entry alone. `type` must be a builtin, non-`kNone` type (the caller passes a validated
/// output spec); the unreachable failure returns "{}".
[[nodiscard]] inline std::string derivedTopicMetadata(sdk::BuiltinObjectType type) {
  sdk::ObjectTopicMetadataBuilder builder;
  builder.builtinObjectType(type).string(sdk::kDerivedMetadataKey, sdk::kDerivedOnDemandValue);
  if (type == sdk::BuiltinObjectType::kImageAnnotations || type == sdk::BuiltinObjectType::kSceneEntities) {
    builder.snapshot(true);
  }
  const Expected<std::string> built = builder.build();
  return built.has_value() ? *built : std::string{"{}"};
}

/// True when `descriptor` describes an on-demand derived topic. Parses `metadata_json`:
/// absent, malformed or another kind of metadata is "not derived", not an error. Cheap
/// enough to run over every topic of a dataset (most are rejected without parsing).
[[nodiscard]] inline bool isDerivedObjectTopic(const ObjectTopicDescriptor& descriptor) {
  if (descriptor.metadata_json.find(sdk::kDerivedMetadataKey) == std::string::npos) {
    return false;
  }
  const nlohmann::json doc = nlohmann::json::parse(descriptor.metadata_json, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object()) {
    return false;
  }
  const auto kind = doc.find(std::string(sdk::kDerivedMetadataKey));
  return kind != doc.end() && kind->is_string() && kind->get<std::string>() == sdk::kDerivedOnDemandValue;
}

}  // namespace PJ
