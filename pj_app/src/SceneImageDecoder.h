// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include "pj_scripting/image_decoder.h"

namespace PJ {

/// The headless decoder an on-demand script's `image:decode()` runs through (docs/derived_objects/ARCHITECTURE.md
/// §6): JPEG and PNG through Scene2D's codecs, and the raw encodings (mono8/8UC1, mono16/16UC1 in either byte order,
/// 32FC1, rgb8/bgr8, rgba8/bgra8; `row_step` honoured, 0 meaning tightly packed) converted to RGB[A] or mono floats
/// in their own units: a uint16 depth keeps its millimetres and nothing is normalized for display. Stateless: each
/// call makes its own codec instances, so pool workers may call it at once. Built with Scene2D; `MainWindow` hands it
/// to `SessionManager::setImageDecoder`, so `pj_runtime` links no codec.
class SceneImageDecoder final : public scripting::IImageDecoder {
 public:
  [[nodiscard]] Expected<scripting::DecodedPixels> decode(
      const sdk::Image& image, const std::shared_ptr<scripting::native::MemoryBudget>& budget) const override;
};

}  // namespace PJ
