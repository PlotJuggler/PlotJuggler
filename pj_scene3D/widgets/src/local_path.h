// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <QUrl>

namespace pj::scene3d {

// "//host/share/..." or "\\host\share\..." (either separator): on Windows, opening it connects to `host` over SMB and
// sends it the user's NTLM credentials. A path of this form that model content supplies is never opened.
[[nodiscard]] inline bool isNetworkSharePath(const QString& path) {
  const auto separator = [&path](qsizetype index) {
    return path.size() > index && (path[index] == u'/' || path[index] == u'\\');
  };
  return separator(0) && separator(1);
}

// The local path a file:// URL names (a "localhost" authority dropped); empty for any other URL, a file:// URL naming
// another host, or a network share path.
[[nodiscard]] inline QString localFileOf(const QUrl& url) {
  if (!url.isLocalFile()) {
    return {};
  }
  const bool localhost = url.host().compare(QLatin1StringView("localhost"), Qt::CaseInsensitive) == 0;
  const QString path = (localhost ? url.adjusted(QUrl::RemoveAuthority) : url).toLocalFile();
  return isNetworkSharePath(path) ? QString{} : path;
}

}  // namespace pj::scene3d
