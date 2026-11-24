// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "mesh_loader.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <string>
#include <unordered_set>
#include <vector>

#include "mesh_import.h"
#include "mock_parser_support.h"
using namespace Qt::StringLiterals;
using pj::scene3d::test::readFixtureBytes;

namespace pj::scene3d {
namespace {

#ifndef PJ_SCENE3D_FIXTURES_DIR
#error "PJ_SCENE3D_FIXTURES_DIR must be defined by the build"
#endif

QString fixturePath(const char* name) {
  return QString(PJ_SCENE3D_FIXTURES_DIR) + "/meshes/" + QLatin1String(name);
}

void expectPopulatedNormals(const MeshData& mesh) {
  ASSERT_FALSE(mesh.vertices.empty());
  bool any_nonzero = false;
  for (const auto& v : mesh.vertices) {
    const float len = std::sqrt(v.normal.x * v.normal.x + v.normal.y * v.normal.y + v.normal.z * v.normal.z);
    EXPECT_GT(len, 0.5f) << "degenerate normal";
    if (len > 0.5f) {
      any_nonzero = true;
    }
  }
  EXPECT_TRUE(any_nonzero);
}

struct Bounds {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  glm::vec3 extent() const {
    return max - min;
  }
};

Bounds computeBounds(const MeshData& mesh) {
  Bounds b;
  b.min = glm::vec3(1e9f);
  b.max = glm::vec3(-1e9f);
  for (const auto& v : mesh.vertices) {
    b.min = glm::min(b.min, v.position);
    b.max = glm::max(b.max, v.position);
  }
  return b;
}

void expectValidIndices(const MeshData& mesh) {
  ASSERT_FALSE(mesh.indices.empty());
  EXPECT_EQ(mesh.indices.size() % 3, 0u);
  for (auto idx : mesh.indices) {
    EXPECT_LT(idx, mesh.vertices.size());
  }

  ASSERT_FALSE(mesh.submeshes.empty());
  std::vector<bool> covered(mesh.indices.size(), false);
  for (const SubMesh& submesh : mesh.submeshes) {
    // Loader invariant: every emitted submesh carries a material (a neutral fallback when the source has none).
    ASSERT_NE(submesh.material, nullptr);
    EXPECT_EQ(submesh.index_count % 3, 0u);
    ASSERT_LE(submesh.index_offset, mesh.indices.size());
    ASSERT_LE(submesh.index_count, mesh.indices.size() - submesh.index_offset);
    for (std::size_t i = submesh.index_offset; i < submesh.index_offset + submesh.index_count; ++i) {
      covered[i] = true;
    }
  }
  for (bool is_covered : covered) {
    EXPECT_TRUE(is_covered);
  }
}

TEST(MeshLoaderTest, LoadsStlFromPath) {
  MeshLoader loader;
  QFuture<MeshData> future = loader.load(fixturePath("cube.stl"));
  const MeshData mesh = future.result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  EXPECT_FALSE(mesh.vertices.empty());
  expectValidIndices(mesh);
  expectPopulatedNormals(mesh);
  EXPECT_EQ(mesh.indices.size(), 36u);
}

// Proves the assimp Conan `with_collada=True` option name: a wrong name silently disables the importer and returns
// empty geometry.

// MeshLoader caches a path's future and hands it to every caller: taking the
// result for one import must leave it intact for the next.
TEST(MeshLoaderTest, CachedLoadServesEveryImport) {
  MeshLoader loader;
  QObject context;
  const QString path = fixturePath("cube.stl");
  const auto import_once = [&]() -> MeshData {
    bool done = false;
    MeshImport job(loader.load(path), &context, [&done] { done = true; });
    // processEvents() returns at once when idle: bound the wait by time, since
    // the import runs on a worker (slow under the sanitizers).
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 10000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      QThread::msleep(1);
    }
    EXPECT_TRUE(done);
    return job.take();
  };
  EXPECT_TRUE(import_once().ok);
  ASSERT_TRUE(loader.load(path).isValid()) << "the first import consumed the cached result";
  EXPECT_TRUE(import_once().ok);
}

TEST(MeshLoaderTest, LoadsColladaFromPath) {
  MeshLoader loader;
  QFuture<MeshData> future = loader.load(fixturePath("cube.dae"));
  const MeshData mesh = future.result();
  ASSERT_TRUE(mesh.ok) << "DAE import returned empty — check assimp with_collada option: " << mesh.error.toStdString();
  EXPECT_FALSE(mesh.vertices.empty());
  expectValidIndices(mesh);
  expectPopulatedNormals(mesh);
  EXPECT_EQ(mesh.indices.size(), 36u);
}

// assimp normalizes a <up_axis>Z_UP</up_axis> file to its internal Y-up via the root-node transform;
// wantsZUpFlip("dae") then re-applies a +90deg X rotation to land Z-up in our world. zup_marker.dae is an ASYMMETRIC
// spike (a symmetric cube cannot reveal it): apex authored at (0,0,1), base in z=0 extended slightly along +Y (0.1).
// The values below are the empirically verified Z-up result: a dropped or doubled flip fails here.

TEST(MeshLoaderTest, ColladaZUpFixtureLoadsZUp) {
  MeshLoader loader;
  const MeshData mesh = loader.load(fixturePath("zup_marker.dae")).result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  expectValidIndices(mesh);

  const Bounds b = computeBounds(mesh);
  // The apex sits near z=+1 and the +Y base feature is small (~0.1): after the flip z-extent (~1.0) must dominate y.
  EXPECT_GT(b.extent().z, 0.9f) << "Z-up apex collapsed — flip missing/wrong";
  EXPECT_LT(b.extent().y, 0.3f) << "apex leaked into +Y — geometry still Y-up";
  EXPECT_GT(b.extent().z, b.extent().y) << "max-z extent must dominate max-y extent for Z-up";
  EXPECT_NEAR(b.max.z, 1.0f, 1e-3f) << "apex should land at z=+1";

  const Vertex* apex = &mesh.vertices.front();
  for (const auto& v : mesh.vertices) {
    if (v.position.z > apex->position.z) {
      apex = &v;
    }
  }
  EXPECT_NEAR(apex->position.x, 0.0f, 1e-3f);
  EXPECT_NEAR(apex->position.y, 0.0f, 1e-3f);
  EXPECT_NEAR(apex->position.z, 1.0f, 1e-3f);
}

TEST(MeshLoaderTest, ColladaExtractsUvsAndResolvesTexturePath) {
  MeshLoader loader;
  const MeshData mesh = loader.load(fixturePath("textured_quad.dae")).result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  expectValidIndices(mesh);

  bool has_nonzero_uv = false;
  for (const Vertex& vertex : mesh.vertices) {
    if (std::abs(vertex.uv.x) > 1e-6f || std::abs(vertex.uv.y) > 1e-6f) {
      has_nonzero_uv = true;
      break;
    }
  }
  EXPECT_TRUE(has_nonzero_uv);

  ASSERT_FALSE(mesh.submeshes.empty());
  const TextureSource& base = mesh.submeshes.front().material->base_color;
  const QString texture_path = base.path;
  const QFileInfo texture_info(texture_path);
  EXPECT_TRUE(base.bytes.empty()) << "external .dae texture must resolve to a path, not inline bytes";
  EXPECT_TRUE(texture_info.isAbsolute()) << texture_path.toStdString();
  EXPECT_TRUE(texture_path.endsWith(QLatin1String("red_4x4.png"))) << texture_path.toStdString();
  EXPECT_TRUE(texture_info.isFile()) << texture_path.toStdString();
}

// In memory there is no directory to resolve the texture against: the reference
// is kept for a caller that knows where the mesh came from.
TEST(MeshLoaderTest, ColladaFromMemoryKeepsTheUnresolvedTextureReference) {
  MeshLoader loader;
  const MeshData mesh = loader.loadFromMemory(readFixtureBytes("textured_quad.dae"), u"dae"_s).result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  ASSERT_FALSE(mesh.submeshes.empty());
  const TextureSource& base = mesh.submeshes.front().material->base_color;
  EXPECT_TRUE(base.empty()) << "no pixels until the reference is fetched";
  EXPECT_EQ(base.unresolved_ref.toStdString(), "red_4x4.png");
}

// An in-memory import never turns a reference into a local file path, absolute
// ones included: the bytes may come from a remote host.
TEST(MeshLoaderTest, ColladaFromMemoryNeverResolvesAbsoluteTextureRefToLocalFile) {
  const QString absolute_texture = fixturePath("red_4x4.png");
  ASSERT_TRUE(QFileInfo(absolute_texture).isFile());
  for (const QString& ref : {absolute_texture, u"file://"_s + absolute_texture}) {
    SCOPED_TRACE(ref.toStdString());
    QByteArray bytes = readFixtureBytes("textured_quad.dae");
    bytes.replace("red_4x4.png", ref.toUtf8());
    MeshLoader loader;
    const MeshData mesh = loader.loadFromMemory(bytes, u"dae"_s).result();
    ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
    ASSERT_FALSE(mesh.submeshes.empty());
    const TextureSource& base = mesh.submeshes.front().material->base_color;
    EXPECT_TRUE(base.path.isEmpty()) << base.path.toStdString();
    EXPECT_TRUE(base.empty());
    EXPECT_EQ(base.unresolved_ref.toStdString(), absolute_texture.toStdString());
  }
}

// A texture ref naming a network share ("//host/share/..."; SMB on Windows) is never opened. Linux reads "//tmp/x" as
// "/tmp/x", so an existing file proves the ref is refused, not merely missing.
TEST(MeshLoaderTest, NetworkShareTextureRefIsNeverOpened) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  ASSERT_TRUE(QFile::copy(fixturePath("red_4x4.png"), dir.filePath(u"red_4x4.png"_s)));
  const QString mesh_path = pj::scene3d::test::writeQuadReferencing(dir, u"/"_s + dir.filePath(u"red_4x4.png"_s));
  MeshLoader loader;
  const MeshData mesh = loader.load(mesh_path).result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  ASSERT_FALSE(mesh.submeshes.empty());
  EXPECT_TRUE(mesh.submeshes.front().material->base_color.empty())
      << mesh.submeshes.front().material->base_color.path.toStdString();
}

