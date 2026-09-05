// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/image_layer.h"

#include <QByteArray>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QString>
#include <any>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/camera_info.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/image_pipeline_source.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_widgets/scene2d_pipelines.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToggleSwitch.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcScene2DImageLayer, "pj.scene2d.layer.image")

bool topicUsesCanonicalImageCodec(const std::string& metadata_json) {
  const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(metadata_json));
  if (!doc.isObject()) {
    if (!metadata_json.empty()) {
      qCWarning(lcScene2DImageLayer) << "topic metadata is not a valid JSON object; cannot detect image_codec";
    }
    return false;
  }
  return doc.object().value(u"image_codec"_s).toString() == "pj_image_v1"_L1;
}

// Build a frame_id -> CameraInfo map by running each "<ns>/camera_info" topic's
// MessageParser on its latest sample. The ObjectStore keeps raw message bytes
// (e.g. foxglove.CameraCalibration) and decodes lazily through the topic's
// parser — so calibration must be obtained the same way an image is, not by
// deserializing the raw bytes as a canonical CameraInfo. The decode worker then
// uses this map (matched by sdk::Image.frame_id) to rectify the camera frames.
std::unordered_map<std::string, sdk::CameraInfo> collectCameraInfoByFrameId(
    SessionManager* session, ObjectStore* store) {
  std::unordered_map<std::string, sdk::CameraInfo> by_frame;
  constexpr std::string_view kSuffix = "/camera_info";
  for (const ObjectTopicId id : store->listTopics()) {
    const std::string& name = store->descriptor(id).topic_name;
    if (name.size() < kSuffix.size() || name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
      continue;
    }
    if (store->entryCount(id) == 0) {
      continue;
    }
    auto* ci_parser = session->parserForObjectTopic(id);
    if (ci_parser == nullptr) {
      continue;
    }
    const auto entry = store->latestAt(id, store->timeRange(id).second);
    if (!entry.has_value() || entry->payload.bytes.empty()) {
      continue;
    }
    const sdk::PayloadView payload = entry->payload;
    // MessageParser plugins aren't thread-safe; serialize via the shared mutex,
    // exactly as ImagePipelineSource does for the image parser.
    const auto mutex = session->parserMutexForObjectTopic(id);
    auto record = [&] {
      if (mutex) {
        const std::lock_guard<std::mutex> lock(*mutex);
        return ci_parser->parseObject(entry->timestamp, payload);
      }
      return ci_parser->parseObject(entry->timestamp, payload);
    }();
    if (!record.has_value() || sdk::typeOf(record->object) != sdk::BuiltinObjectType::kCameraInfo) {
      continue;
    }
    const auto* ci = record->object.get<sdk::CameraInfo>();
    if (ci != nullptr && !ci->frame_id.empty()) {
      by_frame.insert_or_assign(ci->frame_id, *ci);
    }
  }
  return by_frame;
}
}  // namespace

ImageLayer::ImageLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name, QObject* parent)
    : Scene2DLayer(topic_id, object_type, display_name, u"Image"_s, parent) {}

std::unique_ptr<MediaSource> ImageLayer::createMediaSource(const SceneLayerContext& ctx) {
  auto* session = ctx.session;
  auto* store = objectStore();
  if (session == nullptr || store == nullptr) {
    return nullptr;
  }

  if (objectType() != sdk::BuiltinObjectType::kImage) {
    return nullptr;
  }

  auto pipeline = makeScene2DPipelineFor(objectType());
  const auto parser_binding = session->parserBindingForObjectTopic(topicId());
  auto* parser = parser_binding.parser;
  const bool canonical_blob =
      parser == nullptr && topicUsesCanonicalImageCodec(store->descriptor(topicId()).metadata_json);
  if (parser == nullptr && !canonical_blob && pipeline == nullptr) {
    qCWarning(lcScene2DImageLayer) << "no parser and no built-in image pipeline for topic_id=" << topicId().id;
    return nullptr;
  }

  std::unique_ptr<ImagePipelineSource> image_src;
  if (parser != nullptr) {
    image_src =
        std::make_unique<ImagePipelineSource>(store, topicId(), parser, parser_binding.mutex, parser_binding.keepalive);
  } else if (canonical_blob) {
    image_src = std::make_unique<ImagePipelineSource>(store, topicId(), ImagePipelineSource::CanonicalImageCodec{});
  } else {
    image_src = std::make_unique<ImagePipelineSource>(store, topicId(), std::move(pipeline));
  }

  // Supply camera calibration (parsed via each camera_info topic's parser) so the
  // decode worker can rectify frames to native resolution and 2D annotation
  // overlays line up. Built once here, before the first setTimestamp() request:
  // sufficient for file load (all camera_info is ingested before the layer
  // attaches); a camera_info published after attach is not retro-applied.
  image_src->setCameraInfoMap(collectCameraInfoByFrameId(session, store));

  image_source_ = image_src.get();
  image_src->setRectifyEnabled(rectify_enabled_);
  image_src->setFrameReadyCallback(makeQueuedRepaintCallback());
  return image_src;
}

QWidget* ImageLayer::createConfigWidget(QWidget* parent) {
  auto* widget = new QWidget(parent);
  auto* layout = new QFormLayout(widget);
  layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));

  auto* rectify = new ToggleSwitch(widget);
  rectify->setChecked(rectify_enabled_, /*animate=*/false);  // snap to state without emitting toggled
  rectify->setToolTip(
      tr("Rectify (lens-undistort) the image using its camera calibration when a matching\n"
         "CameraInfo is available. Turn off to show the raw image — e.g. when the stream is\n"
         "already rectified and would otherwise be undistorted twice."));
  layout->addRow(tr("Rectify"), rectify);
  connect(rectify, &ToggleSwitch::toggled, this, [this](bool on) { setRectifyEnabled(on); });

  return widget;
}

void ImageLayer::onBeforeDetach() {
  image_source_ = nullptr;
}

void ImageLayer::setRectifyEnabled(bool enabled) {
  if (rectify_enabled_ == enabled) {
    return;
  }
  rectify_enabled_ = enabled;
  applyOptions();
  emit configurationChanged();
}

void ImageLayer::applyOptions() {
  if (image_source_ == nullptr) {
    return;
  }
  // setRectifyEnabled invalidates the worker when the value changes so it re-emits the
  // current frame in the new mode; re-apply the current tracker time so the re-emit
  // happens now (not on the next tick) while paused.
  image_source_->setRectifyEnabled(rectify_enabled_);
  if (const auto ts = lastTrackerTimeNs(); ts.has_value()) {
    image_source_->setTimestamp(*ts);
  }
  emit repaintRequested();
}

void ImageLayer::saveOptions(QDomElement& element) const {
  element.setAttribute(u"rectify_enabled"_s, rectify_enabled_ ? u"true"_s : u"false"_s);
}

bool ImageLayer::loadOptions(const QDomElement& element) {
  // Absent attribute (layout saved before this toggle existed) keeps the current
  // default (true) -> rectification stays on, preserving the historical behaviour.
  const QString saved = element.attribute(u"rectify_enabled"_s, rectify_enabled_ ? u"true"_s : u"false"_s);
  if (saved != u"true"_s && saved != u"false"_s) {
    return false;
  }
  const bool restored = saved == u"true"_s;
  if (rectify_enabled_ != restored) {
    rectify_enabled_ = restored;
    applyOptions();
  }
  return true;
}

}  // namespace PJ
