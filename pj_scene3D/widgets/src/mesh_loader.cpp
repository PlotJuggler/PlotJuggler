// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "mesh_loader.h"

#include <assimp/GltfMaterial.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <QDir>
#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>
#include <array>
#include <assimp/Importer.hpp>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "pj_scene3d_core/model_budget.h"
#include "pj_widgets/Fnv1a.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {

// Post-process flags shared by both entry points (design §B.4):
//   Triangulate          — render passes draw triangles only.
//   GenSmoothNormals      — generate normals only for meshes that arrive WITHOUT
//                           any (assimp skips meshes that already carry normals,
//                           e.g. STL/OBJ, leaving their normals untouched).
//   JoinIdenticalVertices — index the soup so we get a real index buffer.
//   FlipUVs               — match the GL texture origin convention.
//   ImproveCacheLocality  — reorder indices for the post-T&L vertex cache
//                           (added in lieu of meshoptimizer).
//   CalcTangentSpace      — generate per-vertex tangents/bitangents for normal
//                           mapping (only for meshes that carry UVs + normals).
// Deliberately NOT aiProcess_PreTransformVertices: it bakes the scene graph into
// world space. We keep the node hierarchy so appendNode() can apply each node's
// transform itself — including the root transform assimp's Collada importer uses
// to normalize the file's <up_axis> to assimp's internal Y-up convention.
constexpr unsigned int kPostProcessFlags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
                                           aiProcess_JoinIdenticalVertices | aiProcess_FlipUVs |
                                           aiProcess_ImproveCacheLocality | aiProcess_CalcTangentSpace;

// Explicit Y-up -> Z-up rotation (+90deg about X): (x, y, z) -> (x, -z, y).
// Applied to formats that assimp delivers in Y-up — glTF (Y-up by spec) AND
// Collada, which assimp's importer normalizes from the file's <up_axis> to its
// internal Y-up. Verified empirically (zup_marker.dae fixture): a Z_UP-authored
// apex at (0,0,1) arrives as (0,1,0) and is restored to (0,0,1) by this flip.
glm::mat3 zUpFromYUpRotation() {
  // R_x(+90deg): (x, y, z) -> (x, -z, y).
  return glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
}

// FNV-1a over a byte span — a cheap, stable content hash for embedded-texture
// cache keys (dedups identical embedded images and survives a QRhi recreation).
std::string contentHashKey(const std::uint8_t* data, std::size_t size) {
  const std::uint64_t h = PJ::fnv1a(std::span(data, size));
  std::array<char, 17> buf{};
  std::snprintf(buf.data(), buf.size(), "%016llx", static_cast<unsigned long long>(h));
  return std::string("emb:") + buf.data();
}

// Resolve one texture map of a material into a TextureSource. Embedded glTF/GLB
// images (assimp ref "*N") become inline bytes keyed by content hash; external
// refs become an absolute filesystem path (resolved relative to base_dir, with a
// small extension-retry for refs that drop their suffix); with no base_dir (an
// in-memory import) every ref stays `unresolved_ref`, never a local path. Only compressed
// embedded images (PNG/JPEG, the universal glTF case where aiTexture::mHeight ==
// 0) are supported; raw embedded RGBA (mHeight > 0, rare) is skipped and the
// slot stays empty.
struct TextureCopyBudget {
  std::uint64_t bytes = 0;
  bool exceeded = false;
};

