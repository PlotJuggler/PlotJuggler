// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The hardening both Python engines share (python_sandbox.h), driven from the host: the AST check, the audit hook,
// the allocation cap and their per-thread arming. Desktop only.

#include "python_sandbox.h"

#include <gtest/gtest.h>
#include <pybind11/embed.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "pj_base/sdk/platform.hpp"
#include "pj_scripting/memory_budget.h"
#include "pj_scripting/python_engine.h"
#include "pj_scripting/python_object_script.h"
#include "pj_scripting/sandbox.h"
#include "python_test_helpers.h"

namespace py = pybind11;

namespace PJ::scripting {
namespace {

TEST(PythonSandbox, CompileRejectsFrameInternalsNamesAndLargeSources) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  for (const char* chain :
       {"(x for x in ()).gi_frame.f_globals", "g.gi_code", "f.f_back", "f.f_builtins", "c.cr_frame", "a.ag_frame",
        "t.tb_frame", "t.tb_next", "f.f_locals", "f.f_code"}) {
    auto compiled = detail::compileChecked(std::string("x = ") + chain + "\n");
    ASSERT_FALSE(compiled) << chain;
    EXPECT_NE(compiled.error().find("forbidden attribute"), std::string::npos) << compiled.error();
  }
  for (const char* name : {"__builtins__", "globals", "locals", "vars", "__loader__", "__spec__"}) {
    auto named = detail::compileChecked(std::string("x = ") + name + "\n");
    ASSERT_FALSE(named) << name;
    EXPECT_NE(named.error().find(std::string("forbidden name: ") + name), std::string::npos) << named.error();
  }
  auto dunder = detail::compileChecked("x = ().__class__\n");
  ASSERT_FALSE(dunder);
  EXPECT_NE(dunder.error().find("dunder"), std::string::npos) << dunder.error();
  auto too_large = detail::compileChecked(std::string(kMaxSourceBytes + 1, '#'));
  ASSERT_FALSE(too_large);
  EXPECT_NE(too_large.error().find("larger than"), std::string::npos) << too_large.error();
  EXPECT_TRUE(detail::compileChecked("x = (1).real\n"));
}

// `str.format` and `format_map` walk attributes and items from a string, out of the AST check's sight.
TEST(PythonSandbox, CompileRefusesFormatMethods) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  for (const char* source :
       {"import statistics\nx = '{0.__globals__[sys].modules[os].environ[HOME]}'.format(statistics.mean)\n",
        "import statistics\nx = '{m.__globals__[sys]}'.format_map({'m': statistics.mean})\n", "f = str.format\n"}) {
    auto compiled = detail::compileChecked(source);
    ASSERT_FALSE(compiled) << source;
    EXPECT_NE(compiled.error().find("use an f-string"), std::string::npos) << compiled.error();
  }
  EXPECT_TRUE(detail::compileChecked("x = 2\ny = f'{x:.2f}'\n"));
}

// The strict profile's hook also refuses code from strings, which would bypass the AST check; the compatible one,
// the filters', lets it run.
TEST(PythonSandbox, StrictProfileAlsoRefusesExecAndCompile) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  py::object builtins = py::module_::import("builtins");
  auto assign = detail::compileChecked("x = 1\n");
  ASSERT_TRUE(assign) << assign.error();
  const auto denied = [](const auto& call) {
    try {
      call();
    } catch (py::error_already_set& error) {
      return error.matches(PyExc_PermissionError);
    }
    return false;
  };
  {
    detail::ScopedAuditGuard strict(detail::Profile::kStrict);
    EXPECT_TRUE(denied([&] { builtins.attr("exec")("x = 1", py::dict()); }));
    EXPECT_TRUE(denied([&] { builtins.attr("exec")(*assign, py::dict()); })) << "a code object raises only exec";
    EXPECT_TRUE(denied([&] { builtins.attr("eval")("1", py::dict()); }));
    EXPECT_TRUE(denied([&] { builtins.attr("compile")("x = 1", "<string>", "exec"); }));
  }
  {
    detail::ScopedAuditGuard compatible(detail::Profile::kCompatible);
    EXPECT_FALSE(denied([&] { builtins.attr("exec")("x = 1", py::dict()); }));
    EXPECT_FALSE(denied([&] { builtins.attr("eval")("1", py::dict()); }));
  }
}

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
    detail::ScopedAuditGuard armed(detail::Profile::kCompatible);
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
    detail::ScriptCallScope scope(
        std::make_shared<native::MemoryBudget>(std::size_t{1} << 20), 10000, detail::Profile::kCompatible);
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

