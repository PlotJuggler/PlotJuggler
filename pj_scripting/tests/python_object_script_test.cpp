// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The Python lane of the on-demand runner (python_object_script.h): compile once, a fresh namespace per run, the
// binding of the returned value, and the hardening (python_sandbox.h). Desktop only.

#include "pj_scripting/python_object_script.h"

#include <gtest/gtest.h>
#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "object_script_test_utils.h"
#include "pj_base/builtin/camera_info.hpp"
#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_annotations.hpp"
#include "pj_base/builtin/scene_entities.hpp"
#include "pj_base/builtin/voxel_grid.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_scripting/image_decoder.h"
#include "pj_scripting/memory_budget.h"
#include "pj_scripting/on_demand_script.h"
#include "pj_scripting/python_engine.h"
#include "python_object_binder.h"
#include "python_sandbox.h"
#include "python_test_helpers.h"
#include "python_test_probe.h"
#include "script_media.h"

namespace py = pybind11;

namespace PJ::scripting {
namespace {

Expected<CompiledPythonScript> compile(std::string_view source) {
  return compileOnDemandScriptPython(source);
}

Expected<ScriptBundle, ScriptError> run(
    const CompiledPythonScript& script, const std::vector<NamedValue>& inputs = {},
    const std::vector<OutputSpec>& outputs = {}, std::string_view params = "{}", const RunOptions& options = {}) {
  return runOnDemandScriptPython(
      script, Span<const NamedValue>(inputs.data(), inputs.size()), params,
      Span<const OutputSpec>(outputs.data(), outputs.size()), options);
}

/// Compiles and runs `source`; a compile error is returned as the run's error.
Expected<ScriptBundle, ScriptError> runSource(
    std::string_view source, const std::vector<NamedValue>& inputs = {}, const std::vector<OutputSpec>& outputs = {},
    std::string_view params = "{}", const RunOptions& options = {}) {
  auto compiled = compile(source);
  if (!compiled) {
    return unexpected(ScriptError{compiled.error()});
  }
  return run(*compiled, inputs, outputs, params, options);
}

OutputSpec number(const char* name) {
  return OutputSpec{name, OutputType::kNumber, {}};
}

const ScriptValue& output(const ScriptBundle& bundle, std::string_view name) {
  for (const NamedValue& value : bundle.outputs) {
    if (value.name == name) {
      return value.value;
    }
  }
  static const ScriptValue kMissing = Unavailable{"missing in test"};
  ADD_FAILURE() << "no output " << name;
  return kMissing;
}

double numberOf(const Expected<ScriptBundle, ScriptError>& result, std::string_view name = "value") {
  EXPECT_TRUE(result.has_value()) << result.error().message;
  if (!result) {
    return 0.0;
  }
  const auto* value = std::get_if<double>(&output(*result, name));
  EXPECT_NE(value, nullptr) << name;
  return value != nullptr ? *value : 0.0;
}

std::string errorOf(const Expected<ScriptBundle, ScriptError>& result) {
  EXPECT_FALSE(result.has_value());
  return result ? std::string() : result.error().message;
}

// ---- D2 and compile once ----------------------------------------------------------

TEST(PythonObjectScript, GlobalsDoNotLeakBetweenRuns) {
  auto counting = compile(R"PY(
calls = 0
def evaluate(inputs, params):
    global calls
    calls += 1
    return float(calls)
)PY");
  ASSERT_TRUE(counting) << counting.error();
  EXPECT_DOUBLE_EQ(numberOf(run(*counting)), 1.0);
  EXPECT_DOUBLE_EQ(numberOf(run(*counting)), 1.0) << "a run must start from a fresh module namespace";

  auto first = runSource("leaked_global = 42\ndef evaluate(inputs, params):\n    return float(leaked_global)\n");
  EXPECT_DOUBLE_EQ(numberOf(first), 42.0);
  const std::string second = errorOf(runSource("def evaluate(inputs, params):\n    return float(leaked_global)\n"));
  EXPECT_NE(second.find("leaked_global"), std::string::npos) << second;
}

TEST(PythonObjectScript, PythonModuleMutationDoesNotLeak) {
  for (const char* mutation : {"math.pi = 3", "del math.pi", "json.decoder.JSONDecoder = None", "math.extra = 1"}) {
    const std::string source =
        std::string("import math\nimport json\ndef evaluate(inputs, params):\n    ") + mutation + "\n    return 1.0\n";
    const std::string error = errorOf(runSource(source));
    EXPECT_NE(error.find("read-only"), std::string::npos) << mutation << ": " << error;
  }
  EXPECT_DOUBLE_EQ(
      numberOf(runSource("import math\ndef evaluate(inputs, params):\n    return math.pi\n")), std::numbers::pi);
  EXPECT_DOUBLE_EQ(
      numberOf(runSource("from math import pi\ndef evaluate(inputs, params):\n    return pi\n")), std::numbers::pi);
  py::gil_scoped_acquire gil;
  EXPECT_DOUBLE_EQ(py::module_::import("math").attr("pi").cast<double>(), std::numbers::pi)
      << "the real module is untouched";
}

int g_compile_events = 0;

int countCompiles(const char* event, PyObject* /*args*/, void* /*userdata*/) {
  if (std::strcmp(event, "compile") == 0) {
    ++g_compile_events;
  }
  return 0;
}

TEST(PythonObjectScript, ScriptIsCompiledOncePerRevision) {
  {
    detail::ensureInterpreter();
    py::gil_scoped_acquire gil;
    static const bool installed = (PySys_AddAuditHook(&countCompiles, nullptr), true);
    (void)installed;
  }
  const int before = g_compile_events;
  auto compiled = compile("import math\ndef evaluate(inputs, params):\n    return math.sqrt(inputs['x'])\n");
  ASSERT_TRUE(compiled) << compiled.error();
  EXPECT_GT(g_compile_events, before) << "the compile parses and compiles the source";

  const int after_compile = g_compile_events;
  for (int i = 0; i < 50; ++i) {
    EXPECT_DOUBLE_EQ(numberOf(run(*compiled, {NamedValue{.name = "x", .value = 16.0}})), 4.0);
  }
  EXPECT_EQ(g_compile_events, after_compile) << "a run executes the compiled code without parsing the source again";
}

// ---- the check before compiling (PY-06) ---------------------------------------------

TEST(PythonObjectScript, ValidateRejectsMissingEvaluate) {
  auto compiled = compile("x = 1\n");
  ASSERT_FALSE(compiled);
  EXPECT_NE(compiled.error().find("evaluate"), std::string::npos) << compiled.error();
  EXPECT_FALSE(compile("async def evaluate(inputs, params):\n    return 1.0\n"));
  EXPECT_TRUE(compile("def evaluate(inputs, params):\n    return 1.0\n"));
}

TEST(PythonObjectScript, ValidateRejectsSyntaxError) {
  EXPECT_FALSE(compile("def evaluate(inputs, params):\n  return {\n"));
  EXPECT_FALSE(compile("return 1\n")) << "a compile-time error that ast.parse accepts";
  const std::string huge(std::size_t{1} << 20, '#');
  auto too_large = compile(huge + "\ndef evaluate(inputs, params):\n    return 1.0\n");
  ASSERT_FALSE(too_large);
  EXPECT_NE(too_large.error().find("larger than"), std::string::npos) << too_large.error();
}

TEST(PythonObjectScript, SyntaxErrorMessageNamesTheScriptNotTheInterpreter) {
  auto compiled = compile("def evaluate(inputs, params):\n  return {\n");
  ASSERT_FALSE(compiled);
  EXPECT_NE(compiled.error().find("<string>, line"), std::string::npos) << compiled.error();
  EXPECT_EQ(compiled.error().find("At:"), std::string::npos) << compiled.error();
  EXPECT_EQ(compiled.error().find(".conan2"), std::string::npos) << compiled.error();
  EXPECT_EQ(compiled.error().find("ast.py"), std::string::npos) << compiled.error();
}

TEST(PythonObjectScript, DunderAccessRejectedBeforeRun) {
  auto formatted = compile(
      "import statistics\ndef evaluate(inputs, params):\n    return '{0.__globals__[sys]}'.format(statistics.mean)\n");
  ASSERT_FALSE(formatted);
  EXPECT_NE(formatted.error().find("use an f-string"), std::string::npos) << formatted.error();
  auto mapped = compile(
      "import statistics\ndef evaluate(inputs, params):\n    return '{m.__globals__}'.format_map({'m': "
      "statistics.mean})\n");
  ASSERT_FALSE(mapped);
  EXPECT_NE(mapped.error().find("use an f-string"), std::string::npos) << mapped.error();
  auto compiled = compile("def evaluate(inputs, params):\n    leaked = (0).__class__.__bases__\n    return 1.0\n");
  ASSERT_FALSE(compiled);
  EXPECT_NE(compiled.error().find("dunder"), std::string::npos) << compiled.error();
  for (const char* name : {"__builtins__", "globals", "locals", "vars", "__loader__", "__spec__"}) {
    auto named = compile(std::string("def evaluate(inputs, params):\n    x = ") + name + "\n    return 1.0\n");
    ASSERT_FALSE(named) << name;
    EXPECT_NE(named.error().find(std::string("forbidden name: ") + name), std::string::npos) << named.error();
  }
}

// ---- the sandbox layers (PY-03, GAP-24) ----------------------------------------------

TEST(PythonObjectScript, OpenIsNotAvailable) {
  for (const char* name :
       {"open", "exec", "eval", "compile", "input", "breakpoint", "help", "setattr", "delattr", "type"}) {
    const std::string removed =
        errorOf(runSource(std::string("def evaluate(inputs, params):\n    ") + name + "\n    return 1.0\n"));
    EXPECT_NE(removed.find("NameError"), std::string::npos) << name << ": " << removed;
  }
}

TEST(PythonObjectScript, AuditHookBlocksSubprocess) {
  // The import gate refuses `subprocess` first; the audit hook stands behind it (AuditHookDeniesOnlyWhileArmed).
  const std::string error =
      errorOf(runSource("def evaluate(inputs, params):\n    import subprocess\n    subprocess.Popen(['ls'])\n"));
  EXPECT_NE(error.find("not available"), std::string::npos) << error;
  for (const char* module : {"os", "sys", "random", "importlib", "builtins"}) {
    const std::string refused =
        errorOf(runSource(std::string("import ") + module + "\ndef evaluate(inputs, params):\n    return 1.0\n"));
    EXPECT_NE(refused.find("is not available"), std::string::npos) << module << ": " << refused;
  }
  EXPECT_NE(
      errorOf(runSource("from .math import pi\ndef evaluate(inputs, params):\n    return 1.0\n")).find("not available"),
      std::string::npos);
}

TEST(PythonObjectScript, GetattrDunderEscapeIsUnavailable) {
  const std::string error =
      errorOf(runSource("def evaluate(inputs, params):\n    getattr((), '__cl' + 'ass__')\n    return 1.0\n"));
  EXPECT_NE(error.find("getattr"), std::string::npos) << error;
}

TEST(PythonObjectScript, OsReachedThroughAllowedModuleIsDeniedByAuditHook) {
  // statistics keeps `random` in its namespace and random keeps `os` as `_os`: no dunder, no getattr. The read-only
  // copy of statistics leaves both out, and the audit hook refuses every os.* event behind it.
  test::Victim victim;
  const std::string error = errorOf(runSource(
      "import statistics\ndef evaluate(inputs, params):\n    statistics.random._os.remove(" +
      test::pyLiteral(victim.path) + ")\n    return 1.0\n"));
  EXPECT_NE(error.find("no attribute 'random'"), std::string::npos) << error;
  EXPECT_TRUE(std::filesystem::exists(victim.path));
}

TEST(PythonObjectScript, StatisticsSysAndJsonCodecsBuiltinsAreDenied) {
  test::Victim victim;
  const std::string via_sys = errorOf(runSource(
      "import statistics\ndef evaluate(inputs, params):\n    statistics.sys.modules['os'].remove(" +
      test::pyLiteral(victim.path) + ")\n    return 1.0\n"));
  EXPECT_NE(via_sys.find("no attribute 'sys'"), std::string::npos) << via_sys;
  const std::string via_codecs = errorOf(runSource(
      "import json\ndef evaluate(inputs, params):\n    json.codecs.builtins.exec('x = 1')\n    return 1.0\n"));
  EXPECT_NE(via_codecs.find("no attribute 'codecs'"), std::string::npos) << via_codecs;
  const std::string private_name =
      errorOf(runSource("import json\ndef evaluate(inputs, params):\n    return json._default_encoder.indent\n"));
  EXPECT_NE(private_name.find("no attribute '_default_encoder'"), std::string::npos) << private_name;
  EXPECT_TRUE(std::filesystem::exists(victim.path));
}

TEST(PythonObjectScript, StatisticsAndJsonStillImport) {
  auto result = runSource(R"PY(
import statistics
import json
from math import *
from json import decoder
def evaluate(inputs, params):
    assert decoder.JSONDecoder is json.JSONDecoder
    return statistics.mean([1.0, 2.0, 3.0]) + len(json.dumps([1])) + floor(0.5)
)PY");
  EXPECT_DOUBLE_EQ(numberOf(result), 5.0);
  EXPECT_DOUBLE_EQ(
      numberOf(runSource("import cmath\ndef evaluate(inputs, params):\n    return abs(cmath.sqrt(-4))\n")), 2.0);
}

TEST(PythonObjectScript, AllowedModulesImportTheirLazyDependenciesInAStrictRun) {
  // statistics re-exports collections.Counter, whose most_common(n) imports heapq, and decimal.Decimal, whose format
  // falls back to _pydecimal for a spec libmpdec lacks. Both imports would run at the first call, inside the armed
  // run. CPython 3.12's _pydecimal then refuses "_" itself (ValueError), after the import this checks.
  auto result = runSource(R"PY(
import statistics
def evaluate(inputs, params):
    top = statistics.Counter("aba").most_common(1)[0][1]
    try:
        text = format(statistics.Decimal("1234"), "_")
    except ValueError:
        text = "1_234"
    return top + len(text)
)PY");
  EXPECT_DOUBLE_EQ(numberOf(result), 2.0 + 5.0);
}

// ---- limits (GAP-25, the watchdog, the allocation cap) -------------------------------

TEST(PythonObjectScript, WatchdogInterruptsAPureLoop) {
  RunOptions options;
  options.limits.python_watchdog_millis = 50;
  const std::string error =
      errorOf(runSource("def evaluate(inputs, params):\n    while True:\n        pass\n", {}, {}, "{}", options));
  EXPECT_NE(error.find("TimeoutError: evaluate() exceeded its time budget of 50 ms"), std::string::npos) << error;

  const std::string caught = errorOf(runSource(
      "def evaluate(inputs, params):\n    try:\n        while True:\n            pass\n    except BaseException:\n"
      "        pass\n    return 1.0\n",
      {}, {}, "{}", options));
  EXPECT_NE(caught.find("time budget"), std::string::npos) << "a caught timeout still fails the run: " << caught;
  const std::string caught_and_looping = errorOf(runSource(
      "def evaluate(inputs, params):\n    try:\n        while True:\n            pass\n    except TimeoutError:\n"
      "        while True:\n            pass\n",
      {}, {}, "{}", options));
  EXPECT_NE(caught_and_looping.find("time budget"), std::string::npos) << "interrupted again: " << caught_and_looping;
  EXPECT_DOUBLE_EQ(numberOf(runSource("def evaluate(inputs, params):\n    return 1.0\n", {}, {}, "{}", options)), 1.0)
      << "no stale timeout reaches the next run";
}

// A run that catches every MemoryError the cap raises until its time also runs out failed on memory first, and is
// reported so.
TEST(PythonObjectScript, AMemoryStopIsReportedEvenWhenTheTimeAlsoRunsOut) {
  RunOptions options;
  options.limits.mem_bytes = std::size_t{1} << 20;
  options.limits.python_watchdog_millis = 200;
  auto result = runSource(
      "def evaluate(inputs, params):\n    while True:\n        try:\n            x = None\n            while True:\n"
      "                x = (x,)\n        except MemoryError:\n            pass\n",
      {}, {}, "{}", options);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().message, "script memory limit (1 MB) exceeded");
  EXPECT_TRUE(result.error().over_budget);
}

TEST(PythonObjectScript, AllocationCapFailsTheRun) {
  RunOptions options;
  options.limits.mem_bytes = std::size_t{4} << 20;
  auto result = runSource(
      "def evaluate(inputs, params):\n    blob = 'x' * (64 * 1024 * 1024)\n    return float(len(blob))\n", {}, {}, "{}",
      options);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().message, "script memory limit (4 MB) exceeded");
  EXPECT_TRUE(result.error().over_budget);
  auto caught = runSource(
      "def evaluate(inputs, params):\n    try:\n        'x' * (64 << 20)\n    except MemoryError:\n        pass\n"
      "    return 1.0\n",
      {}, {}, "{}", options);
  ASSERT_FALSE(caught) << "a caught MemoryError still fails the run";
  EXPECT_EQ(caught.error().message, "script memory limit (4 MB) exceeded");
  EXPECT_TRUE(caught.error().over_budget);
  // Hit while the host builds the params, before any script code runs.
  std::string big_params = R"({"values": [)";
  for (int i = 0; i < 300000; ++i) {  // about 10 MB of int objects and list
    big_params += std::to_string(i) + ",";
  }
  big_params.back() = ']';
  auto params = runSource("def evaluate(inputs, params):\n    return 1.0\n", {}, {}, big_params + "}", options);
  ASSERT_FALSE(params);
  EXPECT_EQ(params.error().message, "script memory limit (4 MB) exceeded");
  auto text = runSource(
      "def evaluate(inputs, params):\n    return 1.0\n", {}, {},
      R"({"text": ")" + std::string(std::size_t{8} << 20, 'x') + R"("})", options);
  ASSERT_FALSE(text) << "one string past the cap";
  EXPECT_EQ(text.error().message, "script memory limit (4 MB) exceeded");
  EXPECT_DOUBLE_EQ(
      numberOf(runSource("def evaluate(inputs, params):\n    return float(len('x' * 1024))\n", {}, {}, "{}", options)),
      1024.0)
      << "the next run has its own cap";
}

