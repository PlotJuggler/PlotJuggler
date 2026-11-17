#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QStringList>

namespace PJ {

// Collapses a `major.minor.patch` version string to a single comparable integer, tolerating the shapes in GitHub
// release tags:
//   - an optional leading `v`/`V`   ("v3.999.0")
//   - a pre-release/build suffix     ("3.999.0-rc1", "4.0.0+build.7")
//   - a missing minor/patch          ("4", "4.1" -> padded with zeros)
//
// Returns -1 for anything it cannot parse as at least one numeric component (empty, "dev", "latest", a non-numeric
// segment), so a malformed tag or local version is never mistaken for "newer".
//
// The radix (minor x1000, major x1'000'000) leaves room for a 3-digit minor such as 3.999.0. A minor/patch >= 1000
// would overflow into the next field, so it is rejected (-1) rather than silently mis-ordered.
//
// Bespoke rather than Qt's QVersionNumber (which pj_marketplace uses): that type does not strip a leading `v`, and the
// "unparseable -> never treated as newer" policy of isNewerVersion() is not something QVersionNumber gives.
inline int versionToComparable(const QString& version) {
  QString v = version.trimmed();
  if (v.isEmpty()) {
    return -1;
  }
  if (v.startsWith(QLatin1Char('v')) || v.startsWith(QLatin1Char('V'))) {
    v = v.mid(1);
  }
  // Drop any semver pre-release / build-metadata suffix.
  const qsizetype dash = v.indexOf(QLatin1Char('-'));
  const qsizetype plus = v.indexOf(QLatin1Char('+'));
  qsizetype cut = -1;
  if (dash >= 0) {
    cut = dash;
  }
  if (plus >= 0 && (cut < 0 || plus < cut)) {
    cut = plus;
  }
  if (cut >= 0) {
    v = v.left(cut);
  }

  const QStringList parts = v.split(QLatin1Char('.'));
  int components[3] = {0, 0, 0};
  for (int i = 0; i < 3 && i < parts.size(); ++i) {
    bool ok = false;
    const int n = parts[i].toInt(&ok);
    if (!ok || n < 0) {
      return -1;
    }
    // minor (i==1) and patch (i==2) share a 1000-wide field; a value that would
    // bleed into the next component is treated as unparseable, not mis-ordered.
    if (i > 0 && n >= 1000) {
      return -1;
    }
    components[i] = n;
  }
  return components[0] * 1'000'000 + components[1] * 1'000 + components[2];
}

// True only when `candidate` is a strictly newer, parseable version than
// `current` — both must parse, so an unreadable tag on either side yields false
// (we never nag on garbage).
inline bool isNewerVersion(const QString& candidate, const QString& current) {
  const int candidate_number = versionToComparable(candidate);
  const int current_number = versionToComparable(current);
  if (candidate_number < 0 || current_number < 0) {
    return false;
  }
  return candidate_number > current_number;
}

}  // namespace PJ
