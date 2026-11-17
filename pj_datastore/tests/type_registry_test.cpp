// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/type_registry.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "pj_base/expected.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"

namespace PJ {
namespace {

std::shared_ptr<TypeTreeNode> makePointSchema() {
  return makeStruct(
      "Point", {
                   makePrimitive("x", PrimitiveType::kFloat64),
                   makePrimitive("y", PrimitiveType::kFloat64),
               });
}

std::shared_ptr<TypeTreeNode> makePoint3dSchema() {
  return makeStruct(
      "Point3D", {
                     makePrimitive("x", PrimitiveType::kFloat64),
                     makePrimitive("y", PrimitiveType::kFloat64),
                     makePrimitive("z", PrimitiveType::kFloat64),
                 });
}

TEST(TypeRegistryTest, SchemaNameIsTheInverseOfFindByName) {
  TypeRegistry registry;
  auto id = registry.registerSchema("Point", makePointSchema());
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(registry.schemaName(*id), std::optional<std::string>("Point"));
  EXPECT_FALSE(registry.schemaName(*id + 1).has_value());
}

TEST(TypeRegistryTest, RegisterAndLookupById) {
  TypeRegistry registry;
  auto tree = makePointSchema();
  auto* raw_ptr = tree.get();

  auto result = registry.registerSchema("Point", tree);
  ASSERT_TRUE(result.has_value()) << result.error();

  const TypeTreeNode* looked_up = registry.lookup(*result);
  ASSERT_NE(looked_up, nullptr);
  EXPECT_EQ(looked_up, raw_ptr);
  EXPECT_EQ(looked_up->name, "Point");
  EXPECT_EQ(looked_up->kind, TypeKind::kStruct);
  EXPECT_EQ(looked_up->children.size(), 2);
}

TEST(TypeRegistryTest, RegisterAndFindByName) {
  TypeRegistry registry;
  auto tree = makePointSchema();

  auto result = registry.registerSchema("Point", tree);
  ASSERT_TRUE(result.has_value()) << result.error();

  auto found = registry.findByName("Point");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, *result);
}

TEST(TypeRegistryTest, RegisterDuplicateNameFails) {
  TypeRegistry registry;

  auto result1 = registry.registerSchema("Point", makePointSchema());
  ASSERT_TRUE(result1.has_value()) << result1.error();

  auto result2 = registry.registerSchema("Point", makePointSchema());
  ASSERT_FALSE(result2.has_value());
}

TEST(TypeRegistryTest, RegisterOrGetNewName) {
  TypeRegistry registry;

  auto result = registry.registerOrGet("Point", makePointSchema());
  ASSERT_TRUE(result.has_value()) << result.error();

  auto found = registry.findByName("Point");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, *result);

  const TypeTreeNode* looked_up = registry.lookup(*result);
  ASSERT_NE(looked_up, nullptr);
  EXPECT_EQ(looked_up->name, "Point");
}

TEST(TypeRegistryTest, RegisterOrGetExistingName) {
  TypeRegistry registry;

  auto result1 = registry.registerSchema("Point", makePointSchema());
  ASSERT_TRUE(result1.has_value()) << result1.error();

  // register_or_get should return the same ID, ignoring the new tree
  auto result2 = registry.registerOrGet("Point", makePoint3dSchema());
  ASSERT_TRUE(result2.has_value()) << result2.error();
  EXPECT_EQ(*result1, *result2);

  // The original tree should still be the one stored (2 fields, not 3)
  const TypeTreeNode* looked_up = registry.lookup(*result2);
  ASSERT_NE(looked_up, nullptr);
  EXPECT_EQ(looked_up->children.size(), 2);
}

TEST(TypeRegistryTest, LookupUnknownIdReturnsNullptr) {
  TypeRegistry registry;
  EXPECT_EQ(registry.lookup(999), nullptr);
}

TEST(TypeRegistryTest, FindByNameUnknownReturnsNullopt) {
  TypeRegistry registry;
  auto found = registry.findByName("NonExistent");
  EXPECT_FALSE(found.has_value());
}

