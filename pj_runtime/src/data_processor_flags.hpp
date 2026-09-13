#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>

#include "pj_base/sdk/plugin_data_api.hpp"

namespace PJ {

// Every flag bit this host understands. The ABI contract (plugin_data_api.h's
// `flags` paragraph) requires a host to reject an unknown/reserved bit rather
// than silently drop it — silently ignoring one is indistinguishable, from the
// plugin's side, from the host applying it and the effect just not taking.
constexpr uint32_t kKnownFlags = PJ_DATA_PROCESSOR_FLAG_EPHEMERAL | PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT;

}  // namespace PJ
