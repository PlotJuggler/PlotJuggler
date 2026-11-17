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

/// Checked BEFORE allocating from header-declared geometry: the bytes are untrusted, and a crafted header would
/// otherwise drive a multi-gigabyte allocation. 8K is about 33 MP.
inline constexpr int64_t kMaxImagePixels = 100'000'000;

[[nodiscard]] inline bool imageDimensionsWithinLimit(int width, int height) noexcept {
  return width > 0 && height > 0 && static_cast<int64_t>(width) * static_cast<int64_t>(height) <= kMaxImagePixels;
}

enum class ImageContainer : uint8_t {
  kUnknown,
  kJpeg,
  kPng,
};

/// PNG needs the full 8-byte signature: a shorter match can take a raw or Bayer buffer for a container.
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

/// Mono16 → RGB888 grayscale, stretching the non-zero samples' [min, max]; zero is invalid and stays black.
class Mono16ToGrayscale : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;
};

/// Passes display-ready frames through; Mono16 becomes grayscale.
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

/// kMono8 CFA mosaic → RGB888: each missing channel averages its samples in the 3x3 neighbourhood (edges clamped).
class BayerDecode : public CodecStage {
 public:
  explicit BayerDecode(BayerPattern pattern) : pattern_(pattern) {}

  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;

 private:
  BayerPattern pattern_;
};

/// Mono8 class IDs → RGB888, a distinct hue per ID. No production pipeline uses it.
class SegmentationPalette : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;
};

std::unique_ptr<CodecPipeline> makeJpegPipeline();

/// Unwraps a raw buffer a transport wrapped losslessly in a PNG/JPEG (e.g. Mosaico's `serialization_format=image`
/// stores a 16UC1 depth frame as an 8-bit grayscale PNG of width = stride): one byte per sample (gray RGB collapses
/// to its luma; Mono16 keeps two), so reading it at the image's encoding and geometry gives the original buffer.
/// nullopt when `data` is not a decodable container: the bytes are raw already. pj_scene3D's
/// DepthCloudLayer::toDepthView has a QImage-based twin (pj_scene3D cannot depend on this module); keep them in sync.
[[nodiscard]] std::optional<std::shared_ptr<std::vector<uint8_t>>> recoverContainerRawSamples(
    const uint8_t* data, size_t size);

}  // namespace PJ
