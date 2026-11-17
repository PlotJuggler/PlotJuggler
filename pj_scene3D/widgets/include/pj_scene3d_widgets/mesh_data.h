// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// CPU-side mesh + material data model produced by the mesh loader
// (src/mesh_loader.h) and consumed by the view's model pass. Pure data, no GPU:
// buffer/texture upload lives in the pass.

#include <QString>
#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace pj::scene3d {

// One interleaved vertex. Layout fixed here for the renderer:
// position (model space), smooth normal, genuine per-vertex RGBA color when the
// source carries one (otherwise a neutral fallback), and UV0 (already flipped by
// assimp's aiProcess_FlipUVs; do not flip again in the renderer).
struct Vertex {
  glm::vec3 position{0.0f, 0.0f, 0.0f};
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  glm::vec4 color{0.7f, 0.7f, 0.7f, 1.0f};
  glm::vec2 uv{0.0f, 0.0f};
  // Tangent in model space: .xyz is the U-axis direction, .w the handedness sign (+/-1) for the bitangent
  // cross(normal, tangent.xyz) * w. From aiProcess_CalcTangentSpace; a benign default basis keeps meshes without
  // UVs/tangents uploading a well-formed attribute.
  glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
};

// One texture map of a material. Pixels come from EITHER an external file (`path`) OR inline `bytes` (e.g. a glTF/GLB
// embedded image still PNG/JPEG-encoded). `key` is the renderer's per-context texture cache key: the absolute path for
// external maps, a content hash for embedded ones (identical embedded textures dedup and the key survives a QRhi
// recreation); empty when the material lacks this map. The source only describes content; the renderer owns upload
// policy and infers each slot's colour space (ModelRhiPass): base-colour/emissive are colour data,
// metallic-roughness/normal/occlusion linear. Embedded `bytes` stay resident on the owning MeshData for the mesh's
// lifetime (copied once per distinct Material, shared by its SubMeshes) so the renderer can re-decode them for a new
// QRhi.
struct TextureSource {
  QString path;
  std::vector<std::uint8_t> bytes;
  std::string key;
  // An external reference an in-memory import had no directory to resolve (never
  // a local path), relative to the mesh's own location: MeshLoadQueue fetches it
  // beside a URL mesh into `bytes`. Not a pixel source: empty() ignores it.
  QString unresolved_ref;

  [[nodiscard]] bool empty() const {
    return path.isEmpty() && bytes.empty();
  }
  [[nodiscard]] bool named() const {
    return !empty() || !unresolved_ref.isEmpty();
  }
};

// glTF transparency semantics (KHR core). kMask discards fragments below
// `alpha_cutoff`; kBlend draws in the renderer's translucent bucket with alpha
// blending (best-effort: no per-triangle depth sort).
enum class AlphaMode { kOpaque, kMask, kBlend };

// A metallic-roughness material (glTF 2.0 core), read via assimp's material
// abstraction so it also covers DAE/OBJ/FBX. Factors multiply their textures;
// any texture slot may be empty. `has_pbr` is false for sources that carry no
// PBR keys (STL, untextured primitives) — the renderer then falls back to the
// per-view MeshShadingParams roughness/reflectivity so their look is unchanged.
struct Material {
  glm::vec4 base_color_factor{0.7f, 0.7f, 0.7f, 1.0f};
  float metallic_factor{0.0f};
  float roughness_factor{1.0f};
  glm::vec3 emissive_factor{0.0f, 0.0f, 0.0f};
  AlphaMode alpha_mode{AlphaMode::kOpaque};
  float alpha_cutoff{0.5f};
  bool has_pbr{false};
  // glTF doubleSided (assimp AI_MATKEY_TWOSIDED). No pass reads it yet: the model pass never culls back faces, so the
  // flag is latent until something enables culling. Reasoning, not measured: robot meshes often have inconsistent
  // winding, which culling would expose.
  bool double_sided{false};

  TextureSource base_color;
  TextureSource metallic_roughness;
  TextureSource normal;
  TextureSource occlusion;
  TextureSource emissive;

  /// The five maps in the renderer's slot order: base colour, metallic-roughness, normal, occlusion, emissive;
  /// ModelRhiPass::textureIsSrgb() keys off this order.
  [[nodiscard]] std::array<const TextureSource*, 5> textureSlots() const {
    return {&base_color, &metallic_roughness, &normal, &occlusion, &emissive};
  }
  [[nodiscard]] std::array<TextureSource*, 5> textureSlots() {
    return {&base_color, &metallic_roughness, &normal, &occlusion, &emissive};
  }
};

struct SubMesh {
  // Element offsets/counts into MeshData::indices, not byte offsets. index_count
  // is a multiple of 3 for drawable triangle ranges.
  std::size_t index_offset{0};
  std::size_t index_count{0};
  // Material applied to this index range. Shared, not owned per submesh: assimp routinely splits one material across
  // many submeshes, and copying would duplicate each map's embedded bytes. Invariant: the loader always populates it (a
  // neutral fallback when the source has none); the renderer still null-guards, so a default-constructed SubMesh stays
  // valid and allocation-free.
  std::shared_ptr<const Material> material;
};

// CPU mesh: triangle soup, indexed. `ok` is false when import failed or the
// file/buffer held no renderable geometry; `error` carries the assimp message.
struct MeshData {
  std::vector<Vertex> vertices;
  std::vector<std::uint32_t> indices;
  std::vector<SubMesh> submeshes;
  bool ok{false};
  QString error;
};

}  // namespace pj::scene3d
