#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

namespace PJ {

class ISceneLayer;

/// Serializes a layer's `xmlSaveState` element to a standalone XML string, for copy/paste of settings between
/// layers of one family. Empty when the layer saves no state (a null element).
[[nodiscard]] QString serializeLayerParams(const ISceneLayer& layer);

/// Applies a `serializeLayerParams()` blob through the layer's `xmlLoadState`; the layer's setters emit
/// `repaintRequested()`. False when `xml` is empty or unparseable, or the layer rejects it. The blob is applied
/// as-is and a layer ignores attributes it does not know, so a blob from another family is a harmless no-op; the
/// caller gates paste on matching `SceneLayerInfo::family_name`.
[[nodiscard]] bool applyLayerParams(ISceneLayer& layer, const QString& xml);

}  // namespace PJ
