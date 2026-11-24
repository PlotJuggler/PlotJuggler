#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Pure URDF data model (no Qt, assimp or GL). The URDF parser (pj_scene3d_widgets/urdf_parser) fills these structs and
// render passes consume them. A URDF is a frame→visual decoration map (links) posed by the live TF tree: TF owns
// kinematics. Joints are represented only so a FIXED joint can be injected as a static TF edge bridging a frame the
// data never publishes (e.g. a gripper mount); see robot_model_bridges.h. Movable joints are parsed and classified
// but not articulated.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

// originToMat4 builds the URDF RPY rotation via glm::eulerAngleZYX, which lives in
// the experimental gtx tree (the only gtx header in pj_scene3D); the define is the
// supported opt-in for that helper.
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>

namespace pj::scene3d {

// Why a mesh reference did not resolve to a loadable path. Pure data: the resolver sets it and the layer reports it in
// the status line, without depending on Qt.
enum class MeshResolveIssue : std::uint8_t {
  kNone,               // resolved, or no resolution attempted
  kBlockedHttp,        // http(s) ref dropped because the source is not a URL
  kMalformedRef,       // package:// URI with no "/<rel>" part
  kNoAnchor,           // bare relative ref but no urdf_dir to anchor it
  kUnresolvedPackage,  // package:// chain exhausted (unknown package)
  kMissingFile,        // an absolute/anchored path that does not exist on disk
};

// <box size="x y z"/>
struct GeomBox {
  glm::dvec3 size{1.0, 1.0, 1.0};
};

// <cylinder radius="r" length="l"/> (URDF cylinders are Z-aligned)
struct GeomCylinder {
  double radius{1.0};
  double length{1.0};
};

// <sphere radius="r"/>
struct GeomSphere {
  double radius{1.0};
};

// <mesh filename="..." scale="x y z"/>. `filename` is the verbatim reference as
// written in the URDF (package://, file://, http(s)://, or bare relative). The
// resolver turns this into a loadable path; `resolved_path` is filled in once a
// resolution attempt has run ("" + resolved=false ⇒ unresolved, not drawn).
struct GeomMesh {
  std::string filename;  // verbatim reference string from the URDF
  glm::dvec3 scale{1.0, 1.0, 1.0};
  std::string resolved_path;  // empty until resolved
  bool resolved{false};
  // Filled by the resolver (via urdf_parser) when !resolved: why it failed and,
  // for a package:// miss, the offending package name (empty otherwise). The
  // layer derives its per-model unresolved-package list and status hints from
  // these instead of a resolver-global tally (the resolver is dock-shared).
  MeshResolveIssue issue{MeshResolveIssue::kNone};
  std::string unresolved_package;  // package name on kUnresolvedPackage; else ""
};

using GeomShape = std::variant<GeomBox, GeomCylinder, GeomSphere, GeomMesh>;

// One <visual> or <collision> element: a shape, its origin offset within the
// link frame (translation + RPY), and an optional material color.
struct LinkGeom {
  GeomShape shape;
  glm::dvec3 origin_xyz{0.0, 0.0, 0.0};
  glm::dvec3 origin_rpy{0.0, 0.0, 0.0};  // roll, pitch, yaw (radians)
  glm::vec4 color{0.7f, 0.7f, 0.7f, 1.0f};
  bool has_color{false};  // true when an explicit <material><color> applied
};

// One <link>: its name (== TF frame name) plus all visual & collision geoms. A link is posed by the TF frame of the
// same name; joints (below) only matter when the data's TF lacks an edge a FIXED joint can supply.
struct RobotLink {
  std::string name;
  std::vector<LinkGeom> visuals;
  std::vector<LinkGeom> collisions;
};

// URDF joint type. Only kFixed is consumed (as a static TF bridge); the movable kinds are parsed and classified but
// unused. kOther covers any unrecognized type string.
enum class JointType : std::uint8_t { kFixed, kRevolute, kContinuous, kPrismatic, kFloating, kPlanar, kOther };

// One <joint>: the kinematic edge `parent` → `child` (link names, == TF frame names) with its static origin offset
// (translation + RPY). axis/limit/mimic are not parsed: only articulating movable joints would need them.
struct RobotJoint {
  std::string name;
  std::string parent;
  std::string child;
  JointType type{JointType::kOther};
  glm::dvec3 origin_xyz{0.0, 0.0, 0.0};
  glm::dvec3 origin_rpy{0.0, 0.0, 0.0};  // roll, pitch, yaw (radians)
};

// The parsed robot: every link keyed by name, the inferred root link name (first
// link declared; URDF has no explicit root without walking the joint graph), and
// every joint. Joints are used only to bridge TF gaps (see robot_model_bridges.h);
// link poses still come from the live TF tree.
struct RobotModel {
  std::string root_link;
  std::vector<RobotLink> links;
  std::vector<RobotJoint> joints;
};

// Compose a link/visual origin into a 4x4 matrix: translate(xyz) * Rz * Ry * Rx
// (URDF RPY convention: extrinsic (fixed-axis) X-Y-Z == intrinsic Z-Y'-X'',
// applied as Rz(yaw)*Ry(pitch)*Rx(roll)). The product order is load-bearing — a
// "fix" toward Rx*Ry*Rz would silently break every URDF visual origin.
inline glm::dmat4 originToMat4(const glm::dvec3& xyz, const glm::dvec3& rpy) {
  // eulerAngleZYX(yaw, pitch, roll) == Rz(yaw)*Ry(pitch)*Rx(roll), the URDF order.
  glm::dmat4 m = glm::eulerAngleZYX(rpy.z, rpy.y, rpy.x);
  m[3] = glm::dvec4(xyz, 1.0);
  return m;
}

// Quaternion form of a URDF RPY rotation, for the (translation + quaternion)
// StampedTransform used by the TF buffer. Built via quat_cast of the SAME
// eulerAngleZYX matrix originToMat4 uses, so the quaternion and the matrix share
// one rotation convention by construction — keep it that way (a hand-rolled
// qz*qy*qx could drift from originToMat4 and silently mis-orient bridged frames).
inline glm::dquat rpyToQuat(const glm::dvec3& rpy) {
  return glm::normalize(glm::quat_cast(glm::eulerAngleZYX(rpy.z, rpy.y, rpy.x)));
}

}  // namespace pj::scene3d
