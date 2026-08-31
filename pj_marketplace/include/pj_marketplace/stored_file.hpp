#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <string>

namespace PJ {

/// One exact regular file observed during extraction or committed in an object.
struct StoredFile {
  std::string relative_path;
  std::string sha256;
  uint64_t size = 0;

  [[nodiscard]] bool operator==(const StoredFile&) const noexcept = default;
};

}  // namespace PJ
