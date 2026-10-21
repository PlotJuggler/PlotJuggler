// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "python_sandbox.h"

#include <Python.h>
#include <pybind11/embed.h>
#include <tsl/robin_map.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "pj_base/sdk/platform.hpp"
#include "pj_scripting/sandbox.h"

namespace PJ::scripting::detail {
namespace {

// ---- the import gate and the read-only module copies -----------------------------------

// Runs once at boot with the real builtins. A copy is a class whose metaclass refuses attribute writes: class
// attribute reads stay on CPython's fast path, unlike a proxy's Python-level __getattr__.
constexpr const char* kGateSource = R"PY(
import types

class ReadOnlyModule(type):
    def __setattr__(cls, name, value):
        raise AttributeError(f"module '{cls.__name__}' is read-only in Data Processors")

    def __delattr__(cls, name):
        raise AttributeError(f"module '{cls.__name__}' is read-only in Data Processors")

    def __getattr__(cls, name):
        raise AttributeError(f"module '{cls.__name__}' has no attribute '{name}' available in Data Processors")

    def __repr__(cls):
        return f"<module '{cls.__name__}' (read-only)>"

def make_gate(real_import, allowed):
    copies = {}

    def allowed_root(name):
        return name.partition(".")[0] in allowed

    # `building` holds the copies of one outermost call (nested modules may cycle); they are published together, once
    # complete, so a failure midway leaves nothing behind.
    def freeze(module, building=None):
        copy = copies.get(module.__name__)
        if copy is not None:
            return copy
        outermost = building is None
        if outermost:
            building = {}
        copy = building.get(module.__name__)
        if copy is None:
            copy = building[module.__name__] = ReadOnlyModule(module.__name__, (), {})
            for key, value in vars(module).items():
                if key.startswith("_"):
                    continue
                if isinstance(value, types.ModuleType):
                    if not allowed_root(value.__name__):
                        continue
                    value = freeze(value, building)
                type.__setattr__(copy, key, value)
        if outermost:
            copies.update(building)
        return copy

    def gated_import(name, globals=None, locals=None, fromlist=(), level=0):
        if level != 0 or not allowed_root(name):
            raise ImportError(
                f"module '{name}' is not available in Data Processors (allowed: {', '.join(allowed)})")
        return freeze(real_import(name, globals, locals, fromlist, level))

