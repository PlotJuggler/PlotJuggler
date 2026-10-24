// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Smoke test for the embedded-CPython backend: proves the interpreter boots
// (PYTHONHOME / stdlib found), a filter module inspects + instantiates, and
// calculate() computes.

#include "pj_scripting/python_engine.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "pj_scripting/script_engine.h"

using namespace PJ::scripting;

namespace {
constexpr const char* kDoubleSource = R"PY(# pj-script: python
class T:
    id = "double_it"
    name = "Double"
    output = "double"
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        return value * 2
)PY";
}  // namespace

TEST(PythonEngine, InspectReadsClassMetadata) {
  auto engine = makePythonEngine();
  auto classes = engine->inspectModule(kDoubleSource, "test");
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  ASSERT_EQ(classes->size(), 1u);
  EXPECT_EQ((*classes)[0].id, "double_it");
  EXPECT_EQ((*classes)[0].output_kind, "double");
}

TEST(PythonEngine, CalculateComputesValueTimesTwo) {
  auto engine = makePythonEngine();
  auto classes = engine->inspectModule(kDoubleSource, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance((*classes)[0], "{}");
  ASSERT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());
  auto r = (*inst)->calculate(0.0, 21.0);
  EXPECT_FALSE(r.suppress);
  EXPECT_DOUBLE_EQ(r.value, 42.0);
  EXPECT_FALSE((*inst)->failed());
}

TEST(PythonEngine, SyntaxErrorIsReported) {
  auto engine = makePythonEngine();
  auto classes = engine->inspectModule("# pj-script: python\nclass T\n  bad", "test");
  EXPECT_FALSE(classes.has_value());
}

// The compile runs on the host heap, outside the filter's cap: the source size bounds it.
TEST(PythonEngine, ASourceOverOneMegabyteIsRejected) {
  const std::string huge = "# pj-script: python\n" + std::string(kMaxSourceBytes, '#') + "\nclass T:\n    id = 'x'\n";
  auto classes = makePythonEngine()->inspectModule(huge, "test");
  ASSERT_FALSE(classes.has_value());
  EXPECT_NE(classes.error().find("script larger than 1 MB"), std::string::npos) << classes.error();
  FilterClass klass;
  klass.source = huge;
  auto instance = makePythonEngine()->createInstance(klass, "{}");
  ASSERT_FALSE(instance.has_value());
  EXPECT_NE(instance.error().find("script larger than 1 MB"), std::string::npos) << instance.error();
}

TEST(PythonEngine, RuntimeErrorFailsStickily) {
  auto engine = makePythonEngine();
  constexpr const char* kBad = R"PY(# pj-script: python
class T:
    id = "boom"
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        return value + "oops"
)PY";
  auto classes = engine->inspectModule(kBad, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance((*classes)[0], "{}");
  ASSERT_TRUE(inst.has_value());
  auto r = (*inst)->calculate(0.0, 1.0);
  EXPECT_TRUE(r.suppress);         // a runtime error suppresses output
  EXPECT_TRUE((*inst)->failed());  // ...and puts the instance into a sticky failed state
}

// The stdlib's C extensions (lib-dynload/*.so) carry no copy of the interpreter:
// they resolve Py* at dlopen time against whatever the process already exports. An
// embedded CPython that does not publish those symbols dynamically breaks EVERY one
// of them, not just the module that happens to be imported first — so import all
// three allowed modules (whose undefined symbols differ; `statistics` alone pulls
// five extensions) and pin one of them down to a call. Without this the suite passes
// on a build whose Python cannot import anything.
TEST(PythonEngine, StdlibCExtensionsImport) {
  auto engine = makePythonEngine();
  constexpr const char* kImports = R"PY(# pj-script: python
import math
import cmath
import statistics

class T:
    id = "stdlib_sqrt"
    name = "Stdlib sqrt"
    output = "double"
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        return math.sqrt(value)
)PY";
  auto classes = engine->inspectModule(kImports, "test");
  // inspectModule executes the module body, so a broken lib-dynload surfaces here
  // as the ImportError text rather than as a mysterious empty catalogue.
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  ASSERT_EQ(classes->size(), 1u);
  auto inst = engine->createInstance((*classes)[0], "{}");
  ASSERT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());
  auto r = (*inst)->calculate(0.0, 144.0);
  EXPECT_FALSE(r.suppress);
  EXPECT_DOUBLE_EQ(r.value, 12.0);
  EXPECT_FALSE((*inst)->failed());
}

