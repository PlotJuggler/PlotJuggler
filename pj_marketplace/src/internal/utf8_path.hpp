// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace PJ::marketplace_internal {

/// Encodes a filesystem path as a slash-separated UTF-8 string.
inline std::string genericUtf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

/// Decodes a UTF-8 path without routing through the platform locale.
inline std::filesystem::path pathFromUtf8(std::string_view value) {
  const auto* first = reinterpret_cast<const char8_t*>(value.data());
  return std::filesystem::path(std::u8string(first, first + value.size()));
}

}  // namespace PJ::marketplace_internal
