#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/cancel_token.h"
#include "pj_scene2d_core/decoded_frame.h"

namespace PJ {

class FfmpegDecoder;

/// Decodes streaming VideoFrame entries stored in ObjectStore, codec-generically
/// (H.264, HEVC, AV1) keyed on each topic's sdk::VideoFrame::format.
///
/// Each ObjectStore entry is exactly one frame: an Annex-B NAL stream (H.264 /
/// HEVC) or an OBU temporal unit (AV1). Keyframes carry their parameter sets
/// in-band (H.264 SPS/PPS, HEVC VPS/SPS/PPS, AV1 sequence header), so the decoder
/// opens from the codec id alone (no extradata) and learns geometry on the first
/// keyframe. A codec outside the supported set (e.g. VP9, which has an FFmpeg
/// decoder but no keyframe oracle, so it could not be sought) makes decodeAt()
/// fail with a clear "unsupported video codec" error rather than mis-decoding.
///
/// Two modes of use (mutually exclusive per REQUIREMENTS.md §4.3):
/// - **Live mode**: call decodeAt(timeRange().second) on each tick.
///   The decoder advances sequentially — no seeking needed.
/// - **Scrub mode**: buffer is frozen (no pushes/eviction). Call decodeAt()
///   with any timestamp in the retained window. The decoder seeks to the
///   nearest preceding keyframe and decodes forward to the target.
///
/// The decoder builds a keyframe index incrementally by NAL-inspecting each
/// new entry. This is the "streaming sources" path from REQUIREMENTS.md §4.4.
class StreamingVideoDecoder {
 public:
  /// One frame's compressed bytes plus its codec, extracted from a resolved
  /// ObjectStore entry. `bytes` is the raw Annex-B NAL stream (H.264/HEVC) or OBU
  /// temporal unit (AV1) for one frame; it ALIASES memory kept alive by the entry
  /// (and any anchor the extractor closes over), so it only needs to outlive the
  /// immediate decode call (the decoder copies into FFmpeg first). `format` is the
  /// lowercase codec id from sdk::VideoFrame::format ("h264"/"h265"/"av1"); empty
  /// on the raw-bytes path, where the codec is taken to be H.264.
  struct ExtractedFrame {
    Span<const uint8_t> bytes;
    std::string format;
    // True presentation timestamp (PTS) of this frame. The ObjectStore keys entries
    // by DTS (decode order), so PTS must travel separately to present B-frame video
    // in the right order. The parser-aware extractor reads it from
    // sdk::VideoFrame::timestamp_ns; the identity extractor sets it to the entry's
    // store key (DTS == PTS when there are no B-frames). On the payload clock: the
    // decoder adds the entry's payload_stamp_shift, so it indexes, presents and
    // takes queries on the dataset clock, and a merge's packets never collide.
    Timestamp pts = 0;
  };

  /// Extracts one frame's compressed bytes + codec out of a resolved entry.
  ///
  /// The default extractor returns `entry.payload.bytes` verbatim with format
  /// "h264" — the entry already holds raw Annex-B (the simulated-stream /
  /// round-trip path). A parser-aware extractor (installed via
  /// StreamingVideoSource's parser constructor) parses each entry's canonical
  /// PJ.VideoFrame / Foxglove CompressedVideo message and returns its `data` span
  /// plus `format`.
  using NalExtractor = std::function<Expected<ExtractedFrame>(const ResolvedObjectEntry&)>;

  StreamingVideoDecoder();
  ~StreamingVideoDecoder();

  StreamingVideoDecoder(const StreamingVideoDecoder&) = delete;
  StreamingVideoDecoder& operator=(const StreamingVideoDecoder&) = delete;
  StreamingVideoDecoder(StreamingVideoDecoder&&) = delete;
  StreamingVideoDecoder& operator=(StreamingVideoDecoder&&) = delete;

  /// Attach to an ObjectStore topic. Does not open the FFmpeg decoder yet —
  /// waits for the first keyframe to auto-detect codec parameters. Uses the
  /// identity extractor (entry bytes are already raw Annex-B).
  void attach(ObjectStore* store, ObjectTopicId topic);

  /// Attach with a custom NAL extractor — e.g. one that parses each entry's
  /// canonical VideoFrame wire message and returns the aliased `data` span.
  /// A null `extractor` is replaced by the identity extractor.
  void attach(ObjectStore* store, ObjectTopicId topic, NalExtractor extractor);

  /// Decode the frame at-or-before the given timestamp.
  /// Returns the decoded frame, or an error if:
  /// - Not attached
  /// - No keyframe exists yet (waiting to join stream)
  /// - The target's keyframe was evicted (undecodable)
  ///
  /// @param cancel  Optional cooperative cancel. A long forward (GOP) decode —
  ///   notably 4K — aborts and returns "cancelled" when the token is set, so a
  ///   newer scrub target can preempt it instead of waiting it out.
  Expected<DecodedFrame> decodeAt(Timestamp ts, const CancelTokenPtr& cancel = nullptr);

