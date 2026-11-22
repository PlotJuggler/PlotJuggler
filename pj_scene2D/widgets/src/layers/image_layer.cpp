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
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/camera_info.hpp"
#include "pj_base/builtin/camera_info_codec.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/XmlStateOptions.h"
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

// Calibration is dataset-local and joined by frame_id. Classified topics need
// no naming convention; the suffix covers topics without type metadata.
std::unordered_map<std::string, sdk::CameraInfo> collectCameraInfoByFrameId(
    SessionManager* session, ObjectStore* store, DatasetId dataset_id) {
  std::unordered_map<std::string, sdk::CameraInfo> by_frame;
  constexpr std::string_view kSuffix = "/camera_info";
  for (const ObjectTopicId id : store->listTopics()) {
    const auto& descriptor = store->descriptor(id);
    if (descriptor.dataset_id != dataset_id) {
      continue;
    }
    const auto metadata = QJsonDocument::fromJson(QByteArray::fromStdString(descriptor.metadata_json)).object();
    const bool classified = metadata.value(u"builtin_object_type"_s).toString() == u"kCameraInfo"_s;
    const std::string& name = descriptor.topic_name;
    if (!classified && !name.ends_with(kSuffix)) {
      continue;
    }
    if (store->entryCount(id) == 0) {
      continue;
    }
    auto* ci_parser = session->parserForObjectTopic(id);
    const auto entry = store->latestAt(id, store->timeRange(id).second);
    if (!entry.has_value() || entry->payload.bytes.empty()) {
      continue;
    }
    const sdk::PayloadView payload = entry->payload;
    if (ci_parser == nullptr) {
      if (classified) {
        const auto camera = deserializeCameraInfo(payload.bytes.data(), payload.bytes.size());
        if (camera && !camera->frame_id.empty()) {
          by_frame.insert_or_assign(camera->frame_id, *camera);
        }
      }
      continue;
    }
    // MessageParser plugins are not thread-safe: serialize on the shared mutex, as ImagePipelineSource does.
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

  // Built once, before the first setTimestamp() (the source refuses it later): enough for a file, whose
  // camera_info is ingested before attach; a camera_info published after attach is not applied.
  image_src->setCameraInfoMap(collectCameraInfoByFrameId(session, store, store->descriptor(topicId()).dataset_id));

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
  rectify->setChecked(rectify_enabled_, /*animate=*/false);  // does not emit toggled
  updateRectifyControl(rectify);
  connect(this, &ImageLayer::repaintRequested, rectify, [this, rectify]() { updateRectifyControl(rectify); });
  layout->addRow(tr("Rectify"), rectify);
  connect(rectify, &ToggleSwitch::toggled, this, [this](bool on) { setRectifyEnabled(on); });

  return widget;
}

void ImageLayer::updateRectifyControl(ToggleSwitch* toggle) const {
  using RectifyStatus = ImagePipelineSource::RectificationStatus;
  const auto status = image_source_ != nullptr ? image_source_->rectificationStatus() : RectifyStatus::kWaitingForImage;
  toggle->setEnabled(status == RectifyStatus::kReady);
  switch (status) {
    case RectifyStatus::kWaitingForImage:
      toggle->setToolTip(tr("Rectification is unavailable until an image with matching CameraInfo is decoded."));
      break;
    case RectifyStatus::kMissingFrameId:
      toggle->setToolTip(tr("Rectification is unavailable: the image has no camera frame_id."));
      break;
    case RectifyStatus::kMissingCalibration:
      toggle->setToolTip(tr("Rectification is unavailable: no CameraInfo matches this image's frame_id."));
      break;
    case RectifyStatus::kInvalidCalibration:
      toggle->setToolTip(
          tr("Rectification is unavailable: the matching CameraInfo has invalid intrinsics or image size."));
      break;
    case RectifyStatus::kReady:
      toggle->setToolTip(
          tr("Undistort the image using its matching CameraInfo. Turn off to show the raw image,\n"
             "for example when the stream is already rectified."));
      break;
  }
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
  // Re-applying the tracker time re-emits the frame in the new mode now, also while paused.
  image_source_->setRectifyEnabled(rectify_enabled_);
  if (const auto ts = lastTrackerTimeNs(); ts.has_value()) {
    image_source_->setTimestamp(*ts);
  }
  emit repaintRequested();
}

void ImageLayer::saveOptions(QDomElement& element) const {
  element.setAttribute(u"rectify_enabled"_s, rectify_enabled_ ? u"true"_s : u"false"_s);
}

std::optional<bool> ImageLayer::parseOptions(const QDomElement& element, bool current) {
  bool rectify = current;
  if (!readXmlBool(element, u"rectify_enabled"_s, current, rectify)) {
    return std::nullopt;
  }
  return rectify;
}

bool ImageLayer::loadOptions(const QDomElement& element) {
  const std::optional<bool> restored = parseOptions(element, rectify_enabled_);
  if (!restored.has_value()) {
    return false;
  }
  if (rectify_enabled_ != *restored) {
    rectify_enabled_ = *restored;
    applyOptions();
  }
  return true;
}

}  // namespace PJ
