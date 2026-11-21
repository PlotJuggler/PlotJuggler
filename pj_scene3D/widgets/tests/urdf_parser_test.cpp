// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "urdf_parser.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

#include "pj_scene3d_core/robot_model.h"
#include "urdf_package_resolver.h"

namespace pj::scene3d {
namespace {

#ifndef PJ_SCENE3D_FIXTURES_DIR
#error "PJ_SCENE3D_FIXTURES_DIR must be defined by the build"
#endif

std::string readFixture(const std::string& name) {
  std::ifstream in(std::string(PJ_SCENE3D_FIXTURES_DIR) + "/" + name, std::ios::binary);
  EXPECT_TRUE(in.good()) << "missing fixture: " << name;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

const RobotLink* findLink(const RobotModel& m, const std::string& name) {
  for (const auto& l : m.links) {
    if (l.name == name) {
      return &l;
    }
  }
  return nullptr;
}

const RobotJoint* findJoint(const RobotModel& m, const std::string& name) {
  for (const auto& j : m.joints) {
    if (j.name == name) {
      return &j;
    }
  }
  return nullptr;
}

TEST(UrdfParser, ParsesTwoLinkModel) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;  // no roots seeded ⇒ package:// will be unresolved
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), /*source_is_url=*/false);
  ASSERT_TRUE(model.has_value()) << err;
  EXPECT_TRUE(err.empty());

  EXPECT_EQ(model->root_link, "base_link");
  ASSERT_EQ(model->links.size(), 2u);
  EXPECT_NE(findLink(*model, "base_link"), nullptr);
  EXPECT_NE(findLink(*model, "arm_link"), nullptr);
}

TEST(UrdfParser, MultipleVisualsAndCollisionsPerLink) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), false);
  ASSERT_TRUE(model.has_value()) << err;

  const RobotLink* base = findLink(*model, "base_link");
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(base->visuals.size(), 2u);     // mesh + box
  EXPECT_EQ(base->collisions.size(), 1u);  // bare-relative mesh

  const RobotLink* arm = findLink(*model, "arm_link");
  ASSERT_NE(arm, nullptr);
  EXPECT_EQ(arm->visuals.size(), 1u);     // cylinder
  EXPECT_EQ(arm->collisions.size(), 1u);  // bare-relative-that-looks-like-package
}

TEST(UrdfParser, PrimitiveGeometriesParsed) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), false);
  ASSERT_TRUE(model.has_value()) << err;

  const RobotLink* base = findLink(*model, "base_link");
  ASSERT_NE(base, nullptr);
  ASSERT_TRUE(std::holds_alternative<GeomBox>(base->visuals[1].shape));
  const auto& box = std::get<GeomBox>(base->visuals[1].shape);
  EXPECT_DOUBLE_EQ(box.size.x, 0.3);
  EXPECT_DOUBLE_EQ(box.size.y, 0.2);
  EXPECT_DOUBLE_EQ(box.size.z, 0.1);

  const RobotLink* arm = findLink(*model, "arm_link");
  ASSERT_NE(arm, nullptr);
  ASSERT_TRUE(std::holds_alternative<GeomCylinder>(arm->visuals[0].shape));
  const auto& cyl = std::get<GeomCylinder>(arm->visuals[0].shape);
  EXPECT_DOUBLE_EQ(cyl.radius, 0.05);
  EXPECT_DOUBLE_EQ(cyl.length, 1.0);
}

TEST(UrdfParser, NamedMaterialResolvedAndInlineColor) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), false);
  ASSERT_TRUE(model.has_value()) << err;

  const RobotLink* base = findLink(*model, "base_link");
  ASSERT_NE(base, nullptr);
  EXPECT_TRUE(base->visuals[0].has_color);
  EXPECT_FLOAT_EQ(base->visuals[0].color.b, 1.0f);
  EXPECT_FLOAT_EQ(base->visuals[0].color.r, 0.0f);
  EXPECT_TRUE(base->visuals[1].has_color);
  EXPECT_FLOAT_EQ(base->visuals[1].color.r, 1.0f);
}

TEST(UrdfParser, OriginComposedTranslateThenRotate) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), false);
  ASSERT_TRUE(model.has_value()) << err;

  const RobotLink* base = findLink(*model, "base_link");
  ASSERT_NE(base, nullptr);
  EXPECT_DOUBLE_EQ(base->visuals[0].origin_xyz.z, 0.1);
  EXPECT_NEAR(base->visuals[0].origin_rpy.z, 1.5708, 1e-4);

  // originToMat4 = translate(xyz)*Rz: a +90deg yaw maps local +X to world +Y.
  const glm::dmat4 m = originToMat4(base->visuals[0].origin_xyz, base->visuals[0].origin_rpy);
  const glm::dvec4 x_axis = m * glm::dvec4(1, 0, 0, 0);
  EXPECT_NEAR(x_axis.x, 0.0, 1e-3);
  EXPECT_NEAR(x_axis.y, 1.0, 1e-3);
  EXPECT_DOUBLE_EQ(m[3][2], 0.1);
}

