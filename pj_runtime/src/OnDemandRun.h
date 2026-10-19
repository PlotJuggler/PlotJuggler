// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/InputStager.h"
#include "pj_runtime/NodeScheduler.h"
#include "pj_scripting/object_script.h"

namespace PJ {

/// What one on-demand run left: its result, and the libraries the objects in it may need.
struct StagedRun {
  /// Declared first, so they outlive `result`, which may pass an input object through.
  std::vector<InputStager::Keepalive> keepalives;
  Expected<scripting::ScriptBundle, scripting::ScriptError> result{unexpected(scripting::ScriptError{"not run"})};
  bool decode_failed = false;  ///< `result` is an input's fetch or decode error: the script did not run
};

/// Any thread: decodes `staged` and runs `script` once over it, cancellable through `token` inside the interpreter.
/// Touches only `store` (thread-safe) and its arguments: the run body `NodeScheduler` and `GridRunner` share.
inline StagedRun runStaged(
    const ObjectStore& store, const InputStager::Staged& staged, const NodeScheduler::RunFunction& run,
    const scripting::CompiledScript& script, std::string_view params_json, Span<const scripting::OutputSpec> outputs,
    scripting::RunOptions options, const EnginePool::CancelToken& token) {
  StagedRun ran;
  Expected<InputStager::Decoded> decoded = InputStager::decode(store, staged);
  if (!decoded) {
    ran.decode_failed = true;
    ran.result = unexpected(scripting::ScriptError{std::move(decoded.error())});
    return ran;
  }
  ran.keepalives = std::move(decoded->keepalives);
  options.cancelled = [token] { return token.cancelled(); };
  ran.result = run(script, decoded->values, params_json, outputs, options);
  return ran;
}

}  // namespace PJ
