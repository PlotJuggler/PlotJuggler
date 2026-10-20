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
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pj_base/sdk/platform.hpp"
#include "pj_scripting/memory_budget.h"
#include "pj_scripting/python_engine.h"
#include "python_sandbox.h"
#include "python_test_helpers.h"

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
    EXPECT_DOUBLE_EQ(numberOf(run(*compiled, {NamedValue{"x", 16.0}})), 4.0);
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
      {NamedValue{"a", 2.0}, NamedValue{"name", std::string("cam")}},
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
  fails(kOne, {NamedValue{"a", Unavailable{"r"}}}, {}, "{}", "input 'a' is unavailable");
  fails(kOne, {NamedValue{"cloud", sdk::BuiltinObject()}}, {}, "{}", "input 'cloud' is an object");
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
  const std::vector<NamedValue> inputs{NamedValue{"x", 3.0}};
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

}  // namespace
}  // namespace PJ::scripting
