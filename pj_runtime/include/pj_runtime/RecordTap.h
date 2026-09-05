// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string_view>

#include "pj_base/span.hpp"
#include "pj_runtime/RecordingTypes.h"

namespace PJ {

/// The decoding facts of the binding a recorded message belongs to. Non-owning:
/// every view points into host state and is valid only for the call carrying it.
struct RecordedBindingView {
  std::string_view topic;
  std::string_view encoding;  ///< parser encoding, e.g. "cdr", "protobuf", "json"
  std::string_view type_name;
  std::string_view schema_bytes;  ///< may be empty
};

/// Observer of one source's delegated ingest, invoked INLINE on that source's
/// push thread by DataSourceRuntimeHost. It must not parse, must not touch
/// datastore state, and must not wait on anything: an implementation takes a
/// copy or drops the message. Messages of one source arrive in order; different
/// sources call from different threads concurrently.
class RecordTap {
 public:
  virtual ~RecordTap() = default;

  /// Once per pushMessage, BEFORE the parser runs. `binding` and `bytes` are
  /// valid only for the call. A tap that has not seen `binding_id` before
  /// creates whatever it needs from `binding`, which is what lets a recording
  /// started mid-stream capture topics bound long before it. Returning
  /// kStopRecording makes the host drop the tap.
  virtual TapVerdict onMessage(
      uint32_t binding_id, const RecordedBindingView& binding, int64_t log_time_ns, Span<const uint8_t> bytes) = 0;
};

}  // namespace PJ
