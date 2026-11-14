// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "scene_object_classification.h"

#include "pj_runtime/CatalogModel.h"

#ifdef PJ_WITH_SCENE2D
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#endif
#ifdef PJ_WITH_SCENE3D
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#endif
using namespace Qt::StringLiterals;

namespace PJ {

bool is3dSceneObjectType(sdk::BuiltinObjectType type) {
#ifdef PJ_WITH_SCENE3D
  return Scene3DDockWidget::handlesObjectType(type);
#else
  (void)type;
  return false;
#endif
}

bool is2dSceneObjectType(sdk::BuiltinObjectType type) {
#ifdef PJ_WITH_SCENE2D
  return Scene2DDockWidget::handlesObjectType(type);
#else
  (void)type;
  return false;
#endif
}

bool isImageFamilyObjectType(sdk::BuiltinObjectType type) {
  return type == sdk::BuiltinObjectType::kImage || type == sdk::BuiltinObjectType::kVideoFrame ||
         type == sdk::BuiltinObjectType::kDepthImage || type == sdk::BuiltinObjectType::kImageAnnotations;
}

QString sceneKindForObjectType(sdk::BuiltinObjectType type) {
  if (isImageFamilyObjectType(type)) {
    return is2dSceneObjectType(type) ? u"scene2d"_s : QString{};
  }
  if (is3dSceneObjectType(type)) {
    return u"scene3d"_s;
  }
  if (is2dSceneObjectType(type)) {
    return u"scene2d"_s;
  }
  return {};
}

bool isDroppableObjectType(sdk::BuiltinObjectType type) {
  return !sceneKindForObjectType(type).isEmpty();
}

bool isCustomWidgetObjectType(sdk::BuiltinObjectType type) {
  return type != sdk::BuiltinObjectType::kNone && !isPanelOnlyObjectType(type) && !isDroppableObjectType(type);
}

}  // namespace PJ
