#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <array>
#include <string_view>

#include "pj_plugins/widget_qt_modules.h"

namespace PJ {

/// Qt modules the official PlotJuggler packages ship, as Widget plugins name them in the manifest's
/// `qt_modules` (without the "Qt" prefix). Derived from the SDK's canonical X-macro, so the list the
/// template CI checks and the list the host enforces cannot drift apart. A plugin may link a module
/// outside it on a developer machine, where Qt is installed in full, and fail on a user's.
inline constexpr auto kShippedQtModules = std::to_array<std::string_view>({
#define PJ_SHIPPED_QT_MODULE(name, since) #name,
    PJ_WIDGET_QT_MODULES(PJ_SHIPPED_QT_MODULE)
#undef PJ_SHIPPED_QT_MODULE
});

[[nodiscard]] inline bool isShippedQtModule(std::string_view module_name) {
  return std::ranges::find(kShippedQtModules, module_name) != kShippedQtModules.end();
}

}  // namespace PJ
