#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "pj_scene2d_core/codec_pipeline.h"

namespace PJ {

/// Upper bound on decoded image dimensions, enforced BEFORE allocating a pixel
/// buffer from header-declared geometry. Decoders consume untrusted bytes (files,
/// network); a crafted header can declare enormous dimensions that would drive a
/// multi-gigabyte value-initialized allocation (or std::bad_alloc). 100 MP
/// comfortably covers any real camera/video frame (8K is ~33 MP) while bounding
/// the worst case.
inline constexpr int64_t kMaxImagePixels = 100'000'000;

/// True when (width, height) is a positive image size whose pixel count is within
/// kMaxImagePixels. Computed in 64-bit so the product cannot overflow.
[[nodiscard]] inline bool imageDimensionsWithinLimit(int width, int height) noexcept {
  return width > 0 && height > 0 && static_cast<int64_t>(width) * static_cast<int64_t>(height) <= kMaxImagePixels;
}

enum class ImageContainer : uint8_t {
  kUnknown,
  kJpeg,
  kPng,
};

/// Sniff the self-describing image container carried by a byte payload. PNG uses
/// the full 8-byte signature, not only "\x89PNG": a shorter match can confuse
/// raw/Bayer buffers with compressed containers and route them to the wrong path.
[[nodiscard]] inline ImageContainer sniffImageContainer(const uint8_t* data, size_t size) noexcept {
  if (data == nullptr) {
    return ImageContainer::kUnknown;
  }
  if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
    return ImageContainer::kJpeg;
  }
  if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 && data[4] == 0x0D &&
      data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A) {
    return ImageContainer::kPng;
  }
  return ImageContainer::kUnknown;
}

/// What a JPEG's first SOF segment or a PNG's IHDR declares; 0 x 0 when it declares none.
struct ImageHeader {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t components = 0;  ///< a JPEG's; 0 for a PNG
  /// A JPEG libjpeg buffers whole before it outputs a row: progressive, or a first scan without every component (or
  /// no scan found).
  bool multi_scan = false;
};

/// Reads the header of `data` without decoding it, for a caller that bounds what a codec allocates before running
/// it. `container` is what `sniffImageContainer` found; any other (kUnknown) yields an empty header.
[[nodiscard]] ImageHeader peekImageHeader(const uint8_t* data, size_t size, ImageContainer container) noexcept;

/// A conservative, not exact, bound on what decoding `header` allocates beside its output and the input: row
/// pointers, row buffers, the library's state, and for a multi-scan JPEG the whole-image coefficient buffer.
[[nodiscard]] size_t codecWorkspaceBound(const ImageHeader& header, ImageContainer container) noexcept;

/// JPEG → RGB888 via turbojpeg.
class JpegCodec : public CodecStage {
 public:
  JpegCodec();
  ~JpegCodec() override;
  JpegCodec(const JpegCodec&) = delete;
  JpegCodec& operator=(const JpegCodec&) = delete;
  JpegCodec(JpegCodec&&) = delete;
  JpegCodec& operator=(JpegCodec&&) = delete;

  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;

 private:
  void* tj_ = nullptr;
};

/// PNG → RGB888 (8-bit) or RGBA8888 (with alpha) or Mono16 (16-bit grayscale).
/// Preserves Mono16 for downstream stages (e.g. Mono16ToGrayscale).
class PngCodec : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;
};

/// Mono16 → RGB888 grayscale. Normalizes non-zero samples to [min, max].
/// Zero samples are treated as invalid and remain black for all mono16 images.
/// Input must be kMono16 with pixels->size() >= width*height*2.
class Mono16ToGrayscale : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;
};

/// Pass-through for display-ready frames, Mono16 → RGB888 grayscale when needed.
class NormalizeMono16 : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;

 private:
  Mono16ToGrayscale mono16_to_grayscale_;
};

/// JPEG/PNG byte decoder cascade. The output may still be Mono16.
class ImageDecodeCascade : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;

 private:
  JpegCodec jpeg_;
  PngCodec png_;
};

/// JPEG/PNG auto-dispatch followed by Mono16 normalization when needed.
class AutoImageCodec : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;

 private:
  ImageDecodeCascade decode_;
  NormalizeMono16 normalize_;
};

/// Bayer color-filter-array pattern, named by the top-left 2x2 tile.
enum class BayerPattern : uint8_t {
  kRGGB,
  kGRBG,
  kGBRG,
  kBGGR,
};

/// Bayer mosaic → RGB888 demosaic. Input must be a kMono8 frame whose pixels
/// hold the raw CFA mosaic (one sample per pixel). The constructor selects the
/// CFA pattern. Missing channels are filled by averaging the same-channel
/// samples in the 3x3 neighbourhood (clamped at edges).
class BayerDecode : public CodecStage {
 public:
  explicit BayerDecode(BayerPattern pattern) : pattern_(pattern) {}

  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;

 private:
  BayerPattern pattern_;
};

/// Mono8 class IDs → RGB888 false-color. Each class ID (0-255) maps to
/// a distinct hue. Input must be kMono8 with pixels->size() >= width*height.
/// Designed for the planned segmentation layer; not yet wired into any pipeline
/// (no production constructor call).
class SegmentationPalette : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;
};

// --- Pipeline builders ---

std::unique_ptr<CodecPipeline> makeJpegPipeline();  ///< JpegCodec only (raw JPEG input)

/// Decode a PNG/JPEG-wrapped raw frame back to its flat sample bytes — the inverse
/// of a transport that losslessly wraps a raw buffer in an image container (e.g.
/// Mosaico's `serialization_format=image` wraps a 16UC1 depth frame as an 8-bit
/// grayscale PNG of width=stride). Returns the decoded samples as ONE byte per
/// sample (gray-expanded RGB is collapsed back to its luma byte; Mono16 keeps its
/// two bytes per sample), so reinterpreting the result at the frame's logical
/// encoding/geometry recovers the original buffer. Returns nullopt when `data` is
/// not a decodable PNG/JPEG container — callers then treat the bytes as already
/// raw. Use this before reading a raw-encoded image's bytes as raw, so a
/// container-wrapped raw frame isn't mis-read as raw (the depth all-black bug).
/// NOTE: pj_scene3D's DepthCloudLayer::toDepthView has a twin of this (QImage-based,
/// since pj_scene3D cannot depend on pj_scene2d_core); keep the two in sync.
[[nodiscard]] std::optional<std::shared_ptr<std::vector<uint8_t>>> recoverContainerRawSamples(
    const uint8_t* data, size_t size);

}  // namespace PJ
