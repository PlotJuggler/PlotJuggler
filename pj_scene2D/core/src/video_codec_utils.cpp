// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/video_codec_utils.h"

#include "video_codec_utils_internal.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

#include "pj_scene2d_core/h264_utils.h"

namespace PJ {

// Offset of the byte after the next Annex-B start code at/after `offset`, or
// `size` if none. Start codes are 0x000001 (3-byte) or 0x00000001 (4-byte).
size_t nextNalHeader(const uint8_t* data, size_t size, size_t offset) {
  while (offset + 2 < size) {
    if (data[offset] == 0x00 && data[offset + 1] == 0x00) {
      if (data[offset + 2] == 0x01) {
        return offset + 3;
      }
      if (offset + 3 < size && data[offset + 2] == 0x00 && data[offset + 3] == 0x01) {
        return offset + 4;
      }
    }
    ++offset;
  }
  return size;
}

namespace {

std::string toLower(std::string_view s) {
  std::string out(s);
  std::transform(
      out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// HEVC (H.265) Annex-B keyframe oracle. The NAL header is 2 bytes; the type is
// bits 1..6 of the first byte: (b >> 1) & 0x3F. IRAP pictures (random-access
// points) are types 16..21 (BLA_W_LP..CRA_NUT, including IDR 19/20).
bool isHevcKeyframe(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 2) {
    return false;
  }
  for (size_t hdr = nextNalHeader(data, size, 0); hdr < size; hdr = nextNalHeader(data, size, hdr)) {
    const int nal_type = (data[hdr] >> 1) & 0x3F;
    if (nal_type >= 16 && nal_type <= 21) {
      return true;
    }
  }
  return false;
}

// AV1 keyframe oracle. The wire form is a Low-Overhead-Bitstream-Format temporal
// unit (OBU stream, no Annex-B start codes). A random-access point carries a
// Sequence-Header OBU (type 1), so its presence marks a decodable GOP start.
// OBU header byte: obu_type = (b >> 3) & 0x0F; obu_extension_flag = (b >> 2) & 1;
// obu_has_size_field = (b >> 1) & 1. A LEB128 obu_size follows the (optional
// 1-byte extension) header when has_size_field is set.
bool isAv1Keyframe(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) {
    return false;
  }
  constexpr int kObuSequenceHeader = 1;
  size_t pos = 0;
  while (pos < size) {
    const uint8_t header = data[pos];
    const int obu_type = (header >> 3) & 0x0F;
    if (obu_type == kObuSequenceHeader) {
      return true;
    }
    const bool extension_flag = ((header >> 2) & 0x01) != 0;
    const bool has_size_field = ((header >> 1) & 0x01) != 0;
    size_t cursor = pos + 1 + (extension_flag ? 1 : 0);
    if (cursor > size) {
      break;
    }
    size_t payload_size = 0;
    if (has_size_field) {
      // LEB128 obu_size (AV1 spec 4.10.5: at most 8 bytes).
      uint64_t value = 0;
      bool terminated = false;
      for (int byte_index = 0; byte_index < 8 && cursor < size; ++byte_index) {
        const uint8_t byte = data[cursor++];
        value |= static_cast<uint64_t>(byte & 0x7F) << (byte_index * 7);
        if ((byte & 0x80) == 0) {
          terminated = true;
          break;
        }
      }
      if (!terminated) {
        break;
      }
      payload_size = static_cast<size_t>(value);
    } else {
      // No size field: the OBU runs to the end of the temporal unit.
      payload_size = size - cursor;
    }
    const size_t next = cursor + payload_size;
    if (next <= pos) {  // malformed / non-progressing — stop scanning.
      break;
    }
    pos = next;
  }
  return false;
}

}  // namespace

int videoCodecIdFromFormat(std::string_view format) {
  const std::string name = toLower(format);
  // Only codecs with a keyframe oracle in isVideoKeyframe: without one every seek fails.
  const AVCodec* decoder = nullptr;
  if (name == "h264") {
    decoder = avcodec_find_decoder_by_name("h264");
  } else if (name == "h265" || name == "hevc") {
    // Canonical Foxglove name is "h265"; FFmpeg's decoder is named "hevc".
    decoder = avcodec_find_decoder_by_name("hevc");
  } else if (name == "av1") {
    // Prefer the software AV1 decoder; the native "av1" decoder can be a
    // HW-only stub that fails avcodec_open2.
    decoder = avcodec_find_decoder_by_name("libdav1d");
    if (decoder == nullptr) {
      decoder = avcodec_find_decoder_by_name("av1");
    }
  }
  return decoder != nullptr ? static_cast<int>(decoder->id) : static_cast<int>(AV_CODEC_ID_NONE);
}

bool isVideoKeyframe(int codec_id, const uint8_t* data, size_t size) {
  switch (codec_id) {
    case AV_CODEC_ID_H264:
      return isH264Keyframe(data, size);
    case AV_CODEC_ID_HEVC:
      return isHevcKeyframe(data, size);
    case AV_CODEC_ID_AV1:
      return isAv1Keyframe(data, size);
    default:
      return false;
  }
}

AVCodecParameters* makeVideoCodecParams(int codec_id) {
  if (codec_id == AV_CODEC_ID_NONE) {
    return nullptr;
  }
  AVCodecParameters* params = avcodec_parameters_alloc();
  if (params == nullptr) {
    return nullptr;
  }
  params->codec_type = AVMEDIA_TYPE_VIDEO;
  params->codec_id = static_cast<AVCodecID>(codec_id);
  return params;
}

bool primeKeyframeParamSets(AVCodecParameters* params, const uint8_t* data, size_t size) {
  if (params == nullptr || data == nullptr || size < 4) {
    return false;
  }
  // Only Annex-B codecs carry extractable parameter-set NALs; AV1 is an OBU
  // stream whose decoder handles the in-band sequence header reliably.
  const bool is_h264 = params->codec_id == AV_CODEC_ID_H264;
  const bool is_hevc = params->codec_id == AV_CODEC_ID_HEVC;
  if (!is_h264 && !is_hevc) {
    return false;
  }

  std::vector<uint8_t> extradata;
  std::set<int> seen_types;
  size_t hdr = nextNalHeader(data, size, 0);
  while (hdr < size) {
    const size_t next_hdr = nextNalHeader(data, size, hdr);
    // The NAL payload runs to the start of the next start code (3 or 4 bytes
    // before the byte-after-start-code that nextNalHeader returns).
    size_t end = size;
    if (next_hdr < size) {
      end = next_hdr - 3;
      if (end > 0 && data[end - 1] == 0x00) {
        --end;  // 4-byte start code
      }
    }
    const int nal_type = is_h264 ? (data[hdr] & 0x1F) : ((data[hdr] >> 1) & 0x3F);
    const bool is_param_set = is_h264 ? (nal_type == 7 || nal_type == 8)     // SPS / PPS
                                      : (nal_type >= 32 && nal_type <= 34);  // VPS / SPS / PPS
    // Keep only the FIRST occurrence of each parameter-set type: screen-recorder
    // keyframe AUs repeat SPS/PPS (extradata copy + in-band copy), and duplicated
    // sets in extradata reproduce the very first-B-frame drop this priming exists
    // to prevent (a demuxer's avcC carries exactly one of each).
    if (is_param_set && seen_types.count(nal_type) > 0) {
      hdr = next_hdr;
      continue;
    }
    if (is_param_set && end > hdr) {
      seen_types.insert(nal_type);
      const uint8_t k_start_code[4] = {0x00, 0x00, 0x00, 0x01};
      extradata.insert(extradata.end(), k_start_code, k_start_code + 4);
      extradata.insert(extradata.end(), data + hdr, data + end);
    }
    hdr = next_hdr;
  }
  if (extradata.empty()) {
    return false;
  }

  auto* buf = static_cast<uint8_t*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (buf == nullptr) {
    return false;
  }
  std::copy(extradata.begin(), extradata.end(), buf);
  av_freep(&params->extradata);
  params->extradata = buf;
  params->extradata_size = static_cast<int>(extradata.size());
  return true;
}

}  // namespace PJ
