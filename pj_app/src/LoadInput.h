// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QFileInfo>
#include <QString>
#include <memory>

namespace PJ {

// Pins any non-native backing storage used by a load. Desktop paths leave this
// empty; browser uploads use it to keep their MEMFS file alive for deferred
// object fetchers.
using StorageLease = std::shared_ptr<void>;

struct LoadInput {
  QString display_name;     // UI label and extension-based plugin matching.
  QString backing_path;     // Path passed to the DataSource plugin.
  QString source_identity;  // Reload/dedup identity; never shown as a native path.
  // Lower-case SHA-256 of browser-selected bytes. Empty for native paths. The
  // browser source-layout path uses it only to distinguish otherwise
  // indistinguishable same-basename sources; plugins never receive it.
  QString content_sha256;
  StorageLease lease;  // Empty for native files.

  [[nodiscard]] static LoadInput fromNativePath(const QString& path) {
    return LoadInput{
        .display_name = QFileInfo(path).fileName(),
        .backing_path = path,
        .source_identity = path,
        .content_sha256 = {},
        .lease = {},
    };
  }
};

inline constexpr auto kBrowserUploadScheme = "pj-upload://";

[[nodiscard]] inline bool isBrowserUploadIdentity(const QString& identity) {
  return identity.startsWith(QLatin1StringView(kBrowserUploadScheme));
}

// `pj-stream://<record id>`: the source identity a layout binds to one of its stream records.
inline constexpr auto kStreamIdentityScheme = "pj-stream://";

[[nodiscard]] inline bool isStreamIdentity(const QString& identity) {
  return identity.startsWith(QLatin1StringView(kStreamIdentityScheme));
}

}  // namespace PJ
