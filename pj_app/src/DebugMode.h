#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

namespace PJ {

// `--debug-mode`: shows developer-only UI. Set once in main() before the window is built, then read-only, which
// is why a plain global is safe.
void setDebugMode(bool enabled);
[[nodiscard]] bool isDebugMode();

}  // namespace PJ
