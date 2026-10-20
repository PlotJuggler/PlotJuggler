// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <mutex>

#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/streaming_video_decoder.h"
#include "pj_scripting/video_frame_source.h"

namespace PJ {

class MessageParserPluginBase;

/// The video source an on-demand script's `video:frame_at(t)` decodes through (docs/derived_objects/ARCHITECTURE.md
/// §6): one Scene2D `StreamingVideoDecoder` per topic, the frame presented at `t` (B-frames reordered), converted to
/// rgb8 under the run's native budget. Calls on one topic take turns; calls on different topics run at once. Built with
/// Scene2D; `MainWindow` hands it to `SessionManager::setVideoFrameSource`, so `pj_runtime` links no codec. The store
/// must outlive it.
class SceneVideoFrameSource final : public scripting::IVideoFrameSource {
 public:
  /// Makes the extractor of a topic's decoder, once per decoder, on the calling thread: the topic's parser when it has
  /// one (the extractor holds its library), else `canonicalExtractor()`.
  using ExtractorFactory = std::function<StreamingVideoDecoder::NalExtractor(ObjectTopicId)>;

  /// The identity of the parser a topic decodes through now (null: canonical bytes), read on every call, any thread:
  /// a decoder made with another parser (the topic's parser registered again) is replaced. Empty: always null.
  using ParserIdentity = std::function<const void*(ObjectTopicId)>;

  SceneVideoFrameSource(ObjectStore& store, ExtractorFactory extractors, ParserIdentity parsers = {});
  ~SceneVideoFrameSource() override;
  SceneVideoFrameSource(const SceneVideoFrameSource&) = delete;
  SceneVideoFrameSource& operator=(const SceneVideoFrameSource&) = delete;

  /// Reads an entry holding canonical `sdk::VideoFrame` bytes (a source that pushed the object itself).
  [[nodiscard]] static StreamingVideoDecoder::NalExtractor canonicalExtractor();
#ifdef PJ_HAS_FFMPEG
  /// Reads an entry through the topic's parser under its per-topic `mutex`, holding the parser's library (`keepalive`)
  /// for as long as the extractor and the object it parsed last live.
  [[nodiscard]] static StreamingVideoDecoder::NalExtractor parserExtractor(
      MessageParserPluginBase* parser, std::shared_ptr<std::mutex> mutex, std::shared_ptr<void> keepalive);
#endif

  [[nodiscard]] Expected<scripting::VideoFrameResult> frameAt(
      ObjectTopicId topic, Timestamp t, const std::shared_ptr<scripting::native::MemoryBudget>& budget,
      const std::function<bool()>& cancelled) override;
  void retireDataset(DatasetId dataset) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
