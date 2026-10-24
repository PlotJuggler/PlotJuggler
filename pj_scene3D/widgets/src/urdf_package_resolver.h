#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Resolves mesh references found inside a URDF to a loadable local path / URL.
//
// Two entry points:
//   resolveUri(uri, urdf_dir, source_is_url)  — URI-scheme dispatch + the guard.
//   resolve(pkg, rel)                          — the package:// resolution chain.
//
// The "package://" GUARD: only package:// URIs ever enter the package chain.
// Bare relative paths (e.g. "robotiq_arg85_description/meshes/x.stl") resolve
// relative to urdf_dir and NEVER touch the package search — otherwise a bare
// path that happens to look like a package name would wrongly match a same-named
// search-root directory.
//
// Package identity is the directory BASENAME only — never a package.xml check.
//
// Plain C++ class (not a QObject). QSettings is INJECTED via setSettings(); the
// resolver never `new QSettings`. Design: URDF_MESH_ASSET_RESOLUTION_DESIGN.md §3.

#include <QByteArray>
#include <QMap>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <memory>
#include <string>

#include "pj_scene3d_core/robot_model.h"

class QTemporaryDir;

namespace pj::scene3d {

// Outcome of a resolveUri() call. Unresolved tracking is per-call (carried out
// in the return value), NOT resolver-global: the resolver is dock-shared across
// robot layers, so a global tally would let one layer's load wipe/pollute a
// sibling's. The layer collects `package`/`issue` into its own
// per-model set.
struct ResolvedMesh {
  bool resolved{false};
  std::string path;     // local filesystem path or fetchable URL when resolved
  std::string package;  // the unresolved package name (when !resolved && was package://)
  bool is_url{false};   // path is an http(s):// URL to fetch
  // Why resolution failed (kNone when resolved). The layer folds this into its
  // status line ("N http refs blocked", "N absolute paths missing", …).
  MeshResolveIssue issue{MeshResolveIssue::kNone};
};

class UrdfPackageResolver {
 public:
  UrdfPackageResolver();
  ~UrdfPackageResolver();

  UrdfPackageResolver(const UrdfPackageResolver&) = delete;
  UrdfPackageResolver& operator=(const UrdfPackageResolver&) = delete;

  // --- Injection / configuration -----------------------------------------

  // QSettings is owned by the host (e.g. MainWindow's app settings). The
  // resolver only reads/writes the two keys documented in the design.
  void setSettings(QSettings* settings) {
    settings_ = settings;
  }

  // Step-0 in-band embedded-asset dictionary: name == verbatim mesh ref string.
  // Replaces the whole map, so any files already extracted from a previous map
  // may now be stale; the extraction dir is reset here to close that hole (a
  // fresh QTemporaryDir is lazily created on the next embedded-asset hit). Defined
  // out-of-line because resetting the unique_ptr needs QTemporaryDir complete.
  void setEmbeddedAssets(QMap<QString, QByteArray> assets);

  // The dataset source path keys the per-source remembered map (step 1).
  void setSourcePath(const QString& source_path) {
    source_path_ = source_path;
  }

  // Seed the search roots from the URDF dir, the source dir, and the ROS-ish env
  // vars ($ROS_PACKAGE_PATH, $AMENT_PREFIX_PATH/share, $COLCON_PREFIX_PATH/share).
  // Idempotent: existing roots are not duplicated. Reads global roots from
  // QSettings on first call. urdf_dir may be empty (Topic source).
  void autoSeedSearchRoots(const QString& urdf_dir);

  // Append a single search root (e.g. from the "Locate…" dialog). Idempotent.
  void addSearchRoot(const QString& root);

  const QStringList& searchRoots() const {
    return search_roots_;
  }

  // --- Resolution ----------------------------------------------------------

  // Full URI-scheme dispatch + guard. `urdf_dir` is the directory (or URL base)
  // the URDF was loaded from; `source_is_url` gates http(s) refs.
  ResolvedMesh resolveUri(const std::string& uri, const std::string& urdf_dir, bool source_is_url);

  // The package:// chain (steps 0..3). Returns "" on miss; the caller records
  // the unresolved package (the resolver keeps no global tally — see
  // ResolvedMesh). `urdf_dir` enables the ancestor heuristic (step 2);
  // `source_is_url` selects the URL-segment variant of that heuristic.
  std::string resolve(const std::string& pkg, const std::string& rel, const std::string& urdf_dir, bool source_is_url);

  // Manually remember a package root (the "Locate…" result): writes the
  // per-source map AND appends to the global search roots.
  void rememberPackageRoot(const std::string& pkg, const QString& root_dir);

 private:
  // Step helpers — each returns a non-empty path on hit.
  std::string stepEmbeddedAsset(const std::string& uri);
  std::string stepRememberedMap(const std::string& pkg, const std::string& rel);
  std::string stepAncestor(
      const std::string& pkg, const std::string& rel, const std::string& urdf_dir, bool source_is_url);
  std::string stepSearchRoots(const std::string& pkg, const std::string& rel);

  QSettings* settings_{nullptr};
  QMap<QString, QByteArray> embedded_assets_;
  QString source_path_;
  QStringList search_roots_;
  bool seeded_global_roots_{false};
  // Assets extracted to disk so a path can be returned; lives until exit or a
  // setEmbeddedAssets() that resets it. Content is immutable for the dir's
  // lifetime, so stepEmbeddedAsset never rewrites an already-extracted file.
  std::unique_ptr<QTemporaryDir> extracted_assets_dir_;
};

}  // namespace pj::scene3d