TextureSource resolveTexture(
    const aiScene* scene, const aiMaterial* mat, aiTextureType type, const QString& base_dir,
    TextureCopyBudget* texture_budget) {
  aiString texture_path;
  if (aiGetMaterialTexture(mat, type, 0, &texture_path) != AI_SUCCESS) {
    return {};
  }
  const QString raw = QString::fromUtf8(texture_path.C_Str());
  if (raw.isEmpty()) {
    return {};
  }

  if (raw.startsWith(QLatin1Char('*'))) {
    const aiTexture* tex = scene->GetEmbeddedTexture(texture_path.C_Str());
    if (tex == nullptr || tex->mHeight != 0 || tex->pcData == nullptr || tex->mWidth == 0) {
      return {};  // missing, or raw-RGBA embedded (unsupported) — leave untextured.
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(tex->pcData);
    // Charged once per resolved slot (not per distinct image): every resolved
    // slot retains its own encoded copy below, so per-copy charging is what
    // bounds worst-case CPU retention.
    if (texture_budget != nullptr &&
        (texture_budget->bytes + tex->mWidth > kSceneLimits.max_model_encoded_texture_bytes_per_mesh)) {
      texture_budget->exceeded = true;
      return {};
    }
    if (texture_budget != nullptr) {
      texture_budget->bytes += tex->mWidth;
    }
    TextureSource source;
    source.bytes.assign(bytes, bytes + tex->mWidth);  // mWidth is the encoded byte count when mHeight == 0.
    source.key = contentHashKey(bytes, tex->mWidth);
    return source;
  }

  const QString ref = QDir::fromNativeSeparators(raw);
  if (base_dir.isEmpty()) {
    TextureSource source;
    source.unresolved_ref = ref;
    return source;
  }
  const auto resolve = [&base_dir](const QString& path) -> QString {
    const QFileInfo info(QDir::cleanPath(QDir(base_dir).absoluteFilePath(path)));
    return info.isFile() ? info.absoluteFilePath() : QString{};
  };

  QString resolved = resolve(ref);
  if (resolved.isEmpty()) {
    static constexpr const char* kTextureExtensions[] = {".jpg", ".png", ".jpeg", ".tga", ".bmp"};
    for (const char* extension : kTextureExtensions) {
      resolved = resolve(ref + QLatin1String(extension));
      if (!resolved.isEmpty()) {
        break;
      }
    }
  }
  if (resolved.isEmpty()) {
    return {};
  }
  TextureSource source;
  source.path = resolved;
  source.key = resolved.toStdString();
  return source;
}

// Read a full metallic-roughness material (factors + five texture maps) from one
// assimp material. `has_pbr` records whether the source actually specified PBR
// factors, so the renderer can fall back to the scene-wide shading defaults for
// materials that carry none (STL, untextured primitives).
Material readMaterial(
    const aiScene* scene, unsigned int material_index, const QString& base_dir, TextureCopyBudget* texture_budget) {
  Material out;
  if (!scene->HasMaterials() || material_index >= scene->mNumMaterials) {
    return out;
  }
  const aiMaterial* mat = scene->mMaterials[material_index];

  aiColor4D base;
  if (mat->Get(AI_MATKEY_BASE_COLOR, base) == AI_SUCCESS) {
    out.base_color_factor = glm::vec4(base.r, base.g, base.b, base.a);
  } else {
    aiColor4D diffuse;
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS) {
      out.base_color_factor = glm::vec4(diffuse.r, diffuse.g, diffuse.b, diffuse.a);
    }
  }

  float metallic = 0.0f;
  float roughness = 1.0f;
  if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS) {
    out.metallic_factor = metallic;
    out.has_pbr = true;
  }
  if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) {
    out.roughness_factor = roughness;
    out.has_pbr = true;
  }

  aiColor3D emissive;
  if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
    out.emissive_factor = glm::vec3(emissive.r, emissive.g, emissive.b);
  }
  float emissive_intensity = 1.0f;
  if (mat->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissive_intensity) == AI_SUCCESS) {
    out.emissive_factor *= emissive_intensity;
  }

  aiString alpha_mode;
  if (mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alpha_mode) == AI_SUCCESS) {
    const QString mode = QString::fromUtf8(alpha_mode.C_Str());
    if (mode == QLatin1String("MASK")) {
      out.alpha_mode = AlphaMode::kMask;
    } else if (mode == QLatin1String("BLEND")) {
      out.alpha_mode = AlphaMode::kBlend;
    }
  }
  float cutoff = 0.5f;
  if (mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff) == AI_SUCCESS) {
    out.alpha_cutoff = cutoff;
  }

  // glTF doubleSided (and whatever other formats assimp maps to TWOSIDED): when
  // back-face culling is enabled, the renderer drops it for these submeshes
  // (see Material::double_sided in mesh_data.h).
  int two_sided = 0;
  if (mat->Get(AI_MATKEY_TWOSIDED, two_sided) == AI_SUCCESS) {
    out.double_sided = two_sided != 0;
  }

  out.base_color = resolveTexture(scene, mat, aiTextureType_BASE_COLOR, base_dir, texture_budget);
  if (!out.base_color.named()) {
    out.base_color = resolveTexture(scene, mat, aiTextureType_DIFFUSE, base_dir, texture_budget);
  }
  // assimp's glTF2 importer exposes the packed metallic-roughness image under the
  // metalness slot (and aiTextureType_UNKNOWN on older versions) — same file, the
  // shader reads G=roughness/B=metallic.
  out.metallic_roughness = resolveTexture(scene, mat, aiTextureType_METALNESS, base_dir, texture_budget);
  if (!out.metallic_roughness.named()) {
    out.metallic_roughness = resolveTexture(scene, mat, aiTextureType_DIFFUSE_ROUGHNESS, base_dir, texture_budget);
  }
  if (!out.metallic_roughness.named()) {
    out.metallic_roughness = resolveTexture(scene, mat, aiTextureType_UNKNOWN, base_dir, texture_budget);
  }
  out.normal = resolveTexture(scene, mat, aiTextureType_NORMALS, base_dir, texture_budget);
  out.occlusion = resolveTexture(scene, mat, aiTextureType_AMBIENT_OCCLUSION, base_dir, texture_budget);
  if (!out.occlusion.named()) {
    out.occlusion = resolveTexture(scene, mat, aiTextureType_LIGHTMAP, base_dir, texture_budget);
  }
  out.emissive = resolveTexture(scene, mat, aiTextureType_EMISSIVE, base_dir, texture_budget);
  return out;
}

