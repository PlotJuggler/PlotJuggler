#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Resolves mesh references found inside a URDF to a loadable local path / URL.
//
// Two entry points:
//   resolveUri(uri, urdf_dir, source_is_url, roots)  — URI-scheme dispatch + the guard.
//   resolve(pkg, rel, ..., roots)                     — the package:// resolution chain: step 0 embedded assets,
//                                                       1 the remembered per-source map (QSettings), 2 the ancestor
//                                                       heuristic (filesystem or URL), 3 the search roots.
//
// Step 3 looks for <root>/<pkg> in every search root, then searches the user's folders (the global list in QSettings,
// edited in Preferences and by "Locate...") at workspace depth: ~/ws or ~/ws/src finds ~/ws/src/<repo>/<pkg>. The
// URDF and data folders stay one level deep, since the data folder may be $HOME.
//
// `package_url_roots` (package name -> absolute http(s)/file directory URL) is the calling MODEL's explicit mapping,
// passed per call because the resolver is shared by every robot of a dock and keeps no per-model state. A mapped
// package resolves only under its root, with no fallback; unmapped packages take the chain. A mapping never permits
// the model's direct http(s) refs.
//
// The "package://" GUARD: only package:// URIs enter the package chain. Bare relative paths (e.g.
// "robotiq_arg85_description/meshes/x.stl") resolve relative to urdf_dir and NEVER touch the package search, or a bare
// path that looks like a package name would match a same-named search-root directory.
//
// Package identity is the directory BASENAME only, never a package.xml check.
//
// Plain C++ class (not a QObject). QSettings is INJECTED via setSettings(); the resolver never `new QSettings`.

#include <QByteArray>
#include <QHash>
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

// Outcome of a resolveUri() call. Unresolved tracking is per call (carried in the return value), NOT resolver-global:
// the resolver is dock-shared across robot layers, so a global tally would let one layer's load pollute a sibling's.
// The layer collects `package`/`issue` into its own per-model set.
struct ResolvedMesh {
  bool resolved{false};
  std::string path;     // local filesystem path or fetchable URL when resolved
  std::string package;  // the unresolved package name (when !resolved && was package://)
  bool is_url{false};   // path is an http(s):// URL to fetch
  // Why resolution failed (kNone when resolved); the layer folds it into its status line.
  MeshResolveIssue issue{MeshResolveIssue::kNone};
};

class UrdfPackageResolver {
 public:
  UrdfPackageResolver();
  ~UrdfPackageResolver();

  UrdfPackageResolver(const UrdfPackageResolver&) = delete;
  UrdfPackageResolver& operator=(const UrdfPackageResolver&) = delete;

  // QSettings is owned by the host (e.g. MainWindow's app settings). The resolver only reads/writes the two keys
  // kSearchRootsKey and kPerSourceKey.
  void setSettings(QSettings* settings) {
    settings_ = settings;
  }

  // Step-0 in-band embedded-asset dictionary: name == verbatim mesh ref string. Replaces the whole map, so files
  // already extracted from a previous map may be stale: the extraction dir is reset here (a fresh QTemporaryDir is
  // created lazily on the next hit). Out-of-line because resetting the unique_ptr needs QTemporaryDir complete.
  void setEmbeddedAssets(QMap<QString, QByteArray> assets);

  void setSourcePath(const QString& source_path) {
    source_path_ = source_path;
  }

  // Seeds the search roots from the URDF dir, the source dir and the ROS env vars ($ROS_PACKAGE_PATH,
  // $AMENT_PREFIX_PATH/share, $COLCON_PREFIX_PATH/share). Idempotent; reads global roots from QSettings on first call.
  // urdf_dir may be empty (Topic source).
  void autoSeedSearchRoots(const QString& urdf_dir);

  void addSearchRoot(const QString& root);

  // Drops every search root and the folder indexes; the next autoSeedSearchRoots() re-reads the global list. A
  // remembered per-source mapping survives.
  void resetSearchRoots();

  // The user's folders (QSettings kSearchRootsKey), searched at workspace depth.
  static QStringList globalSearchRoots(const QSettings& settings);
  static void setGlobalSearchRoots(QSettings& settings, const QStringList& roots);

  // The shallowest directory named `pkg` under `root` at workspace depth, or `root` itself when it is named `pkg`;
  // "" when none.
  static QString findPackageDir(const QString& root, const QString& pkg);

  const QStringList& searchRoots() const {
    return search_roots_;
  }

  // Full URI-scheme dispatch + guard. `urdf_dir` is the directory (or URL base)
  // the URDF was loaded from; `source_is_url` gates http(s) refs. `remote_only`
  // (a web-hosted layout's robot) refuses everything on this host, as the browser
  // build always does: file:// and absolute paths, bare paths not under a URL,
  // file roots, and the local package chain (remembered map, filesystem ancestor,
  // search roots). Embedded assets and network references still resolve.
  ResolvedMesh resolveUri(
      const std::string& uri, const std::string& urdf_dir, bool source_is_url,
      const QMap<QString, QUrl>& package_url_roots = {}, bool remote_only = false);

  // The package:// chain (an explicit root, else steps 0..3). On the desktop an http(s) root counts only for a URL
  // source or with `remote_only`; for any other model it is skipped and the package takes steps 0..3. Returns "" on a
  // miss, also for a mapped package whose ref escapes its root (decoded and normalized; an encoded separator or a
  // backslash never resolves) or whose root is unusable (relative, another scheme, a file root in the browser or with
  // `remote_only`, which also skips the local steps 1..3); the caller records the unresolved package (see
  // ResolvedMesh). `urdf_dir` enables the ancestor heuristic (step 2); `source_is_url` selects its URL-segment
  // variant.
  std::string resolve(
      const std::string& pkg, const std::string& rel, const std::string& urdf_dir, bool source_is_url,
      const QMap<QString, QUrl>& package_url_roots = {}, bool remote_only = false);

  // The "Locate..." result: writes the per-source map AND appends `search_root` (the folder the user chose; the
  // package directory's parent when empty) to the global search roots.
  void rememberPackageRoot(const std::string& pkg, const QString& root_dir, const QString& search_root = {});

 private:
  std::string stepEmbeddedAsset(const std::string& uri);
  std::string stepRememberedMap(const std::string& pkg, const std::string& rel);
  std::string stepAncestor(
      const std::string& pkg, const std::string& rel, const std::string& urdf_dir, bool source_is_url);
  std::string stepSearchRoots(const std::string& pkg, const std::string& rel);

  // Directory basename -> every directory with that name, shallowest first.
  using PackageIndex = QHash<QString, QStringList>;
  static PackageIndex indexPackageDirs(const QString& root);
  const PackageIndex& packageIndex(const QString& root);

  QSettings* settings_{nullptr};
  QMap<QString, QByteArray> embedded_assets_;
  QString source_path_;
  QStringList search_roots_;
  // The subset of search_roots_ the user chose; only these are searched at workspace depth.
  QStringList global_roots_;
  QHash<QString, PackageIndex> package_indexes_;
  bool seeded_global_roots_{false};
  // Assets extracted to disk so a path can be returned; lives until exit or a setEmbeddedAssets() reset. Content is
  // immutable for the dir's lifetime, so stepEmbeddedAsset never rewrites an extracted file.
  std::unique_ptr<QTemporaryDir> extracted_assets_dir_;
};

}  // namespace pj::scene3d
