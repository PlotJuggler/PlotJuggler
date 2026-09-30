#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomElement>
#include <QString>
#include <glm/vec3.hpp>
#include <optional>

namespace pj::scene3d {

// Range-checked readers for persisted scene-dock XML state (Scene3DDockWidget's
// layout grammar).
//
// All three readers follow the same contract: a MISSING attribute is valid
// (returns true, output untouched — absence means "keep the default"), a
// present-but-malformed or out-of-range value is a validation failure
// (returns false, output untouched).
bool readXmlBool(const QDomElement& element, const QString& key, std::optional<bool>& output);
bool readXmlInt(const QDomElement& element, const QString& key, int minimum, int maximum, std::optional<int>& output);
bool readXmlFloat(
    const QDomElement& element, const QString& key, float minimum, float maximum, std::optional<float>& output);

// The camera_state JSON payload after validation: every field is optional
// (absent keys keep the current camera value), but any present field is
// finite and inside its physical range.
struct ValidatedCameraState {
  std::optional<glm::vec3> focal;
  std::optional<float> radius;
  std::optional<float> azimuth;
  std::optional<float> elevation;
  std::optional<float> fov_y;
  std::optional<float> ortho_scale;
  std::optional<bool> perspective;
};

// Parses and range-checks the camera_state attribute (a JSON object). Returns
// nullopt on malformed JSON, a non-object payload, or any out-of-range field —
// callers treat that as "reject the whole element", never as partial state.
std::optional<ValidatedCameraState> validateCameraState(const QString& encoded);

// Accepted ranges for the persisted <scene_controls> float attributes. One
// definition for both platforms: a layout saved by either backend must restore
// on the other, so neither reader may enforce a narrower range than the widest
// writer (the browser view clamps its setters to exactly these bounds).
inline constexpr float kGridExtentMinM = 0.01f;
inline constexpr float kGridExtentMaxM = 1.0e7f;
inline constexpr float kGizmoSizeMinM = 0.001f;
inline constexpr float kGizmoSizeMaxM = 5.0f;

// The backend-neutral <scene_controls> attributes after validation: every field
// is optional (absence means "keep the current value"). Platform-specific
// controls (native mesh/collision opacity) are read separately by their owner.
struct ValidatedSceneControls {
  std::optional<bool> grid_visible;
  std::optional<int> grid_style;
  std::optional<float> grid_extent_m;
  std::optional<int> grid_divisions;
  std::optional<bool> axes_visible;
  std::optional<float> gizmo_size_m;
  std::optional<float> gizmo_opacity;
  std::optional<bool> tf_parent_lines;
};

// Reads and range-checks the backend-neutral <scene_controls> attributes into
// `output`. False on any malformed or out-of-range value — callers reject the
// element rather than keep partial state.
[[nodiscard]] bool readSceneControls(const QDomElement& element, ValidatedSceneControls& output);

}  // namespace pj::scene3d
