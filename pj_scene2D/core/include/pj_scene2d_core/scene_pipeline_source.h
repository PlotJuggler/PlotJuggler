#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <climits>
#include <functional>
#include <memory>
#include <optional>

#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_core/scene_decoder.h"
#include "pj_scene2d_core/scene_frame.h"

namespace PJ {

/// MediaSource for overlay topics (MediaFrame.overlays). setTimestamp() decodes the entry at or before the time
/// synchronously on the GUI thread: overlay messages are small. `store`, and whatever an entry decoder captures, must
/// outlive the source.
class ScenePipelineSource : public MediaSource {
 public:
  /// The store holds canonical bytes.
  ScenePipelineSource(ObjectStore* store, ObjectTopicId topic, std::unique_ptr<ISceneDecoder> decoder);

  /// Decodes one raw entry with the host's parser, then `decoder`; the host's binding type stays out of this core.
  using ObjectEntryDecoder =
      std::function<Expected<SceneFrame>(ISceneDecoder& decoder, Timestamp timestamp, const sdk::PayloadView& payload)>;

  /// The store holds raw source messages. `entry_decoder` must resolve the parser lease per call and hold it until the
  /// parsed ObjectRecord is decoded and destroyed: the record's storage belongs to the parser's plugin.
  ScenePipelineSource(
      ObjectStore* store, ObjectTopicId topic, ObjectEntryDecoder entry_decoder,
      std::unique_ptr<ISceneDecoder> decoder);

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;
  void invalidate() override;

 private:
  ObjectStore* store_;
  ObjectTopicId topic_;
  std::unique_ptr<ISceneDecoder> decoder_;
  std::optional<SceneFrame> pending_scene_;
  // Moving to a time with no annotation after one was shown emits one empty-overlay frame: nullopt would leave the
  // old overlays up, since the composite retains each layer's last contribution.
  bool pending_clear_ = false;
  bool last_emitted_empty_ = true;
  int64_t last_ts_ = INT64_MIN;
  // Empty for canonical-byte topics.
  ObjectEntryDecoder entry_decoder_;
};

}  // namespace PJ
