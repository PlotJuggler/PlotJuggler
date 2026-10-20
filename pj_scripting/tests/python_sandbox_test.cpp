// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The hardening both Python engines share (python_sandbox.h), driven from the host: the AST check, the audit hook,
// the allocation cap and their per-thread arming. Desktop only.

#include "python_sandbox.h"

#include <gtest/gtest.h>
#include <pybind11/embed.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pj_scripting/memory_budget.h"
#include "pj_scripting/python_engine.h"
#include "pj_scripting/sandbox.h"
#include "python_test_helpers.h"

namespace py = pybind11;

namespace PJ::scripting {
namespace {

// The hook itself, driven from the host: what a script that recovered the real builtins or `os` would reach.
TEST(PythonSandbox, AuditHookDeniesOnlyWhileArmed) {
  test::Victim victim;
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  py::object builtins = py::module_::import("builtins");
  py::object os = py::module_::import("os");
  // sys._getframe audits only with a Python frame to return.
  py::object getframe = builtins.attr("compile")("import sys\nsys._getframe()\n", "<string>", "exec");
  int probed = 0;
  py::cpp_function probe(
      [&probed](py::str event, py::object) { probed += event.cast<std::string>() == "pj.probe" ? 1 : 0; });
  const auto denied = [](const auto& call) {
    try {
      call();
    } catch (py::error_already_set& error) {
      return error.matches(PyExc_PermissionError);
    }
    return false;
  };
  {
    detail::ScopedAuditGuard armed;
    EXPECT_TRUE(denied([&] { builtins.attr("open")(victim.path); }));
    EXPECT_TRUE(denied([&] { os.attr("remove")(victim.path); }));
    EXPECT_TRUE(denied([&] { detail::runCode(getframe, py::dict()); }));
    EXPECT_TRUE(denied([&] { py::module_::import("ctypes"); }));
    // `os` is frozen: importing it again opens no file, so only the import-root check refuses it.
    py::object modules = py::module_::import("sys").attr("modules");
    py::object saved_os = modules.attr("pop")("os");
    try {
      py::module_::import("os");
      ADD_FAILURE() << "importing os again was not refused";
    } catch (py::error_already_set& error) {
      EXPECT_NE(std::string(error.what()).find("import of os is not available"), std::string::npos) << error.what();
    }
    modules["os"] = saved_os;
    py::module_::import("sys").attr("addaudithook")(probe);  // refused silently: CPython drops the error
  }
  py::module_::import("sys").attr("audit")("pj.probe");
  EXPECT_EQ(probed, 0) << "a hook added while armed was not installed";
  EXPECT_FALSE(denied([&] { builtins.attr("open")(victim.path).attr("close")(); })) << "host work is not refused";
  EXPECT_TRUE(std::filesystem::exists(victim.path));
}

TEST(PythonSandbox, AllocationWrapperForwardsForeignBlocks) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  for (const auto domain : {PYMEM_DOMAIN_RAW, PYMEM_DOMAIN_MEM, PYMEM_DOMAIN_OBJ}) {
    PyMemAllocatorEx current{};
    PyMem_GetAllocator(domain, &current);
    auto budget = std::make_shared<native::MemoryBudget>(1024);
    void* foreign_free = nullptr;
    void* foreign_realloc = nullptr;
    {
      foreign_free = current.malloc(current.ctx, 64);
      foreign_realloc = current.malloc(current.ctx, 64);
      ASSERT_NE(foreign_free, nullptr);
      ASSERT_NE(foreign_realloc, nullptr);
      std::memset(foreign_realloc, 0x35, 64);
    }
    {
      detail::ScopedAllocationCap cap(budget);
      current.free(current.ctx, foreign_free);
      EXPECT_EQ(budget->used(), 0u) << "a block allocated outside the cap is not credited";
      auto* grown = static_cast<unsigned char*>(current.realloc(current.ctx, foreign_realloc, 128));
      ASSERT_NE(grown, nullptr);
      EXPECT_EQ(grown[63], 0x35);
      EXPECT_EQ(budget->used(), 128u) << "a foreign block grown under the cap is adopted at its full size";
      EXPECT_EQ(current.malloc(current.ctx, 1024), nullptr) << "past the cap";
      current.free(current.ctx, grown);
      EXPECT_EQ(budget->used(), 0u);
    }
  }
}

// Past the cap only a block of 1 KiB or more is refused; a smaller one is charged and succeeds (CPython mishandles a
// refused small object, a function among them).
TEST(PythonSandbox, PastTheCapOnlyLargeBlocksAreRefused) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  PyMemAllocatorEx current{};
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &current);
  auto budget = std::make_shared<native::MemoryBudget>(std::size_t{64} << 10);
  {
    detail::ScopedAllocationCap cap(budget);
    ASSERT_TRUE(budget->tryAcquire(budget->limit()));  // the cap reached
    void* small = current.malloc(current.ctx, 512);
    EXPECT_NE(small, nullptr) << "a small block past the cap succeeds";
    EXPECT_EQ(budget->used(), budget->limit() + 512) << "and is charged";
    EXPECT_FALSE(cap.refused()) << "below twice the cap nothing is latched";
    EXPECT_EQ(current.malloc(current.ctx, 1024), nullptr) << "a large block past the cap is refused";
    EXPECT_TRUE(cap.refused());
    current.free(current.ctx, small);
    budget->release(budget->limit());
    void* again = current.malloc(current.ctx, 1024);
    EXPECT_NE(again, nullptr) << "once frees bring the total back under the cap, a large block is granted again";
    current.free(current.ctx, again);
  }
}

