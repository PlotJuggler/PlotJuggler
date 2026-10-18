// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "lua.h"
#include "native/point_cloud_ops.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/field_table.hpp"
#include "pj_base/types.hpp"

// The object half of the Luau object-script binding (docs/derived_objects/ARCHITECTURE.md §6): canonical
// objects as read-only views over their SDK field tables, read lazily (a list is a view whose elements
// are read one at a time through `list_at`, so iterating N elements costs N reads), the point-cloud
// methods over native/point_cloud_ops.h, and the small values field access needs (`pj.int64`, the
// `pj.unavailable` marker). Scripts cannot reach the binder's metatables.

namespace PJ::scripting::detail {

/// Prepares `L` (made by `newSandboxedState`) for the functions below: userdata destructors and
/// metatables. `limits` is what the cloud methods allocate under; it must outlive every call made on `L`.
void registerBinder(lua_State* L, const native::NativeLimits& limits);

/// True when `object` can enter a script: its type has an SDK field table.
[[nodiscard]] bool canEnterScript(const sdk::BuiltinObject& object);

/// Pushes `object` as a read-only script object (it shares the value); `canEnterScript(object)` must hold.
void pushObject(lua_State* L, sdk::BuiltinObject object);

/// The object at `idx`, or null when that value is not one.
[[nodiscard]] const sdk::BuiltinObject* toObject(lua_State* L, int idx);

/// Pushes a read-only view of the struct at `owner`, described by `table`; `keep` keeps `owner` alive
/// as long as the view or any view read through it (an empty `keep` when the caller does).
void pushStructView(lua_State* L, const sdk::FieldTableView& table, const void* owner, sdk::BuiltinObject keep);

void pushInt64(lua_State* L, int64_t value);
/// The `pj.int64` at `idx`, or nullopt when that value is not one.
[[nodiscard]] std::optional<int64_t> toInt64(lua_State* L, int idx);

/// Pushes a `pj.unavailable` marker; its reason, cut to 1 KiB, lives in the VM under its memory cap.
void pushUnavailable(lua_State* L, std::string_view reason);
/// The reason of the `pj.unavailable` marker at `idx`, or nullopt when that value is not one.
[[nodiscard]] std::optional<std::string> toUnavailable(lua_State* L, int idx);

/// Pushes the numbers as an array, the one shape of a script-facing vector (`{x, y, z}`).
void pushArray(lua_State* L, std::initializer_list<double> values);

/// The object's own stamp: its top-level `timestamp_ns` (or `timestamp`) field; nullopt when its type
/// has none (scene entities and frame transforms stamp each element instead).
[[nodiscard]] std::optional<Timestamp> ownStamp(const sdk::BuiltinObject& object);

/// `object` with its own stamp moved by `shift`, saturating at the int64 limits (a copy sharing its payload
/// bytes); `object` itself when the shift is 0, the type has no own stamp, or the stamp is 0 (unstamped).
[[nodiscard]] sdk::BuiltinObject withStampShifted(const sdk::BuiltinObject& object, Timestamp shift);

/// Sets `functions` as fields of the table on top of the stack; with an `upvalue`, each closes over it
/// (read back with `upvalueAs`).
void setFunctions(
    lua_State* L, std::initializer_list<std::pair<const char*, lua_CFunction>> functions,
    const void* upvalue = nullptr);

template <class T>
T& upvalueAs(lua_State* L) {
  return *static_cast<T*>(lua_tolightuserdata(L, lua_upvalueindex(1)));
}

}  // namespace PJ::scripting::detail
