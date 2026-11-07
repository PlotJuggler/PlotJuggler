// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Test probes for code that cannot include CPython: how often the Python compiler ran, and a held GIL.

#include <condition_variable>
#include <mutex>
#include <thread>

namespace PJ::test {

/// The Python "compile" audit events since the first call, which starts counting; call it after the interpreter
/// exists (any Python compile starts it). Only the first call takes the GIL.
int pythonCompileEvents();

/// Holds the GIL on a thread of its own from construction (which returns once it is held) until destruction, as a
/// script stuck in a builtin C loop does on the Python lane: nothing else can take it meanwhile. The interpreter must
/// exist.
class PythonGilHold {
 public:
  PythonGilHold();
  ~PythonGilHold();
  PythonGilHold(const PythonGilHold&) = delete;
  PythonGilHold& operator=(const PythonGilHold&) = delete;

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool held_ = false;
  bool release_ = false;
  std::thread thread_;
};

}  // namespace PJ::test
