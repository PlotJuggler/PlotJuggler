#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>
#include <string>
#include <string_view>

#include "pj_scene2d_widgets/layers/scene2d_layer.h"

namespace PJ {

class MediaSource;

// Every vector-overlay layer (ImageAnnotations, SceneEntities, ...): the schema and the list label are injected.
class SceneDecoderLayer final : public Scene2DLayer {
  Q_OBJECT
 public:
  SceneDecoderLayer(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name,
      const QString& kind_label, std::string_view schema, QObject* parent = nullptr);

 protected:
  [[nodiscard]] std::unique_ptr<MediaSource> createMediaSource(const SceneLayerContext& ctx) override;

 private:
  std::string schema_;
};

}  // namespace PJ
