// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QDir>
#include <QString>
#include <QUrl>

namespace pj::scene3d {

// Whether opening `path` reaches another host: "//host/share/..." or "\\host\share\..." (either separator; on
// Windows SMB, which sends the user's NTLM credentials to `host`), the long forms "\\?\UNC\host\..." and
// "\??\..." (the NT namespace, which reaches shares too), and, with `macos_automount`, a path the macOS automounter
// serves from a host ("/net/<host>/...", once cleaned, in any case). A long path to a drive ("\\?\C:\...",
// "//?/C:/...") is local unless a ".." segment follows, which Windows resolves after the prefix. The platform is a
// parameter so every platform's spellings are testable anywhere.
[[nodiscard]] inline bool isNetworkSharePath(const QString& path, bool macos_automount) {
  const auto separator = [&path](qsizetype index) {
    return path.size() > index && (path[index] == u'/' || path[index] == u'\\');
  };
  if (separator(0) && path.size() > 3 && path[1] == u'?' && path[2] == u'?' && separator(3)) {
    return true;
  }
  if (separator(0) && separator(1)) {
    const auto climbs = [&path]() {
      QString rest = path.mid(6);
      return rest.replace(u'\\', u'/').split(u'/').contains(QLatin1StringView(".."));
    };
    const bool long_drive_path =
        path.size() > 5 && path[2] == u'?' && separator(3) && path[4].isLetter() && path[5] == u':' && !climbs();
    return !long_drive_path;
  }
  if (macos_automount) {
    const QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(path));
    return cleaned.size() > 5 && cleaned.startsWith(QLatin1StringView("/net/"), Qt::CaseInsensitive);
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

// `rel` (text model content supplies) joined onto `folder` and cleaned: the path to open. Empty when `rel` names a
// network share itself, or when the joined path is one and `folder` is not ("../../net/<host>/t.png" on macOS).
[[nodiscard]] inline QString joinContentPath(const QString& folder, const QString& rel, bool macos_automount) {
  // The raw text: on Windows QDir::fromNativeSeparators drops a "\\?\" or "//?/" prefix.
  if (isNetworkSharePath(rel, macos_automount)) {
    return {};
  }
  const QString tail = QDir::fromNativeSeparators(rel);
  const QString joined = QDir::cleanPath(QDir(folder).absoluteFilePath(tail));
  return isNetworkSharePath(joined, macos_automount) && !isNetworkSharePath(folder, macos_automount) ? QString{}
                                                                                                     : joined;
}

[[nodiscard]] inline QString joinContentPath(const QString& folder, const QString& rel) {
#ifdef Q_OS_MACOS
  return joinContentPath(folder, rel, /*macos_automount=*/true);
#else
  return joinContentPath(folder, rel, /*macos_automount=*/false);
#endif
}

// Whether the file import of `model_path` may open `file` (a path the model's content leads assimp to): any path but
// a network share, and a share path only inside the folder of a model that is itself on a share (the user chose that
// share; either separator counts). QDir::cleanPath folds a leading "//" into "/" off Windows, so the share test reads
// each path as given.
[[nodiscard]] inline bool importMayOpen(const QString& model_path, const QString& file, bool macos_automount) {
  const QString path = QString(file).replace(u'\\', u'/');
  if (!isNetworkSharePath(path, macos_automount)) {
    return true;
  }
  const QString model = QString(model_path).replace(u'\\', u'/');
  if (!isNetworkSharePath(model, macos_automount)) {
    return false;
  }
  const QString folder = QDir::cleanPath(model).section(u'/', 0, -2) + u'/';
  return QDir::cleanPath(path).startsWith(folder);
}

[[nodiscard]] inline bool importMayOpen(const QString& model_path, const QString& file) {
#ifdef Q_OS_MACOS
  return importMayOpen(model_path, file, /*macos_automount=*/true);
#else
  return importMayOpen(model_path, file, /*macos_automount=*/false);
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
