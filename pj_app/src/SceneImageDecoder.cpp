// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "SceneImageDecoder.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_scene2d_core/codecs.h"

namespace PJ {
namespace {

using scripting::DecodedPixels;
using scripting::native::MemoryBudget;
using scripting::native::MemoryReservation;

/// How the samples of a frame are laid out.
struct Layout {
  uint32_t channels = 0;
  uint32_t sample_bytes = 1;
  bool big_endian = false;
  bool bgr = false;  ///< blue first: swapped into RGB order
};

/// `imageDimensionsWithinLimit` for unsigned sizes, clamped so a huge one cannot wrap negative.
bool dimensionsWithinLimit(uint32_t width, uint32_t height) {
  return imageDimensionsWithinLimit(
      static_cast<int>(std::min<uint32_t>(width, INT32_MAX)), static_cast<int>(std::min<uint32_t>(height, INT32_MAX)));
}

template <class T>
T byteSwapped(T value) {
  if constexpr (sizeof(T) == 2) {
    return static_cast<T>((value >> 8) | (value << 8));
  } else {
    return ((value & 0xffu) << 24) | ((value & 0xff00u) << 8) | ((value >> 8) & 0xff00u) | (value >> 24);
  }
}

/// One sample stored as `Stored` (uint8_t, uint16_t, or uint32_t holding a float), read unaligned.
template <class Stored, bool kFloat, bool kSwap>
float readSample(const uint8_t* bytes) {
  Stored stored;
  std::memcpy(&stored, bytes, sizeof(Stored));
  if constexpr (kSwap) {
    stored = byteSwapped(stored);
  }
  if constexpr (kFloat) {
    return std::bit_cast<float>(stored);
  } else {
    return static_cast<float>(stored);
  }
}

/// Converts `height` rows of `stride` bytes into packed floats; every layout decision is a template parameter.
template <class Stored, bool kFloat, bool kSwap, uint32_t kChannels, bool kBgr>
void convertRows(const uint8_t* data, std::size_t stride, uint32_t width, uint32_t height, float* out) {
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* pixel = data + y * stride;
    for (uint32_t x = 0; x < width; ++x, pixel += kChannels * sizeof(Stored), out += kChannels) {
      for (uint32_t c = 0; c < kChannels; ++c) {
        const uint32_t source = kBgr && c < 3 ? 2 - c : c;
        out[c] = readSample<Stored, kFloat, kSwap>(pixel + source * sizeof(Stored));
      }
    }
  }
}

using Converter = void (*)(const uint8_t*, std::size_t, uint32_t, uint32_t, float*);

/// The kernel for `layout`; null for a layout no encoding produces.
Converter converterFor(const Layout& layout) {
  const bool swap = layout.big_endian != (std::endian::native == std::endian::big);
  if (layout.sample_bytes == 1) {
    switch (layout.channels) {
      case 1:
        return &convertRows<uint8_t, false, false, 1, false>;
      case 3:
        return layout.bgr ? &convertRows<uint8_t, false, false, 3, true>
                          : &convertRows<uint8_t, false, false, 3, false>;
      case 4:
        return layout.bgr ? &convertRows<uint8_t, false, false, 4, true>
                          : &convertRows<uint8_t, false, false, 4, false>;
      default:
        return nullptr;
    }
  }
  if (layout.channels != 1) {
    return nullptr;
  }
  if (layout.sample_bytes == 2) {
    return swap ? &convertRows<uint16_t, false, true, 1, false> : &convertRows<uint16_t, false, false, 1, false>;
  }
  return swap ? &convertRows<uint32_t, true, true, 1, false> : &convertRows<uint32_t, true, false, 1, false>;
}

/// The layout of a raw encoding; nullopt for one this decoder does not know.
std::optional<Layout> rawLayout(const sdk::Image& image) {
  const std::string& encoding = image.encoding;
  if (encoding == "rgb8" || encoding == "bgr8") {
    return Layout{.channels = 3, .bgr = encoding == "bgr8"};
  }
  if (encoding == "rgba8" || encoding == "bgra8") {
    return Layout{.channels = 4, .bgr = encoding == "bgra8"};
  }
  if (encoding == "mono8" || encoding == "8UC1") {
    return Layout{.channels = 1};
  }
  if (encoding == "mono16" || encoding == "16UC1") {
    return Layout{.channels = 1, .sample_bytes = 2, .big_endian = image.is_bigendian};
  }
  if (encoding == "32FC1") {
    return Layout{.channels = 1, .sample_bytes = 4, .big_endian = image.is_bigendian};
  }
  return std::nullopt;
}

/// The layout of a codec's output frame, in native byte order.
std::optional<Layout> frameLayout(PixelFormat format) {
  const bool big = std::endian::native == std::endian::big;
  switch (format) {
    case PixelFormat::kRGB888:
      return Layout{.channels = 3};
    case PixelFormat::kRGBA8888:
      return Layout{.channels = 4};
    case PixelFormat::kMono8:
      return Layout{.channels = 1};
    case PixelFormat::kMono16:
      return Layout{.channels = 1, .sample_bytes = 2, .big_endian = big};
    default:
      return std::nullopt;
  }
}

/// Converts `data` (`stride` bytes per row) to charged float samples; the charge is taken before the allocation.
Expected<DecodedPixels> toSamples(
    const uint8_t* data, std::size_t size, std::size_t stride, uint32_t width, uint32_t height, const Layout& layout,
    const std::shared_ptr<MemoryBudget>& budget) {
  const Converter convert = converterFor(layout);
  if (convert == nullptr) {
    return unexpected("unsupported pixel layout");
  }
  const std::size_t packed_row = std::size_t{width} * layout.channels * layout.sample_bytes;
  if (stride == 0) {
    stride = packed_row;
  }
  if (data == nullptr || stride < packed_row || stride > std::numeric_limits<std::size_t>::max() / height ||
      stride * height > size) {
    return unexpected("image buffer does not match its size and row stride");
  }
  const std::size_t count = std::size_t{width} * height * layout.channels;
  auto charge = std::make_shared<MemoryReservation>(budget);
  if (Status grown = charge->grow(count * sizeof(float)); !grown) {
    return unexpected(grown.error());
  }
  auto samples = std::make_shared<std::vector<float>>(count);
  convert(data, stride, width, height, samples->data());
  DecodedPixels out;
  out.width = width;
  out.height = height;
  out.channels = layout.channels;
  out.samples = std::move(samples);
  out.charge = std::move(charge);
  return out;
}

}  // namespace

