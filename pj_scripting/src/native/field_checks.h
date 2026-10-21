// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"

// The argument checks the builders share. Each names the offending `field` (e.g. "sphere: 'center'").
namespace PJ::scripting::native {

/// The first failure among `checks`, else success. All checks are evaluated.
[[nodiscard]] inline Status firstFailure(std::initializer_list<Status> checks) {
  for (const Status& check : checks) {
    if (!check) {
      return check;
    }
  }
  return okStatus();
}

/// The rejection of `field` when any of `values` is NaN or infinite.
[[nodiscard]] inline Status checkFinite(std::string_view field, std::initializer_list<double> values) {
  for (const double value : values) {
    if (!std::isfinite(value)) {
      return unexpected(std::string(field) + " must be finite");
    }
  }
  return okStatus();
}

/// The rejection of `field` when `value` is not finite or is too large for the float32 it is stored as (the cast would
/// give infinity).
[[nodiscard]] inline Status checkFloat32(std::string_view field, double value) {
  if (Status finite = checkFinite(field, {value}); !finite) {
    return finite;
  }
  if (std::abs(value) > std::numeric_limits<float>::max()) {
    return unexpected(std::string(field) + " must fit a 32-bit float");
  }
  return okStatus();
}

/// `checkFinite` over every point (anything with x and y, and z when it has one).
template <class Point>
[[nodiscard]] Status checkFinitePoints(std::string_view field, const std::vector<Point>& points) {
  for (const Point& point : points) {
    Status finite = okStatus();
    if constexpr (requires { point.z; }) {
      finite = checkFinite(field, {point.x, point.y, point.z});
    } else {
      finite = checkFinite(field, {point.x, point.y});
    }
    if (!finite) {
      return finite;
    }
  }
  return okStatus();
}

/// The rejection of `field` unless `value` is finite and greater than 0.
[[nodiscard]] inline Status checkPositive(std::string_view field, double value) {
  if (!std::isfinite(value) || value <= 0.0) {
    return unexpected(std::string(field) + " must be a finite number greater than 0");
  }
  return okStatus();
}

/// The rejection of `field` when `text` is empty.
[[nodiscard]] inline Status checkNonEmpty(std::string_view field, std::string_view text) {
  if (text.empty()) {
    return unexpected(std::string(field) + " must not be empty");
  }
  return okStatus();
}

}  // namespace PJ::scripting::native
