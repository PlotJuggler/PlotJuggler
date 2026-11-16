#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

namespace PJ {

// App-wide "debug mode" gate. Set once at startup from the `--debug-mode`
// CLI flag (in main.cpp) and read-only thereafter, so a plain global is
// safe — there is no concurrent mutation after launch. Gates developer-only
// UI that ships hidden in normal runs (currently the Appearance page's
// chrome-metric scrubbers and their reset button in PreferencesDialog).
void setDebugMode(bool enabled);
[[nodiscard]] bool isDebugMode();

}  // namespace PJ
