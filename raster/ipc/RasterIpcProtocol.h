// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared IPC contract between PlotJuggler (MPL-2.0) and the external render
// helper (GPLv2). Pure C++, header-only, no Qt — so it compiles cleanly into
// both binaries and unit-tests standalone. This header carries NO engine code.

#include <cstdint>

namespace pj_raster {

constexpr uint32_t kShmMagic = 0x504A5246u;  // 'PJRF'
constexpr uint32_t kShmVersion = 1u;
constexpr uint32_t kBufferCount = 2u;    // double-buffer
constexpr uint32_t kBytesPerPixel = 4u;  // RGB32 stride; matches QImage::Format_RGB32

// Lives at offset 0 of the shared segment; pixels follow.
struct ShmHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t width;
  uint32_t height;
  uint32_t bytes_per_pixel;
  uint32_t buffer_count;
  uint32_t active_index;  // which buffer holds the most recently published frame
  uint32_t frame_seq;     // increments per published frame
};
static_assert(sizeof(uint32_t) == 4, "pj_raster wire layout assumes 32-bit uint32_t");
static_assert(sizeof(ShmHeader) == 32, "pj_raster ShmHeader must be 8 packed uint32_t");

inline uint32_t bufferBytes(uint32_t w, uint32_t h, uint32_t bpp) {
  return w * h * bpp;
}
inline uint32_t shmTotalSize(uint32_t w, uint32_t h, uint32_t bpp) {
  return static_cast<uint32_t>(sizeof(ShmHeader)) + kBufferCount * bufferBytes(w, h, bpp);
}
inline uint32_t bufferOffset(uint32_t index, uint32_t w, uint32_t h, uint32_t bpp) {
  return static_cast<uint32_t>(sizeof(ShmHeader)) + index * bufferBytes(w, h, bpp);
}

// Fixed 8-byte little-endian control message (sent over the local socket).
enum class MsgType : uint8_t { kNone = 0, kFrameReady = 1, kKeyDown = 2, kKeyUp = 3, kBye = 4 };
constexpr int kMessageBytes = 8;

struct Message {
  MsgType type;
  uint32_t arg;  // kFrameReady: frame_seq. kKeyDown/kKeyUp: engine key code.
};

inline void encodeMessage(const Message& m, uint8_t out[kMessageBytes]) {
  out[0] = static_cast<uint8_t>(m.type);
  out[1] = 0;
  out[2] = 0;
  out[3] = 0;
  out[4] = static_cast<uint8_t>(m.arg & 0xFFu);
  out[5] = static_cast<uint8_t>((m.arg >> 8) & 0xFFu);
  out[6] = static_cast<uint8_t>((m.arg >> 16) & 0xFFu);
  out[7] = static_cast<uint8_t>((m.arg >> 24) & 0xFFu);
}

inline Message decodeMessage(const uint8_t in[kMessageBytes]) {
  Message m;
  m.type = static_cast<MsgType>(in[0]);
  m.arg = static_cast<uint32_t>(in[4]) | (static_cast<uint32_t>(in[5]) << 8) | (static_cast<uint32_t>(in[6]) << 16) |
          (static_cast<uint32_t>(in[7]) << 24);
  return m;
}

}  // namespace pj_raster
