#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>

#include "pj_scene2d_widgets/layers/scene2d_layer.h"

namespace PJ {

class MediaSource;

// A video topic: one encoded frame (H.264, HEVC or AV1) per ObjectStore entry, unwrapped by the topic's parser,
// which it requires, and decoded by a StreamingVideoSource.
class VideoLayer final : public Scene2DLayer {
  Q_OBJECT
 public:
  VideoLayer(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name,
      QObject* parent = nullptr);

 protected:
  [[nodiscard]] std::unique_ptr<MediaSource> createMediaSource(const SceneLayerContext& ctx) override;
};

}  // namespace PJ
