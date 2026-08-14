// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/depth_pipeline_source.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/builtin/depth_image.hpp"  // sdk::DepthImage — the colormap decoder's input view
#include "pj_base/builtin/image.hpp"
#include "pj_scene2d_core/codecs.h"  // PngCodec (compressedDepth -> Mono16)
#include "pj_scene2d_core/decoded_frame.h"
#include "pj_scene2d_core/depth_range.h"    // depthPercentileRange (Auto-fit)
#include "pj_scene2d_core/image_resolve.h"  // resolveImage (parser or canonical blob)

namespace PJ {

namespace {

// isValidDepth (finite && > 0) is shared from pj_scene2d_core/depth_range.h.

// Per-sample depth readers. Selected once per frame (see resolveDepthFormat),
// never per pixel.
[[nodiscard]] std::optional<float> read16UC1(const uint8_t* src) noexcept {
  const auto mm = static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
  if (mm == 0) {
    return std::nullopt;
  }
  return static_cast<float>(mm) * 0.001f;
}

[[nodiscard]] std::optional<float> read32FC1(const uint8_t* src) noexcept {
  float meters = 0.0f;
  std::memcpy(&meters, src, sizeof(float));
  if (!isValidDepth(meters)) {
    return std::nullopt;
  }
  return meters;
}

// Resolved depth encoding: stride and sample reader paired so they cannot drift
// apart. Resolved once from the encoding string before the decode loop.
struct DepthFormat {
  size_t bytes_per_pixel = 0;
  std::optional<float> (*read)(const uint8_t*) noexcept = nullptr;
};

[[nodiscard]] std::optional<DepthFormat> resolveDepthFormat(std::string_view encoding) noexcept {
  if (encoding == "16UC1") {
    return DepthFormat{2, &read16UC1};
  }
  if (encoding == "32FC1") {
    return DepthFormat{4, &read32FC1};
  }
  return std::nullopt;
}

// Adapt a depth-encoded sdk::Image into the sdk::DepthImage the colormap decoder
// reads. Raw 16UC1/32FC1 alias the image bytes (zero-copy — read synchronously by
// the caller). compressedDepth is PNG-decoded by reusing the scene2D PngCodec
// (plus the same bare-PNG repair the image path applies) into `scratch` as 16UC1
// millimetres. Returns nullopt for a non-depth encoding or an undecodable payload.
[[nodiscard]] std::optional<sdk::DepthImage> toDepthImage(
    const sdk::Image& img, std::shared_ptr<std::vector<uint8_t>>& scratch) {
  sdk::DepthImage depth;
  depth.timestamp_ns = img.timestamp_ns;
  if (img.encoding == "16UC1" || img.encoding == "32FC1") {
    depth.width = img.width;
    depth.height = img.height;
    depth.encoding = img.encoding;
    // Some transports (Mosaico, serialization_format=image) losslessly PNG/JPEG-wrap
    // the raw depth buffer — e.g. an 8-bit grayscale PNG of width=stride. Recover the
    // flat bytes before aliasing; otherwise the compressed container is read as raw
    // depth and the frame is all-black. A non-container payload returns nullopt and we
    // alias the original bytes (the prior raw behaviour).
    if (auto flat = recoverContainerRawSamples(img.data.data(), img.data.size())) {
      scratch = std::move(*flat);
      depth.data = Span<const uint8_t>(scratch->data(), scratch->size());
    } else {
      depth.data = img.data;
    }
    return depth;
  }
  if (img.encoding == "compressedDepth") {
    auto png = std::make_shared<std::vector<uint8_t>>();
    // Some streams advertise compressedDepth but carry a bare PNG that begins at
    // the IHDR chunk type; restore the 8-byte signature + IHDR length so libpng
    // accepts it (mirrors imageDataBytes in the image path).
    static constexpr uint8_t kPngPrefix[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D};
    if (img.data.size() >= 4 && img.data.data()[0] == 'I' && img.data.data()[1] == 'H' && img.data.data()[2] == 'D' &&
        img.data.data()[3] == 'R') {
      png->insert(png->end(), kPngPrefix, kPngPrefix + sizeof(kPngPrefix));
    }
    png->insert(png->end(), img.data.data(), img.data.data() + img.data.size());

    DecodedFrame encoded;
    encoded.pixels = std::move(png);
    auto mono16 = PngCodec{}.decode(encoded);
    if (!mono16.has_value() || mono16->isNull() || mono16->format != PixelFormat::kMono16) {
      return std::nullopt;
    }
    scratch = mono16->pixels;  // keep the 16-bit depth alive through decodeDepthImage
    depth.width = static_cast<uint32_t>(mono16->width);
    depth.height = static_cast<uint32_t>(mono16->height);
    depth.encoding = "16UC1";  // libpng gave us 16-bit grayscale = depth in millimetres
    depth.data = Span<const uint8_t>(scratch->data(), scratch->size());
    return depth;
  }
  return std::nullopt;  // non-depth encoding (rgb8, jpeg, mono8, ...)
}

}  // namespace

DepthPipelineSource::DepthPipelineSource(ObjectStore* store, ObjectTopicId topic)
    : store_(store), topic_(topic), canonical_(true) {
  startWorker();
}

DepthPipelineSource::DepthPipelineSource(
    ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser, std::shared_ptr<std::mutex> parser_mutex,
    std::shared_ptr<void> parser_keepalive)
    : store_(store),
      topic_(topic),
      parser_(parser),
      parser_mutex_(std::move(parser_mutex)),
      parser_keepalive_(std::move(parser_keepalive)) {
  startWorker();
}

DepthPipelineSource::~DepthPipelineSource() {
  // Stop + join the worker before any member its decode closure reads (store_,
  // parser_, the params) is destroyed. AsyncFrameWorker::stop handles the
  // lost-wakeup hazard.
  worker_.stop();
}

void DepthPipelineSource::startWorker() {
  worker_.start(
      [this](const AsyncFrameWorker::Request& req, AsyncFrameWorker& worker) {
        // invalidate() asked for a fresh decode even at an unchanged entry: clear
        // the dedup so decodeAt() doesn't early-return on the same frame.
        if (req.force_redecode) {
          last_entry_uid_ = {};
        }
        auto result = decodeAt(req.target_ns);
        if (result.has_value() && !result->isNull()) {
          // Snapshot the raw float depth for the main-thread Auto-fit before the
          // frame is moved into the mailbox (shared_ptr aliasing — no copy).
          {
            std::lock_guard<std::mutex> lock(last_frame_mutex_);
            last_depth_pixels_ = result->pixels;
            last_depth_width_ = result->width;
            last_depth_height_ = result->height;
          }
          worker.deposit(std::move(*result));
        }
      },
      [](const char* what) {
        fprintf(
            stderr, "[DepthPipelineSource] decode threw: %s\n",
            (what != nullptr && what[0] != '\0') ? what : "(unknown)");
      });
}

void DepthPipelineSource::setTimestamp(int64_t ts_ns) {
  // Post and return immediately — the PNG inflate runs on the worker thread.
  worker_.requestDecode(ts_ns);
}

void DepthPipelineSource::invalidate() {
  // Force a re-decode even at an unchanged timestamp (e.g. a colormap/range
  // change); the worker re-delivers via the frame-ready callback so the view
  // repaints. Without the force, decodeAt() resolves the same entry and
  // early-returns, and the config change wouldn't show until the next sample.
  worker_.invalidate();
}

std::optional<MediaFrame> DepthPipelineSource::takeFrame() {
  auto frame = worker_.take();
  if (!frame.has_value() || frame->isNull()) {
    return std::nullopt;
  }
  float opacity = 1.0f;
  {
    std::lock_guard<std::mutex> lock(params_mutex_);
    opacity = opacity_;
  }
  MediaFrame media;
  media.pixel_layers.push_back(PixelLayer{std::move(*frame), opacity});
  return media;
}

std::optional<DecodedFrame> DepthPipelineSource::decodeAt(int64_t ts_ns) {
  if (store_ == nullptr) {
    return std::nullopt;
  }
  // Dedup metadata-first (latestEntryIdAt is a pure binary search): an unchanged
  // entry must not resolve payload bytes, which can be a cold refetch (file read
  // + chunk decompress) once the ResidentPayloadPool has evicted its seed. Keyed
  // on the entry UID, not its timestamp — the store permits duplicate stamps, so
  // a same-stamp replacement entry must still decode. Shared by the gate and the
  // post-resolve re-check (a racing insert can change the pick in between).
  const auto already_delivered = [this](SequentialUID uid) { return uid == last_entry_uid_; };
  const auto entry_id = store_->latestEntryIdAt(topic_, ts_ns);
  if (!entry_id.has_value()) {
    return std::nullopt;
  }
  if (already_delivered(entry_id->uid)) {
    return std::nullopt;  // same entry already delivered (force_redecode resets the dedup)
  }
  auto entry = store_->latestAt(topic_, ts_ns);
  if (!entry.has_value() || entry->payload.anchor == nullptr || entry->payload.bytes.empty()) {
    return std::nullopt;
  }
  if (already_delivered(entry->sequential_uid)) {
    return std::nullopt;
  }
  last_entry_uid_ = entry->sequential_uid;

  // Depth arrives as a canonical sdk::Image with a depth encoding (there is no
  // kDepthImage producer); resolve it (via the parser or a pj_image_v1 blob), then
  // convert to raw metric depth (the colormap is applied later on the GPU).
  auto resolved = resolveImage(parser_, parser_mutex_, canonical_, entry->timestamp, entry->payload);
  if (!resolved.has_value()) {
    fprintf(
        stderr, "[DepthPipelineSource] resolve failed at ts=%lld: %s\n", static_cast<long long>(ts_ns),
        resolved.error().message.c_str());
    return std::nullopt;
  }

  // Holds decoded compressedDepth bytes alive while decodeDepthImage reads them.
  std::shared_ptr<std::vector<uint8_t>> scratch;
  auto depth = toDepthImage(resolved->image, scratch);
  if (!depth.has_value()) {
    return std::nullopt;
  }
  return decodeDepthImage(*depth, resolved->pts);
}

void DepthPipelineSource::setFrameReadyCallback(std::function<void()> cb) {
  worker_.setFrameReadyCallback(std::move(cb));
}

void DepthPipelineSource::setColormap(uint8_t colormap_id) {
  {
    std::lock_guard<std::mutex> lock(params_mutex_);
    colormap_ = colormap_id;
  }
  invalidate();
}

void DepthPipelineSource::setInvert(bool invert) {
  {
    std::lock_guard<std::mutex> lock(params_mutex_);
    invert_ = invert;
  }
  invalidate();
}

void DepthPipelineSource::setRange(float near_m, float far_m) {
  if (far_m < near_m) {
    std::swap(near_m, far_m);
  }
  {
    std::lock_guard<std::mutex> lock(params_mutex_);
    near_m_ = near_m;
    far_m_ = far_m;
  }
  invalidate();
}

void DepthPipelineSource::setOpacity(float opacity) {
  {
    std::lock_guard<std::mutex> lock(params_mutex_);
    opacity_ = std::clamp(opacity, 0.0f, 1.0f);
  }
  invalidate();
}

std::optional<Range<float>> DepthPipelineSource::autoRange(float lo_frac, float hi_frac) const {
  std::shared_ptr<const std::vector<uint8_t>> pixels;
  int width = 0;
  int height = 0;
  {
    std::lock_guard<std::mutex> lock(last_frame_mutex_);
    pixels = last_depth_pixels_;
    width = last_depth_width_;
    height = last_depth_height_;
  }
  if (!pixels || width <= 0 || height <= 0) {
    return std::nullopt;
  }
  const size_t count = pixels->size() / sizeof(float);
  if (count < static_cast<size_t>(width) * static_cast<size_t>(height)) {
    return std::nullopt;  // buffer smaller than declared dims — refuse rather than misread
  }
  const auto* data = reinterpret_cast<const float*>(pixels->data());
  return depthPercentileRange(data, count, lo_frac, hi_frac);
}

std::optional<DecodedFrame> DepthPipelineSource::decodeDepthImage(const sdk::DepthImage& depth, int64_t pts) const {
  const auto format = resolveDepthFormat(depth.encoding);
  if (!format.has_value() || depth.width == 0 || depth.height == 0 || depth.data.empty()) {
    return std::nullopt;
  }
  const size_t bytes_per_pixel = format->bytes_per_pixel;
  const size_t pixel_count = static_cast<size_t>(depth.width) * static_cast<size_t>(depth.height);
  if (depth.data.size() < pixel_count * bytes_per_pixel) {
    return std::nullopt;
  }

  // Emit RAW metric depth as float32 — the media shader normalizes by near/far
  // and applies the colormap LUT on the GPU (no per-pixel CPU colormap). Invalid
  // samples become 0, which the shader treats as no-data (transparent).
  auto pixels = std::make_shared<std::vector<uint8_t>>(pixel_count * sizeof(float), 0);
  auto* out = reinterpret_cast<float*>(pixels->data());
  for (size_t i = 0; i < pixel_count; ++i) {
    out[i] = format->read(depth.data.data() + i * bytes_per_pixel).value_or(0.0f);
  }

  DecodedFrame frame;
  frame.pixels = std::move(pixels);
  frame.width = static_cast<int>(depth.width);
  frame.height = static_cast<int>(depth.height);
  frame.format = PixelFormat::kDepthR32F;
  frame.pts = depth.timestamp_ns != 0 ? depth.timestamp_ns : pts;
  // Snapshot the GPU colormap params (written by the main-thread setters) under
  // the lock — this runs on the worker thread.
  {
    std::lock_guard<std::mutex> lock(params_mutex_);
    frame.depth = DepthColorParams{
        .near_m = near_m_,
        .far_m = far_m_,
        .invert = invert_,
        .colormap = colormap_,
        .active = true,
    };
  }
  return frame;
}

}  // namespace PJ
