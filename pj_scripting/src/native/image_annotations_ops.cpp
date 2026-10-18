// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "image_annotations_ops.h"

#include <algorithm>
#include <array>
#include <utility>

#include "field_checks.h"

namespace PJ::scripting::native {
namespace {

struct Topology {
  std::string_view name;
  sdk::AnnotationTopology value;
  std::size_t min_points;
};

constexpr std::array<Topology, 4> kTopologies{{
    {"points", sdk::AnnotationTopology::kPoints, 1},
    {"line_list", sdk::AnnotationTopology::kLineList, 2},
    {"line_strip", sdk::AnnotationTopology::kLineStrip, 2},
    {"line_loop", sdk::AnnotationTopology::kLineLoop, 3},
}};

std::string topologyNames() {
  std::string names;
  for (const Topology& topology : kTopologies) {
    names += (names.empty() ? "" : ", ") + std::string(topology.name);
  }
  return names;
}

}  // namespace

Expected<ImageAnnotationsBuilder> ImageAnnotationsBuilder::create(
    std::string image_topic, std::shared_ptr<MemoryBudget> budget) {
  if (Status valid = checkNonEmpty("annotations: 'image_topic'", image_topic); !valid) {
    return unexpected(valid.error());
  }
  MemoryReservation charge(std::move(budget));
  if (Status reserved = charge.grow(sizeof(sdk::ImageAnnotations) + image_topic.size()); !reserved) {
    return unexpected(reserved.error());
  }
  sdk::ImageAnnotations annotations;
  annotations.image_topic = std::move(image_topic);
  return ImageAnnotationsBuilder(std::move(annotations), std::move(charge));
}

Status ImageAnnotationsBuilder::addPoints(
    std::vector<sdk::Point2> points, std::string_view type, double thickness, sdk::ColorRGBA color,
    sdk::ColorRGBA fill_color) {
  const auto topology = std::ranges::find(kTopologies, type, &Topology::name);
  if (topology == kTopologies.end()) {
    return unexpected("points: 'type' must be one of " + topologyNames());
  }
  if (points.size() < topology->min_points) {
    return unexpected(
        "points: 'points' needs at least " + std::to_string(topology->min_points) + " points for " + std::string(type));
  }
  if (topology->value == sdk::AnnotationTopology::kLineList && points.size() % 2 != 0) {
    return unexpected("points: 'points' needs an even number of points for line_list");
  }
  if (Status valid = firstFailure(
          {checkFinitePoints("points: 'points'", points), checkPositive("points: 'thickness'", thickness)});
      !valid) {
    return valid;
  }
  if (Status reserved = charge_.grow(sizeof(sdk::PointsAnnotation) + points.size() * sizeof(sdk::Point2)); !reserved) {
    return reserved;
  }
  sdk::PointsAnnotation annotation;
  annotation.topology = topology->value;
  annotation.points = std::move(points);
  annotation.thickness = thickness;
  annotation.color = color;
  annotation.fill_color = fill_color;
  annotations_.points.push_back(std::move(annotation));
  return okStatus();
}

Status ImageAnnotationsBuilder::addCircle(
    sdk::Point2 center, double diameter, double thickness, sdk::ColorRGBA color, sdk::ColorRGBA fill_color) {
  if (Status valid = firstFailure(
          {checkFinite("circle: 'center'", {center.x, center.y}), checkPositive("circle: 'diameter'", diameter),
           checkPositive("circle: 'thickness'", thickness)});
      !valid) {
    return valid;
  }
  if (Status reserved = charge_.grow(sizeof(sdk::CircleAnnotation)); !reserved) {
    return reserved;
  }
  sdk::CircleAnnotation circle;
  circle.center = center;
  circle.radius = diameter / 2.0;
  circle.thickness = thickness;
  circle.color = color;
  circle.fill_color = fill_color;
  annotations_.circles.push_back(circle);
  return okStatus();
}

Status ImageAnnotationsBuilder::addText(
    sdk::Point2 position, std::string text, double font_size, sdk::ColorRGBA color) {
  if (Status valid = firstFailure(
          {checkFinite("text: 'position'", {position.x, position.y}), checkPositive("text: 'font_size'", font_size),
           checkNonEmpty("text: 'text'", text)});
      !valid) {
    return valid;
  }
  if (Status reserved = charge_.grow(sizeof(sdk::TextAnnotation) + text.size()); !reserved) {
    return reserved;
  }
  sdk::TextAnnotation label;
  label.position = position;
  label.font_size = font_size;
  label.color = color;
  label.text = std::move(text);
  annotations_.texts.push_back(std::move(label));
  return okStatus();
}

Charged<sdk::ImageAnnotations> ImageAnnotationsBuilder::finish() && {
  return {std::move(annotations_), std::move(charge_)};
}

}  // namespace PJ::scripting::native
