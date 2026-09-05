// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/type_registry.hpp"

#include <fmt/format.h>
#include <tsl/robin_map.h>

#include <string>
#include <utility>
#include <vector>

#include "pj_base/expected.hpp"

namespace PJ {
namespace {

// Flatten a type tree into leaf paths paired with their PrimitiveType.
// For primitives: uses primitive_type directly.
// For enums: uses the underlying primitive_type.
// For arrays: uses the element_type's primitive_type (if primitive/enum).
// For structs: recurses into children.
void flattenLeafTypesImpl(
    const PJ::TypeTreeNode& node, std::string_view prefix,
    std::vector<std::pair<std::string, PJ::PrimitiveType>>& out) {
  std::string current_path = prefix.empty() ? node.name : fmt::format("{}.{}", prefix, node.name);

  switch (node.kind) {
    case PJ::TypeKind::kPrimitive:
      if (node.primitive_type.has_value()) {
        out.emplace_back(std::move(current_path), *node.primitive_type);
      }
      return;
    case PJ::TypeKind::kEnum:
      if (node.primitive_type.has_value()) {
        out.emplace_back(std::move(current_path), *node.primitive_type);
      }
      return;
    case PJ::TypeKind::kArray:
      // Treat the array itself as a leaf node with its element's type
      if (node.element_type && node.element_type->primitive_type.has_value()) {
        out.emplace_back(std::move(current_path), *node.element_type->primitive_type);
      }
      return;
    case PJ::TypeKind::kStruct:
      for (const auto& child : node.children) {
        flattenLeafTypesImpl(*child, current_path, out);
      }
      return;
  }
}

// Flatten starting from root, skipping the root struct name (same convention
// as flatten_field_paths).
std::vector<std::pair<std::string, PJ::PrimitiveType>> flattenLeafTypes(const PJ::TypeTreeNode& root) {
  std::vector<std::pair<std::string, PJ::PrimitiveType>> result;
  if (root.kind != PJ::TypeKind::kStruct) {
    if (root.primitive_type.has_value()) {
      result.emplace_back(root.name, *root.primitive_type);
    }
    return result;
  }
  for (const auto& child : root.children) {
    flattenLeafTypesImpl(*child, "", result);
  }
  return result;
}

}  // namespace

struct TypeRegistry::Impl {
  PJ::SchemaId next_id = 1;
  tsl::robin_map<PJ::SchemaId, std::shared_ptr<PJ::TypeTreeNode>> schemas;
  tsl::robin_map<std::string, PJ::SchemaId> name_to_id;
};

TypeRegistry::TypeRegistry() : impl_(std::make_unique<Impl>()) {}
TypeRegistry::~TypeRegistry() = default;
TypeRegistry::TypeRegistry(TypeRegistry&&) noexcept = default;
TypeRegistry& TypeRegistry::operator=(TypeRegistry&&) noexcept = default;

PJ::Expected<PJ::SchemaId> TypeRegistry::registerSchema(
    std::string schema_name, std::shared_ptr<PJ::TypeTreeNode> type_tree) {
  if (impl_->name_to_id.contains(schema_name)) {
    return PJ::unexpected(fmt::format("Schema '{}' already registered", schema_name));
  }
  PJ::SchemaId id = impl_->next_id++;
  impl_->name_to_id.emplace(schema_name, id);
  impl_->schemas.emplace(id, std::move(type_tree));
  return id;
}

PJ::Expected<PJ::SchemaId> TypeRegistry::registerOrGet(
    std::string schema_name, std::shared_ptr<PJ::TypeTreeNode> type_tree) {
  auto it = impl_->name_to_id.find(schema_name);
  if (it != impl_->name_to_id.end()) {
    return it->second;
  }
  return registerSchema(std::move(schema_name), std::move(type_tree));
}

const PJ::TypeTreeNode* TypeRegistry::lookup(PJ::SchemaId id) const {
  auto it = impl_->schemas.find(id);
  if (it == impl_->schemas.end()) {
    return nullptr;
  }
  return it->second.get();
}

std::optional<PJ::SchemaId> TypeRegistry::findByName(std::string_view name) const {
  auto it = impl_->name_to_id.find(std::string(name));
  if (it == impl_->name_to_id.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::string> TypeRegistry::schemaName(PJ::SchemaId id) const {
  for (const auto& [name, schema_id] : impl_->name_to_id) {
    if (schema_id == id) {
      return name;
    }
  }
  return std::nullopt;
}

PJ::Status TypeRegistry::evolveSchema(PJ::SchemaId id, std::shared_ptr<PJ::TypeTreeNode> updated_tree) {
  auto it = impl_->schemas.find(id);
  if (it == impl_->schemas.end()) {
    return PJ::unexpected(fmt::format("Schema ID {} not found", id));
  }

  const auto& old_tree = it->second;
  auto old_leaves = flattenLeafTypes(*old_tree);
  auto new_leaves = flattenLeafTypes(*updated_tree);

  // Build a map from path -> PrimitiveType for the new tree
  tsl::robin_map<std::string, PJ::PrimitiveType> new_leaf_map;
  new_leaf_map.reserve(new_leaves.size());
  for (auto& [path, ptype] : new_leaves) {
    new_leaf_map.emplace(std::move(path), ptype);
  }

  // Every old leaf must exist in the new tree with the same type
  for (const auto& [old_path, old_type] : old_leaves) {
    auto new_it = new_leaf_map.find(old_path);
    if (new_it == new_leaf_map.end()) {
      return PJ::unexpected(fmt::format("Field '{}' was removed in the updated schema", old_path));
    }
    if (new_it->second != old_type) {
      return PJ::unexpected(fmt::format("Field '{}' changed type in the updated schema", old_path));
    }
  }

  // Validation passed — replace with updated tree
  it.value() = std::move(updated_tree);
  return PJ::okStatus();
}

}  // namespace PJ
