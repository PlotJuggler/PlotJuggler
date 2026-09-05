#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>
#include <optional>
#include <string>

#include "pj_base/expected.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"

namespace PJ {

/// Engine-wide schema registry: assigns SchemaId to named TypeTreeNode trees,
/// shared across topics. registerOrGet() supports late discovery (return existing
/// id by name); evolveSchema() permits additive-only changes (no field removal or
/// type change).
class TypeRegistry {
 public:
  TypeRegistry();
  ~TypeRegistry();
  TypeRegistry(TypeRegistry&&) noexcept;
  TypeRegistry& operator=(TypeRegistry&&) noexcept;

  TypeRegistry(const TypeRegistry&) = delete;
  TypeRegistry& operator=(const TypeRegistry&) = delete;

  // Register a known schema (from Protobuf, ROS, etc.)
  // Fails if schema_name already exists.
  [[nodiscard]] PJ::Expected<PJ::SchemaId> registerSchema(
      std::string schema_name, std::shared_ptr<PJ::TypeTreeNode> type_tree);

  // Late discovery: register from first message (JSON, etc.)
  // Returns existing schema ID if name already registered.
  [[nodiscard]] PJ::Expected<PJ::SchemaId> registerOrGet(
      std::string schema_name, std::shared_ptr<PJ::TypeTreeNode> type_tree);

  // Lookup by ID — returns nullptr if not found
  [[nodiscard]] const PJ::TypeTreeNode* lookup(PJ::SchemaId id) const;

  // Lookup by name — returns nullopt if not found
  [[nodiscard]] std::optional<PJ::SchemaId> findByName(std::string_view name) const;

  // Registered name for `id` (the inverse of findByName) — nullopt if not found.
  // Linear in the number of schemas; meant for UI/diagnostic lookups, not hot paths.
  [[nodiscard]] std::optional<std::string> schemaName(PJ::SchemaId id) const;

  // Schema evolution: add fields to existing schema (additive only).
  // Fails if: ID not found, existing fields changed type, fields removed.
  [[nodiscard]] PJ::Status evolveSchema(PJ::SchemaId id, std::shared_ptr<PJ::TypeTreeNode> updated_tree);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