// Walk the node graph, accumulating each mesh's transformed vertices/indices.
// `node_transform` is the cumulative parent transform (column-major glm).
void appendNode(
    const aiScene* scene, const aiNode* node, const glm::mat4& parent_transform, const glm::mat3& extra_rotation,
    const std::vector<std::shared_ptr<const Material>>& materials, MeshData& out) {
  // assimp row-major aiMatrix4x4 -> glm column-major.
  const aiMatrix4x4& m = node->mTransformation;
  glm::mat4 local(
      m.a1, m.b1, m.c1, m.d1,   // col 0
      m.a2, m.b2, m.c2, m.d2,   // col 1
      m.a3, m.b3, m.c3, m.d3,   // col 2
      m.a4, m.b4, m.c4, m.d4);  // col 3
  const glm::mat4 node_transform = parent_transform * local;
  const glm::mat3 normal_matrix = glm::mat3(glm::transpose(glm::inverse(node_transform)));
  const glm::mat3 linear = glm::mat3(node_transform);

  for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
    const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
    static const std::shared_ptr<const Material> k_fallback = std::make_shared<const Material>();
    const std::shared_ptr<const Material>& material =
        mesh->mMaterialIndex < materials.size() ? materials[mesh->mMaterialIndex] : k_fallback;
    const auto base_index = static_cast<std::uint32_t>(out.vertices.size());

    out.vertices.reserve(out.vertices.size() + mesh->mNumVertices);
    for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
      Vertex vert;
      const aiVector3D& p = mesh->mVertices[v];
      glm::vec3 pos = glm::vec3(node_transform * glm::vec4(p.x, p.y, p.z, 1.0f));
      glm::vec3 nrm(0.0f, 0.0f, 1.0f);
      if (mesh->HasNormals()) {
        const aiVector3D& n = mesh->mNormals[v];
        nrm = glm::normalize(normal_matrix * glm::vec3(n.x, n.y, n.z));
      }
      vert.position = extra_rotation * pos;
      vert.normal = glm::normalize(extra_rotation * nrm);
      // Vertex color carries ONLY genuine per-vertex colors (glTF COLOR_0); the
      // material base color lives in SubMesh::material and is applied by the
      // shader. A white default leaves untextured/non-vertex-colored meshes to be
      // driven purely by the material factor (no double-application).
      vert.color =
          mesh->HasVertexColors(0)
              ? glm::vec4(mesh->mColors[0][v].r, mesh->mColors[0][v].g, mesh->mColors[0][v].b, mesh->mColors[0][v].a)
              : glm::vec4(1.0f);
      if (mesh->HasTextureCoords(0)) {
        const aiVector3D& uv = mesh->mTextureCoords[0][v];
        vert.uv = glm::vec2(uv.x, uv.y);
      }
      // Tangent basis for normal mapping (aiProcess_CalcTangentSpace). Transform
      // into world like the geometry, re-orthogonalize against the final normal
      // (Gram-Schmidt), and store the bitangent handedness in .w.
      if (mesh->HasTangentsAndBitangents()) {
        const aiVector3D& t = mesh->mTangents[v];
        const aiVector3D& b = mesh->mBitangents[v];
        glm::vec3 tan = extra_rotation * (linear * glm::vec3(t.x, t.y, t.z));
        glm::vec3 bit = extra_rotation * (linear * glm::vec3(b.x, b.y, b.z));
        tan = tan - vert.normal * glm::dot(vert.normal, tan);
        if (glm::dot(tan, tan) > 1e-12f) {
          tan = glm::normalize(tan);
          const float sign = glm::dot(glm::cross(vert.normal, tan), bit) < 0.0f ? -1.0f : 1.0f;
          vert.tangent = glm::vec4(tan, sign);
        }
      }
      out.vertices.push_back(vert);
    }
    const std::size_t range_start = out.indices.size();
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
      const aiFace& face = mesh->mFaces[f];
      // aiProcess_Triangulate guarantees 3 indices per face; skip any degenerate
      // line/point primitive assimp may still emit for stray geometry.
      if (face.mNumIndices != 3) {
        continue;
      }
      out.indices.push_back(base_index + face.mIndices[0]);
      out.indices.push_back(base_index + face.mIndices[1]);
      out.indices.push_back(base_index + face.mIndices[2]);
    }
    const std::size_t range_count = out.indices.size() - range_start;
    if (range_count > 0U) {
      out.submeshes.push_back(SubMesh{range_start, range_count, material});
    }
  }

  for (unsigned int c = 0; c < node->mNumChildren; ++c) {
    appendNode(scene, node->mChildren[c], node_transform, extra_rotation, materials, out);
  }
}