// The bare-path-that-looks-like-a-package case: "robotiq_arg85_description/.."
// has no package:// prefix → the guard resolves it against urdf_dir, never the
// package search.
TEST(UrdfParser, BarePathLooksLikePackageResolvesAgainstUrdfDir) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;
  const std::string urdf_dir = std::string(PJ_SCENE3D_FIXTURES_DIR);
  auto [model, err] = parseUrdf(xml, &resolver, urdf_dir, false);
  ASSERT_TRUE(model.has_value()) << err;

  const RobotLink* arm = findLink(*model, "arm_link");
  ASSERT_NE(arm, nullptr);
  const auto& mesh = std::get<GeomMesh>(arm->collisions[0].shape);
  EXPECT_EQ(mesh.filename, "robotiq_arg85_description/meshes/gripper.stl");
  EXPECT_TRUE(mesh.resolved);  // resolved as a bare relative path …
  EXPECT_EQ(mesh.resolved_path, urdf_dir + "/robotiq_arg85_description/meshes/gripper.stl");
  // The guard kept this bare path out of the package chain: no package is
  // recorded on the mesh. (demo_description, a real package:// ref, would be.)
  EXPECT_TRUE(mesh.unresolved_package.empty());
  EXPECT_EQ(mesh.issue, MeshResolveIssue::kNone);
}

TEST(UrdfParser, PackageRefRecordedUnresolvedWhenNoRoots) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;  // no roots, no attachments
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), false);
  ASSERT_TRUE(model.has_value()) << err;

  const RobotLink* base = findLink(*model, "base_link");
  ASSERT_NE(base, nullptr);
  const auto& mesh = std::get<GeomMesh>(base->visuals[0].shape);
  EXPECT_FALSE(mesh.resolved);
  EXPECT_EQ(mesh.unresolved_package, "demo_description");
  EXPECT_EQ(mesh.issue, MeshResolveIssue::kUnresolvedPackage);
}

TEST(UrdfParser, XacroRejectedWithExplicitError) {
  const std::string xml = readFixture("robot.urdf.xacro");
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), false);
  EXPECT_FALSE(model.has_value());
  EXPECT_NE(err.find("xacro"), std::string::npos) << err;
  // A xacro usually needs its launch arguments; the recorded topic is the reliable alternative.
  EXPECT_NE(err.find("/robot_description"), std::string::npos) << err;
}

TEST(UrdfParser, NonRobotRootRejected) {
  const std::string xml = R"(<?xml version="1.0"?><sdf version="1.6"><model/></sdf>)";
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, "", false);
  EXPECT_FALSE(model.has_value());
  EXPECT_NE(err.find("sdf"), std::string::npos) << err;
}

TEST(UrdfParser, MalformedXmlRejected) {
  const std::string xml = R"(<robot name="x"><link name="a")";  // truncated
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, "", false);
  EXPECT_FALSE(model.has_value());
  EXPECT_FALSE(err.empty());
}

TEST(UrdfParser, JointsParsedButCreateNoLinks) {
  // <joint> elements are recorded in model.joints (they bridge TF gaps) but contribute no <link>: TF owns kinematics.
  const std::string xml = R"(<?xml version="1.0"?>
    <robot name="jr">
      <link name="a"/>
      <joint name="a_to_b" type="fixed">
        <parent link="a"/><child link="b"/>
        <origin xyz="1 2 3" rpy="0 0 1.5707963267948966"/>
      </joint>
      <link name="b"/>
      <joint name="b_to_c" type="revolute">
        <parent link="b"/><child link="c"/>
        <origin xyz="0 0 0.5"/>
        <axis xyz="0 0 1"/>
      </joint>
      <link name="c"/>
    </robot>)";
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, "", false);
  ASSERT_TRUE(model.has_value()) << err;
  EXPECT_EQ(model->links.size(), 3u);  // a + b + c; the joints contributed no link

  ASSERT_EQ(model->joints.size(), 2u);
  const RobotJoint* fixed = findJoint(*model, "a_to_b");
  ASSERT_NE(fixed, nullptr);
  EXPECT_EQ(fixed->type, JointType::kFixed);
  EXPECT_EQ(fixed->parent, "a");
  EXPECT_EQ(fixed->child, "b");
  EXPECT_DOUBLE_EQ(fixed->origin_xyz.x, 1.0);
  EXPECT_DOUBLE_EQ(fixed->origin_xyz.y, 2.0);
  EXPECT_DOUBLE_EQ(fixed->origin_xyz.z, 3.0);
  EXPECT_NEAR(fixed->origin_rpy.z, 1.5707963267948966, 1e-9);

  const RobotJoint* movable = findJoint(*model, "b_to_c");
  ASSERT_NE(movable, nullptr);
  EXPECT_EQ(movable->type, JointType::kRevolute);  // classified, but not a fixed bridge
  EXPECT_EQ(movable->parent, "b");
  EXPECT_EQ(movable->child, "c");
  EXPECT_DOUBLE_EQ(movable->origin_xyz.z, 0.5);
}

