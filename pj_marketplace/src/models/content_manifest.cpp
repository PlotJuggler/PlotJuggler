// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/content_manifest.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "internal/utf8_path.hpp"
#include "pj_marketplace/fs_walk.hpp"
#include "pj_marketplace/sha256.hpp"

namespace PJ {
Expected<std::vector<StoredFile>, ContentManifestRejection> inspectContentManifest(
    const std::filesystem::path& root, ContentManifestOptions options) {
  std::vector<StoredFile> files;
  std::optional<ContentManifestRejection> failure;
  auto walked = forEachRegularFile(root, [&](const std::filesystem::path& path) {
    if (failure.has_value()) {
      return;
    }
    const std::string relative_path = marketplace_internal::genericUtf8(path.lexically_relative(root));
    if (!options.skipped_relative_path.empty() && relative_path == options.skipped_relative_path) {
      return;
    }
    std::error_code size_error;
    const uintmax_t file_size = std::filesystem::file_size(path, size_error);
    if (size_error || file_size > std::numeric_limits<uint64_t>::max()) {
      failure = ContentManifestRejection{
          ContentManifestRejectionCode::kIoFailure,
          "cannot inspect content-manifest file size: " + relative_path,
      };
      return;
    }
    std::string digest;
    if (options.hash_contents) {
      auto hashed = sha256File(path);
      if (!hashed) {
        failure = ContentManifestRejection{ContentManifestRejectionCode::kIoFailure, std::move(hashed.error())};
        return;
      }
      digest = std::move(*hashed);
    }
    files.push_back({relative_path, std::move(digest), static_cast<uint64_t>(file_size)});
  });
  if (!walked) {
    return unexpected(ContentManifestRejection{ContentManifestRejectionCode::kUnsafeTree, walked.error()});
  }
  if (failure.has_value()) {
    return unexpected(std::move(*failure));
  }
  std::ranges::sort(files, {}, &StoredFile::relative_path);
  return files;
}

std::string aggregateContentManifestDigest(const std::vector<StoredFile>& files) {
  Sha256 aggregate;
  for (const StoredFile& file : files) {
    aggregate.update(file.relative_path);
    aggregate.update(std::string_view("\0", 1));
    aggregate.update("sha256:" + file.sha256);
    aggregate.update("\n");
  }
  return aggregate.finishHex();
}

}  // namespace PJ
