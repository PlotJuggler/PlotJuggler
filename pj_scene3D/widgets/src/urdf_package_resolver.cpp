// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "urdf_package_resolver.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include "pj_runtime/HttpUrl.h"

using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
constexpr char kPerSourceKey[] = "pj_scene3d/urdf_per_source_packages";
constexpr char kSearchRootsKey[] = "pj_scene3d/urdf_search_roots";
constexpr int kAncestorDepthCap = 10;

// Join a root directory and a relative path with a single separator.
QString joinPath(const QString& root, const std::string& rel) {
  QString r = root;
  while (r.endsWith('/')) {
    r.chop(1);
  }
  QString tail = QString::fromStdString(rel);
  while (tail.startsWith('/')) {
    tail.remove(0, 1);
  }
  return r + '/' + tail;
}

// Whether references may reach host files: never in the browser, which grants no
// host-file access, nor for a web-hosted layout's robot on the desktop.
bool allowsLocalFiles(bool remote_only) {
#ifdef PJ_TARGET_WASM
  (void)remote_only;
  return false;
#else
  return !remote_only;
#endif
}

// Whether a package's network root may be fetched. The desktop honors one only where the model
// already reaches the network, a URL URDF or a web-hosted layout's robot, so a layout file cannot
// send a topic or file URDF's meshes there; the browser reaches meshes no other way.
bool allowsNetworkRoot(bool source_is_url, bool remote_only) {
#ifdef PJ_TARGET_WASM
  (void)source_is_url;
  (void)remote_only;
  return true;
#else
  return source_is_url || remote_only;
#endif
}

// `rel` under the directory `root` names: a URL, a local path for a file root,
// or "" when the root is unusable (a file root without `local_files`) or `rel`
// climbs out of it.
std::string resolveUnderRoot(const QUrl& root, const std::string& rel, bool local_files) {
  const bool local = root.isLocalFile();
  if (local ? !local_files : !PJ::isHttpUrl(root)) {
    return {};
  }
  QUrl directory = root.adjusted(QUrl::NormalizePathSegments);
  if (!directory.path().endsWith('/')) {
    directory.setPath(directory.path() + '/');
  }
  QString tail = QString::fromStdString(rel);
  while (tail.startsWith('/')) {
    tail.remove(0, 1);
  }
  // An encoded separator or a backslash turns into a separator only when the filesystem or the
  // server decodes the path, after any check made here.
  if (tail.contains('\\') || tail.contains("%2f"_L1, Qt::CaseInsensitive) ||
      tail.contains("%5c"_L1, Qt::CaseInsensitive)) {
    return {};
  }
  // A path-only reference: a ':' or "//" in `rel` can never become a scheme or
  // an authority, so the target keeps the root's origin.
  QUrl relative;
  relative.setPath(tail, QUrl::TolerantMode);
  const QUrl target = directory.resolved(relative);
  // Contained as the filesystem or the server sees the path: decoded and normalized.
  if (!QDir::cleanPath(target.path(QUrl::FullyDecoded)).startsWith(directory.path(QUrl::FullyDecoded))) {
    return {};
  }
  return (local ? target.toLocalFile() : target.toString(QUrl::FullyEncoded)).toStdString();
}
}  // namespace

UrdfPackageResolver::UrdfPackageResolver() = default;
UrdfPackageResolver::~UrdfPackageResolver() = default;

void UrdfPackageResolver::setEmbeddedAssets(QMap<QString, QByteArray> assets) {
  embedded_assets_ = std::move(assets);
  extracted_assets_dir_.reset();  // drop files extracted from the previous map
}

void UrdfPackageResolver::addSearchRoot(const QString& root) {
  if (root.isEmpty()) {
    return;
  }
  const QString canonical = QDir::cleanPath(root);
  if (!search_roots_.contains(canonical)) {
    search_roots_.append(canonical);
  }
}

