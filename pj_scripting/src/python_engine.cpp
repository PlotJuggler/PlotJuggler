// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Embedded-CPython backend for Data Processors. Implements the language-agnostic ScriptEngine seam with pybind11's
// embedding API, over the interpreter and hardening it shares with the on-demand lane (python_sandbox.h). See
// python_engine.h for the Python "filter module" contract and its limits.

#include "pj_scripting/python_engine.h"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_scripting/sandbox.h"
#include "python_sandbox.h"

namespace py = pybind11;

namespace PJ::scripting {
namespace {

using detail::ScriptCallScope;
using detail::timeoutMessage;

/// A filter's allocation cap refused an allocation, which the script may have caught.
std::string memoryMessage(const native::MemoryBudget& budget) {
  return "MemoryError: script memory limit (" + native::describeBytes(budget.limit()) + ") exceeded";
}

// The imports a filter script may reach: filters compute over time series (trigonometry, constants, statistics), and a
// script that reaches for the filesystem, the network or a subprocess is told so, not given the module's own error.
//
// Not a security boundary: CPython cannot be confined from within, and an object-graph walk reaches past any
// namespace handed to a script. Filter sources are trusted input. `open` stays a builtin; the shared audit hook
// (python_sandbox.h) refuses the call while a filter runs.
constexpr std::array<std::string_view, 3> kAllowedImports{"math", "cmath", "statistics"};

// Only the root package is matched: "math.foo" follows the "math" entry, and `from x import y` arrives as "x".
bool importAllowed(std::string_view module) {
  const std::string_view root = module.substr(0, module.find('.'));
  return std::find(kAllowedImports.begin(), kAllowedImports.end(), root) != kAllowedImports.end();
}

// A module namespace whose __builtins__ is the real one with __import__ swapped for the allowlist gate. Allowed modules
// import their own dependencies freely: their bodies run under their own globals, with unrestricted builtins.
py::dict makeFilterNamespace() {
  py::dict gated = py::module_::import("builtins").attr("__dict__").attr("copy")();
  py::object real_import = gated["__import__"];

  std::string allowed;
  for (const auto& name : kAllowedImports) {
    allowed += (allowed.empty() ? "" : ", ");
    allowed += name;
  }

  gated["__import__"] = py::cpp_function([real_import, allowed](py::args args, py::kwargs kwargs) {
    if (args.empty()) {
      throw py::import_error("__import__() requires a module name");
    }
    const auto name = py::cast<std::string>(py::str(args[0]));
    if (!importAllowed(name)) {
      throw py::import_error("module '" + name + "' is not available in Data Processors (allowed: " + allowed + ")");
    }
    return real_import(*args, **kwargs);
  });

  py::dict ns;
  ns["__builtins__"] = gated;
  return ns;
}

/// Why the interpreter cannot run filters, empty when it can: a failed boot fails the call, through its Expected.
std::string bootFailure() {
  try {
    detail::ensureInterpreter();
  } catch (const std::exception& e) {
    return e.what();
  }
  return {};
}

/// `source` compiled on the host, unarmed: the armed run executes the code object (detail::runCode). The compile is
/// outside the filter's cap, so the source size bounds it.
Expected<py::object> compileSource(const std::string& source) {
  if (source.size() > kMaxSourceBytes) {
    return PJ::unexpected("script larger than " + native::describeBytes(kMaxSourceBytes));
  }
  try {
    return py::module_::import("builtins").attr("compile")(source, "<string>", "exec");
  } catch (const py::error_already_set& e) {
    return PJ::unexpected(std::string("Python error: ") + detail::pythonErrorText(e));
  }
}

std::string attrString(const py::object& obj, const char* name, const std::string& fallback) {
  if (!py::hasattr(obj, name)) {
    return fallback;
  }
  py::object v = obj.attr(name);
  if (v.is_none()) {
    return fallback;
  }
  return py::str(v).cast<std::string>();
}

// One live Python filter: owns the module namespace (keepalive) and the instance
// object. All py-object touches happen under the GIL.
class PythonFilterInstance final : public FilterInstance {
 public:
  PythonFilterInstance(
      py::dict ns, py::object instance, std::shared_ptr<native::MemoryBudget> memory, std::uint64_t call_millis,
      bool text_output)
      : ns_(std::move(ns)),
        instance_(std::move(instance)),
        memory_(std::move(memory)),
        call_millis_(call_millis),
        text_output_(text_output) {}

  // Dropping the instance and the namespace runs the script's finalizers: under the scope, like any script call.
  ~PythonFilterInstance() override {
    py::gil_scoped_acquire gil;
    ScriptCallScope scope(memory_, call_millis_, detail::Profile::kCompatible);
    detail::ClearedOnExit cleared(ns_);
    instance_ = py::object();
  }

  PythonFilterInstance(const PythonFilterInstance&) = delete;
  PythonFilterInstance& operator=(const PythonFilterInstance&) = delete;

  Result calculate(double t, double v) override {
    return guarded("calculate()", Result{true, 0.0, std::nullopt, {}}, [&]() { return runCalculate(t, v); });
  }

