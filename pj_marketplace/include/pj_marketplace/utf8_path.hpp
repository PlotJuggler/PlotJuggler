// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Public because fs_walk.hpp and sha256.hpp, themselves public headers, build their error text
// with it, and because pj_runtime (which already links this library) needs the same conversions
// rather than a second copy. These helpers move to the SDK's pj_base/sdk/platform.hpp in its
// next release; switch to those then.
//
// A QString converts losslessly without them: std::filesystem::path(q.toStdU16String()) and
// QString::fromStdU16String(p.u16string()).
namespace PJ {

/// Encodes a filesystem path as UTF-8 with the platform's separators, for messages and logs.
/// Use this instead of path::string(): on Windows that narrows through the active code page
/// and throws std::system_error for a name the page cannot represent.
inline std::string pathToUtf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return {utf8.begin(), utf8.end()};
}

/// Encodes a filesystem path as a slash-separated UTF-8 string.
inline std::string genericUtf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.generic_u8string();
  return {utf8.begin(), utf8.end()};
}

/// Decodes a UTF-8 path without routing through the platform locale (std::filesystem::path's
/// std::string constructor reads the active code page on Windows).
inline std::filesystem::path pathFromUtf8(std::string_view value) {
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

}  // namespace PJ
