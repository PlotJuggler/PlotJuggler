// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#ifdef Q_OS_WASM
#include <emscripten/val.h>

#include <string>

namespace PJ {

// Qt gates BOTH its open and save File System Access paths on
// window.showOpenFilePicker. In the cross-origin-isolated (COOP/COEP) context a
// threaded build needs, those pickers are not dependable. Holding this masks
// only that capability probe, so a Qt getOpenFileContent / saveFileContent
// call made meanwhile picks Qt's portable <input type=file> or
// Blob/<a download> path; the browser API is restored on destruction.
class ScopedOpenFilePickerFallback {
 public:
  ScopedOpenFilePickerFallback()
      : window_(emscripten::val::global("window")),
        picker_(window_["showOpenFilePicker"]),
        had_own_property_(
            emscripten::val::global("Object")["prototype"]["hasOwnProperty"].call<bool>(
                "call", window_, std::string("showOpenFilePicker"))) {
    window_.set("showOpenFilePicker", emscripten::val::undefined());
  }

  ~ScopedOpenFilePickerFallback() {
    if (had_own_property_) {
      window_.set("showOpenFilePicker", picker_);
    } else {
      emscripten::val::global("Reflect").call<bool>("deleteProperty", window_, emscripten::val("showOpenFilePicker"));
    }
  }

  ScopedOpenFilePickerFallback(const ScopedOpenFilePickerFallback&) = delete;
  ScopedOpenFilePickerFallback& operator=(const ScopedOpenFilePickerFallback&) = delete;

 private:
  emscripten::val window_;
  emscripten::val picker_;
  bool had_own_property_;
};

}  // namespace PJ
#endif