MeshData buildMeshData(const aiScene* scene, bool flip_to_z_up, const QString& source, const QString& base_dir) {
  MeshData out;
  if (scene == nullptr || scene->mRootNode == nullptr) {
    out.error = u"assimp returned no scene for %1"_s.arg(source);
    return out;
  }
  // Assimp has already parsed the bounded source bytes, but do not let
  // file-declared mesh counts drive our own material/vertex/index allocations.
  // Widen every accumulation before comparing with the scene limits.
  std::uint64_t vertex_count = 0;
  std::uint64_t index_count = 0;
  std::uint64_t submesh_count = 0;
  std::uint64_t node_count = 0;
  std::uint64_t max_node_depth = 0;
  std::vector<std::pair<const aiNode*, std::uint64_t>> pending_nodes{{scene->mRootNode, 1U}};
  while (!pending_nodes.empty()) {
    const auto [node, depth] = pending_nodes.back();
    pending_nodes.pop_back();
    ++node_count;
    max_node_depth = std::max(max_node_depth, depth);
    if (!modelStructureFits(scene->mNumMaterials, node_count, max_node_depth)) {
      out.error = u"model exceeds structure limits in %1 (%2 materials, %3 nodes, depth %4)"_s.arg(
          source, QString::number(scene->mNumMaterials), QString::number(node_count), QString::number(max_node_depth));
      return out;
    }
    for (unsigned int child_index = 0; child_index < node->mNumChildren; ++child_index) {
      if (node->mChildren[child_index] == nullptr ||
          pending_nodes.size() + node_count >= kSceneLimits.max_model_nodes_per_mesh) {
        out.error = u"model has an invalid or over-limit node graph in %1"_s.arg(source);
        return out;
      }
      pending_nodes.emplace_back(node->mChildren[child_index], depth + 1U);
    }
    for (unsigned int reference_index = 0; reference_index < node->mNumMeshes; ++reference_index) {
      const unsigned int mesh_index = node->mMeshes[reference_index];
      if (mesh_index >= scene->mNumMeshes || scene->mMeshes[mesh_index] == nullptr) {
        out.error = u"model has an invalid mesh reference in %1"_s.arg(source);
        return out;
      }
      const aiMesh* mesh = scene->mMeshes[mesh_index];
      if (mesh->mNumVertices > std::numeric_limits<std::uint64_t>::max() - vertex_count) {
        out.error = u"model vertex count overflow in %1"_s.arg(source);
        return out;
      }
      vertex_count += mesh->mNumVertices;
      for (unsigned int face_index = 0; face_index < mesh->mNumFaces; ++face_index) {
        const aiFace& face = mesh->mFaces[face_index];
        if (face.mNumIndices == 3U) {
          if (index_count > std::numeric_limits<std::uint64_t>::max() - 3U) {
            out.error = u"model index count overflow in %1"_s.arg(source);
            return out;
          }
          index_count += 3U;
        }
      }
      if (mesh->mNumFaces != 0U) {
        ++submesh_count;
      }
    }
  }
  if (!modelMeshCountsFit(vertex_count, index_count, submesh_count)) {
    out.error = u"model exceeds geometry limits in %1 (%2 vertices, %3 indices, %4 submeshes)"_s.arg(
        source, QString::number(vertex_count), QString::number(index_count), QString::number(submesh_count));
    return out;
  }
  out.vertices.reserve(static_cast<std::size_t>(vertex_count));
  out.indices.reserve(static_cast<std::size_t>(index_count));
  out.submeshes.reserve(static_cast<std::size_t>(submesh_count));
  // Resolve each material once (embedded-texture extraction is per-material, not
  // per-mesh) and index into it by aiMesh::mMaterialIndex while walking the graph.
  std::vector<std::shared_ptr<const Material>> materials;
  materials.reserve(scene->mNumMaterials);
  TextureCopyBudget texture_budget;
  for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
    materials.push_back(std::make_shared<const Material>(readMaterial(scene, i, base_dir, &texture_budget)));
  }
  if (texture_budget.exceeded) {
    out.error = u"model exceeds the embedded-texture limit in %1"_s.arg(source);
    return out;
  }
  const glm::mat3 extra = flip_to_z_up ? zUpFromYUpRotation() : glm::mat3(1.0f);
  appendNode(scene, scene->mRootNode, glm::mat4(1.0f), extra, materials, out);
  if (out.vertices.empty() || out.indices.empty()) {
    out.error = u"no renderable geometry in %1"_s.arg(source);
    out.ok = false;
    return out;
  }
  const auto retained = modelRetainedByteCount(
      out.vertices.size(), sizeof(Vertex), out.indices.size(), out.submeshes.size(), sizeof(SubMesh),
      texture_budget.bytes);
  if (!retained.has_value() || *retained > kSceneLimits.max_model_retained_bytes_per_mesh) {
    out = {};
    out.error = u"model would retain more than %1 MiB"_s.arg(
        kSceneLimits.max_model_retained_bytes_per_mesh / (1024ULL * 1024ULL));
    return out;
  }
  out.ok = true;
  return out;
}

}  // namespace

