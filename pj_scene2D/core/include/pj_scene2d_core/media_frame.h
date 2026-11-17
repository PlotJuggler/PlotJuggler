#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <optional>
#include <vector>

#include "pj_scene2d_core/decoded_frame.h"
#include "pj_scene2d_core/scene_frame.h"

namespace PJ {

/// One ordered pixel layer in a MediaFrame. Layers are composited bottom to top.
struct PixelLayer {
  DecodedFrame frame;
  float opacity = 1.0f;
};

/// What a MediaSource delivers. `pixel_layers` are drawn bottom to top; when empty, `base` is the only pixel layer.
/// `overlays` are drawn above all pixels. An empty MediaFrame is a valid result meaning "nothing to show".
struct MediaFrame {
  std::optional<DecodedFrame> base;
  std::vector<PixelLayer> pixel_layers;
  std::vector<SceneFrame> overlays;

  [[nodiscard]] bool empty() const noexcept {
    return !base.has_value() && pixel_layers.empty() && overlays.empty();
  }
};

}  // namespace PJ
