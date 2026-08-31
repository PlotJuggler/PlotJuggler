#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file fs_walk.hpp
 * @brief Non-throwing traversal shared by marketplace admission checks.
 */

#include <filesystem>
#include <functional>
#include <string>

#include "pj_base/expected.hpp"

namespace PJ {

/// Invoke @p sink for every regular file recursively found below @p root.
///
/// Symlinks and special entries fail closed instead of being followed or skipped.
/// Filesystem errors are returned as strings instead of throwing. Traversal
/// order is filesystem-defined; callers that need determinism must sort their
/// collected paths.
template <typename Sink>
Expected<void> forEachRegularFile(const std::filesystem::path& root, Sink&& sink) {
  std::error_code error;
  const bool is_directory = std::filesystem::is_directory(root, error);
  if (error) {
    return unexpected("cannot inspect directory " + root.string() + ": " + error.message());
  }
  if (!is_directory) {
    return unexpected("path is not a directory: " + root.string());
  }

  std::filesystem::recursive_directory_iterator iterator(root, error);
  const std::filesystem::recursive_directory_iterator end;
  if (error) {
    return unexpected("cannot iterate directory " + root.string() + ": " + error.message());
  }
  while (iterator != end) {
    const std::filesystem::directory_entry entry = *iterator;
    const std::filesystem::file_status status = entry.symlink_status(error);
    if (error) {
      return unexpected("cannot inspect path " + entry.path().string() + ": " + error.message());
    }
    if (std::filesystem::is_symlink(status)) {
      return unexpected("filesystem tree contains a symbolic link: " + entry.path().string());
    }
    if (std::filesystem::is_regular_file(status)) {
      std::invoke(sink, entry.path());
    } else if (!std::filesystem::is_directory(status)) {
      return unexpected("filesystem tree contains a special entry: " + entry.path().string());
    }
    iterator.increment(error);
    if (error) {
      return unexpected("cannot iterate directory " + root.string() + ": " + error.message());
    }
  }
  return {};
}

}  // namespace PJ
