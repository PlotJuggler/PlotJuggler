#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace PJ {

// Download artifact for one platform in the registry.
struct Platform {
  QString url;
  QString checksum;  ///< Format: "sha256:<hex>"
};

// Registry-declared plugin entry kept for backward-compatible metadata display.
struct ExtensionPlugin {
  QString name;     ///< Plugin class name
  QString type;     ///< "data_loader" | "data_streamer" | "parser" | "toolbox"
  QString library;  ///< Library filename without extension
};

// Extension record as received from the marketplace registry.
struct Extension {
  QString id;
  QString name;
  QString description;
  QString author;
  QString publisher;
  QString website;
  QString repository;
  QString license;   ///< SPDX identifier
  QString icon_url;  ///< Optional

  /// "data_loader" | "data_streamer" | "parser" | "toolbox" | "bundle"
  QString category;
  QStringList tags;

  QString version;
  QString min_sdk_required;
  QString min_plotjuggler_version;  ///< Deprecated; read for back-compat, never gates.
  /// Full-feature floor: the SDK release at which every capability this
  /// plugin uses is available. Informational only (drives the reduced-features
  /// note) — never an admission or install criterion. Empty = full features at
  /// min_sdk_required.
  QString suggested_sdk_version;

  QList<ExtensionPlugin> plugins;
  QMap<QString, Platform> platforms;  ///< Keyed by "linux-x86_64", "windows-x86_64", etc.
  QMap<QString, QString> changelog;   ///< version -> description
};

}  // namespace PJ
