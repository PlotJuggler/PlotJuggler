// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/scene2d_layer.h"

#include <QMetaObject>
#include <QPointer>
#include <QWidget>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "pj_base/time.hpp"  // PJ::fromRaw, PJ::toRaw
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/media_source.h"
using namespace Qt::StringLiterals;

namespace PJ {

Scene2DLayer::Scene2DLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, QString display_name, QString family_name,
    QObject* parent)
    : ISceneLayer(parent) {
  info_.topic_id = topic_id;
  info_.object_type = object_type;
  info_.display_name = std::move(display_name);
  info_.family_name = std::move(family_name);
  info_.visible = true;
}

Scene2DLayer::~Scene2DLayer() = default;

SceneLayerInfo Scene2DLayer::info() const {
  return info_;
}

PJ::Range<PJ::Timepoint> Scene2DLayer::timeRange() const {
  return PJ::liveTopicTimeRange(store_, info_.topic_id);
}

bool Scene2DLayer::attach(const SceneLayerContext& ctx) {
  session_ = ctx.session;
  if (session_ == nullptr) {
    emit warningChanged(true, tr("No active session"));
    return false;
  }
  store_ = &session_->objectStore();
  source_ = createMediaSource(ctx);
  if (source_ == nullptr) {
    store_ = nullptr;
    session_ = nullptr;
    emit warningChanged(true, tr("Layer source could not be created"));
    return false;
  }
  dataset_replace_connection_ =
      connect(session_, &SessionManager::datasetAboutToBeReplaced, this, [this](DatasetId dataset_id) {
        if (store_ == nullptr || source_ == nullptr || store_->descriptor(topicId()).dataset_id != dataset_id) {
          return;
        }
        // The signal is the transaction's pre-swap boundary. Arm the source now;
        // MainWindow's post-load tracker broadcast will see the bumped render key
        // and re-decode even when both cursor and active sample stamp are unchanged.
        ++data_generation_;
        source_->invalidate();
      });
  emit warningChanged(false, {});
  onAfterAttach();
  return true;
}

void Scene2DLayer::detach() {
  onBeforeDetach();
  QObject::disconnect(dataset_replace_connection_);
  dataset_replace_connection_ = {};
  source_.reset();
  store_ = nullptr;
  session_ = nullptr;
  last_tracker_time_ns_.reset();
}

void Scene2DLayer::setTrackerTime(PJ::Timepoint time) {
  last_tracker_time_ns_ = PJ::toRaw(time);
  if (source_ != nullptr) {
    source_->setTimestamp(PJ::toRaw(time));
  }
}

uint64_t Scene2DLayer::renderKey(PJ::Timepoint time) const {
  // Key the displayed frame by the STAMP of the store's active sample at/before
  // `time`, via indexAt() + entryTimestamps() — a pure binary search that never
  // resolves the bytes (no decode/decompression in the per-tick gate). Consecutive
  // 60 Hz ticks that land on the same image/video/depth frame coalesce into one
  // repaint; a new sample changes the key (and its async decode also repaints via
  // the frame-ready callback). Stamp, not index: eviction renumbers indices.
  uint64_t sample_key = static_cast<uint64_t>(PJ::toRaw(time));
  if (store_ != nullptr) {
    if (const auto index = store_->indexAt(topicId(), PJ::toRaw(time)); index.has_value()) {
      if (const auto stamps = store_->entryTimestamps(topicId()); *index < stamps.size()) {
        sample_key = static_cast<uint64_t>(stamps[*index]);
      } else {
        sample_key = PJ::kNoSampleRenderKey;
      }
    } else {
      sample_key = PJ::kNoSampleRenderKey;
    }
  }
  // Odd multiplication is injective modulo 2^64, so for an unchanged sample a
  // replacement generation always produces a different render key.
  return sample_key ^ (data_generation_ * 0x9e3779b97f4a7c15ULL);
}

void Scene2DLayer::setVisible(bool visible) {
  if (info_.visible == visible) {
    return;
  }
  info_.visible = visible;
  emit visibilityChanged(visible);
}

QWidget* Scene2DLayer::createConfigWidget(QWidget* parent) {
  return new QWidget(parent);
}

QDomElement Scene2DLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement element = doc.createElement(u"scene2d_layer"_s);
  saveOptions(element);
  return element;
}

bool Scene2DLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull()) {
    return true;
  }
  if (element.tagName() != u"scene2d_layer"_s) {
    return false;
  }
  return loadOptions(element);
}

std::optional<int64_t> Scene2DLayer::lastTrackerTimeNs() const noexcept {
  return last_tracker_time_ns_;
}

ObjectStore* Scene2DLayer::objectStore() const noexcept {
  return store_;
}

SessionManager* Scene2DLayer::sessionManager() const noexcept {
  return session_;
}

ObjectTopicId Scene2DLayer::topicId() const noexcept {
  return info_.topic_id;
}

sdk::BuiltinObjectType Scene2DLayer::objectType() const noexcept {
  return info_.object_type;
}

QString Scene2DLayer::displayName() const {
  return info_.display_name;
}

void Scene2DLayer::setSource(std::unique_ptr<MediaSource> source) {
  source_ = std::move(source);
}

MediaSource* Scene2DLayer::borrowedSource() const noexcept {
  return source_.get();
}

std::function<void()> Scene2DLayer::makeQueuedRepaintCallback() {
  return [qp = QPointer<Scene2DLayer>(this)]() {
    if (!qp) {
      return;
    }
    QMetaObject::invokeMethod(
        qp.data(),
        [qp]() {
          if (qp) {
            emit qp->repaintRequested();
          }
        },
        Qt::QueuedConnection);
  };
}

void Scene2DLayer::onAfterAttach() {}

void Scene2DLayer::onBeforeDetach() {}

void Scene2DLayer::saveOptions(QDomElement& /*element*/) const {}

bool Scene2DLayer::loadOptions(const QDomElement& /*element*/) {
  return true;
}

}  // namespace PJ
