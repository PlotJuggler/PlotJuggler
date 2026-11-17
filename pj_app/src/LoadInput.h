// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QFileInfo>
#include <QString>
#include <memory>

namespace PJ {

// Keeps a browser upload's MEMFS file alive for deferred object fetches; empty for native paths.
using StorageLease = std::shared_ptr<void>;

struct LoadInput {
  QString display_name;     // UI label; its extension picks the plugin
  QString backing_path;     // what the plugin opens
  QString source_identity;  // reload/dedup identity; a pj-upload:// one is never shown as a path
  // Lower-case SHA-256 of browser-selected bytes, empty for native paths. Only source-bound layouts use it, to tell
  // same-basename sources apart; plugins never see it.
  QString content_sha256;
  StorageLease lease;

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
