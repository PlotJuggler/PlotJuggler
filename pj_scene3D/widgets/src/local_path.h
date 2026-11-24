// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QDir>
#include <QString>
#include <QUrl>

namespace pj::scene3d {

// Whether opening `path` reaches another host: "//host/share/..." or "\\host\share\..." (either separator; on
// Windows SMB, which sends the user's NTLM credentials to `host`), including the long form "\\?\UNC\host\...", and,
// with `macos_automount`, a path the macOS automounter serves from a host ("/net/<host>/...", once cleaned). A long
// path to a drive ("\\?\C:\...", "//?/C:/...") is local. A path of this form that model content supplies is never
// opened; the platform is a parameter so every platform's spellings are testable anywhere.
[[nodiscard]] inline bool isNetworkSharePath(const QString& path, bool macos_automount) {
  const auto separator = [&path](qsizetype index) {
    return path.size() > index && (path[index] == u'/' || path[index] == u'\\');
  };
  if (separator(0) && separator(1)) {
    const bool long_drive_path =
        path.size() > 5 && path[2] == u'?' && separator(3) && path[4].isLetter() && path[5] == u':';
    return !long_drive_path;
  }
  if (macos_automount) {
    const QString cleaned = QDir::cleanPath(path);
    return cleaned.startsWith(QLatin1StringView("/net/")) && cleaned.size() > 5;
  }
  return false;
}

[[nodiscard]] inline bool isNetworkSharePath(const QString& path) {
#ifdef Q_OS_MACOS
  return isNetworkSharePath(path, /*macos_automount=*/true);
#else
  return isNetworkSharePath(path, /*macos_automount=*/false);
#endif
}

// The path a file:// URL names, on a share too ("file://host/s/x" -> "//host/s/x"; a "localhost" authority dropped).
// Only for a URL the user chose; see localFileOf() for one that model content supplies.
[[nodiscard]] inline QString filePathOf(const QUrl& url) {
  const bool localhost = url.host().compare(QLatin1StringView("localhost"), Qt::CaseInsensitive) == 0;
  return (localhost ? url.adjusted(QUrl::RemoveAuthority) : url).toLocalFile();
}

// A file:// URL naming another host or a network share path, or a scheme-less network share path.
[[nodiscard]] inline bool namesNetworkShare(const QUrl& url) {
  if (url.isLocalFile()) {
    return isNetworkSharePath(filePathOf(url));
  }
  return url.scheme().isEmpty() && isNetworkSharePath(url.toString());
}

// The local path a file:// URL names; empty for any other URL and for one naming a network share.
[[nodiscard]] inline QString localFileOf(const QUrl& url) {
  return url.isLocalFile() && !namesNetworkShare(url) ? filePathOf(url) : QString{};
}

}  // namespace pj::scene3d
