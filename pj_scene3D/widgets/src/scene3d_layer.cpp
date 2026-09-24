// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/scene3d_layer.h"

#include <utility>

#include "pj_scene3d_core/tf/tf_buffer.h"

namespace pj::scene3d {

uint64_t foldFrameTransformIntoKey(
    uint64_t key, const TransformBuffer* tf_buffer, const std::string& fixed_frame, const std::string& source_frame,
    PJ::Timepoint time) {
  if (tf_buffer == nullptr || source_frame.empty()) {
    return key;
  }
  if (const auto tf = tf_buffer->tryLookupTransform(fixed_frame, source_frame, time); tf) {
    key = foldTransformIntoKey(key, glm::mat4(tf->matrix()));
  }
  return key;
}

// Out-of-line ctor/dtor anchor the vtable in a single translation unit
// (key-function rule).
Scene3DLayer::Scene3DLayer(QObject* parent) : PJ::ISceneLayer(parent) {}

Scene3DLayer::~Scene3DLayer() = default;

void Scene3DLayer::noteRenderFailure(QString warning) {
  if (render_warning_ == warning) {
    return;
  }
  render_warning_ = std::move(warning);
  updateWarning();
  renderWarningChanged();
}

void Scene3DLayer::noteRenderSuccess() {
  noteRenderFailure({});
}

void Scene3DLayer::setDataWarning(QString warning) {
  if (data_warning_ == warning) {
    return;
  }
  data_warning_ = std::move(warning);
  updateWarning();
}

void Scene3DLayer::updateWarning() {
  QString combined = data_warning_;
  if (!render_warning_.isEmpty()) {
    combined += (combined.isEmpty() ? QString() : QStringLiteral("\n")) + render_warning_;
  }
  if (warning_ == combined) {
    return;
  }
  warning_ = std::move(combined);
  emit statusWarningChanged();
}

}  // namespace pj::scene3d
