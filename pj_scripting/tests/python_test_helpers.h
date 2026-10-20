// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Host-side helpers of the Python tests: escaping data into script source, a file a script must not reach, and fake
// modules registered under an allowed root, as a binder registers a native operation.

#include <Python.h>
#include <gtest/gtest.h>
#include <pybind11/pybind11.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "python_sandbox.h"

namespace PJ::scripting::test {

namespace py = pybind11;

/// `text` as a single-quoted Python literal: a Windows path's backslashes stay characters, not escapes.
inline std::string pyLiteral(std::string_view text) {
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
  std::string path = ::testing::TempDir() + "pj_py_victim_" +
                     ::testing::UnitTest::GetInstance()->current_test_info()->test_suite_name() + "_" +
                     ::testing::UnitTest::GetInstance()->current_test_info()->name();
  Victim() {
    std::ofstream(path) << "x";
  }
  ~Victim() {
    std::filesystem::remove(path);
  }
  Victim(const Victim&) = delete;
  Victim& operator=(const Victim&) = delete;
};

/// What `math.pj_audit_probe.emit()` saw: the hook refused its event, or let it through.
struct AuditProbe {
  int refused = 0;
  int allowed = 0;
};

/// Registers `math.pj_audit_probe` once; its `emit()` raises the "os.system" audit event, which an armed hook
/// refuses, and `emit_compile()` the "compile" event, which only the strict profile refuses; both count the outcome.
/// Takes the GIL.
inline AuditProbe& auditProbe() {
  static AuditProbe probe;
  static const bool registered = [] {
    detail::ensureInterpreter();
    py::gil_scoped_acquire gil;
    py::module_ module = py::module_::import("types").attr("ModuleType")("math.pj_audit_probe");
    module.attr("emit") = py::cpp_function([]() {
      if (PySys_Audit("os.system", "(s)", "pj_audit_probe") < 0) {
        ++probe.refused;
        throw py::error_already_set();
      }
      ++probe.allowed;
    });
    module.attr("emit_compile") = py::cpp_function([]() {  // what compiling a string raises
      if (PySys_Audit("compile", "(ss)", "x = 1", "<string>") < 0) {
        ++probe.refused;
        throw py::error_already_set();
      }
      ++probe.allowed;
    });
    py::module_::import("sys").attr("modules")["math.pj_audit_probe"] = module;
    return true;
  }();
  (void)registered;
  return probe;
}

}  // namespace PJ::scripting::test
