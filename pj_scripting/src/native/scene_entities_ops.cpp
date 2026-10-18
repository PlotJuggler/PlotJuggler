// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "scene_entities_ops.h"

#include <cmath>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace PJ::scripting::native {
namespace {

/// The rejection of `field` (e.g. "sphere: 'center'") when any of `values` is NaN or infinite.
Status checkFinite(std::string_view field, std::initializer_list<double> values) {
  for (const double value : values) {
    if (!std::isfinite(value)) {
      return unexpected(std::string(field) + " must be finite");
    }
  }
  return okStatus();
}

/// The rejection of `field` (e.g. "sphere: 'diameter'") when any of `values` is not a finite number above 0.
Status checkPositive(std::string_view field, std::initializer_list<double> values) {
  for (const double value : values) {
    if (!std::isfinite(value) || value <= 0.0) {
      return unexpected(std::string(field) + " must be a finite number greater than 0");
    }
  }
  return okStatus();
}

}  // namespace

Expected<SceneEntityBuilder> SceneEntityBuilder::create(
    std::string frame_id, std::string id, std::shared_ptr<MemoryBudget> budget) {
  if (id.empty()) {
    return unexpected("scene entity 'id' must not be empty");
  }
  MemoryReservation charge(std::move(budget));
  if (Status reserved = charge.grow(sizeof(sdk::SceneEntity) + id.size() + frame_id.size()); !reserved) {
    return unexpected(reserved.error());
  }
  sdk::SceneEntity entity;
  entity.frame_id = std::move(frame_id);
  entity.id = std::move(id);
  return SceneEntityBuilder(std::move(entity), std::move(charge));
}

Status SceneEntityBuilder::addSphere(sdk::Vector3 center, double diameter, sdk::ColorRGBA color) {
  Status valid = checkFinite("sphere: 'center'", {center.x, center.y, center.z});
  if (valid) {
    valid = checkPositive("sphere: 'diameter'", {diameter});
  }
  if (valid) {
    valid = charge_.grow(sizeof(sdk::SpherePrimitive));
  }
  if (!valid) {
    return valid;
  }
  sdk::SpherePrimitive sphere;
  sphere.pose.position = center;
  sphere.size = sdk::Vector3{diameter, diameter, diameter};
  sphere.color = color;
  entity_.spheres.push_back(sphere);
  return okStatus();
}

Status SceneEntityBuilder::addCube(sdk::Vector3 center, sdk::Vector3 size, sdk::ColorRGBA color) {
  Status valid = checkFinite("cube: 'center'", {center.x, center.y, center.z});
  if (valid) {
    valid = checkPositive("cube: 'size'", {size.x, size.y, size.z});
  }
  if (valid) {
    valid = charge_.grow(sizeof(sdk::CubePrimitive));
  }
  if (!valid) {
    return valid;
  }
  sdk::CubePrimitive cube;
  cube.pose.position = center;
  cube.size = size;
  cube.color = color;
  entity_.cubes.push_back(cube);
  return okStatus();
}

Status SceneEntityBuilder::addText(
    sdk::Vector3 position, std::string text, double font_size, sdk::ColorRGBA color, bool billboard) {
  Status valid = checkFinite("text: 'position'", {position.x, position.y, position.z});
  if (valid) {
    valid = checkPositive("text: 'font_size'", {font_size});
  }
  if (valid && text.empty()) {
    valid = unexpected("text: 'text' must not be empty");
  }
  if (valid) {
    valid = charge_.grow(sizeof(sdk::TextPrimitive) + text.size());
  }
  if (!valid) {
    return valid;
  }
  sdk::TextPrimitive label;
  label.pose.position = position;
  label.billboard = billboard;
  label.font_size = font_size;
  label.color = color;
  label.text = std::move(text);
  entity_.texts.push_back(std::move(label));
  return okStatus();
}

Status SceneEntityBuilder::addLineStrip(std::vector<sdk::Point3> points, double thickness, sdk::ColorRGBA color) {
  Status valid = okStatus();
  if (points.size() < 2) {
    valid = unexpected("line_strip: 'points' needs at least 2 points");
  }
  for (const sdk::Point3& point : points) {
    if (valid) {
      valid = checkFinite("line_strip: 'points'", {point.x, point.y, point.z});
    }
  }
  if (valid) {
    valid = checkPositive("line_strip: 'thickness'", {thickness});
  }
  if (valid) {
    valid = charge_.grow(sizeof(sdk::LinePrimitive) + points.size() * sizeof(sdk::Point3));
  }
  if (!valid) {
    return valid;
  }
  sdk::LinePrimitive line;
  line.type = sdk::LineType::kLineStrip;
  line.thickness = thickness;
  line.color = color;
  line.points = std::move(points);
  entity_.lines.push_back(std::move(line));
  return okStatus();
}

ChargedSceneEntities SceneEntityBuilder::finish() && {
  ChargedSceneEntities out{{}, std::move(charge_)};
  out.scene.deletions.push_back(sdk::SceneEntityDeletion{.type = sdk::SceneEntityDeletion::Type::kAll});
  if (!entity_.spheres.empty() || !entity_.cubes.empty() || !entity_.texts.empty() || !entity_.lines.empty()) {
    out.scene.entities.push_back(std::move(entity_));
  }
  return out;
}

}  // namespace PJ::scripting::native
