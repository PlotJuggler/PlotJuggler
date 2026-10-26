#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Resolves mesh references found inside a URDF to a loadable local path / URL.
//
// Two entry points:
//   resolveUri(uri, urdf_dir, source_is_url, roots)  — URI-scheme dispatch + the guard.
//   resolve(pkg, rel, ..., roots)                     — the package:// resolution chain.
//
// `package_url_roots` (package name -> absolute http(s)/file directory URL) is
// the calling MODEL's explicit mapping, passed per call because the resolver is
// shared by every robot of a dock and must keep no per-model state. A mapped
// package resolves only under its root, with no fallback; unmapped packages take
// the legacy chain. A mapping never permits the model's direct http(s) refs.
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
#include <QUrl>
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
  // the URDF was loaded from; `source_is_url` gates http(s) refs. `remote_only`
  // (a web-hosted layout's robot) refuses everything on this host, as the browser
  // build always does: file:// and absolute paths, bare paths not under a URL,
  // file roots, and the local package chain (remembered map, filesystem ancestor,
  // search roots). Embedded assets and network references still resolve.
  ResolvedMesh resolveUri(
      const std::string& uri, const std::string& urdf_dir, bool source_is_url,
      const QMap<QString, QUrl>& package_url_roots = {}, bool remote_only = false);

  // The package:// chain (an explicit root, else steps 0..3). On the desktop an
  // http(s) root counts only for a URL source or with `remote_only`; for any other
  // model it is skipped and the package takes steps 0..3. Returns "" on
  // miss, also for a mapped package whose ref escapes its root (decoded and
  // normalized; an encoded separator or a backslash never resolves) or whose root
  // is unusable (relative, another scheme, a file root in the browser or with
  // `remote_only`, which also skips the local steps 1..3); the caller
  // records the unresolved package (the resolver keeps no global tally — see
  // ResolvedMesh). `urdf_dir` enables the ancestor heuristic (step 2);
  // `source_is_url` selects the URL-segment variant of that heuristic.
  std::string resolve(
      const std::string& pkg, const std::string& rel, const std::string& urdf_dir, bool source_is_url,
      const QMap<QString, QUrl>& package_url_roots = {}, bool remote_only = false);

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