    return gated_import
)PY";

constexpr std::array<const char*, 4> kStrictImports{"math", "cmath", "statistics", "json"};

/// What the allowed modules import lazily on a path a script reaches: statistics re-exports collections.Counter, whose
/// most_common(n) imports heapq. The browser's acceptance probe (pj_app/tests/wasm_python_stdlib_probe.cpp) finds
/// such imports in Python code; the C-level one, decimal's, is warmed by `ensureInterpreter` instead.
constexpr std::array<const char*, 1> kLazyDependencies{"heapq"};

/// The builtins a script never gets: file and console access, code from strings, and the attribute functions that
/// would rebuild by name the dunder walk `compileChecked` refuses lexically.
constexpr std::array<const char*, 11> kRemovedBuiltins{"open", "exec",    "eval",    "compile", "input", "breakpoint",
                                                       "help", "getattr", "setattr", "delattr", "type"};

py::dict* g_strict_builtins = nullptr;  // built once by the boot, copied per call, never freed

template <std::size_t N>
py::dict makeBuiltins(const py::object& make_gate, const std::array<const char*, N>& allowed) {
  py::object builtins = py::module_::import("builtins");
  py::dict out = builtins.attr("__dict__").attr("copy")();
  for (const char* name : kRemovedBuiltins) {
    out.attr("pop")(name, py::none());
  }
  py::tuple names(N);
  for (std::size_t i = 0; i < N; ++i) {
    names[i] = py::str(allowed[i]);
  }
  py::object gate = make_gate(builtins.attr("__import__"), names);
  for (const char* name : allowed) {
    gate(name);  // the copies, built now, unarmed and uncharged
  }
  out["__import__"] = gate;
  return out;
}

// ---- the audit hook ---------------------------------------------------------------------

// Import roots the hook refuses: a backstop for a denied root reached by another path than the gate. A denylist, not
// the policy's complement, because "import" also fires for the allowed modules' own dependencies.
constexpr std::array<std::string_view, 9> kDeniedImportRoots{"os",      "posix",  "nt",      "subprocess", "socket",
                                                             "_socket", "ctypes", "_ctypes", "importlib"};

constexpr std::array<std::string_view, 6> kDeniedEventPrefixes{"os.",     "shutil.", "subprocess.",
                                                               "socket.", "ctypes.", "_winapi."};

constexpr std::array<std::string_view, 3> kDeniedEvents{"open", "sys._getframe", "sys.addaudithook"};

/// Refused only under the strict profile: code from strings, which would bypass the AST check.
constexpr std::array<std::string_view, 2> kStrictDeniedEvents{"exec", "compile"};

thread_local int g_audit_armed_depth = 0;
thread_local int g_audit_strict_depth = 0;

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
      (g_audit_strict_depth > 0 && std::ranges::find(kStrictDeniedEvents, name) != kStrictDeniedEvents.end()) ||
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

/// A block's charge. The budget is not owned, because a shared_ptr copy per block costs two atomic operations: the
/// scope that charges it keeps it alive, and `LingeringBudgets` once that scope ends with blocks still live. Every
/// record charges at least one byte, so a budget with no live block is one whose `used()` is zero.
struct AllocationRecord {
  std::size_t size = 0;
  native::MemoryBudget* budget = nullptr;
};

/// Blocks are 16-byte aligned. The shift keeps neighbouring blocks in neighbouring buckets, which a loop of small
/// allocations fills and empties in order; a mixing hash scatters them across cache lines and measures 2 to 4 times
/// slower with a million live blocks. ponytail: homogeneous blocks of a few hundred bytes alias onto every n-th bucket
/// and probe a few steps further (measured 1.3 times the bare loop); mix the high bits only if that ever matters.
struct BlockHash {
  std::size_t operator()(const void* block) const noexcept {
    return reinterpret_cast<std::uintptr_t>(block) >> 4;
  }
};

#ifdef Py_GIL_DISABLED
#error "the MEM and OBJ allocation bookkeeping relies on the GIL; a free-threaded CPython needs gil_serialized = false"
#endif

struct AllocatorState {
  PyMemAllocatorEx original{};
  /// True for MEM and OBJ, whose callers must hold the GIL, which then orders every access to `owned` (CPython's own
  /// allocator relies on it too). RAW callers may not hold it, so that domain takes `mutex`. ponytail: a free-threaded
  /// CPython has no such GIL; set it false for every domain then.
  bool gil_serialized = false;
  std::mutex mutex;
  tsl::robin_map<void*, AllocationRecord, BlockHash> owned;
  /// `owned.size()`, read without the lock so a free outside every cap skips it while nothing is owned.
  std::atomic<std::size_t> owned_count{0};
};

/// Holds the domain's mutex when the GIL does not already serialize it.
class StateLock {
 public:
  explicit StateLock(AllocatorState& state) : mutex_(state.gil_serialized ? nullptr : &state.mutex) {
    if (mutex_ != nullptr) {
      mutex_->lock();
    }
  }
  ~StateLock() {
    if (mutex_ != nullptr) {
      mutex_->unlock();
    }
  }
  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;

 private:
  std::mutex* mutex_;
};

/// The calling thread's cap, one trivial struct so an allocator call pays one thread-local lookup.
struct ThreadCap {
  unsigned allocator_depth = 0;            ///< above zero while the wrapper itself allocates
  native::MemoryBudget* budget = nullptr;  ///< the live ScopedAllocationCap's
  bool refused = false;                    ///< latched for that scope
};
thread_local ThreadCap g_cap;

/// Budgets whose scope ended while blocks charged to them were still live, held until the last block is freed (their
/// `used()` is then zero).
class LingeringBudgets {
 public:
  static LingeringBudgets& instance() {
    static auto* const lingering = new LingeringBudgets;  // NOLINT: blocks are freed during teardown
    return *lingering;
  }