// A large block's realloc past the cap is judged by its new size, and needs room only to grow: shrinking it, or keeping
// its size, between the cap and twice the cap succeeds.
TEST(PythonSandbox, PastTheCapALargeBlockShrinksButCannotGrow) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  PyMemAllocatorEx current{};
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &current);
  auto budget = std::make_shared<native::MemoryBudget>(std::size_t{64} << 10);
  {
    detail::ScopedAllocationCap cap(budget);
    void* block = current.malloc(current.ctx, 2048);
    ASSERT_NE(block, nullptr);
    std::vector<void*> smalls;  // small blocks past the cap
    while (budget->used() <= budget->limit()) {
      smalls.push_back(current.malloc(current.ctx, 512));
    }
    EXPECT_TRUE(budget->tryAcquire(0)) << "nothing to grow is never refused";
    EXPECT_EQ(current.realloc(current.ctx, block, 2048 + 64), nullptr) << "a large block cannot grow past the cap";
    void* same = current.realloc(current.ctx, block, 2048);
    ASSERT_NE(same, nullptr) << "nor is keeping its size refused";
    void* shrunk = current.realloc(current.ctx, same, 1536);
    ASSERT_NE(shrunk, nullptr) << "a large block shrinks past the cap";
    current.free(current.ctx, shrunk);
    for (void* each : smalls) {
      current.free(current.ctx, each);
    }
  }
}

// A realloc charged to another call's budget (a block an earlier call left behind) neither latches nor stops the
// current call.
TEST(PythonSandbox, AnotherBudgetsBlockDoesNotStopTheCurrentCall) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  PyMemAllocatorEx current{};
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &current);
  auto earlier = std::make_shared<native::MemoryBudget>(1024);
  void* left_behind = nullptr;
  {
    detail::ScopedAllocationCap cap(earlier);
    left_behind = current.malloc(current.ctx, 512);
    ASSERT_NE(left_behind, nullptr);
  }
  ASSERT_TRUE(earlier->tryAcquire(512));
  earlier->acquireAnyway(1024);  // the earlier call's budget near twice its cap
  bool latched = true;
  {
    detail::ScriptCallScope scope(std::make_shared<native::MemoryBudget>(std::size_t{1} << 20), 10000);
    void* grown = current.realloc(current.ctx, left_behind, 900);  // crosses twice the earlier cap
    ASSERT_NE(grown, nullptr);
    left_behind = grown;
    EXPECT_EQ(current.realloc(current.ctx, left_behind, 4096), nullptr) << "the earlier budget refuses it";
    latched = scope.overBudget();
    EXPECT_FALSE(scope.timedOut());
  }
  EXPECT_FALSE(latched) << "the current call is not over budget";
  current.free(current.ctx, left_behind);
}

// A loop of small objects, never refused, is stopped once its charges pass twice the cap: the watchdog raises a
// MemoryError at the next bytecode, and the call is latched over budget.
TEST(PythonSandbox, SmallObjectsAreStoppedAtTwiceTheCap) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  constexpr std::size_t kCap = std::size_t{8}
                               << 20;  // the stop lands within a GIL switch interval (5 ms) of allocations
  py::object code =
      py::module_::import("builtins")
          .attr("compile")(
              "n = 0\nx = None\nstopped = False\ntry:\n    while True:\n        x = (x,)\n        n += 1\n"
              "except MemoryError:\n    stopped = True\n",
              "<string>", "exec");
  py::dict globals;
  globals["__builtins__"] = py::module_::import("builtins");
  bool over_budget = false;
  {
    detail::ScriptCallScope scope(std::make_shared<native::MemoryBudget>(kCap), 10000);
    try {
      detail::runCode(code, globals);
    } catch (py::error_already_set&) {}
    over_budget = scope.overBudget();
    EXPECT_FALSE(scope.timedOut());
  }
  const auto tuples = globals["n"].cast<std::size_t>();
  constexpr std::size_t kTupleBytes = 48;  // a 1-tuple and its GC header
  EXPECT_GT(tuples * kTupleBytes, kCap * 3 / 2) << "small objects ran past the cap";
  EXPECT_LT(tuples * kTupleBytes, kCap * 3) << "and were stopped near twice the cap";
  EXPECT_TRUE(over_budget);
  EXPECT_TRUE(globals["stopped"].cast<bool>()) << "by a MemoryError";
  globals.clear();
}

