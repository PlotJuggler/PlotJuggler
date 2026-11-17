// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/ffmpeg_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "pj_base/sdk/platform.hpp"

namespace PJ {

namespace {

// Unspecified falls back to the players' heuristic (SD height -> BT.601, else BT.709). BT.2020 takes the same
// heuristic: HDR is not supported (docs/ARCHITECTURE.md "Known gaps").
YuvColorSpace mapColorSpace(AVColorSpace cs, int height) noexcept {
  switch (cs) {
    case AVCOL_SPC_BT709:
      return YuvColorSpace::kBt709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
    case AVCOL_SPC_FCC:
      return YuvColorSpace::kBt601;
    default:
      // UNSPECIFIED, RGB, BT2020, and anything else: guess by resolution.
      return (height > 0 && height <= 576) ? YuvColorSpace::kBt601 : YuvColorSpace::kBt709;
  }
}

// MPEG (studio/limited) is the default for video; an explicit JPEG/PC tag, or one
// of the deprecated YUVJ pixel formats some decoders still emit, means full range.
YuvColorRange mapColorRange(AVColorRange range, AVPixelFormat format) noexcept {
  switch (format) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUVJ440P:
    case AV_PIX_FMT_YUVJ411P:
      return YuvColorRange::kFull;
    default:
      return (range == AVCOL_RANGE_JPEG) ? YuvColorRange::kFull : YuvColorRange::kLimited;
  }
}

// Has `context` read and write samples of the same range, so sws_scale only
// repacks. By default it squeezes a YUVJ (full-range) source into a limited
// one, while the frame keeps its full-range tag for the GPU matrix.
void keepRange(SwsContext* context, bool full_range) {
  int* inverse_table = nullptr;
  int source_range = 0;
  int* table = nullptr;
  int destination_range = 0;
  int brightness = 0;
  int contrast = 0;
  int saturation = 0;
  if (sws_getColorspaceDetails(
          context, &inverse_table, &source_range, &table, &destination_range, &brightness, &contrast, &saturation) <
      0) {
    return;
  }
  const int range = full_range ? 1 : 0;
  sws_setColorspaceDetails(context, inverse_table, range, table, range, brightness, contrast, saturation);
}

// nullptr if no device opens. A VAAPI device is a DRM render node, and the default one is often a GPU without a
// VAAPI driver: honour PJ_VAAPI_DEVICE, else take the first render node that initialises (docs/TECHNICAL_NOTES.md
// "VAAPI device selection"). Other backends use the library default device.
AVBufferRef* tryHwDevice(AVHWDeviceType type) {
  AVBufferRef* ctx = nullptr;
#ifdef __linux__
  if (type == AV_HWDEVICE_TYPE_VAAPI) {
    if (const auto device = sdk::getEnv("PJ_VAAPI_DEVICE"); device) {
      if (av_hwdevice_ctx_create(&ctx, type, device->c_str(), nullptr, 0) >= 0) {
        return ctx;
      }
      ctx = nullptr;
    }
    // Render nodes are numbered from 128; this runs once per decoder open.
    for (int node = 128; node < 192; ++node) {
      const std::string path = "/dev/dri/renderD" + std::to_string(node);
      if (av_hwdevice_ctx_create(&ctx, type, path.c_str(), nullptr, 0) >= 0) {
        return ctx;
      }
      ctx = nullptr;
    }
    return nullptr;
  }
#endif
  if (av_hwdevice_ctx_create(&ctx, type, nullptr, nullptr, 0) >= 0) {
    return ctx;
  }
  return nullptr;
}

// AV_PIX_FMT_NONE when the codec has no hardware config for `type`.
AVPixelFormat hwPixelFormatFor(const AVCodec* codec, AVHWDeviceType type) {
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
    if (config == nullptr) {
      return AV_PIX_FMT_NONE;
    }
    if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 && config->device_type == type) {
      return config->pix_fmt;
    }
  }
}

// Picks the hardware surface format when offered, else FFmpeg's software default. The wanted format travels in
// codec_ctx->opaque, which keeps FFmpeg types out of ffmpeg_decoder.h.
AVPixelFormat pickHwFormat(AVCodecContext* ctx, const AVPixelFormat* formats) {
  const auto wanted = static_cast<AVPixelFormat>(reinterpret_cast<std::intptr_t>(ctx->opaque));
  for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
    if (*p == wanted) {
      return *p;
    }
  }
  return avcodec_default_get_format(ctx, formats);
}

}  // namespace

