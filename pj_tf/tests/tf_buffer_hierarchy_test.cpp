// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

#include "pj_tf/tf_buffer.h"

namespace PJ {
namespace {

using pj::tf::FrameRow;
using pj::tf::StampedTransform;
using pj::tf::TimePoint;
using pj::tf::Transform;
using pj::tf::TransformBuffer;
using namespace std::chrono_literals;

Transform identity() {
  return Transform{{0.0, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}};
}

StampedTransform makeStatic(const std::string& parent, const std::string& child) {
  return StampedTransform{TimePoint{}, parent, child, identity()};
}

// ----------------------- getFrameHierarchy (flat) -----------------------

TEST(TfBufferHierarchy, EmptyBufferReturnsEmptyVector) {
  TransformBuffer buf;
  EXPECT_TRUE(buf.getFrameHierarchy().empty());
}

TEST(TfBufferHierarchy, SingleEdgeProducesRootAndChild) {
  TransformBuffer buf;
  (void)buf.setTransform(makeStatic("odom", "base_link"));

  const auto rows = buf.getFrameHierarchy();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].name, "odom");
  EXPECT_EQ(rows[0].depth, 0);
  EXPECT_EQ(rows[1].name, "base_link");
  EXPECT_EQ(rows[1].depth, 1);
}

TEST(TfBufferHierarchy, DisconnectedForestsKeepBothRootsAtDepthZero) {
  TransformBuffer buf;
  (void)buf.setTransform(makeStatic("map", "robot"));
  (void)buf.setTransform(makeStatic("world", "drone"));

  const auto rows = buf.getFrameHierarchy();
  ASSERT_EQ(rows.size(), 4U);
  EXPECT_EQ(rows[0].name, "map");
  EXPECT_EQ(rows[0].depth, 0);
  EXPECT_EQ(rows[1].name, "robot");
  EXPECT_EQ(rows[1].depth, 1);
  EXPECT_EQ(rows[2].name, "world");
  EXPECT_EQ(rows[2].depth, 0);
  EXPECT_EQ(rows[3].name, "drone");
  EXPECT_EQ(rows[3].depth, 1);
}

TEST(TfBufferHierarchy, ChildrenAtSameDepthAreAlphabetical) {
  TransformBuffer buf;
  // Insert in non-alphabetical order; expect alphabetical output.
  (void)buf.setTransform(makeStatic("base_link", "z_link"));
  (void)buf.setTransform(makeStatic("base_link", "a_link"));
  (void)buf.setTransform(makeStatic("base_link", "m_link"));
  (void)buf.setTransform(makeStatic("odom", "base_link"));

  const auto rows = buf.getFrameHierarchy();
  ASSERT_EQ(rows.size(), 5U);
  EXPECT_EQ(rows[0].name, "odom");
  EXPECT_EQ(rows[1].name, "base_link");
  EXPECT_EQ(rows[2].name, "a_link");
  EXPECT_EQ(rows[3].name, "m_link");
  EXPECT_EQ(rows[4].name, "z_link");
}

TEST(TfBufferHierarchy, MixedCaseSiblingsSortCaseInsensitively) {
  TransformBuffer buf;
  // Capital-O frames interleave with lowercase siblings instead of clustering ahead of them (uppercase < lowercase
  // in ASCII).
  (void)buf.setTransform(makeStatic("base_link", "Omniwheel_1"));
  (void)buf.setTransform(makeStatic("base_link", "imu_link"));
  (void)buf.setTransform(makeStatic("base_link", "Omniwheel_2"));
  (void)buf.setTransform(makeStatic("base_link", "rail_left"));
  (void)buf.setTransform(makeStatic("odom", "base_link"));

  const auto rows = buf.getFrameHierarchy();
  ASSERT_EQ(rows.size(), 6U);
  EXPECT_EQ(rows[0].name, "odom");
  EXPECT_EQ(rows[1].name, "base_link");
  // Children sorted case-insensitively: imu < Omniwheel_1 < Omniwheel_2 < rail
  EXPECT_EQ(rows[2].name, "imu_link");
  EXPECT_EQ(rows[3].name, "Omniwheel_1");
  EXPECT_EQ(rows[4].name, "Omniwheel_2");
  EXPECT_EQ(rows[5].name, "rail_left");
}

TEST(TfBufferHierarchy, FlatWalkIsCycleSafe) {
  TransformBuffer buf;
  (void)buf.setTransform(makeStatic("a", "b"));
  // A deep linear chain; cycles are in ParentCycleStillSurfacesAllFrames.
  (void)buf.setTransform(makeStatic("b", "c"));
  (void)buf.setTransform(makeStatic("c", "d"));

  const auto rows = buf.getFrameHierarchy();
  ASSERT_EQ(rows.size(), 4U);
  EXPECT_EQ(rows[0].name, "a");
  EXPECT_EQ(rows[3].name, "d");
  EXPECT_EQ(rows[3].depth, 3);
}

// ----------------------- getFrameHierarchyJson (nested) -----------------------

TEST(TfBufferHierarchyJson, EmptyBufferReturnsEmptyArray) {
  TransformBuffer buf;
  const auto json = buf.getFrameHierarchyJson();
  EXPECT_TRUE(json.is_array());
  EXPECT_EQ(json.size(), 0U);
}