// An in-memory model has no folder of its own: a glTF buffer uri naming a file on this host (an existing one here) is
// never opened, so the import fails instead of reading it. The same model with an embedded buffer imports.
TEST(MeshLoaderTest, InMemoryGltfNeverOpensAReferencedFile) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::array<float, 9> positions{0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
  const QByteArray buffer(reinterpret_cast<const char*>(positions.data()), sizeof(positions));
  QFile bin(dir.filePath(u"triangle.bin"_s));
  ASSERT_TRUE(bin.open(QIODevice::WriteOnly) && bin.write(buffer) == buffer.size());
  bin.close();
  const auto gltf = [](const QString& uri) {
    return uR"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"buffers":[{"uri":"%1","byteLength":36}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}]})"_s.arg(uri)
        .toUtf8();
  };
  MeshLoader loader;
  const MeshData embedded =
      loader
          .loadFromMemory(
              gltf(u"data:application/octet-stream;base64,"_s + QString::fromLatin1(buffer.toBase64())), u"gltf"_s)
          .result();
  ASSERT_TRUE(embedded.ok) << embedded.error.toStdString();
  const MeshData referenced = loader.loadFromMemory(gltf(bin.fileName()), u"gltf"_s).result();
  EXPECT_FALSE(referenced.ok) << "an in-memory glTF read " << bin.fileName().toStdString();
}

