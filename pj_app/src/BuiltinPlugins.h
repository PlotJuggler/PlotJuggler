#pragma once

// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/PluginRuntimeCatalog.h"

namespace pj_app {

// The plugin entry points linked into this binary; pj_runtime registers them at AppSession startup.
PJ::StaticPluginSet builtInPlugins();

}  // namespace pj_app