TEST(TfBufferHierarchyJson, SingleEdgeNestsChild) {
  TransformBuffer buf;
  (void)buf.setTransform(makeStatic("odom", "base_link"));

  const auto json = buf.getFrameHierarchyJson();
  const auto expected = nlohmann::json::parse(R"([
    {"name":"odom","children":[
      {"name":"base_link","children":[]}
    ]}
  ])");
  EXPECT_EQ(json, expected);
}

TEST(TfBufferHierarchyJson, DisconnectedForestsAppearAsTwoTopLevelEntries) {
  TransformBuffer buf;
  (void)buf.setTransform(makeStatic("map", "robot"));
  (void)buf.setTransform(makeStatic("world", "drone"));

  const auto json = buf.getFrameHierarchyJson();
  ASSERT_TRUE(json.is_array());
  ASSERT_EQ(json.size(), 2U);
  EXPECT_EQ(json[0]["name"], "map");
  EXPECT_EQ(json[0]["children"][0]["name"], "robot");
  EXPECT_EQ(json[1]["name"], "world");
  EXPECT_EQ(json[1]["children"][0]["name"], "drone");
}

TEST(TfBufferHierarchyJson, ChildrenAlphabeticalAtEachDepth) {
  TransformBuffer buf;
  (void)buf.setTransform(makeStatic("base_link", "z_link"));
  (void)buf.setTransform(makeStatic("base_link", "a_link"));
  (void)buf.setTransform(makeStatic("base_link", "m_link"));
  (void)buf.setTransform(makeStatic("odom", "base_link"));

  const auto json = buf.getFrameHierarchyJson();
  ASSERT_EQ(json.size(), 1U);
  const auto& base_link_children = json[0]["children"][0]["children"];
  ASSERT_EQ(base_link_children.size(), 3U);
  EXPECT_EQ(base_link_children[0]["name"], "a_link");
  EXPECT_EQ(base_link_children[1]["name"], "m_link");
  EXPECT_EQ(base_link_children[2]["name"], "z_link");
}

TEST(TfBufferHierarchyJson, DeepChainTerminates) {
  TransformBuffer buf;
  (void)buf.setTransform(makeStatic("a", "b"));
  (void)buf.setTransform(makeStatic("b", "c"));
  (void)buf.setTransform(makeStatic("c", "d"));

  const auto json = buf.getFrameHierarchyJson();
  ASSERT_EQ(json.size(), 1U);
  EXPECT_EQ(json[0]["name"], "a");
  EXPECT_EQ(json[0]["children"][0]["name"], "b");
  EXPECT_EQ(json[0]["children"][0]["children"][0]["name"], "c");
  EXPECT_EQ(json[0]["children"][0]["children"][0]["children"][0]["name"], "d");
  EXPECT_TRUE(json[0]["children"][0]["children"][0]["children"][0]["children"].empty());
}

// ----------------------- cycles and case collisions -----------------------

// Collect the names emitted by getFrameHierarchy (flat) into a set.
std::set<std::string> hierarchyNames(TransformBuffer& buf) {
  std::set<std::string> names;
  for (const auto& row : buf.getFrameHierarchy()) {
    names.insert(row.name);
  }
  return names;
}

// Collect every "name" appearing anywhere in the nested JSON forest.
std::set<std::string> jsonNames(const nlohmann::json& forest) {
  std::set<std::string> names;
  std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& node) {
    names.insert(node.at("name").get<std::string>());
    for (const auto& child : node.at("children")) {
      walk(child);
    }
  };
  for (const auto& root : forest) {
    walk(root);
  }
  return names;
}

// A two-node parent cycle (map<->odom, accepted because each child is parented once) leaves every frame with no
// reachable root: its members are surfaced at depth 0, so the whole tree does not vanish.
TEST(TfBufferHierarchy, ParentCycleStillSurfacesAllFrames) {
  TransformBuffer buf;
  (void)buf.setTransform(makeStatic("map", "odom"));        // parents_[odom] = map
  (void)buf.setTransform(makeStatic("odom", "map"));        // parents_[map]  = odom (forms cycle)
  (void)buf.setTransform(makeStatic("odom", "base_link"));  // child hanging under odom

  const auto names = hierarchyNames(buf);
  EXPECT_EQ(names.count("map"), 1U);
  EXPECT_EQ(names.count("odom"), 1U);
  EXPECT_EQ(names.count("base_link"), 1U);

  const auto json_names = jsonNames(buf.getFrameHierarchyJson());
  EXPECT_EQ(json_names.count("map"), 1U);
  EXPECT_EQ(json_names.count("odom"), 1U);
  EXPECT_EQ(json_names.count("base_link"), 1U);
}

// Sibling frames differing only by ASCII case ('Lidar' vs 'lidar') both appear: FrameNameLess tie-breaks
// case-sensitively.
TEST(TfBufferHierarchy, CaseCollidingSiblingsBothAppear) {
  TransformBuffer buf;
  (void)buf.setTransform(makeStatic("base_link", "Lidar"));
  (void)buf.setTransform(makeStatic("base_link", "lidar"));

  const auto names = hierarchyNames(buf);
  EXPECT_EQ(names.count("Lidar"), 1U);
  EXPECT_EQ(names.count("lidar"), 1U);
  // base_link + both case variants = 3 distinct frames in the flat walk.
  EXPECT_EQ(buf.getFrameHierarchy().size(), 3U);

  const auto json_names = jsonNames(buf.getFrameHierarchyJson());
  EXPECT_EQ(json_names.count("Lidar"), 1U);
  EXPECT_EQ(json_names.count("lidar"), 1U);
}

}  // namespace
}  // namespace PJ