// GAP-25: while the lane is inside a native call that released the GIL, another thread's Python work proceeds, and
// it is neither audited as script code nor charged to the run's cap (both are per thread). The host registers the fake
// native call as a module under an allowed root, as a binder would bind a native operation.
TEST(PythonObjectScript, NativeOpReleasesTheGil) {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool released = false;
  {
    detail::ensureInterpreter();
    py::gil_scoped_acquire gil;
    py::module_ fake = py::module_::import("types").attr("ModuleType")("math.pj_fake_native");
    fake.attr("call") = py::cpp_function([&]() {
      py::gil_scoped_release no_gil;
      std::unique_lock lock(mutex);
      entered = true;
      changed.notify_all();
      return changed.wait_for(lock, std::chrono::seconds(10), [&] { return released; });
    });
    py::module_::import("sys").attr("modules")["math.pj_fake_native"] = fake;
  }
  RunOptions options;
  options.limits.mem_bytes = std::size_t{256} << 10;
  std::thread lane([&] {
    auto result = runSource(
        "from math.pj_fake_native import call\ndef evaluate(inputs, params):\n    return float(call())\n", {}, {}, "{}",
        options);
    EXPECT_DOUBLE_EQ(numberOf(result), 1.0) << "the native call saw the other thread finish";
  });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }));
  }
  std::string big_source = "def evaluate(inputs, params):\n    return len([";
  for (int i = 0; i < 20000; ++i) {
    big_source += "1,";
  }
  auto compiled = compile(big_source + "])\n");  // host work on this thread: compile audit events, megabytes of AST
  EXPECT_TRUE(compiled) << compiled.error();
  auto engine = makePythonEngine();
  auto classes = engine->inspectModule(
      "# pj-script: python\nclass T:\n    id = 'x'\n    @staticmethod\n    def create(params):\n        return T()\n"
      "    def calculate(self, time, value, *args):\n        return value\n",
      "test");
  EXPECT_TRUE(classes) << classes.error();
  {
    std::lock_guard lock(mutex);
    released = true;
  }
  changed.notify_all();
  lane.join();
  py::gil_scoped_acquire gil;
  py::module_::import("sys").attr("modules").attr("pop")("math.pj_fake_native");
}

// A timeout that fires while no bytecode runs (`evaluate` rebound to a native call that releases the GIL) stays
// pending on the thread: the run drops it, so it never reaches the host's next Python call or the next run.
TEST(PythonObjectScript, WatchdogTimeoutNeverReachesTheNextRun) {
  {
    detail::ensureInterpreter();
    py::gil_scoped_acquire gil;
    py::module_ fake = py::module_::import("types").attr("ModuleType")("math.pj_fake_sleep");
    fake.attr("sleep") = py::cpp_function([](py::args /*args*/) {
      py::gil_scoped_release no_gil;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      return 1.0;
    });
    py::module_::import("sys").attr("modules")["math.pj_fake_sleep"] = fake;
  }
  RunOptions options;
  options.limits.python_watchdog_millis = 20;
  const std::string error = errorOf(runSource(
      "from math.pj_fake_sleep import sleep\ndef evaluate(inputs, params):\n    pass\nevaluate = sleep\n", {}, {}, "{}",
      options));
  EXPECT_NE(error.find("time budget"), std::string::npos) << error;
  EXPECT_DOUBLE_EQ(numberOf(runSource("def evaluate(inputs, params):\n    return 1.0\n")), 1.0);
  py::gil_scoped_acquire gil;
  py::module_::import("sys").attr("modules").attr("pop")("math.pj_fake_sleep");
}

// A module copy is published only once complete: a first import that the cap interrupts leaves nothing behind for the
// next run.
TEST(PythonObjectScript, AModuleCopyIsPublishedWhole) {
  {
    detail::ensureInterpreter();
    py::gil_scoped_acquire gil;
    py::module_ wide = py::module_::import("types").attr("ModuleType")("math.pj_fake_wide");
    for (int i = 0; i < 4000; ++i) {
      wide.attr(("a" + std::to_string(i)).c_str()) = i;
    }
    py::module_::import("sys").attr("modules")["math.pj_fake_wide"] = wide;
  }
  RunOptions tiny;
  tiny.limits.mem_bytes = std::size_t{96} << 10;
  const std::string error = errorOf(runSource(
      "from math.pj_fake_wide import a0\ndef evaluate(inputs, params):\n    return 1.0\n", {}, {}, "{}", tiny));
  EXPECT_NE(error.find("script memory limit"), std::string::npos) << error;
  EXPECT_DOUBLE_EQ(
      numberOf(
          runSource("from math.pj_fake_wide import a3999\ndef evaluate(inputs, params):\n    return float(a3999)\n")),
      3999.0);
  py::gil_scoped_acquire gil;
  py::module_::import("sys").attr("modules").attr("pop")("math.pj_fake_wide");
}

// The allowed modules' copies are built when the interpreter starts, so a run's first import neither builds them under
// its cap nor leaves them charged to it.
TEST(PythonObjectScript, AllowedModuleCopiesAreBuiltAtStartup) {
  RunOptions small;
  small.limits.mem_bytes = std::size_t{8} << 10;
  EXPECT_DOUBLE_EQ(
      numberOf(runSource(
          "import statistics\nimport json\nimport cmath\ndef evaluate(inputs, params):\n"
          "    return 1.0\n",
          {}, {}, "{}", small)),
      1.0);
}

// CPython 3.12's MAKE_FUNCTION releases the code object twice when the function's allocation fails, so the cap never
// refuses a small block: every cap from tiny to ample fails its run cleanly on one compiled script, which then still
// runs, many times, and so does a script that catches the MemoryError of every function it makes.
TEST(PythonObjectScript, ACapRefusingAFunctionLeavesTheCompiledScriptIntact) {
  auto compiled = compile(
      "def helper(x):\n    return x + 1.0\ndef evaluate(inputs, params):\n    inner = lambda y: y * 2.0\n"
      "    return helper(inner(1.0))\n");
  ASSERT_TRUE(compiled) << compiled.error();
  for (std::size_t bytes = 1024; bytes <= std::size_t{48} << 10; bytes += 8) {
    RunOptions options;
    options.limits.mem_bytes = bytes;
    (void)run(*compiled, {}, {}, "{}", options);
  }
  for (int i = 0; i < 200; ++i) {
    ASSERT_DOUBLE_EQ(numberOf(run(*compiled)), 3.0) << "run " << i;
  }
  // Within one run: a script that catches the MemoryError of a refused function and makes it again reloads the same
  // code object.
  auto retrying = compile(
      "def evaluate(inputs, params):\n    for _ in range(4):\n        try:\n            made = lambda: 1.0\n"
      "        except MemoryError:\n            pass\n    return 1.0\n");
  ASSERT_TRUE(retrying) << retrying.error();
  for (std::size_t bytes = 1024; bytes <= std::size_t{48} << 10; bytes += 8) {
    RunOptions options;
    options.limits.mem_bytes = bytes;
    (void)run(*retrying, {}, {}, "{}", options);
  }
  EXPECT_DOUBLE_EQ(numberOf(run(*retrying)), 1.0);
  EXPECT_DOUBLE_EQ(numberOf(run(*compiled)), 3.0);
}

// ---- inputs, params and binding --------------------------------------------------------

TEST(PythonObjectScript, ScalarInputsParamsAndOutputsBind) {
  auto declared = runSource(
      R"PY(
def evaluate(inputs, params):
    return {"sum": inputs["a"] + params["scale"]["k"], "label": inputs["name"] + "!", "gone": pj.unavailable("no data")}
)PY",
      {NamedValue{.name = "a", .value = 2.0}, NamedValue{.name = "name", .value = std::string("cam")}},
      {number("sum"), OutputSpec{"label", OutputType::kString, {}}, number("gone")}, R"({"scale": {"k": 3}})");
  ASSERT_TRUE(declared) << declared.error().message;
  ASSERT_EQ(declared->outputs.size(), 3u);
  EXPECT_DOUBLE_EQ(std::get<double>(output(*declared, "sum")), 5.0);
  EXPECT_EQ(std::get<std::string>(output(*declared, "label")), "cam!");
  EXPECT_EQ(std::get<Unavailable>(output(*declared, "gone")).reason, "no data");

  auto inferred = runSource("def evaluate(inputs, params):\n    return {'b': 2, 'a': 'x'}\n");
  ASSERT_TRUE(inferred) << inferred.error().message;
  ASSERT_EQ(inferred->outputs.size(), 2u);
  EXPECT_EQ(inferred->outputs[0].name, "a") << "keyed outputs are sorted by name";
  auto positional = runSource("def evaluate(inputs, params):\n    return (1.0, 'one', 2)\n");
  ASSERT_TRUE(positional) << positional.error().message;
  EXPECT_EQ(positional->outputs[1].name, "text");
  EXPECT_EQ(positional->outputs[2].name, "value_2");
  EXPECT_DOUBLE_EQ(numberOf(runSource("def evaluate(inputs, params):\n    return 7\n"), "value"), 7.0);
  auto cut = runSource("def evaluate(inputs, params):\n    return {'x': pj.unavailable('r' * 5000)}\n");
  ASSERT_TRUE(cut) << cut.error().message;
  EXPECT_EQ(std::get<Unavailable>(output(*cut, "x")).reason.size(), 1024u);
}

TEST(PythonObjectScript, RejectsBadInputsParamsAndReturns) {
  const auto fails = [](std::string_view source, std::vector<NamedValue> inputs, std::vector<OutputSpec> outputs,
                        std::string_view params, std::string_view expected) {
    const std::string error = errorOf(runSource(source, inputs, outputs, params));
    EXPECT_NE(error.find(expected), std::string::npos) << "expected '" << expected << "', got: " << error;
  };
  constexpr const char* kOne = "def evaluate(inputs, params):\n    return 1.0\n";
  fails(kOne, {NamedValue{.name = "a", .value = Unavailable{"r"}}}, {}, "{}", "input 'a' is unavailable");
  fails(kOne, {NamedValue{.name = "cloud", .value = sdk::BuiltinObject()}}, {}, "{}", "which scripts cannot read");
  fails(kOne, {}, {}, "[1]", "params_json is not a JSON object");
  fails(kOne, {}, {}, std::string(40, '[') + std::string(40, ']'), "params_json is not a JSON object");
  fails(kOne, {}, {}, R"({"a":)" + std::string(40, '[') + std::string(40, ']') + "}", "nested deeper than 32");
  fails("def evaluate(inputs, params):\n    return True\n", {}, {}, "{}", "unsupported type: bool");
  fails("def evaluate(inputs, params):\n    return {1: 2.0}\n", {}, {}, "{}", "returned dict keys must be strings");
  fails("def evaluate(inputs, params):\n    return {'x': None}\n", {}, {number("x")}, "{}", "output 'x' missing");
  fails("def evaluate(inputs, params):\n    return {'x': [1.0]}\n", {}, {number("x")}, "{}", "must be a number");
  fails(
      "def evaluate(inputs, params):\n    return {'x': 1.0, 'y': 2.0}\n", {}, {number("x")}, "{}",
      "unexpected output 'y'");
  fails("def evaluate(inputs, params):\n    return [pj.unavailable('r')]\n", {}, {}, "{}", "is unavailable: name");
  fails("def evaluate(inputs, params):\n    return {}\n", {}, {}, "{}", "returned no values");
  fails("def evaluate(inputs, params):\n    return 10 ** 400\n", {}, {}, "{}", "OverflowError");
  fails("def evaluate(inputs, params):\n    return {'x': pj.unavailable(3)}\n", {}, {}, "{}", "must be a string");
  fails(
      "def evaluate(inputs, params):\n    u = pj.unavailable('r')\n    u.reason = 3\n    return {'x': u}\n", {}, {},
      "{}", "must be a string");
  fails("def evaluate(inputs, params):\n    raise ValueError('boom')\n", {}, {}, "{}", "ValueError: boom");
  fails(
      "evaluate = None\ndef evaluate(inputs, params):\n    return 1.0\nevaluate = 3\n", {}, {}, "{}",
      "'int' object is not callable");
  EXPECT_EQ(errorOf(run(CompiledPythonScript{})), "the python script is not compiled");
}

// A run past its deadline fails even when the watchdog never got the GIL: the string is built and returned without a
// bytecode check in between, then copied by the host.
TEST(PythonObjectScript, AnOverdueRunFailsWithoutAnInterruption) {
  RunOptions options;
  options.limits.python_watchdog_millis = 1;
  options.limits.mem_bytes = std::size_t{512} << 20;
  options.native_budget_bytes = std::size_t{512} << 20;
  const std::string error =
      errorOf(runSource("def evaluate(inputs, params):\n    return 'x' * (300 << 20)\n", {}, {}, "{}", options));
  EXPECT_NE(error.find("time budget of 1 ms"), std::string::npos) << error;
}

TEST(PythonObjectScript, ScriptCodeRunsWithTheHookArmed) {
  test::AuditProbe& probe = test::auditProbe();
  const int refused = probe.refused;
  const std::string error = errorOf(
      runSource("from math.pj_audit_probe import emit\ndef evaluate(inputs, params):\n    emit()\n    return 1.0\n"));
  EXPECT_NE(error.find("PermissionError"), std::string::npos) << error;
  const std::string strict = errorOf(runSource(
      "from math.pj_audit_probe import emit_compile\ndef evaluate(inputs, params):\n    emit_compile()\n    return "
      "1.0\n"));
  EXPECT_NE(strict.find("PermissionError: compile is not available"), std::string::npos)
      << "the strict profile: " << strict;
  EXPECT_EQ(probe.refused, refused + 2);
  EXPECT_EQ(probe.allowed, 0);
}

// The namespace is cleared, armed, on every exit: a finalizer a global carries runs inside the run even when
// `evaluate` raises (it keeps `emit` as a default: the cleared namespace no longer holds it).
TEST(PythonObjectScript, FinalizersRunArmedOnEveryExit) {
  test::AuditProbe& probe = test::auditProbe();
  for (const char* ending : {"    return 1.0\n", "    raise ValueError('boom')\n"}) {
    const int refused = probe.refused;
    (void)runSource(
        std::string(
            "from math.pj_audit_probe import emit\nclass Probe:\n    def __del__(self, emit=emit):\n"
            "        emit()\nkept = Probe()\ndef evaluate(inputs, params):\n") +
        ending);
    EXPECT_EQ(probe.refused, refused + 1) << ending;
  }
  EXPECT_EQ(probe.allowed, 0);
}

// The script's namespace is a cycle (its functions hold it), broken inside the run: a finalizer it triggers runs
// under the run's watchdog, not later on whichever thread collects the cycle.
TEST(PythonObjectScript, FinalizersRunInsideTheRun) {
  RunOptions options;
  options.limits.python_watchdog_millis = 50;
  const std::string error = errorOf(runSource(
      R"PY(
class Slow:
    def __del__(self):
        for _ in range(10 ** 8):
            pass

kept = Slow()

def evaluate(inputs, params):
    return 1.0
)PY",
      {}, {}, "{}", options));
  EXPECT_NE(error.find("TimeoutError"), std::string::npos) << error;
}

