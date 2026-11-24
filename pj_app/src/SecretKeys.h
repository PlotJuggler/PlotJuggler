#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>

namespace PJ {

// A key name that suggests a credential (password, token, API key...), ignoring case, '_' and '-'. MCP redacts such a
// key's value; BrowserPersistence never makes one durable.
[[nodiscard]] inline bool isSecretKey(std::string_view key) {
  std::string folded;
  for (const unsigned char c : key) {
    if (c != '_' && c != '-') {
      folded.push_back(static_cast<char>(std::tolower(c)));
    }
  }
  static constexpr std::string_view kWords[] = {"password", "passwd",     "secret",    "token",
                                                "apikey",   "privatekey", "credential"};
  return std::any_of(std::begin(kWords), std::end(kWords), [&folded](std::string_view word) {
    return folded.find(word) != std::string::npos;
  });
}

}  // namespace PJ
