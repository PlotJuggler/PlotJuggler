// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <glm/vec3.hpp>

#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {

// Per-view mesh shading / scene-look knobs. Owned by each SceneViewWidget next to
// CompositeParams (SceneViewWidget::meshShadingParams()) and handed to its passes
// every frame, so the bound view's mesh/collision opacity and shading reach the
// model pass through render-time plumbing rather than a process-global. Read per
// draw; mutate freely from the GUI thread (call view->invalidateContent() afterwards). Two 3D
// docks therefore hold independent look state and never clobber each other.
// Defaults are the User's 2026-06-10 look-dev pick.
struct MeshShadingParams {
  float roughness = look::kRoughness;              // visual-mesh GGX roughness (collision stays 0.85)
  float reflectivity = look::kReflectivity;        // dielectric f0
  float ambient_scale = look::kAmbientScale;       // image-based ambient (diffuse + specular IBL) weight
  float direct_scale = look::kKeyLightScale;       // fixed world key ("sun") light weight
  float fill_light_scale = look::kFillLightScale;  // camera-locked headlight fill weight
  float env_intensity = look::kEnvIntensity;       // analytic specular IBL (environment reflection) weight
  // World-space direction TO the key light (Z-up); the shader normalizes it. A
  // fixed sun keeps shape shading consistent as the camera orbits (vs the old
  // camera-locked headlight). No app UI — a baked look-dev default, derived from
  // the canonical azimuth/elevation defaults.
  glm::vec3 key_light_dir = look::keyDirFromAzEl(look::kKeyLightAzimuthDeg, look::kKeyLightElevationDeg);
  float mesh_opacity = look::kMeshOpacity;            // visual meshes; 0 hides them (plan §9.3)
  float collision_opacity = look::kCollisionOpacity;  // collision hulls (translucent overlay); 0 hides
  bool meshes_visible = true;                         // Part C eye toggles (independent of opacity)
  bool collisions_visible = true;
  // Mesh-shadow flag. ON by default: the app renders shadows unconditionally (no
  // user control), so a default-constructed view/dock shows them from the first
  // frame. When on, the key-light term of mesh + solid-grid-floor receivers is
  // modulated by the shadow map; the camera-fill and IBL ambient stay unshadowed.
  bool shadows_enabled = true;
};

}  // namespace pj::scene3d
