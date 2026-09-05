// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/gl/texture.h"

#include <utility>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d::gl {

Texture::Texture() = default;

Texture::~Texture() {
  if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteTextures(1, &id_); });
  }
}

Texture::Texture(Texture&& other) noexcept
    : id_(std::exchange(other.id_, 0U)), owning_context_(std::exchange(other.owning_context_, nullptr)) {}

Texture& Texture::operator=(Texture&& other) noexcept {
  if (this != &other) {
    if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
      withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteTextures(1, &id_); });
    }
    id_ = std::exchange(other.id_, 0U);
    owning_context_ = std::exchange(other.owning_context_, nullptr);
  }
  return *this;
}

GLuint Texture::id() const noexcept {
  return id_;
}

void Texture::upload(uint32_t width, uint32_t height, const uint8_t* data) {
  withGlFunctions([this, width, height, data](auto& functions) {
    if (id_ == 0U) {
      functions.glGenTextures(1, &id_);
      owning_context_ = QOpenGLContext::currentContext();
    }
    functions.glBindTexture(GL_TEXTURE_2D, id_);
    functions.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);  // R8 rows are not 4-byte aligned
    functions.glTexImage2D(
        GL_TEXTURE_2D, 0, GL_R8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RED, GL_UNSIGNED_BYTE,
        data);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  });
}

void Texture::upload(
    GLenum internal_format, GLenum format, GLenum type, uint32_t width, uint32_t height, const void* data) {
  if (width == 0U || height == 0U) {
    return;
  }
  withGlFunctions([this, internal_format, format, type, width, height, data](auto& functions) {
    if (id_ == 0U) {
      functions.glGenTextures(1, &id_);
      owning_context_ = QOpenGLContext::currentContext();
    }
    functions.glBindTexture(GL_TEXTURE_2D, id_);
    functions.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    functions.glTexImage2D(
        GL_TEXTURE_2D, 0, static_cast<GLint>(internal_format), static_cast<GLsizei>(width),
        static_cast<GLsizei>(height), 0, format, type, data);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  });
}

void Texture::overwrite(GLenum format, GLenum type, uint32_t width, uint32_t height, const void* data) {
  if (id_ == 0U) {
    return;
  }
  withGlFunctions([this, format, type, width, height, data](auto& functions) {
    functions.glBindTexture(GL_TEXTURE_2D, id_);
    functions.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    functions.glTexSubImage2D(
        GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), format, type, data);
  });
}

void Texture::uploadSub(int32_t x, int32_t y, uint32_t width, uint32_t height, const uint8_t* data) {
  if (id_ == 0U) {
    return;  // not allocated yet — caller must upload() first
  }
  withGlFunctions([this, x, y, width, height, data](auto& functions) {
    functions.glBindTexture(GL_TEXTURE_2D, id_);
    functions.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    functions.glTexSubImage2D(
        GL_TEXTURE_2D, 0, x, y, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RED, GL_UNSIGNED_BYTE,
        data);
  });
}

void Texture::allocate(GLenum internal_format, GLenum format, GLenum type, int width, int height, GLenum filter) {
  if (width <= 0 || height <= 0) {
    return;
  }

  withGlFunctions([this, internal_format, format, type, width, height, filter](auto& functions) {
    if (id_ == 0U) {
      functions.glGenTextures(1, &id_);
      owning_context_ = QOpenGLContext::currentContext();
    }
    functions.glBindTexture(GL_TEXTURE_2D, id_);
    functions.glTexImage2D(
        GL_TEXTURE_2D, 0, static_cast<GLint>(internal_format), static_cast<GLsizei>(width),
        static_cast<GLsizei>(height), 0, format, type, nullptr);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(filter));
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  });
}

void Texture::bind(int unit) {
  withGlFunctions([this, unit](auto& functions) {
    functions.glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    functions.glBindTexture(GL_TEXTURE_2D, id_);
  });
}

Texture2D::Texture2D() = default;

Texture2D::~Texture2D() {
  if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteTextures(1, &id_); });
  }
}

Texture2D::Texture2D(Texture2D&& other) noexcept
    : id_(std::exchange(other.id_, 0U)), owning_context_(std::exchange(other.owning_context_, nullptr)) {}

Texture2D& Texture2D::operator=(Texture2D&& other) noexcept {
  if (this != &other) {
    if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
      withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteTextures(1, &id_); });
    }
    id_ = std::exchange(other.id_, 0U);
    owning_context_ = std::exchange(other.owning_context_, nullptr);
  }
  return *this;
}

GLuint Texture2D::id() const noexcept {
  return id_;
}

void Texture2D::adopt(GLuint id) noexcept {
  if (id_ != 0U && id_ != id && deleteAllowedInCurrentContext(owning_context_)) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteTextures(1, &id_); });
  }
  id_ = id;
  owning_context_ = id == 0U ? nullptr : QOpenGLContext::currentContext();
}

Texture3D::Texture3D() = default;

Texture3D::~Texture3D() {
  if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteTextures(1, &id_); });
  }
}

Texture3D::Texture3D(Texture3D&& other) noexcept
    : id_(std::exchange(other.id_, 0U)), owning_context_(std::exchange(other.owning_context_, nullptr)) {}

Texture3D& Texture3D::operator=(Texture3D&& other) noexcept {
  if (this != &other) {
    if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
      withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteTextures(1, &id_); });
    }
    id_ = std::exchange(other.id_, 0U);
    owning_context_ = std::exchange(other.owning_context_, nullptr);
  }
  return *this;
}

GLuint Texture3D::id() const noexcept {
  return id_;
}

void Texture3D::upload(
    GLenum internal_format, GLenum format, GLenum type, uint32_t width, uint32_t height, uint32_t depth,
    const void* data) {
  if (width == 0U || height == 0U || depth == 0U) {
    return;
  }
  withGlFunctions([this, internal_format, format, type, width, height, depth, data](auto& functions) {
    if (id_ == 0U) {
      functions.glGenTextures(1, &id_);
      owning_context_ = QOpenGLContext::currentContext();
    }
    functions.glBindTexture(GL_TEXTURE_3D, id_);
    functions.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);  // R8 / tightly-packed rows aren't 4-byte aligned
    functions.glTexImage3D(
        GL_TEXTURE_3D, 0, static_cast<GLint>(internal_format), static_cast<GLsizei>(width),
        static_cast<GLsizei>(height), static_cast<GLsizei>(depth), 0, format, type, data);
    functions.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    functions.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    functions.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  });
}

void Texture3D::bind(int unit) {
  withGlFunctions([this, unit](auto& functions) {
    functions.glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    functions.glBindTexture(GL_TEXTURE_3D, id_);
  });
}

}  // namespace pj::scene3d::gl
