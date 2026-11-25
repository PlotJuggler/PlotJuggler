// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "urdf_package_resolver.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QList>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>
#include <utility>

#include "local_path.h"
#include "pj_runtime/HttpUrl.h"

using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
constexpr char kPerSourceKey[] = "pj_scene3d/urdf_per_source_packages";
constexpr char kSearchRootsKey[] = "pj_scene3d/urdf_search_roots";
constexpr int kAncestorDepthCap = 10;
// ~/ws/src/<group>/<repo>/<pkg> is depth 4 from ~/ws.
constexpr int kPackageSearchDepth = 5;
// ponytail: the index is built on the GUI thread on the first miss per folder; the cap keeps a folder like $HOME from
// stalling it. Move the scan to a worker if real workspaces exceed the cap.
constexpr qsizetype kPackageSearchMaxDirs = 20000;

// colcon's build/ and log/ trees hold copies of the sources; hidden folders hold caches and VCS data.
bool skippedPackageSearchDir(const QString& name) {
  return name.startsWith('.') || name == "build"_L1 || name == "log"_L1;
}

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

// joinPath() for a local `root` (local_path.h joinContentPath: cleaned, empty when `rel` climbs onto a share). The
// "./" keeps a `rel` that reads as absolute ("C:/x") under `root`, as joinPath() does.
QString joinLocalPath(const QString& root, const std::string& rel) {
  QString tail = QString::fromStdString(rel);
  while (tail.startsWith('/')) {
    tail.remove(0, 1);
  }
  return joinContentPath(root, u"./"_s + tail);
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

// Whether a package's network root may be fetched. A root is the user's chosen source, so it is
// honored whatever the remote-fetch setting. The desktop honors one only where the model already
// reaches the network, a URL URDF or a web-hosted layout's robot, so a layout file cannot send a
// topic or file URDF's meshes there; the browser reaches meshes no other way.
bool allowsNetworkRoot(bool source_is_url, bool remote_only) {
#ifdef PJ_TARGET_WASM
  (void)source_is_url;
  (void)remote_only;
  return true;
#else
  return source_is_url || remote_only;
#endif
}

// `rel` under the directory `root` names: a URL, a local path for a file root (on a network share too: the user chose
// the root), or "" when the root is unusable (a file root without `local_files`) or `rel` climbs out of it.
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
  return (local ? filePathOf(target) : target.toString(QUrl::FullyEncoded)).toStdString();
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

void UrdfPackageResolver::clearPackageIndexes() {
  package_indexes_.clear();
}

QStringList UrdfPackageResolver::globalSearchRoots(const QSettings& settings) {
  return settings.value(QString::fromLatin1(kSearchRootsKey)).toStringList();
}

void UrdfPackageResolver::setGlobalSearchRoots(QSettings& settings, const QStringList& roots) {
  QStringList cleaned;
  for (const QString& root : roots) {
    const QString canonical = QDir::cleanPath(root);
    if (!root.isEmpty() && !cleaned.contains(canonical)) {
      cleaned.append(canonical);
    }
  }
  settings.setValue(QString::fromLatin1(kSearchRootsKey), cleaned);
}

void UrdfPackageResolver::addUserFolder(const QString& folder) {
  if (settings_ != nullptr) {
    setGlobalSearchRoots(*settings_, globalSearchRoots(*settings_) << folder);
  }
}

bool UrdfPackageResolver::folderHasPackage(const QString& folder, const QString& pkg) {
  return !pkg.isEmpty() && !packageIndex(QDir::cleanPath(folder)).value(pkg).isEmpty();
}

QStringList UrdfPackageResolver::rosDistroShareFolders(const QString& base) {
  QStringList folders;
  for (const QFileInfo& distro :
       QDir(base).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed)) {
    if (QFileInfo(distro.filePath() + "/share"_L1).isDir()) {
      folders.append(distro.filePath() + "/share"_L1);
    }
  }
  return folders;
}

QStringList UrdfPackageResolver::userFolders() const {
  return settings_ != nullptr ? globalSearchRoots(*settings_) : QStringList{};
}

QStringList UrdfPackageResolver::searchRoots() const {
  QStringList roots = userFolders();
  for (const QString& root : search_roots_) {
    if (!roots.contains(root)) {
      roots.append(root);
    }
  }
  return roots;
}

UrdfPackageResolver::PackageIndex UrdfPackageResolver::indexPackageDirs(const QString& root) {
  const QString start = QDir::cleanPath(root);
  // A folder the user added may be the package itself.
  PackageIndex index{{QFileInfo(start).fileName(), {start}}};
  if (QFileInfo::exists(start + "/package.xml"_L1)) {
    return index;
  }
  // Breadth-first, so each name lists its shallowest directory first. A directory with a package.xml is a package:
  // its subfolders are its own content, not other packages.
  QList<std::pair<QString, int>> queue{{start, 0}};
  qsizetype visited = 0;
  for (qsizetype i = 0; i < queue.size() && visited < kPackageSearchMaxDirs; ++i) {
    const auto [dir, depth] = queue.at(i);
    for (const QString& name : QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
      if (skippedPackageSearchDir(name)) {
        continue;
      }
      if (++visited > kPackageSearchMaxDirs) {
        break;
      }
      const QString path = QDir(dir).filePath(name);
      index[name].append(path);
      if (depth + 1 < kPackageSearchDepth && !QFileInfo::exists(path + "/package.xml"_L1)) {
        queue.append({path, depth + 1});
      }
    }
  }
  return index;
}