  /// Drops the caller's reference to a budget; the table keeps it while blocks still hold its charge. Never throws
  /// (it runs in a destructor): out of memory for the table, the budget is leaked, because freeing it would leave its
  /// blocks pointing at it. The same policy as the allocator's own table.
  void retire(std::shared_ptr<native::MemoryBudget> budget) noexcept {
    if (!budget || budget->used() == 0) {
      return;
    }
    try {
      std::lock_guard lock(mutex_);
      held_.try_emplace(budget.get(), std::move(budget));
      if (held_.size() >= sweep_at_) {  // amortized: a sweep per doubling
        std::erase_if(held_, [](const auto& entry) { return entry.second->used() == 0; });
        sweep_at_ = std::max<std::size_t>(64, 2 * held_.size());
      }
    } catch (...) {
      // try_emplace moves only on success, so `budget` is still ours: leak a reference to it (if even that cannot
      // be allocated, the budget is released and the blocks keep a dangling pointer; double out-of-memory).
      (void)new (std::nothrow) std::shared_ptr<native::MemoryBudget>(std::move(budget));
    }
  }

 private:
  std::mutex mutex_;
  std::unordered_map<native::MemoryBudget*, std::shared_ptr<native::MemoryBudget>> held_;
  std::size_t sweep_at_ = 64;
};

/// The smallest block the cap refuses. CPython mishandles some failed small allocations: 3.12's MAKE_FUNCTION releases
/// its code object twice when the function cannot be allocated (`Py_DECREF(codeobj)` then `goto error`,
/// https://github.com/python/cpython/blob/v3.12.7/Python/generated_cases.c.h, fixed in 3.14). So a small block is
/// always charged and served, and a run of small objects is stopped at twice the cap by an asynchronous MemoryError.
constexpr std::size_t kSmallestRefusedBlock = 1024;

/// Charges `bytes` of a `block_bytes` allocation to `budget`. Past the limit a large block is refused; a small one is
/// charged anyway, and the first to pass twice the limit asks the watchdog to stop the call. Either is latched for
/// ScopedAllocationCap::refused() only when `budget` is the current call's: a block an earlier call left behind is
/// charged to that call's budget, which may refuse it but does not fail this call.
bool charge(ThreadCap& cap, native::MemoryBudget& budget, std::size_t bytes, std::size_t block_bytes) {
  const bool current = &budget == cap.budget;
  if (block_bytes >= kSmallestRefusedBlock) {
    const bool charged = budget.tryAcquire(bytes);
    cap.refused = cap.refused || (!charged && current);
    return charged;
  }
  const std::size_t stop = budget.limit() > SIZE_MAX / 2 ? SIZE_MAX : 2 * budget.limit();
  const std::size_t used = budget.acquireAnyway(bytes);
  if (current && used > stop && used - bytes <= stop) {
    cap.refused = true;
    PersistentWatchdog::stopForMemory();
  }
  return true;
}

struct AllocatorCall {
  explicit AllocatorCall(ThreadCap& cap) : cap_(cap) {
    ++cap_.allocator_depth;
  }
  ~AllocatorCall() {
    --cap_.allocator_depth;
  }
  AllocatorCall(const AllocatorCall&) = delete;
  AllocatorCall& operator=(const AllocatorCall&) = delete;

