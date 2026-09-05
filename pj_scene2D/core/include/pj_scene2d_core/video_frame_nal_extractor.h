#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>
#include <mutex>

#include "pj_scene2d_core/streaming_video_decoder.h"

namespace PJ {

class MessageParserPluginBase;

/// Build a NAL extractor that unwraps each ObjectStore entry's canonical
/// PJ.VideoFrame / Foxglove CompressedVideo message — it calls
/// `parser->parseObject` (serialized by `parser_mutex`, since MessageParser
/// plugins are not thread-safe), downcasts the result to sdk::VideoFrame, and
/// returns the contained Annex-B `data` span (zero-copy; the span aliases the
/// entry buffer, kept alive across extract+decode).
///
/// Each call returns a FRESH extractor with its own per-call keepalive slot, so
/// give each decoder that runs concurrently (the playback decoder AND the
/// thumbnail builder's decoder) ITS OWN extractor — the slot is single-threaded.
/// A null `parser` yields an extractor that always fails. `parser_mutex` is taken
/// by value so the factory can be called more than once for the same parser.
[[nodiscard]] StreamingVideoDecoder::NalExtractor makeVideoFrameNalExtractor(
    MessageParserPluginBase* parser, std::shared_ptr<std::mutex> parser_mutex);

}  // namespace PJ