FfmpegDecoder::FfmpegDecoder() = default;

FfmpegDecoder::~FfmpegDecoder() {
  if (sws_ctx_ != nullptr) {
    sws_freeContext(sws_ctx_);
  }
  if (codec_ctx_ != nullptr) {
    avcodec_free_context(&codec_ctx_);
  }
  if (hw_device_ctx_ != nullptr) {
    av_buffer_unref(&hw_device_ctx_);
  }
}

bool FfmpegDecoder::open(const AVCodecParameters* params) {
  if (codec_ctx_ != nullptr) {
    avcodec_free_context(&codec_ctx_);
    codec_ctx_ = nullptr;
  }
  if (hw_device_ctx_ != nullptr) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }
  if (sws_ctx_ != nullptr) {
    sws_freeContext(sws_ctx_);
    sws_ctx_ = nullptr;
  }

  const AVCodec* codec = avcodec_find_decoder(params->codec_id);
  if (codec == nullptr) {
    return false;
  }

  codec_ctx_ = avcodec_alloc_context3(codec);
  if (codec_ctx_ == nullptr) {
    return false;
  }

  if (avcodec_parameters_to_context(codec_ctx_, params) < 0) {
    avcodec_free_context(&codec_ctx_);
    return false;
  }

  // The preference list first, then every other backend of this build; the first with a hardware config for the
  // codec and a device that opens wins, else software. The preference pass exists because
  // av_hwdevice_iterate_types() lists DXVA2 before D3D11VA, and the Windows package builds both.
  AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;
  AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
  const auto try_hw_type = [&](AVHWDeviceType type) {
    const AVPixelFormat pix_fmt = hwPixelFormatFor(codec, type);
    if (pix_fmt == AV_PIX_FMT_NONE) {
      return false;
    }
    hw_device_ctx_ = tryHwDevice(type);
    if (hw_device_ctx_ == nullptr) {
      return false;
    }
    hw_type = type;
    hw_pix_fmt = pix_fmt;
    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    return true;
  };
  static constexpr AVHWDeviceType kPreferredHwTypes[] = {AV_HWDEVICE_TYPE_D3D11VA};
  for (const AVHWDeviceType preferred : kPreferredHwTypes) {
    if (try_hw_type(preferred)) {
      break;
    }
  }
  for (AVHWDeviceType type = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
       hw_device_ctx_ == nullptr && type != AV_HWDEVICE_TYPE_NONE; type = av_hwdevice_iterate_types(type)) {
    if (std::find(std::begin(kPreferredHwTypes), std::end(kPreferredHwTypes), type) != std::end(kPreferredHwTypes)) {
      continue;
    }
    try_hw_type(type);
  }

  if (hw_device_ctx_ != nullptr) {
    codec_ctx_->opaque = reinterpret_cast<void*>(static_cast<std::intptr_t>(hw_pix_fmt));
    codec_ctx_->get_format = &pickHwFormat;
  }

  codec_ctx_->thread_count = 4;

  if (hw_device_ctx_ != nullptr) {
    fprintf(stderr, "[FfmpegDecoder] HW accel: %s\n", av_hwdevice_get_type_name(hw_type));
  } else {
    fprintf(stderr, "[FfmpegDecoder] HW accel: none (software decode)\n");
  }

  if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
    avcodec_free_context(&codec_ctx_);
    if (hw_device_ctx_ != nullptr) {
      av_buffer_unref(&hw_device_ctx_);
    }
    return false;
  }

  return true;
}

int FfmpegDecoder::sendPacket(const uint8_t* data, size_t size, int64_t pts, int64_t dts, SendPolicy policy) {
  AVPacket* pkt = av_packet_alloc();
  // pkt->buf stays unset, so avcodec_send_packet copies the bytes: the streaming decoder feeds spans aliasing a
  // transient store entry. Setting pkt->buf to an external ref would break that.
  pkt->data = const_cast<uint8_t*>(data);
  pkt->size = static_cast<int>(size);
  pkt->pts = pts;
  pkt->dts = dts;

  int ret = avcodec_send_packet(codec_ctx_, pkt);
  if (ret == AVERROR(ENOMEM) && policy != SendPolicy::kDecodeSkip) {
    // Surface pool exhausted: flush and retry once.
    avcodec_flush_buffers(codec_ctx_);
    ret = avcodec_send_packet(codec_ctx_, pkt);
  }
  if (ret == AVERROR(EAGAIN) && policy == SendPolicy::kCombinedDecode) {
    // Receive and discard on EAGAIN, then retry.
    AVFrame* drain_frame = av_frame_alloc();
    avcodec_receive_frame(codec_ctx_, drain_frame);
    av_frame_free(&drain_frame);
    ret = avcodec_send_packet(codec_ctx_, pkt);
  }
  av_packet_free(&pkt);
  return ret;
}