// loadFromMemory feeds the format hint so assimp picks the importer for a headerless buffer: Collada bytes here,
// binary glTF (.glb, the embedded-asset format) below.

TEST(MeshLoaderTest, LoadsFromMemory) {
  MeshLoader loader;
  const QByteArray bytes = readFixtureBytes("cube.dae");
  ASSERT_FALSE(bytes.isEmpty());
  QFuture<MeshData> future = loader.loadFromMemory(bytes, u"dae"_s);
  const MeshData mesh = future.result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  expectValidIndices(mesh);
  expectPopulatedNormals(mesh);
}

// The glTF importer in memory: the Y->Z flip path is only reachable via glTF. zup_marker.glb is a hand-crafted
// single-triangle GLB; loading via the "glb" hint must yield non-empty geometry, which loadFromMemory flips to Z-up.

TEST(MeshLoaderTest, LoadsGlbFromMemory) {
  MeshLoader loader;
  const QByteArray bytes = readFixtureBytes("zup_marker.glb");
  ASSERT_FALSE(bytes.isEmpty());
  QFuture<MeshData> future = loader.loadFromMemory(bytes, u"glb"_s);
  const MeshData mesh = future.result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  EXPECT_FALSE(mesh.vertices.empty());
  expectValidIndices(mesh);
  expectPopulatedNormals(mesh);
  EXPECT_EQ(mesh.indices.size(), 3u);
  // Authored Y-up apex at (0,1,0); loadFromMemory("glb") flips it to Z-up at (0,0,1).
  const Bounds b = computeBounds(mesh);
  EXPECT_GT(b.extent().z, b.extent().y) << "glb apex still Y-up — flip not applied";
  EXPECT_NEAR(b.max.z, 1.0f, 1e-3f) << "glb apex should land at z=+1 after flip";
}