TEST(PythonObjectScript, ReturnedStringsAreChargedToTheNativeBudget) {
  RunOptions options;
  options.native_budget_bytes = std::size_t{4} << 20;
  const std::string error = errorOf(
      runSource("def evaluate(inputs, params):\n    s = 'x' * (1 << 20)\n    return [s] * 8\n", {}, {}, "{}", options));
  EXPECT_NE(error.find("script native memory limit (4 MB) exceeded"), std::string::npos) << error;
}

TEST(PythonObjectScript, CancelledRunFailsWithoutRunning) {
  RunOptions options;
  options.cancelled = [] { return true; };
  EXPECT_EQ(
      errorOf(runSource("def evaluate(inputs, params):\n    raise ValueError('ran')\n", {}, {}, "{}", options)),
      "script cancelled");
  int polls = 0;
  options.cancelled = [&polls] { return ++polls > 1; };
  EXPECT_EQ(
      errorOf(runSource("def evaluate(inputs, params):\n    return 1.0\n", {}, {}, "{}", options)), "script cancelled")
      << "a run cancelled while it ran publishes nothing";
}

// ---- objects, builders, pj.tf and media (slice 10b) -------------------------------------------------

using test::FakeImageDecoder;
using test::kNaN;
using test::kSecond;
using test::makeCloud;
using test::makeLineCloud;

NamedValue objectInput(std::string name, sdk::BuiltinObject value, Timestamp stamp_shift = 0) {
  return NamedValue{.name = std::move(name), .value = ScriptValue{std::move(value)}, .stamp_shift = stamp_shift};
}

std::vector<OutputSpec> specs(std::initializer_list<const char*> declarations) {
  std::vector<OutputSpec> out;
  for (const char* declaration : declarations) {
    auto spec = parseOutputSpec(declaration);
    EXPECT_TRUE(spec.has_value()) << declaration;
    if (spec) {
      out.push_back(*spec);
    }
  }
  return out;
}

template <class T>
const T* objectOf(const Expected<ScriptBundle, ScriptError>& result, std::string_view name) {
  if (!result) {
    ADD_FAILURE() << result.error().message;
    return nullptr;
  }
  const auto* object = std::get_if<sdk::BuiltinObject>(&output(*result, name));
  return object != nullptr ? object->get<T>() : nullptr;
}

std::string textOf(const Expected<ScriptBundle, ScriptError>& result, std::string_view name) {
  EXPECT_TRUE(result.has_value()) << result.error().message;
  const auto* text = result ? std::get_if<std::string>(&output(*result, name)) : nullptr;
  return text != nullptr ? *text : std::string("<missing>");
}

std::string reasonOf(const Expected<ScriptBundle, ScriptError>& result, std::string_view name) {
  EXPECT_TRUE(result.has_value()) << result.error().message;
  const auto* unavailable = result ? std::get_if<Unavailable>(&output(*result, name)) : nullptr;
  return unavailable != nullptr ? unavailable->reason : std::string("<available>");
}

RunOptions hostAt(Timestamp query_time, std::shared_ptr<const pj::tf::TransformSnapshot> tf = test::mapFromBaseLink()) {
  RunOptions options;
  options.tf = std::move(tf);
  options.query_time = query_time;
  return options;
}

RunOptions withNativeBudget(std::size_t bytes) {
  RunOptions options;
  options.native_budget_bytes = bytes;
  return options;
}

/// A 4 x 3 gradient (sample = index) behind every image.
RunOptions withGradient(std::shared_ptr<FakeImageDecoder>* decoder = nullptr) {
  auto made = std::make_shared<FakeImageDecoder>(4, 3);
  if (decoder != nullptr) {
    *decoder = made;
  }
  RunOptions options;
  options.image_decoder = made;
  return options;
}

NamedValue imageInput(std::string name, std::string topic, Timestamp stamp, const char* encoding = "mono8") {
  sdk::Image image;
  image.encoding = encoding;
  image.width = 4;
  image.height = 3;
  image.timestamp_ns = stamp;
  NamedValue input = objectInput(std::move(name), sdk::BuiltinObject(image));
  input.topic = std::move(topic);
  return input;
}

// US1 + US5, the twin of object_script_test.cpp's reference script (LUA-04).
constexpr const char* kReferenceScript = R"PY(
def evaluate(inputs, params):
    cloud = inputs["cloud"]
    cropped = cloud.crop_box(min=(-5, -5, params["zmin"]), max=(5, 5, inputs["threshold"]))
    top = cropped.extreme_point("z", "max")
    if top is None:
        return {"cropped": cropped, "max_z": pj.unavailable("no finite points in region"), "report": "empty",
                "witness": pj.scene.new(frame_id=cloud.frame_id, id="witness").finish()}
    witness = (pj.scene.new(frame_id=cloud.frame_id, id="witness")
               .sphere(center=(top["x"], top["y"], top["z"]), diameter=0.2, color=(255, 0, 0, 255))
               .text(position=(top["x"], top["y"], top["z"] + 0.3), text=f"{top['z']:.2f}", font_size=0.2))
    return {"cropped": cropped, "max_z": top["z"], "report": f"{cropped.count()} points; max z {top['z']:.2f}",
            "witness": witness.finish()}
)PY";

Expected<ScriptBundle, ScriptError> runReferenceScript(sdk::PointCloud cloud) {
  return runSource(
      kReferenceScript, {objectInput("cloud", std::move(cloud)), NamedValue{.name = "threshold", .value = 5.0}},
      specs({"cropped:kPointCloud", "max_z:number", "report:string", "witness:kSceneEntities"}), R"({"zmin": -5})");
}

TEST(PythonObjectScript, ReferenceScriptHappyPathPython) {
  auto result = runReferenceScript(makeCloud({{0, 0, 0, 0.1f}, {10, 10, 10, 0.5f}, {1, 1, 1, 0.9f}}));
  EXPECT_DOUBLE_EQ(numberOf(result, "max_z"), 1.0);
  EXPECT_EQ(textOf(result, "report"), "2 points; max z 1.00");
  const auto* cropped = objectOf<sdk::PointCloud>(result, "cropped");
  ASSERT_NE(cropped, nullptr);
  EXPECT_EQ(cropped->width, 2u);
  const auto* witness = objectOf<sdk::SceneEntities>(result, "witness");
  ASSERT_NE(witness, nullptr);
  ASSERT_EQ(witness->deletions.size(), 1u);
  ASSERT_EQ(witness->entities.size(), 1u);
  EXPECT_EQ(witness->entities[0].frame_id, "base_link");
  ASSERT_EQ(witness->entities[0].spheres.size(), 1u);
  EXPECT_EQ(witness->entities[0].spheres[0].pose.position, (sdk::Vector3{1, 1, 1}));
  ASSERT_EQ(witness->entities[0].texts.size(), 1u);
  EXPECT_EQ(witness->entities[0].texts[0].text, "1.00");

  auto empty = runReferenceScript(makeCloud({{10, 10, 10, 0.5f}}));
  EXPECT_EQ(reasonOf(empty, "max_z"), "no finite points in region");
  EXPECT_EQ(textOf(empty, "report"), "empty");
  const auto* cleared = objectOf<sdk::SceneEntities>(empty, "witness");
  ASSERT_NE(cleared, nullptr);
  EXPECT_TRUE(cleared->entities.empty());
}

TEST(PythonObjectScript, AnnotationsBuilderProducesPrimitives) {
  RunOptions options;
  options.result_stamp = 7;
  auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    return (pj.annotations.new(image_topic="/camera")
            .points(points=[(1, 2), (5, 2), (5, 8), (1, 8)], type="line_loop", outline_color=(255, 0, 0, 128))
            .circle(center=(3, 4), diameter=10, fill_color=(0, 255, 0, 90))
            .text(position=(1, 2), text="max_z", font_size=20, text_color=(3, 4, 5))
            .finish())
)PY",
      {}, specs({"overlay:kImageAnnotations"}), "{}", options);
  const auto* annotations = objectOf<sdk::ImageAnnotations>(result, "overlay");
  ASSERT_NE(annotations, nullptr);
  EXPECT_EQ(annotations->image_topic, "/camera");
  EXPECT_EQ(annotations->timestamp, 7);  // no image input: the result's stamp
  ASSERT_EQ(annotations->points.size(), 1u);
  EXPECT_EQ(annotations->points[0].points.size(), 4u);
  EXPECT_EQ(annotations->points[0].color, (sdk::ColorRGBA{255, 0, 0, 128}));
  ASSERT_EQ(annotations->circles.size(), 1u);
  EXPECT_DOUBLE_EQ(annotations->circles[0].radius, 5.0);
  EXPECT_EQ(annotations->circles[0].fill_color.a, 90);
  ASSERT_EQ(annotations->texts.size(), 1u);
  EXPECT_EQ(annotations->texts[0].text, "max_z");
  EXPECT_EQ(annotations->texts[0].color, (sdk::ColorRGBA{3, 4, 5, 255}));
}

TEST(PythonObjectScript, AnnotationsRequireImageTopicAndRejectOpaqueBackground) {
  const auto fails = [](const std::string& body, std::string_view expected) {
    const std::string error = errorOf(runSource("def evaluate(inputs, params):\n    " + body + "\n    return 1\n"));
    EXPECT_NE(error.find(expected), std::string::npos) << body << " -> " << error;
  };
  fails("pj.annotations.new()", "image_topic");
  fails("pj.annotations.new(image_topic='')", "image_topic");
  const std::string head = "pj.annotations.new(image_topic='/cam')";
  fails(head + ".points(points=[(1, 2), (3,)], type='line_strip')", "'points'");
  fails(head + ".points(points=[(1, 2)])", "type");
  fails(head + ".circle(center=(0, 1), diameter=1, outline_color=(256, 0, 0))", "'outline_color'");
  fails(head + ".circle(center=(0, 1), diameter=-1)", "'diameter'");
  fails(head + ".text(position=(0, 1), text='x', background_color=(0, 0, 0, 255))", "'background_color'");
  EXPECT_DOUBLE_EQ(
      numberOf(runSource(
          "def evaluate(inputs, params):\n    " + head +
          ".text(position=(0, 1), text='x', background_color=(0, 0, 0, 0))\n    return 1\n")),
      1.0);
}

TEST(PythonObjectScript, AnnotationsCountSumsPointsOverAllEntries) {
  auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    overlay = inputs["image"].decode()[0].threshold(">", 8)
    built = (pj.annotations.new(image_topic="/cam").points(points=[(0, 0), (1, 1)], type="points")
             .points(points=[(2, 2)], type="points").finish())
    return {"matching": overlay.count(), "built_count": built.count()}
)PY",
      {imageInput("image", "/cam", 0)}, specs({"matching:number", "built_count:number"}), "{}", withGradient());
  EXPECT_DOUBLE_EQ(numberOf(result, "matching"), 3.0);  // 9, 10, 11 exceed 8
  EXPECT_DOUBLE_EQ(numberOf(result, "built_count"), 3.0);
}

TEST(PythonObjectScript, SceneBuilderChainsAllPrimitivesAndFinishSpendsIt) {
  auto result = runSource(R"PY(
def evaluate(inputs, params):
    return (pj.scene.new(frame_id="map", id="e1")
            .sphere(center=(0, 0, 0), diameter=1, color=(255, 0, 0))
            .text(position=(1, 1, 1), text="hi", font_size=0.5, billboard=True)
            .cube(center=(2, 2, 2), size=(1, 2, 3), color=(0, 255, 0, 128))
            .line_strip(points=[(0, 0, 0), (1, 0, 0), (1, 1, 0), (1, 1, 1)], thickness=0.05)
            .finish())
)PY");
  const auto* scene = objectOf<sdk::SceneEntities>(result, "value");
  ASSERT_NE(scene, nullptr);
  ASSERT_EQ(scene->entities.size(), 1u);
  const auto& entity = scene->entities[0];
  ASSERT_EQ(entity.spheres.size(), 1u);
  EXPECT_EQ(entity.spheres[0].color.a, 255);
  ASSERT_EQ(entity.texts.size(), 1u);
  EXPECT_TRUE(entity.texts[0].billboard);
  ASSERT_EQ(entity.cubes.size(), 1u);
  EXPECT_EQ(entity.cubes[0].size, (sdk::Vector3{1, 2, 3}));
  ASSERT_EQ(entity.lines.size(), 1u);
  EXPECT_EQ(entity.lines[0].points.size(), 4u);

  const auto fails = [](const std::string& body, std::string_view expected) {
    const std::string error = errorOf(runSource(
        "def evaluate(inputs, params):\n    b = pj.scene.new(frame_id='map', id='e')\n    " + body +
        "\n    return 1\n"));
    EXPECT_NE(error.find(expected), std::string::npos) << body << " -> " << error;
  };
  fails("b.finish()\n    b.sphere(center=(0, 0, 0), diameter=1)", "finished");
  fails("b.finish()\n    b.finish()", "finished");
  fails("b.sphere(center=(0, 0, 0), diameter=1, color=(300, 0, 0))", "'color'");
  fails("b.sphere(center=(0, 0, 0), diameter=1, color=(1, 2))", "'color'");
  fails("b.line_strip(points=[(0, 0, 0), (1, 1)], thickness=1)", "'points' entry must be a sequence of 3 numbers");
  fails("b.line_strip(points=3, thickness=1)", "'points' must be a sequence");
  fails("b.sphere(center=(0, 0), diameter=1)", "'center' must be a sequence of 3 numbers");
  fails("b.sphere(center=(float('nan'), 0, 0), diameter=1)", "'center'");
  fails("pj.scene.new(frame_id='map', id='')", "'id'");
  // Script code an argument runs (a sequence's __len__) cannot finish the builder the call then uses.
  fails(
      "class Trap:\n        def __len__(self):\n            b.finish()\n            return 3\n"
      "        def __getitem__(self, index):\n            if index >= 3:\n                raise IndexError(index)\n"
      "            return 0.0\n    b.sphere(center=Trap(), diameter=1)",
      "finished");
}

// D14, the twin of ObjectScript.CloudBuilderBuildsPresetsAndDownsamples: `pj.cloud` builds a preset cloud, `add` takes
// the point's values, `add_points` a sequence of them, and `downsample` keeps one centroid per cube.
TEST(PythonObjectScript, CloudBuilderBuildsPresetsAndDownsamples) {
  auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    b = pj.cloud.new(preset="xyzrgb", frame_id="map")
    b.add(1, 2, 3, 255, 0, 0).add_points([(4, 5, 6, 0, 255, 0), [4.5, 5, 6, 0, 0, 255]])
    cloud = b.finish()
    return {"cloud": cloud, "count": cloud.count(), "centroids": cloud.downsample(voxel_size=1)}
)PY",
      {}, specs({"cloud:kPointCloud", "count:number", "centroids:kPointCloud"}));
  EXPECT_DOUBLE_EQ(numberOf(result, "count"), 3.0);
  const auto* cloud = objectOf<sdk::PointCloud>(result, "cloud");
  ASSERT_NE(cloud, nullptr);
  EXPECT_EQ(cloud->frame_id, "map");
  ASSERT_EQ(cloud->data.size(), 48u);
  EXPECT_EQ(cloud->data[12], 255);
  EXPECT_EQ(cloud->data[16 + 13], 255);
  EXPECT_EQ(cloud->data[32 + 14], 255);
  const auto* centroids = objectOf<sdk::PointCloud>(result, "centroids");
  ASSERT_NE(centroids, nullptr);
  EXPECT_EQ(centroids->width, 2u);

  const auto fails = [](const std::string& body, std::string_view expected, const RunOptions& options = {}) {
    const std::string error = errorOf(runSource(
        "def evaluate(inputs, params):\n    b = pj.cloud.new(preset='xyzi', frame_id='map')\n    " + body +
            "\n    return 1\n",
        {}, {}, "{}", options));
    EXPECT_NE(error.find(expected), std::string::npos) << body << " -> " << error;
  };
  fails("b.add(1, 2, 3)", "4 values");
  fails("b.add(1, 2, 3, 'x')", "number");
  fails("b.add_points([(1, 2, 3, 4), (1, 2, 3)])", "4 values");
  fails("b.add_points(3)", "'points' must be a sequence");
  fails("b.add(float('nan'), 2, 3, 4)", "finite");
  fails("b.add(1e300, 2, 3, 4)", "32-bit float");
  fails("b.add_points([(1, 2, 3, -1e300)])", "32-bit float");
  fails("b.finish()\n    b.add(1, 2, 3, 4)", "finished");
  fails("b.finish()\n    b.finish()", "finished");
  fails("pj.cloud.new(preset='xyzrgba', frame_id='map')", "'preset'");
  fails("b.finish().downsample(voxel_size=0)", "voxel_size");
  fails("for i in range(1000000):\n        b.add(i, 0, 0, 0)", "native memory limit", withNativeBudget(1 << 20));
  fails("b.add_points([(1, 2, 3, 4)] * 200000 + ['bad'])", "native memory limit", withNativeBudget(1 << 20));
  // Script code an entry's conversion runs cannot finish the builder the call then uses.
  fails(
      "class Trap:\n        def __float__(self):\n            b.finish()\n            return 0.0\n"
      "    b.add_points([(Trap(), 0, 0, 0)])",
      "finished");
}