  MimoResult calculateMimo(double t, PJ::Span<const double> inputs) override {
    return guarded("calculate()", MimoResult{true, {}, {}}, [&]() { return runCalculateMimo(t, inputs); });
  }

  void reset() override {
    (void)guarded("reset()", false, [&]() {
      if (py::hasattr(instance_, "reset")) {
        try {
          instance_.attr("reset")();
        } catch (const py::error_already_set& e) {
          fail(e.what());
        }
      }
      return true;
    });
  }

  [[nodiscard]] bool failed() const override {
    return failed_;
  }
  [[nodiscard]] const std::string& error() const override {
    return error_;
  }

 private:
  /// Runs `body` (one script call) under the GIL and a ScriptCallScope; `on_fail` when the instance has failed,
  /// before or during the call (a timeout fails it even if the script caught the TimeoutError).
  template <typename R, typename Body>
  R guarded(const char* call, R on_fail, Body&& body) {
    if (failed_) {
      return on_fail;
    }
    py::gil_scoped_acquire gil;
    ScriptCallScope scope(memory_, call_millis_, detail::Profile::kCompatible);
    R out = body();
    const bool timed_out = scope.timedOut();
    if (scope.overBudget()) {  // first: a memory stop the script kept catching may also have run out of time
      fail(memoryMessage(*memory_));
    } else if (timed_out) {
      fail(timeoutMessage(call, call_millis_));
    }
    return failed_ ? on_fail : out;
  }

  /// One returned value of the kind the class declares: a str for a text class (a number is refused, so a forgotten
  /// label is not stored as "3"), else a value pybind11 casts to double. False when it is not.
  bool readValue(const py::handle& item, double& value, std::string& text) const {
    if (text_output_) {
      if (!py::isinstance<py::str>(item)) {
        return false;
      }
      text = item.cast<std::string>();
      return true;
    }
    if (py::isinstance<py::str>(item)) {
      return false;
    }
    value = item.cast<double>();
    return true;
  }

  [[nodiscard]] const char* wrongKindMessage() const {
    return text_output_ ? "calculate() must return a str per output (the class declares output = \"string\")"
                        : "calculate() must return a number per output (declare output = \"string\" to return text)";
  }

  /// The script calls proper, run by guarded(). A Python error fails the instance.
  Result runCalculate(double t, double v) {
    try {
      py::object r = instance_.attr("calculate")(t, v);
      if (r.is_none()) {
        return Result{true, 0.0, std::nullopt, {}};
      }
      // A 2-sequence is (out_time, value); a scalar is emitted at the input time, as in Luau.
      Result out{false, 0.0, std::nullopt, {}};
      py::object value = r;
      if (py::isinstance<py::tuple>(r) || py::isinstance<py::list>(r)) {
        auto seq = r.cast<py::sequence>();
        if (seq.size() == 2) {
          out.out_time = seq[0].cast<double>();
          value = seq[1];
        } else if (seq.size() == 1) {
          value = seq[0];
        } else {
          fail("calculate() returned a sequence of unexpected length (want 1 or 2)");
          return Result{true, 0.0, std::nullopt, {}};
        }
      }
      if (!readValue(value, out.value, out.text)) {
        fail(wrongKindMessage());
        return Result{true, 0.0, std::nullopt, {}};
      }
      return out;
    } catch (const py::error_already_set& e) {
      fail(e.what());
      return Result{true, 0.0, std::nullopt, {}};
    } catch (const std::exception& e) {
      fail(e.what());
      return Result{true, 0.0, std::nullopt, {}};
    }
  }

  MimoResult runCalculateMimo(double t, PJ::Span<const double> inputs) {
    try {
      py::list args;
      for (std::size_t i = 1; i < inputs.size(); ++i) {
        args.append(inputs[i]);
      }
      const double primary = inputs.empty() ? 0.0 : inputs[0];
      py::object r = instance_.attr("calculate")(t, primary, *py::tuple(args));
      if (r.is_none()) {
        return MimoResult{true, {}, {}};
      }
      MimoResult out;
      out.suppress = false;
      const auto take = [&](const py::handle& item) {
        double value = 0.0;
        std::string text;
        if (!readValue(item, value, text)) {
          return false;
        }
        if (text_output_) {
          out.texts.push_back(std::move(text));
        } else {
          out.values.push_back(value);
        }
        return true;
      };
      bool ok = true;
      if (py::isinstance<py::tuple>(r) || py::isinstance<py::list>(r)) {
        for (auto item : r.cast<py::sequence>()) {
          ok = ok && take(item);
        }
      } else {
        ok = take(r);
      }
      if (!ok) {
        fail(wrongKindMessage());
        return MimoResult{true, {}, {}};
      }
      return out;
    } catch (const py::error_already_set& e) {
      fail(e.what());
      return MimoResult{true, {}, {}};
    } catch (const std::exception& e) {
      fail(e.what());
      return MimoResult{true, {}, {}};
    }
  }

  void fail(std::string msg) {
    failed_ = true;
    error_ = std::move(msg);
  }

