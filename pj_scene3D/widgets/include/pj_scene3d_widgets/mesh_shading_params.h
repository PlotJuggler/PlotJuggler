// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <glm/vec3.hpp>

#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {

// Per-view mesh shading / scene-look knobs, owned by each SceneViewWidget next to CompositeParams
// (SceneViewWidget::meshShadingParams()) and handed to its passes every frame, so two 3D docks hold independent look
// state. Read per draw; mutate from the GUI thread and call view->invalidateContent() afterwards.
struct MeshShadingParams {
  float roughness = look::kRoughness;              // visual-mesh GGX roughness (collision stays 0.85)
  float reflectivity = look::kReflectivity;        // dielectric f0
  float ambient_scale = look::kAmbientScale;       // image-based ambient (diffuse + specular IBL) weight
  float direct_scale = look::kKeyLightScale;       // fixed world key ("sun") light weight
  float fill_light_scale = look::kFillLightScale;  // camera-locked headlight fill weight
  float env_intensity = look::kEnvIntensity;       // analytic specular IBL (environment reflection) weight
  // World-space direction TO the key light (Z-up); the shader normalizes it. A fixed sun keeps shape shading
  // consistent as the camera orbits. No app UI: a baked default derived from the canonical azimuth/elevation.
  glm::vec3 key_light_dir = look::keyDirFromAzEl(look::kKeyLightAzimuthDeg, look::kKeyLightElevationDeg);
  float mesh_opacity = look::kMeshOpacity;            // visual meshes; 0 hides them
  float collision_opacity = look::kCollisionOpacity;  // collision hulls (translucent overlay); 0 hides
  bool meshes_visible = true;                         // visibility toggles (independent of opacity)
  bool collisions_visible = true;
  // Mesh-shadow flag. ON by default: the app renders shadows unconditionally (no
  // user control), so a default-constructed view/dock shows them from the first
  // frame. When on, the key-light term of mesh + solid-grid-floor receivers is
  // modulated by the shadow map; the camera-fill and IBL ambient stay unshadowed.
  bool shadows_enabled = true;
};

}  // namespace pj::scene3d