// D16, the twin of ObjectScript.VoxelGridOutputIsProducible; indices are 0-based, as every Python index is.
TEST(PythonObjectScript, VoxelGridOutputIsProducible) {
  auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    grid = pj.voxels.new(frame_id="map", origin=(1, 2, 3), cell_size=0.5, dims=(2, 3, 4))
    grid.set(1, 2, 3, 7.5)
    return grid.set(0, 0, 0, 1).finish()
)PY",
      {}, specs({"occupied:kVoxelGrid"}));
  const auto* grid = objectOf<sdk::VoxelGrid>(result, "occupied");
  ASSERT_NE(grid, nullptr);
  EXPECT_EQ(grid->origin.position, (sdk::Vector3{1, 2, 3}));
  ASSERT_EQ(grid->data.size(), 96u);
  float last = 0;
  std::memcpy(&last, grid->data.data() + 92, sizeof(last));
  EXPECT_EQ(last, 7.5f);

  const auto fails = [](const std::string& body, std::string_view expected) {
    const std::string error = errorOf(runSource(
        "def evaluate(inputs, params):\n    g = pj.voxels.new(frame_id='m', origin=(0, 0, 0), cell_size=1, "
        "dims=(2, 2, 2))\n    " +
        body + "\n    return 1\n"));
    EXPECT_NE(error.find(expected), std::string::npos) << body << " -> " << error;
  };
  fails("g.set(2, 0, 0, 1)", "outside the grid");
  fails("g.set(-1, 0, 0, 1)", "outside the grid");
  fails("g.set(1.5, 0, 0, 1)", "integer");
  fails("g.finish()\n    g.set(0, 0, 0, 1)", "finished");
  fails("g.finish().data", "raw buffer");
  fails("pj.voxels.new(frame_id='m', origin=(0, 0, 0), cell_size=1, dims=(65536, 65536, 65536))", "cells");
  fails("pj.voxels.new(frame_id='m', origin=(0, 0, 0), cell_size=1, dims=(2, 2))", "'dims'");
  for (const char* cell_size : {"-1", "float('nan')", "float('inf')"}) {
    fails(
        std::string("pj.voxels.new(frame_id='m', origin=(0, 0, 0), dims=(2, 2, 2), cell_size=") + cell_size + ")",
        "'cell_size'");
  }
  fails("g.set(1, 1, 1, 1e300)", "32-bit float");
  const auto inferred = runSource(
      "def evaluate(inputs, params):\n    return [pj.voxels.new(frame_id='m', origin=(0, 0, 0), cell_size=1, "
      "dims=(1, 1, 1)).finish()]\n");
  EXPECT_NE(objectOf<sdk::VoxelGrid>(inferred, "voxels"), nullptr);
}

// The twin of ObjectScript.VoxelGridFieldsAreReadable; list indices are 0-based.
TEST(PythonObjectScript, VoxelGridFieldsAreReadable) {
  const auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    built = pj.voxels.new(frame_id="map", origin=(1, 2, 3), cell_size=0.5, dims=(2, 3, 4)).finish()
    given = inputs["grid"]
    return {"frame": built.frame_id, "x": built.origin.position.x, "size": built.cell_size.y,
            "stride": built.cell_stride, "cells": built.column_count * built.row_count * built.slice_count,
            "channel": built.fields[0].name, "given_frame": given.frame_id, "given_slices": given.slice_count,
            "given_size": given.cell_size.z}
)PY",
      {objectInput("grid", test::makeColumnGrid(5))});
  EXPECT_EQ(textOf(result, "frame"), "map");
  EXPECT_DOUBLE_EQ(numberOf(result, "x"), 1.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "size"), 0.5);
  EXPECT_DOUBLE_EQ(numberOf(result, "stride"), 4.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "cells"), 24.0);
  EXPECT_EQ(textOf(result, "channel"), "value");
  EXPECT_EQ(textOf(result, "given_frame"), "odom");
  EXPECT_DOUBLE_EQ(numberOf(result, "given_slices"), 5.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "given_size"), 0.25);
}

// The twin of ObjectScript.BuiltCloudsAndGridsTakeTheResultStamp.
TEST(PythonObjectScript, BuiltCloudsAndGridsTakeTheResultStamp) {
  RunOptions options;
  options.result_stamp = 7;
  auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    cloud = pj.cloud.new(preset="xyz", frame_id="map").add(1, 2, 3).finish()
    grid = pj.voxels.new(frame_id="map", origin=(0, 0, 0), cell_size=1, dims=(1, 1, 1)).finish()
    return {"cloud": cloud, "grid": grid, "centroids": cloud.downsample(voxel_size=1),
            "kept": inputs["scan"].downsample(voxel_size=1)}
)PY",
      {objectInput("scan", makeLineCloud(2))},
      specs({"cloud:kPointCloud", "grid:kVoxelGrid", "centroids:kPointCloud", "kept:kPointCloud"}), "{}", options);
  ASSERT_NE(objectOf<sdk::PointCloud>(result, "cloud"), nullptr);
  EXPECT_EQ(objectOf<sdk::PointCloud>(result, "cloud")->timestamp_ns, 7);
  ASSERT_NE(objectOf<sdk::VoxelGrid>(result, "grid"), nullptr);
  EXPECT_EQ(objectOf<sdk::VoxelGrid>(result, "grid")->timestamp_ns, 7);
  EXPECT_EQ(objectOf<sdk::PointCloud>(result, "centroids")->timestamp_ns, 7);   // its input, the built cloud, carries 7
  EXPECT_EQ(objectOf<sdk::PointCloud>(result, "kept")->timestamp_ns, kSecond);  // the input's, not the result's
}

TEST(PythonObjectScript, RetainedBuilderOutputsAndTheirViewsKeepCounting) {
  const std::string error = errorOf(runSource(
      "def evaluate(inputs, params):\n    kept = []\n    for i in range(200):\n"
      "        kept.append(pj.scene.new(frame_id='map', id='e').text(position=(0, 0, 0), text='x' * 16384, "
      "font_size=1).finish())\n    return len(kept)\n",
      {}, {}, "{}", withNativeBudget(std::size_t{1} << 20)));
  EXPECT_NE(error.find("native memory limit"), std::string::npos) << error;
  // 100 views of about 8.5 KiB leave no room for a crop of 20,000 points, even after the collection it triggers.
  const std::string views = errorOf(runSource(
      "def evaluate(inputs, params):\n    kept = []\n    for i in range(100):\n"
      "        kept.append(pj.scene.new(frame_id='map', id='e').text(position=(0, 0, 0), text='x' * 8192, "
      "font_size=1).finish().entities[0].texts)\n"
      "    return inputs['cloud'].crop_box(min=(-1, -1, -1), max=(100000, 1, 1)).count() + len(kept)\n",
      {objectInput("cloud", makeLineCloud(20000))}, {}, "{}", withNativeBudget(std::size_t{1} << 20)));
  EXPECT_NE(views.find("native memory limit"), std::string::npos) << views;
}

// PY-05: an int64 field is a plain, exact Python int.
TEST(PythonObjectScript, Int64FieldsArePythonInts) {
  auto cloud = makeCloud({{0, 0, 0, 0}});
  cloud.timestamp_ns = 9'007'199'254'740'993LL;  // 2^53 + 1
  auto result = runSource(
      "def evaluate(inputs, params):\n    ts = inputs['cloud'].timestamp_ns\n"
      "    return {'exact': str(ts), 'is_int': 1 if isinstance(ts, int) and not isinstance(ts, bool) else 0}\n",
      {objectInput("cloud", cloud)}, specs({"exact:string", "is_int:number"}));
  EXPECT_EQ(textOf(result, "exact"), "9007199254740993");
  EXPECT_DOUBLE_EQ(numberOf(result, "is_int"), 1.0);
}

// PY-05: the raw buffer is reachable only through the native methods.
TEST(PythonObjectScript, BufferFieldIsNotExposed) {
  const std::string error = errorOf(runSource(
      "def evaluate(inputs, params):\n    return len(inputs['cloud'].data)\n", {objectInput("cloud", makeCloud({}))}));
  EXPECT_NE(error.find("raw buffer"), std::string::npos) << error;
}

TEST(PythonObjectScript, FieldAccessMatchesLuau) {
  sdk::CameraInfo camera;
  camera.K = {500, 0, 320, 0, 500, 240, 0, 0, 1};
  sdk::Image compressed_depth;
  compressed_depth.compressed_depth_min = 0.5F;
  auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    cloud = inputs["cloud"]
    fields = cloud.fields
    try:
        fields[4]
        past_the_end = 0
    except IndexError:
        past_the_end = 1
    return {
        "frame": cloud.frame_id, "first": fields[0].name, "last": fields[-1].name, "count": len(fields),
        "past_the_end": past_the_end, "dense": 1 if cloud.is_dense is True else 0,
        "names": ",".join(field.name for field in fields), "k": len(inputs["camera"].K), "cy": inputs["camera"].K[5],
        "absent": 1 if inputs["image"].compressed_depth_min is None else 0,
        "present": inputs["depth"].compressed_depth_min,
    }
)PY",
      {objectInput("cloud", makeCloud({{0, 0, 0, 0}})), objectInput("camera", camera),
       objectInput("image", sdk::Image{}), objectInput("depth", compressed_depth)},
      specs(
          {"frame:string", "first:string", "last:string", "count:number", "past_the_end:number", "dense:number",
           "names:string", "k:number", "cy:number", "absent:number", "present:number"}));
  EXPECT_EQ(textOf(result, "frame"), "base_link");
  EXPECT_EQ(textOf(result, "first"), "x");
  EXPECT_EQ(textOf(result, "last"), "intensity");
  EXPECT_DOUBLE_EQ(numberOf(result, "count"), 4.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "past_the_end"), 1.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "dense"), 1.0);
  EXPECT_EQ(textOf(result, "names"), "x,y,z,intensity");
  EXPECT_DOUBLE_EQ(numberOf(result, "k"), 9.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "cy"), 240.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "absent"), 1.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "present"), 0.5);
  for (const char* write : {"inputs['cloud'].frame_id = 'x'", "inputs['cloud'].fields[0].name = 'q'"}) {
    const std::string error = errorOf(runSource(
        std::string("def evaluate(inputs, params):\n    ") + write + "\n    return 1\n",
        {objectInput("cloud", makeCloud({}))}));
    EXPECT_NE(error.find("AttributeError"), std::string::npos) << write << " -> " << error;
  }
}

// ARCHITECTURE §6: a list is a lazy sequence, never materialized on a field read. 200,000 elements as Python objects
// would take megabytes; under a 1 MiB cap the run reads, indexes and iterates the list, each element made and freed.
TEST(PythonObjectScript, ListFieldIsReadLazily) {
  sdk::FrameTransforms transforms;
  transforms.transforms.resize(200'000);
  for (auto& transform : transforms.transforms) {
    transform.translation.x = 1.0;
  }
  RunOptions options;
  options.limits.mem_bytes = std::size_t{1} << 20;
  auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    transforms = inputs["tf"].transforms
    total = 0.0
    for transform in transforms:
        total += transform.translation.x
    return {"n": len(inputs["tf"].transforms), "last": transforms[199999].translation.x, "total": total}
)PY",
      {objectInput("tf", sdk::BuiltinObject(std::move(transforms)))},
      specs({"n:number", "last:number", "total:number"}), "{}", options);
  EXPECT_DOUBLE_EQ(numberOf(result, "n"), 200'000.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "last"), 1.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "total"), 200'000.0);
}

TEST(PythonObjectScript, CloudMethodsMatchLuau) {
  auto cloud = makeCloud({{1, 0, 0, 0.1f}, {kNaN, 1, 1, 0.5f}, {3, -2, 0, 0.9f}, {kNaN, kNaN, kNaN, 0}});
  cloud.frame_id = "lidar";
  constexpr double kHalfSqrt2 = 0.70710678118654752440;
  pj::tf::TransformBuffer buffer(pj::tf::TransformBuffer::kKeepAll);
  test::addTf(buffer, "map", "lidar", kSecond, kSecond, {10, 0, 0}, glm::dquat(kHalfSqrt2, 0, 0, kHalfSqrt2));
  auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    cloud = inputs["cloud"]
    box = cloud.bounds()
    n, sumx, last = 0, 0.0, -1
    for index, x, y, z in cloud.xyz_points():
        n += 1
        sumx += x
        last = index
    tf, why = pj.tf.lookup("map", cloud.frame_id)
    moved = cloud.transform(tf)
    first = next(iter(moved.xyz_points()))
    return {"min_y": box["min"][1], "max_x": box["max"][0], "finite": box["count"], "n": n, "sumx": sumx,
            "last": last, "kept": cloud.filter("intensity", ">", 0.3).count(),
            "not_equal": cloud.filter("intensity", "!=", 0.5).count(), "frame": moved.frame_id,
            "moved_x": first[1], "moved_y": first[2], "count": cloud.count(),
            "top": cloud.extreme_point("x", "max")["index"]}
)PY",
      {objectInput("cloud", cloud)},
      specs(
          {"min_y:number", "max_x:number", "finite:number", "n:number", "sumx:number", "last:number", "kept:number",
           "not_equal:number", "frame:string", "moved_x:number", "moved_y:number", "count:number", "top:number"}),
      "{}", hostAt(2 * kSecond, buffer.snapshot()));
  EXPECT_DOUBLE_EQ(numberOf(result, "min_y"), -2.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "max_x"), 3.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "finite"), 2.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "n"), 2.0);  // LUA-11: NaN records are skipped
  EXPECT_DOUBLE_EQ(numberOf(result, "sumx"), 4.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "last"), 2.0);  // 0-based, NaN records keep their index
  EXPECT_DOUBLE_EQ(numberOf(result, "kept"), 2.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "not_equal"), 3.0);
  EXPECT_EQ(textOf(result, "frame"), "map");  // LUA-13
  EXPECT_NEAR(numberOf(result, "moved_x"), 10.0, 1e-6);
  EXPECT_NEAR(numberOf(result, "moved_y"), 1.0, 1e-6);
  EXPECT_DOUBLE_EQ(numberOf(result, "count"), 4.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "top"), 2.0);
  const std::string error = errorOf(runSource(
      "def evaluate(inputs, params):\n    return inputs['cloud'].filter('x', '=>', 1).count()\n",
      {objectInput("cloud", cloud)}));
  EXPECT_NE(error.find("'=>'"), std::string::npos) << error;
  const std::string wrong_type = errorOf(runSource(
      "def evaluate(inputs, params):\n    return inputs['camera'].count()\n",
      {objectInput("camera", sdk::CameraInfo{})}));
  EXPECT_NE(wrong_type.find("point cloud"), std::string::npos) << wrong_type;
}