  /// Reset decoder state. Call when switching modes or after ObjectStore::clear().
  void reset();

  /// Has the decoder seen at least one keyframe and successfully opened?
  [[nodiscard]] bool isInitialized() const;

  /// Snapshot of known keyframe timestamps (ascending). Refreshes the keyframe
  /// index first, so it reflects every entry currently in the store. Exposes the
  /// codec-dispatched keyframe oracle for inspection/tests; decodeSampled also
  /// consults it to choose between its keyframe-only and forward strategies.
  [[nodiscard]] std::vector<Timestamp> keyframeTimestamps();

  /// Sample the attached stream for thumbnails, invoking `sink` for roughly one
  /// frame per `interval_ns` of presentation time (plus the first frame). Picks
  /// the cheaper of two strategies that both meet that cadence
  /// (see sampledUsesKeyframeOnly):
  ///   - **keyframe-only**: when the source's keyframes are already at least as
  ///     dense as the cadence, decode just ~1 (self-contained) keyframe per
  ///     interval — near-instant, no GOP walk.
  ///   - **forward**: otherwise decode every frame ONCE (codec state is
  ///     sequential) but pay the HW-download + YUV pack/convert only on the
  ///     ~1/interval frames surfaced. On 4K that download+convert is the cost the
  ///     gating avoids on the throwaway frames.
  /// Frames arrive roughly in ascending PTS order (the forward path's drained tail
  /// may not be strictly ordered, so sort if you need a strict order). `sink`
  /// returns false to stop early (e.g. budget reached). Use on a dedicated
  /// instance, not interleaved with decodeAt().
  void decodeSampled(Timestamp interval_ns, const std::function<bool(const DecodedFrame&)>& sink);

  /// Would decodeSampled(interval_ns) take the keyframe-only fast path? True when
  /// the source's keyframes are already at least as dense as the requested cadence
  /// (every interval-wide window contains one), so decoding just ~1 keyframe per
  /// interval yields the same cadence for a fraction of the work. Refreshes the
  /// keyframe index. Exposed mainly for diagnostics/tests; decodeSampled calls it.
  [[nodiscard]] bool sampledUsesKeyframeOnly(Timestamp interval_ns);

 private:
  // decodeSampled strategies, dispatched by sampledUsesKeyframeOnly():
  //  - forward: decode every frame once, materialize only the sampled ones.
  //  - keyframes: decode only ~1 independently-decodable keyframe per interval.
  void decodeSampledForward(Timestamp interval_ns, const std::function<bool(const DecodedFrame&)>& sink);
  void decodeSampledKeyframes(Timestamp interval_ns, const std::function<bool(const DecodedFrame&)>& sink);
  void updateKeyframeIndex();
  // One packet: its DTS (the store key) and its UID, so packets at an equal DTS stay apart.
  struct PacketIdentity {
    Timestamp dts = 0;
    SequentialUID uid;
  };
  // The keyframe at or before `target` in decode order (DTS, then arrival), or nullopt. Seeking is a decode-order
  // operation; timestamps can be negative (FFmpeg gives the first frames a negative DTS), hence no sentinel.
  [[nodiscard]] std::optional<PacketIdentity> findKeyframeBefore(PacketIdentity target) const;
  // Which frame to PRESENT at `ts`: the greatest PTS <= ts and the packet carrying it, or nullopt if `ts` precedes the
  // stream. With no B-frames pts == dts.
  [[nodiscard]] std::optional<std::pair<Timestamp, PacketIdentity>> presentationTargetAt(Timestamp ts) const;
  // The extractor's frame, its PTS moved onto the dataset clock (plus the entry's payload_stamp_shift).
  [[nodiscard]] Expected<ExtractedFrame> extract(const ResolvedObjectEntry& entry) const;
  // decodeAt's body; decodeAt checks that the store did not rewrite the topic meanwhile.
  Expected<DecodedFrame> decodeCurrent(Timestamp ts, const CancelTokenPtr& cancel);
  // Drops the codec state and the frames it buffered, keeping the index: what a packet replacing one already fed
  // needs, since no decoded or reference frame can prove it unchanged.
  void resetDecodeState();
  // Resolve the codec from the wire format string (idempotent): latches codec_id_
  // and codec_format_. An unknown/unbuilt format leaves codec_id_ == 0, which
  // decodeAt surfaces as an "unsupported video codec" error.
  void resolveCodec(std::string_view format);
  // The stream's B-frame reorder depth (FFmpeg video_delay/has_b_frames),
  // derived from the presentation index: max_k(decode_index(k-th by pts) - k).
  // 0 for streams without B-frames. Clamped to the codec DPB limit (16).
  [[nodiscard]] int computeReorderDelay() const;
  // Open the FFmpeg decoder for `format`'s codec, priming extradata from the
  // keyframe's in-band parameter sets and video_delay from the presentation
  // index — an open with video_delay 0 can silently DROP the stream's first
  // B-frames (see initDecoder impl). Returns false if the codec is
  // unknown/unbuilt or the decoder cannot open.
  bool initDecoder(std::string_view format, Span<const uint8_t> keyframe_bytes);
  // Feed decode-order entries from `start_idx`, buffering decoded frames with PTS
  // >= `target_ts` until the exact target frame surfaces or the tip is drained.
  // `target_idx` is only for hole detection before the target; FFmpeg reorder and
  // frame-thread delay mean the target may surface several packets later. The
  // indices are those of the moment `target_uid` sat at `target_idx` (packetAt).
  Expected<DecodedFrame> serveForward(
      size_t start_idx, size_t target_idx, SequentialUID target_uid, Timestamp target_ts, const CancelTokenPtr& cancel);
  // The PTS to feed `frame` (of packet `uid`) with: its own when it presents its PTS, else one below every target, so
  // a packet a newer one replaced at that PTS never overwrites the newer one's frame in a forward walk.
  [[nodiscard]] Timestamp fedPts(const ExtractedFrame& frame, SequentialUID uid) const;
  // The packet at position `index` as counted when `anchor` sat at `anchor_index`: a front eviction since shifts every
  // position, so a walk follows its packets instead of their old indices. nullopt once the packet (or the anchor) is
  // gone.
  [[nodiscard]] std::optional<ResolvedObjectEntry> packetAt(
      size_t index, SequentialUID anchor, size_t anchor_index) const;
  // Pop the buffered frame at exactly `ts` (dropping every earlier, now-past
  // frame), or nullopt if it is not buffered yet. Sets last_served_ts_/last_frame_.
  std::optional<DecodedFrame> takeReadyFrame(Timestamp ts);

