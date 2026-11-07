// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_core/image_resolve.h"

#include <string>
#include <utility>

#include "pj_base/builtin/image_codec.hpp"  // deserializeImage
#include "pj_datastore/saturating_time.hpp"

namespace PJ {

Expected<ResolvedImage, ParserObjectError> resolveImage(
    MessageParserPluginBase* parser, const std::shared_ptr<std::mutex>& parser_mutex, bool canonical, int64_t entry_ts,
    const sdk::PayloadView& payload, int64_t payload_stamp_shift) {
  if (parser != nullptr) {
    auto parsed = parseObjectAs<sdk::Image>(
        *parser, parser_mutex, entry_ts, payload, sdk::BuiltinObjectType::kImage, "sdk::Image");
    if (!parsed.has_value()) {
      return unexpected(parsed.error());
    }
    // Copy the image out of the parsed record: its BufferAnchor travels with the
    // copy, so the bytes stay alive for as long as the caller holds the result.
    return ResolvedImage{
        .image = *parsed->value,
        .pts = parsed->record.ts.value_or(entry_ts),
        .stamp = saturatingAdd(parsed->value->timestamp_ns, payload_stamp_shift)};
  }

  if (canonical) {
    auto img = deserializeImage(payload.bytes.data(), payload.bytes.size());
    if (!img.has_value()) {
      return unexpected(
          ParserObjectError{
              ParserObjectErrorKind::kParseFailed, std::move(img.error()), sdk::BuiltinObjectType::kImage});
    }
    const int64_t stamp = saturatingAdd(img->timestamp_ns, payload_stamp_shift);
    return ResolvedImage{.image = std::move(*img), .pts = entry_ts, .stamp = stamp};
  }

  return unexpected(
      ParserObjectError{
          ParserObjectErrorKind::kParseFailed, "resolveImage: neither a parser nor the canonical codec was selected",
          sdk::BuiltinObjectType::kImage});
}

}  // namespace PJ