// BUD-15: retained native outputs share one budget.
TEST(PythonObjectScript, AggregateNativeCloudBudgetRejectsRetainedSecondOutput) {
  const std::vector<NamedValue> inputs{objectInput("cloud", makeLineCloud(100'000))};  // 1.6 MB
  constexpr std::size_t kBudget = 2'500'000;                                           // one crop fits, two do not
  constexpr const char* kBox = "min=(-1e9, -1e9, -1e9), max=(1e9, 1e9, 1e9)";
  EXPECT_DOUBLE_EQ(
      numberOf(runSource(
          std::string("def evaluate(inputs, params):\n    a = inputs['cloud'].crop_box(") + kBox +
              ")\n    return a.count()\n",
          inputs, {}, "{}", withNativeBudget(kBudget))),
      100'000.0);
  const std::string error = errorOf(runSource(
      std::string("def evaluate(inputs, params):\n    a = inputs['cloud'].crop_box(") + kBox +
          ")\n    b = inputs['cloud'].crop_box(" + kBox + ")\n    return a.count() + b.count()\n",
      inputs, {}, "{}", withNativeBudget(kBudget)));
  EXPECT_NE(error.find("native memory limit"), std::string::npos) << error;
}

// A dead crop in a reference cycle keeps its charge until the cycle is collected: a native operation that fails on
// the budget collects once and retries.
TEST(PythonObjectScript, ANativeOpRetriesOnceAfterAGarbageCollection) {
  EXPECT_DOUBLE_EQ(
      numberOf(runSource(
          R"PY(
def evaluate(inputs, params):
    n = 0
    for i in range(3):
        cycle = [inputs["cloud"].crop_box(min=(-1e9, -1e9, -1e9), max=(1e9, 1e9, 1e9))]
        cycle.append(cycle)
        n += cycle[0].count()
        cycle = None
    return n
)PY",
          {objectInput("cloud", makeLineCloud(100'000))}, {}, "{}", withNativeBudget(2'500'000))),
      300'000.0);
}

TEST(PythonObjectScript, TfLookupReturnsValueOrReasonTuple) {
  auto found = runSource(
      R"PY(
def evaluate(inputs, params):
    tf, why = pj.tf.lookup("map", "base_link")
    missing, reason = pj.tf.lookup("map", "unknown_frame")
    return {"tx": tf["translation"][0], "tz": tf["translation"][2], "rw": tf["rotation"][3],
            "target": tf["target_frame"], "why": "none" if why is None else why,
            "missing": 1 if missing is None else 0, "reason": reason,
            "frames": ",".join(sorted(pj.tf.frames()))}
)PY",
      {},
      specs(
          {"tx:number", "tz:number", "rw:number", "target:string", "why:string", "missing:number", "reason:string",
           "frames:string"}),
      "{}", hostAt(2 * kSecond));
  EXPECT_DOUBLE_EQ(numberOf(found, "tx"), 1.0);
  EXPECT_DOUBLE_EQ(numberOf(found, "tz"), 3.0);
  EXPECT_DOUBLE_EQ(numberOf(found, "rw"), 1.0);
  EXPECT_EQ(textOf(found, "target"), "map");
  EXPECT_EQ(textOf(found, "why"), "none");
  EXPECT_DOUBLE_EQ(numberOf(found, "missing"), 1.0);
  EXPECT_EQ(textOf(found, "reason"), "unknown source frame");
  EXPECT_EQ(textOf(found, "frames"), "base_link,map");
  auto none = runSource(
      "def evaluate(inputs, params):\n    tf, why = pj.tf.lookup('map', 'base_link')\n"
      "    return {'why': why, 'frames': len(pj.tf.frames()), 'now': str(pj.tf.now)}\n",
      {}, specs({"why:string", "frames:number", "now:string"}), "{}", hostAt(1'234'567'890'123'456'789LL, nullptr));
  EXPECT_EQ(textOf(none, "why"), "no tf snapshot");
  EXPECT_DOUBLE_EQ(numberOf(none, "frames"), 0.0);
  EXPECT_EQ(textOf(none, "now"), "1234567890123456789");  // exact past 2^53

  // GAP-29: at an object, the header clock at the object's own stamp on the dataset clock (its shift applied).
  constexpr int64_t kShift = 1000 * kSecond;
  pj::tf::TransformBuffer buffer(pj::tf::TransformBuffer::kKeepAll);
  test::addTf(buffer, "map", "lidar", 1100 * kSecond, 1300 * kSecond, {1, 0, 0});
  test::addTf(buffer, "map", "lidar", 1200 * kSecond, 1310 * kSecond, {2, 0, 0});
  auto cloud = makeCloud({{0, 0, 0, 0}});
  cloud.timestamp_ns = 100 * kSecond;
  auto placed = runSource(
      R"PY(
def evaluate(inputs, params):
    placed, why = pj.tf.lookup("map", "lidar", inputs["cloud"])
    queried, why = pj.tf.lookup("map", "lidar")
    at_time, why = pj.tf.lookup("map", "lidar", 1310 * 10**9)
    return {"stamp_s": inputs["cloud"].timestamp_ns / 1e9, "placed_x": placed["translation"][0],
            "queried_x": queried["translation"][0], "at_time_x": at_time["translation"][0]}
)PY",
      {objectInput("cloud", cloud, kShift)},
      specs({"stamp_s:number", "placed_x:number", "queried_x:number", "at_time_x:number"}), "{}",
      hostAt(1305 * kSecond, buffer.snapshot()));
  EXPECT_DOUBLE_EQ(numberOf(placed, "stamp_s"), 1100.0);
  EXPECT_DOUBLE_EQ(numberOf(placed, "placed_x"), 1.0);
  EXPECT_DOUBLE_EQ(numberOf(placed, "queried_x"), 1.0);  // entry clock at 1305 s: only the first sample arrived
  EXPECT_DOUBLE_EQ(numberOf(placed, "at_time_x"), 2.0);  // entry clock at 1310 s: both samples arrived
  const std::string range = errorOf(runSource(
      "def evaluate(inputs, params):\n    return pj.tf.lookup('map', 'base_link', 10 ** 30)\n", {}, {}, "{}",
      hostAt(0)));
  EXPECT_NE(range.find("time out of range"), std::string::npos) << range;
}

// ---- observation flags (CACHE-09) ------------------------------------------------------------------

ScriptBundle runObserving(const std::string& body, const RunOptions& host) {
  auto result = runSource("def evaluate(inputs, params):\n    " + body + "\n", {}, {}, "{}", host);
  EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
  return result ? *result : ScriptBundle{};
}

TEST(PythonObjectScript, ReadingTfNowMarksTheClockObserved) {
  const ScriptBundle bundle = runObserving("return pj.tf.now", hostAt(42));
  EXPECT_TRUE(bundle.observed_query_clock);
  EXPECT_DOUBLE_EQ(std::get<double>(output(bundle, "value")), 42.0);
}

TEST(PythonObjectScript, LookupWithoutTimeMarksTheClockObserved) {
  EXPECT_TRUE(
      runObserving("return 1 if pj.tf.lookup('map', 'base_link')[0] else 0", hostAt(2 * kSecond)).observed_query_clock);
}

TEST(PythonObjectScript, LookupWithExplicitTimeDoesNotMarkIt) {
  const ScriptBundle bundle =
      runObserving("return 1 if pj.tf.lookup('map', 'base_link', 2 * 10**9)[0] else 0", hostAt(5 * kSecond));
  EXPECT_FALSE(bundle.observed_query_clock);
  EXPECT_TRUE(bundle.observed_tf);
  EXPECT_DOUBLE_EQ(std::get<double>(output(bundle, "value")), 1.0);
}

TEST(PythonObjectScript, ScriptWithoutTfLeavesTheFlagFalse) {
  const ScriptBundle bundle = runObserving("return 1", hostAt(42));
  EXPECT_FALSE(bundle.observed_query_clock);
  EXPECT_FALSE(bundle.observed_tf);
}

TEST(PythonObjectScript, ObservationInsideABranchIsRecordedOnlyWhenTaken) {
  constexpr const char* kScript =
      "def evaluate(inputs, params):\n    if inputs['x'].count() > 2:\n        return pj.tf.now\n    return 0\n";
  auto one_point = runSource(kScript, {objectInput("x", makeLineCloud(1))}, {}, "{}", hostAt(99));
  EXPECT_DOUBLE_EQ(numberOf(one_point), 0.0);
  EXPECT_FALSE(one_point && one_point->observed_query_clock);
  auto three_points = runSource(kScript, {objectInput("x", makeLineCloud(3))}, {}, "{}", hostAt(99));
  EXPECT_DOUBLE_EQ(numberOf(three_points), 99.0);
  EXPECT_TRUE(three_points && three_points->observed_query_clock);
}

TEST(PythonObjectScript, TfLookupMarksTheTfObserved) {
  EXPECT_TRUE(runObserving("pj.tf.lookup('map', 'base_link', 2 * 10**9)\n    return 1", hostAt(0)).observed_tf);
  EXPECT_TRUE(runObserving("pj.tf.lookup('map', 'base_link')\n    return 1", hostAt(0)).observed_tf);
}

TEST(PythonObjectScript, TfFramesMarksTheTfObserved) {
  EXPECT_TRUE(runObserving("return len(pj.tf.frames())", hostAt(0)).observed_tf);
}

TEST(PythonObjectScript, QueryingAnEmptyTreeStillObservesIt) {
  EXPECT_TRUE(
      runObserving("pj.tf.lookup('map', 'base_link', 2 * 10**9)\n    return 1", hostAt(0, nullptr)).observed_tf);
  EXPECT_TRUE(runObserving("return len(pj.tf.frames())", hostAt(0, nullptr)).observed_tf);
}

TEST(PythonObjectScript, ClockAndPlainScriptsDoNotObserveTheTf) {
  EXPECT_FALSE(runObserving("return pj.tf.now", hostAt(0)).observed_tf);
  EXPECT_FALSE(runObserving("return 1", hostAt(0)).observed_tf);
}

TEST(PythonObjectScript, AFailedRunReportsWhatItObserved) {
  auto result = runSource(
      "def evaluate(inputs, params):\n    pj.tf.frames()\n    t = pj.tf.now\n    raise ValueError('late')\n", {}, {},
      "{}", hostAt(0));
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(result.error().observed_tf);
  EXPECT_TRUE(result.error().observed_query_clock);
}

// ---- images (MED-18's binder half, the twins of object_script_test.cpp's) ----------------------------------

TEST(PythonObjectScript, DecodedPixelsMatchLuau) {
  auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    image = inputs["image"]
    d, why = image.decode()
    if why is not None:
        raise ValueError("a decode that succeeds returns (pixels, None)")
    c = d.crop(1, 1, 2, 2)
    s = c.stats()
    return {"representation": image.representation(), "w": d.width, "h": d.height, "c": d.channels,
            "direct": d.pixel(1, 1), "from_crop": c.pixel(0, 0), "gray": d.to_gray().pixel(1, 1),
            "min": s["min"], "max": s["max"], "ann": d.threshold(">", 8)}
)PY",
      {imageInput("image", "/cam", 0)},
      specs(
          {"representation:string", "w:number", "h:number", "c:number", "direct:number", "from_crop:number",
           "gray:number", "min:number", "max:number", "ann:kImageAnnotations"}),
      "{}", withGradient());
  EXPECT_EQ(textOf(result, "representation"), "raw:mono8");
  EXPECT_DOUBLE_EQ(numberOf(result, "w"), 4.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "h"), 3.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "c"), 1.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "direct"), 5.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "from_crop"), 5.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "gray"), 5.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "min"), 5.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "max"), 10.0);
  const auto* annotations = objectOf<sdk::ImageAnnotations>(result, "ann");
  ASSERT_NE(annotations, nullptr);
  EXPECT_EQ(annotations->image_topic, "/cam");  // paired with the image input
  ASSERT_EQ(annotations->points.size(), 1u);
  EXPECT_EQ(annotations->points[0].points.size(), 3u);

  auto undecoded = runSource(
      "def evaluate(inputs, params):\n    d, why = inputs['image'].decode()\n    return pj.unavailable(why)\n",
      {imageInput("image", "/cam", 0)}, {}, "{}");
  EXPECT_EQ(reasonOf(undecoded, "value"), "no image decoder");
  for (const auto& [call, expected] : std::vector<std::pair<std::string, std::string>>{
           {"inputs['image'].decode()[0].crop(3, 0, 2, 1)", "outside"},
           {"inputs['image'].decode()[0].pixel(-1, 0)", "pixel integer"},
           {"inputs['image'].decode()[0].threshold('=>', 1)", "'=>'"},
           {"inputs['image'].to_point_cloud(inputs['image'])", "camera info"},
           {"inputs['camera'].decode()", "image or depth image"}}) {
    const std::string error = errorOf(runSource(
        "def evaluate(inputs, params):\n    return " + call + "\n",
        {imageInput("image", "/cam", 0), objectInput("camera", sdk::CameraInfo{})}, {}, "{}", withGradient()));
    EXPECT_NE(error.find(expected), std::string::npos) << call << " -> " << error;
  }
}

TEST(PythonObjectScript, TooManyPointsIsUnavailable) {
  RunOptions options;
  options.image_decoder = std::make_shared<FakeImageDecoder>(1000, 600, 100.0f);
  options.native_budget_bytes = std::size_t{8} << 20;  // 2.4 MB of pixels fit; 600k points (9.6 MB) do not
  auto result = runSource(
      "def evaluate(inputs, params):\n    return {'ann': inputs['image'].decode()[0].threshold('>', 50)}\n",
      {imageInput("image", "/cam", 0)}, specs({"ann:kImageAnnotations"}), "{}", options);
  EXPECT_NE(reasonOf(result, "ann").find("native memory limit"), std::string::npos) << reasonOf(result, "ann");
}

TEST(PythonObjectScript, ParserDepthImageSupportsProjectionAndRejectsOrdinaryImages) {
  sdk::CameraInfo camera;
  camera.K = {2, 0, 0, 0, 2, 0, 0, 0, 1};
  camera.frame_id = "optical";
  for (const char* encoding : {"16UC1", "mono16", "32FC1", "mono8"}) {
    SCOPED_TRACE(encoding);
    auto decoder = std::make_shared<FakeImageDecoder>(2, 1, 1000.0f);
    RunOptions options;
    options.image_decoder = decoder;
    auto result = runSource(
        "def evaluate(inputs, params):\n    first = inputs['image'].to_point_cloud(inputs['camera'])\n"
        "    return {'cloud': inputs['image'].to_point_cloud(inputs['camera'], step=1)}\n",
        {imageInput("image", "/depth", 42, encoding), objectInput("camera", camera)}, specs({"cloud:kPointCloud"}),
        "{}", options);
    EXPECT_EQ(decoder->calls, 1);
    if (std::string_view(encoding) == "mono8") {
      EXPECT_EQ(reasonOf(result, "cloud"), "unsupported depth encoding: mono8");
      continue;
    }
    const auto* cloud = objectOf<sdk::PointCloud>(result, "cloud");
    ASSERT_NE(cloud, nullptr);
    EXPECT_EQ(cloud->timestamp_ns, 42);
    EXPECT_EQ(cloud->width, 2u);
    EXPECT_EQ(cloud->frame_id, "optical");
    float z = 0;
    std::memcpy(&z, cloud->data.data() + 8, sizeof(z));
    EXPECT_EQ(z, std::string_view(encoding) == "32FC1" ? 1000.0f : 1.0f);
  }
  sdk::DepthImage distorted;
  distorted.distortion_model = "plumb_bob";
  RunOptions options;
  options.image_decoder = std::make_shared<FakeImageDecoder>(2, 1, 1000.0f);
  auto result = runSource(
      "def evaluate(inputs, params):\n    return {'cloud': inputs['depth'].to_point_cloud(inputs['camera'])}\n",
      {objectInput("depth", distorted), objectInput("camera", camera)}, specs({"cloud:kPointCloud"}), "{}", options);
  EXPECT_EQ(reasonOf(result, "cloud"), "depth must be rectified before unprojection");
}

// GAP-25: every operation that walks the data runs without the GIL. The probes are the run's cancel check, which the
// cloud and image operations poll, and the decoder.
class GilProbeDecoder final : public IImageDecoder {
 public:
  mutable std::atomic<int> held{0};
  mutable std::atomic<int> calls{0};
  /// 300 x 300 pixels: RGB for an rgb8 image (so to_gray converts), else one channel.
  Expected<DecodedPixels> decode(
      const sdk::Image& image, const std::shared_ptr<native::MemoryBudget>& /*budget*/) const override {
    ++calls;
    held += PyGILState_Check();
    DecodedPixels pixels;
    pixels.width = 300;
    pixels.height = 300;
    pixels.channels = image.encoding == "rgb8" ? 3 : 1;
    pixels.samples = std::make_shared<const std::vector<float>>(std::size_t{300} * 300 * pixels.channels, 10.0f);
    return pixels;
  }
};

