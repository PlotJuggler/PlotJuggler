// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "urdf_package_resolver.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QSettings>
#include <QString>
#include <QTemporaryFile>
#include <QUrl>
#include <string>

using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {

#ifndef PJ_SCENE3D_FIXTURES_DIR
#error "PJ_SCENE3D_FIXTURES_DIR must be defined by the build"
#endif

std::string fixtures() {
  return std::string(PJ_SCENE3D_FIXTURES_DIR);
}

// A throwaway in-memory QSettings (Ini format, unique temp file per test).
class ScopedSettings {
 public:
  ScopedSettings() {
    // QTemporaryFile::open() is [[nodiscard]] as of Qt 6.11; check it (we build -Werror).
    EXPECT_TRUE(tmp_.open()) << "failed to create temp QSettings file";
    tmp_.close();
    settings_ = std::make_unique<QSettings>(tmp_.fileName(), QSettings::IniFormat);
  }
  QSettings* get() {
    return settings_.get();
  }

 private:
  QTemporaryFile tmp_;
  std::unique_ptr<QSettings> settings_;
};

// ---------------------------------------------------------------------------
// URI-scheme dispatch + the package:// guard
// ---------------------------------------------------------------------------

TEST(UrdfResolver, FileSchemeStrippedToAbsolutePath) {
  UrdfPackageResolver r;
  const ResolvedMesh m = r.resolveUri("file:///abs/path/mesh.stl", "/some/dir", false);
  EXPECT_TRUE(m.resolved);
  EXPECT_EQ(m.path, "/abs/path/mesh.stl");
  EXPECT_FALSE(m.is_url);
}

TEST(UrdfResolver, BarePathJoinsUrdfDirAndSkipsPackageSearch) {
  UrdfPackageResolver r;
  r.addSearchRoot(QString::fromStdString(fixtures() + "/search_root"));
  // This bare path's first segment matches a package dir in the search root,
  // but the guard must keep it OUT of the package search.
  const ResolvedMesh m = r.resolveUri("demo_description/meshes/x.stl", "/urdf/dir", false);
  EXPECT_TRUE(m.resolved);
  EXPECT_EQ(m.path, "/urdf/dir/demo_description/meshes/x.stl");
  // The chain never ran: no package recorded on the result.
  EXPECT_TRUE(m.package.empty());
  EXPECT_EQ(m.issue, MeshResolveIssue::kNone);
}

TEST(UrdfResolver, HttpBlockedForNonUrlSource) {
  UrdfPackageResolver r;
  const ResolvedMesh blocked = r.resolveUri("https://host/m.stl", "/dir", /*source_is_url=*/false);
  EXPECT_FALSE(blocked.resolved);
  EXPECT_EQ(blocked.issue, MeshResolveIssue::kBlockedHttp);
  EXPECT_TRUE(blocked.package.empty());  // not a package:// ref
  const ResolvedMesh allowed = r.resolveUri("https://host/m.stl", "https://host/", /*source_is_url=*/true);
  EXPECT_TRUE(allowed.resolved);
  EXPECT_TRUE(allowed.is_url);
  EXPECT_EQ(allowed.issue, MeshResolveIssue::kNone);
}

// An absolute filesystem path that exists is returned verbatim (NOT joined onto
// urdf_dir); a missing one is unresolved with kMissingFile.
TEST(UrdfResolver, AbsolutePathPassthroughAndMissing) {
  UrdfPackageResolver r;
  QTemporaryFile existing;
  ASSERT_TRUE(existing.open());
  existing.write("STL");
  existing.close();
  const std::string abs = existing.fileName().toStdString();
  const ResolvedMesh hit = r.resolveUri(abs, "/some/urdf/dir", /*source_is_url=*/false);
  EXPECT_TRUE(hit.resolved);
  EXPECT_EQ(hit.path, abs);  // verbatim, not "/some/urdf/dir" + abs
  EXPECT_FALSE(hit.is_url);
  EXPECT_EQ(hit.issue, MeshResolveIssue::kNone);

  const ResolvedMesh miss = r.resolveUri("/does/not/exist/arm.stl", "/some/urdf/dir", /*source_is_url=*/false);
  EXPECT_FALSE(miss.resolved);
  EXPECT_EQ(miss.issue, MeshResolveIssue::kMissingFile);
}

// ---------------------------------------------------------------------------
// Chain step 0 — embedded-asset exact-name lookup
// ---------------------------------------------------------------------------

TEST(UrdfResolver, Step0_AttachmentExactNameHit) {
  UrdfPackageResolver r;
  QMap<QString, QByteArray> att;
  att.insert("package://demo_description/meshes/base.dae", QByteArray("DAE-BYTES"));
  r.setEmbeddedAssets(att);

  const ResolvedMesh m = r.resolveUri("package://demo_description/meshes/base.dae", "", false);
  ASSERT_TRUE(m.resolved);
  // Extracted to a temp file; the bytes must round-trip.
  QFile f(QString::fromStdString(m.path));
  ASSERT_TRUE(f.open(QIODevice::ReadOnly));
  EXPECT_EQ(f.readAll(), QByteArray("DAE-BYTES"));
}

// ---------------------------------------------------------------------------
// Chain step 1 — remembered per-source map (QSettings)
// ---------------------------------------------------------------------------

TEST(UrdfResolver, Step1_RememberedPerSourceMapHit) {
  ScopedSettings s;
  UrdfPackageResolver r;
  r.setSettings(s.get());
  r.setSourcePath("/data/run42.dat");
  // rememberPackageRoot writes both the per-source map and the global roots.
  r.rememberPackageRoot("demo_description", QString::fromStdString(fixtures() + "/search_root/demo_description"));

  // A fresh resolver reading the same settings + source path resolves via step 1.
  UrdfPackageResolver r2;
  r2.setSettings(s.get());
  r2.setSourcePath("/data/run42.dat");
  const std::string p = r2.resolve("demo_description", "meshes/base.stl", "", false);
  EXPECT_EQ(p, fixtures() + "/search_root/demo_description/meshes/base.stl");
}

// ---------------------------------------------------------------------------
// Chain step 2 — ancestor heuristic (walk up urdf_dir for basename == pkg)
// ---------------------------------------------------------------------------

TEST(UrdfResolver, Step2_AncestorHeuristicHit) {
  UrdfPackageResolver r;
  // urdf_dir is inside ws/demo_description/ ; walking up finds the pkg-named
  // ancestor and verifies ancestor/rel exists.
  const std::string urdf_dir = fixtures() + "/ws/demo_description";
  const std::string p = r.resolve("demo_description", "meshes/anc.stl", urdf_dir, false);
  EXPECT_EQ(p, fixtures() + "/ws/demo_description/meshes/anc.stl");
}

TEST(UrdfResolver, Step2_AncestorRejectsWhenRelMissing) {
  UrdfPackageResolver r;
  const std::string urdf_dir = fixtures() + "/ws/demo_description";
  // The ancestor basename matches, but the rel file does not exist there → no
  // false-positive accept; falls through (and, with no roots, is unresolved).
  const std::string p = r.resolve("demo_description", "meshes/DOES_NOT_EXIST.stl", urdf_dir, false);
  EXPECT_TRUE(p.empty());
}

TEST(UrdfResolver, Step2_UrlAncestorHeuristic) {
  UrdfPackageResolver r;
  const std::string base = "https://example.com/repo/demo_description/urdf";
  const std::string p = r.resolve("demo_description", "meshes/base.dae", base, /*source_is_url=*/true);
  EXPECT_EQ(p, "https://example.com/repo/demo_description/meshes/base.dae");
}

// ---------------------------------------------------------------------------
// Chain step 3 — search roots (basename match, no package.xml)
// ---------------------------------------------------------------------------

TEST(UrdfResolver, Step3_SearchRootHit) {
  UrdfPackageResolver r;
  r.addSearchRoot(QString::fromStdString(fixtures() + "/search_root"));
  const std::string p = r.resolve("demo_description", "meshes/base.stl", "", false);
  EXPECT_EQ(p, fixtures() + "/search_root/demo_description/meshes/base.stl");
}

TEST(UrdfResolver, Step3_AutoSeedFromEnv) {
  // Two entries joined with the native list separator (';' on Windows, where
  // absolute paths contain ':' and a colon split would shear the drive letter).
  const char sep = QDir::listSeparator().toLatin1();
  const std::string env_value = fixtures() + "/no_such_root" + sep + fixtures() + "/search_root";
  qputenv("ROS_PACKAGE_PATH", QByteArray::fromStdString(env_value));
  UrdfPackageResolver r;
  r.autoSeedSearchRoots("");  // no urdf dir; env supplies the roots
  qunsetenv("ROS_PACKAGE_PATH");
  const std::string p = r.resolve("demo_description", "meshes/base.stl", "", false);
  EXPECT_EQ(p, fixtures() + "/search_root/demo_description/meshes/base.stl");
}

// ---------------------------------------------------------------------------
// Chain miss — unresolved package is reported on the result (not a global tally)
// ---------------------------------------------------------------------------

TEST(UrdfResolver, PackageMissReportedOnResult) {
  UrdfPackageResolver r;  // no assets, no settings, no roots
  // resolve() (the package chain) returns "" on a miss; resolveUri() surfaces
  // the offending package + reason in the return value (no resolver-global tally).
  const std::string p = r.resolve("ghost_pkg", "meshes/x.stl", "", false);
  EXPECT_TRUE(p.empty());
  const ResolvedMesh m = r.resolveUri("package://ghost_pkg/meshes/x.stl", "", false);
  EXPECT_FALSE(m.resolved);
  EXPECT_EQ(m.package, "ghost_pkg");
  EXPECT_EQ(m.issue, MeshResolveIssue::kUnresolvedPackage);
}

// A malformed package:// URI (no "/rel" part) is reported as kMalformedRef.
TEST(UrdfResolver, MalformedPackageUriReported) {
  UrdfPackageResolver r;
  const ResolvedMesh m = r.resolveUri("package://justpkg", "", false);
  EXPECT_FALSE(m.resolved);
  EXPECT_EQ(m.issue, MeshResolveIssue::kMalformedRef);
  EXPECT_TRUE(m.package.empty());
}

// ---------------------------------------------------------------------------
// Stop-at-first-hit ordering: attachment beats search root.
// ---------------------------------------------------------------------------

TEST(UrdfResolver, AttachmentTakesPriorityOverSearchRoot) {
  UrdfPackageResolver r;
  r.addSearchRoot(QString::fromStdString(fixtures() + "/search_root"));
  QMap<QString, QByteArray> att;
  att.insert("package://demo_description/meshes/base.stl", QByteArray("ATTACHED"));
  r.setEmbeddedAssets(att);

  const std::string p = r.resolve("demo_description", "meshes/base.stl", "", false);
  QFile f(QString::fromStdString(p));
  ASSERT_TRUE(f.open(QIODevice::ReadOnly));
  EXPECT_EQ(f.readAll(), QByteArray("ATTACHED"));  // not the on-disk search-root file
}

// rememberPackageRoot persists globally and makes the package resolvable.
TEST(UrdfResolver, RememberPersistsAndResolves) {
  ScopedSettings s;
  UrdfPackageResolver r;
  r.setSettings(s.get());
  r.setSourcePath("/data/x.dat");
  EXPECT_TRUE(r.resolve("demo_description", "meshes/base.stl", "", false).empty());

  r.rememberPackageRoot("demo_description", QString::fromStdString(fixtures() + "/search_root/demo_description"));
  // Now resolvable via the freshly-added global root.
  const std::string p = r.resolve("demo_description", "meshes/base.stl", "", false);
  EXPECT_EQ(p, fixtures() + "/search_root/demo_description/meshes/base.stl");
}

// rememberPackageRoot must enable cross-dataset (step-3) resolution: a FRESH
// resolver on a DIFFERENT source (so the per-source map, step 1, misses) resolves
// via the persisted GLOBAL search root. Regression guard for the
// parent-vs-package-dir double-nesting bug (which made step 3 silently miss).
TEST(UrdfResolver, RememberEnablesCrossDatasetSearchRoot) {
  ScopedSettings s;
  UrdfPackageResolver r1;
  r1.setSettings(s.get());
  r1.setSourcePath("/data/first.dat");
  r1.rememberPackageRoot("demo_description", QString::fromStdString(fixtures() + "/search_root/demo_description"));

  UrdfPackageResolver r2;
  r2.setSettings(s.get());
  r2.setSourcePath("/data/second.dat");  // different source → step 1 cannot hit
  r2.autoSeedSearchRoots("");            // loads the persisted global root (the PARENT dir)
  const std::string p = r2.resolve("demo_description", "meshes/base.stl", "", false);
  EXPECT_EQ(p, fixtures() + "/search_root/demo_description/meshes/base.stl");
}

// ---------------------------------------------------------------------------
// Attachment extraction: collision-proof on-disk names + immutable rewrite-skip
// ---------------------------------------------------------------------------

// Two well-formed refs that flatten to the SAME name ("package://pkg/a/b.stl" vs
// "package://pkg/a_b.stl" both -> "package___pkg_a_b.stl") must extract to
// DISTINCT files with their respective bytes — the hash prefix disambiguates
// them.
TEST(UrdfResolver, CollidingAttachmentRefsExtractToDistinctFiles) {
  UrdfPackageResolver r;
  QMap<QString, QByteArray> att;
  att.insert("package://pkg/a/b.stl", QByteArray("FIRST"));
  att.insert("package://pkg/a_b.stl", QByteArray("SECOND"));
  r.setEmbeddedAssets(att);

  const ResolvedMesh first = r.resolveUri("package://pkg/a/b.stl", "", false);
  const ResolvedMesh second = r.resolveUri("package://pkg/a_b.stl", "", false);
  ASSERT_TRUE(first.resolved);
  ASSERT_TRUE(second.resolved);
  EXPECT_NE(first.path, second.path);  // distinct on-disk files, no overwrite

  QFile f1(QString::fromStdString(first.path));
  ASSERT_TRUE(f1.open(QIODevice::ReadOnly));
  EXPECT_EQ(f1.readAll(), QByteArray("FIRST"));
  QFile f2(QString::fromStdString(second.path));
  ASSERT_TRUE(f2.open(QIODevice::ReadOnly));
  EXPECT_EQ(f2.readAll(), QByteArray("SECOND"));
}

// A second resolve of the same attachment returns the same path WITHOUT
// rewriting the file (immutable content) — guards the truncation-under-assimp
// race. We overwrite the extracted bytes out-of-band, resolve
// again, and assert the resolver left our bytes intact (it did not reopen
// WriteOnly).
TEST(UrdfResolver, AttachmentReResolveDoesNotRewrite) {
  UrdfPackageResolver r;
  QMap<QString, QByteArray> att;
  att.insert("package://demo/mesh.stl", QByteArray("ORIGINAL"));
  r.setEmbeddedAssets(att);

  const ResolvedMesh first = r.resolveUri("package://demo/mesh.stl", "", false);
  ASSERT_TRUE(first.resolved);
  // Tamper with the extracted file out-of-band.
  {
    QFile tamper(QString::fromStdString(first.path));
    ASSERT_TRUE(tamper.open(QIODevice::WriteOnly | QIODevice::Truncate));
    tamper.write("TAMPERED");
    tamper.close();
  }
  const ResolvedMesh second = r.resolveUri("package://demo/mesh.stl", "", false);
  ASSERT_TRUE(second.resolved);
  EXPECT_EQ(second.path, first.path);
  // The resolver must NOT have rewritten the (immutable) file with attachment
  // bytes — our tampered content is still there.
  QFile check(QString::fromStdString(second.path));
  ASSERT_TRUE(check.open(QIODevice::ReadOnly));
  EXPECT_EQ(check.readAll(), QByteArray("TAMPERED"));
}

// setEmbeddedAssets replaces the map AND resets the extraction dir, so a stale
// file from the previous map cannot be served for a new ref.
TEST(UrdfResolver, SetAttachmentsResetsExtractionDir) {
  UrdfPackageResolver r;
  QMap<QString, QByteArray> first_map;
  first_map.insert("package://demo/mesh.stl", QByteArray("OLD"));
  r.setEmbeddedAssets(first_map);
  const ResolvedMesh old = r.resolveUri("package://demo/mesh.stl", "", false);
  ASSERT_TRUE(old.resolved);

  QMap<QString, QByteArray> second_map;
  second_map.insert("package://demo/mesh.stl", QByteArray("NEW"));
  r.setEmbeddedAssets(second_map);
  const ResolvedMesh fresh = r.resolveUri("package://demo/mesh.stl", "", false);
  ASSERT_TRUE(fresh.resolved);
  QFile f(QString::fromStdString(fresh.path));
  ASSERT_TRUE(f.open(QIODevice::ReadOnly));
  EXPECT_EQ(f.readAll(), QByteArray("NEW"));
}

// ---------------------------------------------------------------------------
// Explicit per-model package URL roots
// ---------------------------------------------------------------------------

// The dock shares one resolver across its robots, so each model's mapping
// travels with the call: two models naming the same package resolve to their
// own roots, and a model without a mapping keeps the legacy chain. An HTTP root
// works for a topic source of a web-hosted layout, yet the topic's own http refs
// stay blocked.
TEST(UrdfResolver, TwoModelsKeepIndependentMappings) {
  UrdfPackageResolver shared;
  shared.addSearchRoot(QString::fromStdString(fixtures() + "/search_root"));
  const QMap<QString, QUrl> first_roots{{u"demo_description"_s, QUrl(u"http://first.test/assets/demo_description/"_s)}};
  // No trailing slash: the root still names a directory.
  const QMap<QString, QUrl> second_roots{{u"demo_description"_s, QUrl(u"https://second.test/v2/pkg"_s)}};

  // Network roots of a topic URDF count only in a web-hosted layout (remote_only).
  const ResolvedMesh first = shared.resolveUri(
      "package://demo_description/meshes/base.stl", "", /*source_is_url=*/false, first_roots, /*remote_only=*/true);
  ASSERT_TRUE(first.resolved);
  EXPECT_TRUE(first.is_url);
  EXPECT_EQ(first.path, "http://first.test/assets/demo_description/meshes/base.stl");

  const ResolvedMesh second = shared.resolveUri(
      "package://demo_description/meshes/base.stl", "", /*source_is_url=*/false, second_roots, /*remote_only=*/true);
  ASSERT_TRUE(second.resolved);
  EXPECT_EQ(second.path, "https://second.test/v2/pkg/meshes/base.stl");

  const ResolvedMesh legacy = shared.resolveUri("package://demo_description/meshes/base.stl", "", false);
  ASSERT_TRUE(legacy.resolved);
  EXPECT_FALSE(legacy.is_url);
  EXPECT_EQ(legacy.path, fixtures() + "/search_root/demo_description/meshes/base.stl");

  // The mapping is authoritative for its package and authorizes nothing else.
  const ResolvedMesh unmapped = shared.resolveUri("package://ghost_pkg/meshes/x.stl", "", false, first_roots, true);
  EXPECT_EQ(unmapped.issue, MeshResolveIssue::kUnresolvedPackage);
  const ResolvedMesh direct =
      shared.resolveUri("http://first.test/assets/demo_description/meshes/base.stl", "", false, first_roots, true);
  EXPECT_EQ(direct.issue, MeshResolveIssue::kBlockedHttp);
  // A reference that climbs out of the mapped root is not a package fetch.
  const ResolvedMesh escaped =
      shared.resolveUri("package://demo_description/../../private/key.stl", "", false, first_roots, true);
  EXPECT_FALSE(escaped.resolved);
  EXPECT_EQ(escaped.package, "demo_description");
}

#ifndef PJ_TARGET_WASM
// On the desktop a layout file cannot point a topic or file URDF's meshes at the network: its
// network root is skipped and the package resolves on this computer. A URL URDF, already fetched
// over the network, and a web-hosted layout (remote_only) keep it.
TEST(UrdfResolver, DesktopNetworkRootsNeedAUrlSourceOrAWebLayout) {
  const QMap<QString, QUrl> roots{{u"demo_description"_s, QUrl(u"https://cdn.test/demo_description/"_s)}};
  const std::string uri = "package://demo_description/meshes/base.stl";
  UrdfPackageResolver local;
  local.addSearchRoot(QString::fromStdString(fixtures() + "/search_root"));

  const ResolvedMesh topic = local.resolveUri(uri, "", /*source_is_url=*/false, roots);
  ASSERT_TRUE(topic.resolved);
  EXPECT_FALSE(topic.is_url);
  EXPECT_EQ(topic.path, fixtures() + "/search_root/demo_description/meshes/base.stl");

  UrdfPackageResolver bare;
  const ResolvedMesh missing = bare.resolveUri(uri, "", /*source_is_url=*/false, roots);
  EXPECT_EQ(missing.issue, MeshResolveIssue::kUnresolvedPackage);
  EXPECT_EQ(missing.package, "demo_description");

  for (const auto& [source_is_url, remote_only] : {std::pair{true, false}, std::pair{false, true}}) {
    const ResolvedMesh network =
        bare.resolveUri(uri, "https://host.test/robot/urdf", source_is_url, roots, remote_only);
    ASSERT_TRUE(network.resolved) << source_is_url << remote_only;
    EXPECT_TRUE(network.is_url);
    EXPECT_EQ(network.path, "https://cdn.test/demo_description/meshes/base.stl");
  }
}
#endif

// An encoded separator or dot segment, or a backslash, cannot climb out of a mapped root either:
// file roots resolve to a local path the filesystem would decode and normalize itself.
TEST(UrdfResolver, EncodedTraversalStaysInsideTheMappedRoot) {
  UrdfPackageResolver resolver;
  const QMap<QString, QUrl> file_roots{{u"pkg"_s, QUrl(u"file:///allowed/pkg/"_s)}};
  const QMap<QString, QUrl> http_roots{{u"pkg"_s, QUrl(u"https://host.test/assets/pkg/"_s)}};
  for (const char* rel : {
           "meshes/%2F..%2F..%2Fprivate.stl",
           "meshes/%2f..%2f..%2fprivate.stl",
           "meshes/..%5C..%5Cprivate.stl",
           "meshes\\..\\..\\private.stl",
           "meshes/%2E%2E/%2E%2E/private.stl",
           "%2e%2e/private.stl",
       }) {
    const std::string uri = std::string("package://pkg/") + rel;
    for (const QMap<QString, QUrl>* roots : {&file_roots, &http_roots}) {
      const ResolvedMesh mesh = resolver.resolveUri(uri, "", false, *roots);
      EXPECT_FALSE(mesh.resolved) << rel << " -> " << mesh.path;
      EXPECT_EQ(mesh.issue, MeshResolveIssue::kUnresolvedPackage) << rel;
    }
  }
  // An ordinary encoded character is still fine.
  const ResolvedMesh spaced = resolver.resolveUri("package://pkg/meshes/my%20part.stl", "", false, file_roots);
  ASSERT_TRUE(spaced.resolved);
  EXPECT_EQ(spaced.path, "/allowed/pkg/meshes/my part.stl");
}

// A web-hosted layout's robot names only network resources and its own embedded assets: even on
// the desktop, local files and the local package chain (search roots, remembered roots, the
// filesystem ancestor walk) stay out of reach, whatever the URDF itself names.
TEST(UrdfResolver, RemoteOnlyRefusesLocalResources) {
  ScopedSettings settings;
  UrdfPackageResolver resolver;
  resolver.setSettings(settings.get());
  resolver.addSearchRoot(QString::fromStdString(fixtures() + "/search_root"));
  const std::string local_mesh = fixtures() + "/search_root/demo_description/meshes/base.stl";
  const std::string local_package = "package://demo_description/meshes/base.stl";
  const std::string urdf_dir = "https://host.test/presets/robot/";

  // Without the policy, the same references reach the local files.
  EXPECT_TRUE(resolver.resolveUri("file://" + local_mesh, urdf_dir, true).resolved);
  EXPECT_TRUE(resolver.resolveUri(local_mesh, urdf_dir, true).resolved);
  EXPECT_TRUE(resolver.resolveUri(local_package, urdf_dir, true).resolved);

  for (const std::string& uri : {"file://" + local_mesh, local_mesh, local_package}) {
    const ResolvedMesh blocked = resolver.resolveUri(uri, urdf_dir, true, {}, /*remote_only=*/true);
    EXPECT_FALSE(blocked.resolved) << uri << " -> " << blocked.path;
  }
  const QMap<QString, QUrl> file_root{
      {u"demo_description"_s,
       QUrl::fromLocalFile(QString::fromStdString(fixtures() + "/search_root/demo_description/"))}};
  EXPECT_FALSE(resolver.resolveUri(local_package, urdf_dir, true, file_root, true).resolved);
  // A topic URDF (no directory) has nothing local to fall back to either.
  EXPECT_FALSE(resolver.resolveUri(local_package, "", false, {}, true).resolved);

  // Network references still resolve: direct, relative to the URDF, under an http(s) root.
  const ResolvedMesh direct = resolver.resolveUri("https://cdn.test/m.stl", urdf_dir, true, {}, true);
  EXPECT_TRUE(direct.resolved && direct.is_url);
  const ResolvedMesh relative = resolver.resolveUri("meshes/base.stl", urdf_dir, true, {}, true);
  EXPECT_TRUE(relative.resolved && relative.is_url) << relative.path;
  const QMap<QString, QUrl> http_root{{u"demo_description"_s, QUrl(u"https://cdn.test/demo_description/"_s)}};
  const ResolvedMesh mapped = resolver.resolveUri(local_package, urdf_dir, true, http_root, true);
  ASSERT_TRUE(mapped.resolved);
  EXPECT_EQ(mapped.path, "https://cdn.test/demo_description/meshes/base.stl");
}

}  // namespace
}  // namespace pj::scene3d