  ObjectStore* store_ = nullptr;
  ObjectTopicId topic_{};
  // Extracts raw Annex-B bytes from a resolved entry. Defaults to identity
  // (entry.payload.bytes). Never null after construction/attach.
  NalExtractor nal_extractor_;

  std::unique_ptr<FfmpegDecoder> decoder_;
  bool initialized_ = false;

  // Keyframes in decode order. Only a rewrite (an earlier DTS, a removal) can land below the newest one, and a
  // rewrite rebuilds the index, so an incremental scan only appends.
  std::vector<PacketIdentity> keyframes_;
  // The newest packet scanned: the arrival cursor of the incremental scan.
  SequentialUID last_scanned_uid_;
  // The store revisions the index reflects; equal to the store's, nothing is scanned.
  OrderedEntries::ContentIdentity indexed_identity_;
  // Presentation index: real PTS -> the retained packets carrying it, oldest arrival first; the newest presents it,
  // and an older one again once the newer is evicted. Lets decodeAt() map a presentation time to the frame to
  // display, then seek and feed in decode order.
  std::map<Timestamp, std::vector<PacketIdentity>> presentation_index_;
  // The store front (DTS floor and first UID) at the last prune; the O(n) prune runs only when either moves.
  std::optional<Timestamp> pruned_floor_;
  SequentialUID pruned_first_uid_;
  // The last packet fed to the decoder (the feed cursor: a store position, not the PTS of a decoded frame).
  SequentialUID last_sent_uid_;
  // Decoded frames received but not yet served, keyed by presentation timestamp.
  // Buffering across calls lets forward playback feed each packet once and serve
  // each frame exactly once (O(1) amortised) while still returning the precise
  // requested frame despite the decoder's reorder/threading delay.
  std::map<Timestamp, DecodedFrame> ready_frames_;
  // PTS of the last frame served. Distinguishes forward continuation from a
  // backward seek and backs the same-timestamp re-request cache (last_frame_).
  std::optional<Timestamp> last_served_ts_;
  // Set after a tip drain (NULL-packet EOF): the codec must be flushed before any
  // further decode, so the next request is forced onto the (flushing) seek path.
  bool ended_ = false;
  DecodedFrame last_frame_;  // Cached value for same-timestamp re-requests

  // Codec resolved from the first entry's VideoFrame.format (libavcodec
  // AVCodecID as int; 0 = AV_CODEC_ID_NONE = unresolved/unsupported). One codec
  // per topic, latched once. codec_format_ keeps the wire string for diagnostics.
  int codec_id_ = 0;
  std::string codec_format_;
};

}  // namespace PJ
