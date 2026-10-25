#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// URDF → RobotModel parser (QDomDocument-based). Parses <robot>/<link> and
// <joint>; per <visual>/<collision> it reads <origin>, <geometry>, and
// <material>. Multiple visuals/collisions per link are supported.
//
// <joint> is parsed into RobotModel::joints (name/type/parent/child/origin), but
// TF still owns kinematics: joints exist only so a FIXED joint can be injected as
// a static TF edge to bridge a frame the data never publishes (see
// robot_model_bridges.h). axis/limit/mimic are not parsed (movable-joint
// articulation is deferred).
//
// xacro is detected (a `<xacro:` element or a .xacro filename) and rejected with
// an explicit error string — never a cryptic XML parse failure. Format is
// inferred from the root element (<robot> ⇒ urdf).
//
// Mesh references are dispatched through the resolver: package:// enters the
// resolver chain, bare/file/http paths go through resolveUri's guard.

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "pj_scene3d_core/robot_model.h"
#include "urdf_package_resolver.h"

namespace pj::scene3d {

// Optional structural envelope for untrusted descriptions. A null limits
// pointer preserves the native parser's existing behavior; the browser passes
// an explicit bounded profile after applying the source-byte cap.
struct UrdfParseLimits {
  std::size_t max_links = 0;
  std::size_t max_joints = 0;
  std::size_t max_geometries = 0;
  std::size_t max_materials = 0;
  std::size_t max_string_bytes = 0;
};

// Parse `xml`. `urdf_dir` is the directory (or URL base) the URDF came from,
// used by the resolver for bare-path and ancestor resolution. `source_is_url`
// declares that the URDF itself was fetched over http(s) — which is what
// permits http(s) mesh refs and the URL-ancestor package heuristic (same-origin
// consent); it does not change how `xml` is parsed.
//
// Returns {model, ""} on success, or {nullopt, error} on failure (xacro,
// non-urdf root, malformed XML). `resolver` may be null (then mesh refs are
// recorded with resolved=false and resolution is skipped). `package_url_roots`
// is the model's explicit package mapping and `remote_only` its network-only
// policy (see UrdfPackageResolver::resolveUri).
std::pair<std::optional<RobotModel>, std::string> parseUrdf(
    const std::string& xml, UrdfPackageResolver* resolver, const std::string& urdf_dir, bool source_is_url = false,
    const std::string& filename = {}, const UrdfParseLimits* limits = nullptr,
    const QMap<QString, QUrl>& package_url_roots = {}, bool remote_only = false);

// Returns true if `xml` (and/or `filename`) is xacro and must be expanded first.
bool looksLikeXacro(const std::string& xml, const std::string& filename = {});

}  // namespace pj::scene3d
