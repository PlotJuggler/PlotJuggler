// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// GL-free unit tests for the owning-context delete guard and the RAII wrappers'
// teardown safety. The app deliberately does NOT share
// OpenGL contexts, so a wrapper must DROP its GL name rather than delete it when
// no owning context is current; deleting under a foreign context would corrupt a
// sibling view. These cases are reachable WITHOUT a GL platform because the
// guard short-circuits on QOpenGLContext::currentContext() == nullptr.

#include <gtest/gtest.h>

#include <QPointer>

#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/framebuffer.h"
#include "pj_scene3d_widgets/gl/gl_functions.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/texture.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"

namespace pj::scene3d::gl {
namespace {

// With no context current, deletion is never allowed — the owner (if any) must
// free its own name later, and a null/destroyed owner means the name is gone.
TEST(GlOwningContextTest, NoCurrentContextNeverAllowsDelete) {
  const QPointer<QOpenGLContext> null_owner;
  EXPECT_FALSE(deleteAllowedInCurrentContext(null_owner))
      << "no current context + null owner must not delete (would target a foreign name space)";
}

// A fresh, never-bound wrapper holds id 0 and a null owning context. Destroying
// it must be a clean no-op even with no GL platform present.
TEST(GlOwningContextTest, UnboundWrappersDestructCleanly) {
  { Buffer buffer; }
  { VertexArray vao; }
  { Texture texture; }
  { Texture2D color_texture; }
  { Framebuffer framebuffer; }
  SUCCEED() << "default-constructed wrappers destruct without a current context";
}

// Move-construct / move-assign on never-bound wrappers transfer the (null)
// owning context alongside the (zero) id without attempting a GL delete.
TEST(GlOwningContextTest, MoveOfUnboundWrappersIsNoGlOp) {
  Buffer source;
  Buffer moved_to(std::move(source));
  EXPECT_EQ(moved_to.id(), 0U);

  Buffer assign_target;
  assign_target = std::move(moved_to);
  EXPECT_EQ(assign_target.id(), 0U);

  Texture2D source_texture;
  Texture2D moved_texture(std::move(source_texture));
  EXPECT_EQ(moved_texture.id(), 0U);
  SUCCEED();
}

// Texture2D::adopt(0) releases ownership: it leaves id 0 and a null owner, so a
// subsequent destructor is a no-op (the documented release semantics, L.114).
TEST(GlOwningContextTest, Texture2DAdoptZeroReleasesOwnership) {
  Texture2D color_texture;
  color_texture.adopt(0U);  // no-op release; never holds a name without a context
  EXPECT_EQ(color_texture.id(), 0U);
}

}  // namespace
}  // namespace pj::scene3d::gl
