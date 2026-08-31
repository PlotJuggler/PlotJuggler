#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file archive_limits.hpp
 * @brief Size, count, depth and entry-type policy applied while unpacking an extension archive.
 *
 * A ZIP is not just content: each entry states a name, a destination and a size,
 * so extracting one means executing write instructions that came from the file.
 * The path guard in DownloadManager bounds *where* an entry may write; these
 * limits bound *how much* it may write and *what kind* of entry is admitted.
 *
 * Qt-free and side-effect-free on purpose: every function decides from its
 * arguments alone, so the caller can check a limit before opening a file rather
 * than discovering the breach after the bytes are on disk.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace PJ {

/// Compressed artifact cap (128 MiB), bounding what a transfer may deliver.
inline constexpr uint64_t kDefaultMaximumCompressedBytes = 128U * 1024U * 1024U;
/// Single-entry expanded cap (256 MiB), rejecting one-entry decompression bombs.
inline constexpr uint64_t kDefaultMaximumEntryExpandedBytes = 256U * 1024U * 1024U;
/// Aggregate expanded cap (512 MiB), bounding one extension's resident bytes.
inline constexpr uint64_t kDefaultMaximumTotalExpandedBytes = 512U * 1024U * 1024U;
/// Entry cap, bounding both file and directory metadata amplification.
inline constexpr size_t kDefaultMaximumEntryCount = 4096U;
/// Path-depth cap, bounding nested-directory and path-processing work.
inline constexpr size_t kDefaultMaximumPathSegments = 16U;

/// Budgets enforced while an archive is unpacked.
///
/// Passed in rather than read from the constants so a test can drive a breach
/// with a few kilobytes instead of materializing a 256 MiB fixture.
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

/// Admit one entry against the budget and fold it in.
///
/// `budget` is updated only when the entry is admitted, so a rejected entry
/// leaves the counters untouched. What is folded in is the size the caller
/// passes, which for an entry that declares one is the declaration rather than a
/// count of bytes written — the two agree only because the reader refuses an
/// entry that does not deliver exactly what it declared, in either direction.
/// A caller reading a format without that guarantee has to reconcile the
/// difference itself once the entry closes, or the budget will describe what was
/// promised instead of what landed.
///
/// Returns the reason when the entry does not fit, `std::nullopt` when it does.
[[nodiscard]] std::optional<std::string> admitEntry(
    const ArchiveLimits& limits, ArchiveBudget& budget, uint64_t entry_expanded_bytes);

/// Accept only regular files and directories.
///
/// Anything else is refused rather than materialized as a regular file, which is
/// what an extractor that only special-cases directories ends up doing.
[[nodiscard]] std::optional<std::string> admitEntryKind(ArchiveEntryKind kind);

/// Reject an archive-relative path that no extension has any reason to carry.
///
/// Absolute paths, drive letters, backslash separators, `.`/`..` segments,
/// control bytes, Windows device aliases, segments ending in a dot or space, and
/// paths deeper than the cap all fail here — before the path reaches the
/// filesystem. The traversal guard in the caller stays as the second line of
/// defence; this one refuses shapes rather than resolved locations, so it also
/// rejects names that are harmless on Linux and mean something else on Windows.
[[nodiscard]] std::optional<std::string> admitEntryPath(std::string_view path, size_t maximum_segments);

}  // namespace PJ
