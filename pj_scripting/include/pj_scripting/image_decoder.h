// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/builtin/image.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_scripting/memory_budget.h"

// The headless image codec seam of the object scripts (docs/derived_objects/ARCHITECTURE.md §6): a script
// decodes an image's original samples, independent of any viewer's colormap, rectification or zoom. The session
// injects the codec (`RunOptions::image_decoder`), so scripting links no codec library.

namespace PJ::scripting {

/// Decoded samples: tightly packed, interleaved, mono or RGB[A] in that channel order, as floats in the source's
/// own units (a uint16 depth keeps its millimetres, a float32 depth its metres; nothing is normalized for display).
/// Copies share their samples and their charge. Pixel coordinates start at 0; `origin_x`/`origin_y` place a crop
/// in its source image, so what a crop finds is reported where it is in the image the annotations pair with.
struct DecodedPixels {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t channels = 0;
  std::shared_ptr<const std::vector<float>> samples;
  /// The bytes `samples` hold, charged to the run's native budget until the last copy dies.
  std::shared_ptr<const native::MemoryReservation> charge;
  /// Where the source came from, for the annotations made from it: the store topic of the input it was decoded
  /// from, or its alias when that input has no store topic (empty when it is not an input's), and its own stamp on
  /// the dataset clock (0 is a valid stamp).
  std::string image_topic;
  Timestamp timestamp_ns = 0;
  uint32_t origin_x = 0;
  uint32_t origin_y = 0;
};

/// Decodes an `sdk::Image` into `DecodedPixels`. Called on pool workers, so an implementation must be callable
/// from several threads at once.
class IImageDecoder {
 public:
  virtual ~IImageDecoder() = default;
  /// Reserves on `budget` (null: unbounded) the expanded samples, and any temporary codec storage for as long as
  /// the call holds it, BEFORE allocating them; an image that does not fit fails without reading its pixels. The
  /// result's `charge` holds the samples' bytes; `image_topic`, `timestamp_ns` and the origin are the caller's.
  [[nodiscard]] virtual Expected<DecodedPixels> decode(
      const sdk::Image& image, const std::shared_ptr<native::MemoryBudget>& budget) const = 0;
};

}  // namespace PJ::scripting
