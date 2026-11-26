// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <chrono>

namespace PJ {

// Least time between two flushes of a file load's worker (TECHNICAL_NOTES "File-load flush interval"). Header-only and
// Qt-free, so test plugins can wait past it.
inline constexpr std::chrono::milliseconds kFileLoadFlushInterval{250};

}  // namespace PJ
