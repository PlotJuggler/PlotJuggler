// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pj_base/builtin/scene_entities.hpp"
#include "pj_base/expected.hpp"
#include "pj_scripting/memory_budget.h"

// Native SceneEntities builder behind a script's scene builder (D27). Luau-free, so the Luau and Python
// binders share it, like point_cloud_ops.h.
namespace PJ::scripting::native {

/// Accumulates ONE entity; `finish()` packages it as a complete snapshot. Every primitive is charged to
/// the evaluation's budget before it is stored (its own size plus its text or points; vector slack is not
/// charged), so a builder grown in a loop stops at the limit. Coordinates must be finite, and every size
/// (`diameter`, `size`, `font_size`, `thickness`) finite and greater than 0. Every rejection, the budget's
/// aside, names the offending field. Timestamps stay 0: the runtime stamps the snapshot with the result's stamp, the
/// newest resolved input time (docs/derived_objects/ARCHITECTURE.md §7).
class SceneEntityBuilder {
 public:
  /// `id` is the snapshot's de-duplication key and must not be empty; `frame_id` may be. `budget` null:
  /// unbounded.
  [[nodiscard]] static Expected<SceneEntityBuilder> create(
      std::string frame_id, std::string id, std::shared_ptr<MemoryBudget> budget);

  /// A sphere of `diameter` (> 0) at `center`, identity orientation.
  [[nodiscard]] Status addSphere(sdk::Vector3 center, double diameter, sdk::ColorRGBA color);
  /// A cuboid centred at `center` with extents `size` (each > 0).
  [[nodiscard]] Status addCube(sdk::Vector3 center, sdk::Vector3 size, sdk::ColorRGBA color);
  /// A label at `position`; `text` must not be empty.
  [[nodiscard]] Status addText(
      sdk::Vector3 position, std::string text, double font_size, sdk::ColorRGBA color, bool billboard);
  /// An open polyline through at least 2 `points`, `thickness` > 0.
  [[nodiscard]] Status addLineStrip(std::vector<sdk::Point3> points, double thickness, sdk::ColorRGBA color);

  /// The snapshot idiom: one `kAll` deletion, then the entity only if it has a primitive, so an empty
  /// builder clears the topic instead of publishing a bare entity. Consumes the builder: the entity and its
  /// charge move into the result, so a binder must not use the builder afterwards.
  [[nodiscard]] Charged<sdk::SceneEntities> finish() &&;

 private:
  SceneEntityBuilder(sdk::SceneEntity entity, MemoryReservation charge)
      : entity_(std::move(entity)), charge_(std::move(charge)) {}

  sdk::SceneEntity entity_;
  MemoryReservation charge_;
};

}  // namespace PJ::scripting::native
