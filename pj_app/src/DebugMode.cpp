// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "DebugMode.h"

namespace PJ {

namespace {
bool g_debug_mode = false;
}  // namespace

void setDebugMode(bool enabled) {
  g_debug_mode = enabled;
}

bool isDebugMode() {
  return g_debug_mode;
}

}  // namespace PJ
