// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Public because fs_walk.hpp and sha256.hpp, themselves public headers, build their error text
// with it, and because pj_runtime's plugin catalog (which already links this library) needs the
// same conversions for its scan roots and diagnostics rather than a second copy.
namespace PJ::marketplace_internal {

/// Encodes a filesystem path as UTF-8 with the platform's separators, for messages and logs.
/// Use this instead of path::string(): on Windows that narrows through the active code page
/// and throws std::system_error for a name the page cannot represent.
inline std::string pathToUtf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

/// Encodes a filesystem path as a slash-separated UTF-8 string.
inline std::string genericUtf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

/// Decodes a UTF-8 path without routing through the platform locale (std::filesystem::path's
/// std::string constructor reads the active code page on Windows).
inline std::filesystem::path pathFromUtf8(std::string_view value) {
  const auto* first = reinterpret_cast<const char8_t*>(value.data());
  return std::filesystem::path(std::u8string(first, first + value.size()));
}

}  // namespace PJ::marketplace_internal