Expected<DecodedFrame> FfmpegDecoder::receiveFrame(const std::function<bool(int64_t)>& want) {
  AVFrame* frame = av_frame_alloc();
  int ret = avcodec_receive_frame(codec_ctx_, frame);
  if (ret < 0) {
    av_frame_free(&frame);
    if (ret == AVERROR(EAGAIN)) {
      return unexpected("need more packets");
    }
    return unexpected("avcodec_receive_frame failed");
  }
  if (want && !want(frame->pts)) {
    av_frame_free(&frame);
    return DecodedFrame{};  // decoded, but `want` rejected it
  }
  auto result = avFrameToDecodedFrame(frame);
  if (result.has_value()) {
    result->pts = frame->pts;
  }
  av_frame_free(&frame);
  return result;
}

Expected<DecodedFrame> FfmpegDecoder::receiveFiltered(const std::function<bool(int64_t)>& want) {
  if (codec_ctx_ == nullptr) {
    return unexpected("decoder not open");
  }
  return receiveFrame(want);
}

bool FfmpegDecoder::sendOnly(const uint8_t* data, size_t size, int64_t pts, int64_t dts) {
  if (codec_ctx_ == nullptr) {
    return false;
  }
  int ret = sendPacket(data, size, pts, dts, SendPolicy::kStreamingPump);
  return ret >= 0;
}

Expected<DecodedFrame> FfmpegDecoder::decodePacket(
    const uint8_t* data, size_t size, int64_t pts, int64_t dts, const std::function<bool(int64_t)>& want,
    const CancelTokenPtr& cancel) {
  if (codec_ctx_ == nullptr) {
    return unexpected("decoder not open");
  }

  if (sendPacket(data, size, pts, dts, SendPolicy::kCombinedDecode) < 0) {
    return unexpected("avcodec_send_packet failed");
  }

  if (cancel != nullptr && cancel->isCancelled()) {
    return unexpected("cancelled");
  }

  return receiveFrame(want);
}

Expected<DecodedFrame> FfmpegDecoder::decode(
    const uint8_t* data, size_t size, int64_t pts, int64_t dts, const CancelTokenPtr& cancel) {
  return decodePacket(data, size, pts, dts, std::function<bool(int64_t)>{}, cancel);
}

Expected<DecodedFrame> FfmpegDecoder::decodeFiltered(
    const uint8_t* data, size_t size, int64_t pts, int64_t dts, const std::function<bool(int64_t)>& want) {
  return decodePacket(data, size, pts, dts, want, CancelTokenPtr{});
}

int64_t FfmpegDecoder::decodeSkip(const uint8_t* data, size_t size, int64_t pts, int64_t dts) {
  if (codec_ctx_ == nullptr) {
    return -1;
  }

  int ret = sendPacket(data, size, pts, dts, SendPolicy::kDecodeSkip);
  if (ret < 0 && ret != AVERROR(EAGAIN)) {
    return -1;
  }

  AVFrame* frame = av_frame_alloc();
  ret = avcodec_receive_frame(codec_ctx_, frame);
  int64_t result_pts = (ret >= 0) ? frame->pts : -1;
  av_frame_free(&frame);
  return result_pts;
}

void FfmpegDecoder::flush() {
  if (codec_ctx_ != nullptr) {
    avcodec_flush_buffers(codec_ctx_);
  }
}

std::vector<DecodedFrame> FfmpegDecoder::drain() {
  std::vector<DecodedFrame> frames;
  if (codec_ctx_ == nullptr) {
    return frames;
  }

  avcodec_send_packet(codec_ctx_, nullptr);

  AVFrame* frame = av_frame_alloc();
  while (avcodec_receive_frame(codec_ctx_, frame) >= 0) {
    auto result = avFrameToDecodedFrame(frame);
    if (result.has_value()) {
      result->pts = frame->pts;
      frames.push_back(std::move(*result));
    }
    av_frame_unref(frame);
  }
  av_frame_free(&frame);
  return frames;
}

int FfmpegDecoder::width() const {
  return codec_ctx_ != nullptr ? codec_ctx_->width : 0;
}