Expected<scripting::DecodedPixels> SceneImageDecoder::decode(
    const sdk::Image& image, const std::shared_ptr<MemoryBudget>& budget) const {
  const uint8_t* data = image.data.data();
  const std::size_t size = image.data.size();
  if (image.encoding != "jpeg" && image.encoding != "png") {
    const std::optional<Layout> layout = rawLayout(image);
    if (!layout) {
      return unexpected("unsupported image encoding: " + image.encoding);
    }
    if (!dimensionsWithinLimit(image.width, image.height)) {
      return unexpected("invalid image dimensions");
    }
    return toSamples(data, size, image.row_step, image.width, image.height, *layout, budget);
  }
  if (data == nullptr || size == 0) {
    return unexpected("empty compressed image");
  }
  // The header that sizes the charge and the codec that decodes must both be the container's own: a payload whose
  // bytes are not what its encoding says is refused before either reads it.
  const ImageContainer container = image.encoding == "png" ? ImageContainer::kPng : ImageContainer::kJpeg;
  if (sniffImageContainer(data, size) != container) {
    return unexpected("the payload is not a " + image.encoding + " container");
  }
  const ImageHeader header = peekImageHeader(data, size, container);
  if (!dimensionsWithinLimit(header.width, header.height) ||
      (container == ImageContainer::kJpeg && (header.components == 0 || header.components > 4))) {
    return unexpected("invalid or missing " + image.encoding + " header");
  }
  // Charge what the codec allocates before it runs, for as long as the call holds it: its output (at most 4 bytes per
  // pixel), the copy of the input it reads, and its workspace.
  const std::size_t output_bound = std::size_t{header.width} * header.height * 4;
  MemoryReservation temporary(budget);
  if (Status grown = temporary.grow(output_bound + size + codecWorkspaceBound(header, container)); !grown) {
    return unexpected(grown.error());
  }
  DecodedFrame input;
  input.pixels = std::make_shared<std::vector<uint8_t>>(data, data + size);
  Expected<DecodedFrame> frame =
      container == ImageContainer::kPng ? PngCodec{}.decode(input) : JpegCodec{}.decode(input);
  if (!frame) {
    return unexpected(frame.error());
  }
  if (!frame->isValid() || uint32_t(frame->width) != header.width || uint32_t(frame->height) != header.height) {
    return unexpected("the decoded " + image.encoding + " frame does not match its header");
  }
  const std::optional<Layout> layout = frameLayout(frame->format);
  if (!layout) {
    return unexpected("unsupported decoded pixel format");
  }
  return toSamples(frame->pixels->data(), frame->pixels->size(), 0, header.width, header.height, *layout, budget);
}

}  // namespace PJ
