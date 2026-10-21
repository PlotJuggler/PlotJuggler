// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The browser acceptance probe of the on-demand Python lane (tests/wasm/python.spec.js). Included by
// wasm_acceptance_probes.inc only when PJ_WASM_WITH_PYTHON is on. Every result is a "PJ_WASM_PY_*" console line; the
// spec holds the criteria.

#include <sys/stat.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pj_scripting/object_script.h"
#include "pj_scripting/python_object_script.h"

/// wasm_python_stdlib_probe.cpp: the stdlib zip's gaps a strict script could reach, empty when there is none.
std::string wasmPythonStdlibProblems();

namespace {

double wasmPyMillisSince(std::chrono::steady_clock::time_point begin) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

PJ::Expected<PJ::scripting::ScriptBundle, PJ::scripting::ScriptError> wasmPyRun(
    const PJ::scripting::CompiledPythonScript& script, const std::vector<PJ::scripting::NamedValue>& inputs,
    const PJ::scripting::RunOptions& options = {}) {
  return PJ::scripting::runOnDemandScriptPython(
      script, PJ::Span<const PJ::scripting::NamedValue>(inputs.data(), inputs.size()), "{}", {}, options);
}

std::string wasmPyOutcome(const PJ::Expected<PJ::scripting::ScriptBundle, PJ::scripting::ScriptError>& result) {
  if (!result) {
    return "error=" + result.error().message;
  }
  for (const auto& output : result->outputs) {
    if (const auto* number = std::get_if<double>(&output.value)) {
      return "value=" + std::to_string(*number);
    }
  }
  return "no-number";
}

constexpr const char* kWasmPyValueScript =
    "import statistics, math\ndef evaluate(inputs, params):\n"
    "    return {'y': statistics.fmean([1, 2, 3]) + math.pi}\n";
constexpr const char* kWasmPyFourLineScript =
    "import math\ndef evaluate(inputs, params):\n    x = inputs[\"x\"]\n"
    "    return {\"y\": math.sqrt(x * x + 1.0)}\n";

/// pj_scripting's AllowedModulesImportTheirLazyDependenciesInAStrictRun: 7.0 when both lazy imports work in a run.
constexpr const char* kWasmPyLazyScript = R"PY(import statistics
def evaluate(inputs, params):
    top = statistics.Counter("aba").most_common(1)[0][1]
    return top + len(format(statistics.Decimal("1234"), "_"))
)PY";

/// The desktop bench's 43-line script (python_object_script_test.cpp kFortyThreeLineScript).
constexpr const char* kWasmPyFortyThreeLineScript = R"PY(import math
import statistics

LIMIT = 10.0
WEIGHTS = (0.2, 0.3, 0.5)

def clamp(value, low, high):
    return max(low, min(high, value))

def norm(x, y, z):
    return math.sqrt(x * x + y * y + z * z)

def weighted(values):
    total = 0.0
    for value, weight in zip(values, WEIGHTS):
        total += value * weight
    return total

class Window:
    def __init__(self, size):
        self.size = size
        self.values = []

    def push(self, value):
        self.values.append(value)
        if len(self.values) > self.size:
            self.values.pop(0)

    def mean(self):
        return statistics.fmean(self.values) if self.values else 0.0

def classify(value):
    if value > LIMIT:
        return "high"
    if value < -LIMIT:
        return "low"
    return "ok"

def evaluate(inputs, params):
    x = inputs["x"]
    window = Window(3)
    for value in (x, x + 1.0, x + 2.0):
        window.push(clamp(value, -LIMIT, LIMIT))
    magnitude = norm(x, window.mean(), weighted(window.values))
    return {"y": magnitude, "state": classify(magnitude)}
)PY";

/// 200k iterations of pure bytecode, and of a METH_O builtin call (the call trampoline's path).
constexpr const char* kWasmPyPureLoop =
    "def evaluate(inputs, params):\n    s = 0.0\n    for i in range(200000):\n        s += i * 0.5\n    return s\n";
constexpr const char* kWasmPyBuiltinLoop =
    "def evaluate(inputs, params):\n    s = 0\n    for i in range(200000):\n        s += abs(-i)\n    return "
    "float(s)\n";

/// Mean per-call micros over 2001 back-to-back runs. Not the desktop bench's median: browsers coarsen the clock
/// (about 20 us steps), which a per-call median reads as one step.
double wasmPyMeanMicros(
    const PJ::scripting::CompiledPythonScript& script, const std::vector<PJ::scripting::NamedValue>& inputs) {
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < 2001; ++i) {
    (void)wasmPyRun(script, inputs);
  }
  return wasmPyMillisSince(begin) * 1000.0 / 2001.0;
}

