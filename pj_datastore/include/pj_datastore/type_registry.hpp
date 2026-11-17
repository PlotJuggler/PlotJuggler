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

/// Engine-wide schema registry, shared across topics: assigns a SchemaId to each named TypeTreeNode tree. No locking:
/// hold DataEngine::lockEngine() across every use, and do not keep the TypeTreeNode* from lookup() past it
/// (evolveSchema() replaces the tree).
class TypeRegistry {
 public:
  TypeRegistry();
  ~TypeRegistry();
  TypeRegistry(TypeRegistry&&) noexcept;
  TypeRegistry& operator=(TypeRegistry&&) noexcept;

  TypeRegistry(const TypeRegistry&) = delete;
  TypeRegistry& operator=(const TypeRegistry&) = delete;

  /// Fails if `schema_name` is already registered.
  [[nodiscard]] PJ::Expected<PJ::SchemaId> registerSchema(
      std::string schema_name, std::shared_ptr<PJ::TypeTreeNode> type_tree);

  /// Returns the id of an already registered `schema_name`, ignoring `type_tree`.
  [[nodiscard]] PJ::Expected<PJ::SchemaId> registerOrGet(
      std::string schema_name, std::shared_ptr<PJ::TypeTreeNode> type_tree);

  [[nodiscard]] const PJ::TypeTreeNode* lookup(PJ::SchemaId id) const;

  [[nodiscard]] std::optional<PJ::SchemaId> findByName(std::string_view name) const;

  /// Linear in the number of schemas: for UI and diagnostics, not hot paths.
  [[nodiscard]] std::optional<std::string> schemaName(PJ::SchemaId id) const;

  /// Replaces the tree when it keeps every existing leaf path with its type (an array counts as one leaf); added
  /// fields are accepted. Fails for an unknown id, a removed leaf or a changed type.
  [[nodiscard]] PJ::Status evolveSchema(PJ::SchemaId id, std::shared_ptr<PJ::TypeTreeNode> updated_tree);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
