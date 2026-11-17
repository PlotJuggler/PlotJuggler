#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>

#include "pj_base/buffer_anchor.hpp"
#include "pj_base/data_source_protocol.h"

namespace PJ {
namespace detail {

/// Wraps a plugin's C-ABI payload anchor as an sdk::BufferAnchor whose deleter calls `anchor.release(anchor.ctx)` while
/// HOLDING `library_keepalive`, the producing plugin's DSO token.
///
/// `anchor.release` is a function pointer compiled into the plugin's `.so`. A PayloadView / ResolvedObjectEntry
/// carrying this anchor can be cached past the point where the extension catalog dlcloses the plugin (e.g. a scene
/// widget destroyed after the session). Capturing the DSO token in the deleter keeps the `.so` mapped until this anchor
/// and every copy are destroyed, so `release` is never invoked against unmapped code.
///
/// A null `anchor.release` yields an empty anchor (the buffer dies with the C-ABI call; the caller must copy). A null
/// `library_keepalive` is allowed and gives no DSO protection, for callers with no token to supply.
[[nodiscard]] sdk::BufferAnchor wrapPayloadAnchor(
    const PJ_payload_anchor_t& anchor, std::shared_ptr<void> library_keepalive);

}  // namespace detail
}  // namespace PJ
