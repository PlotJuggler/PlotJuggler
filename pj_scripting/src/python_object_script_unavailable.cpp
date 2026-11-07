// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// WASM: no embedded CPython. Every entry point fails cleanly, so a caller that skips `pythonAvailable()` gets an
// error instead of a link failure.
#include <cstdint>
#include <string>

#include "pj_scripting/python_object_script.h"

namespace PJ::scripting {

namespace {
constexpr const char* kUnavailableMessage = "python is not available in this build";
}  // namespace

bool pythonAvailable() {
  return false;
}

void prestartPythonHelper() {}

void setPythonValidateWaitForTest(std::uint64_t /*millis*/) {}

Expected<sdk::ScriptCheck> checkOnDemandScriptPython(std::string_view /*source*/) {
  return unexpected(std::string(kUnavailableMessage));
}

Expected<CompiledPythonScript> deferOnDemandScriptPython(std::string_view /*source*/) {
  return unexpected(std::string(kUnavailableMessage));
}

Expected<CompiledPythonScript> compileOnDemandScriptPython(std::string_view /*source*/) {
  return unexpected(std::string(kUnavailableMessage));
}

Expected<ScriptBundle, ScriptError> runOnDemandScriptPython(
    const CompiledPythonScript& /*script*/, Span<const NamedValue> /*inputs*/, std::string_view /*params_json*/,
    Span<const OutputSpec> /*outputs*/, const RunOptions& /*options*/) {
  return unexpected(ScriptError{kUnavailableMessage});
}

}  // namespace PJ::scripting