// embedded_base.glb is a hand-built textured quad whose base-color image is an embedded bufferView PNG and whose
// material sets metallic/roughness/emissive factors (the embedded-asset SceneEntity case): the loader must extract the
// image into inline bytes (not a path) and read the PBR factors, rather than dropping the texture as "*N".

TEST(MeshLoaderTest, EmbeddedGlbExtractsBaseColorTextureAndPbrFactors) {
  MeshLoader loader;
  const QByteArray bytes = readFixtureBytes("embedded_base.glb");
  ASSERT_FALSE(bytes.isEmpty());
  const MeshData mesh = loader.loadFromMemory(bytes, u"glb"_s).result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  ASSERT_FALSE(mesh.submeshes.empty());

  const Material& material = *mesh.submeshes.front().material;
  // The embedded base-color image arrives as inline bytes (still PNG-encoded), keyed by content hash.
  EXPECT_FALSE(material.base_color.empty());
  EXPECT_FALSE(material.base_color.bytes.empty()) << "embedded base-color texture was dropped";
  EXPECT_TRUE(material.base_color.path.isEmpty());
  EXPECT_TRUE(material.base_color.key.rfind("emb:", 0) == 0) << material.base_color.key;

  EXPECT_TRUE(material.has_pbr);
  EXPECT_NEAR(material.metallic_factor, 0.25f, 1e-3f);
  EXPECT_NEAR(material.roughness_factor, 0.75f, 1e-3f);
  EXPECT_NEAR(material.emissive_factor.x, 0.10f, 1e-3f);
  EXPECT_NEAR(material.emissive_factor.y, 0.20f, 1e-3f);
  EXPECT_NEAR(material.emissive_factor.z, 0.30f, 1e-3f);
  EXPECT_EQ(material.alpha_mode, AlphaMode::kOpaque);

  EXPECT_TRUE(material.metallic_roughness.empty());
  EXPECT_TRUE(material.normal.empty());
  EXPECT_TRUE(material.emissive.empty());
}

// The embedded-PBR fixture carries all five glTF metallic-roughness material maps in one GLB. Each must remain an
// inline encoded capability with a stable content key so the renderer can apply its slot-specific color space without
// reopening a path.
TEST(MeshLoaderTest, EmbeddedPbrGlbExtractsAllFiveMaterialMaps) {
  MeshLoader loader;
  const QByteArray bytes = readFixtureBytes("embedded_pbr.glb");
  ASSERT_FALSE(bytes.isEmpty());
  const MeshData mesh = loader.loadFromMemory(bytes, u"glb"_s).result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  ASSERT_EQ(mesh.submeshes.size(), 1U);
  ASSERT_NE(mesh.submeshes.front().material, nullptr);

  const Material& material = *mesh.submeshes.front().material;
  const std::array<const TextureSource*, 5> maps{
      &material.base_color, &material.metallic_roughness, &material.normal, &material.occlusion, &material.emissive};
  std::unordered_set<std::string> keys;
  for (const TextureSource* map : maps) {
    ASSERT_NE(map, nullptr);
    EXPECT_FALSE(map->empty());
    EXPECT_TRUE(map->path.isEmpty());
    EXPECT_FALSE(map->bytes.empty());
    EXPECT_TRUE(map->key.rfind("emb:", 0) == 0) << map->key;
    keys.insert(map->key);
  }
  EXPECT_EQ(keys.size(), maps.size()) << "the fixture's five distinct images must retain distinct cache keys";
  EXPECT_TRUE(material.has_pbr);
  EXPECT_NEAR(material.metallic_factor, 0.65F, 1e-3F);
  EXPECT_NEAR(material.roughness_factor, 0.55F, 1e-3F);
  EXPECT_NEAR(material.emissive_factor.x, 0.0F, 1e-3F);
  EXPECT_NEAR(material.emissive_factor.y, 1.0F, 1e-3F);
  EXPECT_NEAR(material.emissive_factor.z, 1.0F, 1e-3F);
  for (const Vertex& vertex : mesh.vertices) {
    const glm::vec3 tangent(vertex.tangent);
    EXPECT_NEAR(glm::length(tangent), 1.0F, 1e-3F);
    EXPECT_NEAR(glm::dot(glm::normalize(vertex.normal), tangent), 0.0F, 1e-3F);
    EXPECT_TRUE(std::abs(vertex.tangent.w - 1.0F) < 1e-3F || std::abs(vertex.tangent.w + 1.0F) < 1e-3F);
  }
}

