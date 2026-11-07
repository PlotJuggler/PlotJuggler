// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The Python on-demand runner (see python_object_script.h): compile once, then per run a fresh namespace over the
// compiled code, inside one ScriptCallScope. Objects and their methods are python_object_binder.cpp's; the binding
// rules are script_contract.h's, shared with Luau.

#include "pj_scripting/python_object_script.h"

#include <Python.h>
#include <pybind11/eval.h>
#include <pybind11/pybind11.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pj_scripting/memory_budget.h"
#include "python_object_binder.h"
#include "python_sandbox.h"
#include "script_contract.h"
#include "script_media.h"

namespace py = pybind11;

namespace PJ::scripting {

/// A script's source and, once compiled, a strong reference to its code object (dropped under the GIL by the deleter
/// `deferOnDemandScriptPython` gives) or its compile error. A thread takes `mutex` before the GIL, never while holding
/// it: a compile hands the GIL to other threads now and then, and one waiting for `mutex` with the GIL would deadlock.
struct PythonCode {
  mutable std::string source;  ///< guarded by `mutex`; freed once compiled
  mutable std::mutex mutex;
  mutable bool compiled = false;     ///< guarded by `mutex`: the compile ran, leaving `code` or `error`
  mutable PyObject* code = nullptr;  ///< guarded by `mutex` until `compiled`
  mutable std::string error;         ///< guarded by `mutex` until `compiled`
};

namespace {

// Runs once with the real builtins. `pj.unavailable` is a function, so a script cannot subclass the marker.
constexpr const char* kPjSource = R"PY(
import types

class Unavailable:
    __slots__ = ("reason",)

    def __init__(self, reason):
        self.reason = reason

