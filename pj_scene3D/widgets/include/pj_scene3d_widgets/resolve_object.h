// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "pj_base/builtin/builtin_object.hpp"  // BuiltinObjectType
#include "pj_base/sdk/plugin_data_api.hpp"     // ObjectRecord, PayloadView
#include "pj_base/types.hpp"                   // Timestamp, Expected
#include "pj_runtime/SessionManager.h"         // SessionManager::ParserBinding

namespace pj::scene3d {

/// True when `type` has a pj_base canonical wire codec that resolveObject() can decode WITHOUT a MessageParser, so an
/// attach()-time gate can accept a parser-less (canonical) object topic. Reports DECODE capability, NOT that a render
/// layer consumes the type via the canonical path: it is true for more types than are wired (the browser point, pose
/// and occupancy adapters and TF ingest use it; native OccupancyGrid/VoxelGrid/SceneEntities layers still require a
/// parser). A caller using it as an attach gate must already know its own layer renders `type`.
[[nodiscard]] bool hasCanonical3DCodec(PJ::sdk::BuiltinObjectType type) noexcept;

/// Decodes one ObjectStore sample into an ObjectRecord, choosing the decode path every 3D consumer must:
///
///  - **Parser bound** (file / streaming sources whose objects arrive as parser-decoded messages): routed through the
///    topic's MessageParser via parseLocked(); `type` is ignored.
///  - **No parser bound** (a data source / toolbox that pushed an already *serialized canonical* object): the bytes are
///    a pj_base wire blob, decoded with the canonical codec for `type` (the topic's `builtin_object_type` metadata).
///
/// As in pj_scene2D, the HOST always performs the decode, never the producing plugin. The returned object owns/anchors
/// its own bytes (the canonical codecs deep-copy, parsers anchor the payload), so callers may use it after `binding`
/// drops (see parse_locked.h). On the canonical path `ObjectRecord::ts` is nullopt so the host falls back to the store
/// entry's timestamp; per-element stamps (e.g. FrameTransforms) are read from the decoded struct. An error when no
/// parser is bound and `type` has no canonical codec here.
[[nodiscard]] PJ::Expected<PJ::sdk::ObjectRecord> resolveObject(
    const PJ::SessionManager::ParserBinding& binding, PJ::sdk::BuiltinObjectType type, PJ::Timestamp ts,
    const PJ::sdk::PayloadView& payload);

}  // namespace pj::scene3d
