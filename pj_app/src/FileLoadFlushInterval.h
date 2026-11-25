// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <chrono>

namespace PJ {

// Least time between two flush-and-notify cycles of a file load's worker (FileLoader). Each flush seals a chunk per
// topic with rows, so a shorter interval multiplies chunks and their per-column cost (TECHNICAL_NOTES "File-load flush
// interval"). Tests and test sources that need a mid-load flush wait longer than this; SDK-only, so a test plugin
// can include it.
inline constexpr std::chrono::milliseconds kFileLoadFlushInterval{250};

}  // namespace PJ
