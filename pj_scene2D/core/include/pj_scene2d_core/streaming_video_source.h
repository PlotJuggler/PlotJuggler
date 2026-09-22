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

/// MediaSource for streaming video from ObjectStore. Wraps
/// StreamingVideoDecoder on a dedicated worker thread.
///
/// setTimestamp() posts a request to the worker; takeFrame() polls
/// for the latest decoded result. Latest-wins semantics — if a new
/// timestamp arrives while the worker is busy, it picks up the new
/// request after completing the current decode.
///
/// Thread safety: setTimestamp() and takeFrame() must be called from
/// the main thread only. The worker thread is internal.
///
/// Ownership: `store` is NOT owned (must outlive this object).
/// See ARCHITECTURE.md §5.3.
class StreamingVideoSource : public MediaSource {
 public:
  /// Raw-bytes constructor: each ObjectStore entry already holds the raw
  /// Annex-B NAL stream for one frame (the simulated-stream / round-trip path).
  /// @param store  ObjectStore containing video entries (not owned)
  /// @param topic  Topic ID with H.264 VideoFrame entries
  StreamingVideoSource(ObjectStore* store, ObjectTopicId topic);

  /// Parser-mode constructor: each ObjectStore entry holds a wrapping canonical
  /// PJ.VideoFrame / Foxglove CompressedVideo message. The decoder installs a
  /// NAL extractor that, on each entry, locks `parser_mutex` (MessageParser
  /// plugins are not thread-safe — fastcdr et al. keep stateful scratch), calls
  /// `parser->parseObject`, downcasts the result to sdk::VideoFrame, and returns
  /// the contained `data` span. The span aliases the entry's buffer (kept alive
  /// by the resolved entry across extract+decode) — no copy of the H.264 blob.
  /// @param store         ObjectStore containing video entries (not owned)
  /// @param topic         Topic ID with VideoFrame message entries
  /// @param parser        MessageParser for this topic (not owned; may be null,
  ///                      in which case extraction always fails and no frame
  ///                      surfaces — mirrors ImagePipelineSource).
  /// @param parser_mutex  Shared mutex serialising parseObject across consumers
  ///                      of this parser singleton (may be null).
  /// @param parser_keepalive  Opaque shared owner of the parser handle (parser
  ///                      instance + plugin DSO). Held for this source's whole
  ///                      lifetime so the parser can't be torn down / dlclosed
  ///                      under an in-flight parseObject on the worker thread.
  ///                      Get it from SessionManager::parserKeepaliveForObjectTopic.
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

  /// Install a notification fired (FROM the worker thread) every time the worker
  /// has deposited a fresh decoded frame, i.e. takeFrame() now has data. Pass
  /// nullptr to clear. Mirrors ImagePipelineSource::setFrameReadyCallback: the
  /// decode is asynchronous, so a single takeFrame() right after setTimestamp()
  /// races the worker; consumers (the GUI dock) hook this to re-poll once the
  /// frame is ready instead of leaving a stopped scrub frozen on the previous
  /// frame. The callback runs on the worker thread — implementers must hop to
  /// their own thread (e.g. QMetaObject::invokeMethod(..., Qt::QueuedConnection)).
  /// Must be called from the same (main) thread as setTimestamp()/takeFrame().
  void setFrameReadyCallback(std::function<void()> cb);

  [[nodiscard]] bool isInitialized() const;

 private:
  // Arm worker_ with the decode body + error logging; called at the end of both
  // constructors once every member the closures touch is initialized.
  void startWorker();
  // The decode body run by worker_ for each coalesced request (worker thread):
  // optional instant thumbnail preview on scrubs, then the full-res decode.
  void decodeRequest(const AsyncFrameWorker::Request& request, AsyncFrameWorker& worker);

  // Keeps the MessageParser handle (parser instance + plugin DSO) mapped for as
  // long as this source — and thus the decode worker that calls parseObject — is
  // alive. Null in raw-bytes mode. See SessionManager::parserKeepaliveForObjectTopic.
  //
  // Declared FIRST so it is destroyed LAST. Everything below it must die while the
  // DSO is still mapped: the worker (no parseObject on a dlclosed parser) and every
  // cached parse — an ObjectRecord is a std::any whose manager function, and whose
  // BufferAnchor control block, are addresses inside that DSO.
  std::shared_ptr<void> parser_keepalive_;

  // Owns the NAL extractor, which caches the last parsed ObjectRecord in a slot —
  // hence destroyed before parser_keepalive_ drops (see above).
  std::unique_ptr<StreamingVideoDecoder> decoder_;

  // Scrub-preview thumbnail cache (file-backed topics only; null for streaming
  // and raw-bytes modes). Its build thread decodes keyframes through the same
  // parser and its extractor keeps its own record slot, so it too is destroyed —
  // and joined — before parser_keepalive_ drops.
  std::unique_ptr<EntryThumbnailCache> thumbnail_cache_;

  // Worker-thread-only decode state: ts of the last successfully decoded
  // full-res frame (tells a scrub from contiguous playback so thumbnail
  // previews fire only on scrubs), and the last failure text (dedupes a
  // persistent decode error to a single log line).
  int64_t last_decoded_ts_ = INT64_MIN;
  std::string last_decode_error_;

  // The shared latest-wins worker engine (request/result/callback channels +
  // thread + lost-wakeup-safe teardown), started with cancellation enabled so
  // a newer setTimestamp() preempts a slow (4K) GOP decode. Declared LAST so
  // its implicit stop() joins before thumbnail_cache_ (whose build thread the
  // decode path shares a parser with) and parser_keepalive_ are destroyed; the
  // explicit stop() in the destructor body is the primary safety net.
  AsyncFrameWorker worker_;
};

}  // namespace PJ