    def __repr__(self):
        return f"pj.unavailable({self.reason!r})"

def unavailable(reason=""):
    if not isinstance(reason, str):
        raise TypeError("pj.unavailable(reason): reason must be a string")
    return Unavailable(reason)

namespace = types.SimpleNamespace
)PY";

py::object interned(const char* text) {
  return detail::checked(PyUnicode_InternFromString(text));
}

/// What every run reuses: the `pj` pieces, and the constant names and value of a run's namespace, interned once (a
/// run builds nothing it can share, and the interpreter looks `__builtins__` up on every call a script makes).
struct PjHelpers {
  py::object unavailable_type;
  py::object unavailable;
  py::object namespace_type;
  py::object builtins_key;
  py::object name_key;
  py::object module_name;
  py::object pj_key;
  py::object evaluate_key;
};

// Built under the GIL on first use and never freed. Not a function-local static: building runs bytecode, which may
// hand the GIL to a thread that would then wait on the static's guard while holding it.
PjHelpers* g_pj = nullptr;

const PjHelpers& pjHelpers() {
  if (g_pj == nullptr) {
    py::dict scope;
    scope["__builtins__"] = py::module_::import("builtins");
    py::exec(kPjSource, scope);
    auto* built =
        new PjHelpers{scope["Unavailable"], scope["unavailable"], scope["namespace"], interned("__builtins__"),
                      interned("__name__"), interned("recipe"),   interned("pj"),     interned("evaluate")};
    if (g_pj == nullptr) {
      g_pj = built;
    } else {
      delete built;  // another thread built them while this one ran bytecode
    }
  }
  return *g_pj;
}

void setItem(const py::object& dict, const py::object& key, const py::object& value) {
  if (PyDict_SetItem(dict.ptr(), key.ptr(), value.ptr()) != 0) {
    throw py::error_already_set();
  }
}

py::object toPython(const nlohmann::json& value) {
  switch (value.type()) {
    case nlohmann::json::value_t::boolean:
      return py::bool_(value.get<bool>());
    case nlohmann::json::value_t::number_integer:
      return detail::checked(PyLong_FromLongLong(value.get<int64_t>()));
    case nlohmann::json::value_t::number_unsigned:
      return detail::checked(PyLong_FromUnsignedLongLong(value.get<uint64_t>()));
    case nlohmann::json::value_t::number_float:
      return detail::checked(PyFloat_FromDouble(value.get<double>()));
    case nlohmann::json::value_t::string:
      return detail::newString(value.get_ref<const std::string&>());
    case nlohmann::json::value_t::array: {
      py::object out = detail::checked(PyList_New(0));
      for (const auto& element : value) {
        if (PyList_Append(out.ptr(), toPython(element).ptr()) != 0) {
          throw py::error_already_set();
        }
      }
      return out;
    }
    case nlohmann::json::value_t::object: {
      py::object out = detail::checked(PyDict_New());
      for (auto it = value.begin(); it != value.end(); ++it) {
        setItem(out, detail::newString(it.key()), toPython(it.value()));
      }
      return out;
    }
    default:
      return py::none();
  }
}

/// A scalar or object input, noting a media input in `media`; the caller has rejected unavailable inputs.
py::object toPython(const NamedValue& input, ScriptMedia& media) {
  if (const auto* number = std::get_if<double>(&input.value)) {
    return detail::checked(PyFloat_FromDouble(*number));
  }
  if (const auto* text = std::get_if<std::string>(&input.value)) {
    return detail::newString(*text);
  }
  sdk::BuiltinObject object = detail::withStampShifted(std::get<sdk::BuiltinObject>(input.value), input.stamp_shift);
  media.noteInput(object, input);
  return detail::newObject(std::move(object), input.keepalive);
}

/// The UTF-8 of a `str`, charged to `charge` before it is copied: a script must not multiply one string into
/// gigabytes of copies. `max_bytes` cuts it.
Expected<std::string> copyText(PyObject* text, native::MemoryReservation& charge, std::size_t max_bytes = SIZE_MAX) {
  Py_ssize_t length = 0;
  const char* data = PyUnicode_AsUTF8AndSize(text, &length);
  if (data == nullptr) {
    return unexpected(py::error_already_set().what());
  }
  const std::size_t size = std::min(static_cast<std::size_t>(length), max_bytes);
  if (Status charged = charge.grow(size); !charged) {
    return unexpected(charged.error());
  }
  return std::string(data, size);
}

/// Reads one returned value into `item`. Runs no script code: the marker and objects are matched by exact type,
/// numbers, strings and containers are read from their storage, never through a method a script could override.
Status readValue(PyObject* value, native::MemoryReservation& charge, detail::Returned& item) {
  item.type_name = Py_TYPE(value)->tp_name;
  if (Py_TYPE(value) == reinterpret_cast<PyTypeObject*>(pjHelpers().unavailable_type.ptr())) {
    py::object reason = py::reinterpret_steal<py::object>(PyObject_GetAttrString(value, "reason"));
    if (!reason || PyUnicode_CheckExact(reason.ptr()) == 0) {
      PyErr_Clear();
      return unexpected(std::string("pj.unavailable(reason): reason must be a string"));
    }
    Expected<std::string> text = copyText(reason.ptr(), charge, detail::kMaxReasonBytes);
    if (!text) {
      return unexpected(text.error());
    }
    item.value = Unavailable{std::move(*text)};
  } else if (PyBool_Check(value) != 0) {
    // not a number output, whatever Python's int hierarchy says
  } else if (PyFloat_Check(value) != 0) {
    item.value = PyFloat_AS_DOUBLE(value);
  } else if (PyLong_Check(value) != 0) {
    const double number = PyLong_AsDouble(value);
    if (number == -1.0 && PyErr_Occurred() != nullptr) {
      return unexpected(py::error_already_set().what());
    }
    item.value = number;
  } else if (PyUnicode_Check(value) != 0) {
    Expected<std::string> text = copyText(value, charge);
    if (!text) {
      return unexpected(text.error());
    }
    item.value = std::move(*text);
  } else if (const sdk::BuiltinObject* object = detail::toObject(value)) {
    item.value = *object;
    item.keepalive = detail::anchorOf(value);  // a builder's output carries its native charge only there
  }
  return okStatus();
}

/// The returned value, one item per output candidate: a dict is keyed (a `None` value is a missing output), a list
/// or tuple positional, anything else one item. Each item is charged to `charge`, which bounds their count.
Expected<std::vector<detail::Returned>> collectReturned(PyObject* result, native::MemoryReservation& charge) {
  std::vector<detail::Returned> returned;
  const auto add = [&](PyObject* value, std::string key, int position) -> Status {
    if (Status charged = charge.grow(sizeof(detail::Returned)); !charged) {
      return charged;
    }
    detail::Returned item;
    item.key = std::move(key);
    item.position = position;
    if (Status read = readValue(value, charge, item); !read) {
      return read;
    }
    returned.push_back(std::move(item));
    return okStatus();
  };
  if (PyDict_Check(result) != 0) {
    Py_ssize_t cursor = 0;
    PyObject* key = nullptr;
    PyObject* value = nullptr;
    while (PyDict_Next(result, &cursor, &key, &value) != 0) {
      if (PyUnicode_Check(key) == 0) {
        return unexpected(std::string("returned dict keys must be strings"));
      }
      if (value == Py_None) {
        continue;
      }
      Expected<std::string> name = copyText(key, charge);
      if (!name) {
        return unexpected(name.error());
      }
      if (Status added = add(value, std::move(*name), 0); !added) {
        return unexpected(added.error());
      }
    }
  } else if (PyList_Check(result) != 0 || PyTuple_Check(result) != 0) {
    const Py_ssize_t size = PySequence_Fast_GET_SIZE(result);
    for (Py_ssize_t index = 0; index < size; ++index) {
      if (Status added = add(PySequence_Fast_GET_ITEM(result, index), {}, static_cast<int>(index) + 1); !added) {
        return unexpected(added.error());
      }
    }
  } else if (Status added = add(result, {}, 0); !added) {
    return unexpected(added.error());
  }
  return returned;
}

/// The one thread for the Python host work its caller must not wait on without a bound (the GIL may be held for minutes
/// by a script stuck in a builtin C loop, which no watchdog interrupts): a compiled script's last release, which nobody
/// waits for, and a validate's throwaway compile, waited for at most `kPythonValidateWaitMillis`. FIFO. `busy()` while
/// an item runs, so a validate behind a stuck item gives up at once instead of after its bound. Started on
/// first use and never joined (a join at exit deadlocks on Windows under the loader lock), so this stays in a library
/// the executable links, like the watchdog.
class PythonHelper {
 public:
  static PythonHelper& instance() {
    static PythonHelper* const helper = new PythonHelper();  // NOLINT: never destroyed
    return *helper;
  }

