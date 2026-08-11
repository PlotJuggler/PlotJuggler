// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/scene_decoder_layer.h"

#include <memory>
#include <string>
#include <utility>

#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_core/parser_object.h"
#include "pj_scene2d_core/scene_decoder.h"
#include "pj_scene2d_core/scene_pipeline_source.h"

namespace PJ {

SceneDecoderLayer::SceneDecoderLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name, const QString& kind_label,
    std::string_view schema, QObject* parent)
    : Scene2DLayer(topic_id, object_type, display_name, kind_label, parent), schema_(schema) {}

std::unique_ptr<MediaSource> SceneDecoderLayer::createMediaSource(const SceneLayerContext& /*ctx*/) {
  auto* store = objectStore();
  if (store == nullptr) {
    return nullptr;
  }
  auto decoder = makeSceneDecoder(schema_);
  if (decoder == nullptr) {
    return nullptr;
  }
  // Topics produced by a message parser (e.g. yolo_msgs/DetectionArray, markers)
  // store the RAW source message under pure-lazy ingest. Keep SessionManager's
  // replaceable parser lease in this host-aware layer: pj_scene2d_core receives
  // only a complete entry decoder and stays independent of runtime binding types.
  // Topics whose loader writes canonical bytes directly have no parser and
  // decode them as-is.
  if (auto* session = sessionManager(); session != nullptr) {
    if (session->parserBindingForObjectTopic(topicId())) {
      const ObjectTopicId topic = topicId();
      const sdk::BuiltinObjectType expected_type = objectType();
      return std::make_unique<ScenePipelineSource>(
          store, topic,
          [session, topic, expected_type](
              ISceneDecoder& scene_decoder, Timestamp timestamp,
              const sdk::PayloadView& payload) -> Expected<SceneFrame> {
            // Resolve per parse, not at attach: reload replaces this stable
            // topic's parser slot. `binding` is declared before `record`, so it
            // also outlives the parsed std::any and keeps the plugin DSO mapped
            // through decode and ObjectRecord destruction.
            const auto binding = session->parserBindingForObjectTopic(topic);
            if (!binding) {
              return unexpected(std::string("no parser registered for scene topic"));
            }
            auto record = parseObjectRecordAs(*binding.parser, binding.mutex, timestamp, payload, expected_type);
            if (!record.has_value()) {
              return unexpected(std::move(record.error().message));
            }
            return scene_decoder.decode(record->object);
          },
          std::move(decoder));
    }
  }
  return std::make_unique<ScenePipelineSource>(store, topicId(), std::move(decoder));
}

}  // namespace PJ