const UrdfPackageResolver::PackageIndex& UrdfPackageResolver::packageIndex(const QString& root) {
  auto found = package_indexes_.find(root);
  if (found == package_indexes_.end()) {
    found = package_indexes_.insert(root, indexPackageDirs(root));
  }
  return *found;
}

void UrdfPackageResolver::autoSeedSearchRoots(const QString& urdf_dir) {
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
#ifndef Q_OS_WIN
  // An app started from a desktop icon has no sourced ROS environment; binary packages install here.
  for (const QString& share : rosDistroShareFolders(u"/opt/ros"_s)) {
    addSearchRoot(share);
  }
#endif
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
    // Absolute local path: skip the package chain. localFileOf percent-decodes (%20 -> space), which a raw mid(7)
    // slice would not.
    const QUrl url(quri);
    if (namesNetworkShare(url)) {
      out.issue = MeshResolveIssue::kNetworkShare;
      return out;
    }
    const QString path = localFileOf(url);
    if (path.isEmpty()) {
      out.issue = MeshResolveIssue::kMissingFile;
      return out;
    }
    out.resolved = true;
    out.path = path.toStdString();
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

  // Absolute filesystem path (POSIX "/..." or a Windows drive "C:/..."): returned verbatim, since joining it onto
  // urdf_dir would mangle it (xacro $(find) expansion and MoveIt emit absolute mesh paths). Same existence-check policy
  // as the package steps, so a missing file surfaces in the status instead of resolving to a dead path.
  if (QDir::isAbsolutePath(quri)) {
    if (local_files && isNetworkSharePath(quri)) {
      out.issue = MeshResolveIssue::kNetworkShare;
    } else if (local_files && QFileInfo::exists(quri)) {
      out.resolved = true;
      out.path = uri;
    } else {
      out.issue = MeshResolveIssue::kMissingFile;
    }
    return out;
  }

  // Bare relative path (the GUARD): never enters the package chain; resolved against urdf_dir.
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
  const QString path = url_base ? joinPath(base, uri) : joinLocalPath(base, uri);
  if (path.isEmpty()) {
    out.issue = MeshResolveIssue::kNetworkShare;
    return out;
  }
  out.resolved = true;
  out.path = path.toStdString();
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
  const std::string uri = "package://" + pkg + "/" + rel;
  if (std::string p = stepEmbeddedAsset(uri); !p.empty()) {
    return p;
  }
  // The rest of the chain finds packages on this host, except the URL ancestor walk.
  if (remote_only) {
    return source_is_url ? stepAncestor(pkg, rel, urdf_dir, source_is_url) : std::string{};
  }
  if (std::string p = stepRememberedMap(pkg, rel); !p.empty()) {
    return p;
  }
  if (std::string p = stepAncestor(pkg, rel, urdf_dir, source_is_url); !p.empty()) {
    return p;
  }
  if (std::string p = stepSearchRoots(pkg, rel); !p.empty()) {
    return p;
  }
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
  // On-disk filename = "<8-hex-sha1-of-key>_<flattened-ref>". The flatten alone is non-injective ("package://a/b.stl"
  // and "package://a_b.stl" flatten identically), so distinct refs would overwrite each other; the hash of the verbatim
  // key makes the name collision-proof while the flattened tail keeps the extension last for format sniffing. Path
  // traversal is already neutralized: '/' becomes '_'.
  QString flat = key;
  flat.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
  const QString digest =
      QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex()).left(8);
  const QString out_path = extracted_assets_dir_->filePath(digest + '_' + flat);
  // Asset content is immutable for the dir's lifetime (setEmbeddedAssets resets the dir), so an already-extracted file
  // is never rewritten: that would truncate bytes under an in-flight assimp read on Retry/Locate.
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
  const QString candidate = joinLocalPath(root, rel);
  if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
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
    // URL variant: take the rightmost path segment == pkg, rebuild the URL up to and including it, then append rel.
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

  QDir dir(QString::fromStdString(urdf_dir));
  for (int depth = 0; depth <= kAncestorDepthCap; ++depth) {
    if (dir.dirName() == qpkg) {
      const QString candidate = joinLocalPath(dir.absolutePath(), rel);
      if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
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
    const QString pkg_dir = joinLocalPath(root, pkg);
    // Package identity = directory basename only (no package.xml check). Accept only when the mesh file itself exists
    // (mirrors stepAncestor): a package-dir stub missing the mesh must miss (""), not yield a path the loader silently
    // fails to open.
    if (!pkg_dir.isEmpty() && QFileInfo(pkg_dir).isDir()) {
      const QString candidate = joinLocalPath(pkg_dir, rel);
      if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
        return candidate.toStdString();
      }
    }
  }
  const QString qpkg = QString::fromStdString(pkg);
  for (const QString& root : userFolders()) {
    for (const QString& pkg_dir : packageIndex(QDir::cleanPath(root)).value(qpkg)) {
      const QString candidate = joinLocalPath(pkg_dir, rel);
      if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
        return candidate.toStdString();
      }
    }
  }
  return {};
}

}  // namespace pj::scene3d
