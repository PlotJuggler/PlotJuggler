#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <string>
#include <string_view>
#include <unordered_map>

namespace PJ {

/// Maps a scalar to a CSS color name or "#rrggbb" string. The returned pointer must stay valid until the next call to
/// the same callback.
using ColorMapEvalFn = const char* (*)(double value, void* user_ctx);

/// Named colormap callbacks, at most one active. No locking.
class ColorMapRegistry {
 public:
  ColorMapRegistry() = default;
  ~ColorMapRegistry() = default;

  ColorMapRegistry(const ColorMapRegistry&) = delete;
  ColorMapRegistry& operator=(const ColorMapRegistry&) = delete;

  /// Replaces a map of the same name. The registered map becomes active.
  void registerMap(std::string_view name, ColorMapEvalFn eval_fn, void* user_ctx);

  /// If it was the active map, nothing is active afterwards.
  void unregisterMap(std::string_view name);

  /// No-op if `name` is not registered.
  void setActive(std::string_view name);

  /// Empty when no map is active.
  [[nodiscard]] std::string evaluate(double value) const;

  [[nodiscard]] bool hasActive() const;

  [[nodiscard]] const std::string& activeName() const {
    return active_;
  }

 private:
  struct Entry {
    ColorMapEvalFn eval_fn;
    void* user_ctx;
  };
  std::unordered_map<std::string, Entry> maps_;
  std::string active_;
};

}  // namespace PJ
