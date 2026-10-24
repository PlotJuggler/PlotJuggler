// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/builtin/image_annotations.hpp"
#include "pj_base/expected.hpp"
#include "pj_scripting/memory_budget.h"

// Native ImageAnnotations builder behind a script's annotations builder. Luau-free, so the Luau and Python
// binders share it, like scene_entities_ops.h.
namespace PJ::scripting::native {

/// Accumulates the pixel-space primitives (origin top left) drawn over ONE image. Every primitive is
/// charged to the evaluation's budget before it is stored (its own size plus its text or points; vector
/// slack is not charged), and every rejection, the budget's aside, names the offending field. The
/// annotations leave unstamped (timestamp 0): the builder knows no image, so the binder pairs them with the image
/// input they name (`ScriptMedia::pairing`).
class ImageAnnotationsBuilder {
 public:
  /// `image_topic` names the image the annotations belong to (the compositor pairs them by it) and must not
  /// be empty. `budget` null: unbounded.
  [[nodiscard]] static Expected<ImageAnnotationsBuilder> create(
      std::string image_topic, std::shared_ptr<MemoryBudget> budget);

  /// A polyline or point set. `type` is points, line_list (an even count), line_strip (at least 2 points)
  /// or line_loop (at least 3); `fill_color` fills a line_loop only, a = 0 meaning no fill.
  [[nodiscard]] Status addPoints(
      std::vector<sdk::Point2> points, std::string_view type, double thickness, sdk::ColorRGBA color,
      sdk::ColorRGBA fill_color);
  /// A circle of `diameter` (stored as the SDK's radius).
  [[nodiscard]] Status addCircle(
      sdk::Point2 center, double diameter, double thickness, sdk::ColorRGBA color, sdk::ColorRGBA fill_color);
  /// A label at `position`; `font_size` must be finite and > 0, `text` non-empty.
  [[nodiscard]] Status addText(sdk::Point2 position, std::string text, double font_size, sdk::ColorRGBA color);

  /// The annotations, empty ones included. Consumes the builder: they and their charge move into the
  /// result, so a binder must not use the builder afterwards.
  [[nodiscard]] Charged<sdk::ImageAnnotations> finish() &&;

 private:
  ImageAnnotationsBuilder(sdk::ImageAnnotations annotations, MemoryReservation charge)
      : annotations_(std::move(annotations)), charge_(std::move(charge)) {}

  sdk::ImageAnnotations annotations_;
  MemoryReservation charge_;
};

}  // namespace PJ::scripting::native
