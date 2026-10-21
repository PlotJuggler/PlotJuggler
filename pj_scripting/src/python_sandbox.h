// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// The one embedded CPython interpreter and the hardening shared by the Python filter engine and the on-demand Python
// lane. NOT a sandbox (pj_scripting/CLAUDE.md): CPython cannot be confined from inside the process. Each piece
// narrows one escape and says what it leaves open.

#include <pybind11/pybind11.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_scripting/memory_budget.h"

namespace PJ::scripting::detail {

namespace py = pybind11;

/// Boots the interpreter on first call, never finalized (finalizing embedded CPython at static destruction is
/// fragile, and the watchdog thread outlives every caller), then leaves the GIL released: every entry point takes it
/// with `py::gil_scoped_acquire`. The boot installs the audit hook and the allocation wrapper while only the booting
/// thread runs Python, and imports the modules scripts may import while nothing is armed (a first import opens files,
/// which an armed hook refuses). Idempotent; any thread. Boots once even when it fails: every call then throws
/// `std::runtime_error` with the first failure, and the caller must not touch Python. The interpreter is unusable, and
/// the GIL is free after a failure past the start but still held by the booting thread when CPython failed to start.
void ensureInterpreter();

/// What a script call runs under, beyond the limits and the hook every call gets. kCompatible: nothing more, the
/// filters' profile, so filters written before the sandbox run unchanged (python_engine.h). kStrict: the on-demand
/// scripts' profile, new code held to more: `strictBuiltins`, `compileChecked`, and a hook that also refuses `exec` and
/// `compile`.
enum class Profile : std::uint8_t { kCompatible, kStrict };

/// The strict profile's `__builtins__`, a fresh copy per call (a script that reaches it cannot leave anything for the
/// next one): the real builtins without `open`, `exec`, `eval`, `compile`, `input`, `breakpoint`, `help`, `getattr`,
/// `setattr`, `delattr` and `type`, and `__import__` replaced by a gate over `math`, `cmath`, `statistics` and `json`.
/// The gate refuses relative imports and every other root ("module 'X' is not available in Data Processors (allowed:
/// ...)") and returns read-only copies: an allowed module's public names (no `_` prefix), each nested module under an
/// allowed root copied the same way and the others left out, so `statistics.sys` and `json.codecs` do not exist.
/// Assigning or deleting an attribute of a copy raises. The allowed modules are copied when the interpreter starts; a
/// copy built later (a submodule loaded since) is published only once complete. What a copy shares with the real module
/// is not frozen: a class's attributes, and its functions' globals, which are the real module's dict, so a filter (the
/// compatible profile imports the real modules) can patch e.g. `statistics._sum` and later on-demand runs call the
/// patched code; accepted under the trust model. An allowed module's own code runs under its own, real builtins. GIL
/// held.
[[nodiscard]] py::dict strictBuiltins();

/// `source` checked and compiled, run nothing: a syntax error ("syntax error: ... (<string>, line N)"), an attribute
/// starting with `__`, a frame, code or traceback internal (`gi_frame`, `f_globals`, `tb_frame`, ...), `format` or
/// `format_map` (they walk attributes from a string; scripts use f-strings, whose fields the check sees) or a name
/// reaching the interpreter's namespaces (`__builtins__`, `__loader__`, `__spec__`, `globals`, `locals`, `vars`) is
/// an error, and so is a source over `kMaxSourceBytes` or, when `required_function` is given, no top-level `def` of
/// that name. The checks are lexical: `getattr` and friends are absent from `gatedBuiltins` for the same reason. GIL
/// held.
[[nodiscard]] Expected<py::object> compileChecked(std::string_view source, std::string_view required_function = {});

/// Runs a code object compiled on the host in `globals`, as `exec` would, without raising the "exec" audit event.
/// Throws `py::error_already_set`. GIL held.
void runCode(py::handle code, const py::dict& globals);

/// `e.what()` without the "At:" frames pybind11 appends, which name interpreter files, not the script.
[[nodiscard]] std::string pythonErrorText(const std::exception& e);

/// While alive on this thread, the process-wide audit hook refuses `open`, `os.*`, `shutil.*`, `subprocess.*`,
/// `socket.*`, `ctypes.*`, `sys._getframe`, `sys.addaudithook` and the import of `os`, `subprocess`, `socket`,
/// `ctypes`, `importlib` and their native halves, with a `PermissionError`, and under `Profile::kStrict` also `exec`
/// and `compile`. Unarmed (the default, on every other thread) the hook refuses nothing, because the interpreter's own
/// imports and the host's compiles raise the same events. Arm it only around script code; nested guards stay armed
/// until the outermost ends.
class ScopedAuditGuard {
 public:
  explicit ScopedAuditGuard(Profile profile);
  ~ScopedAuditGuard();
  ScopedAuditGuard(const ScopedAuditGuard&) = delete;
  ScopedAuditGuard& operator=(const ScopedAuditGuard&) = delete;

