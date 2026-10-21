// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>
#include <variant>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_scripting/object_script.h"
#include "pj_scripting/python_object_script.h"

// An on-demand script in either language (docs/derived_objects/ARCHITECTURE.md §14): where a recipe's `language`
// picks its compiler and runner, so the runtime holds and runs a script without knowing its language.

namespace PJ::scripting {

/// A recipe's script compiled once per revision in its language; the Python one is an opaque handle.
using CompiledOnDemandScript = std::variant<CompiledScript, CompiledPythonScript>;

/// Compiles `source` as `language`, "luau" or "python"; any other language is refused by name. Where
/// `pythonAvailable()` is false, "python" fails with "python is not available in this build".
[[nodiscard]] Expected<CompiledOnDemandScript> compileOnDemand(std::string_view language, std::string_view source);

/// `runOnDemandScript` or `runOnDemandScriptPython`, by the script's language: same contract. A Python run takes the
/// GIL, so it belongs on the Python lane.
[[nodiscard]] Expected<ScriptBundle, ScriptError> runOnDemand(
    const CompiledOnDemandScript& script, Span<const NamedValue> inputs, std::string_view params_json,
    Span<const OutputSpec> outputs, const RunOptions& options);

}  // namespace PJ::scripting
