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
// A forward jump over this is a scrub: it gets a thumbnail preview and preempts the in-flight decode.
constexpr int64_t kScrubPreviewThresholdNs = 500'000'000;

// `reference` is the in-flight or last decoded target.
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
  // One extractor per concurrent decoder: its keepalive slot is single-threaded.
  decoder_->attach(store, topic, makeVideoFrameNalExtractor(parser, parser_mutex));

  // Thumbnails only for topics without a time-window retention; a retained live topic gets none. The cache skips
  // low-resolution sources itself.
  const int64_t retention_ns = (store != nullptr) ? store->retentionBudget(topic).time_window_ns : -1;
  if (store != nullptr && retention_ns == 0) {
    thumbnail_cache_ =
        std::make_unique<EntryThumbnailCache>(store, topic, makeVideoFrameNalExtractor(parser, parser_mutex));
    thumbnail_cache_->buildAsync();
  }

  startWorker();
}

StreamingVideoSource::~StreamingVideoSource() {
  // Before thumbnail_cache_ and parser_keepalive_ are destroyed: the decode reads both.
  worker_.stop();
}

void StreamingVideoSource::startWorker() {
  worker_.start(
      [this](const AsyncFrameWorker::Request& request, AsyncFrameWorker& worker) { decodeRequest(request, worker); },
      [this](const char* what) {
        if (what[0] == '\0') {
          fprintf(stderr, "[StreamingVideoSource] decode threw a non-std exception\n");
        } else if (last_decode_error_ != what) {
          last_decode_error_ = what;
          fprintf(stderr, "[StreamingVideoSource] decode threw: %s\n", what);
        }
      },
      AsyncFrameWorker::Options{
          .use_cancel_token = true,
          // A cancel drops the decoder's forward continuation (a GOP re-seek follows), so a playback step must not
          // preempt: every decode would be cancelled before delivering (docs/TECHNICAL_NOTES.md "Scrub preemption").
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

  if (thumbnail_cache_ && isScrub(last_decoded_ts_, ts)) {
    if (auto preview = thumbnail_cache_->lookup(ts); preview.has_value()) {
      worker.deposit(std::move(*preview));
    }
  }

  auto result = decoder_->decodeAt(ts, request.cancel);
  if (result.has_value() && !result->isNull()) {
    last_decoded_ts_ = ts;
    last_decode_error_.clear();
    worker.deposit(std::move(*result));
  } else if (!result.has_value() && result.error() != "cancelled") {
    // Logged once per distinct error; "cancelled" is a preemption, not a failure.
    if (result.error() != last_decode_error_) {
      last_decode_error_ = result.error();
      fprintf(
          stderr, "[StreamingVideoSource] decode failed @ts=%lld: %s\n", static_cast<long long>(ts),
          result.error().c_str());
    }
  }
}

}  // namespace PJ
