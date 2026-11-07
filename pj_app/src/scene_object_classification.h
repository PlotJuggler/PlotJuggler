// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>

#include "pj_base/builtin/builtin_object.hpp"

namespace PJ {

// Host-side routing/iconography forwarders. Each scene family owns its accepted
// type set (the dock's static handlesObjectType, next to the code implementing
// support); these aliases exist so shell code reads as policy. kSceneEntities is
// handled by both families; the factory ladder gives 3D precedence (3D is tried
// first, 2D falls back for types the 3D family does not claim).
[[nodiscard]] bool is3dSceneObjectType(sdk::BuiltinObjectType type);
[[nodiscard]] bool is2dSceneObjectType(sdk::BuiltinObjectType type);

// The image-family object types — those a 2D media viewer renders: stills
// (kImage, raw or compressed), depth images (kImage with a depth encoding, or
// kDepthImage), video (kVideoFrame), and image annotations. Used for the curve-
// list "2D-viewable" icon and for placeholder-drop routing.
//
// A depth-encoded kImage is ALSO hostable in 3D (DepthCloud), but the depth-vs-
// color split is an `encoding` (payload) distinction, not a type one — so it is
// resolved by the consumer (the dock peeking a sample), never here. Because this
// predicate cannot tell depth from color, placeholder-drop routing treats the
// whole family as 2D-first; depth→3D is an explicit action (drop onto an
// existing 3D dock, or "Open in 3D view").
[[nodiscard]] bool isImageFamilyObjectType(sdk::BuiltinObjectType type);

// The drop-routing decision, in one place: the object-dock kind ("scene2d" /
// "scene3d") a catalog drop of `type` resolves to, or empty when no compiled
// family accepts it. Image-family types route 2D-first and never fall back to
// 3D (a color image must not land in a 3D dock that would reject it; depth→3D
// stays an explicit action); kSceneEntities is claimed by both families and 3D
// wins. MainWindow's object-dock factory consumes this directly.
[[nodiscard]] QString sceneKindForObjectType(sdk::BuiltinObjectType type);

// True when a drop of `type` resolves to a dock that accepts it — so the curve
// list never offers a drag that could only end in the "cannot display"
// fallback. Types outside every claim (kOccupancyGridUpdate, ...) are consumed
// indirectly or have no viewer yet; kCameraInfo is also outside, but a plugin
// panel reads it (isPanelOnlyObjectType).
[[nodiscard]] bool isDroppableObjectType(sdk::BuiltinObjectType type);

}  // namespace PJ