  /// Whether an item is running now (a release or a compile, which may be waiting for the GIL). Any thread.
  [[nodiscard]] bool busy() const {
    return busy_.load(std::memory_order_acquire);
  }

  /// Whether the calling thread is the helper.
  [[nodiscard]] static bool onHelper() {
    return t_on_helper;
  }

  /// Any thread; returns at once. Runs `work`, then `done` once `busy()` is false again. Neither may throw.
  void post(std::function<void()> work, std::function<void()> done = {}) {
    {
      const std::lock_guard lock(mutex_);
      queue_.push_back({std::move(work), std::move(done)});
    }
    wake_.notify_one();
  }

 private:
  struct Item {
    std::function<void()> work;
    std::function<void()> done;
  };

  PythonHelper() {
    std::thread([this] { run(); }).detach();
  }

  [[noreturn]] void run() {
    t_on_helper = true;
    while (true) {
      Item item;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return !queue_.empty(); });
        item = std::move(queue_.front());
        queue_.pop_front();
      }
      busy_.store(true, std::memory_order_release);
      item.work();
      item.work = nullptr;  // what it holds dies here, still busy
      busy_.store(false, std::memory_order_release);
      if (item.done) {
        item.done();
      }
    }
  }

  static thread_local bool t_on_helper;

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Item> queue_;  ///< guarded by mutex_
  std::atomic<bool> busy_{false};
};

thread_local bool PythonHelper::t_on_helper = false;

/// Drops the code object on the helper, under the GIL, never on the caller's thread (inline when that is the helper); a
/// script never compiled needs no GIL. The last reference: nothing else reads it.
void release(const PythonCode* dead) {
  if (dead->code == nullptr) {
    delete dead;
    return;
  }
  if (PythonHelper::onHelper()) {
    const py::gil_scoped_acquire gil;
    Py_DECREF(dead->code);
    delete dead;
    return;
  }
  PythonHelper::instance().post([dead] {
    py::gil_scoped_acquire gil;
    Py_DECREF(dead->code);
    delete dead;
  });
}

/// Boots the interpreter on first use, then checks and compiles `source`: a new strong reference to its code object.
Expected<PyObject*> compileNow(const std::string& source) {
  try {
    detail::ensureInterpreter();
    py::gil_scoped_acquire gil;
    Expected<py::object> code = detail::compileChecked(source, "evaluate");
    if (!code) {
      return unexpected(code.error());
    }
    (void)pjHelpers();  // built here, unarmed: building them compiles, which a run's armed hook refuses
    (void)detail::pjNamespaces();
    return code->release().ptr();
  } catch (const std::exception& error) {
    return unexpected("Python error: " + std::string(error.what()));
  }
}

/// `script`'s code object, compiled by the first call; a compile error is kept and returned to every later call.
/// Takes the GIL to compile, after `script.mutex`. Without the GIL.
Expected<PyObject*> compiledCode(const PythonCode& script) {
  assert(Py_IsInitialized() == 0 || PyGILState_Check() == 0);  // the GIL after the mutex, never before
  const std::lock_guard lock(script.mutex);
  if (!script.compiled) {
    Expected<PyObject*> code = compileNow(script.source);
    std::string().swap(script.source);
    script.compiled = true;
    if (code) {
      script.code = *code;
    } else {
      script.error = std::move(code.error());
    }
  }
  if (script.code == nullptr) {
    return unexpected(script.error);
  }
  return script.code;
}

}  // namespace

