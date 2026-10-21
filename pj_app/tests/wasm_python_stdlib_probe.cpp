// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A module the stdlib imports lazily, inside a function, breaks a strict script that reaches that function: inside a
// run the audit hook refuses the file open an import needs, and the browser's stdlib zip
// (cmake/wasm_python/stdlib_zip.py) may not even hold it. python_sandbox.cpp therefore loads such modules at the
// interpreter's boot. This probe walks what a strict on-demand script can reach and fails on every lazy import that
// is not loaded by then. Compiled only with PJ_WASM_WITH_PYTHON and PJ_WASM_ENABLE_INGRESS_PROBE; its own file
// because Python.h must come before the standard headers.

#include <Python.h>

#include <string>

namespace {

/// The strict profile's reach, as python_sandbox.cpp's import gate builds it: the allowed modules (kStrictImports),
/// their attributes not starting with "_", and modules only under an allowed root. Every function and method there
/// (a class's whole MRO, properties, nested code) has its imports checked, except those inside a try block (the
/// stdlib's own fallbacks): each must name a module already loaded, builtin or frozen. It imports nothing it checks;
/// what it loads itself (dis) comes after the snapshot. C code is not walked: decimal's _pydecimal fallback is checked
/// by a strict run instead (wasm_python_probe.inc). Unsandboxed, on a worker, outside any script call.
constexpr const char* kSurfaceWalk = R"PY(
import sys, _imp
loaded = set(sys.modules)
import dis, types

ALLOWED = ("math", "cmath", "statistics", "json")

def surface_problems():
    roots = set(ALLOWED)
    seen = set()
    problems = []

    def check(code, package, where):
        guarded = [(entry.start, entry.end) for entry in dis.Bytecode(code).exception_entries]
        instructions = list(dis.get_instructions(code))
        for index, instruction in enumerate(instructions):
            if instruction.opname != "IMPORT_NAME":
                continue
            if any(start <= instruction.offset < end for start, end in guarded):
                continue
            level, name = instructions[index - 2].argval, instruction.argval
            if level:
                base = package.rsplit(".", level - 1)[0]
                name = f"{base}.{name}" if name else base
            if name not in loaded and name not in sys.builtin_module_names and not _imp.is_frozen(name):
                problems.append(f"{where} imports {name}, which a strict run cannot load")
        for constant in code.co_consts:
            if isinstance(constant, types.CodeType):
                check(constant, package, where)

    def visit(value, where):
        if id(value) in seen:
            return
        seen.add(id(value))
        if isinstance(value, (staticmethod, classmethod)):
            value = value.__func__
        if isinstance(value, property):
            for accessor in (value.fget, value.fset, value.fdel):
                if accessor is not None:
                    visit(accessor, where)
        elif isinstance(value, types.FunctionType):
            module = sys.modules.get(value.__module__)
            check(value.__code__, getattr(module, "__package__", "") or "", where)
        elif isinstance(value, type):
            for klass in value.__mro__:
                for key, member in vars(klass).items():
                    visit(member, f"{where}.{key}")
        elif isinstance(value, types.ModuleType):
            if value.__name__.partition(".")[0] in roots:
                for key, member in vars(value).items():
                    if not key.startswith("_"):
                        visit(member, f"{value.__name__}.{key}")

    for name in ALLOWED:
        visit(__import__(name), name)
    return "; ".join(sorted(set(problems)))

problems = surface_problems()
)PY";

std::string describeRaised() {
  PyObject* raised = PyErr_GetRaisedException();
  PyObject* text = raised != nullptr ? PyObject_Str(raised) : nullptr;
  const char* utf8 = text != nullptr ? PyUnicode_AsUTF8(text) : nullptr;
  std::string out = "the walk raised: " + std::string(utf8 != nullptr ? utf8 : "?");
  Py_XDECREF(text);
  Py_XDECREF(raised);
  PyErr_Clear();
  return out;
}

}  // namespace

/// Empty when every lazy import the walk finds is loaded, else one "<where> imports <module>, ..." per gap. Needs the
/// interpreter booted (any compile does it) and should run before anything imports more; any thread.
std::string wasmPythonStdlibProblems() {
  const PyGILState_STATE gil = PyGILState_Ensure();
  std::string out;
  PyObject* scope = PyDict_New();
  PyDict_SetItemString(scope, "__builtins__", PyEval_GetBuiltins());
  PyObject* result = PyRun_String(kSurfaceWalk, Py_file_input, scope, scope);
  if (result == nullptr) {
    out = describeRaised();
  } else {
    PyObject* problems = PyDict_GetItemString(scope, "problems");  // borrowed
    const char* utf8 = problems != nullptr ? PyUnicode_AsUTF8(problems) : nullptr;
    out = utf8 != nullptr ? utf8 : "the walk left no result";
    Py_DECREF(result);
  }
  Py_DECREF(scope);
  PyGILState_Release(gil);
  return out;
}
