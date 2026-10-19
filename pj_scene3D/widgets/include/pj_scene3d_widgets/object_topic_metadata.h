// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string_view>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"

namespace pj::scene3d {

/// Decode the 'builtin_object_type' key from an ObjectTopicDescriptor's
/// metadata_json (the store-level metadata contract). Returns kNone when the
/// field is absent, not a string, or unrecognised. Single decoder so the dock
/// and any layer config widget share one parse path and can't silently diverge.
[[nodiscard]] PJ::sdk::BuiltinObjectType builtinObjectTypeFor(const PJ::ObjectTopicDescriptor& descriptor);

/// True when the descriptor marks a snapshot stream (`sdk::kSnapshotMetadataKey` = "true"): every entry is a
/// complete clear-and-replace snapshot, as each result of a derived SceneEntities topic is (D27).
[[nodiscard]] bool isSnapshotStream(const PJ::ObjectTopicDescriptor& descriptor);

struct UniqueObjectTopicResolution {
  std::optional<PJ::ObjectTopicId> topic_id;
  bool ambiguous = false;
};

/// Resolves an unqualified topic only when name and type identify one live entry.
[[nodiscard]] UniqueObjectTopicResolution resolveUniqueObjectTopic(
    PJ::ObjectStore& store, std::string_view topic_name, PJ::sdk::BuiltinObjectType object_type);

}  // namespace pj::scene3d
