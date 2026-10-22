// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "lua.h"
#include "native/point_cloud_ops.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/field_table.hpp"

// The object half of the Luau object-script binding (docs/derived_objects/ARCHITECTURE.md §6): canonical
// objects as read-only views over their SDK field tables, read lazily (a list is a view whose elements
// are read one at a time through `list_at`, so iterating N elements costs N reads), the point-cloud
// methods over native/point_cloud_ops.h, the image methods (decode, decoded pixels, depth unprojection) over
// native/image_ops.h and native/depth_ops.h, the scene, annotation, cloud and voxel builders over
// native/scene_entities_ops.h, native/image_annotations_ops.h, native/point_cloud_builder.h and
// native/voxel_grid_ops.h, and the small values field access needs (`pj.int64`, the `pj.unavailable` marker). Scripts
// cannot reach the binder's metatables.

namespace PJ::scripting {
class ScriptMedia;
}  // namespace PJ::scripting

namespace PJ::scripting::detail {

/// Prepares a state for the functions below, once per state: userdata destructors and metatables. The functions read
/// their run through the calling thread's `setRunMedia`, so one state serves run after run.
void registerBinder(lua_State* L);

/// Makes `media` the run of every binder call made on thread `L` (its thread data): the object and decoded-image
/// methods and the builders allocate under its limits, decode through it, and pair the annotations a builder finishes
/// with it. It must outlive every call made on `L`.
void setRunMedia(lua_State* L, ScriptMedia& media);

/// Sets the builder namespaces `scene`, `annotations`, `cloud` and `voxels` (`pj.scene.new{...}`, ...) as fields of
/// the table on top of the stack.
void installBuilders(lua_State* L);

/// Pushes `object` as a read-only script object (it shares the value); `canEnterScript(object)` must hold.
/// `charge` is what a builder paid for `object`'s native memory: the box, and every view read through it,
/// shares it, so it counts against the evaluation's budget until the script has dropped them all (or its run ends).
void pushObject(
    lua_State* L, sdk::BuiltinObject object, std::shared_ptr<const native::MemoryReservation> charge = nullptr);

/// The object at `idx`, or null when that value is not one.
[[nodiscard]] const sdk::BuiltinObject* toObject(lua_State* L, int idx);

/// What a builder paid for the object at `idx` (its native charge), which a result must keep to go on counting after
/// its run ends; null when that value is not an object or carries none.
[[nodiscard]] std::shared_ptr<const void> toCharge(lua_State* L, int idx);

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