TEST(MeshLoaderTest, EmbeddedNeutralGlbIsAMapFreeOpaqueControl) {
  MeshLoader loader;
  const QByteArray bytes = readFixtureBytes("embedded_neutral.glb");
  ASSERT_FALSE(bytes.isEmpty());
  const MeshData mesh = loader.loadFromMemory(bytes, u"glb"_s).result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  ASSERT_EQ(mesh.submeshes.size(), 1U);
  ASSERT_NE(mesh.submeshes.front().material, nullptr);

  const Material& material = *mesh.submeshes.front().material;
  EXPECT_TRUE(material.has_pbr);
  EXPECT_TRUE(material.base_color.empty());
  EXPECT_TRUE(material.metallic_roughness.empty());
  EXPECT_TRUE(material.normal.empty());
  EXPECT_TRUE(material.occlusion.empty());
  EXPECT_TRUE(material.emissive.empty());
  EXPECT_EQ(material.alpha_mode, AlphaMode::kOpaque);
  EXPECT_NEAR(material.base_color_factor.r, 0.65F, 1e-3F);
  EXPECT_NEAR(material.base_color_factor.g, 0.65F, 1e-3F);
  EXPECT_NEAR(material.base_color_factor.b, 0.65F, 1e-3F);
  EXPECT_NEAR(material.base_color_factor.a, 1.0F, 1e-3F);
  EXPECT_NEAR(material.metallic_factor, 0.0F, 1e-3F);
  EXPECT_NEAR(material.roughness_factor, 1.0F, 1e-3F);
  EXPECT_EQ(material.emissive_factor, glm::vec3(0.0F));
}

// The embedded-texture cache key is a content hash, so re-loading the same bytes yields the same key: the renderer's
// per-context texture cache survives a QRhi recreation and dedups identical embedded images.
TEST(MeshLoaderTest, EmbeddedTextureKeyIsStableAcrossLoads) {
  MeshLoader loader;
  const QByteArray bytes = readFixtureBytes("embedded_base.glb");
  ASSERT_FALSE(bytes.isEmpty());
  const MeshData a = loader.loadFromMemory(bytes, u"glb"_s).result();
  const MeshData b = loader.loadFromMemory(bytes, u"glb"_s).result();
  ASSERT_TRUE(a.ok && b.ok);
  ASSERT_FALSE(a.submeshes.empty());
  ASSERT_FALSE(b.submeshes.empty());
  EXPECT_EQ(a.submeshes.front().material->base_color.key, b.submeshes.front().material->base_color.key);
  EXPECT_FALSE(a.submeshes.front().material->base_color.key.empty());
}

