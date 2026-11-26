// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <chrono>

namespace PJ {

// How often a file load's worker flushes, so loaded rows reach the views. A flush visits every column of the topics
// with new rows (TECHNICAL_NOTES "File-load flush interval"). Header-only and Qt-free, so test plugins can wait past it.
inline constexpr std::chrono::milliseconds kFileLoadFlushInterval{250};

}  // namespace PJ
