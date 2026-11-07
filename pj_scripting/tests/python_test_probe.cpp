// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "python_test_probe.h"

#include <Python.h>

#include <atomic>
#include <cstring>

namespace PJ::test {
namespace {

std::atomic<int> g_compiles{0};

int countCompiles(const char* event, PyObject* /*args*/, void* /*userdata*/) {
  if (std::strcmp(event, "compile") == 0) {
    ++g_compiles;
  }
  return 0;
}

}  // namespace

int pythonCompileEvents() {
  static const bool counting = [] {
    const PyGILState_STATE gil = PyGILState_Ensure();
    const bool added = PySys_AddAuditHook(&countCompiles, nullptr) == 0;
    PyGILState_Release(gil);
    return added;
  }();
  (void)counting;
  return g_compiles.load();
}

PythonGilHold::PythonGilHold() {
  thread_ = std::thread([this] {
    const PyGILState_STATE gil = PyGILState_Ensure();
    std::unique_lock lock(mutex_);
    held_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return release_; });
    lock.unlock();
    PyGILState_Release(gil);
  });
  std::unique_lock lock(mutex_);
  changed_.wait(lock, [this] { return held_; });
}

PythonGilHold::~PythonGilHold() {
  {
    const std::lock_guard lock(mutex_);
    release_ = true;
  }
  changed_.notify_all();
  thread_.join();
}

}  // namespace PJ::test