  py::dict ns_;  // module namespace, kept alive
  py::object instance_;
  std::shared_ptr<native::MemoryBudget> memory_;  // this instance's live bytes, across calls
  std::uint64_t call_millis_;
  bool text_output_;  ///< the class declares output = "string"
  bool failed_ = false;
  std::string error_;
};

class PythonEngine final : public ScriptEngine {
 public:
  explicit PythonEngine(BudgetLimits limits) : limits_(limits) {}

  // The scope covers the attribute reads too: a script's metaclass or `__str__` runs on them.
  Expected<std::vector<FilterClass>> inspectModule(const std::string& source, const std::string& origin) override {
    if (std::string failure = bootFailure(); !failure.empty()) {
      return PJ::unexpected(std::move(failure));
    }
    py::gil_scoped_acquire gil;
    Expected<py::object> code = compileSource(source);
    if (!code) {
      return PJ::unexpected(code.error());
    }
    auto memory = std::make_shared<native::MemoryBudget>(limits_.mem_bytes);
    ScriptCallScope scope(memory, limits_.python_watchdog_millis, detail::Profile::kCompatible);
    Expected<std::vector<FilterClass>> out = inspect(*code, source, origin);
    const bool timed_out = scope.timedOut();
    if (scope.overBudget()) {
      return PJ::unexpected(memoryMessage(*memory));
    }
    if (timed_out) {
      return PJ::unexpected(timeoutMessage("module evaluation", limits_.python_watchdog_millis));
    }
    return out;
  }

  Expected<std::unique_ptr<FilterInstance>> createInstance(
      const FilterClass& klass, const std::string& params_json) override {
    if (std::string failure = bootFailure(); !failure.empty()) {
      return PJ::unexpected(std::move(failure));
    }
    py::gil_scoped_acquire gil;
    Expected<py::object> code = compileSource(klass.source);
    if (!code) {
      return PJ::unexpected(code.error());
    }
    auto memory = std::make_shared<native::MemoryBudget>(limits_.mem_bytes);  // the instance's, for its whole life
    ScriptCallScope scope(memory, limits_.python_watchdog_millis, detail::Profile::kCompatible);
    Expected<std::unique_ptr<FilterInstance>> out = create(*code, params_json, memory, klass.textOutput());
    const bool timed_out = scope.timedOut();
    if (scope.overBudget()) {
      return PJ::unexpected(memoryMessage(*memory));
    }
    if (timed_out) {
      return PJ::unexpected(timeoutMessage("module evaluation + create()", limits_.python_watchdog_millis));
    }
    return out;
  }

 private:
  static Expected<std::vector<FilterClass>> inspect(
      const py::object& code, const std::string& source, const std::string& origin) {
    try {
      py::dict ns = makeFilterNamespace();
      detail::ClearedOnExit cleared(ns);
      detail::runCode(code, ns);
      if (!ns.contains("T")) {
        return PJ::unexpected("Python filter module must define a top-level class named 'T'");
      }
      py::object cls = ns["T"];
      if (!py::hasattr(cls, "id")) {
        return PJ::unexpected("Python filter class 'T' must define an 'id'");
      }
      FilterClass fc;
      fc.id = attrString(cls, "id", "");
      if (fc.id.empty()) {
        return PJ::unexpected("Python filter class 'T' has an empty 'id'");
      }
      fc.name = attrString(cls, "name", fc.id);
      fc.description = attrString(cls, "description", "");
      fc.version = attrString(cls, "version", "");
      fc.output_kind = attrString(cls, "output", "double");
      fc.source = source;
      fc.origin = origin;
      return std::vector<FilterClass>{std::move(fc)};
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("Python error: ") + detail::pythonErrorText(e));
    }
  }

  Expected<std::unique_ptr<FilterInstance>> create(
      const py::object& code, const std::string& params_json, std::shared_ptr<native::MemoryBudget> memory,
      bool text_output) const {
    try {
      py::dict nsd = makeFilterNamespace();
      detail::ClearedOnExit cleared(nsd);
      detail::runCode(code, nsd);
      if (!nsd.contains("T")) {
        return PJ::unexpected("Python filter module must define a top-level class named 'T'");
      }
      py::object cls = nsd["T"];
      py::object params;
      if (params_json.empty() || params_json == "{}") {
        params = py::dict();
      } else {
        params = py::module_::import("json").attr("loads")(params_json);
      }
      if (!py::hasattr(cls, "create")) {
        return PJ::unexpected("Python filter class 'T' must define a static create(params)");
      }
      py::object instance = cls.attr("create")(params);
      if (!py::hasattr(instance, "calculate")) {
        return PJ::unexpected("the object returned by T.create() must have a calculate(self, time, value) method");
      }
      cleared.release();
      return std::unique_ptr<FilterInstance>(std::make_unique<PythonFilterInstance>(
          std::move(nsd), std::move(instance), std::move(memory), limits_.python_call_millis, text_output));
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("Python error: ") + detail::pythonErrorText(e));
    }
  }

  BudgetLimits limits_;
};

}  // namespace

std::shared_ptr<ScriptEngine> makePythonEngine(BudgetLimits limits) {
  return std::make_shared<PythonEngine>(limits);
}

}  // namespace PJ::scripting