// The memory stop is not a timeout: a call that passed twice the cap in host code, before the watchdog could interrupt
// it, is over budget and not timed out.
TEST(PythonSandbox, AMemoryStopIsNotATimeout) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  PyMemAllocatorEx current{};
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &current);
  std::vector<void*> blocks;
  bool timed_out = true;
  bool over_budget = false;
  {
    detail::ScriptCallScope scope(std::make_shared<native::MemoryBudget>(std::size_t{64} << 10), 10000);
    for (int i = 0; i < 300; ++i) {  // 150 KiB of small blocks, past twice the cap
      blocks.push_back(current.malloc(current.ctx, 512));
    }
    timed_out = scope.timedOut();
    over_budget = scope.overBudget();
  }
  for (void* block : blocks) {
    EXPECT_NE(block, nullptr);
    current.free(current.ctx, block);
  }
  EXPECT_FALSE(timed_out);
  EXPECT_TRUE(over_budget);
}

// The audit guard and the allocation cap are per thread: while one thread holds a tiny cap and the armed hook (and
// has released the GIL, as a native call does), another opens a file and allocates megabytes, neither refused nor
// charged.
TEST(PythonSandbox, GuardAndCapArePerThread) {
  detail::ensureInterpreter();
  std::mutex mutex;
  std::condition_variable changed;
  bool armed = false;
  bool done = false;
  std::thread other([&] {
    py::gil_scoped_acquire gil;
    detail::ScriptCallScope scope(std::make_shared<native::MemoryBudget>(std::size_t{64} << 10), 10000);
    py::gil_scoped_release no_gil;
    std::unique_lock lock(mutex);
    armed = true;
    changed.notify_all();
    changed.wait_for(lock, std::chrono::seconds(10), [&] { return done; });
  });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(10), [&] { return armed; }));
  }
  test::Victim victim;
  try {
    py::gil_scoped_acquire gil;
    py::module_::import("builtins").attr("open")(victim.path).attr("close")();
    py::list big;
    for (int i = 0; i < 100000; ++i) {
      big.append(py::float_(static_cast<double>(i)));
    }
  } catch (py::error_already_set& error) {
    ADD_FAILURE() << "another thread's guard or cap reached this one: " << error.what();
  }
  {
    std::lock_guard lock(mutex);
    done = true;
  }
  changed.notify_all();
  other.join();
}

namespace {

constexpr const char* kFilterHead =
    "class T:\n    id = 'probe'\n    @staticmethod\n    def create(params):\n        return T()\n"
    "    def calculate(self, time, value, *args):\n";

/// A filter module: `header`, then class T whose calculate() runs `body` (one line) and returns `value`.
std::string filter(const std::string& body, const std::string& header = "") {
  return "# pj-script: python\n" + header + kFilterHead + "        " + body + "\n        return value\n";
}

}  // namespace

// Script code of a filter (module evaluation and calculate()) runs with the hook armed: an event a recovered `os` or
// a native call would raise is refused.
TEST(PythonSandbox, FilterScriptCallsArmTheHook) {
  test::AuditProbe& probe = test::auditProbe();
  auto engine = makePythonEngine();
  const int refused = probe.refused;
  auto top_level = engine->inspectModule(filter("pass", "from math.pj_audit_probe import emit\nemit()\n"), "test");
  ASSERT_FALSE(top_level);
  EXPECT_NE(top_level.error().find("PermissionError"), std::string::npos) << top_level.error();
  auto classes = engine->inspectModule(filter("emit()", "from math.pj_audit_probe import emit\n"), "test");
  ASSERT_TRUE(classes) << classes.error();
  auto instance = engine->createInstance((*classes)[0], "{}");
  ASSERT_TRUE(instance) << instance.error();
  EXPECT_TRUE((*instance)->calculate(0.0, 1.0).suppress);
  EXPECT_NE((*instance)->error().find("PermissionError"), std::string::npos) << (*instance)->error();
  EXPECT_EQ(probe.refused, refused + 2);
  EXPECT_EQ(probe.allowed, 0);
}

