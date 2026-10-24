#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <memory>
#include <mutex>

#include "pj_base/builtin/image.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"  // sdk::PayloadView
#include "pj_scene2d_core/parser_object.h"  // ParserObjectError

namespace PJ {

class MessageParserPluginBase;

/// A resolved canonical image plus its effective presentation timestamp.
struct ResolvedImage {
  sdk::Image image;
  int64_t pts = 0;  ///< record.ts (parser) or the entry timestamp (canonical blob).
  /// `image.timestamp_ns` moved onto the dataset clock by the entry's `payload_stamp_shift` (saturating): the stamp the
  /// annotations made from the image carry (DecodedFrame::stamp).
  int64_t stamp = 0;
};

/// Resolve a store entry's bytes into a canonical sdk::Image — the single source
/// of truth shared by the image and depth pipeline sources.
///
/// When `parser` is non-null the bytes are a raw message decoded via the topic's
/// MessageParser (serialized through `parser_mutex`, which plugins require since
/// they are not thread-safe); otherwise, with `canonical` set, the bytes are a
/// serialized pj_image_v1 blob decoded with deserializeImage. `parser` and
/// `canonical` are mutually exclusive selectors.
///
/// The returned image aliases `payload`'s bytes/anchor, so it must be consumed
/// before the payload is released (both callers decode it synchronously).
/// `payload_stamp_shift` is the entry's (ResolvedObjectEntry), which a merge moved.
[[nodiscard]] Expected<ResolvedImage, ParserObjectError> resolveImage(
    MessageParserPluginBase* parser, const std::shared_ptr<std::mutex>& parser_mutex, bool canonical, int64_t entry_ts,
    const sdk::PayloadView& payload, int64_t payload_stamp_shift);

}  // namespace PJ