void UrdfPackageResolver::autoSeedSearchRoots(const QString& urdf_dir) {
  // Global roots come first (most authoritative user choice), seeded once.
  if (!seeded_global_roots_ && settings_ != nullptr) {
    const QStringList global = settings_->value(QString::fromLatin1(kSearchRootsKey)).toStringList();
    for (const QString& g : global) {
      addSearchRoot(g);
    }
    seeded_global_roots_ = true;
  }

  if (!urdf_dir.isEmpty()) {
    addSearchRoot(urdf_dir);
  }
  if (!source_path_.isEmpty()) {
    addSearchRoot(QFileInfo(source_path_).absolutePath());
  }

  // Path-list env vars split on the native separator (':' POSIX, ';' Windows —
  // a ':' split would shear the drive letter off "D:/...").
  const QChar list_sep = QDir::listSeparator();
  // $ROS_PACKAGE_PATH entries are used verbatim.
  const QString ros_pkg = qEnvironmentVariable("ROS_PACKAGE_PATH");
  for (const QString& p : ros_pkg.split(list_sep, Qt::SkipEmptyParts)) {
    addSearchRoot(p);
  }
  // $AMENT_PREFIX_PATH and $COLCON_PREFIX_PATH entries get "/share" appended.
  for (const char* env : {"AMENT_PREFIX_PATH", "COLCON_PREFIX_PATH"}) {
    const QString v = qEnvironmentVariable(env);
    for (const QString& p : v.split(list_sep, Qt::SkipEmptyParts)) {
      addSearchRoot(p + "/share");
    }
  }
}

ResolvedMesh UrdfPackageResolver::resolveUri(
    const std::string& uri, const std::string& urdf_dir, bool source_is_url,
    const QMap<QString, QUrl>& package_url_roots, bool remote_only) {
  ResolvedMesh out;
  const QString quri = QString::fromStdString(uri);
  // A browser file picker grants bytes for the selected URDF only, not a capability
  // to arbitrary host paths named by its contents; a web-hosted layout grants none.
  const bool local_files = allowsLocalFiles(remote_only);

  if (quri.startsWith("file://")) {
    if (!local_files) {
      out.issue = MeshResolveIssue::kMissingFile;
      return out;
    }
    // Absolute local path; skip the package chain entirely. QUrl::toLocalFile
    // percent-decodes (e.g. %20 -> space) and strips an optional localhost
    // authority — a raw mid(7) slice would mangle encoded paths.
    out.resolved = true;
    out.path = QUrl(quri).toLocalFile().toStdString();
    return out;
  }

  if (quri.startsWith("http://") || quri.startsWith("https://")) {
    // Only honored for URL sources (same-origin consent). Otherwise blocked.
    if (source_is_url) {
      out.resolved = true;
      out.is_url = true;
      out.path = uri;
    } else {
      out.issue = MeshResolveIssue::kBlockedHttp;
    }
    return out;
  }

  if (quri.startsWith("package://")) {
    const QString rest = quri.mid(static_cast<int>(std::string("package://").size()));
    const qsizetype slash = rest.indexOf('/');
    if (slash <= 0) {
      out.issue = MeshResolveIssue::kMalformedRef;
      return out;  // malformed package URI
    }
    const std::string pkg = rest.left(slash).toStdString();
    const std::string rel = rest.mid(slash + 1).toStdString();
    const std::string path = resolve(pkg, rel, urdf_dir, source_is_url, package_url_roots, remote_only);
    if (!path.empty()) {
      out.resolved = true;
      out.path = path;
      // An http(s) root or a URL ancestor hit (step 2 URL variant) returns a URL.
      out.is_url = path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0;
    } else {
      out.package = pkg;
      out.issue = MeshResolveIssue::kUnresolvedPackage;
    }
    return out;
  }

  // Absolute filesystem path (POSIX "/..." or a Windows drive "C:/..."): return
  // it verbatim — joining it onto urdf_dir would mangle it. Machine-generated
  // URDFs (xacro $(find) expansion, MoveIt) emit absolute mesh paths. Apply the
  // same existence-check-else-unresolved policy as the package steps so a
  // missing file surfaces in the status instead of resolving to a dead path.
  if (QDir::isAbsolutePath(quri)) {
    if (local_files && QFileInfo::exists(quri)) {
      out.resolved = true;
      out.path = uri;
    } else {
      out.issue = MeshResolveIssue::kMissingFile;
    }
    return out;
  }

  // Bare relative path — the GUARD. Never enters the package chain. Resolve
  // relative to urdf_dir (filesystem join, or URL base concatenation).
  if (urdf_dir.empty()) {
    out.issue = MeshResolveIssue::kNoAnchor;
    return out;  // Topic source with no dir: a bare path has nothing to anchor.
  }
  const QString base = QString::fromStdString(urdf_dir);
  const bool url_base = source_is_url || base.startsWith("http://") || base.startsWith("https://");
  if (remote_only && !url_base) {
    out.issue = MeshResolveIssue::kMissingFile;
    return out;
  }
  out.resolved = true;
  out.path = joinPath(base, uri).toStdString();
  out.is_url = url_base;
  return out;
}

