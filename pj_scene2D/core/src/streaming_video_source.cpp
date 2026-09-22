// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/streaming_video_source.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "pj_scene2d_core/entry_thumbnail_cache.h"
#include "pj_scene2d_core/streaming_video_decoder.h"
#include "pj_scene2d_core/video_frame_nal_extractor.h"

namespace PJ {

namespace {
// A scrub (vs contiguous playback) is a backward jump or a forward jump larger
// than this; only a scrub publishes an instant thumbnail preview and only a
// scrub preempts the in-flight decode (see isScrub below).
constexpr int64_t kScrubPreviewThresholdNs = 500'000'000;  // 0.5 s

// Scrub test shared by the preview and the preempt predicate. `reference` is
// the in-flight/last-decoded position, `target` the newly requested one.
bool isScrub(int64_t reference, int64_t target) {
  return reference == INT64_MIN || target < reference || (target - reference) > kScrubPreviewThresholdNs;
}
}  // namespace

StreamingVideoSource::StreamingVideoSource(ObjectStore* store, ObjectTopicId topic)
    : decoder_(std::make_unique<StreamingVideoDecoder>()) {
  decoder_->attach(store, topic);
  startWorker();
}

StreamingVideoSource::StreamingVideoSource(
    ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser, std::shared_ptr<std::mutex> parser_mutex,
    std::shared_ptr<void> parser_keepalive)
    : parser_keepalive_(std::move(parser_keepalive)), decoder_(std::make_unique<StreamingVideoDecoder>()) {
  // Each concurrently-running decoder needs its own extractor (the single-thread
  // keepalive slot must not be shared). The playback decoder gets one here.
  decoder_->attach(store, topic, makeVideoFrameNalExtractor(parser, parser_mutex));

  // Scrub-preview thumbnails: only for BOUNDED (file-backed) topics. A streaming
  // topic carries a retention budget and is tip-following while live; its
  // thumbnails would be built on pause instead (not yet wired). The cache's own
  // resolution gate further skips low-res sources where on-scrub decode is cheap.
  const int64_t retention_ns = (store != nullptr) ? store->retentionBudget(topic).time_window_ns : -1;
  if (store != nullptr && retention_ns == 0) {
    thumbnail_cache_ =
        std::make_unique<EntryThumbnailCache>(store, topic, makeVideoFrameNalExtractor(parser, parser_mutex));
    thumbnail_cache_->buildAsync();
  }

  startWorker();
}

StreamingVideoSource::~StreamingVideoSource() {
  // Stop and join the decode worker (cancelling any in-flight GOP decode)
  // BEFORE thumbnail_cache_ / parser_keepalive_ are destroyed — the decode
  // closure reads both.
  worker_.stop();
}

void StreamingVideoSource::startWorker() {
  worker_.start(
      [this](const AsyncFrameWorker::Request& request, AsyncFrameWorker& worker) { decodeRequest(request, worker); },
      [this](const char* what) {
        // Same dedup as a persistent decode failure: a repeatedly-throwing
        // parser/extractor would otherwise log on every tick/scrub.
        if (what[0] == '\0') {
          fprintf(stderr, "[StreamingVideoSource] decode threw a non-std exception\n");
        } else if (last_decode_error_ != what) {
          last_decode_error_ = what;
          fprintf(stderr, "[StreamingVideoSource] decode threw: %s\n", what);
        }
      },
      AsyncFrameWorker::Options{
          .use_cancel_token = true,
          // Preempt ONLY on scrubs. A cancelled decode wipes the decoder's
          // forward-continuation state (StreamingVideoDecoder::serveForward
          // on_cancelled), so the next request pays a full GOP re-seek; if
          // contiguous playback ticks (one frame forward, ~16ms at 60 Hz)
          // preempted too, every decode would be cancelled before delivering
          // and playback would drop to zero frames. Letting the in-flight
          // decode finish costs at most one frame of latency — the pending
          // target is still latest-wins coalesced.
          .preempt_predicate = [](Timestamp in_flight, Timestamp incoming) { return isScrub(in_flight, incoming); }});
}

void StreamingVideoSource::setTimestamp(int64_t ts_ns) {
  worker_.requestDecode(ts_ns);
}

std::optional<MediaFrame> StreamingVideoSource::takeFrame() {
  auto frame = worker_.take();
  if (!frame.has_value()) {
    return std::nullopt;
  }
  MediaFrame mf;
  mf.base = std::move(*frame);
  return mf;
}

void StreamingVideoSource::setFrameReadyCallback(std::function<void()> cb) {
  worker_.setFrameReadyCallback(std::move(cb));
}

bool StreamingVideoSource::isInitialized() const {
  return decoder_->isInitialized();
}

void StreamingVideoSource::decodeRequest(const AsyncFrameWorker::Request& request, AsyncFrameWorker& worker) {
  const int64_t ts = request.target_ns;

  // Instant scrub preview: on a backward seek or a large forward jump, publish
  // the nearest thumbnail right away so the drag has feedback while the (slow,
  // 4K) GOP decode runs. The full-res frame replaces it below when ready.
  if (thumbnail_cache_ && isScrub(last_decoded_ts_, ts)) {
    if (auto preview = thumbnail_cache_->lookup(ts); preview.has_value()) {
      worker.deposit(std::move(*preview));
    }
  }

  auto result = decoder_->decodeAt(ts, request.cancel);
  if (result.has_value() && !result->isNull()) {
    last_decoded_ts_ = ts;
    last_decode_error_.clear();  // recovered — let the next genuine failure log again
    worker.deposit(std::move(*result));
  } else if (!result.has_value() && result.error() != "cancelled") {
    // Surface a genuine decode failure (unsupported codec, evicted keyframe,
    // parser/extractor error) instead of leaving a frozen frame with no clue.
    // "cancelled" is the benign latest-wins preemption and is skipped.
    if (result.error() != last_decode_error_) {
      last_decode_error_ = result.error();
      fprintf(
          stderr, "[StreamingVideoSource] decode failed @ts=%lld: %s\n", static_cast<long long>(ts),
          result.error().c_str());
    }
  }
}

}  // namespace PJ