TEST(TypeRegistryTest, EvolveSchemaAdditiveChange) {
  TypeRegistry registry;

  auto original = makePointSchema();
  auto result = registry.registerSchema("Point", original);
  ASSERT_TRUE(result.has_value()) << result.error();
  SchemaId id = *result;

  auto evolved = makePoint3dSchema();
  auto* evolved_ptr = evolved.get();
  PJ::Status status = registry.evolveSchema(id, evolved);
  ASSERT_TRUE(status.has_value()) << status.error();

  const TypeTreeNode* looked_up = registry.lookup(id);
  ASSERT_NE(looked_up, nullptr);
  EXPECT_EQ(looked_up, evolved_ptr);
  EXPECT_EQ(looked_up->children.size(), 3);
}

TEST(TypeRegistryTest, EvolveSchemaRemovedFieldFails) {
  TypeRegistry registry;

  auto original = makePoint3dSchema();
  auto result = registry.registerSchema("Point3D", original);
  ASSERT_TRUE(result.has_value()) << result.error();

  auto reduced = makePointSchema();
  PJ::Status status = registry.evolveSchema(*result, reduced);
  ASSERT_FALSE(status.has_value());
}

TEST(TypeRegistryTest, EvolveSchemaTypeChangeFails) {
  TypeRegistry registry;

  auto original = makePointSchema();
  auto result = registry.registerSchema("Point", original);
  ASSERT_TRUE(result.has_value()) << result.error();

  auto changed = makeStruct(
      "Point", {
                   makePrimitive("x", PrimitiveType::kInt32),
                   makePrimitive("y", PrimitiveType::kFloat64),
                   makePrimitive("z", PrimitiveType::kFloat64),
               });
  PJ::Status status = registry.evolveSchema(*result, changed);
  ASSERT_FALSE(status.has_value());
}

TEST(TypeRegistryTest, EvolveSchemaUnknownIdFails) {
  TypeRegistry registry;

  PJ::Status status = registry.evolveSchema(999, makePointSchema());
  ASSERT_FALSE(status.has_value());
}

TEST(TypeRegistryTest, MultipleSchemas) {
  TypeRegistry registry;

  auto tree_a = makePrimitive("temp", PrimitiveType::kFloat32);
  auto tree_b = makePointSchema();
  auto tree_c = makeStruct(
      "Pose", {
                  makePrimitive("frame", PrimitiveType::kString),
                  makeStruct(
                      "position",
                      {
                          makePrimitive("x", PrimitiveType::kFloat64),
                          makePrimitive("y", PrimitiveType::kFloat64),
                          makePrimitive("z", PrimitiveType::kFloat64),
                      }),
              });

  auto* raw_a = tree_a.get();
  auto* raw_b = tree_b.get();
  auto* raw_c = tree_c.get();

  auto id_a = registry.registerSchema("Temperature", tree_a);
  auto id_b = registry.registerSchema("Point", tree_b);
  auto id_c = registry.registerSchema("Pose", tree_c);

  ASSERT_TRUE(id_a.has_value()) << id_a.error();
  ASSERT_TRUE(id_b.has_value()) << id_b.error();
  ASSERT_TRUE(id_c.has_value()) << id_c.error();

  EXPECT_NE(*id_a, *id_b);
  EXPECT_NE(*id_a, *id_c);
  EXPECT_NE(*id_b, *id_c);

  EXPECT_EQ(registry.lookup(*id_a), raw_a);
  EXPECT_EQ(registry.lookup(*id_b), raw_b);
  EXPECT_EQ(registry.lookup(*id_c), raw_c);

  auto found_a = registry.findByName("Temperature");
  auto found_b = registry.findByName("Point");
  auto found_c = registry.findByName("Pose");
  ASSERT_TRUE(found_a.has_value());
  ASSERT_TRUE(found_b.has_value());
  ASSERT_TRUE(found_c.has_value());
  EXPECT_EQ(*found_a, *id_a);
  EXPECT_EQ(*found_b, *id_b);
  EXPECT_EQ(*found_c, *id_c);
}

}  // namespace
}  // namespace PJ
