// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/scene_entities_decode.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace pj::scene3d {

glm::mat4 poseToMat4(const PJ::sdk::Pose& pose) {
  const glm::mat4 t = glm::translate(
      glm::mat4(1.0F), glm::vec3(
                           static_cast<float>(pose.position.x), static_cast<float>(pose.position.y),
                           static_cast<float>(pose.position.z)));
  const glm::quat q(
      static_cast<float>(pose.orientation.w), static_cast<float>(pose.orientation.x),
      static_cast<float>(pose.orientation.y), static_cast<float>(pose.orientation.z));
  return t * glm::mat4_cast(glm::normalize(q));
}

glm::vec3 toVec3(const PJ::sdk::Point3& point) {
  return {static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z)};
}

glm::vec3 toVec3(const PJ::sdk::Vector3& vec) {
  return {static_cast<float>(vec.x), static_cast<float>(vec.y), static_cast<float>(vec.z)};
}

glm::vec4 toVec4(const PJ::sdk::ColorRGBA& color) {
  return {color.r / 255.0F, color.g / 255.0F, color.b / 255.0F, color.a / 255.0F};
}

}  // namespace pj::scene3d