// Destroying a filter clears its namespace (a cycle with its functions) under the guard, so a finalizer a module
// global carries runs then, armed, and not later on whichever thread collects the cycle. The finalizer keeps `emit`
// as a default: the cleared namespace no longer holds it.
TEST(PythonSandbox, FilterTeardownRunsUnderTheGuard) {
  test::AuditProbe& probe = test::auditProbe();
  auto engine = makePythonEngine();
  const int before_inspect = probe.refused;
  auto classes = engine->inspectModule(
      filter(
          "pass",
          "from math.pj_audit_probe import emit\nclass Probe:\n    def __del__(self, emit=emit):\n        emit()\n"
          "kept = Probe()\n"),
      "test");
  ASSERT_TRUE(classes) << classes.error();
  EXPECT_EQ(probe.refused, before_inspect + 1) << "inspection's namespace is cleared before it returns";
  auto instance = engine->createInstance((*classes)[0], "{}");
  ASSERT_TRUE(instance) << instance.error();
  const int refused = probe.refused;
  EXPECT_EQ(refused, before_inspect + 1) << "the instance keeps its namespace";
  std::unique_ptr<FilterInstance> filter_instance = std::move(*instance);
  filter_instance.reset();
  EXPECT_EQ(probe.refused, refused + 1) << "the finalizer ran, armed, while the instance was destroyed";
  EXPECT_EQ(probe.allowed, 0);
}

// A timeout that fires while no bytecode runs (`calculate` is a native call that releases the GIL) stays pending on
// the thread: the call drops it, so it never reaches the next filter's call.
TEST(PythonSandbox, AFilterTimeoutNeverReachesTheNextCall) {
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
  BudgetLimits limits;
  limits.python_call_millis = 20;
  auto engine = makePythonEngine(limits);
  auto slow_classes = engine->inspectModule(
      "# pj-script: python\nfrom math.pj_fake_sleep import sleep\nclass T:\n    id = 'slow'\n"
      "    calculate = staticmethod(sleep)\n    @staticmethod\n    def create(params):\n        return T()\n",
      "test");
  ASSERT_TRUE(slow_classes) << slow_classes.error();
  auto slow = engine->createInstance((*slow_classes)[0], "{}");
  ASSERT_TRUE(slow) << slow.error();
  auto plain_classes = engine->inspectModule(filter("pass"), "test");
  ASSERT_TRUE(plain_classes) << plain_classes.error();
  auto plain = engine->createInstance((*plain_classes)[0], "{}");
  ASSERT_TRUE(plain) << plain.error();
  EXPECT_TRUE((*slow)->calculate(0.0, 1.0).suppress);
  EXPECT_NE((*slow)->error().find("time budget"), std::string::npos) << (*slow)->error();
  EXPECT_FALSE((*plain)->calculate(0.0, 1.0).suppress) << (*plain)->error();
  py::gil_scoped_acquire gil;
  py::module_::import("sys").attr("modules").attr("pop")("math.pj_fake_sleep");
}

// A filter making functions past its cap fails cleanly and stays failed, whichever allocation the cap meets first:
// every cap from 16 to 64 KiB, in steps smaller than any object.
TEST(PythonSandbox, AFilterMakingFunctionsPastItsCapFailsCleanly) {
  const std::string source =
      filter("self.head = None\n        while True:\n            self.head = (lambda: 1.0, self.head)");
  auto classes = makePythonEngine()->inspectModule(source, "test");
  ASSERT_TRUE(classes) << classes.error();
  int exercised = 0;
  for (std::size_t cap = std::size_t{16} << 10; cap <= std::size_t{64} << 10; cap += 40) {
    BudgetLimits limits;
    limits.mem_bytes = cap;
    auto instance = makePythonEngine(limits)->createInstance((*classes)[0], "{}");
    if (!instance) {  // a process-wide table that grows during the create is charged to it, and may not fit
      EXPECT_NE(instance.error().find("MemoryError"), std::string::npos) << instance.error();
      continue;
    }
    ++exercised;
    EXPECT_TRUE((*instance)->calculate(0.0, 1.0).suppress);
    ASSERT_TRUE((*instance)->failed()) << cap;
    EXPECT_NE((*instance)->error().find("MemoryError"), std::string::npos) << (*instance)->error();
    EXPECT_TRUE((*instance)->calculate(0.0, 1.0).suppress) << "sticky";
  }
  EXPECT_GT(exercised, 1000);
}

}  // namespace
}  // namespace PJ::scripting