 private:
  bool strict_;
};

/// While alive on this thread, its Python allocations (all three domains) are charged to `budget`. Past the limit a
/// block of 1 KiB or more fails, which Python raises as `MemoryError`; a smaller one is always served (CPython
/// mishandles some failed small allocations: python_sandbox.cpp's `kSmallestRefusedBlock`), and once the charges pass
/// twice the limit the watchdog stops the call with a MemoryError at its next bytecode. `refused()` latches either,
/// which a script may have caught. A block keeps its charge until freed, on any thread and after the scope, and keeps
/// the budget alive for that long; every block charges at least a byte. A block allocated outside any cap passes
/// through, and is adopted at its full size when reallocated under one. Under the GIL (MEM and OBJ) the bookkeeping
/// takes no lock, which assumes one GIL: a script that creates a subinterpreter with its own GIL could break it,
/// accepted under the trust model (cooperative limits, not a security boundary). Nests: the destructor restores the
/// outer cap.
class ScopedAllocationCap {
 public:
  explicit ScopedAllocationCap(std::shared_ptr<native::MemoryBudget> budget);
  ~ScopedAllocationCap();
  ScopedAllocationCap(const ScopedAllocationCap&) = delete;
  ScopedAllocationCap& operator=(const ScopedAllocationCap&) = delete;

  [[nodiscard]] bool refused() const;

 private:
  std::shared_ptr<native::MemoryBudget> budget_;
  native::MemoryBudget* previous_;  ///< the enclosing scope's, kept alive by that scope
  bool previous_refused_;
};

/// The process-wide watchdog of Python script calls: one thread, started on first use and never joined (a join at
/// thread exit deadlocks on Windows under the loader lock; so this code must stay in a library the executable links,
/// never in an unloadable plugin), serving one slot per thread that runs Python. `arm`/`disarm` are a few atomic
/// stores on the caller's slot. The thread sleeps until the earliest deadline (at most `kMaxSleepMillis`, so a later,
/// shorter arming is caught on time) and parks once nothing was armed for `kIdleParkMillis`. On expiry it takes the
/// GIL and, if the same arming is still live, schedules `TimeoutError` on that thread (a MemoryError when the cap asked
/// for it, `stopForMemory`), and again every `kRefireMillis` until disarmed, so a script that catches it is interrupted
/// again: a pure-Python loop stops at its next bytecode; a native call always runs to completion first.
///
/// Threading: `arm`/`disarm` run with the GIL held on the slot's thread; the fire path holds the GIL too, so the GIL
/// orders a fire against a disarm. Nothing may wait for the watchdog thread while holding the GIL.
class PersistentWatchdog {
 public:
  /// One Python thread's timing state; owned by that thread (thread_local) and by the registry while registered.
  struct Slot {
    std::uint64_t generation = 0;                    ///< bumped by every arm; the slot's thread only
    std::atomic<std::uint64_t> armed_generation{0};  ///< live arming, 0 when disarmed
    std::atomic<std::int64_t> timeout_ns{0};         ///< steady_clock deadline of the live arming
    std::atomic<std::int64_t> deadline_ns{0};        ///< when to fire next: the timeout, a refire, or now
    std::atomic<bool> memory_stop{false};            ///< the cap asked for a MemoryError instead of a TimeoutError
    std::atomic<unsigned long> thread_id{0};         ///< Python thread ident of the slot's thread
    /// The arming the watchdog fired for. Written with the GIL held, and read with it.
    std::atomic<std::uint64_t> fired_generation{0};
    std::atomic<bool> cancel_stop{false};  ///< a cancel flag was seen set: a KeyboardInterrupt instead
    /// The live arming's cancel flags (any set: interrupt it); guarded by the watchdog's mutex, set only when
    /// non-empty.
    std::vector<std::shared_ptr<const std::atomic<bool>>> cancel_flags;
  };

