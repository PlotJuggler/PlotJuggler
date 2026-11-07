// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <mutex>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"

namespace PJ {

// Decode one object through a topic's MessageParser, holding the binding's
// per-topic mutex for the parseObject call.
//
// MessageParser plugins keep stateful scratch (fastcdr et al.) and aren't
// thread-safe; when a topic's parser is shared across consumers (SessionManager
// hands back a singleton) the mutex serialises parseObject. It is held for the
// decode only — the returned object owns its bytes, so callers use the result
// without the lock. Mirrors ImagePipelineSource's shared-parser_mutex discipline.
//
// Always fetch the binding from SessionManager per use — a file reload
// re-registers the topic's parser slot, so a parser pointer cached across calls
// dangles. The binding's keepalive holds the parser instance (and its plugin
// DSO) alive for the duration of this call. Precondition: `binding` is valid
// (binding.parser != nullptr); callers must early-out on `!binding` before
// reaching here. `binding.mutex` may be null only on a path where the caller
// guarantees exclusive single-threaded access to that parser instance; the
// decode then runs unlocked.
inline auto parseLocked(
    const PJ::SessionManager::ParserBinding& binding, PJ::Timestamp ts, const PJ::sdk::PayloadView& payload) {
  if (binding.mutex) {
    std::lock_guard<std::mutex> lock(*binding.mutex);
    return binding.parser->parseObject(ts, payload);
  }
  return binding.parser->parseObject(ts, payload);
}

}  // namespace PJ
