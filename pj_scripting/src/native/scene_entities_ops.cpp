// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "scene_entities_ops.h"

#include <utility>

#include "field_checks.h"

namespace PJ::scripting::native {

Expected<SceneEntityBuilder> SceneEntityBuilder::create(
    std::string frame_id, std::string id, std::shared_ptr<MemoryBudget> budget) {
  if (Status valid = checkNonEmpty("scene entity 'id'", id); !valid) {
    return unexpected(valid.error());
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
  if (Status valid = firstFailure(
          {checkFinite("sphere: 'center'", {center.x, center.y, center.z}),
           checkPositive("sphere: 'diameter'", diameter)});
      !valid) {
    return valid;
  }
  if (Status reserved = charge_.grow(sizeof(sdk::SpherePrimitive)); !reserved) {
    return reserved;
  }
  sdk::SpherePrimitive sphere;
  sphere.pose.position = center;
  sphere.size = sdk::Vector3{diameter, diameter, diameter};
  sphere.color = color;
  entity_.spheres.push_back(sphere);
  return okStatus();
}

Status SceneEntityBuilder::addCube(sdk::Vector3 center, sdk::Vector3 size, sdk::ColorRGBA color) {
  if (Status valid = firstFailure(
          {checkFinite("cube: 'center'", {center.x, center.y, center.z}), checkPositive("cube: 'size'", size.x),
           checkPositive("cube: 'size'", size.y), checkPositive("cube: 'size'", size.z)});
      !valid) {
    return valid;
  }
  if (Status reserved = charge_.grow(sizeof(sdk::CubePrimitive)); !reserved) {
    return reserved;
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
  if (Status valid = firstFailure(
          {checkFinite("text: 'position'", {position.x, position.y, position.z}),
           checkPositive("text: 'font_size'", font_size), checkNonEmpty("text: 'text'", text)});
      !valid) {
    return valid;
  }
  if (Status reserved = charge_.grow(sizeof(sdk::TextPrimitive) + text.size()); !reserved) {
    return reserved;
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
  if (points.size() < 2) {
    return unexpected("line_strip: 'points' needs at least 2 points");
  }
  if (Status valid = firstFailure(
          {checkFinitePoints("line_strip: 'points'", points), checkPositive("line_strip: 'thickness'", thickness)});
      !valid) {
    return valid;
  }
  if (Status reserved = charge_.grow(sizeof(sdk::LinePrimitive) + points.size() * sizeof(sdk::Point3)); !reserved) {
    return reserved;
  }
  sdk::LinePrimitive line;
  line.type = sdk::LineType::kLineStrip;
  line.thickness = thickness;
  line.color = color;
  line.points = std::move(points);
  entity_.lines.push_back(std::move(line));
  return okStatus();
}

Charged<sdk::SceneEntities> SceneEntityBuilder::finish() && {
  Charged<sdk::SceneEntities> out{{}, std::move(charge_)};
  out.value.deletions.push_back(sdk::SceneEntityDeletion{.type = sdk::SceneEntityDeletion::Type::kAll});
  if (!entity_.spheres.empty() || !entity_.cubes.empty() || !entity_.texts.empty() || !entity_.lines.empty()) {
    out.value.entities.push_back(std::move(entity_));
  }
  return out;
}

}  // namespace PJ::scripting::native
