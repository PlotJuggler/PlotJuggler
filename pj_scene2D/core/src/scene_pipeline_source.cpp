// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/scene_pipeline_source.h"

#include <cstdio>
#include <utility>

namespace PJ {

ScenePipelineSource::ScenePipelineSource(
    ObjectStore* store, ObjectTopicId topic, std::unique_ptr<ISceneDecoder> decoder)
    : store_(store), topic_(topic), decoder_(std::move(decoder)) {}

ScenePipelineSource::ScenePipelineSource(
    ObjectStore* store, ObjectTopicId topic, ObjectEntryDecoder entry_decoder, std::unique_ptr<ISceneDecoder> decoder)
    : store_(store), topic_(topic), decoder_(std::move(decoder)), entry_decoder_(std::move(entry_decoder)) {}

void ScenePipelineSource::setTimestamp(int64_t ts_ns) {
  if (ts_ns == last_ts_) {
    return;
  }
  last_ts_ = ts_ns;

  // No annotation here: clear only if something is shown.
  auto mark_cleared = [this] {
    pending_scene_.reset();
    pending_clear_ = !last_emitted_empty_;
  };

  // No store lock is held through the decode.
  auto entry = store_->latestAt(topic_, ts_ns);
  if (!entry.has_value() || entry->payload.anchor == nullptr || entry->payload.bytes.empty()) {
    mark_cleared();
    return;
  }

  auto apply = [&](Expected<SceneFrame> result) {
    if (result.has_value()) {
      pending_scene_ = std::move(*result);
    } else {
      fprintf(
          stderr, "[ScenePipelineSource] decode failed at ts=%lld: %s\n", static_cast<long long>(ts_ns),
          result.error().c_str());
      pending_scene_.reset();
    }
  };

  if (entry_decoder_) {
    apply(entry_decoder_(*decoder_, entry->timestamp, entry->payload));
  } else {
    apply(decoder_->decode(entry->payload.bytes.data(), entry->payload.bytes.size()));
  }
}

std::optional<MediaFrame> ScenePipelineSource::takeFrame() {
  if (pending_scene_.has_value()) {
    MediaFrame mf;
    mf.overlays.push_back(std::move(*pending_scene_));
    pending_scene_.reset();
    last_emitted_empty_ = false;
    return mf;
  }
  if (pending_clear_) {
    pending_clear_ = false;
    last_emitted_empty_ = true;
    return MediaFrame{};
  }
  return std::nullopt;
}

void ScenePipelineSource::invalidate() {
  last_ts_ = INT64_MIN;
  pending_scene_.reset();
}

}  // namespace PJ