TEST(PythonObjectScript, NativeOpsReleaseTheGil) {
  auto decoder = std::make_shared<GilProbeDecoder>();
  std::atomic<int> polls{0};
  std::atomic<int> held{0};
  RunOptions options = hostAt(2 * kSecond);
  options.image_decoder = decoder;
  options.cancelled = [&] {
    ++polls;
    held += PyGILState_Check();
    return false;
  };
  sdk::CameraInfo camera;
  camera.K = {2, 0, 0, 0, 2, 0, 0, 0, 1};
  camera.width = 300;
  camera.height = 300;
  const std::vector<std::string> calls{
      "c.crop_box(min=(-1, -1, -1), max=(1e9, 1, 1)).count()",
      "c.filter('x', '>', 5).count()",
      "c.transform(pj.tf.lookup('map', 'base_link')[0]).count()",
      "d.to_gray().width",
      "d.crop(0, 0, 300, 300).width",
      "d.threshold('>', 5).count()",
      "d.stats()['max']",
      "inputs['depth'].to_point_cloud(inputs['camera']).count()"};
  for (const std::string& call : calls) {
    polls = 0;
    auto result = runSource(
        "def evaluate(inputs, params):\n    c = inputs['cloud']\n    d = inputs['image'].decode()[0]\n    return " +
            call + "\n",
        {objectInput("cloud", makeLineCloud(200'000)), imageInput("image", "/cam", 0, "rgb8"),
         imageInput("depth", "/depth", 0, "16UC1"), objectInput("camera", camera)},
        {}, "{}", options);
    EXPECT_TRUE(result.has_value()) << call << " -> " << result.error().message;
    EXPECT_GT(polls.load(), 2) << call << ": the operation polled the cancel check";
  }
  EXPECT_EQ(held.load(), 0) << "an operation polled its cancel check holding the GIL";
  EXPECT_GT(decoder->calls.load(), 0);
  EXPECT_EQ(decoder->held.load(), 0) << "the decoder ran holding the GIL";
}

// BUD-18: the watchdog never interrupts a native call. The crop's cancel check, polled every 65536 records, observes
// the native call: its first poll sleeps past the 1 ms deadline, so the watchdog fires while the crop runs; the crop
// still polls as often as an untimed one (it ran to its end), and the timeout lands after it.
TEST(PythonObjectScript, WatchdogDoesNotInterruptANativeCall) {
  const std::vector<NamedValue> inputs{objectInput("cloud", makeLineCloud(1'000'000))};
  constexpr const char* kScript =
      "def evaluate(inputs, params):\n"
      "    return inputs['cloud'].crop_box(min=(-0.5, -0.5, -0.5), max=(0.5, 0.5, 0.5)).count()\n";
  int polls = 0;
  RunOptions options;
  options.cancelled = [&polls] {
    if (++polls == 2) {  // the first poll inside the crop
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return false;
  };
  EXPECT_DOUBLE_EQ(numberOf(runSource(kScript, inputs, {}, "{}", options)), 1.0);
  const int untimed_polls = polls;
  ASSERT_GT(untimed_polls, 4) << "the crop polls inside the native call";
  polls = 0;
  options.limits.python_watchdog_millis = 1;
  const std::string error = errorOf(runSource(kScript, inputs, {}, "{}", options));
  EXPECT_NE(error.find("TimeoutError"), std::string::npos) << error;
  // A failed run polls after it too (a cancellation outranks its error); every poll of the crop's happened.
  EXPECT_EQ(polls, untimed_polls) << "the crop ran to its end before the timeout was delivered";
}

// PY-05 and the sandbox of slice 10a: no proxy has a name beyond its documented ones (no conduit or buffer to a raw
// pointer), takes an attribute (a shared one would carry state into the next run) or exposes a buffer.
TEST(PythonObjectScript, ProxiesExposeOnlyTheirDocumentedNames) {
  auto result = runSource(
      R"PY(
def closed(x):
    try:
        x.leak = 1
        return "settable"
    except AttributeError:
        pass
    try:
        memoryview(x)
        return "buffer"
    except TypeError:
        pass
    return ",".join(sorted(name for name in dir(x) if not name.startswith("__")))

def evaluate(inputs, params):
    cloud = inputs["cloud"]
    return {"object": closed(cloud), "struct": closed(cloud.fields[0]), "list": closed(cloud.fields),
            "points": closed(cloud.xyz_points()), "decoded": closed(inputs["image"].decode()[0]),
            "scene": closed(pj.scene.new(frame_id="m", id="e")),
            "annotations": closed(pj.annotations.new(image_topic="/cam")), "scene_new": closed(pj.scene),
            "annotations_new": closed(pj.annotations), "tf": closed(pj.tf),
            "cloud": closed(pj.cloud.new(preset="xyz", frame_id="m")), "cloud_new": closed(pj.cloud),
            "voxels": closed(pj.voxels.new(frame_id="m", origin=(0, 0, 0), cell_size=1, dims=(1, 1, 1))),
            "voxels_new": closed(pj.voxels)}
)PY",
      {objectInput("cloud", makeCloud({{0, 0, 0, 0}})), imageInput("image", "/cam", 0)},
      specs(
          {"object:string", "struct:string", "list:string", "points:string", "decoded:string", "scene:string",
           "annotations:string", "scene_new:string", "annotations_new:string", "tf:string", "cloud:string",
           "cloud_new:string", "voxels:string", "voxels_new:string"}),
      "{}", withGradient());
  EXPECT_EQ(
      textOf(result, "object"),
      "bounds,count,crop_box,decode,downsample,extreme_point,filter,frame_at,representation,to_point_cloud,transform,"
      "xyz_points");
  EXPECT_EQ(textOf(result, "struct"), "");
  EXPECT_EQ(textOf(result, "list"), "");
  EXPECT_EQ(textOf(result, "points"), "");
  EXPECT_EQ(textOf(result, "decoded"), "channels,crop,height,pixel,stats,threshold,to_gray,width");
  EXPECT_EQ(textOf(result, "scene"), "cube,finish,line_strip,sphere,text");
  EXPECT_EQ(textOf(result, "annotations"), "circle,finish,points,text");
  EXPECT_EQ(textOf(result, "scene_new"), "new");
  EXPECT_EQ(textOf(result, "annotations_new"), "new");
  EXPECT_EQ(textOf(result, "tf"), "frames,lookup,now");
  EXPECT_EQ(textOf(result, "cloud"), "add,add_points,finish");
  EXPECT_EQ(textOf(result, "cloud_new"), "new");
  EXPECT_EQ(textOf(result, "voxels"), "finish,set");
  EXPECT_EQ(textOf(result, "voxels_new"), "new");
  EXPECT_FALSE(compile("def evaluate(inputs, params):\n    return inputs['cloud'].xyz_points().__class__\n"));
}

// An object a script leaves behind (on a class its module copy shares, a known opening of the trust model) raises when
// called outside a run, instead of reaching the dead run's limits and media.
TEST(PythonObjectScript, AnObjectKeptPastItsRunRaises) {
  EXPECT_DOUBLE_EQ(
      numberOf(runSource(
          "import statistics\ndef evaluate(inputs, params):\n    statistics.NormalDist.kept = inputs['cloud']\n"
          "    return 1\n",
          {objectInput("cloud", makeLineCloud(10))})),
      1.0);
  py::gil_scoped_acquire gil;
  py::object normal_dist = py::module_::import("statistics").attr("NormalDist");
  py::object kept = normal_dist.attr("kept");
  py::delattr(normal_dist, "kept");
  EXPECT_DOUBLE_EQ(kept.attr("count")().cast<double>(), 10.0) << "a read needs no run";
  try {
    kept.attr("crop_box")(py::make_tuple(0, 0, 0), py::make_tuple(1, 1, 1));
    ADD_FAILURE() << "a native operation ran outside a run";
  } catch (const py::error_already_set& error) {
    EXPECT_NE(std::string(error.what()).find("only inside a run"), std::string::npos) << error.what();
  }
}

/// Runs `compiled` under `cap` bytes of Python allocations; true when it succeeds, else expects the memory limit.
bool runsUnder(
    std::size_t cap, const CompiledPythonScript& compiled, const std::vector<NamedValue>& inputs, RunOptions options) {
  options.limits.mem_bytes = cap;
  auto result = run(compiled, inputs, {}, "{}", options);
  if (!result) {
    EXPECT_NE(result.error().message.find("script memory limit"), std::string::npos)
        << "cap " << cap << ": " << result.error().message;
  }
  return result.has_value();
}

// A cap the host meets while it wraps the inputs (a large block refused, or small ones past twice the cap) fails the
// run before any script code runs. Each cap, in steps smaller than any allocation, is met at a different one of 400
// inputs.
TEST(PythonObjectScript, InputsTheCapRefusesFailTheRunBeforeAnyScriptRuns) {
  const std::vector<NamedValue> inputs(400, objectInput("cloud", makeCloud({{0, 0, 0, 0}})));
  std::vector<NamedValue> named = inputs;
  for (std::size_t index = 0; index < named.size(); ++index) {
    named[index].name = "cloud" + std::to_string(index);
  }
  auto compiled = compile("def evaluate(inputs, params):\n    return 1\n");
  ASSERT_TRUE(compiled) << compiled.error();
  for (std::size_t cap = 8; cap < 16384; cap += 8) {
    EXPECT_FALSE(runsUnder(cap, *compiled, named, {})) << cap;
  }
}

// Wherever the cap meets the binder inside `evaluate` (views, dicts and tuples a method returns, a builder), the run
// fails on the memory limit, never a crash: the caps just below the smallest that fits, in steps smaller than any
// allocation, all on one compiled script (ACapRefusingAFunctionLeavesTheCompiledScriptIntact).
TEST(PythonObjectScript, AllocationsTheCapRefusesFailTheRun) {
  constexpr const char* kScript = R"PY(
def evaluate(inputs, params):
    cloud = inputs["cloud"]
    kept = []
    for i in range(8):
        kept.append((cloud.fields[0], cloud.fields[0].name, cloud.bounds(), cloud.extreme_point("x", "max"),
                     pj.scene.new(frame_id=cloud.frame_id, id="e").sphere(center=(0, 0, 0), diameter=1),
                     pj.tf.lookup("map", "base_link"), next(iter(cloud.xyz_points()))))
    return {"scene": kept[0][4].finish(), "n": len(kept)}
)PY";
  const std::vector<NamedValue> inputs{objectInput("cloud", makeCloud({{0, 0, 0, 0}}))};
  const RunOptions options = hostAt(2 * kSecond);
  auto compiled = compile(kScript);
  ASSERT_TRUE(compiled) << compiled.error();
  std::size_t fails = 0;
  std::size_t fits = std::size_t{1} << 20;
  ASSERT_TRUE(runsUnder(fits, *compiled, inputs, options));
  while (fits - fails > 8) {
    const std::size_t middle = (fails + fits) / 2 / 8 * 8;
    (runsUnder(middle, *compiled, inputs, options) ? fits : fails) = middle;
  }
  for (std::size_t cap = fits - 2048; cap < fits; cap += 8) {
    EXPECT_FALSE(runsUnder(cap, *compiled, inputs, options)) << cap;
  }
}

// Every allocation of an image method's return (the decoded pixels, a decode's `(None, reason)`, a pixel or stats dict,
// a crop, a threshold's annotations, an unavailable marker) that the cap refuses fails the run on the memory limit.
TEST(PythonObjectScript, ImageAllocationsTheCapRefusesFailTheRun) {
  constexpr const char* kDecoded = R"PY(
def evaluate(inputs, params):
    kept = []
    for i in range(4):
        d = inputs["image"].decode()[0]
        kept.append((d, d.pixel(1, 1), d.stats(), d.crop(0, 0, 2, 2), d.to_gray(), d.threshold(">", 8),
                     inputs["image"].representation(), inputs["image"].to_point_cloud(inputs["camera"]),
                     inputs["rgb"].decode()[0].pixel(1, 1)))
    return len(kept)
)PY";
  constexpr const char* kUndecoded = R"PY(
def evaluate(inputs, params):
    kept = [inputs["image"].decode() for i in range(8)]
    return len(kept)
)PY";
  const std::vector<NamedValue> inputs{
      imageInput("image", "/cam", 0), imageInput("rgb", "/rgb", 0, "rgb8"), objectInput("camera", sdk::CameraInfo{})};
  RunOptions decoding;
  decoding.image_decoder = std::make_shared<GilProbeDecoder>();  // RGB for the rgb8 image: pixel() returns a dict
  for (const auto& [source, options] : {std::pair{kDecoded, decoding}, std::pair{kUndecoded, RunOptions{}}}) {
    auto compiled = compile(source);
    ASSERT_TRUE(compiled) << compiled.error();
    std::size_t fails = 0;
    std::size_t fits = std::size_t{1} << 20;
    ASSERT_TRUE(runsUnder(fits, *compiled, inputs, options));
    while (fits - fails > 8) {
      const std::size_t middle = (fails + fits) / 2 / 8 * 8;
      (runsUnder(middle, *compiled, inputs, options) ? fits : fails) = middle;
    }
    for (std::size_t cap = fits - 4096; cap < fits; cap += 8) {
      EXPECT_FALSE(runsUnder(cap, *compiled, inputs, options)) << cap;
    }
  }
}

// A conversion's script code (`__float__`) that empties the list being read cannot make the binder read freed storage:
// the binder reads a snapshot of every coordinate and colour sequence.
TEST(PythonObjectScript, ConversionsCannotEmptyTheSequenceBeingRead) {
  for (const char* call :
       {"b.sphere(center=items, diameter=1)", "b.sphere(center=(0, 0, 0), diameter=1, color=items)",
        "b.line_strip(points=[items, (1, 1, 1)], thickness=1)"}) {
    EXPECT_DOUBLE_EQ(
        numberOf(runSource(
            std::string(
                "class Trap:\n    def __float__(self):\n        items.clear()\n        return 0.0\n"
                "items = [Trap(), 0, 0]\n"
                "def evaluate(inputs, params):\n    b = pj.scene.new(frame_id='m', id='e')\n    ") +
            call + "\n    return 1\n")),
        1.0)
        << call;
  }
}

// A translation component whose `__float__` deletes the dict's rotation cannot leave the binder reading it freed: the
// binder holds both values first (it reads the identity rotation, not what the freed tuple's storage holds next).
TEST(PythonObjectScript, ATransformDictEditedByAConversionIsReadWhole) {
  auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    tf, why = pj.tf.lookup("map", "base_link")
    class Trap:
        def __float__(self):
            del tf["rotation"]
            reused = [(9.0, 9.0, 9.0, 9.0 + i) for i in range(100)]
            return 0.0
    tf["translation"] = (Trap(), 0, 0)
    for index, x, y, z in inputs["cloud"].transform(tf).xyz_points():
        return {"x": x, "y": y}
)PY",
      {objectInput("cloud", makeCloud({{1, 0, 0, 0}}))}, specs({"x:number", "y:number"}), "{}", hostAt(2 * kSecond));
  EXPECT_DOUBLE_EQ(numberOf(result, "x"), 1.0);
  EXPECT_DOUBLE_EQ(numberOf(result, "y"), 0.0);
}

// An object a script keeps past its run holds the input's keepalive (the library of the parser that made it), so its
// payload's deleter, plugin code, runs before the library can unload.
TEST(PythonObjectScript, AnObjectKeptPastItsRunKeepsItsLibraryLoaded) {
  auto library = std::make_shared<int>(1);
  const std::weak_ptr<int> loaded = library;
  int deletes_after_unload = 0;
  {
    sdk::PointCloud cloud = makeCloud({{0, 0, 0, 0}, {kNaN, 0, 0, 0}});
    const sdk::BufferAnchor bytes = cloud.anchor;
    cloud.anchor = sdk::BufferAnchor(bytes.get(), [bytes, loaded, &deletes_after_unload](const void*) {
      deletes_after_unload += loaded.expired() ? 1 : 0;
    });
    NamedValue input = objectInput("cloud", cloud);
    input.keepalive = library;
    EXPECT_DOUBLE_EQ(
        numberOf(runSource(
            "import statistics\ndef evaluate(inputs, params):\n    cloud = inputs['cloud']\n"
            "    statistics.NormalDist.kept = (cloud, cloud.xyz_points(), cloud.fields, cloud.fields[0])\n"
            "    return 1\n",
            {input})),
        1.0);
  }
  library.reset();  // the caller's keepalive: the library would unload here, if nothing else held it
  {
    py::gil_scoped_acquire gil;
    py::delattr(py::module_::import("statistics").attr("NormalDist"), "kept");
  }
  EXPECT_EQ(deletes_after_unload, 0);
  EXPECT_TRUE(loaded.expired()) << "the proxies released the keepalive with the object";
}

// F1b: a list walk reuses its element views, but a run leaves none behind: the free list is empty after it, so no
// recycled block keeps the run's allocation budget alive past it, and nothing keeps the input's keepalive.
TEST(PythonObjectScript, ARunLeavesNoRecycledViewBehind) {
  auto library = std::make_shared<int>(1);
  const std::weak_ptr<int> loaded = library;
  {
    sdk::FrameTransforms transforms;
    transforms.transforms.resize(200);
    for (auto& transform : transforms.transforms) {
      transform.translation.x = 1.0;
    }
    NamedValue input = objectInput("tf", sdk::BuiltinObject(std::move(transforms)));
    input.keepalive = library;
    EXPECT_DOUBLE_EQ(
        numberOf(runSource(
            "def evaluate(inputs, params):\n    s = 0.0\n    for t in inputs['tf'].transforms:\n"
            "        s += t.translation.x\n    return s\n",
            {input})),
        200.0);
  }
  {
    py::gil_scoped_acquire gil;
    EXPECT_EQ(detail::recycledViewCount(), 0u) << "the run emptied the free list at its end";
  }
  library.reset();
  EXPECT_TRUE(loaded.expired());
}