// Data Processors compute over timeseries; the filesystem, the network and
// subprocesses are out of scope. A rejection must name the policy — a script author
// who imports `os` should learn it is unavailable here, not read a linker error.
TEST(PythonEngine, DisallowedImportsAreRejectedByPolicy) {
  auto engine = makePythonEngine();
  for (const char* module : {"os", "subprocess", "socket", "shutil"}) {
    const std::string source = std::string("# pj-script: python\nimport ") + module + "\n";
    auto classes = engine->inspectModule(source, "test");
    ASSERT_FALSE(classes.has_value()) << "'" << module << "' must not be importable from a filter";
    EXPECT_NE(classes.error().find("not available in Data Processors"), std::string::npos)
        << "rejection of '" << module << "' must state the policy, got: " << classes.error();
  }
}

// The gate governs only what the SCRIPT imports. `statistics` internally imports
// `random`, `fractions`, `_statistics` and more; those run under the module's own
// globals with unrestricted builtins, so an allowlist entry must not be crippled by
// its own dependencies being off-list.
TEST(PythonEngine, AllowedModulesKeepTheirOwnDependencies) {
  auto engine = makePythonEngine();
  constexpr const char* kSource = R"PY(# pj-script: python
import statistics

class T:
    id = "stdev"
    output = "double"
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        return statistics.mean([value, value, value])
)PY";
  auto classes = engine->inspectModule(kSource, "test");
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  auto inst = engine->createInstance((*classes)[0], "{}");
  ASSERT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());
  EXPECT_DOUBLE_EQ((*inst)->calculate(0.0, 7.0).value, 7.0);
}

// ---------------------------------------------------------------------------
// The sandbox the on-demand lane shares (python_sandbox.h, PY-07)
// ---------------------------------------------------------------------------

namespace {

// Builds an instance of `source` (a module defining class T) on `engine`.
std::unique_ptr<FilterInstance> makeInstance(ScriptEngine& engine, const std::string& source) {
  auto classes = engine.inspectModule(source, "test");
  EXPECT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  if (!classes.has_value()) {
    return nullptr;
  }
  auto inst = engine.createInstance((*classes)[0], "{}");
  EXPECT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());
  return inst.has_value() ? std::move(*inst) : nullptr;
}

std::unique_ptr<FilterInstance> makeInstance(const std::string& source, BudgetLimits limits = {}) {
  return makeInstance(*makePythonEngine(limits), source);
}

// A filter class whose calculate() body is `body` (indented by the caller's newlines), after `header`.
std::string filterWith(const std::string& body, const std::string& header = "") {
  return "# pj-script: python\n" + header +
         "class T:\n    id = 'probe'\n    @staticmethod\n    def create(params):\n        return T()\n"
         "    def calculate(self, time, value, *args):\n        " +
         body + "\n        return value\n";
}

// The error of the first calculate() of `source`, which must fail.
std::string calculateError(const std::string& source, BudgetLimits limits = {}) {
  auto inst = makeInstance(source, limits);
  if (inst == nullptr) {
    return "no instance";
  }
  EXPECT_TRUE(inst->calculate(0.0, 1.0).suppress);
  EXPECT_TRUE(inst->failed());
  return inst->error();
}

/// `text` as a single-quoted Python literal: a Windows path's backslashes stay characters, not escapes.
std::string pyLiteral(const std::string& text) {
  std::string out = "'";
  for (const char c : text) {
    if (c == '\\' || c == '\'') {
      out += '\\';
    }
    out += c;
  }
  return out + "'";
}

/// A file a script must not be able to remove; removed when the test ends.
struct Victim {
  std::string path =
      ::testing::TempDir() + "pj_py_filter_victim_" + ::testing::UnitTest::GetInstance()->current_test_info()->name();
  Victim() {
    std::ofstream(path) << "x";
  }
  ~Victim() {
    std::filesystem::remove(path);
  }
};

constexpr const char* kSpinInCalculate = R"PY(# pj-script: python
class T:
    id = "spin"
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        if value > 0:
            while True:
                pass
        return value
)PY";

}  // namespace

