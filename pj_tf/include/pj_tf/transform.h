#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>

#include "pj_base/time.hpp"

namespace pj::tf {

// An absolute frame stamp; being a time_point, not a Duration, TimePoint + TimePoint does not compile.
using TimePoint = PJ::Timepoint;

// A rigid SE(3) pose. `q` must be a unit quaternion: inverse() uses the conjugate and operator*/matrix() assume
// |q| == 1, so a non-unit q silently gives wrong results. TransformBuffer normalizes on write; the constructor
// does not.
struct Transform {
  glm::dvec3 t{0.0};
  glm::dquat q{1.0, 0.0, 0.0, 0.0};

  Transform() = default;
  Transform(const glm::dvec3& tr, const glm::dquat& rot) : t(tr), q(rot) {}

  static Transform identity() {
    return {};
  }

  Transform inverse() const {
    const glm::dquat qi = glm::conjugate(q);
    return {-(qi * t), qi};
  }

  Transform operator*(const Transform& rhs) const {
    return {t + q * rhs.t, q * rhs.q};
  }

  glm::dvec3 operator*(const glm::dvec3& p) const {
    return t + q * p;
  }

  glm::dmat4 matrix() const {
    glm::dmat4 m = glm::mat4_cast(q);
    m[3] = glm::dvec4(t, 1.0);
    return m;
  }

  /// Exact (bitwise-value) equality: a cache key, not a tolerance test.
  bool operator==(const Transform&) const = default;
};

struct StampedTransform {
  TimePoint stamp;
  std::string parent_frame;
  std::string child_frame;
  Transform transform;
};

}  // namespace pj::tf