// A view freed in a run is kept for reuse with nothing of its own: not its object's keepalive, and once the run ends
// not its block, which stays charged to the budget of the run that allocated it until it is freed. Outside a run a
// view is freed.
TEST(PythonObjectBinder, ARecycledViewCarriesNothingOfTheRunThatMadeIt) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  (void)detail::pjNamespaces();  // registers the binder's types
  const auto list_of = [](const char* parent, std::shared_ptr<const void> keepalive) -> py::object {
    sdk::FrameTransforms transforms;
    transforms.transforms.resize(2);
    transforms.transforms[0].parent_frame_id = parent;
    return detail::newObject(sdk::BuiltinObject(std::move(transforms)), std::move(keepalive)).attr("transforms");
  };
  const auto element = [](const py::object& list) { return detail::checked(PySequence_GetItem(list.ptr(), 0)); };
  auto library = std::make_shared<int>(1);
  const std::weak_ptr<int> loaded = library;
  py::object first = list_of("first", std::move(library));
  const py::object second = list_of("second", nullptr);
  const RunOptions options;
  ScriptMedia media(nullptr, native::NativeLimits{}, 0);

  auto budget = std::make_shared<native::MemoryBudget>(std::size_t{1} << 20);
  const std::weak_ptr<native::MemoryBudget> spent = budget;
  {
    detail::ScopedAllocationCap cap(std::move(budget));
    detail::PythonRun run(options, media);
    PyObject* const block = element(first).ptr();  // allocated under this run's cap, then freed
    EXPECT_EQ(detail::recycledViewCount(), 1u) << "a view freed in a run is kept for reuse";
    first = py::none();
    EXPECT_TRUE(loaded.expired()) << "a recycled view keeps no keepalive";
    const py::object reused = element(second);
    EXPECT_EQ(reused.ptr(), block);
    EXPECT_EQ(detail::recycledViewCount(), 0u);
    EXPECT_EQ(reused.attr("parent_frame_id").cast<std::string>(), "second");
  }
  ASSERT_FALSE(spent.expired()) << "a recycled block keeps the budget of the run that allocated it";
  EXPECT_GT(spent.lock()->used(), 0u) << "and stays charged to it until it is freed";
  {
    detail::PythonRun run(options, media);
    (void)element(second);
    EXPECT_EQ(detail::recycledViewCount(), 1u);
    detail::releaseRecycledViews();  // what a run does at its end
    EXPECT_EQ(detail::recycledViewCount(), 0u);
  }
  if (const auto budget_left = spent.lock()) {  // a budget with nothing charged goes at a later sweep
    EXPECT_EQ(budget_left->used(), 0u) << "the freed block gave its charge back";
  }
  (void)element(second);
  EXPECT_EQ(detail::recycledViewCount(), 0u) << "outside a run a view is freed, not recycled";
}

// A result that returns an input holds the input's keepalive in its output, and releases the value first: the payload's
// deleter, plugin code, runs while the library is still loaded.
TEST(PythonObjectScript, AReturnedInputReleasesItsValueBeforeItsKeepalive) {
  auto library = std::make_shared<int>(1);
  const std::weak_ptr<int> loaded = library;
  int deletes_after_unload = 0;
  std::optional<ScriptBundle> kept;
  {
    sdk::PointCloud cloud = makeCloud({{0, 0, 0, 0}});
    const sdk::BufferAnchor bytes = cloud.anchor;
    cloud.anchor = sdk::BufferAnchor(bytes.get(), [bytes, loaded, &deletes_after_unload](const void*) {
      deletes_after_unload += loaded.expired() ? 1 : 0;
    });
    NamedValue input = objectInput("cloud", cloud);
    input.keepalive = library;
    auto result = runSource("def evaluate(inputs, params):\n    return inputs['cloud']\n", {input});
    ASSERT_TRUE(result) << result.error().message;
    kept = std::move(*result);
  }
  library.reset();  // the caller's keepalive: only the result holds the library now
  kept.reset();
  EXPECT_EQ(deletes_after_unload, 0);
  EXPECT_TRUE(loaded.expired());
}

// GAP-25 and the busy guard: `xyz_points()` scans a run of non-finite records without the GIL, so another thread runs
// Python meanwhile, and the iterator refuses to advance from it while it scans. Whichever thread reaches `next` second
// (the lane, or this one through the iterator the script stashed) sees "already advancing".
TEST(PythonObjectScript, APointScanReleasesTheGilAndRefusesAConcurrentAdvance) {
  std::atomic<bool> started{false};
  {
    detail::ensureInterpreter();
    py::gil_scoped_acquire gil;
    py::module_ fake = py::module_::import("types").attr("ModuleType")("math.pj_fake_mark");
    fake.attr("mark") = py::cpp_function([&started] { started = true; });
    py::module_::import("sys").attr("modules")["math.pj_fake_mark"] = fake;
  }
  const std::vector<NamedValue> inputs{
      objectInput("cloud", makeCloud(std::vector<std::array<float, 4>>(10'000'000, {kNaN, kNaN, kNaN, 0})))};
  std::string lane_saw;
  std::thread lane([&] {
    auto result = runSource(
        "import statistics\nfrom math.pj_fake_mark import mark\ndef evaluate(inputs, params):\n"
        "    it = inputs['cloud'].xyz_points()\n    statistics.NormalDist.it = it\n    mark()\n"
        "    try:\n        next(it, None)\n    except RuntimeError as error:\n        return str(error)\n"
        "    return 'scanned'\n",
        inputs);
    lane_saw = result ? std::get<std::string>(result->outputs.front().value) : result.error().message;
  });
  while (!started) {
    std::this_thread::yield();
  }
  std::string this_saw;
  {
    py::gil_scoped_acquire gil;
    try {
      py::module_::import("builtins")
          .attr("next")(py::module_::import("statistics").attr("NormalDist").attr("it"), py::none());
      this_saw = "scanned";
    } catch (const py::error_already_set& error) {
      this_saw = error.what();
    }
  }
  lane.join();
  const bool lane_refused = lane_saw.find("already advancing") != std::string::npos;
  const bool this_refused = this_saw.find("already advancing") != std::string::npos;
  EXPECT_NE(lane_refused, this_refused) << "lane: " << lane_saw << "; this thread: " << this_saw;
  py::gil_scoped_acquire gil;
  py::delattr(py::module_::import("statistics").attr("NormalDist"), "it");
  py::module_::import("sys").attr("modules").attr("pop")("math.pj_fake_mark");
}

// A builder belongs to the run that made it: one a script keeps cannot grow in a later run, on that run's budget.
TEST(PythonObjectScript, ABuilderWorksOnlyInTheRunThatMadeIt) {
  EXPECT_DOUBLE_EQ(
      numberOf(runSource(
          "import statistics\ndef evaluate(inputs, params):\n"
          "    statistics.NormalDist.kept = pj.scene.new(frame_id='m', id='e')\n    return 1\n")),
      1.0);
  const std::string error = errorOf(runSource(
      "import statistics\ndef evaluate(inputs, params):\n"
      "    statistics.NormalDist.kept.sphere(center=(0, 0, 0), diameter=1)\n    return 1\n",
      {}, {}, "{}", withNativeBudget(1)));
  EXPECT_NE(error.find("the run that made it"), std::string::npos) << error;
  py::gil_scoped_acquire gil;
  py::delattr(py::module_::import("statistics").attr("NormalDist"), "kept");
}

// A builder's output keeps its native charge in the result: results the caller keeps, of runs sharing one ledger,
// count against it until they are dropped.
TEST(PythonObjectScript, RetainedResultsKeepTheirNativeCharge) {
  RunOptions options;
  options.native_budget = makeNativeBudget(std::size_t{1} << 20);
  std::vector<ScriptBundle> kept;
  std::string error;
  for (int call = 0; call < 8 && error.empty(); ++call) {
    auto result = runSource(
        "def evaluate(inputs, params):\n    return pj.scene.new(frame_id='m', id='e')"
        ".text(position=(0, 0, 0), text='x' * 200000, font_size=1).finish()\n",
        {}, {}, "{}", options);
    if (result) {
      kept.push_back(std::move(*result));
    } else {
      error = result.error().message;
    }
  }
  EXPECT_EQ(kept.size(), 5u);
  EXPECT_NE(error.find("native memory limit"), std::string::npos) << error;
  kept.clear();
  EXPECT_EQ(options.native_budget->used(), 0u);
}

// The points a builder converts are charged before they are allocated: a million points against a 1 MiB budget fail on
// the budget, before the conversion reaches the bad last entry.
TEST(PythonObjectScript, PointConversionIsChargedBeforeItAllocates) {
  const std::string error = errorOf(runSource(
      "def evaluate(inputs, params):\n"
      "    pj.scene.new(frame_id='m', id='e').line_strip(points=[(0, 0, 0)] * 999999 + ['bad'], thickness=1)\n"
      "    return 1\n",
      {}, {}, "{}", withNativeBudget(std::size_t{1} << 20)));
  EXPECT_NE(error.find("native memory limit"), std::string::npos) << error;
}

// A cancel flag the watchdog polls stops pure Python at its next bytecode with the cancellation's error, also inside a
// loop that catches every Exception (it raises KeyboardInterrupt); the thread then runs the next script as usual. The
// deadline is 600 s, so a missed poll hangs the test instead of passing on a timeout. The flag goes up at the
// runner's pre-run poll, so the script always starts with it set.
TEST(PythonObjectScript, ACancelFlagInterruptsAPurePythonLoop) {
  for (const char* loop :
       {"    while True:\n        pass\n",
        "    while True:\n        try:\n            while True:\n                pass\n"
        "        except Exception:\n            pass\n"}) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    auto polls = std::make_shared<int>(0);
    RunOptions options;
    options.limits.python_watchdog_millis = 600'000;
    options.cancel_flags = {flag};
    options.cancelled = [flag, polls] {
      if ((*polls)++ == 0) {
        flag->store(true);  // the pre-run poll answers no, then the run is cancelled
        return false;
      }
      return flag->load();
    };
    const auto looping =
        runSource(std::string("def evaluate(inputs, params):\n") + loop, {}, {number("x")}, "{}", options);
    EXPECT_EQ(errorOf(looping), "script cancelled") << loop;
  }
  EXPECT_DOUBLE_EQ(numberOf(runSource("def evaluate(inputs, params):\n    return 2\n")), 2.0);
}

// The runtime's compile (`compileOnDemand`, which a recipe's create, validate and restore call on the GUI thread) never
// takes the GIL: while a script stuck in a builtin C loop holds it (a held GIL here) it returns at once, and so does a
// compiled script's last release (posted to the helper, where it waits for the GIL). The script compiles at its first
// run, once: a compile error is that run's error, with the message an eager compile reports, and every later run
// returns it without compiling again. An unbounded wait would hang the test; the elapsed-time bound is lax.
TEST(PythonObjectScript, AnOnDemandCompileNeverWaitsForTheGil) {
  std::optional<Expected<CompiledPythonScript>> kept = compile("def evaluate(inputs, params):\n    return 1\n");
  ASSERT_TRUE(kept->has_value()) << kept->error();
  const int before = ::PJ::test::pythonCompileEvents();
  PyObject* const code = detail::codeObjectOf(**kept);
  PyObject* weak = nullptr;
  {
    py::gil_scoped_acquire gil;
    weak = PyWeakref_NewRef(code, nullptr);
    ASSERT_NE(weak, nullptr);
  }
  constexpr const char* kBroken = "def evaluate(inputs, params:\n    return 2\n";
  std::optional<Expected<CompiledOnDemandScript>> good;
  std::optional<Expected<CompiledOnDemandScript>> broken;
  {
    const ::PJ::test::PythonGilHold hold;
    const auto begin = std::chrono::steady_clock::now();
    kept.reset();  // the last copy: its release waits for the GIL on the helper
    good = compileOnDemand("python", "def evaluate(inputs, params):\n    return 3\n");
    broken = compileOnDemand("python", kBroken);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(250)) << "no wait for the GIL";
  }
  ASSERT_TRUE(good->has_value()) << good->error();
  ASSERT_TRUE(broken->has_value()) << broken->error() << ": a Python compile error is its first run's";
  EXPECT_EQ(::PJ::test::pythonCompileEvents(), before) << "nothing compiled before a run";
  const auto runs = [](const CompiledOnDemandScript& script) {
    return runOnDemand(script, {}, "{}", {}, RunOptions{});
  };
  EXPECT_DOUBLE_EQ(numberOf(runs(**good)), 3.0);
  const int compiled_once = ::PJ::test::pythonCompileEvents();
  EXPECT_GT(compiled_once, before) << "the first run compiled";
  EXPECT_DOUBLE_EQ(numberOf(runs(**good)), 3.0);
  const auto failed = runs(**broken);
  ASSERT_FALSE(failed.has_value());
  const auto eager = compile(kBroken);
  ASSERT_FALSE(eager.has_value());
  EXPECT_EQ(failed.error().message, eager.error()) << "the same message as an eager compile";
  const int after_broken = ::PJ::test::pythonCompileEvents();
  EXPECT_EQ(errorOf(runs(**broken)), eager.error());
  EXPECT_EQ(::PJ::test::pythonCompileEvents(), after_broken) << "a compile error is kept, not compiled again";
  for (int attempt = 0; attempt < 200; ++attempt) {
    py::gil_scoped_acquire gil;
    if (PyWeakref_GetObject(weak) == Py_None) {
      break;
    }
    const py::gil_scoped_release released;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  {
    py::gil_scoped_acquire gil;
    EXPECT_EQ(PyWeakref_GetObject(weak), Py_None) << "the posted release dropped the code object";
    Py_DECREF(weak);
  }
}

// Two first runs of one handle racing (both inside while the GIL is held, released once they are): the script compiles
// once, and both runs return the same value. The handle's lock is taken before the GIL, so neither deadlocks.
TEST(PythonObjectScript, AConcurrentFirstRunCompilesOnce) {
  ASSERT_TRUE(compile("def evaluate(inputs, params):\n    return 0\n").has_value());  // booted
  int before = ::PJ::test::pythonCompileEvents();
  ASSERT_TRUE(compile("def evaluate(inputs, params):\n    return -1\n").has_value());
  const int per_compile = ::PJ::test::pythonCompileEvents() - before;
  ASSERT_GT(per_compile, 0);
  const auto script = deferOnDemandScriptPython("def evaluate(inputs, params):\n    return 7\n");
  ASSERT_TRUE(script.has_value()) << script.error();
  before = ::PJ::test::pythonCompileEvents();
  std::atomic<int> inside{0};
  double first = 0.0;
  double second = 0.0;
  const auto runner = [&](double& value) {
    ++inside;
    value = numberOf(run(*script));
  };
  std::thread one;
  std::thread two;
  {
    const ::PJ::test::PythonGilHold hold;
    one = std::thread(runner, std::ref(first));
    two = std::thread(runner, std::ref(second));
    while (inside.load() < 2) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // both blocked: one on the GIL, one on the lock
  }
  one.join();
  two.join();
  EXPECT_DOUBLE_EQ(first, 7.0);
  EXPECT_DOUBLE_EQ(second, 7.0);
  EXPECT_EQ(::PJ::test::pythonCompileEvents() - before, per_compile) << "one compile for both first runs";
}

// A first run cancelled before it starts compiles nothing and caches nothing: the next run compiles and succeeds.
TEST(PythonObjectScript, ACancelledFirstRunDoesNotCacheAnError) {
  ASSERT_TRUE(compile("def evaluate(inputs, params):\n    return 0\n").has_value());  // booted
  const auto script = deferOnDemandScriptPython("def evaluate(inputs, params):\n    return 5\n");
  ASSERT_TRUE(script.has_value()) << script.error();
  const int before = ::PJ::test::pythonCompileEvents();
  RunOptions cancelled;
  cancelled.cancelled = [] { return true; };
  EXPECT_EQ(errorOf(run(*script, {}, {}, "{}", cancelled)), "script cancelled");
  EXPECT_EQ(::PJ::test::pythonCompileEvents(), before) << "a cancelled run compiles nothing";
  EXPECT_DOUBLE_EQ(numberOf(run(*script)), 5.0);
  EXPECT_GT(::PJ::test::pythonCompileEvents(), before);
}

// A run stopped by the watchdog leaves nothing behind that fails the next revision's compile on the same thread.
TEST(PythonObjectScript, ATimedOutRunDoesNotPoisonTheNextRevisionsCompile) {
  const auto spinning = deferOnDemandScriptPython("def evaluate(inputs, params):\n    while True:\n        pass\n");
  ASSERT_TRUE(spinning.has_value()) << spinning.error();
  RunOptions quick;
  quick.limits.python_watchdog_millis = 50;
  EXPECT_NE(errorOf(run(*spinning, {}, {}, "{}", quick)).find("TimeoutError"), std::string::npos);
  const auto next = deferOnDemandScriptPython("def evaluate(inputs, params):\n    return 9\n");
  ASSERT_TRUE(next.has_value()) << next.error();
  EXPECT_DOUBLE_EQ(numberOf(run(*next)), 9.0);
}