bool pythonAvailable() {
  return true;
}

void prestartPythonHelper() {
  (void)PythonHelper::instance();
}

namespace {
std::atomic<std::uint64_t> g_validate_wait_for_test{0};  ///< 0: `kPythonValidateWaitMillis`
}  // namespace

void setPythonValidateWaitForTest(std::uint64_t millis) {
  g_validate_wait_for_test.store(millis, std::memory_order_relaxed);
}

Expected<sdk::ScriptCheck> checkOnDemandScriptPython(std::string_view source) {
  struct Pending {
    std::mutex mutex;
    std::condition_variable done;
    std::optional<Status> result;  ///< guarded by mutex
    bool finished = false;         ///< guarded by mutex: the helper is no longer busy with it
    bool abandoned = false;        ///< guarded by mutex: the caller stopped waiting, so the helper skips the compile
  };
  PythonHelper& helper = PythonHelper::instance();
  if (helper.busy()) {  // behind an item that may hold the helper for as long as a script holds the GIL
    return sdk::ScriptCheck{sdk::ScriptCheckStatus::kUnchecked, "not checked: the Python helper is busy"};
  }
  auto pending = std::make_shared<Pending>();
  // A fresh handle, compiled and dropped on the helper (inline, under the GIL): nothing a recipe holds. The waiter
  // hears the result once the helper is no longer busy, so its next validate is not refused as busy.
  helper.post(
      [pending, throwaway = *deferOnDemandScriptPython(source)] {
        {
          const std::lock_guard lock(pending->mutex);
          if (pending->abandoned) {
            return;
          }
        }
        const Expected<PyObject*> code = compiledCode(*throwaway.code);
        const std::lock_guard lock(pending->mutex);
        pending->result = code ? okStatus() : Status(unexpected(code.error()));
      },
      [pending] {
        {
          const std::lock_guard lock(pending->mutex);
          pending->finished = true;
        }
        pending->done.notify_one();
      });
  std::unique_lock lock(pending->mutex);
  const std::uint64_t override_millis = g_validate_wait_for_test.load(std::memory_order_relaxed);
  const std::chrono::milliseconds bound(override_millis != 0 ? override_millis : kPythonValidateWaitMillis);
  if (!pending->done.wait_for(lock, bound, [&pending] { return pending->finished; })) {
    pending->abandoned = true;
    return sdk::ScriptCheck{
        sdk::ScriptCheckStatus::kUnchecked, "not checked: Python did not compile it within " +
                                                std::to_string(bound.count()) + " ms (busy or still booting)"};
  }
  if (!*pending->result) {
    return sdk::ScriptCheck{sdk::ScriptCheckStatus::kInvalid, pending->result->error()};
  }
  return sdk::ScriptCheck{};
}

PyObject* detail::codeObjectOf(const CompiledPythonScript& script) {
  const std::lock_guard lock(script.code->mutex);
  return script.code->code;
}

Expected<CompiledPythonScript> deferOnDemandScriptPython(std::string_view source) {
  return CompiledPythonScript{
      std::shared_ptr<const PythonCode>(new PythonCode{.source = std::string(source)}, &release)};
}

Expected<CompiledPythonScript> compileOnDemandScriptPython(std::string_view source) {
  Expected<CompiledPythonScript> script = deferOnDemandScriptPython(source);
  if (const Expected<PyObject*> code = compiledCode(*script->code); !code) {
    return unexpected(code.error());
  }
  return script;
}

