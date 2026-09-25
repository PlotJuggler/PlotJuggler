// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The desktop and the browser render the same solids through completely separate
// backends — loose uniforms and runtime-composed GLSL 450 on one side, a std140 UBO and
// ahead-of-time-baked qsb packs on the other. What they must NOT differ on is how a solid
// LOOKS, and the key light and edge band used to be spelled once per shader with nothing
// keeping the copies equal.
//
// They now share one source: pj_scene3D/widgets/shaders/cube/*.glslinc, which the browser
// shaders #include directly (qsb honours GL_GOOGLE_include_directive) and which CMake
// embeds into cube_shader_sources.h for the desktop. This test guards that arrangement
// from the two ways it can quietly rot:
//
//   1. a browser shader stops including the shared file, or
//   2. someone re-inlines a literal light or edge constant next to the include.
//
// Runs on the DESKTOP build, reading the browser shaders as data, because the WASM target
// is built with PJ_WASM_WITH_SCENE3D=OFF in CI and so never compiles them there.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "pj_scene3d_widgets/cube_mesh.h"
#include "pj_widgets/Colormap.h"

namespace {

using pj::scene3d::kCubeEdgeGlsl;
using pj::scene3d::kCubeLightingGlsl;

std::string readShader(std::string_view name) {
  const std::filesystem::path path = std::filesystem::path(PJ_WASM_SHADER_DIR) / name;
  std::ifstream stream(path);
  EXPECT_TRUE(stream.is_open()) << "cannot open " << path.string();
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

// The literals that must exist in the shared source and NOWHERE else.
constexpr std::string_view kKeyLightVector = "0.4, 0.5, 0.8";
constexpr std::string_view kAmbient = "0.35";
constexpr std::string_view kEdgeWidth = "0.06";
constexpr std::string_view kLightingInclude = "cube/cube_lighting.glslinc";
constexpr std::string_view kEdgeInclude = "cube/cube_edge.glslinc";

TEST(WasmShaderConstantsTest, TheSharedSourceStillDefinesTheAppearance) {
  EXPECT_NE(std::string_view(kCubeLightingGlsl).find(kKeyLightVector), std::string_view::npos);
  EXPECT_NE(std::string_view(kCubeLightingGlsl).find(kAmbient), std::string_view::npos);
  EXPECT_NE(std::string_view(kCubeEdgeGlsl).find(kEdgeWidth), std::string_view::npos)
      << "the generated header no longer carries the shared GLSL — check the CMake codegen";
}

// Every browser shader that lights a solid must take that light from the shared file.
TEST(WasmShaderConstantsTest, BrowserSolidsIncludeTheSharedLighting) {
  for (const std::string_view shader : {"cube.frag", "voxel.frag", "point.frag"}) {
    const std::string source = readShader(shader);
    EXPECT_NE(source.find(kLightingInclude), std::string::npos) << shader << " stopped including the shared key light";
    EXPECT_EQ(source.find(kKeyLightVector), std::string::npos)
        << shader << " re-inlined the key light instead of using the shared definition — "
        << "the desktop and browser can now drift apart";
  }
}

TEST(WasmShaderConstantsTest, BrowserSolidsIncludeTheSharedEdgeBand) {
  for (const std::string_view shader : {"cube.frag", "voxel.frag"}) {
    const std::string source = readShader(shader);
    EXPECT_NE(source.find(kEdgeInclude), std::string::npos) << shader << " stopped including the shared edge band";
    EXPECT_EQ(source.find("cubeEdgeFactor(vec3"), std::string::npos)
        << shader << " re-declared cubeEdgeFactor locally instead of including it";
    EXPECT_EQ(source.find(kEdgeWidth), std::string::npos) << shader << " re-inlined the edge band half-width";
  }
}

// The QRhi point and grid-map shaders evaluate the colormap polynomials from a file copy of
// PJ::colormapGlsl() (qsb can only #include files); the two must stay identical.
TEST(WasmShaderConstantsTest, ColormapCopyMatchesTheSharedPolynomials) {
  const std::string copy = readShader("colormap.glslinc");
  EXPECT_NE(copy.find(PJ::colormapGlsl()), std::string::npos)
      << "rhi/shaders/colormap.glslinc drifted from PJ::colormapGlsl(): paste the Colormap.cpp string again";
  for (const std::string_view shader : {"point.vert", "cube.vert", "gridmap.frag"}) {
    EXPECT_NE(readShader(shader).find("colormap.glslinc"), std::string::npos) << shader;
  }
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