std::string UrdfPackageResolver::resolve(
    const std::string& pkg, const std::string& rel, const std::string& urdf_dir, bool source_is_url,
    const QMap<QString, QUrl>& package_url_roots, bool remote_only) {
  // An explicit root is the model author's answer for this package: authoritative,
  // so a miss under it never falls back to a heuristic that might find another copy.
  // A network root this model may not fetch is skipped: the package resolves here.
  if (const auto root = package_url_roots.constFind(QString::fromStdString(pkg));
      root != package_url_roots.cend() && (!PJ::isHttpUrl(*root) || allowsNetworkRoot(source_is_url, remote_only))) {
    return resolveUnderRoot(*root, rel, allowsLocalFiles(remote_only));
  }
  // Step 0 — embedded-asset exact-name lookup.
  const std::string uri = "package://" + pkg + "/" + rel;
  if (std::string p = stepEmbeddedAsset(uri); !p.empty()) {
    return p;
  }
  // The rest of the chain finds packages on this host, except the URL ancestor walk.
  if (remote_only) {
    return source_is_url ? stepAncestor(pkg, rel, urdf_dir, source_is_url) : std::string{};
  }
  // Step 1 — remembered per-source map.
  if (std::string p = stepRememberedMap(pkg, rel); !p.empty()) {
    return p;
  }
  // Step 2 — ancestor heuristic (filesystem or URL).
  if (std::string p = stepAncestor(pkg, rel, urdf_dir, source_is_url); !p.empty()) {
    return p;
  }
  // Step 3 — search roots.
  if (std::string p = stepSearchRoots(pkg, rel); !p.empty()) {
    return p;
  }
  // Miss: the caller records `pkg` (the resolver keeps no global tally).
  return {};
}

std::string UrdfPackageResolver::stepEmbeddedAsset(const std::string& uri) {
  const QString key = QString::fromStdString(uri);
  auto it = embedded_assets_.constFind(key);
  if (it == embedded_assets_.constEnd()) {
    return {};
  }
  if (!extracted_assets_dir_) {
    extracted_assets_dir_ = std::make_unique<QTemporaryDir>();
  }
  if (!extracted_assets_dir_->isValid()) {
    return {};
  }
  // On-disk filename = "<8-hex-sha1-of-key>_<flattened-ref>". The flatten alone
  // is lossy/non-injective (e.g. "package://a/b.stl" and "package://a_b.stl" both
  // flatten identically), so two distinct refs would collide and overwrite each
  // other. Prefixing a hash of the verbatim key makes the name collision-proof
  // while the flattened tail keeps the original extension last for format
  // sniffing. (Path traversal is already neutralized: '/' becomes '_'.)
  QString flat = key;
  flat.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
  const QString digest =
      QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex()).left(8);
  const QString out_path = extracted_assets_dir_->filePath(digest + '_' + flat);
  // Asset content is immutable for the dir's lifetime (setEmbeddedAssets
  // resets the dir), so an already-extracted file is never rewritten — this
  // avoids truncating bytes under an in-flight assimp read on Retry/Locate.
  if (QFileInfo::exists(out_path)) {
    return out_path.toStdString();
  }
  QFile f(out_path);
  if (!f.open(QIODevice::WriteOnly)) {
    return {};
  }
  f.write(*it);
  f.close();
  return out_path.toStdString();
}

std::string UrdfPackageResolver::stepRememberedMap(const std::string& pkg, const std::string& rel) {
  if (settings_ == nullptr || source_path_.isEmpty()) {
    return {};
  }
  const QVariantMap top = settings_->value(QString::fromLatin1(kPerSourceKey)).toMap();
  const QVariant per_source = top.value(source_path_);
  if (!per_source.isValid()) {
    return {};
  }
  const QVariantMap pkg_map = per_source.toMap();
  const QString root = pkg_map.value(QString::fromStdString(pkg)).toString();
  if (root.isEmpty()) {
    return {};
  }
  const QString candidate = joinPath(root, rel);
  if (QFileInfo::exists(candidate)) {
    return candidate.toStdString();
  }
  return {};
}

