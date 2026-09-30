// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QString>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_scene3d_widgets/mesh_data.h"

namespace pj::scene3d {

class Scene3DLayer;

// View-level visibility/opacity applies only to robot visual and collision
// groups. Independent model draws (SceneEntities ModelPrimitive) retain their
// layer's own presentation controls.
enum class ModelDrawGroup : std::uint8_t { kIndependent, kVisual, kCollision };

struct ModelDrawCall {
  std::string mesh_key;
  std::string frame_id;
  glm::dmat4 model{1.0};
  glm::vec4 override_color{1.0F};
  bool use_material = true;
  ModelDrawGroup group = ModelDrawGroup::kIndependent;
};

// Non-owning QRhi input seam shared by layers that submit imported or
// procedural MeshData; the view owns the GPU resources. Bump modelRevision()
// whenever modelMeshes() or modelDrawCalls() change. TF motion of the draw
// frames needs no bump: the view resolves every placement each frame. A
// published MeshData is immutable: change a mesh by storing a new shared_ptr
// under its key. A bump rebuilds the GPU state only of keys whose pointer
// changed, so keeping the pointer of an unchanged mesh avoids its re-upload.
class ModelRenderable {
 public:
  virtual ~ModelRenderable() = default;

  [[nodiscard]] virtual const std::vector<ModelDrawCall>& modelDrawCalls() const = 0;
  [[nodiscard]] virtual const std::unordered_map<std::string, std::shared_ptr<const MeshData>>& modelMeshes() const = 0;
  [[nodiscard]] virtual std::uint64_t modelRevision() const = 0;
  [[nodiscard]] virtual bool visible() const = 0;
  [[nodiscard]] virtual float opacity() const = 0;
  [[nodiscard]] virtual bool colorOverrideEnabled() const = 0;
  [[nodiscard]] virtual QColor overrideColor() const = 0;
  [[nodiscard]] virtual bool wireframe() const = 0;
  // Root frame of the model draws; see PointRenderable::renderFrameId().
  [[nodiscard]] virtual const std::string& renderFrameId() const = 0;
  [[nodiscard]] virtual bool contributesToSceneBounds() const = 0;
  // The layer this is (every implementer is a Scene3DLayer), which carries the
  // renderer part of its row warning.
  [[nodiscard]] virtual Scene3DLayer& modelLayer() = 0;
};

}  // namespace pj::scene3d