  /// Starts timing one script call on the calling thread, which `cancel` (thread-safe flags, polled at least every
  /// `kMaxSleepMillis`) also interrupts: once one is set, a KeyboardInterrupt (which `except Exception` does not catch)
  /// at the next bytecode and every `kRefireMillis` after. GIL held.
  static void arm(std::uint64_t max_millis, Span<const std::shared_ptr<const std::atomic<bool>>> cancel = {});
  /// Ends the calling thread's live arming; true when its deadline has passed (also when the GIL was held past it, so
  /// it never fired). Drops a not-yet-delivered exception, so it never reaches a later call. Idempotent. GIL held.
  [[nodiscard]] static bool disarm();
  /// Asks the watchdog for a MemoryError on the calling thread's live arming at once, and again every `kRefireMillis`
  /// until the arming's deadline, after which it raises the TimeoutError. Callable from an allocator: it allocates
  /// nothing and only takes the watchdog's mutex, which no holder keeps while allocating; does nothing outside an
  /// arming.
  static void stopForMemory() noexcept;

 private:
  static constexpr std::int64_t kMaxSleepMillis = 50;
  static constexpr std::int64_t kIdleParkMillis = 200;
  static constexpr std::int64_t kRefireMillis = 10;

  PersistentWatchdog();
  static PersistentWatchdog& instance();
  static Slot& currentSlot();
  void run();

  std::mutex mutex_;  ///< guards slots_ and pairs with wake_
  std::condition_variable wake_;
  std::vector<std::shared_ptr<Slot>> slots_;
  /// Armed slots. Incremented by arm before reading parked_, read by the watchdog after setting parked_ (both
  /// seq_cst), so a park never misses an arming.
  std::atomic<int> armed_slots_{0};
  std::atomic<bool> parked_{false};
  /// Activity counter for the idle park: armed_slots_ alone misses an arm and disarm between two wakeups.
  std::atomic<std::uint64_t> arms_total_{0};
};

/// Clears a script namespace when it goes out of scope, exceptions included. Its functions hold it, so otherwise the
/// cycle, and the finalizers its globals carry, wait for a garbage collection on any thread, unarmed. Declare it
/// inside the ScriptCallScope; `release()` keeps a namespace that outlives the call (a filter instance's). GIL held.
class ClearedOnExit {
 public:
  explicit ClearedOnExit(const py::dict& ns) : ns_(ns.inc_ref().ptr()) {}
  ~ClearedOnExit() {
    if (ns_ != nullptr) {
      PyDict_Clear(ns_);
    }
    release();
  }
  ClearedOnExit(const ClearedOnExit&) = delete;
  ClearedOnExit& operator=(const ClearedOnExit&) = delete;

  void release() {
    Py_XDECREF(ns_);
    ns_ = nullptr;
  }

 private:
  PyObject* ns_;  ///< a strong reference; not a py::object, whose hidden visibility this exported class cannot hold
};

/// "TimeoutError: <call> exceeded its time budget of <N> ms", the wording of both Python engines.
[[nodiscard]] std::string timeoutMessage(std::string_view call, std::uint64_t max_millis);

/// Everything one call into script code runs under: the audit hook armed, allocations charged to `memory`, and the
/// watchdog timing it, and interrupting it once one of `cancel` is set (`PersistentWatchdog::arm`). `timedOut()` ends
/// the timing and reports whether the call ran out of time, even when the script caught the `TimeoutError`; the
/// destructor ends it otherwise. `overBudget()` reports a refused allocation, even a caught `MemoryError`. Destroy it
/// before dropping the GIL.
class ScriptCallScope {
 public:
  ScriptCallScope(
      std::shared_ptr<native::MemoryBudget> memory, std::uint64_t max_millis, Profile profile,
      Span<const std::shared_ptr<const std::atomic<bool>>> cancel = {});
  ~ScriptCallScope();
  ScriptCallScope(const ScriptCallScope&) = delete;
  ScriptCallScope& operator=(const ScriptCallScope&) = delete;

  [[nodiscard]] bool timedOut();
  [[nodiscard]] bool overBudget() const {
    return cap_.refused();
  }

 private:
  ScopedAuditGuard audit_;
  ScopedAllocationCap cap_;
};

}  // namespace PJ::scripting::detail
