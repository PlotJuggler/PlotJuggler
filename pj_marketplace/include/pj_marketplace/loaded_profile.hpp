#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file loaded_profile.hpp
 * @brief Persisted id -> content-addressed-object map for managed extensions.
 */

#include <QMap>
#include <QString>

namespace PJ {

/// Filename of the loaded-extensions profile under the store's profiles root: per installed extension id, the digest
/// of the store object that backs it, so the runtime can load managed plugins from the content-addressed store instead
/// of extensions/<id>/.
inline constexpr char kLoadedProfileFileName[] = "loaded.json";

/// Reads the id -> artifact-digest map from `profiles_root`/loaded.json. Returns an empty map when the file is absent
/// or malformed and never partially applies a corrupt file, so a damaged profile degrades to "no store-backed loads"
/// (the extensions/<id>/ scan still loads every plugin) rather than to a wrong load.
[[nodiscard]] QMap<QString, QString> readLoadedProfile(const QString& profiles_root);

/// Atomically writes the id -> artifact-digest map to `profiles_root`/loaded.json (QSaveFile: a crash mid-write
/// leaves the previous profile). The caller must hold the store writer lease. Returns false on any I/O failure; that
/// is non-fatal: the objects are still present, only the runtime's shortcut to them is not recorded.
bool writeLoadedProfile(const QString& profiles_root, const QMap<QString, QString>& id_to_digest);

}  // namespace PJ
