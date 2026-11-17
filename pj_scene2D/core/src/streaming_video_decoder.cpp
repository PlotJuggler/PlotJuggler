// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/streaming_video_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <fmt/format.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "pj_datastore/saturating_time.hpp"
#include "pj_scene2d_core/ffmpeg_decoder.h"
#include "pj_scene2d_core/video_codec_utils.h"

namespace PJ {

namespace {

// The entry's bytes are one frame of raw H.264 Annex-B; the span aliases the entry's buffer.
Expected<StreamingVideoDecoder::ExtractedFrame> identityNalExtractor(const ResolvedObjectEntry& entry) {
  // No message carries a PTS here, so the store key (DTS) presents.
  return StreamingVideoDecoder::ExtractedFrame{
      entry.payload.bytes, "h264", saturatingSub(entry.timestamp, entry.payload_stamp_shift)};
}

}  // namespace

StreamingVideoDecoder::StreamingVideoDecoder() : nal_extractor_(identityNalExtractor) {}

StreamingVideoDecoder::~StreamingVideoDecoder() = default;

Expected<StreamingVideoDecoder::ExtractedFrame> StreamingVideoDecoder::extract(const ResolvedObjectEntry& entry) const {
  auto frame = nal_extractor_(entry);
  if (frame) {
    frame->pts = saturatingAdd(frame->pts, entry.payload_stamp_shift);
  }
  return frame;
}

void StreamingVideoDecoder::attach(ObjectStore* store, ObjectTopicId topic) {
  attach(store, topic, NalExtractor{});
}

void StreamingVideoDecoder::attach(ObjectStore* store, ObjectTopicId topic, NalExtractor extractor) {
  reset();
  store_ = store;
  topic_ = topic;
  nal_extractor_ = extractor ? std::move(extractor) : NalExtractor(identityNalExtractor);
}

Expected<DecodedFrame> StreamingVideoDecoder::decodeAt(Timestamp ts, const CancelTokenPtr& cancel) {
  if (store_ == nullptr) {
    return unexpected("not attached");
  }
  // Appends and front evictions leave every fed packet where its UID finds it; only a rewrite can change one.
  const uint64_t before = store_->contentIdentity(topic_).rewrite;
  auto result = decodeCurrent(ts, cancel);
  if (store_->contentIdentity(topic_).rewrite != before) {
    reset();
    return unexpected("video content changed during decode");
  }
  return result;
}

Expected<DecodedFrame> StreamingVideoDecoder::decodeCurrent(Timestamp ts, const CancelTokenPtr& cancel) {
  updateKeyframeIndex();
  if (codec_id_ == AV_CODEC_ID_NONE && !codec_format_.empty()) {
    return unexpected("unsupported video codec: '" + codec_format_ + "'");
  }

  // The store is keyed by DTS; presenting the greatest PTS <= ts is what plays B-frame video in order.
  auto target = presentationTargetAt(ts);
  if (!target.has_value()) {
    return unexpected("no entry at timestamp");
  }
  const Timestamp target_ts = target->first;  // PTS
  const PacketIdentity target_packet = target->second;
  auto target_idx_opt = store_->indexOf(topic_, target_packet.uid);
  if (!target_idx_opt.has_value()) {
    return unexpected("entry not available");
  }
  const size_t target_idx = *target_idx_opt;

  // Same frame again: the display polls faster than frames arrive.
  if (initialized_ && last_served_ts_ == target_ts && !last_frame_.isNull()) {
    return last_frame_;
  }

  // Forward request whose frame was already decoded during an earlier look-ahead:
  // serve straight from the reorder buffer, no codec work.
  if (initialized_ && last_served_ts_.has_value() && target_ts >= *last_served_ts_) {
    if (auto served = takeReadyFrame(target_ts); served.has_value()) {
      return *served;
    }
  }

  // Continue forward when the codec is live and at or before the target, else seek. last_served_ts_ is a PTS, so
  // forward playback continues even with B-frames.
  const bool can_forward =
      initialized_ && !ended_ && last_sent_uid_.valid() && last_served_ts_.has_value() && target_ts >= *last_served_ts_;
  if (can_forward) {
    // Re-resolve the feed cursor by UID: a front eviction shifts every index.
    auto fed_idx = store_->indexOf(topic_, last_sent_uid_);
    const size_t next_idx = fed_idx.has_value() ? *fed_idx + 1 : 0;
    return serveForward(next_idx, target_idx, target_packet.uid, target_ts, cancel);
  }

  // Seek (backward, first decode, or after a drain): the keyframe before the target in decode order.
  if (keyframes_.empty()) {
    return unexpected("no keyframe yet");
  }
  auto kf_ts = findKeyframeBefore(target_packet);
  if (!kf_ts.has_value()) {
    return unexpected("no keyframe before target");
  }
  auto kf_idx_opt = store_->indexOf(topic_, kf_ts->uid);
  if (!kf_idx_opt.has_value()) {
    return unexpected("keyframe evicted — target undecodable");
  }
  // Next to the keyframe's read, so a front eviction since the target's barely moves either: positions as packetAt
  // counts them, relative to the target at `target_idx`.
  const std::optional<size_t> target_now = store_->indexOf(topic_, target_packet.uid);
  if (!target_now.has_value() || *kf_idx_opt + target_idx < *target_now) {
    return unexpected("entry not available");
  }
  const size_t kf_idx = *kf_idx_opt + target_idx - *target_now;

  if (!initialized_) {
    auto kf_entry = packetAt(kf_idx, target_packet.uid, target_idx);
    if (!kf_entry.has_value()) {
      return unexpected("keyframe entry not available");
    }
    auto nal = extract(*kf_entry);
    if (!nal.has_value()) {
      return unexpected("keyframe extraction failed: " + nal.error());
    }
    if (!initDecoder(nal->format, nal->bytes)) {
      return unexpected(
          "failed to open decoder for codec: '" + (nal->format.empty() ? std::string("h264") : nal->format) + "'");
    }
  }

  decoder_->flush();
  ready_frames_.clear();
  last_served_ts_.reset();
  last_sent_uid_ = {};
  ended_ = false;

  // Skip-decode the frames before the target; serveForward materialises from the target on.
  for (size_t i = kf_idx; i < target_idx; ++i) {
    if (cancel != nullptr && cancel->isCancelled()) {
      return unexpected("cancelled");
    }
    auto entry = packetAt(i, target_packet.uid, target_idx);
    if (!entry.has_value()) {
      return unexpected(fmt::format("entry evicted mid-GOP (index {})", i));
    }
    last_sent_uid_ = entry->sequential_uid;
    auto nal = extract(*entry);
    if (!nal.has_value()) {
      return unexpected(fmt::format("frame extraction failed (index {}): {}", i, nal.error()));
    }
    decoder_->decodeSkip(nal->bytes.data(), nal->bytes.size(), fedPts(*nal, entry->sequential_uid), entry->timestamp);
  }

  return serveForward(target_idx, target_idx, target_packet.uid, target_ts, cancel);
}

std::optional<DecodedFrame> StreamingVideoDecoder::takeReadyFrame(Timestamp ts) {
  auto it = ready_frames_.find(ts);
  if (it == ready_frames_.end()) {
    return std::nullopt;
  }
  DecodedFrame frame = std::move(it->second);
  // Forward playback never revisits an earlier frame without a seek: drop them, so the buffer stays bounded.
  ready_frames_.erase(ready_frames_.begin(), std::next(it));
  last_served_ts_ = ts;
  last_frame_ = frame;
  return frame;
}

Timestamp StreamingVideoDecoder::fedPts(const ExtractedFrame& frame, SequentialUID uid) const {
  const auto candidates = presentation_index_.find(frame.pts);
  if (candidates == presentation_index_.end() || candidates->second.back().uid == uid) {
    return frame.pts;
  }
  return std::numeric_limits<Timestamp>::min();  // below every target: its frame is never kept
}

std::optional<ResolvedObjectEntry> StreamingVideoDecoder::packetAt(
    size_t index, SequentialUID anchor, size_t anchor_index) const {
  const std::optional<size_t> now = store_->indexOf(topic_, anchor);
  if (!now || index + *now < anchor_index) {
    return std::nullopt;
  }
  return store_->at(topic_, index + *now - anchor_index);
}

Expected<DecodedFrame> StreamingVideoDecoder::serveForward(
    size_t start_idx, size_t target_idx, SequentialUID target_uid, Timestamp target_ts, const CancelTokenPtr& cancel) {
  const size_t count = store_->entryCount(topic_);

  // A cancel can land after the target packet was fed but before its frame came out; continuing forward would skip
  // it ("forward decode produced no frame"), so a cancel forces the next decodeAt to re-seek.
  auto on_cancelled = [this]() -> Expected<DecodedFrame> {
    last_sent_uid_ = {};
    ready_frames_.clear();
    return unexpected("cancelled");
  };

  // Receive every queued frame BEFORE each send, so the send cannot hit EAGAIN, whose recovery discards a queued
  // frame (see FfmpegDecoder::receiveFiltered). Frames before the target are not materialised.
  const auto want = [target_ts](int64_t frame_pts) { return frame_pts >= target_ts; };
  auto pump = [&]() -> std::optional<DecodedFrame> {
    while (true) {
      auto frame = decoder_->receiveFiltered(want);
      if (!frame.has_value()) {
        return std::nullopt;  // EAGAIN (queue empty) or transient: feed more
      }
      if (frame->isNull()) {
        continue;  // pre-target frame, cheaply skipped
      }
      ready_frames_.insert_or_assign(frame->pts, std::move(*frame));
      if (auto served = takeReadyFrame(target_ts); served.has_value()) {
        return served;
      }
    }
  };

  for (size_t i = start_idx; i < count; ++i) {
    // Before each feed, so last_sent_uid_ names only fed packets.
    if (cancel != nullptr && cancel->isCancelled()) {
      return on_cancelled();
    }
    if (auto served = pump(); served.has_value()) {
      return *served;
    }
    auto entry = packetAt(i, target_uid, target_idx);
    if (!entry.has_value()) {
      // A hole before the target breaks the decode chain; past it, stop feeding and drain.
      if (i <= target_idx) {
        return unexpected(fmt::format("entry evicted mid-GOP (index {})", i));
      }
      break;
    }
    last_sent_uid_ = entry->sequential_uid;
    // `entry` outlives the send call, so the extractor's aliased Span (into the
    // entry bytes or an anchor) stays valid while FFmpeg copies it.
    auto nal = extract(*entry);
    if (!nal.has_value()) {
      return unexpected(fmt::format("frame extraction failed (index {}): {}", i, nal.error()));
    }
    decoder_->sendOnly(nal->bytes.data(), nal->bytes.size(), fedPts(*nal, entry->sequential_uid), entry->timestamp);
  }

  // Final pump: the target may have surfaced off the last sends.
  if (auto served = pump(); served.has_value()) {
    return *served;
  }

  // At the tip without the target: drain. That ends the codec stream, so the next decodeAt must seek (flush).
  for (auto& f : decoder_->drain()) {
    if (!f.isNull() && f.pts >= target_ts) {
      ready_frames_.insert_or_assign(f.pts, std::move(f));
    }
  }
  ended_ = true;
  if (auto served = takeReadyFrame(target_ts); served.has_value()) {
    return *served;
  }
  return unexpected("forward decode produced no frame");
}

std::vector<Timestamp> StreamingVideoDecoder::keyframeTimestamps() {
  updateKeyframeIndex();
  std::vector<Timestamp> stamps;
  stamps.reserve(keyframes_.size());
  for (const auto& packet : keyframes_) {
    stamps.push_back(packet.dts);
  }
  return stamps;
}

bool StreamingVideoDecoder::sampledUsesKeyframeOnly(Timestamp interval_ns) {
  if (store_ == nullptr || interval_ns <= 0) {
    return false;
  }
  updateKeyframeIndex();
  // Need at least two keyframes to reason about spacing; a single keyframe over a
  // long span cannot cover a 1/interval cadence, so the forward pass wins.
  if (keyframes_.size() < 2) {
    return false;
  }
  // Keyframe-only is correct ONLY when every interval-wide window already holds a
  // keyframe — then it yields the SAME cadence as the forward pass for a fraction
  // of the work (each keyframe decodes in isolation). Guarantee that by requiring
  // the leading gap, every consecutive gap, and the trailing gap to be <= interval.
  const auto range = store_->timeRange(topic_);
  if (keyframes_.front().dts - range.first > interval_ns) {
    return false;
  }
  if (range.second - keyframes_.back().dts > interval_ns) {
    return false;
  }
  for (size_t i = 1; i < keyframes_.size(); ++i) {
    if (keyframes_[i].dts - keyframes_[i - 1].dts > interval_ns) {
      return false;
    }
  }
  return true;
}

void StreamingVideoDecoder::decodeSampled(Timestamp interval_ns, const std::function<bool(const DecodedFrame&)>& sink) {
  if (store_ == nullptr || interval_ns <= 0 || store_->entryCount(topic_) == 0) {
    return;
  }
  if (sampledUsesKeyframeOnly(interval_ns)) {
    decodeSampledKeyframes(interval_ns, sink);
  } else {
    decodeSampledForward(interval_ns, sink);
  }
}

void StreamingVideoDecoder::decodeSampledKeyframes(
    Timestamp interval_ns, const std::function<bool(const DecodedFrame&)>& sink) {
  // Precondition: sampledUsesKeyframeOnly() held. Each pick is one keyframe decoded alone after a flush.
  std::optional<Timestamp> next_sample;
  for (const PacketIdentity& packet : keyframes_) {
    const Timestamp kf_ts = packet.dts;
    if (next_sample.has_value() && kf_ts < *next_sample) {
      continue;  // this interval bucket already has a sample
    }
    auto idx = store_->indexOf(topic_, packet.uid);
    if (!idx.has_value()) {
      continue;
    }
    auto entry = store_->at(topic_, *idx);
    if (!entry.has_value()) {
      continue;
    }
    auto nal = extract(*entry);
    if (!nal.has_value()) {
      continue;
    }
    if (!initialized_ && !initDecoder(nal->format, nal->bytes)) {
      return;
    }
    // Flush so the keyframe decodes in isolation, then drain if frame-threading /
    // reorder latency held it back (a lone IDR often surfaces only on drain).
    decoder_->flush();
    DecodedFrame frame;
    auto decoded = decoder_->decode(nal->bytes.data(), nal->bytes.size(), nal->pts, entry->timestamp, nullptr);
    if (decoded.has_value() && !decoded->isNull()) {
      frame = std::move(*decoded);
    } else {
      auto drained = decoder_->drain();
      if (!drained.empty()) {
        frame = std::move(drained.back());
      }
    }
    // Accept planar YUV420P (software) or native NV12 (hardware decode); the
    // thumbnail encoder deinterleaves NV12. Reject anything else.
    if (frame.isNull() || (frame.format != PixelFormat::kYUV420P && frame.format != PixelFormat::kNV12)) {
      continue;
    }
    if (!sink(frame)) {
      return;  // resolution gate / budget reached
    }
    next_sample = kf_ts + interval_ns;
  }
}

void StreamingVideoDecoder::decodeSampledForward(
    Timestamp interval_ns, const std::function<bool(const DecodedFrame&)>& sink) {
  const size_t count = store_->entryCount(topic_);

  std::optional<Timestamp> next_sample;
  // Pure predicate: should THIS decoded frame (by true output PTS) be surfaced?
  // True for the first frame and whenever PTS crosses the next sample boundary.
  // `next_sample` advances only after a frame is actually sunk (below), so the
  // decoder pays the download+convert cost on exactly the sampled frames.
  auto want = [&](int64_t frame_pts) -> bool { return !next_sample.has_value() || frame_pts >= *next_sample; };

  // One forward pass over every entry. Every packet advances the codec, but
  // decodeFiltered materializes pixels only for frames `want` accepts — the rest
  // come back null and cost just send+receive.
  for (size_t i = 0; i < count; ++i) {
    auto entry = store_->at(topic_, i);
    if (!entry.has_value()) {
      break;  // gap — stop the pass
    }
    auto nal = extract(*entry);
    if (!nal.has_value()) {
      continue;
    }
    if (!initialized_ && !initDecoder(nal->format, nal->bytes)) {
      return;
    }
    auto frame = decoder_->decodeFiltered(nal->bytes.data(), nal->bytes.size(), nal->pts, entry->timestamp, want);
    if (!frame.has_value()) {
      continue;  // EAGAIN / transient error — keep feeding packets
    }
    if (frame->isNull() || (frame->format != PixelFormat::kYUV420P && frame->format != PixelFormat::kNV12)) {
      continue;  // decoded but not a usable/sampled frame (NV12 ok — encoder deinterleaves)
    }
    if (!sink(*frame)) {
      return;  // budget reached
    }
    next_sample = frame->pts + interval_ns;  // advance only on a real sample
  }

  // Tail: drain materializes the reorder/threading buffer (a handful of frames),
  // so apply the same sampling gate here before sinking.
  for (auto& frame : decoder_->drain()) {
    // Accept planar YUV420P (software) or native NV12 (hardware decode); the
    // thumbnail encoder deinterleaves NV12. Reject anything else.
    if (frame.isNull() || (frame.format != PixelFormat::kYUV420P && frame.format != PixelFormat::kNV12)) {
      continue;
    }
    if (!want(frame.pts)) {
      continue;
    }
    if (!sink(frame)) {
      return;
    }
    next_sample = frame.pts + interval_ns;
  }
}

void StreamingVideoDecoder::resetDecodeState() {
  decoder_.reset();
  initialized_ = false;
  last_sent_uid_ = {};
  ready_frames_.clear();
  last_served_ts_.reset();
  ended_ = false;
  last_frame_ = {};
  codec_id_ = 0;
  codec_format_.clear();
}

void StreamingVideoDecoder::reset() {
  resetDecodeState();
  keyframes_.clear();
  presentation_index_.clear();
  pruned_floor_.reset();
  pruned_first_uid_ = {};
  last_scanned_uid_ = {};
  indexed_identity_ = {};
}

bool StreamingVideoDecoder::isInitialized() const {
  return initialized_;
}

void StreamingVideoDecoder::updateKeyframeIndex() {
  if (store_ == nullptr) {
    return;
  }
  const auto identity = store_->contentIdentity(topic_);
  if (identity == indexed_identity_) {
    return;
  }
  if (identity.rewrite != indexed_identity_.rewrite) {
    reset();
  }
  if (store_->entryCount(topic_) == 0) {
    reset();
    indexed_identity_ = identity;
    return;
  }

  const Timestamp floor = store_->timeRange(topic_).first;
  const SequentialUID first = store_->firstSequentialUID(topic_);
  if (pruned_floor_ != floor || pruned_first_uid_ != first) {
    pruned_floor_ = floor;
    pruned_first_uid_ = first;
    const auto evicted = [this, floor](const PacketIdentity& packet) {
      return packet.dts < floor || (packet.dts == floor && !store_->indexOf(topic_, packet.uid));
    };
    std::erase_if(keyframes_, evicted);
    for (auto it = presentation_index_.begin(); it != presentation_index_.end();) {
      const SequentialUID winner = it->second.back().uid;
      std::erase_if(it->second, evicted);
      if (it->second.empty()) {
        it = presentation_index_.erase(it);
        continue;
      }
      if (it->second.back().uid != winner) {
        resetDecodeState();  // a retained packet presents this PTS again: the frame shown was the evicted one's
      }
      ++it;
    }
  }

  // Only the packets that arrived since the last scan (a rewrite reset the cursor above, rescanning everything).
  auto arrivals = store_->drainNewSince(topic_, last_scanned_uid_);
  std::sort(arrivals.begin(), arrivals.end(), [](const auto& left, const auto& right) {
    return left.timestamp < right.timestamp ||
           (left.timestamp == right.timestamp && left.sequential_uid < right.sequential_uid);
  });
  for (const auto& entry : arrivals) {
    auto nal = extract(entry);
    if (!nal) {
      continue;
    }
    const PacketIdentity packet{entry.timestamp, entry.sequential_uid};
    std::vector<PacketIdentity>& candidates = presentation_index_[nal->pts];
    if (!candidates.empty()) {
      // A later packet replaces the frame at this PTS, even when its DTS is newer.
      resetDecodeState();
    }
    resolveCodec(nal->format);
    if (isVideoKeyframe(codec_id_, nal->bytes.data(), nal->bytes.size())) {
      keyframes_.push_back(packet);
    }
    candidates.insert(
        std::upper_bound(
            candidates.begin(), candidates.end(), packet,
            [](const PacketIdentity& left, const PacketIdentity& right) { return left.uid < right.uid; }),
        packet);
  }
  indexed_identity_ = identity;
}

std::optional<StreamingVideoDecoder::PacketIdentity> StreamingVideoDecoder::findKeyframeBefore(
    PacketIdentity target) const {
  const auto it = std::upper_bound(
      keyframes_.begin(), keyframes_.end(), target, [](const PacketIdentity& left, const PacketIdentity& right) {
        return left.dts < right.dts || (left.dts == right.dts && left.uid < right.uid);
      });
  return it == keyframes_.begin() ? std::nullopt : std::optional<PacketIdentity>(*std::prev(it));
}

std::optional<std::pair<Timestamp, StreamingVideoDecoder::PacketIdentity>> StreamingVideoDecoder::presentationTargetAt(
    Timestamp ts) const {
  if (presentation_index_.empty()) {
    return std::nullopt;
  }
  auto it = presentation_index_.upper_bound(ts);
  if (it == presentation_index_.begin()) {
    if (store_ == nullptr || ts < store_->timeRange(topic_).first) {
      return std::nullopt;
    }
  } else {
    --it;
  }
  return std::make_pair(it->first, it->second.back());
}

void StreamingVideoDecoder::resolveCodec(std::string_view format) {
  if (codec_id_ != AV_CODEC_ID_NONE) {
    return;  // already latched
  }
  // An empty format (raw bytes, or a producer that omitted it) means H.264.
  codec_format_ = format.empty() ? std::string("h264") : std::string(format);
  codec_id_ = videoCodecIdFromFormat(codec_format_);
}

int StreamingVideoDecoder::computeReorderDelay() const {
  // The decoder's required reorder depth (FFmpeg `has_b_frames`/`video_delay`):
  // the k-th frame in presentation order can only be emitted after its packet —
  // at decode-order index i_k — has been fed, so the output lags the input by
  // max_k(i_k - k). Computed from the presentation index (pts -> entry key) the
  // keyframe scan already built; a stream without B-frames yields 0. This mirrors
  // what a container demuxer reports in AVCodecParameters::video_delay.
  int delay = 0;
  int presentation_rank = 0;
  for (const auto& [pts, candidates] : presentation_index_) {
    auto idx = store_->indexOf(topic_, candidates.back().uid);
    if (idx.has_value()) {
      delay = std::max(delay, static_cast<int>(*idx) - presentation_rank);
    }
    ++presentation_rank;
  }
  // H.264/HEVC cap the DPB reorder depth at 16; clamp so a pathological index
  // (e.g. mid-eviction skew) cannot impose an absurd output delay.
  return std::min(delay, 16);
}

bool StreamingVideoDecoder::initDecoder(std::string_view format, Span<const uint8_t> keyframe_bytes) {
  resolveCodec(format);
  AVCodecParameters* params = makeVideoCodecParams(codec_id_);
  if (params == nullptr) {
    return false;  // unknown/unbuilt codec
  }
  primeKeyframeParamSets(params, keyframe_bytes.data(), keyframe_bytes.size());
  // Opened with video_delay 0, libavcodec's H.264 decoder can drop a B-frame stream's first B-frames, and the
  // in-band SPS does not reliably correct it (no VUI bitstream restriction); the presentation index gives the
  // reorder depth a demuxer would report (docs/TECHNICAL_NOTES.md "B-frames").
  params->video_delay = computeReorderDelay();

  decoder_ = std::make_unique<FfmpegDecoder>();
  bool ok = decoder_->open(params);
  avcodec_parameters_free(&params);

  if (!ok) {
    decoder_.reset();
    return false;
  }

  initialized_ = true;
  return true;
}

}  // namespace PJ
