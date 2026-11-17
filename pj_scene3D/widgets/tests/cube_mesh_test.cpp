// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Guards two geometric invariants of the shared cube that nothing else catches cheaply:
//
//  * WINDING. kCubeVertices must be CCW viewed from outside: the QRhi cube/voxel pipelines cull back faces, and a CW
//    table draws the cube's far side. Eyeballing the table does not catch this; the cross product does.
//  * FAN COVERAGE. The hexagon fan must tile exactly the three faces whose outward normal points at the camera, for
//    every one of the eight sign combinations: the C++ mirror of cubeFanCorner() below is checked against the same
//    table the shader compiles, so a table edit that breaks one octant fails here.

#include "pj_scene3d_widgets/cube_mesh.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <map>
#include <set>
#include <vector>

namespace {

using pj::scene3d::CubeVertex;
using pj::scene3d::kCubeFanIndices;
using pj::scene3d::kCubeIndices;
using pj::scene3d::kCubeVertices;

glm::vec3 position(const CubeVertex& v) {
  return {v.px, v.py, v.pz};
}
glm::vec3 normal(const CubeVertex& v) {
  return {v.nx, v.ny, v.nz};
}

TEST(CubeMeshTest, EveryTriangleIsWoundCounterClockwiseFromOutside) {
  ASSERT_EQ(kCubeIndices.size() % 3U, 0U);
  for (std::size_t i = 0; i < kCubeIndices.size(); i += 3U) {
    const CubeVertex& a = kCubeVertices[kCubeIndices[i]];
    const CubeVertex& b = kCubeVertices[kCubeIndices[i + 1U]];
    const CubeVertex& c = kCubeVertices[kCubeIndices[i + 2U]];
    const glm::vec3 geometric = glm::cross(position(b) - position(a), position(c) - position(a));
    EXPECT_GT(glm::dot(geometric, normal(a)), 0.0f) << "triangle " << (i / 3U) << " is wound inside-out";
  }
}

TEST(CubeMeshTest, StoredNormalsAreUnitAxisAlignedAndConsistentPerFace) {
  for (std::size_t face = 0; face < 6U; ++face) {
    const glm::vec3 face_normal = normal(kCubeVertices[face * 4U]);
    EXPECT_FLOAT_EQ(glm::length(face_normal), 1.0f);
    for (std::size_t corner = 0; corner < 4U; ++corner) {
      const CubeVertex& v = kCubeVertices[face * 4U + corner];
      EXPECT_EQ(normal(v), face_normal) << "face " << face << " has mixed normals";
      EXPECT_FLOAT_EQ(glm::dot(position(v), face_normal), 0.5f);
    }
  }
}

// C++ mirror of cubeFanCorner() in shaders/cube/cube_fan.glslinc, deliberately literal so a change to the GLSL table
// without a change here fails instead of drifting.
glm::vec3 fanCorner(int vertex_id, const glm::vec3& face_signs) {
  static const std::array<glm::vec3, 7> kFanFlip = {{
      {1, 1, 1},
      {-1, 1, 1},
      {-1, -1, 1},
      {1, -1, 1},
      {1, -1, -1},
      {1, 1, -1},
      {-1, 1, -1},
  }};
  int ring = vertex_id;
  if (ring > 0 && face_signs.x * face_signs.y * face_signs.z < 0.0f) {
    ring = 7 - ring;
  }
  return 0.5f * face_signs * kFanFlip[static_cast<std::size_t>(ring)];
}

std::vector<glm::vec3> allSignCombinations() {
  std::vector<glm::vec3> out;
  for (float x : {-1.0f, 1.0f}) {
    for (float y : {-1.0f, 1.0f}) {
      for (float z : {-1.0f, 1.0f}) {
        out.emplace_back(x, y, z);
      }
    }
  }
  return out;
}

TEST(CubeMeshTest, FanCornersAreCubeCornersAndIncludeTheNearOne) {
  for (const glm::vec3& signs : allSignCombinations()) {
    std::set<std::array<float, 3>> seen;
    for (int vertex_id = 0; vertex_id < 7; ++vertex_id) {
      const glm::vec3 corner = fanCorner(vertex_id, signs);
      EXPECT_FLOAT_EQ(std::abs(corner.x), 0.5f);
      EXPECT_FLOAT_EQ(std::abs(corner.y), 0.5f);
      EXPECT_FLOAT_EQ(std::abs(corner.z), 0.5f);
      seen.insert({corner.x, corner.y, corner.z});
    }
    EXPECT_EQ(seen.size(), 7U) << "fan repeats a corner, so a triangle is degenerate";
    EXPECT_EQ(fanCorner(0, signs), 0.5f * signs);
  }
}

TEST(CubeMeshTest, FanTilesExactlyTheThreeCameraFacingFaces) {
  ASSERT_EQ(kCubeFanIndices.size(), 18U);
  for (const glm::vec3& signs : allSignCombinations()) {
    std::vector<glm::vec3> face_normals;
    for (std::size_t i = 0; i < kCubeFanIndices.size(); i += 3U) {
      const glm::vec3 a = fanCorner(static_cast<int>(kCubeFanIndices[i]), signs);
      const glm::vec3 b = fanCorner(static_cast<int>(kCubeFanIndices[i + 1U]), signs);
      const glm::vec3 c = fanCorner(static_cast<int>(kCubeFanIndices[i + 2U]), signs);

      int constant_axis = -1;
      for (int axis = 0; axis < 3; ++axis) {
        if (a[axis] == b[axis] && b[axis] == c[axis]) {
          constant_axis = axis;
        }
      }
      ASSERT_NE(constant_axis, -1) << "triangle spans more than one face — not planar with a face";

      glm::vec3 face_normal(0.0f);
      face_normal[constant_axis] = a[constant_axis] > 0.0f ? 1.0f : -1.0f;
      EXPECT_FLOAT_EQ(face_normal[constant_axis], signs[constant_axis])
          << "fan drew a face pointing away from the camera";

      const glm::vec3 geometric = glm::cross(b - a, c - a);
      EXPECT_GT(glm::dot(geometric, face_normal), 0.0f) << "fan triangle is wound inside-out";
      EXPECT_FLOAT_EQ(glm::length(geometric) * 0.5f, 0.5f) << "fan triangle does not half-tile its face";

      face_normals.push_back(face_normal);
    }
    ASSERT_EQ(face_normals.size(), 6U);
    // Exactly three distinct faces, two triangles each. Note the two triangles of a
    // face are NOT always adjacent in the index list: mirroring reverses the ring, so
    // the pairs shift by one. Count them rather than assuming an order.
    std::map<std::array<float, 3>, int> triangles_per_face;
    for (const glm::vec3& face_normal : face_normals) {
      ++triangles_per_face[{face_normal.x, face_normal.y, face_normal.z}];
    }
    EXPECT_EQ(triangles_per_face.size(), 3U) << "the fan covers other than three distinct faces";
    for (const auto& entry : triangles_per_face) {
      EXPECT_EQ(entry.second, 2) << "a face is not tiled by exactly two triangles";
    }
  }
}

// kCubeEdgeMeanScale keeps a LOD sprite as bright as the cube it replaces and is DERIVED from the edge band's width,
// its darkening strength and the sRGB decode: edit any of those and the constant is silently wrong.
// scene3d_rhi_point_test measures the consequence end to end but only where a GPU is available; this pure-math check
// runs everywhere.
TEST(CubeEdgeMeanScaleTest, MatchesTheIntegralOfItsOwnConstants) {
  EXPECT_NEAR(pj::scene3d::cubeEdgeMeanScale(), pj::scene3d::kCubeEdgeMeanScale, 5e-4)
      << "kCubeEdgeMeanScale no longer matches an integration of kCubeEdgeWidth / "
         "kCubeEdgeDarken through the sRGB decode — one of them was retuned without the other";
}

// The flat shade a sub-2-px sphere gets must stay the disc mean of the shared Lambert,
// or spheres change brightness as they cross the flat-disc threshold.
TEST(SphereDiscShadingTest, MatchesTheDiscMeanOfTheSharedLight) {
  EXPECT_NEAR(pj::scene3d::sphereDiscMeanShading(), pj::scene3d::kSphereDiscShading, 1e-3);
}

}  // namespace