bool MeshLoader::wantsZUpFlip(const QString& format_hint) {
  const QString h = format_hint.toLower();
  // glTF is Y-up by spec; assimp also normalizes Collada's <up_axis> to its
  // internal Y-up (verified empirically — see zUpFromYUpRotation). Both need the
  // explicit Y->Z flip into our Z-up world. STL/OBJ carry no up-axis convention
  // and assimp passes their coordinates through unrotated, so they stay false.
  return h == QLatin1String("glb") || h == QLatin1String("gltf") || h == QLatin1String("gltf2") ||
         h == QLatin1String("dae") || h == QLatin1String("collada");
}

MeshData MeshLoader::importFromFile(const QString& path, bool flip_to_z_up) {
  // Exception barrier: assimp drives allocations from file-declared counts and
  // buildMeshData reserves from them too, so hostile input can throw
  // bad_alloc/length_error. This runs on a QtConcurrent worker — an escaped
  // exception would be rethrown by QFuture::result() on the GUI thread and
  // terminate the app. Mirrors pointcloud_codecs.cpp's barriers.
  try {
    const QFileInfo source_info(path);
    if (source_info.size() < 0 ||
        static_cast<std::uint64_t>(source_info.size()) > kSceneLimits.max_model_source_bytes) {
      MeshData out;
      out.error =
          u"model source exceeds the %1 MiB limit"_s.arg(kSceneLimits.max_model_source_bytes / (1024ULL * 1024ULL));
      return out;
    }
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path.toStdString(), kPostProcessFlags);
    if (scene == nullptr) {
      MeshData out;
      out.error = QString::fromUtf8(importer.GetErrorString());
      if (out.error.isEmpty()) {
        out.error = u"assimp failed to import %1"_s.arg(path);
      }
      return out;
    }
    return buildMeshData(scene, flip_to_z_up, path, QFileInfo(path).absolutePath());
  } catch (const std::exception& ex) {
    MeshData out;
    out.error = u"mesh import of %1 threw: %2"_s.arg(path, QString::fromUtf8(ex.what()));
    return out;
  } catch (...) {
    MeshData out;
    out.error = u"mesh import of %1 threw an unknown exception"_s.arg(path);
    return out;
  }
}

