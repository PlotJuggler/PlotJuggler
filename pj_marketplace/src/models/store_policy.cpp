// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/store_policy.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace PJ {
namespace {

bool isWindowsDeviceName(std::string_view segment) {
  const size_t extension = segment.find('.');
  std::string base(segment.substr(0, extension));
  std::ranges::transform(
      base, base.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  if (base == "con" || base == "prn" || base == "aux" || base == "nul") {
    return true;
  }
  if (base.size() == 4U && (base.starts_with("com") || base.starts_with("lpt"))) {
    return base[3] >= '1' && base[3] <= '9';
  }
  return false;
}

}  // namespace

Expected<std::string, StoreRejection> validateArchivePath(std::string_view path, size_t maximum_segments) {
  if (path.empty()) {
    return unexpected(storeRejection(StoreRejectionCode::kUnsafePath, "archive entry path is empty"));
  }
  if (path.front() == '/' || path.front() == '\\' ||
      (path.size() >= 2U && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':')) {
    return unexpected(storeRejection(StoreRejectionCode::kUnsafePath, "archive entry path is absolute"));
  }
  if (path.find('\\') != std::string_view::npos) {
    return unexpected(
        storeRejection(StoreRejectionCode::kUnsafePath, "archive entry path uses a Windows separator or escape"));
  }

  while (!path.empty() && path.back() == '/') {
    path.remove_suffix(1U);
  }
  if (path.empty()) {
    return unexpected(storeRejection(StoreRejectionCode::kUnsafePath, "archive entry path names the root"));
  }

  std::vector<std::string_view> segments;
  size_t start = 0;
  while (start <= path.size()) {
    const size_t separator = path.find('/', start);
    const size_t end = separator == std::string_view::npos ? path.size() : separator;
    const std::string_view segment = path.substr(start, end - start);
    if (segment.empty() || segment == "." || segment == "..") {
      return unexpected(storeRejection(StoreRejectionCode::kUnsafePath, "archive entry path contains a dot segment"));
    }
    if (segment.back() == '.' || segment.back() == ' ' || isWindowsDeviceName(segment)) {
      return unexpected(storeRejection(StoreRejectionCode::kUnsafePath, "archive entry path has a Windows alias"));
    }
    for (unsigned char character : segment) {
      if (character < 0x20U || character == 0x7fU || character == ':') {
        return unexpected(
            storeRejection(StoreRejectionCode::kUnsafePath, "archive entry path contains a forbidden character"));
      }
    }
    segments.push_back(segment);
    if (segments.size() > maximum_segments) {
      return unexpected(storeRejection(StoreRejectionCode::kLimitExceeded, "archive entry path exceeds depth quota"));
    }
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1U;
  }
  return std::string(path);
}

Expected<void, StoreRejection> validateArchiveEntryType(ArchiveEntryType type, bool has_hardlink_target) {
  if (has_hardlink_target) {
    return unexpected(storeRejection(StoreRejectionCode::kUnsupportedEntryType, "archive hardlinks are not allowed"));
  }
  if (type != ArchiveEntryType::kRegularFile && type != ArchiveEntryType::kDirectory) {
    return unexpected(storeRejection(
        StoreRejectionCode::kUnsupportedEntryType,
        "archive entries must be regular files or directories; links and special files are not allowed"));
  }
  return {};
}

Expected<ArchiveBudgetUsage, StoreRejection> checkEntryBudget(
    const StoreQuotas& quotas, ArchiveBudgetUsage current, uint64_t entry_expanded_bytes) {
  if (current.entry_count >= quotas.maximum_file_count) {
    return unexpected(storeRejection(StoreRejectionCode::kLimitExceeded, "archive entry count exceeds quota"));
  }
  if (entry_expanded_bytes > quotas.maximum_entry_expanded_bytes) {
    return unexpected(storeRejection(StoreRejectionCode::kLimitExceeded, "archive entry expanded size exceeds quota"));
  }
  if (current.total_expanded_bytes > quotas.maximum_total_expanded_bytes ||
      entry_expanded_bytes > quotas.maximum_total_expanded_bytes - current.total_expanded_bytes) {
    return unexpected(storeRejection(StoreRejectionCode::kLimitExceeded, "archive total expanded size exceeds quota"));
  }
  ++current.entry_count;
  current.total_expanded_bytes += entry_expanded_bytes;
  return current;
}

}  // namespace PJ
