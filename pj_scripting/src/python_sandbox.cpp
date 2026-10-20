// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "python_sandbox.h"

#include <Python.h>
#include <pybind11/embed.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <utility>

#include "pj_base/sdk/platform.hpp"
#include "pj_scripting/sandbox.h"

namespace PJ::scripting::detail {
namespace {

// ---- the audit hook ---------------------------------------------------------------------

// Import roots the hook refuses: a backstop for a denied root reached by another path than the gate. A denylist, not
// the policy's complement, because "import" also fires for the allowed modules' own dependencies.
constexpr std::array<std::string_view, 9> kDeniedImportRoots{"os",      "posix",  "nt",      "subprocess", "socket",
                                                             "_socket", "ctypes", "_ctypes", "importlib"};

constexpr std::array<std::string_view, 6> kDeniedEventPrefixes{"os.",     "shutil.", "subprocess.",
                                                               "socket.", "ctypes.", "_winapi."};

constexpr std::array<std::string_view, 3> kDeniedEvents{"open", "sys._getframe", "sys.addaudithook"};

thread_local int g_audit_armed_depth = 0;

int deny(std::string_view event) {
  PyErr_SetString(PyExc_PermissionError, (std::string(event) + " is not available in Data Processors").c_str());
  return -1;
}

int auditHook(const char* event, PyObject* args, void* /*userdata*/) {
  if (g_audit_armed_depth <= 0) {
    return 0;
  }
  const std::string_view name(event);
  if (std::ranges::find(kDeniedEvents, name) != kDeniedEvents.end() ||
      std::ranges::any_of(kDeniedEventPrefixes, [&](std::string_view prefix) { return name.starts_with(prefix); })) {
    return deny(name);
  }
  if (name == "import" && args != nullptr && PyTuple_Check(args) != 0 && PyTuple_GET_SIZE(args) >= 1) {
    PyObject* module = PyTuple_GET_ITEM(args, 0);  // borrowed
    Py_ssize_t length = 0;
    const char* text = PyUnicode_Check(module) != 0 ? PyUnicode_AsUTF8AndSize(module, &length) : nullptr;
    if (text == nullptr) {
      PyErr_Clear();
      return 0;
    }
    const std::string_view module_name(text, static_cast<std::size_t>(length));
    const std::string_view root = module_name.substr(0, module_name.find('.'));
    if (std::ranges::find(kDeniedImportRoots, root) != kDeniedImportRoots.end()) {
      return deny("import of " + std::string(root));
    }
  }
  return 0;
}

// ---- the allocation cap -------------------------------------------------------------------

// Ownership comes from a side table, never from reading outside a block, so blocks allocated before the wrapper or
// outside any cap stay valid inputs to the original allocator. Nested domain calls (OBJ through MEM through RAW) are
// charged once.
struct AllocationRecord {
  std::size_t size = 0;
  std::shared_ptr<native::MemoryBudget> budget;
};

struct AllocatorState {
  PyMemAllocatorEx original{};
  std::mutex mutex;
  std::unordered_map<void*, AllocationRecord> owned;
  /// `owned.size()`, read without the lock so a free outside every cap skips it while nothing is owned.
  std::atomic<std::size_t> owned_count{0};
};

thread_local unsigned g_allocator_depth = 0;
thread_local std::shared_ptr<native::MemoryBudget> g_python_budget;
thread_local bool g_python_budget_refused = false;

/// The smallest block the cap refuses. CPython mishandles some failed small allocations: 3.12's MAKE_FUNCTION releases
/// its code object twice when the function cannot be allocated (`Py_DECREF(codeobj)` then `goto error`,
/// https://github.com/python/cpython/blob/v3.12.7/Python/generated_cases.c.h, fixed in 3.14). So a small block is
/// always charged and served, and a run of small objects is stopped at twice the cap by an asynchronous MemoryError.
constexpr std::size_t kSmallestRefusedBlock = 1024;

/// Charges `bytes` of a `block_bytes` allocation to `budget`. Past the limit a large block is refused; a small one is
/// charged anyway, and the first to pass twice the limit asks the watchdog to stop the call. Either is latched for
/// ScopedAllocationCap::refused() only when `budget` is the current call's: a block an earlier call left behind is
/// charged to that call's budget, which may refuse it but does not fail this call.
bool charge(native::MemoryBudget& budget, std::size_t bytes, std::size_t block_bytes) {
  const bool current = &budget == g_python_budget.get();
  if (block_bytes >= kSmallestRefusedBlock) {
    const bool charged = budget.tryAcquire(bytes);
    g_python_budget_refused = g_python_budget_refused || (!charged && current);
    return charged;
  }
  const std::size_t stop = budget.limit() > SIZE_MAX / 2 ? SIZE_MAX : 2 * budget.limit();
  const std::size_t used = budget.acquireAnyway(bytes);
  if (current && used > stop && used - bytes <= stop) {
    g_python_budget_refused = true;
    PersistentWatchdog::stopForMemory();
  }
  return true;
}

struct AllocatorCall {
  AllocatorCall() {
    ++g_allocator_depth;
  }
  ~AllocatorCall() {
    --g_allocator_depth;
  }
  AllocatorCall(const AllocatorCall&) = delete;
  AllocatorCall& operator=(const AllocatorCall&) = delete;
};

// Leaked: blocks may be freed during interpreter teardown, after static destruction.
std::array<AllocatorState, 3>& allocatorStates() {
  static auto* states = new std::array<AllocatorState, 3>;
  return *states;
}

void* cappedMalloc(void* context, std::size_t size) {
  auto& state = *static_cast<AllocatorState*>(context);
  if (g_allocator_depth != 0 || !g_python_budget) {
    return state.original.malloc(state.original.ctx, size);
  }
  AllocatorCall call;
  const std::shared_ptr<native::MemoryBudget>& budget = g_python_budget;
  if (!charge(*budget, size, size)) {
    return nullptr;
  }
  void* block = state.original.malloc(state.original.ctx, size);
  if (block == nullptr) {
    budget->release(size);
    return nullptr;
  }
  try {
    std::lock_guard lock(state.mutex);
    state.owned.emplace(block, AllocationRecord{size, budget});
    state.owned_count.store(state.owned.size(), std::memory_order_release);
  } catch (...) {
    state.original.free(state.original.ctx, block);
    budget->release(size);
    return nullptr;
  }
  return block;
}

void* cappedCalloc(void* context, std::size_t count, std::size_t size) {
  auto& state = *static_cast<AllocatorState*>(context);
  if (g_allocator_depth != 0 || !g_python_budget) {
    return state.original.calloc(state.original.ctx, count, size);
  }
  if (count != 0 && size > SIZE_MAX / count) {
    g_python_budget_refused = true;
    return nullptr;
  }
  void* block = cappedMalloc(context, count * size);
  if (block != nullptr) {
    std::memset(block, 0, count * size);
  }
  return block;
}

void* cappedRealloc(void* context, void* block, std::size_t size) {
  auto& state = *static_cast<AllocatorState*>(context);
  if (g_allocator_depth != 0) {
    return state.original.realloc(state.original.ctx, block, size);
  }
  if (block == nullptr) {
    return cappedMalloc(context, size);
  }
  if (!g_python_budget && state.owned_count.load(std::memory_order_acquire) == 0) {
    return state.original.realloc(state.original.ctx, block, size);
  }
  AllocatorCall call;
  std::lock_guard lock(state.mutex);
  auto found = state.owned.find(block);
  if (found == state.owned.end() && !g_python_budget) {
    return state.original.realloc(state.original.ctx, block, size);
  }
  const bool known = found != state.owned.end();
  const std::size_t old_size = known ? found->second.size : 0;
  std::shared_ptr<native::MemoryBudget> budget = known ? found->second.budget : g_python_budget;
  const std::size_t growth = size > old_size ? size - old_size : 0;
  if (!charge(*budget, growth, size)) {
    return nullptr;
  }
  // The record exists before realloc, whose success invalidates `block`.
  try {
    if (!known) {
      found = state.owned.emplace(block, AllocationRecord{0, budget}).first;
    }
  } catch (...) {
    budget->release(growth);
    return nullptr;
  }
  auto node = state.owned.extract(found);
  // CPython wants a distinct block for zero bytes; realloc(block, 0) may free it on some platforms.
  void* moved = state.original.realloc(state.original.ctx, block, std::max(size, std::size_t{1}));
  if (moved == nullptr) {
    budget->release(growth);
    if (known) {
      state.owned.insert(std::move(node));
    }
    state.owned_count.store(state.owned.size(), std::memory_order_release);
    return nullptr;
  }
  if (old_size > size) {
    budget->release(old_size - size);
  }
  node.key() = moved;
  node.mapped().size = size;
  state.owned.insert(std::move(node));
  state.owned_count.store(state.owned.size(), std::memory_order_release);
  return moved;
}

void cappedFree(void* context, void* block) {
  auto& state = *static_cast<AllocatorState*>(context);
  if (block == nullptr) {
    return;
  }
  if (g_allocator_depth == 0 && state.owned_count.load(std::memory_order_acquire) != 0) {
    AllocatorCall call;
    std::lock_guard lock(state.mutex);
    if (auto found = state.owned.find(block); found != state.owned.end()) {
      found->second.budget->release(found->second.size);
      state.owned.erase(found);
      state.owned_count.store(state.owned.size(), std::memory_order_release);
    }
  }
  state.original.free(state.original.ctx, block);
}

// PyMem_SetAllocator is not thread-safe: called by the boot, while only the booting thread runs Python.
void installAllocatorWrapper() {
  constexpr std::array domains{PYMEM_DOMAIN_RAW, PYMEM_DOMAIN_MEM, PYMEM_DOMAIN_OBJ};
  auto& states = allocatorStates();
  for (std::size_t i = 0; i < domains.size(); ++i) {
    PyMem_GetAllocator(domains[i], &states[i].original);
    PyMemAllocatorEx wrapper{&states[i], &cappedMalloc, &cappedCalloc, &cappedRealloc, &cappedFree};
    PyMem_SetAllocator(domains[i], &wrapper);
  }
}

/// The calling thread's live arming, for the allocator's memory stop.
thread_local PersistentWatchdog::Slot* g_armed_slot = nullptr;

std::int64_t steadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool isArmed(const PersistentWatchdog::Slot& slot) {
  return slot.armed_generation.load(std::memory_order_acquire) != 0;
}

}  // namespace

void ensureInterpreter() {
  static const bool booted = []() {
#ifdef PJ_PYTHON_HOME
    // The stdlib lives under the CPython prefix baked at build time, unless the user set PYTHONHOME.
    if (!sdk::getEnv("PYTHONHOME")) {
#ifdef _WIN32
      _putenv_s("PYTHONHOME", PJ_PYTHON_HOME);
#else
      ::setenv("PYTHONHOME", PJ_PYTHON_HOME, 1);
#endif
    }
#endif
    new py::scoped_interpreter();  // NOLINT: leaked on purpose, never finalized
    PySys_AddAuditHook(&auditHook, nullptr);
    installAllocatorWrapper();
    for (const char* name : {"json", "math", "cmath", "statistics"}) {
      py::module_::import(name);
    }
    new py::gil_scoped_release();  // NOLINT: leaked on purpose, the GIL is handed to the callers
    return true;
  }();
  (void)booted;
}

void runCode(py::handle code, const py::dict& globals) {
  PyObject* result = PyEval_EvalCode(code.ptr(), globals.ptr(), globals.ptr());
  if (result == nullptr) {
    throw py::error_already_set();
  }
  Py_DECREF(result);
}

std::string pythonErrorText(const std::exception& e) {
  std::string text = e.what();
  if (const std::size_t frames = text.find("\n\nAt:"); frames != std::string::npos) {
    text.erase(frames);
  }
  return text;
}

ScopedAuditGuard::ScopedAuditGuard() {
  ++g_audit_armed_depth;
}

ScopedAuditGuard::~ScopedAuditGuard() {
  --g_audit_armed_depth;
}

ScopedAllocationCap::ScopedAllocationCap(std::shared_ptr<native::MemoryBudget> budget)
    : previous_(std::exchange(g_python_budget, std::move(budget))),
      previous_refused_(std::exchange(g_python_budget_refused, false)) {}

ScopedAllocationCap::~ScopedAllocationCap() {
  g_python_budget = std::move(previous_);
  g_python_budget_refused = previous_refused_;
}

bool ScopedAllocationCap::refused() const {
  return g_python_budget_refused;
}

std::string timeoutMessage(std::string_view call, std::uint64_t max_millis) {
  return "TimeoutError: " + std::string(call) + " exceeded its time budget of " + std::to_string(max_millis) + " ms";
}

ScriptCallScope::ScriptCallScope(std::shared_ptr<native::MemoryBudget> memory, std::uint64_t max_millis)
    : cap_(std::move(memory)) {
  PersistentWatchdog::arm(max_millis);
}

ScriptCallScope::~ScriptCallScope() {
  (void)PersistentWatchdog::disarm();
}

bool ScriptCallScope::timedOut() {
  return PersistentWatchdog::disarm();
}

// ---- the watchdog -------------------------------------------------------------------------------

PersistentWatchdog& PersistentWatchdog::instance() {
  static PersistentWatchdog* const watchdog = new PersistentWatchdog();  // NOLINT: never destroyed
  return *watchdog;
}

PersistentWatchdog::PersistentWatchdog() {
  std::thread([this]() { run(); }).detach();
}

PersistentWatchdog::Slot& PersistentWatchdog::currentSlot() {
  struct Holder {
    std::shared_ptr<Slot> slot = std::make_shared<Slot>();
    Holder() {
      PersistentWatchdog& self = instance();
      std::lock_guard lock(self.mutex_);
      self.slots_.push_back(slot);
    }
    ~Holder() {
      PersistentWatchdog& self = instance();
      std::lock_guard lock(self.mutex_);
      std::erase(self.slots_, slot);
    }
    Holder(const Holder&) = delete;
    Holder& operator=(const Holder&) = delete;
  };
  thread_local Holder holder;
  return *holder.slot;
}

void PersistentWatchdog::arm(std::uint64_t max_millis) {
  Slot& slot = currentSlot();
  const std::uint64_t generation = ++slot.generation;
  slot.thread_id.store(PyThread_get_thread_ident(), std::memory_order_relaxed);
  const std::int64_t timeout = steadyNowNs() + static_cast<std::int64_t>(max_millis) * 1'000'000;
  slot.timeout_ns.store(timeout, std::memory_order_relaxed);
  slot.deadline_ns.store(timeout, std::memory_order_relaxed);
  slot.memory_stop.store(false, std::memory_order_relaxed);
  g_armed_slot = &slot;
  PersistentWatchdog& self = instance();
  if (slot.armed_generation.exchange(generation, std::memory_order_seq_cst) == 0) {
    self.armed_slots_.fetch_add(1, std::memory_order_seq_cst);
  }
  self.arms_total_.fetch_add(1, std::memory_order_relaxed);
  if (self.parked_.load(std::memory_order_seq_cst)) {  // only the first arming after a park pays for the wakeup
    {
      std::lock_guard lock(self.mutex_);
      self.parked_.store(false, std::memory_order_seq_cst);
    }
    self.wake_.notify_one();
  }
}

bool PersistentWatchdog::disarm() {
  Slot& slot = currentSlot();
  const std::uint64_t generation = slot.armed_generation.exchange(0, std::memory_order_acq_rel);
  if (generation == 0) {
    return false;
  }
  g_armed_slot = nullptr;
  instance().armed_slots_.fetch_sub(1, std::memory_order_seq_cst);
  if (slot.fired_generation.load(std::memory_order_relaxed) == generation) {
    // The request may still be pending (no safepoint reached, or the script caught it and returned).
    PyThreadState_SetAsyncExc(slot.thread_id.load(std::memory_order_relaxed), nullptr);
  }
  return steadyNowNs() >= slot.timeout_ns.load(std::memory_order_relaxed);
}

void PersistentWatchdog::stopForMemory() noexcept {
  Slot* slot = g_armed_slot;
  if (slot == nullptr) {
    return;
  }
  slot->memory_stop.store(true, std::memory_order_relaxed);
  slot->deadline_ns.store(0, std::memory_order_release);
  // Under the watchdog's mutex, so the wakeup cannot fall between its reading the deadlines and its waiting. Safe from
  // an allocator: locking allocates nothing, and the watchdog never allocates Python memory or takes the GIL holding
  // it.
  PersistentWatchdog& self = instance();
  std::lock_guard lock(self.mutex_);
  self.wake_.notify_one();
}

void PersistentWatchdog::run() {
  std::int64_t last_active_ns = steadyNowNs();
  std::uint64_t seen_arms = 0;
  std::vector<std::shared_ptr<Slot>> expired;
  while (true) {
    expired.clear();
    {
      std::unique_lock lock(mutex_);
      const std::int64_t now = steadyNowNs();
      const std::uint64_t arms = arms_total_.load(std::memory_order_relaxed);
      if (arms != seen_arms || armed_slots_.load(std::memory_order_acquire) != 0) {
        seen_arms = arms;
        last_active_ns = now;
      }
      if (now - last_active_ns > kIdleParkMillis * 1'000'000) {
        parked_.store(true, std::memory_order_seq_cst);
        if (armed_slots_.load(std::memory_order_seq_cst) == 0) {
          wake_.wait(lock, [this]() { return !parked_.load(std::memory_order_seq_cst); });
        }
        parked_.store(false, std::memory_order_seq_cst);
        last_active_ns = steadyNowNs();
        continue;
      }
      std::int64_t sleep_ns = kMaxSleepMillis * 1'000'000;
      for (const auto& slot : slots_) {
        if (isArmed(*slot)) {
          sleep_ns = std::min(sleep_ns, slot->deadline_ns.load(std::memory_order_acquire) - now);
        }
      }
      wake_.wait_for(lock, std::chrono::nanoseconds(std::max<std::int64_t>(sleep_ns, 1'000'000)));
      const std::int64_t after = steadyNowNs();
      for (const auto& slot : slots_) {
        if (isArmed(*slot) && after >= slot->deadline_ns.load(std::memory_order_acquire)) {
          expired.push_back(slot);  // keeps the slot alive past its thread's exit
        }
      }
    }
    if (expired.empty() || Py_IsInitialized() == 0) {
      continue;
    }
    py::gil_scoped_acquire gil;
    // With the GIL held no disarm runs, so "still the same arming" is stable here.
    for (const auto& slot : expired) {
      const std::uint64_t armed = slot->armed_generation.load(std::memory_order_acquire);
      const std::int64_t now = steadyNowNs();
      if (armed != 0 && now >= slot->deadline_ns.load(std::memory_order_acquire)) {
        slot->fired_generation.store(armed, std::memory_order_relaxed);
        slot->deadline_ns.store(now + kRefireMillis * 1'000'000, std::memory_order_relaxed);  // again, if caught
        // Past the call's deadline a TimeoutError, which a script catching every MemoryError does not catch.
        const bool memory =
            slot->memory_stop.load(std::memory_order_relaxed) && now < slot->timeout_ns.load(std::memory_order_relaxed);
        PyThreadState_SetAsyncExc(
            slot->thread_id.load(std::memory_order_relaxed), memory ? PyExc_MemoryError : PyExc_TimeoutError);
      }
    }
  }
}

}  // namespace PJ::scripting::detail