TEST(PythonEngine, OpenIsDeniedByTheSharedAuditHook) {
  // `open` stays a builtin (the compatible profile); the hook refuses the call.
  const std::string error = calculateError(filterWith("open('/etc/passwd')"));
  EXPECT_NE(error.find("PermissionError: open is not available"), std::string::npos) << error;
}

TEST(PythonEngine, OsReachedThroughAllowedModuleIsDeniedByAuditHook) {
  Victim victim;
  const std::string error =
      calculateError(filterWith("statistics.random._os.remove(" + pyLiteral(victim.path) + ")", "import statistics\n"));
  EXPECT_NE(error.find("PermissionError: os.remove is not available"), std::string::npos) << error;
  EXPECT_TRUE(std::filesystem::exists(victim.path));
}

TEST(PythonEngine, InfiniteLoopInCalculateFailsWithinBudget) {
  BudgetLimits limits;
  limits.python_call_millis = 50;
  auto inst = makeInstance(kSpinInCalculate, limits);
  ASSERT_NE(inst, nullptr);
  EXPECT_TRUE(inst->calculate(0.0, 1.0).suppress);
  ASSERT_TRUE(inst->failed());
  EXPECT_NE(inst->error().find("TimeoutError: calculate() exceeded its time budget of 50 ms"), std::string::npos)
      << inst->error();
  EXPECT_TRUE(inst->calculate(0.0, -1.0).suppress) << "sticky: a failed instance does not run again";
}

TEST(PythonEngine, CallsWithinBudgetDoNotTrip) {
  BudgetLimits limits;
  limits.python_call_millis = 50;
  auto inst = makeInstance(kDoubleSource, limits);
  ASSERT_NE(inst, nullptr);
  for (int i = 0; i < 20000; ++i) {
    ASSERT_FALSE(inst->calculate(static_cast<double>(i), 1.0).suppress) << "row " << i << ": " << inst->error();
  }
}

TEST(PythonEngine, ScriptCannotSwallowTheTimeout) {
  BudgetLimits limits;
  limits.python_call_millis = 50;
  const std::string error = calculateError(
      filterWith(
          "try:\n            while True:\n                pass\n        except BaseException:\n"
          "            pass"),
      limits);
  EXPECT_NE(error.find("time budget"), std::string::npos) << error;
}

TEST(PythonEngine, NoStaleTimeoutLeaksIntoTheNextInstance) {
  BudgetLimits limits;
  limits.python_call_millis = 50;
  auto engine = makePythonEngine(limits);
  auto spin = makeInstance(*engine, kSpinInCalculate);
  auto ok = makeInstance(*engine, kDoubleSource);
  ASSERT_NE(spin, nullptr);
  ASSERT_NE(ok, nullptr);
  (void)spin->calculate(0.0, 1.0);
  ASSERT_TRUE(spin->failed());
  for (int i = 0; i < 1000; ++i) {
    ASSERT_FALSE(ok->calculate(0.0, 1.0).suppress) << ok->error();
  }
}

TEST(PythonEngine, InfiniteLoopAtModuleTopLevelAbortsInspect) {
  BudgetLimits limits;
  limits.python_watchdog_millis = 50;
  auto classes = makePythonEngine(limits)->inspectModule(
      "# pj-script: python\nwhile True:\n    pass\nclass T:\n    id = 'x'\n", "t");
  ASSERT_FALSE(classes.has_value());
  EXPECT_NE(classes.error().find("TimeoutError: module evaluation exceeded"), std::string::npos) << classes.error();
}

TEST(PythonEngine, MemoryHogFailsTheInstanceAndSiblingSurvives) {
  constexpr const char* kHog = R"PY(# pj-script: python
class T:
    id = "hog"
    @staticmethod
    def create(params):
        t = T()
        t.keep = []
        return t
    def calculate(self, time, value, *args):
        self.keep.append(bytearray(1 << 20))
        return value
)PY";
  BudgetLimits limits;
  limits.mem_bytes = std::size_t{8} << 20;
  auto engine = makePythonEngine(limits);
  auto hog = makeInstance(*engine, kHog);
  auto ok = makeInstance(*engine, kDoubleSource);
  ASSERT_NE(hog, nullptr);
  ASSERT_NE(ok, nullptr);
  for (int i = 0; i < 64 && !hog->failed(); ++i) {  // retained state counts across calls
    (void)hog->calculate(0.0, 1.0);
  }
  ASSERT_TRUE(hog->failed());
  EXPECT_NE(hog->error().find("MemoryError"), std::string::npos) << hog->error();
  EXPECT_FALSE(ok->calculate(0.0, 2.0).suppress) << ok->error();
}

