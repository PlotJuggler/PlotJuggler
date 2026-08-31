// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/archive_limits.hpp"

#include <algorithm>
#include <array>

namespace PJ {

namespace {

/// DOS device names are resolved by the Windows loader wherever they appear as a
/// path segment, with or without an extension, so `aux.so` is as unusable as `aux`.
bool isWindowsDeviceName(std::string_view segment) {
  static constexpr std::array<std::string_view, 22> kDeviceNames = {
      "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
      "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

  const size_t dot = segment.find('.');
  const std::string_view stem = dot == std::string_view::npos ? segment : segment.substr(0, dot);

  std::string upper;
  upper.reserve(stem.size());
  for (const char c : stem) {
    upper.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c));
  }
  return std::find(kDeviceNames.begin(), kDeviceNames.end(), upper) != kDeviceNames.end();
}

}  // namespace

std::optional<std::string> admitEntry(
    const ArchiveLimits& limits, ArchiveBudget& budget, const uint64_t entry_expanded_bytes) {
  if (budget.entry_count >= limits.maximum_entry_count) {
    return "archive holds more than " + std::to_string(limits.maximum_entry_count) + " entries";
  }
  if (entry_expanded_bytes > limits.maximum_entry_expanded_bytes) {
    return "archive entry expands to more than " + std::to_string(limits.maximum_entry_expanded_bytes) + " bytes";
  }
  // Subtract instead of adding: the sum could wrap on a hostile declared size.
  if (entry_expanded_bytes > limits.maximum_total_expanded_bytes - budget.total_expanded_bytes) {
    return "archive expands to more than " + std::to_string(limits.maximum_total_expanded_bytes) + " bytes in total";
  }

  budget.entry_count += 1;
  budget.total_expanded_bytes += entry_expanded_bytes;
  return std::nullopt;
}

std::optional<std::string> admitEntryKind(const ArchiveEntryKind kind) {
  switch (kind) {
    case ArchiveEntryKind::kRegularFile:
    case ArchiveEntryKind::kDirectory:
      return std::nullopt;
    case ArchiveEntryKind::kOther:
      break;
  }
  return "archive entry is neither a regular file nor a directory";
}

std::optional<std::string> admitEntryPath(const std::string_view path, const size_t maximum_segments) {
  if (path.empty()) {
    return "archive entry has an empty path";
  }
  // A backslash separates directories on Windows and is an ordinary filename
  // character everywhere else, so one archive lays out two different shapes: on
  // Linux "sub\\plugin.so" is a single file whose name holds the character, on
  // Windows it is "plugin.so" inside "sub". Refused rather than normalized —
  // guessing which one the author meant is how a package ends up installing
  // correctly on one platform and subtly wrong on the other.
  if (path.find('\\') != std::string_view::npos) {
    return "archive entry path uses a backslash separator";
  }
  if (path.front() == '/') {
    return "archive entry path is absolute";
  }
  // A drive-relative spelling ("C:x") is as unusable as an absolute one, and on a
  // POSIX host it would silently become a file with a colon in its name.
  if (path.size() >= 2 && path[1] == ':') {
    return "archive entry path carries a drive letter";
  }

  size_t segments = 0;
  size_t start = 0;
  while (start <= path.size()) {
    const size_t end = std::min(path.find('/', start), path.size());
    const std::string_view segment = path.substr(start, end - start);

    // A trailing separator closes the last segment; an empty one anywhere else is
    // a doubled separator, which normalizes differently across platforms.
    if (segment.empty()) {
      if (end == path.size()) {
        break;
      }
      return "archive entry path has an empty segment";
    }
    if (segment == "." || segment == "..") {
      return "archive entry path has a dot segment";
    }
    if (segment.back() == '.' || segment.back() == ' ') {
      return "archive entry path segment ends in a dot or space";
    }
    if (isWindowsDeviceName(segment)) {
      return "archive entry path uses a reserved device name";
    }
    for (const char c : segment) {
      if (static_cast<unsigned char>(c) < 0x20) {
        return "archive entry path holds a control byte";
      }
    }

    segments += 1;
    if (segments > maximum_segments) {
      return "archive entry path is deeper than " + std::to_string(maximum_segments) + " segments";
    }
    if (end == path.size()) {
      break;
    }
    start = end + 1;
  }

  return std::nullopt;
}

}  // namespace PJ