TEST(UrdfParser, OversizedInputRejectedWithSizeInMessage) {
  // Build a string just over the 32 MiB cap by padding with XML comment content.
  // We don't attempt to parse real XML — the check fires before setContent.
  constexpr std::size_t kLimitBytes = 32u * 1024u * 1024u;
  const std::string prefix = "<?xml version=\"1.0\"?><robot name=\"x\"><!-- ";
  const std::string suffix = " --><link name=\"a\"/></robot>";
  const std::size_t pad_size = kLimitBytes + 1 - prefix.size() - suffix.size();
  std::string xml = prefix + std::string(pad_size, 'x') + suffix;
  ASSERT_GT(xml.size(), kLimitBytes);

  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, "", false);
  EXPECT_FALSE(model.has_value());
  EXPECT_NE(err.find("MiB"), std::string::npos) << "error was: " << err;
  EXPECT_NE(err.find("32"), std::string::npos) << "error was: " << err;
}

TEST(UrdfParser, OptionalStructuralLimitsRejectBeforeGrowingTheModel) {
  const std::string xml = R"(
    <robot name="bounded">
      <material name="red"><color rgba="1 0 0 1"/></material>
      <material name="blue"><color rgba="0 0 1 1"/></material>
      <link name="a">
        <visual><geometry><box size="1 1 1"/></geometry></visual>
        <collision><geometry><sphere radius="1"/></geometry></collision>
      </link>
      <link name="b"/>
      <joint name="a_to_b" type="fixed"><parent link="a"/><child link="b"/></joint>
      <joint name="b_to_a" type="fixed"><parent link="b"/><child link="a"/></joint>
    </robot>)";
  UrdfPackageResolver resolver;

  struct LimitCase {
    UrdfParseLimits limits;
    const char* expected_error;
  };
  const std::array<LimitCase, 4> cases{{
      {UrdfParseLimits{.max_links = 1}, "link limit"},
      {UrdfParseLimits{.max_joints = 1}, "joint limit"},
      {UrdfParseLimits{.max_geometries = 1}, "geometry limit"},
      {UrdfParseLimits{.max_materials = 1}, "material limit"},
  }};

  for (const auto& limit_case : cases) {
    auto [model, err] = parseUrdf(xml, &resolver, "", false, "", &limit_case.limits);
    EXPECT_FALSE(model.has_value()) << limit_case.expected_error;
    EXPECT_NE(err.find(limit_case.expected_error), std::string::npos) << "error was: " << err;
  }

  // The same input without the explicit browser envelope takes the native path.
  auto [model, err] = parseUrdf(xml, &resolver, "", false);
  ASSERT_TRUE(model.has_value()) << err;
  EXPECT_EQ(model->links.size(), 2u);
  EXPECT_EQ(model->joints.size(), 2u);
}

TEST(UrdfParser, OptionalStringLimitCoversNamesJointFieldsAndMeshReferences) {
  UrdfPackageResolver resolver;
  const UrdfParseLimits limits{.max_string_bytes = 4};

  const std::array<std::pair<std::string, std::string>, 4> cases{{
      {R"(<robot name="r"><link name="long_link"/></robot>)", "link name"},
      {R"(<robot name="r"><link name="a"/><link name="b"/><joint name="long_joint" type="fixed"><parent link="a"/><child link="b"/></joint></robot>)",
       "joint field"},
      {R"(<robot name="r"><link name="a"><visual><geometry><mesh filename="long.glb"/></geometry></visual></link></robot>)",
       "mesh reference"},
      {R"(<robot name="r"><link name="a"><visual><geometry><box size="1 1 1"/></geometry><material name="long_material"/></visual></link></robot>)",
       "material reference"},
  }};

  for (const auto& [xml, expected_error] : cases) {
    auto [model, err] = parseUrdf(xml, &resolver, "", false, "", &limits);
    EXPECT_FALSE(model.has_value()) << expected_error;
    EXPECT_NE(err.find(expected_error), std::string::npos) << "error was: " << err;
  }
}

TEST(UrdfParser, OptionalJointLimitCountsMalformedJointElements) {
  const std::string xml = R"(
    <robot name="bounded">
      <link name="a"/>
      <joint name="missing_child" type="fixed"><parent link="a"/></joint>
      <joint name="missing_parent" type="fixed"><child link="a"/></joint>
    </robot>)";
  UrdfPackageResolver resolver;
  const UrdfParseLimits limits{.max_joints = 1};

  auto [model, err] = parseUrdf(xml, &resolver, "", false, "", &limits);
  EXPECT_FALSE(model.has_value());
  EXPECT_NE(err.find("joint limit"), std::string::npos) << "error was: " << err;
}

}  // namespace
}  // namespace pj::scene3d
