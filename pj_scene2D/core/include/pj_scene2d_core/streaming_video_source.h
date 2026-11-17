#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/async_frame_worker.h"
#include "pj_scene2d_core/media_source.h"

namespace PJ {

class EntryThumbnailCache;
class MessageParserPluginBase;
class StreamingVideoDecoder;

/// MediaSource for a VideoFrame topic: a StreamingVideoDecoder on an AsyncFrameWorker. A scrub (a backward jump or a
/// forward jump over 0.5 s) cancels the in-flight decode; a playback step lets it finish, since a cancel forces a GOP
/// re-seek (docs/ARCHITECTURE.md "Cancellation and scrub preemption"). Public methods are GUI-thread only. `store` is
/// not owned and must outlive the source.
class StreamingVideoSource : public MediaSource {
 public:
  /// Each entry holds one frame's raw H.264 Annex-B bytes (tests and simulated streams).
  StreamingVideoSource(ObjectStore* store, ObjectTopicId topic);

  /// Each entry is a message the topic's parser turns into an sdk::VideoFrame (makeVideoFrameNalExtractor). A null
  /// `parser` shows no frame. The arguments are those of ImagePipelineSource's parser constructor. A topic with no
  /// time-window retention also gets a scrub-thumbnail cache.
  StreamingVideoSource(
      ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser,
      std::shared_ptr<std::mutex> parser_mutex, std::shared_ptr<void> parser_keepalive);

  ~StreamingVideoSource() override;

  StreamingVideoSource(const StreamingVideoSource&) = delete;
  StreamingVideoSource& operator=(const StreamingVideoSource&) = delete;
  StreamingVideoSource(StreamingVideoSource&&) = delete;
  StreamingVideoSource& operator=(StreamingVideoSource&&) = delete;

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;

  /// Fired on the worker thread after each deposit. Without it a stopped scrub stays on the previous frame: the one
  /// takeFrame() after setTimestamp() races the worker. nullptr clears it.
  void setFrameReadyCallback(std::function<void()> cb);

  [[nodiscard]] bool isInitialized() const;

 private:
  void startWorker();
  // Worker thread: a thumbnail preview on a scrub, then the full decode.
  void decodeRequest(const AsyncFrameWorker::Request& request, AsyncFrameWorker& worker);

  // Declared FIRST so it is destroyed LAST: everything below must die while the plugin library is still loaded (the
  // worker calls parseObject, and a cached ObjectRecord's std::any manager and BufferAnchor control block live in
  // that library). Null for raw bytes.
  std::shared_ptr<void> parser_keepalive_;

  std::unique_ptr<StreamingVideoDecoder> decoder_;

  // Null unless the topic has no time-window retention. Its build thread parses through the same parser.
  std::unique_ptr<EntryThumbnailCache> thumbnail_cache_;

  // Worker thread: the last decoded target (a scrub is measured from it) and the last logged error (logged once).
  int64_t last_decoded_ts_ = INT64_MIN;
  std::string last_decode_error_;

  // Declared LAST: its stop() joins before thumbnail_cache_ and parser_keepalive_, which the decode reads, go away.
  AsyncFrameWorker worker_;
};

}  // namespace PJ
