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

#include "pj_base/builtin/depth_image.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_scene2d_core/codecs.h"
#include "pj_scene2d_core/decoded_frame.h"
#include "pj_scene2d_core/depth_range.h"
#include "pj_scene2d_core/image_resolve.h"

namespace PJ {

namespace {

// Wire samples to metres, an invalid sample to 0. One loop per encoding, branch-free: a reader called through a
// pointer for every pixel cannot be vectorized.
void metersFrom16UC1(const uint8_t* src, float* out, size_t count) noexcept {
  for (size_t i = 0; i < count; ++i) {
    const auto mm = static_cast<uint16_t>(src[2 * i] | (src[2 * i + 1] << 8));
    out[i] = static_cast<float>(mm) * 0.001f;  // 0 mm, the no-data value, stays 0
  }
}

void metersFrom32FC1(const uint8_t* src, float* out, size_t count) noexcept {
  for (size_t i = 0; i < count; ++i) {
    float meters = 0.0f;
    std::memcpy(&meters, src + i * sizeof(float), sizeof(float));
    out[i] = isValidDepth(meters) ? meters : 0.0f;
  }
}

struct DepthFormat {
  size_t bytes_per_pixel = 0;
  void (*to_meters)(const uint8_t* src, float* out, size_t count) noexcept = nullptr;
};

[[nodiscard]] std::optional<DepthFormat> resolveDepthFormat(std::string_view encoding) noexcept {
  if (encoding == "16UC1") {
    return DepthFormat{2, &metersFrom16UC1};
  }
  if (encoding == "32FC1") {
    return DepthFormat{4, &metersFrom32FC1};
  }
  return std::nullopt;
}

// Raw 16UC1/32FC1 alias the image bytes (the caller reads them synchronously); compressedDepth is inflated into
// `scratch` as 16UC1 millimetres. nullopt for a non-depth encoding or an undecodable payload.
[[nodiscard]] std::optional<sdk::DepthImage> toDepthImage(
    const sdk::Image& img, std::shared_ptr<std::vector<uint8_t>>& scratch) {
  sdk::DepthImage depth;
  depth.timestamp_ns = img.timestamp_ns;
  if (img.encoding == "16UC1" || img.encoding == "32FC1") {
    depth.width = img.width;
    depth.height = img.height;
    depth.encoding = img.encoding;
    // A container-wrapped raw buffer (recoverContainerRawSamples) read as raw would show black.
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
    // Some streams advertising compressedDepth carry a PNG starting at the IHDR chunk type; restore the signature and
    // IHDR length so libpng accepts it.
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
    scratch = mono16->pixels;
    depth.width = static_cast<uint32_t>(mono16->width);
    depth.height = static_cast<uint32_t>(mono16->height);
    depth.encoding = "16UC1";  // millimetres
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
  // Before any member the decode closure reads is destroyed.
  worker_.stop();
}

void DepthPipelineSource::startWorker() {
  worker_.start(
      [this](const AsyncFrameWorker::Request& req, AsyncFrameWorker& worker) {
        if (req.force_redecode) {
          last_entry_uid_ = {};
        }
        auto result = decodeAt(req.target_ns);
        if (result.has_value() && result->isNull()) {
          {
            std::lock_guard<std::mutex> lock(last_frame_mutex_);
            last_depth_pixels_.reset();  // Auto-fit never fits a frame no longer shown
          }
          worker.deposit(std::move(*result));  // a clear
        } else if (result.has_value()) {
          // For autoRange(); shares the pixels.
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
  worker_.requestDecode(ts_ns);
}

void DepthPipelineSource::invalidate() {
  worker_.invalidate();
}

std::optional<MediaFrame> DepthPipelineSource::takeFrame() {
  auto frame = worker_.take();
  if (!frame.has_value()) {
    return std::nullopt;
  }
  if (frame->isNull()) {
    return MediaFrame{};  // a clear: no pixels, which the compositor and the viewer drop
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
  // Dedup by entry UID before resolving bytes: resolving can be a cold refetch (file read + decompress) once the
  // ResidentPayloadPool evicted the payload. Checked again after the resolve, since an insert can change the pick.
  const auto already_delivered = [this](SequentialUID uid) { return uid == last_entry_uid_; };
  const auto entry_id = store_->latestEntryIdAt(topic_, ts_ns);
  if (!entry_id.has_value()) {
    if (last_entry_stamp_ && ts_ns < *last_entry_stamp_) {
      // A seek before the shown entry, with nothing there: clear it. At or after it, the entry was evicted under
      // the cursor and stays.
      last_entry_stamp_.reset();
      last_entry_uid_ = {};
      return DecodedFrame{};
    }
    return std::nullopt;
  }
  if (already_delivered(entry_id->uid)) {
    return std::nullopt;  // already delivered
  }
  auto entry = store_->latestAt(topic_, ts_ns);
  if (!entry.has_value() || entry->payload.anchor == nullptr || entry->payload.bytes.empty()) {
    return std::nullopt;
  }
  if (already_delivered(entry->sequential_uid)) {
    return std::nullopt;
  }
  last_entry_uid_ = entry->sequential_uid;
  last_entry_stamp_ = entry->timestamp;

  auto resolved =
      resolveImage(parser_, parser_mutex_, canonical_, entry->timestamp, entry->payload, entry->payload_stamp_shift);
  if (!resolved.has_value()) {
    fprintf(
        stderr, "[DepthPipelineSource] resolve failed at ts=%lld: %s\n", static_cast<long long>(ts_ns),
        resolved.error().message.c_str());
    return std::nullopt;
  }

  if (resolved->image.data.empty()) {
    DecodedFrame clear;  // an empty image (a derived topic's tombstone) clears what was shown
    clear.pts = resolved->pts;
    return clear;
  }
  std::shared_ptr<std::vector<uint8_t>> scratch;
  auto depth = toDepthImage(resolved->image, scratch);
  if (!depth.has_value()) {
    return std::nullopt;
  }
  auto frame = decodeDepthImage(*depth, resolved->pts);
  if (frame) {
    frame->stamp = resolved->stamp;
    frame->entry_ts = entry->timestamp;
  }
  return frame;
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

  // Invalid samples become 0, which the shader draws transparent.
  auto pixels = std::make_shared<std::vector<uint8_t>>(pixel_count * sizeof(float), 0);
  format->to_meters(depth.data.data(), reinterpret_cast<float*>(pixels->data()), pixel_count);

  DecodedFrame frame;
  frame.pixels = std::move(pixels);
  frame.width = static_cast<int>(depth.width);
  frame.height = static_cast<int>(depth.height);
  frame.format = PixelFormat::kDepthR32F;
  frame.pts = depth.timestamp_ns != 0 ? depth.timestamp_ns : pts;
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
