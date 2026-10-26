// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// GPU-free tests of the QRhi model pass's draw policy (ModelRhiPass): the
// texture-slot colour space, the texture-cache key, mesh and placed bounds, the
// visual/collision gating and opacity, blend routing, and per-view shading.
// admit() runs without a QRhi; everything that uploads or draws is covered by the
// display suites (scene3d_rhi_model_cache_test, scene3d_rhi_render_test).

#include <gtest/gtest.h>

#include <QColor>
#include <QSize>
#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../rhi/passes/model_rhi_pass.h"
#include "fake_model_layer.h"
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_widgets/model_renderable.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_tf/transform.h"

namespace pj::scene3d {
namespace {

// Material texture slots, in ModelRhiPass order.
constexpr std::size_t kBaseColor = 0;
constexpr std::size_t kMetallicRoughness = 1;
constexpr std::size_t kNormal = 2;
constexpr std::size_t kOcclusion = 3;
constexpr std::size_t kEmissive = 4;

using test::FakeModelLayer;

Vertex at(float x, float y, float z) {
  Vertex vertex;
  vertex.position = {x, y, z};
  return vertex;
}

// A unit box (corners at +-1) with one submesh.
std::shared_ptr<const MeshData> unitBox() {
  auto mesh = std::make_shared<MeshData>();
  mesh->ok = true;
  mesh->vertices = {at(-1, -1, -1), at(1, 1, 1), at(1, -1, 1)};
  mesh->indices = {0, 1, 2};
  mesh->submeshes.push_back(SubMesh{0, 3, std::make_shared<Material>()});
  return mesh;
}

ModelDrawCall drawOf(ModelDrawGroup group, glm::dvec3 translation) {
  ModelDrawCall draw;
  draw.mesh_key = "box";
  draw.frame_id = "map";
  draw.model = glm::translate(glm::dmat4(1.0), translation);
  draw.group = group;
  return draw;
}

// "map" sits 5 m above the fixed frame; nothing else can be placed.
std::optional<Transform> placeMap(
    const std::string& source_frame, std::optional<PJ::Timepoint> /*object_stamp*/, PlacementHold* /*hold*/) {
  if (source_frame != "map") {
    return std::nullopt;
  }
  Transform fixed_from_map;
  fixed_from_map.t = {0.0, 0.0, 5.0};
  return fixed_from_map;
}

// The placed bounds of one admission of `layer`.
AABB admitOnce(FakeModelLayer& layer, const MeshShadingParams& shading) {
  ModelRhiPass pass;
  pass.setLayers({&layer});
  pass.admit(placeMap, shading, /*shadows_requested=*/true);
  return pass.lastBounds();
}

// Base colour and emissive are colour data (sRGB, linearized on sample); the
// data maps must stay linear so their channels are read verbatim.
TEST(ModelRhiPolicy, TextureSlotColorSpace) {
  EXPECT_TRUE(ModelRhiPass::textureIsSrgb(kBaseColor));
  EXPECT_TRUE(ModelRhiPass::textureIsSrgb(kEmissive));
  EXPECT_FALSE(ModelRhiPass::textureIsSrgb(kMetallicRoughness));
  EXPECT_FALSE(ModelRhiPass::textureIsSrgb(kNormal));
  EXPECT_FALSE(ModelRhiPass::textureIsSrgb(kOcclusion));
}

// One image serving a colour slot in one material and a data slot in another is
// two cache entries; a cached sRGB upload never satisfies a linear request.
TEST(ModelRhiPolicy, TextureCacheIsKeyedByKeyAndColorSpace) {
  TextureSource image;
  image.key = "emb:deadbeef";
  const ModelRhiPass::TextureKey srgb = ModelRhiPass::textureKey(image, kBaseColor);
  const ModelRhiPass::TextureKey linear = ModelRhiPass::textureKey(image, kNormal);
  EXPECT_EQ(srgb.first, "emb:deadbeef");
  EXPECT_EQ(linear.first, "emb:deadbeef");
  EXPECT_NE(srgb, linear) << "same key in two colour spaces must not dedup to one entry";
  EXPECT_EQ(srgb, ModelRhiPass::textureKey(image, kEmissive)) << "two sRGB slots share the decode";

  std::map<ModelRhiPass::TextureKey, int> cache;
  cache.emplace(srgb, 1);
  EXPECT_EQ(cache.count(linear), 0U);
  TextureSource other;
  other.key = "emb:other";
  EXPECT_EQ(cache.count(ModelRhiPass::textureKey(other, kBaseColor)), 0U);
  cache.emplace(linear, 2);
  EXPECT_EQ(cache.size(), 2U);
}

// Unkeyed sources share only with themselves.
TEST(ModelRhiPolicy, UnkeyedTexturesKeyByIdentity) {
  TextureSource first;
  TextureSource second;
  EXPECT_EQ(ModelRhiPass::textureKey(first, kBaseColor), ModelRhiPass::textureKey(first, kBaseColor));
  EXPECT_NE(ModelRhiPass::textureKey(first, kBaseColor), ModelRhiPass::textureKey(second, kBaseColor));
}

// Oversized maps decode scaled down, aspect kept, within both the backend limit
// and the scene's texture dimension.
TEST(ModelRhiPolicy, DecodedTextureSizeFitsBothLimits) {
  EXPECT_EQ(ModelRhiPass::decodedTextureSize(QSize(512, 256), 8192, kBrowserSceneLimits), QSize(512, 256));
  EXPECT_EQ(ModelRhiPass::decodedTextureSize(QSize(4096, 2048), 8192, kBrowserSceneLimits), QSize(1024, 512));
  EXPECT_EQ(ModelRhiPass::decodedTextureSize(QSize(4096, 4096), 2048, kDesktopSceneLimits), QSize(2048, 2048));
  EXPECT_EQ(ModelRhiPass::decodedTextureSize(QSize(65536, 1), 8192, kBrowserSceneLimits), QSize(1024, 1));
}

// A PNG that decodes to a 1024 px texture still holds its full-size image while
// decoding; a 16k PNG (a few MiB of flat colour) must not get that far.
TEST(ModelRhiPolicy, NonScalingDecodeIsBoundedBySourcePixels) {
  const QSize huge(16384, 16384);
  const QSize scaled = ModelRhiPass::decodedTextureSize(huge, 16384, kBrowserSceneLimits);
  EXPECT_FALSE(ModelRhiPass::decodeFitsTransientLimit(huge, scaled, false, kBrowserSceneLimits));
  EXPECT_TRUE(ModelRhiPass::decodeFitsTransientLimit(huge, scaled, true, kBrowserSceneLimits));
  const QSize map(2048, 2048);
  EXPECT_TRUE(
      ModelRhiPass::decodeFitsTransientLimit(
          map, ModelRhiPass::decodedTextureSize(map, 16384, kBrowserSceneLimits), false, kBrowserSceneLimits));
}

// A browser robot with fourteen 2048x2048 maps (ANYmal) fits one layer's decoded
// texture budget; over it, the whole layer would fail to draw.
TEST(ModelRhiPolicy, BrowserLayerTextureBudgetFitsARobotOf2kMaps) {
  constexpr std::uint64_t kMaps = 14;
  const QSize size = ModelRhiPass::decodedTextureSize(QSize(2048, 2048), 8192, kBrowserSceneLimits);
  const std::uint64_t pixels = static_cast<std::uint64_t>(size.width()) * static_cast<std::uint64_t>(size.height());
  EXPECT_LE(pixels, kBrowserSceneLimits.max_model_texture_pixels_per_mesh);
  EXPECT_LE(kMaps * pixels, kBrowserSceneLimits.max_model_texture_pixels_per_layer);
}

// Caster and scene bounds start from the model-space box of every finite vertex.
TEST(ModelRhiPolicy, MeshBoundsEncloseAllFiniteVertices) {
  MeshData data;
  data.ok = true;
  data.vertices = {at(0, 0, 0), at(2, -3, 5), at(-1, 4, -2), at(std::numeric_limits<float>::quiet_NaN(), 100, 100)};
  const AABB box = ModelRhiPass::meshBounds(data);
  ASSERT_TRUE(box.valid);
  EXPECT_EQ(box.min, glm::vec3(-1, -3, -2));
  EXPECT_EQ(box.max, glm::vec3(2, 4, 5));
}

TEST(ModelRhiPolicy, MeshBoundsOfEmptyMeshIsInvalid) {
  EXPECT_FALSE(ModelRhiPass::meshBounds(MeshData{}).valid);
}

// The shadow frustum fits the casters' mesh boxes lifted by their placed model
// matrices, relative to the render origin.
TEST(ModelRhiPolicy, CasterBoundsAreTheTransformedMeshBox) {
  const AABB local = ModelRhiPass::meshBounds(*unitBox());
  const glm::dmat4 model = glm::translate(glm::dmat4(1.0), {10.0, 0.0, 0.0});
  const AABB placed = transformedBounds(local, model, glm::dvec3(1.0, 0.0, 0.0));
  ASSERT_TRUE(placed.valid);
  EXPECT_EQ(placed.min, glm::vec3(8, -1, -1));
  EXPECT_EQ(placed.max, glm::vec3(10, 1, 1));
}

// admit() unions each drawn mesh's box, placed by TF times the draw's model.
TEST(ModelRhiPolicy, AdmitUnionsPlacedMeshBounds) {
  FakeModelLayer layer;
  layer.meshes["box"] = unitBox();
  layer.draws = {drawOf(ModelDrawGroup::kIndependent, {10, 0, 0}), drawOf(ModelDrawGroup::kIndependent, {-10, 0, 0})};
  const AABB bounds = admitOnce(layer, MeshShadingParams{});
  ASSERT_TRUE(bounds.valid);
  EXPECT_EQ(bounds.min, glm::vec3(-11, -1, 4));
  EXPECT_EQ(bounds.max, glm::vec3(11, 1, 6));
}

// No draws, or none that can be placed: no bounds.
TEST(ModelRhiPolicy, AdmitWithoutPlacedDrawsHasNoBounds) {
  FakeModelLayer layer;
  layer.meshes["box"] = unitBox();
  EXPECT_FALSE(admitOnce(layer, MeshShadingParams{}).valid);

  layer.draws = {drawOf(ModelDrawGroup::kIndependent, {0, 0, 0})};
  layer.draws.front().frame_id = "mars";
  EXPECT_FALSE(admitOnce(layer, MeshShadingParams{}).valid);
}

// The view's eye toggles and opacities gate robot visuals and collisions in the
// colour pass; SceneEntities models keep their own controls. Shadows: a hidden
// visual still casts, a collision never does.
TEST(ModelRhiPolicy, VisualAndCollisionGating) {
  MeshShadingParams shading;
  for (const auto group : {ModelDrawGroup::kIndependent, ModelDrawGroup::kVisual, ModelDrawGroup::kCollision}) {
    EXPECT_TRUE(ModelRhiPass::drawsInColorPass(group, shading));
  }
  shading.meshes_visible = false;
  EXPECT_FALSE(ModelRhiPass::drawsInColorPass(ModelDrawGroup::kVisual, shading));
  EXPECT_TRUE(ModelRhiPass::drawsInColorPass(ModelDrawGroup::kCollision, shading));
  EXPECT_TRUE(ModelRhiPass::drawsInColorPass(ModelDrawGroup::kIndependent, shading));
  shading = {};
  shading.mesh_opacity = 0.0F;
  EXPECT_FALSE(ModelRhiPass::drawsInColorPass(ModelDrawGroup::kVisual, shading));
  EXPECT_TRUE(ModelRhiPass::drawsInColorPass(ModelDrawGroup::kIndependent, shading));
  shading = {};
  shading.collisions_visible = false;
  EXPECT_FALSE(ModelRhiPass::drawsInColorPass(ModelDrawGroup::kCollision, shading));
  EXPECT_TRUE(ModelRhiPass::drawsInColorPass(ModelDrawGroup::kVisual, shading));
  shading = {};
  shading.collision_opacity = 0.0F;
  EXPECT_FALSE(ModelRhiPass::drawsInColorPass(ModelDrawGroup::kCollision, shading));

  EXPECT_TRUE(ModelRhiPass::castsShadow(ModelDrawGroup::kVisual, true));
  EXPECT_TRUE(ModelRhiPass::castsShadow(ModelDrawGroup::kIndependent, true));
  EXPECT_FALSE(ModelRhiPass::castsShadow(ModelDrawGroup::kCollision, true));
  EXPECT_FALSE(ModelRhiPass::castsShadow(ModelDrawGroup::kVisual, false));
}

// The view opacities scale their group only, on top of the layer opacity.
TEST(ModelRhiPolicy, VisualAndCollisionOpacity) {
  MeshShadingParams shading;
  shading.mesh_opacity = 0.5F;
  shading.collision_opacity = 0.25F;
  EXPECT_FLOAT_EQ(ModelRhiPass::drawOpacity(0.8F, ModelDrawGroup::kVisual, shading), 0.4F);
  EXPECT_FLOAT_EQ(ModelRhiPass::drawOpacity(0.8F, ModelDrawGroup::kCollision, shading), 0.2F);
  EXPECT_FLOAT_EQ(ModelRhiPass::drawOpacity(0.8F, ModelDrawGroup::kIndependent, shading), 0.8F);
}

// A hidden group adds nothing to the scene bounds, even though its visuals may
// still be admitted to cast shadows.
TEST(ModelRhiPolicy, HiddenGroupsContributeNoBounds) {
  FakeModelLayer layer;
  layer.meshes["box"] = unitBox();
  layer.draws = {drawOf(ModelDrawGroup::kVisual, {10, 0, 0}), drawOf(ModelDrawGroup::kCollision, {-10, 0, 0})};
  MeshShadingParams shading;
  shading.meshes_visible = false;
  const AABB bounds = admitOnce(layer, shading);
  ASSERT_TRUE(bounds.valid);
  EXPECT_EQ(bounds.min, glm::vec3(-11, -1, 4)) << "only the collision is drawn";
  EXPECT_EQ(bounds.max, glm::vec3(-9, 1, 6));

  shading.collision_opacity = 0.0F;
  EXPECT_FALSE(admitOnce(layer, shading).valid) << "nothing is drawn in colour";
}

// A blend material routes its submesh to the translucent bucket; mask and opaque
// ones stay opaque, and so does any material the draw ignores. Collisions and a
// final alpha below 1 are translucent whatever the material.
TEST(ModelRhiPolicy, BlendedMaterialRouting) {
  Material opaque;
  Material mask;
  mask.alpha_mode = AlphaMode::kMask;
  Material blend;
  blend.alpha_mode = AlphaMode::kBlend;
  EXPECT_FLOAT_EQ(ModelRhiPass::shaderAlphaMode(opaque, true), 0.0F);
  EXPECT_FLOAT_EQ(ModelRhiPass::shaderAlphaMode(mask, true), 1.0F);
  EXPECT_FLOAT_EQ(ModelRhiPass::shaderAlphaMode(blend, true), 2.0F);
  EXPECT_FLOAT_EQ(ModelRhiPass::shaderAlphaMode(blend, false), 0.0F) << "an ignored material cannot blend";
  EXPECT_FLOAT_EQ(ModelRhiPass::shaderAlphaMode(Material{}, true), 0.0F) << "the null-material fallback is opaque";

  const auto translucent = [](ModelDrawGroup group, float alpha, const Material& material, bool applies) {
    return ModelRhiPass::drawIsTranslucent(group, alpha, ModelRhiPass::shaderAlphaMode(material, applies));
  };
  EXPECT_FALSE(translucent(ModelDrawGroup::kIndependent, 1.0F, opaque, true));
  EXPECT_FALSE(translucent(ModelDrawGroup::kIndependent, 1.0F, mask, true));
  EXPECT_TRUE(translucent(ModelDrawGroup::kIndependent, 1.0F, blend, true));
  EXPECT_FALSE(translucent(ModelDrawGroup::kIndependent, 1.0F, blend, false));
  EXPECT_TRUE(translucent(ModelDrawGroup::kIndependent, 0.5F, opaque, true));
  EXPECT_TRUE(translucent(ModelDrawGroup::kCollision, 1.0F, opaque, true));
  // The alpha the rule sees already carries the layer and view opacities.
  MeshShadingParams shading;
  shading.mesh_opacity = 0.5F;
  EXPECT_TRUE(translucent(
      ModelDrawGroup::kVisual, ModelRhiPass::drawOpacity(1.0F, ModelDrawGroup::kVisual, shading), opaque, true));
}

// Each view owns its shading and hands it to the pass per frame, so two 3D docks
// never share look state.
TEST(ModelRhiPolicy, ViewsCarryIndependentShading) {
  SceneViewWidget view_a;
  SceneViewWidget view_b;
  EXPECT_NE(&view_a.meshShadingParams(), &view_b.meshShadingParams());
  view_a.meshShadingParams().mesh_opacity = 0.25F;
  view_a.meshShadingParams().meshes_visible = false;
  EXPECT_EQ(view_b.meshShadingParams().mesh_opacity, MeshShadingParams{}.mesh_opacity);
  EXPECT_TRUE(view_b.meshShadingParams().meshes_visible);

  FakeModelLayer layer;
  layer.meshes["box"] = unitBox();
  layer.draws = {drawOf(ModelDrawGroup::kVisual, {0, 0, 0})};
  EXPECT_FALSE(admitOnce(layer, view_a.meshShadingParams()).valid);
  EXPECT_TRUE(admitOnce(layer, view_b.meshShadingParams()).valid);
}

}  // namespace
}  // namespace pj::scene3d