 private:
  ThreadCap& cap_;
};

// Leaked: blocks may be freed during interpreter teardown, after static destruction.
std::array<AllocatorState, 3>& allocatorStates() {
  static auto* states = new std::array<AllocatorState, 3>;
  return *states;
}

void* cappedMalloc(void* context, std::size_t size) {
  auto& state = *static_cast<AllocatorState*>(context);
  ThreadCap& cap = g_cap;
  if (cap.allocator_depth != 0 || cap.budget == nullptr) {
    return state.original.malloc(state.original.ctx, size);
  }
  AllocatorCall call(cap);
  native::MemoryBudget& budget = *cap.budget;
  const std::size_t charged = std::max(size, std::size_t{1});
  if (!charge(cap, budget, charged, size)) {
    return nullptr;
  }
  void* block = state.original.malloc(state.original.ctx, size);
  if (block == nullptr) {
    budget.release(charged);
    return nullptr;
  }
  try {
    StateLock lock(state);
    state.owned.insert({block, AllocationRecord{charged, &budget}});
    state.owned_count.store(state.owned.size(), std::memory_order_release);
  } catch (...) {
    state.original.free(state.original.ctx, block);
    budget.release(charged);
    return nullptr;
  }
  return block;
}

void* cappedCalloc(void* context, std::size_t count, std::size_t size) {
  auto& state = *static_cast<AllocatorState*>(context);
  ThreadCap& cap = g_cap;
  if (cap.allocator_depth != 0 || cap.budget == nullptr) {
    return state.original.calloc(state.original.ctx, count, size);
  }
  if (count != 0 && size > SIZE_MAX / count) {
    cap.refused = true;
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
  ThreadCap& cap = g_cap;
  if (cap.allocator_depth != 0) {
    return state.original.realloc(state.original.ctx, block, size);
  }
  if (block == nullptr) {
    return cappedMalloc(context, size);
  }
  if (cap.budget == nullptr && state.owned_count.load(std::memory_order_acquire) == 0) {
    return state.original.realloc(state.original.ctx, block, size);
  }
  AllocatorCall call(cap);
  StateLock lock(state);
  auto found = state.owned.find(block);
  const bool known = found != state.owned.end();
  if (!known && cap.budget == nullptr) {
    return state.original.realloc(state.original.ctx, block, size);
  }
  const std::size_t old_size = known ? found->second.size : 0;
  native::MemoryBudget& budget = known ? *found->second.budget : *cap.budget;
  const std::size_t new_size = std::max(size, std::size_t{1});
  const std::size_t growth = new_size > old_size ? new_size - old_size : 0;
  if (!charge(cap, budget, growth, size)) {
    return nullptr;
  }
  // CPython wants a distinct block for zero bytes; realloc(block, 0) may free it on some platforms.
  void* moved = state.original.realloc(state.original.ctx, block, new_size);
  if (moved == nullptr) {
    budget.release(growth);
    return nullptr;
  }
  try {
    if (known && moved == block) {
      found.value().size = new_size;
    } else {
      if (known) {
        state.owned.erase(found);
      }
      state.owned.insert({moved, AllocationRecord{new_size, &budget}});
    }
  } catch (...) {
    // Out of memory for the table: the block goes on untracked, as one allocated outside any cap, and gives back its
    // charge. An erased record is gone either way.
    state.owned_count.store(state.owned.size(), std::memory_order_release);
    budget.release(old_size + growth);
    return moved;
  }
  if (old_size > new_size) {
    budget.release(old_size - new_size);
  }
  state.owned_count.store(state.owned.size(), std::memory_order_release);
  return moved;
}

void cappedFree(void* context, void* block) {
  auto& state = *static_cast<AllocatorState*>(context);
  if (block == nullptr) {
    return;
  }
  ThreadCap& cap = g_cap;
  if (cap.allocator_depth == 0 && state.owned_count.load(std::memory_order_acquire) != 0) {
    AllocatorCall call(cap);
    StateLock lock(state);
    if (auto found = state.owned.find(block); found != state.owned.end()) {
      const AllocationRecord record = found->second;
      state.owned.erase(found);
      state.owned_count.store(state.owned.size(), std::memory_order_release);
      record.budget->release(record.size);  // last: a budget with nothing charged may be destroyed
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
    states[i].gil_serialized = domains[i] != PYMEM_DOMAIN_RAW;
    PyMemAllocatorEx wrapper{&states[i], &cappedMalloc, &cappedCalloc, &cappedRealloc, &cappedFree};
    PyMem_SetAllocator(domains[i], &wrapper);
  }
}

// ---- the AST check ----------------------------------------------------------------------------

constexpr std::array<std::string_view, 6> kBlockedNames{"__builtins__", "__loader__", "__spec__",
                                                        "globals",      "locals",     "vars"};

/// Frame, code and traceback internals: each leads to a frame's globals or builtins without a dunder.
constexpr std::array<std::string_view, 13> kBlockedAttributes{
    "gi_frame", "gi_code",    "cr_frame", "cr_code", "ag_frame", "ag_code", "f_globals",
    "f_locals", "f_builtins", "f_back",   "f_code",  "tb_frame", "tb_next"};

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
  // Once per process, failure included: a second start is refused by pybind11 ("already running") or, after CPython
  // failed to start, runs on a half-initialized runtime, either way losing the cause. Leaked like the interpreter: a
  // thread still running at exit may call this after static destruction.
  static const std::string* const failure = new std::string([]() -> std::string {  // NOLINT: never destroyed
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
    try {
      new py::scoped_interpreter();  // NOLINT: leaked on purpose, never finalized
    } catch (const std::exception& error) {
      // CPython's start-up status: the runtime is left half-initialized, so nothing touches it again.
      return std::string("Python failed to start: ") + error.what();
    }
    std::string error;
    try {
      PySys_AddAuditHook(&auditHook, nullptr);
      installAllocatorWrapper();
      for (const char* name : {"ast", "types", "json", "math", "cmath", "statistics"}) {
        py::module_::import(name);
      }
      // Modules the allowed ones import at a first call: loaded now, before any run arms the hook, because an import
      // inside a run opens its file, which the hook refuses. The browser's stdlib zip (stdlib_zip.py) roots them too.
      for (const char* name : kLazyDependencies) {
        py::module_::import(name);
      }
      // statistics re-exports decimal.Decimal, whose C format falls back to _pydecimal for a spec libmpdec lacks (the
      // "_" grouping). It imports it through the caller's __import__, which in a strict run is the gate, and keeps the
      // class once found: one such format now makes every later one import nothing. A _pydecimal that refuses the spec
      // too (CPython 3.12) raises ValueError after the import.
      try {
        (void)py::module_::import("decimal").attr("Decimal")(1).attr("__format__")("_");
      } catch (py::error_already_set& error) {
        if (!error.matches(PyExc_ValueError)) {
          throw;
        }
      }
      py::dict scope;
      scope["__builtins__"] = py::module_::import("builtins");
      py::exec(kGateSource, scope);
      g_strict_builtins = new py::dict(makeBuiltins(scope["make_gate"], kStrictImports));
    } catch (const std::exception& caught) {
      error = "Python failed to start: " + pythonErrorText(caught);  // a Python error dies here, the GIL still held
    }
    // Released on failure too: the interpreter runs, and no thread may wait for a GIL the booting thread keeps.
    new py::gil_scoped_release();  // NOLINT: leaked on purpose, the GIL is handed to the callers
    return error;
  }());
  if (!failure->empty()) {
    throw std::runtime_error(*failure);
  }
}

py::dict strictBuiltins() {
  PyObject* copy = PyDict_Copy(g_strict_builtins->ptr());
  if (copy == nullptr) {
    throw py::error_already_set();
  }
  return py::reinterpret_steal<py::dict>(copy);
}

Expected<py::object> compileChecked(std::string_view source, std::string_view required_function) {
  if (source.size() > kMaxSourceBytes) {
    return unexpected("script larger than " + native::describeBytes(kMaxSourceBytes));
  }
  try {
    py::object ast = py::module_::import("ast");
    py::object tree;
    try {
      tree = ast.attr("parse")(py::str(source.data(), source.size()), "<string>");
    } catch (py::error_already_set& error) {
      return unexpected("syntax error: " + pythonErrorText(error));
    }
    py::object attribute_type = ast.attr("Attribute");
    py::object name_type = ast.attr("Name");
    for (py::handle node : ast.attr("walk")(tree)) {
      if (py::isinstance(node, attribute_type)) {
        const auto attribute = node.attr("attr").cast<std::string>();
        if (attribute.starts_with("__")) {
          return unexpected("forbidden dunder attribute access: ." + attribute);
        }
        if (std::ranges::find(kBlockedAttributes, attribute) != kBlockedAttributes.end()) {
          return unexpected("forbidden attribute access: ." + attribute);
        }
        if (attribute == "format" || attribute == "format_map") {
          return unexpected("forbidden attribute access: ." + attribute + " (use an f-string)");
        }
      } else if (py::isinstance(node, name_type)) {
        const auto name = node.attr("id").cast<std::string>();
        if (std::ranges::find(kBlockedNames, name) != kBlockedNames.end()) {
          return unexpected("forbidden name: " + name);
        }
      }
    }
    if (!required_function.empty()) {
      bool defined = false;
      py::object function_type = ast.attr("FunctionDef");
      for (py::handle statement : tree.attr("body")) {
        defined = defined || (py::isinstance(statement, function_type) &&
                              statement.attr("name").cast<std::string>() == required_function);
      }
      if (!defined) {
        return unexpected("the script must define a top-level function named '" + std::string(required_function) + "'");
      }
    }
    // Compiles the checked tree, not the source again; catches what parse accepts (`return` outside a function).
    return py::module_::import("builtins").attr("compile")(tree, "<string>", "exec");
  } catch (py::error_already_set& error) {
    return unexpected("syntax error: " + pythonErrorText(error));
  }
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

ScopedAuditGuard::ScopedAuditGuard(Profile profile) : strict_(profile == Profile::kStrict) {
  ++g_audit_armed_depth;
  g_audit_strict_depth += strict_ ? 1 : 0;
}

ScopedAuditGuard::~ScopedAuditGuard() {
  --g_audit_armed_depth;
  g_audit_strict_depth -= strict_ ? 1 : 0;
}

ScopedAllocationCap::ScopedAllocationCap(std::shared_ptr<native::MemoryBudget> budget)
    : budget_(std::move(budget)),
      previous_(std::exchange(g_cap.budget, budget_.get())),
      previous_refused_(std::exchange(g_cap.refused, false)) {}

ScopedAllocationCap::~ScopedAllocationCap() {
  g_cap.budget = previous_;
  g_cap.refused = previous_refused_;
  LingeringBudgets::instance().retire(std::move(budget_));
}

bool ScopedAllocationCap::refused() const {
  return g_cap.refused;
}

std::string timeoutMessage(std::string_view call, std::uint64_t max_millis) {
  return "TimeoutError: " + std::string(call) + " exceeded its time budget of " + std::to_string(max_millis) + " ms";
}

ScriptCallScope::ScriptCallScope(
    std::shared_ptr<native::MemoryBudget> memory, std::uint64_t max_millis, Profile profile,
    Span<const std::shared_ptr<const std::atomic<bool>>> cancel)
    : audit_(profile), cap_(std::move(memory)) {
  PersistentWatchdog::arm(max_millis, cancel);
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

void PersistentWatchdog::arm(std::uint64_t max_millis, Span<const std::shared_ptr<const std::atomic<bool>>> cancel) {
  Slot& slot = currentSlot();
  slot.cancel_stop.store(false, std::memory_order_relaxed);
  if (!cancel.empty()) {
    std::lock_guard lock(instance().mutex_);
    slot.cancel_flags.assign(cancel.begin(), cancel.end());
  }
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
  if (!slot.cancel_flags.empty()) {  // only this thread fills it, so the unlocked read sees its own write
    std::lock_guard lock(instance().mutex_);
    slot.cancel_flags.clear();
  }
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
        const bool cancelled = std::ranges::any_of(
            slot->cancel_flags, [](const auto& flag) { return flag->load(std::memory_order_acquire); });
        if (isArmed(*slot) && cancelled && !slot->cancel_stop.exchange(true, std::memory_order_relaxed)) {
          slot->deadline_ns.store(0, std::memory_order_release);  // interrupt it now
        }
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
        PyObject* raised = slot->cancel_stop.load(std::memory_order_relaxed) ? PyExc_KeyboardInterrupt
                           : memory                                          ? PyExc_MemoryError
                                                                             : PyExc_TimeoutError;
        PyThreadState_SetAsyncExc(slot->thread_id.load(std::memory_order_relaxed), raised);
      }
    }
  }
}

}  // namespace PJ::scripting::detail
