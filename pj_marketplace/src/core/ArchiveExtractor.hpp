#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file ArchiveExtractor.hpp
 * @brief Streaming libarchive extraction behind the immutable-store policy.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_marketplace/store_policy.hpp"
#include "pj_marketplace/stored_file.hpp"

namespace PJ {

/// Complete validated result of streaming one archive into a private directory.
struct ArchiveExtraction {
  std::vector<StoredFile> files;
  uint64_t total_expanded_bytes = 0;
};

/// Test interposition for simulating write failures such as a full volume.
using ExtractionWriteHook = std::function<bool(const std::filesystem::path&, const char*, size_t)>;

/// Streams ZIP entries from a bounded file and applies every quota before publication.
class ArchiveExtractor {
 public:
  /// Extract into an operation-private directory, hashing each file as it is written.
  [[nodiscard]] Expected<ArchiveExtraction, StoreRejection> extract(
      const std::filesystem::path& archive_path, const std::filesystem::path& destination, const StoreQuotas& quotas,
      const std::atomic<bool>* cancel_requested = nullptr, const ExtractionWriteHook& write_hook = {}) const;
};

}  // namespace PJ
