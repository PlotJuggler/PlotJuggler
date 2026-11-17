#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <vector>

#include "pj_base/builtin/image_annotations.hpp"
#include "pj_base/types.hpp"

namespace PJ {

// Module-local names for the SDK annotation types (this module spells ImageAnnotation in the singular).
using AnnotationTopology = sdk::AnnotationTopology;
using CircleAnnotation = sdk::CircleAnnotation;
using ColorRGBA = sdk::ColorRGBA;
using ImageAnnotation = sdk::ImageAnnotations;
using Point2 = sdk::Point2;
using PointsAnnotation = sdk::PointsAnnotation;
using TextAnnotation = sdk::TextAnnotation;

// The annotations an ISceneDecoder produced for one entry.
struct SceneFrame {
  Timestamp timestamp = 0;
  std::vector<ImageAnnotation> annotations;
  bool operator==(const SceneFrame&) const = default;

  [[nodiscard]] bool empty() const noexcept {
    return annotations.empty();
  }
};

}  // namespace PJ
