// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <Python.h>
#include <pybind11/pybind11.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "pj_base/builtin/builtin_object.hpp"

// The object half of the Python on-demand binding (docs/derived_objects/ARCHITECTURE.md §6, §14), the twin of
// object_binder.h over the same native operations: objects as read-only views over their SDK field tables (a list is a
// lazy sequence read through `list_at`, an int64 field a Python int), the point-cloud, image and decoded-pixel methods,
// the `pj.scene` / `pj.annotations` / `pj.cloud` / `pj.voxels` builders and `pj.tf`. The surface is
// python_object_script.h's.
//
// Every operation that walks the data runs without the GIL: it reads only C++ state the call's arguments keep alive.
// The types are plain CPython types, registered once per process: they have no constructor, take no attribute and
// expose no buffer, nothing a script reaches leads to a module's globals, and an allocation the cap refuses fails the
// call cleanly. A builder works only in the run that made it. A call that needs the run's limits, media or observations
// (`PythonRun`, the thread's) raises outside its run instead of reaching a dead one; field reads, `count`, `bounds`,
// `extreme_point` and `xyz_points` iteration work anywhere.

namespace PJ::scripting {
class ScriptMedia;
struct RunOptions;
struct CompiledPythonScript;
}  // namespace PJ::scripting

namespace PJ::scripting::detail {

namespace py = pybind11;

/// Steals `object`, or throws the pending Python error when its construction failed, as when the cap refused it.
/// pybind11's own constructors (`py::int_`, `py::dict`, ...) fail through `pybind11_fail` instead, which asserts in a
/// debug build.
[[nodiscard]] py::object checked(PyObject* object);

/// A `str` of the UTF-8 `text`, or the pending Python error when the cap refused it.
[[nodiscard]] py::object newString(std::string_view text);

/// What one run's binder calls read and record; the calling thread's run while alive. Its members are plain pointers:
/// a pybind11 type's hidden visibility does not fit a struct the library exports.
struct PythonRun {
  PythonRun(const RunOptions& run_options, ScriptMedia& run_media);
  ~PythonRun();
  PythonRun(const PythonRun&) = delete;
  PythonRun& operator=(const PythonRun&) = delete;

  const RunOptions& options;
  ScriptMedia& media;
  const std::uint64_t generation;   ///< unique per run: a builder made in it works in no other
  PyObject* unavailable = nullptr;  ///< `pj.unavailable` (borrowed), which makes the marker an operation returns
  bool observed_query_clock = false;
  bool observed_tf = false;

 private:
  PythonRun* previous_;
};

/// `pj.scene`, `pj.annotations`, `pj.cloud`, `pj.voxels` and `pj.tf`, shared by every run (they hold nothing, and
/// take no attribute); strong references, never released.
struct PjNamespaces {
  PyObject* scene = nullptr;
  PyObject* annotations = nullptr;
  PyObject* cloud = nullptr;
  PyObject* voxels = nullptr;
  PyObject* tf = nullptr;
};

/// Frees the struct-view blocks the runs on any thread recycled (each stays charged to, and keeps alive, the budget of
/// the run that allocated it). Every run calls it at its end, inside its ScriptCallScope. GIL held.
void releaseRecycledViews();

/// How many freed struct-view blocks wait for reuse: 0 once a run has ended. For tests. GIL held.
[[nodiscard]] std::size_t recycledViewCount();

/// Registers the binder's types on the first call, which must hold the GIL outside any ScriptCallScope (registering is
/// host work the armed hook could refuse); later calls only read.
[[nodiscard]] const PjNamespaces& pjNamespaces();

/// `object` as a read-only script object (it shares the value) holding `keepalive` (`NamedValue::keepalive`) for as
/// long as it or a view read from it lives; `canEnterScript(object)` must hold. GIL held.
[[nodiscard]] py::object newObject(sdk::BuiltinObject object, std::shared_ptr<const void> keepalive = {});

/// The object `value` wraps, matched by exact type so no script code runs, or null.
[[nodiscard]] const sdk::BuiltinObject* toObject(PyObject* value);

/// What keeps the object `value` wraps alive (a copy of it, its native charge, its input's keepalive), for a result to
/// keep, so the charge counts until the result dies. Null when `value` is not an object.
[[nodiscard]] std::shared_ptr<const void> anchorOf(PyObject* value);

/// The code object `script` holds (borrowed; null until compiled), for a test's weak reference to it. Takes the
/// script's lock: call it without the GIL.
[[nodiscard]] PyObject* codeObjectOf(const CompiledPythonScript& script);

}  // namespace PJ::scripting::detail