/// The worker half: everything a script run checks, off the GUI thread, as the app's EnginePool lanes run scripts.
void wasmPyWorkerChecks() {
  using namespace PJ::scripting;
  // First, before anything else imports: the allowed modules' lazy dependencies inside a strict, armed run (heapq
  // behind statistics.Counter, decimal's C fallback to _pydecimal behind statistics.Decimal), then the surface walk.
  if (auto lazy = compileOnDemandScriptPython(kWasmPyLazyScript)) {
    qInfo("PJ_WASM_PY_LAZY %s", wasmPyOutcome(wasmPyRun(*lazy, {})).c_str());
  } else {
    qInfo("PJ_WASM_PY_LAZY compile-error=%s", lazy.error().c_str());
  }
  const std::string stdlib_problems = wasmPythonStdlibProblems();
  qInfo("PJ_WASM_PY_STDLIB_SURFACE %s", stdlib_problems.empty() ? "ok" : stdlib_problems.c_str());

  const std::vector<NamedValue> no_inputs;
  auto value = compileOnDemandScriptPython(kWasmPyValueScript);
  if (!value) {
    qInfo("PJ_WASM_PY_VALUE compile-error=%s", value.error().c_str());
  } else {
    const auto begin = std::chrono::steady_clock::now();
    const std::string outcome = wasmPyOutcome(wasmPyRun(*value, no_inputs));
    qInfo("PJ_WASM_PY_VALUE %s first_run_ms=%.2f", outcome.c_str(), wasmPyMillisSince(begin));
  }

  const std::vector<NamedValue> x_input{NamedValue{.name = "x", .value = 3.0}};
  if (auto four = compileOnDemandScriptPython(kWasmPyFourLineScript)) {
    const std::string outcome = wasmPyOutcome(wasmPyRun(*four, x_input));
    qInfo("PJ_WASM_PY_PERCALL four_line_mean_us=%.2f %s", wasmPyMeanMicros(*four, x_input), outcome.c_str());
  }

  if (auto forty_three = compileOnDemandScriptPython(kWasmPyFortyThreeLineScript)) {
    qInfo("PJ_WASM_PY_PERCALL43 mean_us=%.2f", wasmPyMeanMicros(*forty_three, x_input));
  }
  for (const auto& [name, source] : {std::pair{"pure", kWasmPyPureLoop}, std::pair{"builtin", kWasmPyBuiltinLoop}}) {
    if (auto loop = compileOnDemandScriptPython(source)) {
      const auto begin = std::chrono::steady_clock::now();
      const std::string outcome = wasmPyOutcome(wasmPyRun(*loop, no_inputs));
      qInfo("PJ_WASM_PY_LOOP %s ns_per_iter=%.1f %s", name, wasmPyMillisSince(begin) * 1e6 / 200000.0, outcome.c_str());
    }
  }

  // The same pure loop in Luau: separates a browser's general wasm speed from CPython's.
  if (auto luau = compileOnDemandScript(
          "local inputs = ...\nlocal s = 0\nfor i = 0, 199999 do s += i * 0.5 end\nreturn { y = s }\n")) {
    const auto begin = std::chrono::steady_clock::now();
    auto result = runOnDemandScript(*luau, {}, "{}", {});
    qInfo(
        "PJ_WASM_PY_LOOP luau_pure ns_per_iter=%.1f %s", wasmPyMillisSince(begin) * 1e6 / 200000.0,
        wasmPyOutcome(result).c_str());
  }
  if (auto luau4 =
          compileOnDemandScript("local inputs = ...\nlocal x = inputs.x\nreturn { y = math.sqrt(x * x + 1.0) }\n")) {
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 2001; ++i) {
      (void)runOnDemandScript(*luau4, PJ::Span<const NamedValue>(x_input.data(), x_input.size()), "{}", {});
    }
    qInfo("PJ_WASM_PY_PERCALL luau_four_line_mean_us=%.2f", wasmPyMillisSince(begin) * 1000.0 / 2001.0);
  }

  if (auto spin = compileOnDemandScriptPython("def evaluate(inputs, params):\n    while True:\n        pass\n")) {
    RunOptions options;
    options.limits.python_watchdog_millis = 2000;
    const auto begin = std::chrono::steady_clock::now();
    const std::string outcome = wasmPyOutcome(wasmPyRun(*spin, no_inputs, options));
    qInfo("PJ_WASM_PY_WATCHDOG elapsed_ms=%.1f %s", wasmPyMillisSince(begin), outcome.c_str());
  }

  if (auto os = compileOnDemandScriptPython("import os\ndef evaluate(inputs, params):\n    return 1.0\n")) {
    qInfo("PJ_WASM_PY_IMPORT_OS %s", wasmPyOutcome(wasmPyRun(*os, no_inputs)).c_str());
  } else {
    qInfo("PJ_WASM_PY_IMPORT_OS compile-error=%s", os.error().c_str());
  }
  if (auto pi = compileOnDemandScriptPython(
          "import math\ndef evaluate(inputs, params):\n    math.pi = 3\n    return 1.0\n")) {
    qInfo("PJ_WASM_PY_PATCH_PI %s", wasmPyOutcome(wasmPyRun(*pi, no_inputs)).c_str());
  }
  // The allocation cap (64 MiB default): one large block, then many small ones (CPython 3.14 without pymalloc).
  for (const auto& [name, source] :
       {std::pair{"big", "def evaluate(inputs, params):\n    x = [0.0] * 100_000_000\n    return 1.0\n"},
        std::pair{
            "small", "def evaluate(inputs, params):\n    x = [str(i) for i in range(10_000_000)]\n    return 1.0\n"}}) {
    if (auto hog = compileOnDemandScriptPython(source)) {
      RunOptions options;
      options.limits.python_watchdog_millis = 20000;
      const auto result = wasmPyRun(*hog, no_inputs, options);
      qInfo(
          "PJ_WASM_PY_MEMCAP %s over_budget=%d %s", name, result ? 0 : (result.error().over_budget ? 1 : 0),
          wasmPyOutcome(result).c_str());
    }
  }
  qInfo("PJ_WASM_PY_DONE");
}

}  // namespace

