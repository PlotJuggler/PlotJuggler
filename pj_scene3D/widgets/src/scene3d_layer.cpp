// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/scene3d_layer.h"

#include <utility>

#include "pj_scene3d_core/tf/object_placement.h"
#include "pj_tf/tf_buffer.h"

namespace pj::scene3d {

uint64_t foldFrameTransformIntoKey(
    uint64_t key, const TransformBuffer* tf_buffer, const std::string& fixed_frame, const std::string& source_frame,
    std::optional<PJ::Timepoint> object_stamp, PJ::Timepoint time, PlacementHold* hold) {
  if (tf_buffer == nullptr || source_frame.empty()) {
    return key;
  }
  if (const auto tf = placeObject(*tf_buffer, fixed_frame, source_frame, object_stamp, time, hold); tf) {
    key = foldTransformIntoKey(key, tf->matrix());
  }
  return key;
}

// Out-of-line ctor/dtor anchor the vtable in a single translation unit
// (key-function rule).
Scene3DLayer::Scene3DLayer(QObject* parent)
    : PJ::ISceneLayer(parent), placement_hold_(std::make_unique<PlacementHold>()) {}

Scene3DLayer::~Scene3DLayer() = default;

const std::string& Scene3DLayer::renderFrameId() const {
  static const std::string kNone;
  return kNone;
}

void Scene3DLayer::noteRenderFailure(QString warning) {
  if (renderWarning() == warning) {
    return;
  }
  setWarningPart(PJ::SceneWarningParts::Part::kRender, std::move(warning));
  renderWarningChanged();
}

void Scene3DLayer::noteDrawnObject(bool is_current, std::optional<PJ::Timepoint> cursor) {
  if (cursor.has_value()) {
    resolved_cursor_ = cursor;
  }
  if (is_current) {
    drawn_current_at_ = resolved_cursor_;
    placement_hold_->cursor.reset();
  } else {
    placement_hold_->cursor = drawn_current_at_;
  }
}

void Scene3DLayer::noteRenderSuccess() {
  noteRenderFailure({});
}

const QString& Scene3DLayer::dataWarning() const {
  return warningParts().text(PJ::SceneWarningParts::Part::kData);
}

const QString& Scene3DLayer::renderWarning() const {
  return warningParts().text(PJ::SceneWarningParts::Part::kRender);
}

}  // namespace pj::scene3d
