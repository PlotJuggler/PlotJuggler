// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Real-GL regression for the mesh-shadow "acne" bug: the shadow depth pre-pass
// (MeshRenderPass::renderDepthOnly) draws each caster's VAO and — unlike the
// visual pass — leaves it bound. If the NEXT caster is uploaded while that prior
// VAO is still bound, uploadIfNeeded() must not let the new mesh's element-buffer
// bind land in the prior mesh's VAO. It once did (it uploaded the EBO before
// binding its own VAO; GL_ELEMENT_ARRAY_BUFFER is VAO state), so the first mesh
// got the second's index buffer and the visual pass drew it with the wrong
// indices — garbage micro-triangles read as fine speckle. This only bit the app
// (streaming TF dirties meshes mid-frame, so the upload fires inside the caster
// loop); the static demo uploads once up front and never reproduced it.
//
// The test runs the exact two-dirty-caster depth pass on a real context and
// asserts each mesh's VAO still records ITS OWN element buffer. Skips below GL 4.5
// (the pass programs are #version 450), mirroring voxel_grid_render_pass_gl_test.

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

#include "pj_scene3d_widgets/mesh_data.h"
#include "pj_scene3d_widgets/passes/mesh_render_pass.h"

namespace pj::scene3d {
namespace {

constexpr int kW = 64;
constexpr int kH = 64;

// A trivial triangle mesh with `vertex_count` vertices (indices 0..n-1). Distinct
// vertex/index counts per mesh guarantee distinct GL buffer objects, so a swapped
// element-buffer binding is detectable by GL name alone.
MeshData triangleMesh(int vertex_count) {
  MeshData data;
  data.ok = true;
  for (int i = 0; i < vertex_count; ++i) {
    Vertex v;
    v.position = {static_cast<float>(i), 0.0f, 0.0f};
    data.vertices.push_back(v);
  }
  for (int i = 0; i + 2 < vertex_count; ++i) {
    data.indices.push_back(static_cast<std::uint32_t>(i));
    data.indices.push_back(static_cast<std::uint32_t>(i + 1));
    data.indices.push_back(static_cast<std::uint32_t>(i + 2));
  }
  data.submeshes.push_back(SubMesh{0, static_cast<std::uint32_t>(data.indices.size()), nullptr});
  return data;
}

MeshRenderPass::DrawCall meshDraw(const std::string& key) {
  MeshRenderPass::DrawCall d;
  d.kind = MeshRenderPass::GeometryKind::kMesh;
  d.mesh_key = key;
  d.model = glm::mat4(1.0f);
  return d;
}

class MeshRenderPassShadowUploadGlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    QSurfaceFormat fmt;
    fmt.setVersion(4, 5);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);

    surface_ = std::make_unique<QOffscreenSurface>();
    surface_->setFormat(fmt);
    surface_->create();
    if (!surface_->isValid()) {
      GTEST_SKIP() << "no usable offscreen surface (headless without GL)";
    }
    ctx_ = std::make_unique<QOpenGLContext>();
    ctx_->setFormat(fmt);
    if (!ctx_->create() || !ctx_->makeCurrent(surface_.get())) {
      GTEST_SKIP() << "could not create/make-current an OpenGL context";
    }
    const auto* version = reinterpret_cast<const char*>(ctx_->functions()->glGetString(GL_VERSION));
    const QStringList parts = QString::fromLatin1(version).section(QLatin1Char(' '), 0, 0).split(QLatin1Char('.'));
    if (std::pair<int, int>(parts.value(0).toInt(), parts.value(1).toInt()) < std::pair<int, int>(4, 5)) {
      GTEST_SKIP() << "GL " << (version != nullptr ? version : "?") << " below 4.5 — can't compile scene shaders";
    }

    QOpenGLFramebufferObjectFormat fbo_fmt;
    fbo_fmt.setAttachment(QOpenGLFramebufferObject::Depth);
    fbo_ = std::make_unique<QOpenGLFramebufferObject>(kW, kH, fbo_fmt);
    ASSERT_TRUE(fbo_->bind());
    ctx_->functions()->glViewport(0, 0, kW, kH);
  }

  void TearDown() override {
    fbo_.reset();
    if (ctx_ != nullptr) {
      ctx_->doneCurrent();
    }
  }

  // The element-buffer GL name recorded in `vao` (binding it makes its element
  // buffer the queryable GL_ELEMENT_ARRAY_BUFFER_BINDING).
  unsigned int elementBufferOf(unsigned int vao) {
    auto* ef = ctx_->extraFunctions();
    ef->glBindVertexArray(vao);
    GLint bound = 0;
    ctx_->functions()->glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &bound);
    ef->glBindVertexArray(0);
    return static_cast<unsigned int>(bound);
  }

  std::unique_ptr<QOffscreenSurface> surface_;
  std::unique_ptr<QOpenGLContext> ctx_;
  std::unique_ptr<QOpenGLFramebufferObject> fbo_;
};

// The depth pre-pass uploads two freshly-dirtied casters back-to-back, leaving the
// first's VAO bound while the second uploads. Each VAO must still own its own EBO.
TEST_F(MeshRenderPassShadowUploadGlTest, DepthPassUploadKeepsEachCastersOwnElementBuffer) {
  MeshRenderPass pass;
  pass.initializeGL();
  pass.setMeshData("A", triangleMesh(3));  // distinct sizes -> distinct GL buffers
  pass.setMeshData("B", triangleMesh(5));

  // Two casters in one depth pass: renderDepthOnly leaves A's VAO bound when it
  // calls uploadIfNeeded(B) — the exact moment the ordering bug struck.
  pass.renderDepthOnly(glm::mat4(1.0f), {meshDraw("A"), meshDraw("B")});

  const auto names_a = pass.resourceGlNamesForTest("A");
  const auto names_b = pass.resourceGlNamesForTest("B");
  ASSERT_TRUE(names_a.has_value());
  ASSERT_TRUE(names_b.has_value());
  EXPECT_NE(names_a->ebo, names_b->ebo) << "distinct meshes must own distinct element buffers";

  EXPECT_EQ(elementBufferOf(names_a->vao), names_a->ebo)
      << "A's VAO must still bind A's index buffer — uploading B must not hijack it";
  EXPECT_EQ(elementBufferOf(names_b->vao), names_b->ebo) << "B's VAO must bind B's own index buffer";
}

}  // namespace
}  // namespace pj::scene3d
