// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// A model layer the tests drive directly, as a RobotModelLayer or
// SceneEntitiesLayer feeds the view. GPU-free: shared by the model pass's policy
// tests and its display cache tests.

#include <QColor>
#include <QStringList>
#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_scene3d_widgets/model_renderable.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

namespace pj::scene3d::test {

class FakeModelLayer final : public Scene3DLayer, public ModelRenderable {
 public:
  /// A layer placed in `frame_id`.
  explicit FakeModelLayer(std::string frame_id = "map") : frame(std::move(frame_id)) {}

  /// Publishes (or replaces) `key` and bumps the revision; a new key's draw goes
  /// at `at` in `frame`.
  void put(const std::string& key, MeshData mesh, glm::dvec3 at = glm::dvec3(0.0)) {
    if (!meshes.contains(key)) {
      draws.push_back({.mesh_key = key, .frame_id = frame, .model = glm::translate(glm::dmat4(1.0), at)});
    }
    meshes[key] = std::make_shared<const MeshData>(std::move(mesh));
    ++revision;
  }
  void erase(const std::string& key) {
    meshes.erase(key);
    std::erase_if(draws, [&](const ModelDrawCall& draw) { return draw.mesh_key == key; });
    ++revision;
  }

  const std::vector<ModelDrawCall>& modelDrawCalls() const override {
    return draws;
  }
  const std::unordered_map<std::string, std::shared_ptr<const MeshData>>& modelMeshes() const override {
    return meshes;
  }
  std::uint64_t modelRevision() const override {
    return revision;
  }
  bool visible() const override {
    return true;
  }
  float opacity() const override {
    return 1.0F;
  }
  bool colorOverrideEnabled() const override {
    return false;
  }
  QColor overrideColor() const override {
    return {};
  }
  bool wireframe() const override {
    return wireframe_on;
  }
  const std::string& renderFrameId() const override {
    return frame;
  }
  bool contributesToSceneBounds() const override {
    return true;
  }
  Scene3DLayer& modelLayer() override {
    return *this;
  }
  pj::scene3d::SceneContributions sceneContributions() override {
    return {.models = this};
  }
  QStringList fallbackFrames() const override {
    return {};
  }
  QString sourceFrame() const override {
    return {};
  }
  PJ::SceneLayerInfo info() const override {
    return {};
  }
  PJ::Range<PJ::Timepoint> timeRange() const override {
    return PJ::liveTopicTimeRange(nullptr, PJ::ObjectTopicId{1});
  }
  bool attach(const PJ::SceneLayerContext& /*ctx*/) override {
    return true;
  }
  void detach() override {}
  void setTrackerTime(PJ::Timepoint /*time*/) override {}
  void setVisible(bool /*visible*/) override {}
  QWidget* createConfigWidget(QWidget* /*parent*/) override {
    return nullptr;
  }

  std::vector<ModelDrawCall> draws;
  std::unordered_map<std::string, std::shared_ptr<const MeshData>> meshes;
  std::uint64_t revision = 1;
  bool wireframe_on = false;
  std::string frame;
};

}  // namespace pj::scene3d::test