// aiProcess_CalcTangentSpace must populate per-vertex tangents for normal mapping. embedded_base.glb's UVs map U->+Y,
// so the computed tangent is unambiguously not the (1,0,0) default; after the glb Y->Z flip it lands at (0,0,1). Each
// tangent must be unit length, orthogonal to the normal, and carry a +/-1 handedness.
TEST(MeshLoaderTest, GeneratesTangentBasis) {
  MeshLoader loader;
  const QByteArray bytes = readFixtureBytes("embedded_base.glb");
  ASSERT_FALSE(bytes.isEmpty());
  const MeshData mesh = loader.loadFromMemory(bytes, u"glb"_s).result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  ASSERT_FALSE(mesh.vertices.empty());

  for (const Vertex& v : mesh.vertices) {
    const glm::vec3 t(v.tangent);
    EXPECT_NEAR(glm::length(t), 1.0f, 1e-3f) << "tangent must be unit length";
    EXPECT_NEAR(glm::dot(glm::normalize(v.normal), t), 0.0f, 1e-3f) << "tangent must be orthogonal to normal";
    EXPECT_TRUE(std::abs(v.tangent.w - 1.0f) < 1e-3f || std::abs(v.tangent.w + 1.0f) < 1e-3f)
        << "handedness must be +/-1, was " << v.tangent.w;
    EXPECT_GT(glm::length(t - glm::vec3(1.0f, 0.0f, 0.0f)), 0.5f) << "tangent looks like the unset default";
  }
}

// A cache HIT means the second load() must not re-run the importer. Equal counts would also pass after a deterministic
// re-import, so identity is checked: load() returns the cached QFuture by value and copies of one future share the
// same result storage, so a hit's resultPointer aliases the same MeshData while a miss allocates a distinct future.

TEST(MeshLoaderTest, CachesByResolvedPath) {
  MeshLoader loader;
  QFuture<MeshData> first = loader.load(fixturePath("cube.stl"));
  QFuture<MeshData> second = loader.load(fixturePath("cube.stl"));
  ASSERT_TRUE(first.result().ok);
  ASSERT_TRUE(second.result().ok);

  const MeshData* first_ptr = &(*first.constBegin());
  const MeshData* second_ptr = &(*second.constBegin());
  EXPECT_EQ(first_ptr, second_ptr) << "second load() re-imported instead of hitting the cache";

  QFuture<MeshData> other = loader.load(fixturePath("cube.dae"));
  ASSERT_TRUE(other.result().ok);
  EXPECT_NE(&(*other.constBegin()), first_ptr) << "distinct paths must not share a cache entry";
}

// double_sided.gltf is a minimal hand-written text glTF (single triangle, base64 data-URI buffer) whose material sets
// "doubleSided": true. The loader must surface it (via assimp's AI_MATKEY_TWOSIDED) so the renderer can drop back-face
// culling for those submeshes.

TEST(MeshLoaderTest, ReadsGltfDoubleSidedFlag) {
  MeshLoader loader;
  const MeshData mesh = loader.load(fixturePath("double_sided.gltf")).result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  ASSERT_FALSE(mesh.submeshes.empty());
  EXPECT_TRUE(mesh.submeshes.front().material->double_sided);
}

// And the default stays single-sided for a material that does not opt in.
TEST(MeshLoaderTest, SingleSidedMaterialStaysCullable) {
  MeshLoader loader;
  const QByteArray bytes = readFixtureBytes("embedded_base.glb");
  ASSERT_FALSE(bytes.isEmpty());
  const MeshData mesh = loader.loadFromMemory(bytes, u"glb"_s).result();
  ASSERT_TRUE(mesh.ok) << mesh.error.toStdString();
  ASSERT_FALSE(mesh.submeshes.empty());
  EXPECT_FALSE(mesh.submeshes.front().material->double_sided);
}

TEST(MeshLoaderTest, MissingFileFailsCleanly) {
  MeshLoader loader;
  const MeshData mesh = loader.load(fixturePath("does_not_exist.stl")).result();
  EXPECT_FALSE(mesh.ok);
  EXPECT_FALSE(mesh.error.isEmpty());
}

// A failed import must not stay cached or Retry never retries. Sequence: cache the import, corrupt the file (the cache
// must still serve the OLD result: no silent re-import), evict() the entry, re-load: the re-import sees the corrupted
// bytes and fails.