MeshData MeshLoader::importFromMemory(const QByteArray& bytes, const QString& format_hint, bool flip_to_z_up) {
  // Same exception barrier as importFromFile (see the comment there).
  try {
    if (bytes.size() < 0 || static_cast<std::uint64_t>(bytes.size()) > kSceneLimits.max_model_source_bytes) {
      MeshData out;
      out.error =
          u"model source exceeds the %1 MiB limit"_s.arg(kSceneLimits.max_model_source_bytes / (1024ULL * 1024ULL));
      return out;
    }
    Assimp::Importer importer;
    const std::string hint = format_hint.toStdString();
    const aiScene* scene = importer.ReadFileFromMemory(
        bytes.constData(), static_cast<std::size_t>(bytes.size()), kPostProcessFlags,
        hint.empty() ? nullptr : hint.c_str());
    if (scene == nullptr) {
      MeshData out;
      out.error = QString::fromUtf8(importer.GetErrorString());
      if (out.error.isEmpty()) {
        out.error = u"assimp failed to import %1 buffer"_s.arg(format_hint);
      }
      return out;
    }
    // Per-submesh materials are kept (embedded textures: readMaterial); external refs stay unresolved.
    return buildMeshData(scene, flip_to_z_up, u"<memory:%1>"_s.arg(format_hint), QString{});
  } catch (const std::exception& ex) {
    MeshData out;
    out.error = u"mesh import of %1 buffer threw: %2"_s.arg(format_hint, QString::fromUtf8(ex.what()));
    return out;
  } catch (...) {
    MeshData out;
    out.error = u"mesh import of %1 buffer threw an unknown exception"_s.arg(format_hint);
    return out;
  }
}

namespace {
// Compose the cache key from the resolved path and the effective flip. A NUL
// byte separates them: NUL cannot appear in a filesystem path, so the two flip
// variants of one path can never collide with each other or with another path.
QString meshCacheKey(const QString& resolved_path, bool flip) {
  return resolved_path + QLatin1Char('\0') + QLatin1Char(flip ? '1' : '0');
}
}  // namespace

QFuture<MeshData> MeshLoader::load(const QString& resolved_path, std::optional<bool> flip_override) {
  // assimp infers the format from the extension. The effective flip is the
  // caller's override when given, else the per-format default (wantsZUpFlip):
  // DAE/glTF arrive Y-up and flip; STL/OBJ stay as-is.
  const bool flip = flip_override.value_or(wantsZUpFlip(QFileInfo(resolved_path).suffix()));
  const QString key = meshCacheKey(resolved_path, flip);
  QMutexLocker lock(&cache_mutex_);
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    return it.value();
  }
  QFuture<MeshData> future =
      QtConcurrent::run([resolved_path, flip]() { return MeshLoader::importFromFile(resolved_path, flip); });
  cache_.insert(key, future);
  return future;
}

QFuture<MeshData> MeshLoader::loadFromMemory(
    const QByteArray& bytes, const QString& format_hint, std::optional<bool> flip_override) {
  // Embedded buffers are not path-keyed (no stable identity); skip the cache.
  const bool flip = flip_override.value_or(wantsZUpFlip(format_hint));
  return QtConcurrent::run(
      [bytes, format_hint, flip]() { return MeshLoader::importFromMemory(bytes, format_hint, flip); });
}

void MeshLoader::clearCache() {
  QMutexLocker lock(&cache_mutex_);
  cache_.clear();
}

void MeshLoader::evict(const QString& resolved_path) {
  QMutexLocker lock(&cache_mutex_);
  // The cache is keyed by path+effective-flip; remove both flip variants so a
  // Retry re-imports regardless of which override was in effect at load time.
  cache_.remove(meshCacheKey(resolved_path, false));
  cache_.remove(meshCacheKey(resolved_path, true));
}

}  // namespace pj::scene3d
