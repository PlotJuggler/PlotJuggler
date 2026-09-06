#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Test support: an ExtensionCatalogService whose marketplace and bundled roots
// live in a private temp dir. The marketplace dir is always a scan tier and
// seeding runs in every mode, so a test that constructed the service with only
// an explicit plugin dir would otherwise scan — and seed into — the real user
// profile.

#include <QTemporaryDir>
#include <utility>

#include "pj_runtime/ExtensionCatalogService.h"

namespace PJ::test {

struct HermeticCatalog {
  explicit HermeticCatalog(const QString& install_dir)
      : service(
            ExtensionCatalogService::Paths{install_dir, sandbox.path(), sandbox.path() + "/no_bundled"},
            DiagnosticSink{}, nullptr) {}

  explicit HermeticCatalog(StaticPluginSet static_plugins, DiagnosticSink sink = {})
      : service(
            ExtensionCatalogService::Paths{QString(), sandbox.path(), sandbox.path() + "/no_bundled"}, std::move(sink),
            std::move(static_plugins), nullptr) {}

  QTemporaryDir sandbox;  ///< marketplace root; the bundled path under it does not exist, so seeding is a no-op
  ExtensionCatalogService service;
};

}  // namespace PJ::test
