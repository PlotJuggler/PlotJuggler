#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file store_policy.hpp
 * @brief Qt-free quota, archive-path, and entry-type policy for immutable artifacts.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "pj_base/expected.hpp"
#include "pj_marketplace/archive_limits.hpp"
#include "pj_marketplace/store_rejection.hpp"

namespace PJ {

// The compressed / per-entry / total expanded caps and the path-depth cap are the
// shared archive budgets defined in archive_limits.hpp, and reused here so the
// store and the classic extractor agree on one set of numbers. Only the
// entry-count default carries a store-specific name.
/// Default archive-entry cap, bounding both file and directory metadata amplification.
inline constexpr size_t kDefaultMaximumFileCount = 4096U;

/// Budgets enforced while bytes are downloaded and before an object becomes visible.
struct StoreQuotas {
  uint64_t maximum_compressed_bytes = kDefaultMaximumCompressedBytes;
  uint64_t maximum_entry_expanded_bytes = kDefaultMaximumEntryExpandedBytes;
  uint64_t maximum_total_expanded_bytes = kDefaultMaximumTotalExpandedBytes;
  size_t maximum_file_count = kDefaultMaximumFileCount;
  size_t maximum_path_segments = kDefaultMaximumPathSegments;
};

/// Platform-neutral archive entry classes used by the pure type-policy decision.
enum class ArchiveEntryType {
  kRegularFile,
  kDirectory,
  kSymbolicLink,
  kDevice,
  kFifo,
  kSocket,
  kOther,
};

/// Running archive-entry budget usage while an archive is streamed.
struct ArchiveBudgetUsage {
  size_t entry_count = 0;
  uint64_t total_expanded_bytes = 0;
};

/// Admit one archive entry and return the incremented, overflow-safe budget usage.
[[nodiscard]] Expected<ArchiveBudgetUsage, StoreRejection> checkEntryBudget(
    const StoreQuotas& quotas, ArchiveBudgetUsage current, uint64_t entry_expanded_bytes);

/// Validate and normalize one archive-relative path using portable filesystem rules.
///
/// Both slash spellings, absolute/drive paths, dot segments, Windows device aliases,
/// trailing dot/space aliases, control bytes, and paths deeper than the configured
/// segment cap fail closed before any filesystem operation.
[[nodiscard]] Expected<std::string, StoreRejection> validateArchivePath(std::string_view path, size_t maximum_segments);

/// Accept only regular files and directories with no hardlink target.
///
/// Symlinks, hardlinks, devices, fifos, sockets, and unknown types are rejected;
/// none are materialized as regular files as a fallback.
[[nodiscard]] Expected<void, StoreRejection> validateArchiveEntryType(ArchiveEntryType type, bool has_hardlink_target);

}  // namespace PJ