// A block of zero bytes still holds a block, and its budget must stay alive for it: every block charges at least a
// byte.
TEST(PythonSandbox, EveryBlockChargesAtLeastOneByte) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  for (const auto domain : {PYMEM_DOMAIN_RAW, PYMEM_DOMAIN_MEM, PYMEM_DOMAIN_OBJ}) {
    PyMemAllocatorEx current{};
    PyMem_GetAllocator(domain, &current);
    auto budget = std::make_shared<native::MemoryBudget>(1024);
    detail::ScopedAllocationCap cap(budget);
    void* empty = current.malloc(current.ctx, 0);
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(budget->used(), 1u);
    empty = current.realloc(current.ctx, empty, 64);
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(budget->used(), 64u);
    empty = current.realloc(current.ctx, empty, 0);
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(budget->used(), 1u);
    current.free(current.ctx, empty);
    EXPECT_EQ(budget->used(), 0u);
  }
}

// A realloc is judged by its new size and charged exactly: growing across the 1 KiB line is refused past the cap and
// leaves the block as it was, shrinking across it succeeds, and below the cap every size is accounted to the byte.
TEST(PythonSandbox, ReallocAcrossTheLargeBlockLineIsAccountedExactly) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  PyMemAllocatorEx current{};
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &current);
  auto budget = std::make_shared<native::MemoryBudget>(std::size_t{64} << 10);
  detail::ScopedAllocationCap cap(budget);
  auto* block = static_cast<unsigned char*>(current.malloc(current.ctx, 512));
  ASSERT_NE(block, nullptr);
  std::memset(block, 0x5a, 512);
  for (const std::size_t size : {std::size_t{2048}, std::size_t{100}, std::size_t{5000}, std::size_t{1500}}) {
    block = static_cast<unsigned char*>(current.realloc(current.ctx, block, size));
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block[0], 0x5a) << "the content moves with the block";
    EXPECT_EQ(budget->used(), size);
  }
  ASSERT_TRUE(budget->tryAcquire(budget->limit() - budget->used()));  // the cap reached
  std::vector<void*> smalls;
  while (budget->used() <= budget->limit() + 600) {
    smalls.push_back(current.malloc(current.ctx, 100));
  }
  const std::size_t before = budget->used();
  EXPECT_EQ(current.realloc(current.ctx, smalls.front(), 4096), nullptr) << "a small block cannot become large";
  EXPECT_TRUE(cap.refused());
  EXPECT_EQ(budget->used(), before) << "and stays as it was";
  smalls.front() = current.realloc(current.ctx, smalls.front(), 100);  // still a valid, tracked block
  ASSERT_NE(smalls.front(), nullptr);
  block = static_cast<unsigned char*>(current.realloc(current.ctx, block, 200));
  ASSERT_NE(block, nullptr) << "a large block shrinks to a small one";
  EXPECT_EQ(budget->used(), before - 1300);
  current.free(current.ctx, block);
  for (void* each : smalls) {
    current.free(current.ctx, each);
  }
  EXPECT_EQ(budget->used(), budget->limit() - 1500) << "only the manual acquire remains";
}

// Many live blocks, resized and freed in any order, are charged and released exactly.
TEST(PythonSandbox, ManyBlocksFreedInAnyOrderReleaseTheirChargeExactly) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  PyMemAllocatorEx current{};
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &current);
  auto budget = std::make_shared<native::MemoryBudget>(std::size_t{1} << 30);
  std::mt19937 random(7);
  std::vector<std::pair<void*, std::size_t>> blocks;
  {
    detail::ScriptCallScope scope(budget, 60000, detail::Profile::kCompatible);
    std::size_t expected = 0;
    for (int i = 0; i < 60000; ++i) {
      const std::size_t size = 1 + random() % 3000;
      void* block = current.malloc(current.ctx, size);
      ASSERT_NE(block, nullptr);
      blocks.emplace_back(block, size);
      expected += size;
    }
    EXPECT_EQ(budget->used(), expected);
    std::shuffle(blocks.begin(), blocks.end(), random);
    for (std::size_t i = 0; i < blocks.size(); i += 3) {  // a third are resized, many across 1 KiB
      const std::size_t size = 1 + random() % 3000;
      void* moved = current.realloc(current.ctx, blocks[i].first, size);
      ASSERT_NE(moved, nullptr);
      expected = expected - blocks[i].second + size;
      blocks[i] = {moved, size};
    }
    EXPECT_EQ(budget->used(), expected);
    EXPECT_FALSE(scope.overBudget());
    for (std::size_t i = 0; i < blocks.size() / 2; ++i) {
      current.free(current.ctx, blocks[i].first);
      expected -= blocks[i].second;
    }
    EXPECT_EQ(budget->used(), expected);
    blocks.erase(blocks.begin(), blocks.begin() + static_cast<std::ptrdiff_t>(blocks.size() / 2));
  }
  for (const auto& [block, size] : blocks) {  // after the scope, with no cap armed
    current.free(current.ctx, block);
  }
  EXPECT_EQ(budget->used(), 0u);
}

