#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDateTime>
#include <QString>
#include <cstdint>

namespace PJ {

// Installed extension discovered from an embedded plugin manifest on disk.
//
// The name/description/category trio mirrors the same fields on Extension, but
// is sourced from the manifest rather than the registry. It exists so an
// extension NO registry lists can still be rendered as an ordinary row: without
// it, the manifest metadata read during discovery would be discarded and the
// UI would have nothing to display for a sideloaded plugin.
struct InstalledExtension {
  QString id;  ///< Matches Extension::id from the registry
  QString version;
  QDateTime install_date;
  QString path;  ///< Absolute path to <config-root>/extensions/<id>/
  bool enabled = true;

  QString name;         ///< Manifest display name; falls back to id when the manifest omits it
  QString description;  ///< Manifest description; may be empty
  /// Manifest category, expected to use the same vocabulary the registry ships
  /// ("data_loader" | "data_stream" | "message_parser" | "toolbox") since that is
  /// what the filter row matches against. A plugin declaring anything else simply
  /// matches no category facet.
  QString category;
  /// ABI major read from the DSO protocol. Zero means an older descriptor did
  /// not expose it, in which case the loader's ABI-symbol check remains the
  /// authoritative gate.
  uint32_t abi_major = 0;
  /// Manifest `min_sdk_required`: the oldest SDK contract whose API the plugin
  /// requires. Empty means undeclared. Unlike the application release floor,
  /// this is a hard host-side load gate defined by the SDK.
  QString min_sdk_required;
  /// Manifest `min_plotjuggler_version`: the oldest host build this plugin declares
  /// it can run on. Empty means the manifest declared none, which imposes no floor —
  /// so empty and "0" are NOT interchangeable. Mirrors Extension::min_plotjuggler_version,
  /// but sourced from the DSO rather than the registry, which is what lets a sideload
  /// be gated on the same rule as a registry install (see ExtensionManager::hostCompatibility).
  QString min_plotjuggler_version;
};

}  // namespace PJ
