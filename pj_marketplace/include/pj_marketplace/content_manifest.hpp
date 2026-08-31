#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file content_manifest.hpp
 * @brief Portable content-manifest identity shared by runtime storage and release tooling.
 */

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_marketplace/stored_file.hpp"

namespace PJ {

enum class ContentManifestRejectionCode {
  kUnsafeTree,
  kIoFailure,
};

struct ContentManifestRejection {
  ContentManifestRejectionCode code;
  std::string message;
};

struct ContentManifestOptions {
  bool hash_contents = true;
  std::string_view skipped_relative_path;
};

/// Hash and deterministically sort every regular file below a directory.
///
/// Symlinks and special entries fail closed. Paths use forward slashes on every
/// platform so build-time and runtime identities are byte-identical.
[[nodiscard]] Expected<std::vector<StoredFile>, ContentManifestRejection> inspectContentManifest(
    const std::filesystem::path& root, ContentManifestOptions options = {});

/// Aggregate sorted `path\0sha256:<hex>\n` records into the artifact identity.
[[nodiscard]] std::string aggregateContentManifestDigest(const std::vector<StoredFile>& files);

}  // namespace PJ
