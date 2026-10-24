// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <nlohmann/json.hpp>

#include "pj_base/builtin/builtin_object.hpp"

// A small JSON description of an object output's content, for an evaluation report: a report
// never carries object bytes, only this summary (the `summary` key of `poll_evaluation`'s report,
// pj_base/plugin_data_api.h).

namespace PJ::scripting {

/// One JSON object per builtin type, enough to judge a result without decoding its bytes. Every
/// summary carries "type" (`sdk::name`); the rest is per type:
///   - kPointCloud: "frame_id", "points", and "bounds" {min, max} (omitted when no finite point or no
///     x/y/z channels);
///   - kSceneEntities: "entities", "deletions" (list sizes), "frame_ids" (the entities' unique frame ids,
///     in order of first appearance);
///   - kImageAnnotations: "circles", "points", "texts" (the primitive list sizes);
///   - kFrameTransforms: "transforms" (list size), "parents" (unique parent frame ids, in order);
///   - any other type: "type" only.
[[nodiscard]] nlohmann::json summarizeObject(const sdk::BuiltinObject& object);

/// Whether `object` has no content in the sense of `summarizeObject`'s counts: a cloud with no points, scene
/// entities with no entity (deletions are not content: a clear-only snapshot is empty), annotations with no
/// primitive, transforms with no transform. Never true for another type. A report's "empty" status, distinct
/// from "unavailable" (D25).
[[nodiscard]] bool isEmptyObject(const sdk::BuiltinObject& object);

}  // namespace PJ::scripting