// A block outlives its scope and its caller's reference to the budget: the budget stays alive for it, and the free
// (here on another thread) still reaches it.
TEST(PythonSandbox, ABlockKeepsItsBudgetAliveUntilItIsFreedOnAnyThread) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  PyMemAllocatorEx current{};
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &current);
  auto budget = std::make_shared<native::MemoryBudget>(std::size_t{1} << 20);
  const std::weak_ptr<native::MemoryBudget> weak = budget;
  void* block = nullptr;
  {
    detail::ScopedAllocationCap cap(budget);
    budget.reset();
    block = current.malloc(current.ctx, 300);
    ASSERT_NE(block, nullptr);
  }
  ASSERT_FALSE(weak.expired()) << "a live block holds its budget";
  EXPECT_EQ(weak.lock()->used(), 300u);
  {
    py::gil_scoped_release no_gil;
    std::thread([&] {
      py::gil_scoped_acquire thread_gil;
      current.free(current.ctx, block);
    }).join();
  }
  if (const auto still = weak.lock()) {
    EXPECT_EQ(still->used(), 0u) << "the other thread released the charge";
  }
}

// A budget that no live block holds goes with its scope, and the ones kept for their blocks do not pile up once those
// are freed.
TEST(PythonSandbox, BudgetsAreNotKeptOnceTheirBlocksAreFreed) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  PyMemAllocatorEx current{};
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &current);
  std::weak_ptr<native::MemoryBudget> clean;
  {
    auto budget = std::make_shared<native::MemoryBudget>(std::size_t{1} << 20);
    clean = budget;
    detail::ScopedAllocationCap cap(std::move(budget));
    current.free(current.ctx, current.malloc(current.ctx, 100));
  }
  EXPECT_TRUE(clean.expired()) << "nothing was left behind";
  std::vector<std::weak_ptr<native::MemoryBudget>> kept;
  for (int i = 0; i < 2000; ++i) {
    void* block = nullptr;
    {
      auto budget = std::make_shared<native::MemoryBudget>(std::size_t{1} << 20);
      kept.emplace_back(budget);
      detail::ScopedAllocationCap cap(std::move(budget));
      block = current.malloc(current.ctx, 100);
    }
    current.free(current.ctx, block);
  }
  const auto alive = std::ranges::count_if(kept, [](const auto& weak) { return !weak.expired(); });
  EXPECT_LT(alive, 512) << "a budget with nothing charged is dropped at a later sweep";
}

// A filter's budget spans its calls: each call's scope ends with the instance's state still live, and the budget is
// kept once, not once per call.
TEST(PythonSandbox, ABudgetReusedAcrossCallsIsKeptOnce) {
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  PyMemAllocatorEx current{};
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &current);
  auto budget = std::make_shared<native::MemoryBudget>(std::size_t{1} << 20);
  void* state = nullptr;
  {
    detail::ScopedAllocationCap cap(budget);
    state = current.malloc(current.ctx, 100);
  }
  for (int call = 0; call < 1000; ++call) {
    detail::ScopedAllocationCap cap(budget);
  }
  EXPECT_EQ(budget.use_count(), 2) << "the caller's, and the one kept for the live block";
  current.free(current.ctx, state);
}