std::string UrdfPackageResolver::stepAncestor(
    const std::string& pkg, const std::string& rel, const std::string& urdf_dir, bool source_is_url) {
  if (urdf_dir.empty()) {
    return {};  // Topic source: no path/URL to walk.
  }
  const QString qpkg = QString::fromStdString(pkg);

  if (source_is_url) {
    // URL variant: scan path segments for the rightmost == pkg, rebuild the URL
    // up to (and including) that segment, then append rel. No double-nesting.
    QUrl url(QString::fromStdString(urdf_dir));
    const QString path = url.path();
    const QStringList segs = path.split('/', Qt::SkipEmptyParts);
    qsizetype hit = -1;
    for (qsizetype i = segs.size() - 1; i >= 0; --i) {
      if (segs[i] == qpkg) {
        hit = i;
        break;
      }
    }
    if (hit < 0) {
      return {};
    }
    QString rebuilt = url.scheme() + "://" + url.host();
    if (url.port() != -1) {
      rebuilt += ':' + QString::number(url.port());
    }
    for (qsizetype i = 0; i <= hit; ++i) {
      rebuilt += '/' + segs[i];
    }
    return joinPath(rebuilt, rel).toStdString();
  }

  // Filesystem variant: walk up from urdf_dir; at each ancestor whose basename
  // == pkg, accept only after verifying ancestor/rel exists on disk.
  QDir dir(QString::fromStdString(urdf_dir));
  for (int depth = 0; depth <= kAncestorDepthCap; ++depth) {
    if (dir.dirName() == qpkg) {
      const QString candidate = joinPath(dir.absolutePath(), rel);
      if (QFileInfo::exists(candidate)) {
        return candidate.toStdString();
      }
    }
    if (!dir.cdUp()) {
      break;
    }
  }
  return {};
}

std::string UrdfPackageResolver::stepSearchRoots(const std::string& pkg, const std::string& rel) {
  for (const QString& root : search_roots_) {
    const QString pkg_dir = joinPath(root, pkg);
    // Package identity = directory basename only. No package.xml check. Accept
    // only when the mesh file itself exists (mirrors stepAncestor) — a bare
    // package-dir stub missing the mesh must miss (return ""), not yield a
    // false-positive path the loader would silently fail to open.
    if (QFileInfo(pkg_dir).isDir()) {
      const QString candidate = joinPath(pkg_dir, rel);
      if (QFileInfo::exists(candidate)) {
        return candidate.toStdString();
      }
    }
  }
  return {};
}

void UrdfPackageResolver::rememberPackageRoot(const std::string& pkg, const QString& root_dir) {
  const QString qpkg = QString::fromStdString(pkg);
  // `root_dir` is the resolved PACKAGE directory (the folder named <pkg> that
  // contains `rel`). The two consumers need DIFFERENT levels:
  //   - per-source map  -> stepRememberedMap does joinPath(root, rel)        -> store the package dir
  //   - search roots  -> stepSearchRoots does joinPath(root, pkg) + rel    -> store the PARENT
  // Storing the package dir in both (the old bug) double-nested <pkg>/<pkg> in
  // step 3, silently breaking cross-dataset reuse.
  const QString pkg_dir = QDir::cleanPath(root_dir);
  const QString parent = QFileInfo(pkg_dir).absolutePath();

  // Append the PARENT to the global search roots (cross-dataset reuse).
  addSearchRoot(parent);

  if (settings_ != nullptr) {
    // Persist the PARENT to the global roots list.
    QStringList global = settings_->value(QString::fromLatin1(kSearchRootsKey)).toStringList();
    if (!global.contains(parent)) {
      global.append(parent);
      settings_->setValue(QString::fromLatin1(kSearchRootsKey), global);
    }
    // Persist the PACKAGE dir to the per-source remembered map (reopen repeatability).
    if (!source_path_.isEmpty()) {
      QVariantMap top = settings_->value(QString::fromLatin1(kPerSourceKey)).toMap();
      QVariantMap pkg_map = top.value(source_path_).toMap();
      pkg_map.insert(qpkg, pkg_dir);
      top.insert(source_path_, pkg_map);
      settings_->setValue(QString::fromLatin1(kPerSourceKey), top);
    }
  }
}

}  // namespace pj::scene3d