TEST(MeshLoaderTest, EvictForcesReimportOfChangedFile) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath(u"evict_me.stl"_s);
  {
    QFile fixture(fixturePath("cube.stl"));
    ASSERT_TRUE(fixture.copy(path));
  }

  MeshLoader loader;
  const MeshData first = loader.load(path).result();
  ASSERT_TRUE(first.ok) << first.error.toStdString();

  {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("definitely not a mesh");
  }
  const MeshData cached = loader.load(path).result();
  EXPECT_TRUE(cached.ok) << "load() re-imported without evict() — cache identity regression";

  loader.evict(path);
  const MeshData reimported = loader.load(path).result();
  EXPECT_FALSE(reimported.ok) << "evict() did not drop the cached entry (old result still served)";
}

// load(path, flip_override) lets the layer force the Y->Z flip on/off, honoring the "Ignore COLLADA up_axis" toggle.
// zup_marker.dae is authored Z_UP: the DEFAULT load (flip=true) lands the apex at z=+1; with flip_override=false the
// flip is skipped and the apex stays at assimp's internal Y-up position (y=+1). The overrides must yield DIFFERENT
// geometry.

TEST(MeshLoaderTest, ColladaFlipOverrideChangesGeometry) {
  MeshLoader loader;
  const MeshData flipped = loader.load(fixturePath("zup_marker.dae"), /*flip_override=*/true).result();
  const MeshData unflipped = loader.load(fixturePath("zup_marker.dae"), /*flip_override=*/false).result();
  ASSERT_TRUE(flipped.ok) << flipped.error.toStdString();
  ASSERT_TRUE(unflipped.ok) << unflipped.error.toStdString();

  const Bounds flipped_bounds = computeBounds(flipped);
  EXPECT_NEAR(flipped_bounds.max.z, 1.0f, 1e-3f) << "flipped apex should land at z=+1";

  const Bounds unflipped_bounds = computeBounds(unflipped);
  EXPECT_GT(unflipped_bounds.extent().y, 0.9f) << "unflipped apex should extend along +Y (still Y-up)";
  EXPECT_LT(unflipped_bounds.extent().z, 0.3f) << "unflipped geometry must not be Z-up";

  // The override is part of the cache key: the same path under two overrides gets distinct cache slots, not one shared
  // entry that would hand back the flipped MeshData.
  const MeshData* flipped_ptr = &(*loader.load(fixturePath("zup_marker.dae"), true).constBegin());
  const MeshData* unflipped_ptr = &(*loader.load(fixturePath("zup_marker.dae"), false).constBegin());
  EXPECT_NE(flipped_ptr, unflipped_ptr) << "flip variants must occupy distinct cache slots";
}

// The default (no override) must match the explicit-true override for a format
// that flips, and re-loading the same default hits the same cache slot.
TEST(MeshLoaderTest, ColladaDefaultMatchesExplicitFlip) {
  MeshLoader loader;
  const MeshData* default_ptr = &(*loader.load(fixturePath("zup_marker.dae")).constBegin());
  const MeshData* explicit_ptr = &(*loader.load(fixturePath("zup_marker.dae"), /*flip_override=*/true).constBegin());
  ASSERT_TRUE(loader.load(fixturePath("zup_marker.dae")).result().ok);
  EXPECT_EQ(default_ptr, explicit_ptr) << "default flip for .dae must equal explicit flip_override=true";
}

// evict(path) must drop BOTH flip variants so a Retry re-imports regardless of
// which override was in effect (the cache is keyed by path+effective-flip).
TEST(MeshLoaderTest, EvictDropsBothFlipVariants) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath(u"evict_both.dae"_s);
  {
    QFile fixture(fixturePath("cube.dae"));
    ASSERT_TRUE(fixture.copy(path));
  }

  MeshLoader loader;
  ASSERT_TRUE(loader.load(path, /*flip_override=*/true).result().ok);
  ASSERT_TRUE(loader.load(path, /*flip_override=*/false).result().ok);

  {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("definitely not a mesh");
  }
  EXPECT_TRUE(loader.load(path, true).result().ok);
  EXPECT_TRUE(loader.load(path, false).result().ok);

  loader.evict(path);
  EXPECT_FALSE(loader.load(path, true).result().ok) << "evict() left the flip=true variant cached";
  EXPECT_FALSE(loader.load(path, false).result().ok) << "evict() left the flip=false variant cached";
}

}  // namespace
}  // namespace pj::scene3d