// The RAW domain is called without the GIL, from any thread: blocks allocated under several threads' caps, resized and
// freed by other threads with no cap armed, all give back their charge.
TEST(PythonSandbox, RawBlocksPassBetweenThreadsWithoutTheGil) {
  detail::ensureInterpreter();
  PyMemAllocatorEx raw{};
  PyMem_GetAllocator(PYMEM_DOMAIN_RAW, &raw);
  constexpr int kThreads = 4;
  constexpr int kRounds = 20000;
  std::vector<std::shared_ptr<native::MemoryBudget>> budgets;
  for (int i = 0; i < kThreads; ++i) {
    budgets.push_back(std::make_shared<native::MemoryBudget>(std::size_t{1} << 30));
  }
  std::mutex mutex;
  std::vector<void*> handed;
  const auto drain = [&](bool all) {
    std::vector<void*> mine;
    {
      std::lock_guard lock(mutex);
      mine.swap(handed);
    }
    for (std::size_t i = 0; i < mine.size(); ++i) {
      if (all || i % 2 == 0) {
        raw.free(raw.ctx, raw.realloc(raw.ctx, mine[i], 700 + i));  // resized, then freed, outside any cap
      } else {
        std::lock_guard lock(mutex);
        handed.push_back(mine[i]);
      }
    }
  };
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      detail::ScopedAllocationCap cap(budgets[t]);
      for (int round = 0; round < kRounds; ++round) {
        void* block = raw.malloc(raw.ctx, 16 + static_cast<std::size_t>(round) % 2000);
        if (round % 3 == 0) {
          raw.free(raw.ctx, block);
        } else {
          std::lock_guard lock(mutex);
          handed.push_back(block);
        }
        if (round % 16 == 0 && t % 2 == 0) {
          drain(false);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  drain(true);
  drain(true);
  for (const auto& budget : budgets) {
    EXPECT_EQ(budget->used(), 0u);
  }
}

// Scopes on fresh budgets end on several threads, each leaving RAW blocks that the next thread frees with no cap armed,
// while every scope end retires its budget and keeps triggering sweeps. A budget must outlive each of its blocks (the
// freeing thread checks before the free that it has not expired) and must be gone once they are all freed.
TEST(PythonSandbox, RetiredBudgetsOutliveBlocksFreedByOtherThreadsAndThenExpire) {
  detail::ensureInterpreter();
  PyMemAllocatorEx raw{};
  PyMem_GetAllocator(PYMEM_DOMAIN_RAW, &raw);
  constexpr int kThreads = 4;
  constexpr int kRounds = 3000;
  constexpr int kBlocksPerRound = 3;
  struct Handoff {
    void* block;
    std::weak_ptr<native::MemoryBudget> budget;
  };
  struct Mailbox {
    std::mutex mutex;
    std::vector<Handoff> items;
  };
  std::vector<Mailbox> mailboxes(kThreads);
  std::vector<std::vector<std::weak_ptr<native::MemoryBudget>>> made(kThreads);
  std::atomic<int> expired_early{0};
  std::atomic<int> started{0};

  const auto free_all = [&](int self) {
    std::vector<Handoff> mine;
    {
      std::lock_guard lock(mailboxes[self].mutex);
      mine.swap(mailboxes[self].items);
    }
    for (const Handoff& item : mine) {
      if (item.budget.expired()) {  // a live block holds its budget
        ++expired_early;
      }
      raw.free(raw.ctx, item.block);
    }
  };
  std::vector<std::thread> threads;
  for (int self = 0; self < kThreads; ++self) {
    threads.emplace_back([&, self] {
      ++started;
      while (started.load() < kThreads) {  // start together
        std::this_thread::yield();
      }
      for (int round = 0; round < kRounds; ++round) {
        {
          auto budget = std::make_shared<native::MemoryBudget>(std::size_t{1} << 30);
          made[self].emplace_back(budget);
          detail::ScopedAllocationCap cap(std::move(budget));  // the only owner; its end retires the budget
          std::lock_guard lock(mailboxes[(self + 1) % kThreads].mutex);
          for (int block = 0; block < kBlocksPerRound; ++block) {
            mailboxes[(self + 1) % kThreads].items.push_back(
                {raw.malloc(raw.ctx, 16 + static_cast<std::size_t>(round + block) % 1500), made[self].back()});
          }
        }
        if (round % 4 == 0) {
          free_all(self);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  for (int self = 0; self < kThreads; ++self) {
    free_all(self);
  }
  EXPECT_EQ(expired_early.load(), 0) << "a budget was destroyed while one of its blocks was live";

  // Sweeps are lazy: pin and unpin budgets until the ones above have been swept.
  const auto all_expired = [&] {
    return std::ranges::all_of(made, [](const auto& weaks) {
      return std::ranges::all_of(weaks, [](const auto& weak) { return weak.expired(); });
    });
  };
  for (int i = 0; i < 1'000'000 && !(i % 64 == 0 && all_expired()); ++i) {
    void* kept = nullptr;
    {
      detail::ScopedAllocationCap cap(std::make_shared<native::MemoryBudget>(std::size_t{1} << 20));
      kept = raw.malloc(raw.ctx, 64);  // the scope ends with it live, so its budget is retired and kept
    }
    raw.free(raw.ctx, kept);
  }
  EXPECT_TRUE(all_expired());
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
    detail::ScriptCallScope scope(std::make_shared<native::MemoryBudget>(kCap), 10000, detail::Profile::kCompatible);
    try {
      detail::runCode(code, globals);
    } catch (py::error_already_set&) {}
    over_budget = scope.overBudget();
    EXPECT_FALSE(scope.timedOut());
  }
  const auto tuples = globals["n"].cast<std::size_t>();
  constexpr std::size_t kTupleBytes = 48;  // a 1-tuple and its GC header
  EXPECT_GT(tuples * kTupleBytes, kCap * 3 / 2) << "small objects ran past the cap";
  // The stop is asynchronous (the watchdog raises at the next bytecode after a GIL switch), so a scheduler tick of
  // allocations lands past twice the cap: a Windows runner measured 3.0008 times it.
  EXPECT_LT(tuples * kTupleBytes, kCap * 4) << "and were stopped soon after twice the cap";
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
    detail::ScriptCallScope scope(
        std::make_shared<native::MemoryBudget>(std::size_t{64} << 10), 10000, detail::Profile::kCompatible);
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
    detail::ScriptCallScope scope(
        std::make_shared<native::MemoryBudget>(std::size_t{64} << 10), 10000, detail::Profile::kCompatible);
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

// ---- a failed boot -----------------------------------------------------------------------------

/// A failed boot lasts for the process, so these cases run only alone, as ctest runs every case.
bool runsAlone() {
  return ::testing::UnitTest::GetInstance()->test_to_run_count() == 1;
}

void setEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  ::setenv(name, value.c_str(), 1);
#endif
}

std::filesystem::path freshDirectory() {
  auto dir = std::filesystem::temp_directory_path() / ("pj_python_boot_" + std::to_string(std::random_device{}()));
  std::filesystem::create_directories(dir);
  return dir;
}

std::string bootError() {
  try {
    detail::ensureInterpreter();
  } catch (const std::exception& error) {
    return error.what();
  }
  return {};
}

/// `work()` on a detached thread, waited for at most 5 s: nullopt when it hangs, instead of hanging the test.
template <typename Work>
std::optional<std::invoke_result_t<Work>> onAnotherThread(Work work) {
  auto task = std::make_shared<std::packaged_task<std::invoke_result_t<Work>()>>(std::move(work));
  auto result = task->get_future();
  std::thread([task] { (*task)(); }).detach();
  if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    return std::nullopt;
  }
  return result.get();
}

// The interpreter starts and an import of the boot fails (a `json` shadowing the stdlib's): every later call, on any
// thread, reports that failure, and the GIL is not left held by the booting thread.
TEST(PythonBootFailure, AFailedImportIsReportedToEveryCallAndLeavesTheGilFree) {
  if (!runsAlone()) {
    GTEST_SKIP() << "a failed boot lasts for the process: run this case alone";
  }
  ASSERT_EQ(Py_IsInitialized(), 0) << "Python booted before the test: it cannot fail the boot";
  const std::filesystem::path dir = freshDirectory();
  std::ofstream(dir / "json.py") << "raise RuntimeError('injected boot failure')\n";
  setEnvironment("PYTHONPATH", dir.string());
  const std::string first = bootError();
  EXPECT_NE(first.find("injected boot failure"), std::string::npos) << first;
  EXPECT_EQ(bootError(), first);
  EXPECT_TRUE(onAnotherThread([] {
                py::gil_scoped_acquire gil;
                return true;
              }).has_value())
      << "the booting thread still holds the GIL";
  const auto compiled =
      onAnotherThread([] { return compileOnDemandScriptPython("def evaluate(inputs, params):\n    return 1\n"); });
  ASSERT_TRUE(compiled.has_value()) << "a compile on another thread hung";
  ASSERT_FALSE(*compiled);
  EXPECT_NE(compiled->error().find("injected boot failure"), std::string::npos) << compiled->error();
  const auto filters = onAnotherThread([] { return makePythonEngine()->inspectModule(filter("pass"), "test"); });
  ASSERT_TRUE(filters.has_value()) << "a filter inspection on another thread hung";
  ASSERT_FALSE(*filters);
  EXPECT_NE(filters->error().find("injected boot failure"), std::string::npos) << filters->error();
  std::filesystem::remove_all(dir);
}

// The interpreter itself fails to start (no stdlib under PYTHONHOME): pybind11 throws CPython's status, and every
// later call reports it instead of starting the half-initialized runtime again.
TEST(PythonBootFailure, AnInterpreterThatCannotStartIsReportedToEveryCall) {
  if (!runsAlone()) {
    GTEST_SKIP() << "a failed boot lasts for the process: run this case alone";
  }
  ASSERT_EQ(Py_IsInitialized(), 0) << "Python booted before the test: it cannot fail the boot";
  const std::filesystem::path dir = freshDirectory();
  setEnvironment("PYTHONHOME", dir.string());
  const std::string first = bootError();
  EXPECT_FALSE(first.empty());
  EXPECT_EQ(bootError(), first);
  const auto compiled =
      onAnotherThread([] { return compileOnDemandScriptPython("def evaluate(inputs, params):\n    return 1\n"); });
  ASSERT_TRUE(compiled.has_value()) << "a compile on another thread hung";
  ASSERT_FALSE(*compiled);
  EXPECT_NE(compiled->error().find(first), std::string::npos) << compiled->error();
  std::filesystem::remove_all(dir);
}

// PJ_BENCH=1 prints the best-of-N ns per element of five loops (the last allocates nothing, a control), run bare (no
// cap armed: the wrapper installed, forwarding) and inside a ScriptCallScope with the cap armed, whose limit is never
// reached so only the bookkeeping is timed. Pin it to one core (`taskset -c 2`): the ratio is the cost of the cap.
TEST(PythonSandboxBench, AllocationCapOverhead) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  detail::ensureInterpreter();
  py::gil_scoped_acquire gil;
  constexpr int kElements = 1'000'000;
  constexpr int kRuns = 9;
  py::dict globals;
  globals["__builtins__"] = py::module_::import("builtins");
  py::exec(
      R"PY(
def sum_range(n):
    return sum(range(n))

def list_append(n):
    items = []
    for i in range(n):
        items.append(i)
    return len(items)

def dict_build(n):
    table = {}
    for i in range(n):
        table[i] = i
    return len(table)

def live_bytes_600(n):
    items = []
    for i in range(n):
        items.append(bytes(600))
    return len(items)

def arithmetic(n):
    x = 0.0
    for v in VALUES:
        x = x * 0.999 + v
    return x

VALUES = [1.5] * 1000000
)PY",
      globals);
  const auto best_nanos = [&](const char* name, bool capped) {
    const int elements = std::strcmp(name, "live_bytes_600") == 0 ? kElements / 5 : kElements;  // 120 MB live
    double best = 1e30;
    for (int run = 0; run < kRuns; ++run) {
      std::unique_ptr<detail::ScriptCallScope> scope;
      if (capped) {
        scope = std::make_unique<detail::ScriptCallScope>(
            std::make_shared<native::MemoryBudget>(std::size_t{4} << 30), 600'000, detail::Profile::kCompatible);
      }
      const auto begin = std::chrono::steady_clock::now();
      globals[name](elements);
      best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count());
    }
    return best / elements;
  };
  std::printf("%-12s %10s %10s %8s\n", "loop", "bare ns", "capped ns", "ratio");
  for (const char* name : {"sum_range", "list_append", "dict_build", "live_bytes_600", "arithmetic"}) {
    (void)best_nanos(name, false);  // warm-up
    const double bare = best_nanos(name, false);
    const double capped = best_nanos(name, true);
    std::printf("%-12s %10.1f %10.1f %7.2fx\n", name, bare, capped, capped / bare);
  }
  globals.clear();
}

}  // namespace
}  // namespace PJ::scripting