Expected<ScriptBundle, ScriptError> runOnDemandScriptPython(
    const CompiledPythonScript& script, Span<const NamedValue> inputs, std::string_view params_json,
    Span<const OutputSpec> outputs, const RunOptions& options) {
  const native::NativeLimits native_limits{detail::nativeBudgetOf(options), options.cancelled};
  ScriptMedia media(options.image_decoder, native_limits, options.result_stamp, options.video_source);
  detail::PythonRun run(options, media);
  const auto fail = [&run](std::string message) {
    const bool over_budget = detail::isOverBudget(message);
    return unexpected(ScriptError{std::move(message), run.observed_query_clock, run.observed_tf, over_budget});
  };
  if (script.code == nullptr) {
    return fail("the python script is not compiled");
  }
  for (const NamedValue& input : inputs) {
    if (std::string problem = detail::inputProblem(input); !problem.empty()) {
      return fail(std::move(problem));
    }
  }
  const Expected<nlohmann::json> params = detail::parseParams(params_json);
  if (!params) {
    return fail(params.error());
  }
  if (options.cancelled && options.cancelled()) {
    return fail("script cancelled");  // first: a cancelled or removed run compiles nothing
  }
  const Expected<PyObject*> code = compiledCode(*script.code);  // before the scope: its armed hook refuses compiles
  if (!code) {
    return fail(code.error());
  }

  native::MemoryReservation strings(native_limits.budget);
  Expected<std::vector<detail::Returned>> returned = std::vector<detail::Returned>{};
  {
    py::gil_scoped_acquire gil;
    // Every script object dies inside the scope, so a finalizer it triggers runs armed and timed. The host's own
    // constructions are charged too, and fail like the script's.
    detail::ScriptCallScope scope(
        std::make_shared<native::MemoryBudget>(options.limits.mem_bytes), options.limits.python_watchdog_millis,
        detail::Profile::kStrict, options.cancel_flags);
    try {
      const PjHelpers& pj = pjHelpers();
      const detail::PjNamespaces& namespaces = detail::pjNamespaces();
      run.unavailable = pj.unavailable.ptr();
      const py::dict globals = detail::checked(PyDict_New());
      const detail::ClearedOnExit cleared(globals);  // its functions hold it: cleared here, on every exit
      setItem(globals, pj.builtins_key, detail::strictBuiltins());
      setItem(globals, pj.name_key, pj.module_name);
      // Checked CPython calls throughout: pybind11's own fail through pybind11_fail, leaving the MemoryError pending.
      const py::object pj_members = detail::checked(PyDict_New());
      setItem(pj_members, interned("unavailable"), pj.unavailable);
      setItem(pj_members, interned("scene"), py::reinterpret_borrow<py::object>(namespaces.scene));
      setItem(pj_members, interned("annotations"), py::reinterpret_borrow<py::object>(namespaces.annotations));
      setItem(pj_members, interned("cloud"), py::reinterpret_borrow<py::object>(namespaces.cloud));
      setItem(pj_members, interned("voxels"), py::reinterpret_borrow<py::object>(namespaces.voxels));
      setItem(pj_members, interned("tf"), py::reinterpret_borrow<py::object>(namespaces.tf));
      const py::object no_arguments = detail::checked(PyTuple_New(0));
      setItem(
          globals, pj.pj_key,
          detail::checked(PyObject_Call(pj.namespace_type.ptr(), no_arguments.ptr(), pj_members.ptr())));
      const py::object script_inputs = detail::checked(PyDict_New());
      for (const NamedValue& input : inputs) {
        setItem(script_inputs, detail::newString(input.name), toPython(input, media));
      }
      detail::runCode(*code, globals);
      // Read raw: the script may have rebound or deleted `evaluate` at its top level.
      PyObject* evaluate = PyDict_GetItem(globals.ptr(), pj.evaluate_key.ptr());
      if (evaluate == nullptr) {
        returned = unexpected(std::string("the script must define a top-level function named 'evaluate'"));
      } else {
        const py::object script_params = toPython(*params);
        const py::object result =
            detail::checked(PyObject_CallFunctionObjArgs(evaluate, script_inputs.ptr(), script_params.ptr(), nullptr));
        returned = collectReturned(result.ptr(), strings);
      }
    } catch (py::error_already_set& error) {
      returned = unexpected(
          error.matches(PyExc_MemoryError) ? detail::vmLimitMessage(options.limits.mem_bytes)
                                           : detail::pythonErrorText(error));
    } catch (const std::exception& error) {  // pybind11_fail and the like: still a failed run, never an escape
      PyErr_Clear();
      returned = unexpected("Python error: " + std::string(error.what()));
    }
    detail::releaseRecycledViews();  // their blocks keep this run's budget alive
    const bool timed_out = scope.timedOut();
    if (scope.overBudget()) {  // even when the script caught the MemoryError, and first: it may also be out of time
      returned = unexpected(detail::vmLimitMessage(options.limits.mem_bytes));
    } else if (timed_out) {
      returned = unexpected(detail::timeoutMessage("evaluate()", options.limits.python_watchdog_millis));
    }
  }
  if (options.cancelled && options.cancelled()) {  // first: the watchdog's interrupt is how it ended
    return fail("script cancelled");
  }
  if (!returned) {
    return fail(std::move(returned.error()));
  }
  Expected<ScriptBundle> bundle = detail::bindReturned(*returned, outputs);
  if (!bundle) {
    return fail(std::move(bundle.error()));
  }
  bundle->observed_query_clock = run.observed_query_clock;
  bundle->observed_tf = run.observed_tf;
  return std::move(*bundle);
}

}  // namespace PJ::scripting
