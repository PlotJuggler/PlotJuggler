// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QPointer>
#include <cstdint>

namespace pj::scene3d::gl {

// RAII 2D texture. Mirrors gl::Buffer's ownership model (move-only, lazy gen on
// first use, deleted only when the owning context — or a sharing one — is
// current). upload()/uploadSub() keep the existing R8 occupancy-grid path;
// allocate() creates arbitrary single-sample 2D textures for render targets
// such as RGBA16F/depth.
class Texture {
 public:
  Texture();
  ~Texture();

  Texture(Texture&& other) noexcept;
  Texture& operator=(Texture&& other) noexcept;

  Texture(const Texture&) = delete;
  Texture& operator=(const Texture&) = delete;

  [[nodiscard]] GLuint id() const noexcept;

  // (Re)allocate as a width x height R8 texture and upload width*height bytes.
  void upload(uint32_t width, uint32_t height, const uint8_t* data);
  // (Re)allocate as width x height with `internal_format` and upload the full
  // image from `data` (row-major, tightly packed: GL_UNPACK_ALIGNMENT = 1), the
  // 2D twin of Texture3D::upload. Sets GL_NEAREST min/mag and CLAMP_TO_EDGE.
  // `data` may be null to allocate uninitialized.
  void upload(GLenum internal_format, GLenum format, GLenum type, uint32_t width, uint32_t height, const void* data);
  // Re-fill the whole image in place (glTexSubImage2D, no re-allocation) of a
  // texture created by that upload() at the SAME width x height; `format`/`type`
  // describe `data`. Like uploadSub(), silently no-ops when never uploaded.
  void overwrite(GLenum format, GLenum type, uint32_t width, uint32_t height, const void* data);
  // Patch a sub-rectangle. The texture must already be upload()ed at the
  // matching full dimensions; this SILENTLY NO-OPS if upload() never ran (the
  // one operation in the wrapper family that drops work rather than lazily
  // creating). `data` is row-major width*height bytes.
  void uploadSub(int32_t x, int32_t y, uint32_t width, uint32_t height, const uint8_t* data);

  // Allocate storage for an uninitialized single-sample 2D texture. `filter` is
  // the min/mag filter (GL_NEAREST default; GL_LINEAR for a render target that
  // gets downsampled by a fullscreen pass, e.g. supersampled scene color — but
  // never for a depth attachment, where averaging depth corrupts reconstruction).
  void allocate(GLenum internal_format, GLenum format, GLenum type, int width, int height, GLenum filter = GL_NEAREST);

  void bind(int unit);

 private:
  GLuint id_{0};
  QPointer<QOpenGLContext> owning_context_;
};

// Move-only owner for color 2D texture names adopted after pass-specific upload.
// Kept separate from the R8 occupancy-grid Texture so filtering, wrapping, and
// internal formats cannot accidentally cross-contaminate the two call sites.
// Same owning-context delete guard as the other wrappers.
class Texture2D {
 public:
  Texture2D();
  ~Texture2D();

  Texture2D(Texture2D&& other) noexcept;
  Texture2D& operator=(Texture2D&& other) noexcept;

  Texture2D(const Texture2D&) = delete;
  Texture2D& operator=(const Texture2D&) = delete;

  [[nodiscard]] GLuint id() const noexcept;
  // Take ownership of `id`. Deletes the previously owned name unless `id` matches
  // the current one; adopt(0) releases ownership (deletes the old name, owns
  // nothing). The owning context is recorded from the current context. Caller
  // must ensure `id` was created under the now-current context.
  void adopt(GLuint id) noexcept;

 private:
  GLuint id_{0};
  QPointer<QOpenGLContext> owning_context_;
};

// RAII 3D texture (GL_TEXTURE_3D). Same move-only, lazy-gen, owning-context-guard
// ownership model as the other gl:: wrappers. VoxelGridRenderPass uses one to hold
// a voxel field as a dense volume the vertex shader point-samples per instance via
// integer texelFetch (so filtering/wrapping must never blend across cells).
class Texture3D {
 public:
  Texture3D();
  ~Texture3D();

  Texture3D(Texture3D&& other) noexcept;
  Texture3D& operator=(Texture3D&& other) noexcept;

  Texture3D(const Texture3D&) = delete;
  Texture3D& operator=(const Texture3D&) = delete;

  [[nodiscard]] GLuint id() const noexcept;

  // (Re)allocate as width x height x depth with `internal_format` and upload the
  // full volume from `data` (laid out x-fastest, then y, then z — the SDK's dense
  // Z-Y-X voxel order). `data` may be null to allocate uninitialized. Sets
  // GL_NEAREST min/mag and CLAMP_TO_EDGE on S/T/R, and GL_UNPACK_ALIGNMENT = 1.
  void upload(
      GLenum internal_format, GLenum format, GLenum type, uint32_t width, uint32_t height, uint32_t depth,
      const void* data);

  void bind(int unit);

 private:
  GLuint id_{0};
  QPointer<QOpenGLContext> owning_context_;
};

}  // namespace pj::scene3d::gl
