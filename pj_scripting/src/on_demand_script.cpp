// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scripting/on_demand_script.h"

#include <string>
#include <utility>

namespace PJ::scripting {
namespace {

template <class Compiled>
Expected<CompiledOnDemandScript> held(Expected<Compiled> compiled) {
  if (!compiled) {
    return unexpected(std::move(compiled.error()));
  }
  return CompiledOnDemandScript{std::move(*compiled)};
}

}  // namespace

Expected<CompiledOnDemandScript> compileOnDemand(std::string_view language, std::string_view source) {
  if (language == "luau") {
    return held(compileOnDemandScript(source));
  }
  if (language == "python") {
    return held(compileOnDemandScriptPython(source));
  }
  return unexpected("unsupported language '" + std::string(language) + "' (only 'luau' or 'python')");
}

Expected<ScriptBundle, ScriptError> runOnDemand(
    const CompiledOnDemandScript& script, Span<const NamedValue> inputs, std::string_view params_json,
    Span<const OutputSpec> outputs, const RunOptions& options) {
  if (const auto* python = std::get_if<CompiledPythonScript>(&script)) {
    return runOnDemandScriptPython(*python, inputs, params_json, outputs, options);
  }
  return runOnDemandScript(std::get<CompiledScript>(script), inputs, params_json, outputs, options);
}

}  // namespace PJ::scripting
