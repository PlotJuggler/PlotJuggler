#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_scene2d_core/cancel_token.h"
#include "pj_scene2d_core/decoded_frame.h"

struct AVCodecContext;
struct AVCodecParameters;
struct AVBufferRef;
struct AVFrame;
struct SwsContext;

namespace PJ {

/// Converts a 4:2:0 YUV frame (kYUV420P or kNV12) into tightly packed rgb24 at `out` (width * height * 3 bytes), with
/// the matrix the media shader applies for its colour space and range (`buildYuvMatrix`), through swscale: within 3
/// levels per channel of it (swscale's fast converter at a width that is a multiple of 16 and an even height, its exact
/// rounding, within 1, at any other). False for any other format or a frame whose buffer does not match its size. Any
/// thread.
[[nodiscard]] bool convertToRgb24(const DecodedFrame& frame, uint8_t* out);

/// Compressed video packets to DecodedFrame: YUV420P from software decode, NV12 from hardware decode, with the
/// codec's colorimetry. open() takes the first hardware backend of this FFmpeg build that can decode the codec, else
/// software (docs/TECHNICAL_NOTES.md "VAAPI device selection"). Not thread-safe: one thread at a time.
class FfmpegDecoder {
 public:
  FfmpegDecoder();
  ~FfmpegDecoder();

  FfmpegDecoder(const FfmpegDecoder&) = delete;
  FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;
  FfmpegDecoder(FfmpegDecoder&&) = delete;
  FfmpegDecoder& operator=(FfmpegDecoder&&) = delete;

  bool open(const AVCodecParameters* params);

  /// Unexpected on EAGAIN (need more packets), on error, or when `cancel` is set between send and receive. Feed
  /// packets in decode order with their real PTS (not the DTS store key), so `DecodedFrame::pts` comes back in
  /// presentation order on B-frame streams.
  Expected<DecodedFrame> decode(
      const uint8_t* data, size_t size, int64_t pts, int64_t dts, const CancelTokenPtr& cancel = nullptr);

  /// Advances the codec without the hardware download or conversion; the decoded frame's PTS, or -1 on EAGAIN or
  /// error.
  int64_t decodeSkip(const uint8_t* data, size_t size, int64_t pts, int64_t dts);

  /// Pays the hardware download and conversion only when `want` accepts the output frame's PTS (the frame out is not
  /// necessarily the packet in, under B-frame reorder). A frame `want` rejects comes back null; EAGAIN and errors
  /// are unexpected. An empty `want` accepts every frame.
  Expected<DecodedFrame> decodeFiltered(
      const uint8_t* data, size_t size, int64_t pts, int64_t dts, const std::function<bool(int64_t)>& want);

  /// Receives one frame without sending; `want` as in decodeFiltered(), "need more packets" when the queue is empty.
  /// Pump it to EAGAIN before each sendOnly(): libavcodec guarantees a send cannot return EAGAIN right after a
  /// receive did, and the combined calls' EAGAIN recovery discards a queued frame (on a loaded B-frame stream, the
  /// next request's frame).
  Expected<DecodedFrame> receiveFiltered(const std::function<bool(int64_t)>& want);

  /// Sends without the combined calls' receive-and-discard EAGAIN recovery (ENOMEM is still retried after a flush).
  /// False when the codec rejects the packet.
  bool sendOnly(const uint8_t* data, size_t size, int64_t pts, int64_t dts);

  /// Required after a seek.
  void flush();

  /// End of stream: returns the buffered frames; flush() before decoding again.
  std::vector<DecodedFrame> drain();

  [[nodiscard]] int width() const;
  [[nodiscard]] int height() const;

 private:
  enum class SendPolicy {
    kCombinedDecode,  // ENOMEM flush-retry + EAGAIN receive/discard/retry.
    kStreamingPump,   // ENOMEM flush-retry only; the caller drained the output queue.
    kDecodeSkip       // No retry.
  };

  Expected<DecodedFrame> decodePacket(
      const uint8_t* data, size_t size, int64_t pts, int64_t dts, const std::function<bool(int64_t)>& want,
      const CancelTokenPtr& cancel);
  Expected<DecodedFrame> receiveFrame(const std::function<bool(int64_t)>& want);
  // Copies the bytes, so the caller's buffer need not outlive the call. avcodec_send_packet's result.
  int sendPacket(const uint8_t* data, size_t size, int64_t pts, int64_t dts, SendPolicy policy);
  Expected<DecodedFrame> avFrameToDecodedFrame(AVFrame* frame);

  AVCodecContext* codec_ctx_ = nullptr;
  AVBufferRef* hw_device_ctx_ = nullptr;
  SwsContext* sws_ctx_ = nullptr;
  int sws_src_w_ = 0;
  int sws_src_h_ = 0;
  int sws_src_fmt_ = -1;
  bool sws_full_range_ = false;  ///< Range sws_ctx_ keeps on both sides; part of its cache key.
};

}  // namespace PJ
