#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <string_view>

namespace PJ {

/// Text equality that reads "\r\n" as "\n": a script or filter source read from
/// a Windows checkout carries CRLF, while its layout-snapshot copy went through
/// XML end-of-line normalization and carries LF. Neither is the identity of the
/// processor. A '\r' not followed by '\n' is data (a long string can hold one)
/// and stays significant. Compares in place — the stored text stays byte-identical.
inline bool sameTextIgnoringLineEndings(std::string_view lhs, std::string_view rhs) {
  const auto skip_crlf_prefix = [](std::string_view text, std::size_t pos) {
    return pos + 1 < text.size() && text[pos] == '\r' && text[pos + 1] == '\n' ? pos + 1 : pos;
  };
  std::size_t left = 0;
  std::size_t right = 0;
  while (left < lhs.size() && right < rhs.size()) {
    if (lhs[left] != rhs[right]) {
      left = skip_crlf_prefix(lhs, left);
      right = skip_crlf_prefix(rhs, right);
      if (lhs[left] != rhs[right]) {
        return false;
      }
    }
    ++left;
    ++right;
  }
  return left == lhs.size() && right == rhs.size();
}

}  // namespace PJ
