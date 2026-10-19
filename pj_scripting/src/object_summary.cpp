// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scripting/object_summary.h"

#include <string>
#include <unordered_set>
#include <vector>

#include "native/point_cloud_ops.h"
#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_base/builtin/image_annotations.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/scene_entities.hpp"

namespace PJ::scripting {
namespace {

/// The unique `field(item)` of `items`, in order of first appearance.
template <typename Container, typename Field>
[[nodiscard]] std::vector<std::string> uniqueInOrder(const Container& items, Field&& field) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  for (const auto& item : items) {
    const std::string& value = field(item);
    if (seen.insert(value).second) {
      out.push_back(value);
    }
  }
  return out;
}

}  // namespace

nlohmann::json summarizeObject(const sdk::BuiltinObject& object) {
  nlohmann::json doc;
  doc["type"] = std::string(sdk::name(sdk::typeOf(object)));

  if (const sdk::PointCloud* cloud = object.get<sdk::PointCloud>()) {
    doc["frame_id"] = cloud->frame_id;
    doc["points"] = native::countPoints(*cloud);
    if (const Expected<native::Bounds> bounds = native::bounds(*cloud); bounds.has_value() && bounds->count > 0) {
      doc["bounds"] = {
          {"min", {bounds->min.x, bounds->min.y, bounds->min.z}},
          {"max", {bounds->max.x, bounds->max.y, bounds->max.z}},
      };
    }
    return doc;
  }
  if (const sdk::SceneEntities* scene = object.get<sdk::SceneEntities>()) {
    doc["entities"] = scene->entities.size();
    doc["deletions"] = scene->deletions.size();
    doc["frame_ids"] = uniqueInOrder(
        scene->entities, [](const sdk::SceneEntity& entity) -> const std::string& { return entity.frame_id; });
    return doc;
  }
  if (const sdk::ImageAnnotations* annotations = object.get<sdk::ImageAnnotations>()) {
    doc["circles"] = annotations->circles.size();
    doc["points"] = annotations->points.size();
    doc["texts"] = annotations->texts.size();
    return doc;
  }
  if (const sdk::FrameTransforms* transforms = object.get<sdk::FrameTransforms>()) {
    doc["transforms"] = transforms->transforms.size();
    doc["parents"] = uniqueInOrder(
        transforms->transforms,
        [](const sdk::FrameTransform& transform) -> const std::string& { return transform.parent_frame_id; });
    return doc;
  }
  return doc;
}

bool isEmptyObject(const sdk::BuiltinObject& object) {
  if (const sdk::PointCloud* cloud = object.get<sdk::PointCloud>()) {
    return native::countPoints(*cloud) == 0;
  }
  if (const sdk::SceneEntities* scene = object.get<sdk::SceneEntities>()) {
    return scene->entities.empty();
  }
  if (const sdk::ImageAnnotations* annotations = object.get<sdk::ImageAnnotations>()) {
    return annotations->circles.empty() && annotations->points.empty() && annotations->texts.empty();
  }
  if (const sdk::FrameTransforms* transforms = object.get<sdk::FrameTransforms>()) {
    return transforms->transforms.empty();
  }
  return false;
}

}  // namespace PJ::scripting