/// The GUI half: the first compile boots the interpreter on the helper while this (the browser main) thread waits,
/// unbounded, exactly as the app's first Python compile does; then a bounded compile; then the worker checks.
extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_python_smoke() {
  using namespace PJ::scripting;
  qInfo("PJ_WASM_PY_AVAILABLE %d", pythonAvailable() ? 1 : 0);
  struct stat zip_stat{};
  qInfo(
      "PJ_WASM_PY_STDLIB stat=%d bytes=%lld", ::stat("/lib/python314.zip", &zip_stat),
      static_cast<long long>(zip_stat.st_size));
  auto begin = std::chrono::steady_clock::now();
  auto first = compileOnDemandScriptPython(kWasmPyValueScript);
  qInfo(
      "PJ_WASM_PY_BOOT gui_wait_ms=%.1f %s", wasmPyMillisSince(begin),
      first ? "ok" : ("error=" + first.error()).c_str());
  begin = std::chrono::steady_clock::now();
  auto second = compileOnDemandScriptPython(kWasmPyFourLineScript);
  qInfo(
      "PJ_WASM_PY_COMPILE gui_wait_ms=%.2f %s", wasmPyMillisSince(begin),
      second ? "ok" : ("error=" + second.error()).c_str());
  std::thread(&wasmPyWorkerChecks).detach();
}

/// Worst case for the GUI thread: a worker script holds the GIL inside one C call (`sum` over a long range, sized to
/// outlast the 2 s compile bound with room even in the fastest browser; it is a wall-clock hold, which
/// neither the GIL switch interval nor the watchdog interrupts) while the GUI compiles. A pure-Python loop would not
/// do: it hands the GIL over every switch interval (5 ms).
/// `pj_wasm_test_python_hold_gil` starts the holder; `pj_wasm_test_python_compile_now`, called while it runs, logs how
/// long the GUI waited (the bound is kPythonCompileWaitMillis).
extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_python_hold_gil() {
  std::thread([] {
    using namespace PJ::scripting;
    if (auto spin =
            compileOnDemandScriptPython("def evaluate(inputs, params):\n    return float(sum(range(60_000_000)))\n")) {
      RunOptions options;
      options.limits.python_watchdog_millis = 60000;
      const auto begin = std::chrono::steady_clock::now();
      const std::string outcome = wasmPyOutcome(wasmPyRun(*spin, {}, options));
      qInfo("PJ_WASM_PY_HOLD_DONE elapsed_ms=%.1f %s", wasmPyMillisSince(begin), outcome.c_str());
    }
  }).detach();
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_python_compile_now() {
  const auto begin = std::chrono::steady_clock::now();
  auto compiled = PJ::scripting::compileOnDemandScriptPython(kWasmPyFourLineScript);
  qInfo(
      "PJ_WASM_PY_BUSY_COMPILE gui_wait_ms=%.1f %s", wasmPyMillisSince(begin),
      compiled ? "ok" : ("error=" + compiled.error()).c_str());
}
