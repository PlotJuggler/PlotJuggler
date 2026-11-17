#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

struct AVCodecParameters;

namespace PJ {

/// sdk::VideoFrame::format ("h264", "h265"/"hevc", "av1") to an FFmpeg AVCodecID, as int to keep this header
/// FFmpeg-free. Only codecs with a keyframe oracle: anything else, VP9 included (decodable but not seekable), is
/// AV_CODEC_ID_NONE (0), which the decoder reports as unsupported.
int videoCodecIdFromFormat(std::string_view format);

/// One frame's bytes are a random-access point: H.264 IDR (NAL type 5), HEVC IRAP (NAL types 16..21), AV1 with a
/// Sequence Header OBU. False for any other codec.
bool isVideoKeyframe(int codec_id, const uint8_t* data, size_t size);

/// No extradata: prime it with primeKeyframeParamSets(). nullptr for AV_CODEC_ID_NONE or on allocation failure; free
/// with avcodec_parameters_free().
AVCodecParameters* makeVideoCodecParams(int codec_id);

/// Copies an Annex-B keyframe's parameter sets (H.264 SPS/PPS, HEVC VPS/SPS/PPS) into `params->extradata`, as a
/// demuxer would. Opening without them can drop the stream's first B-frames (docs/TECHNICAL_NOTES.md "B-frames").
/// True when a set was installed; no-op for AV1 and for keyframes without parameter sets.
bool primeKeyframeParamSets(AVCodecParameters* params, const uint8_t* data, size_t size);

}  // namespace PJ
