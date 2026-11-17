// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>

#include "pj_base/builtin/builtin_object.hpp"

namespace PJ {

// Forward to each family's handlesObjectType, which owns its accepted type set; false when the family is not
// built.
[[nodiscard]] bool is3dSceneObjectType(sdk::BuiltinObjectType type);
[[nodiscard]] bool is2dSceneObjectType(sdk::BuiltinObjectType type);

// The types a 2D media viewer renders. A depth-encoded kImage is also hostable in 3D, but depth vs color is in
// the payload's `encoding`, not the type, so drop routing treats the whole family as 2D-first; depth to 3D is
// an explicit action (a drop onto a 3D dock, or "Open in 3D view").
[[nodiscard]] bool isImageFamilyObjectType(sdk::BuiltinObjectType type);

// The drop-routing decision: "scene2d", "scene3d", or empty when no built family accepts `type`. Image-family
// types never fall back to 3D (a color image would be rejected there); a type both families claim
// (kSceneEntities) goes to 3D.
[[nodiscard]] QString sceneKindForObjectType(sdk::BuiltinObjectType type);

// So the curve list never offers a drag that can only end in "cannot display".
[[nodiscard]] bool isDroppableObjectType(sdk::BuiltinObjectType type);

// An object type no dock family claims that a Custom Widget input can still take (panel-only types excluded):
// its catalog row starts a drag, and an empty dock answers a drop by saying a Custom Widget shows it.
[[nodiscard]] bool isCustomWidgetObjectType(sdk::BuiltinObjectType type);

}  // namespace PJ