// The handle a run holds keeps the code object alive: dropping the last other copy while the first run executes
// releases the code object only after the run returned.
TEST(PythonObjectScript, DroppingTheLastExternalCopyDuringItsFirstRunReleasesAfterTheRun) {
  std::optional<Expected<CompiledPythonScript>> external = deferOnDemandScriptPython(
      "def evaluate(inputs, params):\n    s = 0\n    for i in range(3_000_000):\n        s += i\n    return 1\n");
  ASSERT_TRUE(external->has_value()) << external->error();
  CompiledPythonScript held = **external;  // the run's copy, as a job holds it
  std::atomic<bool> returned{false};
  double value = 0.0;
  RunOptions patient;
  patient.limits.python_watchdog_millis = 60'000;
  std::thread runner([&] {
    value = numberOf(run(held, {}, {}, "{}", patient));
    returned = true;
  });
  PyObject* code = nullptr;
  for (int attempt = 0; attempt < 500 && code == nullptr; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    code = detail::codeObjectOf(**external);  // under the handle's lock: after the runner's compile
  }
  ASSERT_NE(code, nullptr);
  PyObject* weak = nullptr;
  {
    py::gil_scoped_acquire gil;
    weak = PyWeakref_NewRef(code, nullptr);
  }
  ASSERT_NE(weak, nullptr);
  ASSERT_FALSE(returned.load()) << "the loop outlasts the setup";
  external.reset();  // the last copy but the run's
  {
    py::gil_scoped_acquire gil;
    EXPECT_NE(PyWeakref_GetObject(weak), Py_None) << "alive while the run holds its copy";
  }
  runner.join();
  EXPECT_DOUBLE_EQ(value, 1.0);
  held = CompiledPythonScript{};  // the run's copy goes: its release is posted to the helper
  bool released = false;
  for (int attempt = 0; attempt < 500 && !released; ++attempt) {
    {
      py::gil_scoped_acquire gil;
      released = PyWeakref_GetObject(weak) == Py_None;
    }
    if (!released) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  EXPECT_TRUE(released) << "released once the run's copy went";
  py::gil_scoped_acquire gil;
  Py_DECREF(weak);
}

// ---- video: frame_at (the twins of object_script_test.cpp's) ------------------------------------------------------

using test::BucketVideo;
using test::videoInput;
using test::withVideo;

// `frame_at` returns `(frame, None)`; each frame pairs with its own presentation time, decoded once; a builder finished
// between two frames pairs with the one decoded so far, one after frames that disagree with the video input's own
// stamp.
TEST(PythonObjectScript, VideoFramesPairAndDecodeLikeLuau) {
  auto video = std::make_shared<BucketVideo>();
  auto decoder = std::make_shared<FakeImageDecoder>(1, 1, 1.0f);
  auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    a, why = inputs["video"].frame_at(13)
    built = pj.annotations.new(image_topic="/video").points(points=[(0, 0)], type="points").finish()
    b = inputs["video"].frame_at(23)[0]
    if why is not None or a["presentation_ns"] != 10 or b["presentation_ns"] != 20:
        raise ValueError("presentation times")
    a["image"].decode()
    return {"a": a["image"].decode()[0].threshold(">", 0), "b": b["image"].decode()[0].threshold(">", 0), "built": built,
            "after": pj.annotations.new(image_topic="video").finish()}
)PY",
      {videoInput()},
      specs({"a:kImageAnnotations", "b:kImageAnnotations", "built:kImageAnnotations", "after:kImageAnnotations"}), "{}",
      withVideo(video, decoder));
  for (const auto& [name, stamp] :
       std::vector<std::pair<std::string, Timestamp>>{{"a", 10}, {"b", 20}, {"built", 10}, {"after", 99}}) {
    const auto* annotations = objectOf<sdk::ImageAnnotations>(result, name);
    ASSERT_NE(annotations, nullptr) << name;
    EXPECT_EQ(annotations->image_topic, "/video") << name;
    EXPECT_EQ(annotations->timestamp, stamp) << name;
  }
  EXPECT_EQ(video->calls, 2);
  EXPECT_EQ(decoder->calls, 2);
}

// `(None, reason)` for a missing source, a chained video and a frame over the native budget, which first retries once
// after a garbage collection; a time that is not an integer raises; a merged entry's query and presentation pass
// through on the dataset clock; the source gets the run's cancel check; another object has no `frame_at` to call.
TEST(PythonObjectScript, FrameAtReportsLikeLuau) {
  const char* reason = "def evaluate(inputs, params):\n    frame, why = inputs['video'].frame_at(13)\n    return why\n";
  EXPECT_EQ(textOf(runSource(reason, {videoInput()}, specs({"why:string"})), "why"), "no video frame source");
  NamedValue chained = videoInput();
  chained.topic_id = {};
  EXPECT_EQ(
      textOf(
          runSource(reason, {chained}, specs({"why:string"}), "{}", withVideo(std::make_shared<BucketVideo>(), {})),
          "why"),
      "video input 'video' has no store topic");
  RunOptions tiny = withVideo(std::make_shared<BucketVideo>(), {});
  tiny.native_budget_bytes = 256;  // the reason fits, the frame's 768 bytes do not
  EXPECT_NE(
      textOf(runSource(reason, {videoInput()}, specs({"why:string"}), "{}", tiny), "why").find("native memory"),
      std::string::npos);
  RunOptions one_frame = withVideo(std::make_shared<BucketVideo>(), {});
  one_frame.native_budget_bytes = 1200;  // one frame fits, a second only once a collection frees the dead first
  EXPECT_DOUBLE_EQ(
      numberOf(runSource(
          R"PY(
def evaluate(inputs, params):
    n = 0
    for i in range(3):
        cycle = [inputs["video"].frame_at(13)[0]["image"]]
        cycle.append(cycle)
        n += 1
        cycle = None
    return n
)PY",
          {videoInput()}, {}, "{}", one_frame)),
      3.0);

  // A whole-number float is a time, as in Luau and `pj.tf.lookup`; a bool is not.
  EXPECT_DOUBLE_EQ(
      numberOf(
          runSource(
              "def evaluate(inputs, params):\n    return inputs['video'].frame_at(13.0)[0]['presentation_ns']\n",
              {videoInput()}, {number("pts")}, "{}", withVideo(std::make_shared<BucketVideo>(), {})),
          "pts"),
      10.0);
  for (const char* time : {"13.5", "'13'", "10 ** 30", "1e300", "True"}) {
    const std::string source =
        std::string("def evaluate(inputs, params):\n    return inputs['video'].frame_at(") + time + ")[0]['image']\n";
    EXPECT_NE(
        errorOf(
            runSource(
                source, {videoInput()}, specs({"image:kImage"}), "{}", withVideo(std::make_shared<BucketVideo>(), {})))
            .find("frame_at: time"),
        std::string::npos)
        << time;
  }

  NamedValue shifted = videoInput();
  shifted.stamp_shift = 1003;
  auto video = std::make_shared<BucketVideo>();
  RunOptions options = withVideo(video, {});
  options.cancelled = [] { return false; };
  EXPECT_DOUBLE_EQ(
      numberOf(
          runSource(
              "def evaluate(inputs, params):\n    return inputs['video'].frame_at(1023)[0]['presentation_ns']\n",
              {shifted}, {number("pts")}, "{}", options),
          "pts"),
      1020.0);
  EXPECT_EQ(video->received, 1023);
  EXPECT_TRUE(video->saw_cancel_check);

  EXPECT_NE(
      errorOf(runSource(
                  "def evaluate(inputs, params):\n    return inputs['image'].frame_at(13)\n",
                  {imageInput("image", "/cam", 0)}, {number("x")}, "{}",
                  withVideo(std::make_shared<BucketVideo>(), {})))
          .find("video frame"),
      std::string::npos);
}

// A media input the script drops keeps its address for the whole run, so no frame decoded afterwards takes the dropped
// image's identity (and its decode) for its own: every frame pairs with its own presentation time.
TEST(PythonObjectScript, AMediaInputTheScriptDropsKeepsItsAddressForTheRun) {
  NamedValue image = imageInput("image", "/cam", 5);
  image.stamp_shift = 1;  // the run's copy is a struct of its own, which only the script's proxy would hold
  constexpr int kFrames = 8;
  std::vector<OutputSpec> outputs;
  for (int index = 0; index < kFrames; ++index) {
    outputs.push_back(parseOutputSpec("f" + std::to_string(index) + ":kImageAnnotations").value());
  }
  auto result = runSource(
      R"PY(
def evaluate(inputs, params):
    inputs["image"].decode()
    video = inputs["video"]
    del inputs["image"]
    frames = [video.frame_at(13 + 10 * index)[0]["image"] for index in range(8)]
    return [frame.decode()[0].threshold(">", 0) for frame in frames]
)PY",
      {image, videoInput()}, outputs, "{}",
      withVideo(std::make_shared<BucketVideo>(), std::make_shared<FakeImageDecoder>(1, 1, 1.0f)));
  for (int index = 0; index < kFrames; ++index) {
    const std::string name = "f" + std::to_string(index);
    const auto* annotations = objectOf<sdk::ImageAnnotations>(result, name);
    ASSERT_NE(annotations, nullptr) << name;
    EXPECT_EQ(annotations->image_topic, "/video") << name;
    EXPECT_EQ(annotations->timestamp, 10 + 10 * index) << name;
  }
}

TEST(PythonObjectScriptInferred, ObjectIsValueOfItsType) {
  const std::vector<NamedValue> inputs{objectInput("cloud", makeCloud({{0, 0, 0, 0}}))};
  auto single = runSource("def evaluate(inputs, params):\n    return inputs['cloud']\n", inputs);
  ASSERT_TRUE(single) << single.error().message;
  ASSERT_EQ(single->outputs.size(), 1u);
  EXPECT_EQ(single->outputs[0].name, "value");
  auto positional = runSource(
      "def evaluate(inputs, params):\n    return (3.0, inputs['cloud'], pj.scene.new(frame_id='m', id='e').finish())\n",
      inputs);
  ASSERT_TRUE(positional) << positional.error().message;
  ASSERT_EQ(positional->outputs.size(), 3u);
  EXPECT_EQ(positional->outputs[1].name, "cloud");
  EXPECT_EQ(positional->outputs[2].name, "scene");
}

// ---- bench ---------------------------------------------------------------------------------

constexpr const char* kFourLineScript = R"PY(import math
def evaluate(inputs, params):
    x = inputs["x"]
    return {"y": math.sqrt(x * x + 1.0)}
)PY";

constexpr const char* kFortyThreeLineScript = R"PY(import math
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

// PJ_BENCH=1 prints the median per-call cost of a run over the compiled script (compile once) against compiling it
// on every call, as the prototype did, for a 4-line and a 43-line script; median of 2001 calls.
TEST(PythonObjectScriptBench, PerCallSetup) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  const auto median = [](auto&& call) {
    std::vector<double> micros;
    for (int i = 0; i < 2001; ++i) {
      const auto begin = std::chrono::steady_clock::now();
      call();
      micros.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
    }
    std::nth_element(micros.begin(), micros.begin() + 1000, micros.end());
    return micros[1000];
  };
  const std::vector<NamedValue> inputs{NamedValue{.name = "x", .value = 3.0}};
  for (const auto& [name, source] :
       {std::pair{"4-line", kFourLineScript}, std::pair{"43-line", kFortyThreeLineScript}}) {
    auto compiled = compile(source);
    ASSERT_TRUE(compiled) << compiled.error();
    ASSERT_TRUE(run(*compiled, inputs)) << "the bench script runs";
    const double once = median([&] { (void)run(*compiled, inputs); });
    const double every_call = median([&] { (void)run(*compile(source), inputs); });
    std::printf("%s script: %.2f us per call compiled once, %.1f us compiling every call\n", name, once, every_call);
  }
}

// PJ_BENCH=1 prints the cost of iterating a 100,000-element list field (Luau: 112 ns per element, ARCHITECTURE §19)
// and of the xyz_points() iterator over 100,000 points, per element; median of 5 runs.
TEST(PythonObjectScriptBench, ListIterationAndXyzPoints) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr int kElements = 100'000;
  sdk::FrameTransforms transforms;
  transforms.transforms.resize(kElements);
  for (auto& transform : transforms.transforms) {
    transform.translation.x = 1.0;
  }
  const std::vector<NamedValue> inputs{
      objectInput("tf", sdk::BuiltinObject(std::move(transforms))), objectInput("cloud", makeLineCloud(kElements))};
  RunOptions options;
  options.limits.python_watchdog_millis = 60'000;
  for (const auto& [name, source, expected] : std::vector<std::tuple<const char*, const char*, double>>{
           {"for loop over a list field",
            "def evaluate(inputs, params):\n    s = 0.0\n    for t in inputs['tf'].transforms:\n"
            "        s += t.translation.x\n    return s\n",
            kElements},
           {"indexed loop, the list held in a local",
            "def evaluate(inputs, params):\n    s = 0.0\n    items = inputs['tf'].transforms\n"
            "    for i in range(len(items)):\n        s += items[i].translation.x\n    return s\n",
            kElements},
           {"xyz_points()",
            "def evaluate(inputs, params):\n    s = 0.0\n    for i, x, y, z in inputs['cloud'].xyz_points():\n"
            "        s += x\n    return s\n",
            0.5 * kElements * (kElements - 1)}}) {
    auto compiled = compile(source);
    ASSERT_TRUE(compiled) << compiled.error();
    std::vector<double> nanos;
    for (int sample = 0; sample < 5; ++sample) {
      const auto begin = std::chrono::steady_clock::now();
      auto result = run(*compiled, inputs, {}, "{}", options);
      nanos.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count());
      EXPECT_DOUBLE_EQ(numberOf(result), expected);
    }
    std::sort(nanos.begin(), nanos.end());
    std::printf("%s: %.0f ns per element\n", name, nanos[2] / kElements);
  }
}

// PJ_BENCH=1 prints what each step of a list-element read costs in the Python binder (median of 5 runs, ns per
// element): a 10k-element list of FrameTransforms walked ten times, from an empty loop over the list (the element's
// view only) to a nested field read, against an empty `range` loop (the interpreter's floor). The Luau twin is
// `ObjectScriptBench.BinderPerElementCost`.
TEST(PythonObjectScriptBench, ListElementStepCost) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr int kListSize = 10'000;
  constexpr double kElements = 10.0 * kListSize;
  sdk::FrameTransforms transforms;
  transforms.transforms.resize(kListSize);
  for (auto& transform : transforms.transforms) {
    transform.translation.x = 1.0;
    transform.rotation.w = 1.0;
  }
  const std::vector<NamedValue> inputs{objectInput("tf", sdk::BuiltinObject(std::move(transforms)))};
  RunOptions options;
  options.limits.python_watchdog_millis = 60'000;
  const std::vector<std::pair<const char*, const char*>> cases{
      {"empty range loop", "    for i in range(100000):\n        s += 1.0\n"},
      {"for t in list (element view)", "    for p in range(10):\n        for t in items:\n            pass\n"},
      {"t.parent_frame_id (view + scalar)",
       "    for p in range(10):\n        for t in items:\n            e = t.parent_frame_id\n"},
      {"t.translation (two views)",
       "    for p in range(10):\n        for t in items:\n            e = t.translation\n"},
      {"t.translation.x", "    for p in range(10):\n        for t in items:\n            s += t.translation.x\n"},
      {"t.rotation.w (last field, last member)",
       "    for p in range(10):\n        for t in items:\n            s += t.rotation.w\n"},
  };
  for (const auto& [name, body] : cases) {
    const std::string source =
        std::string("def evaluate(inputs, params):\n    s = 0.0\n    items = inputs['tf'].transforms\n") + body +
        "    return s\n";
    auto compiled = compile(source);
    ASSERT_TRUE(compiled) << compiled.error();
    std::vector<double> nanos;
    for (int sample = 0; sample < 5; ++sample) {
      const auto begin = std::chrono::steady_clock::now();
      auto result = run(*compiled, inputs, {}, "{}", options);
      nanos.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count());
      ASSERT_TRUE(result.has_value()) << result.error().message;
    }
    std::sort(nanos.begin(), nanos.end());
    std::printf("[PythonObjectScriptBench] %-40s %6.1f ns per element\n", name, nanos[2] / kElements);
  }
}

}  // namespace
}  // namespace PJ::scripting