int FfmpegDecoder::height() const {
  return codec_ctx_ != nullptr ? codec_ctx_->height : 0;
}

Expected<DecodedFrame> FfmpegDecoder::avFrameToDecodedFrame(AVFrame* frame) {
  AVFrame* sw_frame = frame;
  AVFrame* tmp_frame = nullptr;

  if (frame->hw_frames_ctx != nullptr) {
    tmp_frame = av_frame_alloc();
    if (av_hwframe_transfer_data(tmp_frame, frame, 0) < 0) {
      av_frame_free(&tmp_frame);
      return unexpected("HW frame transfer failed");
    }
    sw_frame = tmp_frame;
  }

  int w = sw_frame->width;
  int h = sw_frame->height;
  auto src_fmt = static_cast<AVPixelFormat>(sw_frame->format);

  // Read the colour tags from the decoded frame: a hardware download fills `sw_frame` with pixels only. The repack
  // below keeps the range, so the tags stay valid.
  const YuvColorSpace color_space = mapColorSpace(frame->colorspace, h);
  const YuvColorRange color_range = mapColorRange(frame->color_range, src_fmt);
  const bool full_range = color_range == YuvColorRange::kFull;

  if (src_fmt == AV_PIX_FMT_YUV420P) {
    int uv_w = (w + 1) / 2;
    int uv_h = (h + 1) / 2;
    auto buf_size = expectedBufferSize(w, h, PixelFormat::kYUV420P);
    auto pixels = std::make_shared<std::vector<uint8_t>>(buf_size);
    uint8_t* dst = pixels->data();
    int y_size = w * h;
    int uv_size = uv_w * uv_h;

    for (int row = 0; row < h; ++row) {
      std::memcpy(dst + row * w, sw_frame->data[0] + row * sw_frame->linesize[0], static_cast<size_t>(w));
    }
    for (int row = 0; row < uv_h; ++row) {
      std::memcpy(
          dst + y_size + row * uv_w, sw_frame->data[1] + row * sw_frame->linesize[1], static_cast<size_t>(uv_w));
    }
    for (int row = 0; row < uv_h; ++row) {
      std::memcpy(
          dst + y_size + uv_size + row * uv_w, sw_frame->data[2] + row * sw_frame->linesize[2],
          static_cast<size_t>(uv_w));
    }

    if (tmp_frame != nullptr) {
      av_frame_free(&tmp_frame);
    }

    DecodedFrame result;
    result.pixels = std::move(pixels);
    result.width = w;
    result.height = h;
    result.format = PixelFormat::kYUV420P;
    result.color_space = color_space;
    result.color_range = color_range;
    return result;
  }

  // NV12 stays NV12: the viewer samples it as two planes (no repack).
  if (src_fmt == AV_PIX_FMT_NV12) {
    const int uv_w = (w + 1) / 2;
    const int uv_h = (h + 1) / 2;
    const int y_size = w * h;
    auto pixels = std::make_shared<std::vector<uint8_t>>(expectedBufferSize(w, h, PixelFormat::kNV12));
    uint8_t* dst = pixels->data();

    for (int row = 0; row < h; ++row) {
      std::memcpy(dst + row * w, sw_frame->data[0] + row * sw_frame->linesize[0], static_cast<size_t>(w));
    }
    // Interleaved UV: uv_w pairs (2 bytes each) per row, uv_h rows.
    const int uv_row_bytes = 2 * uv_w;
    for (int row = 0; row < uv_h; ++row) {
      std::memcpy(
          dst + y_size + row * uv_row_bytes, sw_frame->data[1] + row * sw_frame->linesize[1],
          static_cast<size_t>(uv_row_bytes));
    }

    if (tmp_frame != nullptr) {
      av_frame_free(&tmp_frame);
    }

    DecodedFrame result;
    result.pixels = std::move(pixels);
    result.width = w;
    result.height = h;
    result.format = PixelFormat::kNV12;
    result.color_space = color_space;
    result.color_range = color_range;
    return result;
  }

  // Any other format (P010, YUV422, ...) is repacked to YUV420P.
  if (sws_ctx_ == nullptr || w != sws_src_w_ || h != sws_src_h_ || static_cast<int>(src_fmt) != sws_src_fmt_ ||
      full_range != sws_full_range_) {
    if (sws_ctx_ != nullptr) {
      sws_freeContext(sws_ctx_);
    }
    sws_ctx_ = sws_getContext(w, h, src_fmt, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (sws_ctx_ != nullptr) {
      keepRange(sws_ctx_, full_range);
    }
    sws_src_w_ = w;
    sws_src_h_ = h;
    sws_src_fmt_ = static_cast<int>(src_fmt);
    sws_full_range_ = full_range;
  }

  if (sws_ctx_ == nullptr) {
    if (tmp_frame != nullptr) {
      av_frame_free(&tmp_frame);
    }
    return unexpected("sws_getContext failed for pixel format conversion");
  }

  int uv_w = (w + 1) / 2;
  int uv_h = (h + 1) / 2;
  int y_size = w * h;
  int uv_size = uv_w * uv_h;
  auto pixels = std::make_shared<std::vector<uint8_t>>(expectedBufferSize(w, h, PixelFormat::kYUV420P));
  uint8_t* dst_planes[3] = {pixels->data(), pixels->data() + y_size, pixels->data() + y_size + uv_size};
  int dst_linesize[3] = {w, uv_w, uv_w};
  sws_scale(sws_ctx_, sw_frame->data, sw_frame->linesize, 0, h, dst_planes, dst_linesize);

  if (tmp_frame != nullptr) {
    av_frame_free(&tmp_frame);
  }

  DecodedFrame result;
  result.pixels = std::move(pixels);
  result.width = w;
  result.height = h;
  result.format = PixelFormat::kYUV420P;
  result.color_space = color_space;
  result.color_range = color_range;
  return result;
}

bool convertToRgb24(const DecodedFrame& frame, uint8_t* out) {
  const bool nv12 = frame.format == PixelFormat::kNV12;
  if ((!nv12 && frame.format != PixelFormat::kYUV420P) || !frame.isValid() || out == nullptr) {
    return false;
  }
  const int width = frame.width;
  const int height = frame.height;
  // Nearest chroma, as the shader samples it. SWS_POINT alone (swscale's fast converter) only at a width that is a
  // multiple of 16 and an even height: its x86 kernel writes 16 pixels at a time and elsewhere leaves a row's last
  // pixels unwritten or writes past the frame. Other frames take the exact rounding (docs/TECHNICAL_NOTES.md "CPU
  // RGB conversion for scripts").
  const bool fast = width % 16 == 0 && height % 2 == 0;
  const int flags = fast ? SWS_POINT : SWS_POINT | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;
  const int* coefficients =
      sws_getCoefficients(frame.color_space == YuvColorSpace::kBt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
  const int source_range = frame.color_range == YuvColorRange::kFull ? 1 : 0;
  const int chroma_width = (width + 1) / 2;
  const size_t luma_size = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t chroma_size = static_cast<size_t>(chroma_width) * static_cast<size_t>((height + 1) / 2);
  const uint8_t* base = frame.pixels->data();
  const int strides[3] = {width, nv12 ? chroma_width * 2 : chroma_width, nv12 ? 0 : chroma_width};
  const int destination_stride[1] = {width * 3};
  // Converts `rows` rows from `first_row` (even) as a frame of their own.
  const auto convertRows = [&](int first_row, int rows) {
    SwsContext* context = sws_getContext(
        width, rows, nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P, width, rows, AV_PIX_FMT_RGB24, flags, nullptr,
        nullptr, nullptr);
    if (context == nullptr) {
      return false;
    }
    sws_setColorspaceDetails(context, coefficients, source_range, coefficients, 1, 0, 1 << 16, 1 << 16);
    const size_t chroma_offset = static_cast<size_t>(first_row / 2) * static_cast<size_t>(strides[1]);
    const uint8_t* planes[3] = {
        base + static_cast<size_t>(first_row) * static_cast<size_t>(width), base + luma_size + chroma_offset,
        nv12 ? nullptr : base + luma_size + chroma_size + chroma_offset};
    uint8_t* destination[1] = {out + static_cast<size_t>(first_row) * static_cast<size_t>(destination_stride[0])};
    const int converted = sws_scale(context, planes, strides, 0, rows, destination, destination_stride);
    sws_freeContext(context);
    return converted == rows;
  };
  // swscale stretches an odd height's ceil(h/2) chroma rows over its h rows, where each chroma row belongs to two
  // luma rows (the last one cropped), so from about mid-frame odd rows read the next chroma row. The even part
  // converts as is; the last row converts alone, reading its own chroma row.
  const int even_rows = height - height % 2;
  return (even_rows == 0 || convertRows(0, even_rows)) && (even_rows == height || convertRows(even_rows, 1));
}

}  // namespace PJ
