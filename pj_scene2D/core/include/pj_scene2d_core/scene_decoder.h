#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <any>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_scene2d_core/image_annotation_codec.h"

namespace PJ {

// One overlay message to a SceneFrame, statelessly. decode(bytes) serves topics whose store holds canonical bytes;
// decode(object) serves parser-backed topics, whose parser already produced the canonical object.
class ISceneDecoder {
 public:
  virtual ~ISceneDecoder() = default;
  /// Retains neither the input nor any state across messages.
  virtual Expected<SceneFrame> decode(const uint8_t* data, size_t size) = 0;
  virtual Expected<SceneFrame> decode(const sdk::BuiltinObject& object) = 0;
};

namespace detail {

[[nodiscard]] inline SceneFrame imageAnnotationsToFrame(ImageAnnotation annotations) {
  SceneFrame frame;
  frame.timestamp = annotations.timestamp_ns;
  frame.annotations.push_back(std::move(annotations));
  return frame;
}

class ImageAnnotationsSceneDecoder final : public ISceneDecoder {
 public:
  Expected<SceneFrame> decode(const uint8_t* data, size_t size) override {
    auto annotations = deserializeImageAnnotation(data, size);
    if (!annotations.has_value()) {
      return unexpected(std::move(annotations).error());
    }
    return imageAnnotationsToFrame(std::move(*annotations));
  }

  Expected<SceneFrame> decode(const sdk::BuiltinObject& object) override {
    const auto* annotations = object.get<sdk::ImageAnnotations>();
    if (annotations == nullptr) {
      return unexpected(std::string("ImageAnnotationsSceneDecoder: object is not ImageAnnotations"));
    }
    return imageAnnotationsToFrame(*annotations);
  }
};

}  // namespace detail

/// Projects one sdk::SceneEntities message into one ImageAnnotations batch, using only XY positions: frame_locked,
/// deletions, lifetime_ns, Z and orientation are ignored; cylinders and models are skipped.
class SceneEntities2DDecoder final : public ISceneDecoder {
 public:
  Expected<SceneFrame> decode(const uint8_t* data, size_t size) override;
  Expected<SceneFrame> decode(const sdk::BuiltinObject& object) override;
};

/// nullptr for an unsupported schema.
[[nodiscard]] inline std::unique_ptr<ISceneDecoder> makeSceneDecoder(std::string_view schema_name) {
  if (schema_name == kSchemaImageAnnotations) {
    return std::make_unique<detail::ImageAnnotationsSceneDecoder>();
  }
  if (schema_name == kSchemaSceneEntities) {
    return std::make_unique<SceneEntities2DDecoder>();
  }
  return nullptr;
}

}  // namespace PJ
