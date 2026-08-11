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

/// MediaSource for vector-overlay topics (annotations, markers, etc.).
///
/// Symmetric to ImagePipelineSource but produces SceneFrames in MediaFrame.overlays
/// rather than DecodedFrames in MediaFrame.base. On setTimestamp() it queries the
/// store via latestAt() and decodes the bytes synchronously — overlay messages
/// are small (KBs) so on-the-fly decode is cheap.
///
/// Ownership: `store` is NOT owned (must outlive this object). `decoder`
/// is owned (moved in). An entry decoder, when supplied, is owned but typically
/// closes over host state it does not own — whatever it captures must outlive
/// this object too.
class ScenePipelineSource : public MediaSource {
 public:
  /// Canonical-producer topics: the store holds bytes the decoder reads directly.
  /// @param store    ObjectStore to query (not owned)
  /// @param topic    Topic ID with serialized scene/annotation messages
  /// @param decoder  Format-specific decoder for this topic's schema (owned)
  ScenePipelineSource(ObjectStore* store, ObjectTopicId topic, std::unique_ptr<ISceneDecoder> decoder);

  /// Host-supplied decoding of one raw ObjectStore entry. The callback receives
  /// the format-specific scene decoder owned by this source, so the host can
  /// resolve its current parser lease and keep it alive across both parseObject
  /// and decode without leaking the host's binding type into this core.
  using ObjectEntryDecoder =
      std::function<Expected<SceneFrame>(ISceneDecoder& decoder, Timestamp timestamp, const sdk::PayloadView& payload)>;

  /// Parser-backed topics: the store holds the RAW source message (e.g.
  /// yolo_msgs/DetectionArray under pure-lazy ingest). The parser converts it to
  /// the canonical builtin object, which the decoder consumes directly —
  /// mirroring the 3D consumer pattern (see parse_locked.h).
  ///
  /// Parser lookup and lifetime are deliberately a host concern: the core knows
  /// only how to fetch an entry and invoke this decoder. The host callback must
  /// resolve any replaceable parser lease per call and hold it until the parsed
  /// ObjectRecord has been decoded and destroyed.
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
  // When the cursor moves to a time with no annotation (no entry, empty payload,
  // or a decode failure) AFTER something was on screen, takeFrame() emits one
  // empty-overlay frame so the compositor clears the stale overlays — returning
  // nullopt would instead leave them up (the composite retains the last
  // contribution). Coalesced so an already-clear layer publishes nothing new.
  bool pending_clear_ = false;
  bool last_emitted_empty_ = true;
  int64_t last_ts_ = INT64_MIN;
  // Set for host-decoded topics (normally parser-backed raw messages). Empty for
  // canonical-producer topics, whose store bytes the decoder reads directly.
  ObjectEntryDecoder entry_decoder_;
};

}  // namespace PJ