TEST(PythonEngine, ACaughtTimeoutIsInterruptedAgain) {
  BudgetLimits limits;
  limits.python_call_millis = 50;
  const std::string error = calculateError(
      filterWith(
          "try:\n            while True:\n                pass\n        except TimeoutError:\n"
          "            while True:\n                pass"),
      limits);
  EXPECT_NE(error.find("time budget"), std::string::npos) << error;
}

// A call past its deadline fails even when the watchdog never got the GIL to interrupt it: the string is built and
// returned without a bytecode check in between.
TEST(PythonEngine, AnOverdueCallFailsWithoutAnInterruption) {
  BudgetLimits limits;
  limits.python_call_millis = 1;
  limits.mem_bytes = std::size_t{512} << 20;
  auto inst = makeInstance(
      "# pj-script: python\nclass T:\n    id = 'slow'\n    @staticmethod\n    def create(params):\n        return T()\n"
      "    def calculate(self, time, value, *args):\n        return value if 'x' * (300 << 20) else value\n",
      limits);
  ASSERT_NE(inst, nullptr);
  EXPECT_TRUE(inst->calculate(0.0, 1.0).suppress);
  EXPECT_NE(inst->error().find("time budget of 1 ms"), std::string::npos) << inst->error();
}

TEST(PythonEngine, ACaughtMemoryErrorFailsTheInstance) {
  BudgetLimits limits;
  limits.mem_bytes = std::size_t{8} << 20;
  auto inst = makeInstance(
      filterWith(
          "try:\n            bytearray(64 << 20)\n        except MemoryError:\n"
          "            pass"),
      limits);
  ASSERT_NE(inst, nullptr);
  EXPECT_TRUE(inst->calculate(0.0, 1.0).suppress);
  ASSERT_TRUE(inst->failed());
  EXPECT_NE(inst->error().find("MemoryError: script memory limit (8 MB) exceeded"), std::string::npos) << inst->error();
  auto classes = makePythonEngine(limits)->inspectModule(
      "# pj-script: python\ntry:\n    bytearray(64 << 20)\nexcept MemoryError:\n    pass\nclass T:\n    id = 'x'\n",
      "test");
  ASSERT_FALSE(classes);
  EXPECT_NE(classes.error().find("script memory limit (8 MB) exceeded"), std::string::npos) << classes.error();
}

// The compatible profile: what a filter written for the release before the sandbox uses keeps working, real builtins,
// string formatting and real modules included.
TEST(PythonEngine, ExistingFiltersKeepTheirBuiltinsAndSyntax) {
  auto inst = makeInstance(R"PY(# pj-script: python
import math
class T:
    id = "compat"
    scale = 2.0
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        text = "{}".format(value) + "{v}".format_map({"v": ""}) + f"{value:.1f}"
        assert type(value) is float and isinstance(text, str)
        factor = getattr(self, "scale") * eval("1 + 0") * math.cos(0.0)
        return float(text[: len(str(value))]) * factor
)PY");
  ASSERT_NE(inst, nullptr);
  auto result = inst->calculate(0.0, 3.0);
  ASSERT_FALSE(result.suppress) << inst->error();
  EXPECT_DOUBLE_EQ(result.value, 6.0);
}

// A call that catches every MemoryError the cap raises until its time also runs out failed on memory first, and is
// reported so.
TEST(PythonEngine, AMemoryStopIsReportedEvenWhenTheTimeAlsoRunsOut) {
  BudgetLimits limits;
  limits.mem_bytes = std::size_t{1} << 20;
  limits.python_call_millis = 200;
  const std::string error = calculateError(
      filterWith(
          "while True:\n            try:\n                x = None\n                while True:\n"
          "                    x = (x,)\n            except MemoryError:\n                pass"),
      limits);
  EXPECT_NE(error.find("MemoryError: script memory limit (1 MB) exceeded"), std::string::npos) << error;
}
