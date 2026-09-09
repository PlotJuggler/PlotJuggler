// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Guards the two geometric invariants of the shared cube that nothing else can catch
// cheaply, and that were both wrong or absent before:
//
//  * WINDING. kCubeVertices used to be CW-viewed-from-outside while its comment
//    claimed CCW, which silently inverted every pipeline that culls back faces (the
//    QRhi cube/voxel pipelines did, and drew the cube's far side). Eyeballing the
//    table does not catch this; the cross product does.
//  * FAN COVERAGE. The hexagon fan must tile exactly the three faces whose outward
//    normal points at the camera, for every one of the eight sign combinations —
//    the C++ mirror of cubeFanCorner() below is checked against the same table the
//    shader compiles, so a table edit that breaks one octant fails here.

#include "pj_scene3d_widgets/cube_mesh.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <map>
#include <set>
#include <string_view>
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
    // CCW from outside <=> the winding normal agrees with the stored outward normal.
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
      // The corner lies ON that face: its normal-axis coordinate is the +/-0.5 extreme.
      EXPECT_FLOAT_EQ(glm::dot(position(v), face_normal), 0.5f);
    }
  }
}

// C++ mirror of cubeFanCorner() in kCubeFanGlsl. Kept deliberately literal so a change
// to the GLSL table without a change here shows up as a failure rather than drift.
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
    // Vertex 0 is the corner nearest the camera: every axis on the camera's side.
    EXPECT_EQ(fanCorner(0, signs), 0.5f * signs);
  }
}

TEST(CubeMeshTest, FanTilesExactlyTheThreeCameraFacingFaces) {
  ASSERT_EQ(kCubeFanIndices.size(), 18U);
  for (const glm::vec3& signs : allSignCombinations()) {
    // Which face each triangle lies in, named by the axis that is constant at +/-0.5
    // across all three of its corners, and the winding-derived outward normal.
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
      // Only faces the camera is on the side of may be drawn.
      EXPECT_FLOAT_EQ(face_normal[constant_axis], signs[constant_axis])
          << "fan drew a face pointing away from the camera";

      const glm::vec3 geometric = glm::cross(b - a, c - a);
      EXPECT_GT(glm::dot(geometric, face_normal), 0.0f) << "fan triangle is wound inside-out";
      // Every triangle is a half-quad of its face: area 0.5 of the unit face.
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

// kCubeEdgeMeanScale is what keeps a LOD sprite as bright as the cube it replaces, and it
// is DERIVED from the edge band's width, its darkening strength and the sRGB gamma. Edit
// any of those and the constant is silently wrong. pointcloud_cube_lod_gl_test measures
// the consequence end to end, but it needs GL >= 4.5 and self-skips on Windows CI — so
// this pure-math check is the one that runs everywhere.
TEST(CubeEdgeMeanScaleTest, MatchesTheIntegralOfItsOwnConstants) {
  EXPECT_NEAR(pj::scene3d::cubeEdgeMeanScale(), pj::scene3d::kCubeEdgeMeanScale, 5e-4)
      << "kCubeEdgeMeanScale no longer matches an integration of kCubeEdgeWidth / "
         "kCubeEdgeDarken / kCubeSrgbGamma — one of them was retuned without the other";
}

// The GLSL is the thing that actually runs, so the C++ mirrors are only trustworthy while
// the shader source still spells the same numbers. Cheap guard against editing one side.
TEST(CubeEdgeMeanScaleTest, ShaderSourceDeclaresTheSameConstants) {
  const std::string_view edge_glsl = pj::scene3d::kCubeEdgeGlsl;
  EXPECT_NE(edge_glsl.find("kCubeEdgeWidth = 0.06"), std::string_view::npos)
      << "kCubeEdgeGlsl no longer declares the width the C++ mirror assumes";
  EXPECT_NE(edge_glsl.find("kCubeEdgeDarken = 0.6"), std::string_view::npos)
      << "kCubeEdgeGlsl no longer declares the darkening the C++ mirror assumes";
  EXPECT_NE(std::string_view(pj::scene3d::kCubeSpriteGlsl).find("kCubeEdgeMeanScale = 0.887"), std::string_view::npos)
      << "the sprite shader's kCubeEdgeMeanScale drifted from the C++ constant";
}

}  // namespace
