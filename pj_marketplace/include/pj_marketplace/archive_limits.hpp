#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file archive_limits.hpp
 * @brief Size, count, depth and entry-type policy applied while unpacking an extension archive.
 *
 * Each ZIP entry states a name, a destination and a size, so extracting one executes write instructions that came
 * from the file. The path guard in DownloadManager bounds where an entry may write; these limits bound how much it
 * may write and what kind of entry is admitted. Qt-free and side-effect-free: every function decides from its
 * arguments alone, so a limit is checked before a file is opened.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace PJ {

/// Compressed artifact cap, bounding what a transfer may deliver (checked once the whole download is in memory).
inline constexpr uint64_t kDefaultMaximumCompressedBytes = 128U * 1024U * 1024U;
/// Single-entry expanded cap, rejecting one-entry decompression bombs.
inline constexpr uint64_t kDefaultMaximumEntryExpandedBytes = 256U * 1024U * 1024U;
/// Aggregate expanded cap, bounding one extension's resident bytes.
inline constexpr uint64_t kDefaultMaximumTotalExpandedBytes = 512U * 1024U * 1024U;
/// Entry cap, bounding both file and directory metadata amplification.
inline constexpr size_t kDefaultMaximumEntryCount = 4096U;
/// Path-depth cap, bounding nested-directory and path-processing work.
inline constexpr size_t kDefaultMaximumPathSegments = 16U;

/// Budgets enforced while an archive is unpacked. Passed in rather than read from the constants so a test can drive a
/// breach with a few kilobytes.
struct ArchiveLimits {
  uint64_t maximum_compressed_bytes = kDefaultMaximumCompressedBytes;
  uint64_t maximum_entry_expanded_bytes = kDefaultMaximumEntryExpandedBytes;
  uint64_t maximum_total_expanded_bytes = kDefaultMaximumTotalExpandedBytes;
  size_t maximum_entry_count = kDefaultMaximumEntryCount;
  size_t maximum_path_segments = kDefaultMaximumPathSegments;
};

/// Running budget consumed so far by the entries already admitted.
struct ArchiveBudget {
  size_t entry_count = 0;
  uint64_t total_expanded_bytes = 0;
};

/// Archive entry classes, kept independent of libarchive's constants so this
/// header stays free of its headers.
enum class ArchiveEntryKind {
  kRegularFile,
  kDirectory,
  kOther,  ///< symlink, hardlink, device, fifo, socket, or anything unrecognized
};

/// Admits one entry against the budget and folds it in; `budget` changes only when the entry is admitted. What is
/// folded in is the size the caller passes: for an entry that declares one, the declaration, which equals the bytes
/// written only because the reader refuses an entry that does not deliver exactly what it declared. A caller reading a
/// format without that guarantee must reconcile once the entry closes, or the budget describes what was promised
/// instead of what landed. Returns the reason when the entry does not fit.
[[nodiscard]] std::optional<std::string> admitEntry(
    const ArchiveLimits& limits, ArchiveBudget& budget, uint64_t entry_expanded_bytes);

/// Accepts only regular files and directories; anything else is refused rather than materialized as a regular file.
[[nodiscard]] std::optional<std::string> admitEntryKind(ArchiveEntryKind kind);

/// Rejects an archive-relative path no extension has a reason to carry: absolute paths, drive letters, backslash
/// separators, `.`/`..` segments, control bytes, Windows device aliases, segments ending in a dot or space, and paths
/// deeper than the cap, before the path reaches the filesystem. The traversal guard in DownloadManager stays as the
/// second line of defence; this one refuses shapes rather than resolved locations, so it also rejects names that are
/// harmless on Linux and mean something else on Windows.
[[nodiscard]] std::optional<std::string> admitEntryPath(std::string_view path, size_t maximum_segments);

}  // namespace PJ
