// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "object_binder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "lualib.h"
#include "luau_sandbox_state.h"
#include "native/depth_ops.h"
#include "native/image_annotations_ops.h"
#include "native/image_ops.h"
#include "native/point_cloud_builder.h"
#include "native/scene_entities_ops.h"
#include "native/voxel_grid_ops.h"
#include "pj_base/builtin/field_table_registry.hpp"
#include "pj_scripting/memory_budget.h"
#include "pj_scripting/object_script.h"
#include "script_contract.h"
#include "script_media.h"

namespace PJ::scripting::detail {
namespace {

// Userdata tags: a tag both identifies a boxed value and selects its destructor.
enum Tag : int {
  kObjectTag = 1,
  kViewTag,
  kInt64Tag,
  kPointIteratorTag,
  kSceneBuilderTag,
  kAnnotationsBuilderTag,
  kDecodedTag,
  kCloudBuilderTag,
  kVoxelsBuilderTag,
};

constexpr const char* kUnavailableMetatable = "pj.unavailable";

/// What keeps an object alive for the script: the value and, for a builder's output, what the builder paid
/// for it. A view read from the object shares it, so neither lapses while a view can still read the value.
struct Anchor {
  sdk::BuiltinObject object;
  std::shared_ptr<const native::MemoryReservation> charge;
};

/// A script object: its anchor, its field table and the struct behind it, resolved once.
struct ObjectBox {
  Anchor anchor;
  const sdk::FieldTableView* table = nullptr;
  const void* data = nullptr;
};

/// A read-only window onto part of an object: a struct (`table`) or a list field (`list`, read on `owner`).
/// `keep` shares the object the window reads.
struct View {
  Anchor keep;
  const sdk::FieldTableView* table = nullptr;
  const sdk::FieldDescriptor* list = nullptr;
  const void* owner = nullptr;
};

struct PointIterator {
  sdk::BuiltinObject cloud;
  native::XyzAccess access;
  std::size_t next = 0;
};

template <class T>
void destroy(lua_State* /*L*/, void* box) {
  static_cast<T*>(box)->~T();
}

template <class T>
T* newBox(lua_State* L, Tag tag, T value) {
  return new (lua_newuserdatataggedwithmetatable(L, sizeof(T), tag)) T(std::move(value));
}

/// The box at `index`, or a type error naming `what` when that value is not one.
template <class T>
T& checkBox(lua_State* L, int index, Tag tag, const char* what) {
  void* box = lua_touserdatatagged(L, index, tag);
  if (box == nullptr) {
    luaL_typeerror(L, index, what);
  }
  return *static_cast<T*>(box);
}

/// Raises `message` as the script's error.
[[noreturn]] void raise(lua_State* L, const std::string& message) {
  luaL_error(L, "%s", message.c_str());
}

/// The run of the calling thread (`setRunMedia`).
ScriptMedia& runMedia(lua_State* L) {
  auto* media = static_cast<ScriptMedia*>(lua_getthreaddata(L));
  if (media == nullptr) {
    luaL_error(L, "no script run on this thread");
  }
  return *media;
}

/// The value of a native operation, else its error raised; a cancellation also trips the run, so a script
/// `pcall` cannot swallow it.
template <class T>
T nativeResult(lua_State* L, Expected<T> result) {
  if (!result) {
    if (result.error() == native::kCancelledError) {
      tripRun(L, *static_cast<BudgetState*>(lua_callbacks(L)->userdata), "script cancelled");
    }
    raise(L, result.error());
  }
  return std::move(*result);
}

// ---- field reads -------------------------------------------------------------

/// A scalar read through `field`'s accessors at `pointer`: the owning struct for a member, the element
/// for a list element.
void pushScalar(lua_State* L, sdk::FieldKind kind, const sdk::FieldDescriptor& field, const void* pointer) {
  switch (kind) {
    case sdk::FieldKind::kBool:
      lua_pushboolean(L, field.get_number(pointer) != 0.0);
      return;
    case sdk::FieldKind::kInt64:
      pushInt64(L, field.get_int64(pointer));
      return;
    case sdk::FieldKind::kNumber:
    case sdk::FieldKind::kEnum:
      lua_pushnumber(L, field.get_number(pointer));
      return;
    case sdk::FieldKind::kString: {
      const std::string_view text = field.get_string(pointer);
      lua_pushlstring(L, text.data(), text.size());
      return;
    }
    default:
      lua_pushnil(L);
      return;
  }
}

void pushField(lua_State* L, const sdk::FieldDescriptor& field, const void* owner, const Anchor& keep) {
  switch (field.kind) {
    case sdk::FieldKind::kStruct:
      newBox(L, kViewTag, View{keep, field.nested, nullptr, field.struct_ptr(owner)});
      return;
    case sdk::FieldKind::kList:
      newBox(L, kViewTag, View{keep, nullptr, &field, owner});
      return;
    case sdk::FieldKind::kBuffer:
      raise(L, "field '" + std::string(field.name) + "' is a raw buffer; use the object's methods");
    case sdk::FieldKind::kOptionalNumber:
      if (field.has_value(owner)) {
        lua_pushnumber(L, field.get_number(owner));
      } else {
        lua_pushnil(L);
      }
      return;
    default:
      pushScalar(L, field.kind, field, owner);
      return;
  }
}

/// Pushes the field named by the key at `key_index` (nil for an unknown name).
int pushNamedField(
    lua_State* L, const sdk::FieldTableView& table, const void* owner, const Anchor& keep, int key_index) {
  if (lua_type(L, key_index) == LUA_TSTRING) {
    std::size_t length = 0;
    const char* key = lua_tolstring(L, key_index, &length);
    for (const sdk::FieldDescriptor& field : table.fields) {
      if (field.name == std::string_view(key, length)) {
        pushField(L, field, owner, keep);
        return 1;
      }
    }
  }
  lua_pushnil(L);
  return 1;
}

int lReadOnly(lua_State* L) {
  luaL_error(L, "objects are read-only");
}

View& viewAt(lua_State* L, int index) {
  return checkBox<View>(L, index, kViewTag, "object view");
}

const View& listAt(lua_State* L, int index) {
  const View& view = viewAt(L, index);
  if (view.list == nullptr) {
    luaL_typeerror(L, index, "list");
  }
  return view;
}

/// Element `position` (1-based) of a list view, or nil past either end.
int pushListElement(lua_State* L, const View& view, double position) {
  const sdk::FieldDescriptor& list = *view.list;
  const auto size = static_cast<double>(list.list_size(view.owner));
  if (!(position >= 1 && position <= size && std::floor(position) == position)) {
    lua_pushnil(L);
    return 1;
  }
  const void* element = list.list_at(view.owner, static_cast<std::size_t>(position) - 1);
  if (list.nested != nullptr) {
    newBox(L, kViewTag, View{view.keep, list.nested, nullptr, element});
  } else {
    pushScalar(L, list.element_kind, list, element);
  }
  return 1;
}

int lViewIndex(lua_State* L) {
  const View& view = viewAt(L, 1);
  if (view.list == nullptr) {
    return pushNamedField(L, *view.table, view.owner, view.keep, 2);
  }
  if (lua_type(L, 2) != LUA_TNUMBER) {
    lua_pushnil(L);
    return 1;
  }
  return pushListElement(L, view, lua_tonumber(L, 2));
}

int lViewLen(lua_State* L) {
  const View& list = listAt(L, 1);
  lua_pushnumber(L, static_cast<double>(list.list->list_size(list.owner)));
  return 1;
}

int lListNext(lua_State* L) {
  const double position = luaL_checknumber(L, 2) + 1;
  pushListElement(L, listAt(L, 1), position);
  if (lua_isnil(L, -1)) {
    return 0;
  }
  lua_pushnumber(L, position);
  lua_insert(L, -2);
  return 2;
}

/// `for i, element in list do`: generalized iteration over a list view.
int lViewIter(lua_State* L) {
  listAt(L, 1);
  lua_pushcfunction(L, lListNext, "next");
  lua_pushvalue(L, 1);
  lua_pushnumber(L, 0);
  return 3;
}

ObjectBox& objectBoxAt(lua_State* L, int index) {
  return checkBox<ObjectBox>(L, index, kObjectTag, "object");
}

/// `__index` of an object: its type's methods (upvalue 1, a table keyed by type), then its fields.
int lObjectIndex(lua_State* L) {
  const ObjectBox& box = objectBoxAt(L, 1);
  lua_rawgeti(L, lua_upvalueindex(1), static_cast<int>(box.anchor.object.type()));
  if (lua_istable(L, -1)) {
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) {
      return 1;
    }
  }
  if (box.table == nullptr) {
    raise(L, "a " + std::string(sdk::name(box.anchor.object.type())) + " has no readable fields");
  }
  return pushNamedField(L, *box.table, box.data, box.anchor, 2);
}

// ---- pj.int64 ----------------------------------------------------------------

int64_t checkInt64(lua_State* L, int index) {
  return checkBox<int64_t>(L, index, kInt64Tag, "pj.int64");
}

int lInt64ToString(lua_State* L) {
  const std::string text = std::to_string(checkInt64(L, 1));
  lua_pushlstring(L, text.data(), text.size());
  return 1;
}
int lInt64Eq(lua_State* L) {
  lua_pushboolean(L, checkInt64(L, 1) == checkInt64(L, 2));
  return 1;
}
int lInt64Lt(lua_State* L) {
  lua_pushboolean(L, checkInt64(L, 1) < checkInt64(L, 2));
  return 1;
}
int lInt64Le(lua_State* L) {
  lua_pushboolean(L, checkInt64(L, 1) <= checkInt64(L, 2));
  return 1;
}
/// `a:diff_s(b)`: (a - b) in seconds; exact in the subtraction unless it overflows int64.
int lInt64DiffS(lua_State* L) {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  const int64_t minuend = checkInt64(L, 1);
  const int64_t subtrahend = checkInt64(L, 2);
  const bool overflows = subtrahend < 0 ? minuend > kMax + subtrahend : minuend < kMin + subtrahend;
  const double nanoseconds = overflows ? static_cast<double>(minuend) - static_cast<double>(subtrahend)
                                       : static_cast<double>(minuend - subtrahend);
  lua_pushnumber(L, nanoseconds / 1e9);
  return 1;
}
int lInt64ToNumber(lua_State* L) {
  lua_pushnumber(L, static_cast<double>(checkInt64(L, 1)));
  return 1;
}

// ---- point-cloud methods -------------------------------------------------------

const sdk::PointCloud& checkCloud(lua_State* L) {
  const sdk::PointCloud* cloud = objectBoxAt(L, 1).anchor.object.get<sdk::PointCloud>();
  if (cloud == nullptr) {
    luaL_typeerror(L, 1, "point cloud");
  }
  return *cloud;
}

/// Runs an allocating native operation. A failure on the native budget runs one full collection, which frees the
/// bytes still charged to dead outputs, then retries once.
template <class Operation>
auto retryAfterCollect(lua_State* L, Operation&& operation) -> decltype(operation()) {
  auto result = operation();
  if (!result && result.error().starts_with(native::kNativeLimitError)) {
    lua_gc(L, LUA_GCCOLLECT, 0);
    return operation();
  }
  return result;
}

/// Runs an allocating native operation and pushes its cloud.
template <class Operation>
int pushNativeCloud(lua_State* L, Operation&& operation) {
  pushObject(L, sdk::BuiltinObject(nativeResult(L, retryAfterCollect(L, operation))));
  return 1;
}

/// The N numbers of the array at `index`; `subject` names it in the error.
template <std::size_t N>
std::array<double, N> toArray(lua_State* L, int index, const std::string& subject) {
  std::array<double, N> values{};
  const bool is_table = lua_istable(L, index);
  for (std::size_t position = 0; position < N; ++position) {
    if (is_table) {
      lua_rawgeti(L, index, static_cast<int>(position) + 1);
    } else {
      lua_pushnil(L);
    }
    if (lua_type(L, -1) != LUA_TNUMBER) {
      luaL_error(L, "%s must be an array of %d numbers", subject.c_str(), static_cast<int>(N));
    }
    values[position] = lua_tonumber(L, -1);
    lua_pop(L, 1);
  }
  return values;
}

/// The required array field `key` of the table at `table`: N numbers. Fields are read raw, so no script
/// code (an `__index` metamethod) runs while a native operation holds a reference into the VM's boxes.
template <std::size_t N>
std::array<double, N> readArray(lua_State* L, int table, const char* key) {
  lua_rawgetfield(L, table, key);
  const auto values = toArray<N>(L, lua_gettop(L), std::string("'") + key + "'");
  lua_pop(L, 1);
  return values;
}

/// The N numbers as a `Point` ({x, y} or {x, y, z}).
template <class Point, std::size_t N>
Point toPoint(const std::array<double, N>& values) {
  return std::apply([](auto... coordinates) { return Point{coordinates...}; }, values);
}

/// The required array field `key` as a `Point`.
template <class Point, std::size_t N>
Point readPoint(lua_State* L, int table, const char* key) {
  return toPoint<Point>(readArray<N>(L, table, key));
}

void setNumber(lua_State* L, const char* key, double value) {
  lua_pushnumber(L, value);
  lua_setfield(L, -2, key);
}

int lCloudCount(lua_State* L) {
  lua_pushnumber(L, static_cast<double>(native::countPoints(checkCloud(L))));
  return 1;
}

/// `cloud:bounds()` -> `{min={x, y, z}, max={x, y, z}, count=n}` over the finite points, nil when none.
int lCloudBounds(lua_State* L) {
  const native::Bounds bounds = nativeResult(L, native::bounds(checkCloud(L)));
  if (bounds.count == 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, 0, 3);
  pushArray(L, {bounds.min.x, bounds.min.y, bounds.min.z});
  lua_setfield(L, -2, "min");
  pushArray(L, {bounds.max.x, bounds.max.y, bounds.max.z});
  lua_setfield(L, -2, "max");
  setNumber(L, "count", static_cast<double>(bounds.count));
  return 1;
}

/// `cloud:extreme_point(channel, "max"|"min")` -> `{index (1-based), x, y, z, value}`, nil when none.
int lCloudExtremePoint(lua_State* L) {
  const sdk::PointCloud& cloud = checkCloud(L);
  const char* channel = luaL_checkstring(L, 2);
  const std::optional<native::Extreme> extreme = parseExtreme(luaL_checkstring(L, 3));
  if (!extreme) {
    luaL_error(L, "extreme_point: the second argument must be 'max' or 'min'");
  }
  const auto best = nativeResult(L, native::extremePoint(cloud, channel, *extreme));
  if (!best) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, 0, 5);
  setNumber(L, "index", static_cast<double>(best->point.index) + 1.0);
  setNumber(L, "x", best->point.position.x);
  setNumber(L, "y", best->point.position.y);
  setNumber(L, "z", best->point.position.z);
  setNumber(L, "value", best->value);
  return 1;
}

/// `cloud:crop_box{min={x, y, z}, max={x, y, z}}`.
int lCloudCropBox(lua_State* L) {
  const sdk::PointCloud& cloud = checkCloud(L);
  luaL_checktype(L, 2, LUA_TTABLE);
  const auto min = readPoint<sdk::Vector3, 3>(L, 2, "min");
  const auto max = readPoint<sdk::Vector3, 3>(L, 2, "max");
  return pushNativeCloud(L, [&] { return native::cropBox(cloud, min, max, runMedia(L).limits()); });
}

/// The comparison named by the string at `index` (`parseComparison`); `what` names the caller.
native::CompareOp checkComparison(lua_State* L, int index, const char* what) {
  const char* op = luaL_checkstring(L, index);
  const std::optional<native::CompareOp> comparison = parseComparison(op, "~=");
  if (!comparison) {
    luaL_error(L, "%s: unknown comparison '%s' (expected <, <=, >, >=, ==, ~=)", what, op);
  }
  return *comparison;
}

/// `cloud:filter(channel, op, value)`.
int lCloudFilter(lua_State* L) {
  const sdk::PointCloud& cloud = checkCloud(L);
  const char* channel = luaL_checkstring(L, 2);
  const native::CompareOp op = checkComparison(L, 3, "filter");
  const double threshold = luaL_checknumber(L, 4);
  return pushNativeCloud(L, [&] { return native::filterByField(cloud, channel, op, threshold, runMedia(L).limits()); });
}

/// `cloud:transform(tf)`, `tf` as `pj.tf.lookup` returns it: the result is in `tf.target_frame` when set.
int lCloudTransform(lua_State* L) {
  const sdk::PointCloud& cloud = checkCloud(L);
  luaL_checktype(L, 2, LUA_TTABLE);
  sdk::FrameTransform tf;
  tf.translation = readPoint<sdk::Vector3, 3>(L, 2, "translation");
  const auto rotation = readArray<4>(L, 2, "rotation");
  tf.rotation = {rotation[0], rotation[1], rotation[2], rotation[3]};
  lua_getfield(L, 2, "target_frame");
  if (lua_type(L, -1) == LUA_TSTRING) {
    std::size_t length = 0;
    const char* frame = lua_tolstring(L, -1, &length);
    tf.parent_frame_id.assign(frame, length);  // charged with the output by transformXyz
  }
  lua_pop(L, 1);
  return pushNativeCloud(L, [&] { return native::transformXyz(cloud, tf, runMedia(L).limits()); });
}

int lPointsNext(lua_State* L) {
  auto& iterator = checkBox<PointIterator>(L, 1, kPointIteratorTag, "point iterator");
  const auto point = native::nextFinitePoint(*iterator.cloud.get<sdk::PointCloud>(), iterator.access, iterator.next);
  if (!point) {
    return 0;
  }
  lua_pushnumber(L, static_cast<double>(point->index) + 1.0);
  lua_pushnumber(L, point->position.x);
  lua_pushnumber(L, point->position.y);
  lua_pushnumber(L, point->position.z);
  return 4;
}

/// `for i, x, y, z in cloud:xyz_points() do`: the finite points, `i` 1-based over every record.
int lCloudXyzPoints(lua_State* L) {
  const native::XyzAccess access = nativeResult(L, native::resolveXyz(checkCloud(L)));
  lua_pushcfunction(L, lPointsNext, "xyz_points_next");
  newBox(L, kPointIteratorTag, PointIterator{objectBoxAt(L, 1).anchor.object, access, 0});
  lua_pushnil(L);
  return 3;
}

// ---- builders: pj.scene and pj.annotations ---------------------------------------
//
// The arguments and defaults are documented in object_script.h. A method reads its options table raw
// (lua_rawgetfield, lua_rawgeti): script code, such as an `__index` metamethod, must not run inside a call
// that holds its builder, or it could finish that builder.

using SceneSlot = std::optional<native::SceneEntityBuilder>;
using AnnotationsSlot = std::optional<native::ImageAnnotationsBuilder>;
using CloudSlot = std::optional<native::PointCloudBuilder>;
using VoxelsSlot = std::optional<native::VoxelGridBuilder>;

constexpr sdk::ColorRGBA kWhite{255, 255, 255, 255};
constexpr sdk::ColorRGBA kTransparent{0, 0, 0, 0};

/// The required string field `key` of the table at `table`.
std::string readString(lua_State* L, int table, const char* key) {
  lua_rawgetfield(L, table, key);
  if (lua_type(L, -1) != LUA_TSTRING) {
    luaL_error(L, "'%s' must be a string", key);
  }
  std::size_t length = 0;
  const char* text = lua_tolstring(L, -1, &length);
  std::string value(text, length);
  lua_pop(L, 1);
  return value;
}

/// The number field `key` of the table at `table`; without a `fallback` it is required.
double readNumber(lua_State* L, int table, const char* key, std::optional<double> fallback = std::nullopt) {
  lua_rawgetfield(L, table, key);
  if (fallback && lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return *fallback;
  }
  if (lua_type(L, -1) != LUA_TNUMBER) {
    luaL_error(L, "'%s' must be a number", key);
  }
  const double value = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return value;
}

/// The `{r, g, b}` or `{r, g, b, a}` field `key` (components 0-255, alpha 255 when absent), or `fallback`
/// when the field is.
sdk::ColorRGBA readColor(lua_State* L, int table, const char* key, sdk::ColorRGBA fallback) {
  lua_rawgetfield(L, table, key);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return fallback;
  }
  const int color = lua_gettop(L);
  const int count = lua_istable(L, color) ? lua_objlen(L, color) : 0;
  if (count != 3 && count != 4) {
    luaL_error(L, "'%s' must be {r, g, b} or {r, g, b, a}", key);
  }
  std::array<uint8_t, 4> channels{0, 0, 0, 255};
  for (int position = 0; position < count; ++position) {
    lua_rawgeti(L, color, position + 1);
    const double value = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : -1.0;
    if (!(value >= 0.0 && value <= 255.0)) {
      luaL_error(L, "'%s' components must be numbers from 0 to 255", key);
    }
    channels[static_cast<std::size_t>(position)] = static_cast<uint8_t>(value);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return {channels[0], channels[1], channels[2], channels[3]};
}

/// The array field `key` of the table at `table`, each entry N numbers read as a `Point`.
template <class Point, std::size_t N>
std::vector<Point> readPoints(lua_State* L, int table, const char* key) {
  lua_rawgetfield(L, table, key);
  if (!lua_istable(L, -1)) {
    luaL_error(L, "'%s' must be an array of points", key);
  }
  const int list = lua_gettop(L);
  const int count = lua_objlen(L, list);
  const std::string subject = std::string("'") + key + "' entry";
  std::vector<Point> points;
  points.reserve(static_cast<std::size_t>(count));
  for (int position = 1; position <= count; ++position) {
    lua_rawgeti(L, list, position);
    points.push_back(toPoint<Point>(toArray<N>(L, lua_gettop(L), subject)));
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return points;
}

SceneSlot& sceneSlot(lua_State* L) {
  return checkBox<SceneSlot>(L, 1, kSceneBuilderTag, "scene builder");
}

AnnotationsSlot& annotationsSlot(lua_State* L) {
  return checkBox<AnnotationsSlot>(L, 1, kAnnotationsBuilderTag, "annotations builder");
}

CloudSlot& cloudSlot(lua_State* L) {
  return checkBox<CloudSlot>(L, 1, kCloudBuilderTag, "cloud builder");
}

VoxelsSlot& voxelsSlot(lua_State* L) {
  return checkBox<VoxelsSlot>(L, 1, kVoxelsBuilderTag, "voxels builder");
}

/// The builder in `slot`, or an error when `finish()` already consumed it.
template <class Builder>
Builder& live(lua_State* L, std::optional<Builder>& slot) {
  if (!slot) {
    raise(L, "the builder is already finished");
  }
  return *slot;
}

/// `live`, for a primitive method: argument 2 is its options table.
template <class Builder>
Builder& liveWithOptions(lua_State* L, std::optional<Builder>& slot) {
  Builder& builder = live(L, slot);
  luaL_checktype(L, 2, LUA_TTABLE);
  return builder;
}

/// The end of a primitive method: a rejection raised as the script's error, else the builder again, so
/// calls chain.
int chain(lua_State* L, const Status& status) {
  if (!status) {
    raise(L, status.error());
  }
  lua_pushvalue(L, 1);
  return 1;
}

/// Pushes an object a native operation made, which carries its charge until the script drops it.
template <class Object>
int pushCharged(lua_State* L, native::Charged<Object> done) {
  pushObject(
      L, sdk::BuiltinObject(std::move(done.value)),
      std::make_shared<const native::MemoryReservation>(std::move(done.charge)));
  return 1;
}

/// `:finish()`: pushes the finished object, which carries the builder's charge, and spends the builder. An object with
/// a stamp of its own is stamped with the result's (`ScriptMedia::stampUnset`).
template <class Builder>
int finishBuilder(lua_State* L, std::optional<Builder>& slot) {
  auto done = std::move(live(L, slot)).finish();
  slot.reset();
  if constexpr (requires { done.value.timestamp_ns; }) {
    runMedia(L).stampUnset(done.value.timestamp_ns);
  }
  return pushCharged(L, std::move(done));
}

/// `pj.scene.new{frame_id=, id=}`.
int lSceneNew(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  std::string frame_id = readString(L, 1, "frame_id");
  std::string id = readString(L, 1, "id");
  auto builder = native::SceneEntityBuilder::create(std::move(frame_id), std::move(id), runMedia(L).limits().budget);
  newBox(L, kSceneBuilderTag, SceneSlot(nativeResult(L, std::move(builder))));
  return 1;
}

/// `:sphere{center=, diameter=, color=}`.
int lSceneSphere(lua_State* L) {
  native::SceneEntityBuilder& builder = liveWithOptions(L, sceneSlot(L));
  const auto center = readPoint<sdk::Vector3, 3>(L, 2, "center");
  const double diameter = readNumber(L, 2, "diameter");
  return chain(L, builder.addSphere(center, diameter, readColor(L, 2, "color", kWhite)));
}

/// `:cube{center=, size=, color=}`.
int lSceneCube(lua_State* L) {
  native::SceneEntityBuilder& builder = liveWithOptions(L, sceneSlot(L));
  const auto center = readPoint<sdk::Vector3, 3>(L, 2, "center");
  const auto size = readPoint<sdk::Vector3, 3>(L, 2, "size");
  return chain(L, builder.addCube(center, size, readColor(L, 2, "color", kWhite)));
}

/// `:text{position=, text=, font_size=, color=, billboard=}`.
int lSceneText(lua_State* L) {
  native::SceneEntityBuilder& builder = liveWithOptions(L, sceneSlot(L));
  const auto position = readPoint<sdk::Vector3, 3>(L, 2, "position");
  std::string text = readString(L, 2, "text");
  const double font_size = readNumber(L, 2, "font_size");
  const sdk::ColorRGBA color = readColor(L, 2, "color", kWhite);
  lua_rawgetfield(L, 2, "billboard");
  const bool billboard = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return chain(L, builder.addText(position, std::move(text), font_size, color, billboard));
}

/// `:line_strip{points=, thickness=, color=}`.
int lSceneLineStrip(lua_State* L) {
  native::SceneEntityBuilder& builder = liveWithOptions(L, sceneSlot(L));
  std::vector<sdk::Point3> points = readPoints<sdk::Point3, 3>(L, 2, "points");
  const double thickness = readNumber(L, 2, "thickness");
  return chain(L, builder.addLineStrip(std::move(points), thickness, readColor(L, 2, "color", kWhite)));
}

int lSceneFinish(lua_State* L) {
  return finishBuilder(L, sceneSlot(L));
}

/// `pj.annotations.new{image_topic=}`.
int lAnnotationsNew(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  std::string image_topic = readString(L, 1, "image_topic");
  auto builder = native::ImageAnnotationsBuilder::create(std::move(image_topic), runMedia(L).limits().budget);
  newBox(L, kAnnotationsBuilderTag, AnnotationsSlot(nativeResult(L, std::move(builder))));
  return 1;
}

/// `:points{points=, type=, thickness=, outline_color=, fill_color=}`.
int lAnnotationsPoints(lua_State* L) {
  native::ImageAnnotationsBuilder& builder = liveWithOptions(L, annotationsSlot(L));
  std::vector<sdk::Point2> points = readPoints<sdk::Point2, 2>(L, 2, "points");
  const std::string type = readString(L, 2, "type");
  const sdk::PointsAnnotation defaults;
  const double thickness = readNumber(L, 2, "thickness", defaults.thickness);
  const sdk::ColorRGBA outline = readColor(L, 2, "outline_color", defaults.color);
  return chain(
      L, builder.addPoints(
             std::move(points), type, thickness, outline, readColor(L, 2, "fill_color", defaults.fill_color)));
}

/// `:circle{center=, diameter=, thickness=, outline_color=, fill_color=}`.
int lAnnotationsCircle(lua_State* L) {
  native::ImageAnnotationsBuilder& builder = liveWithOptions(L, annotationsSlot(L));
  const auto center = readPoint<sdk::Point2, 2>(L, 2, "center");
  const double diameter = readNumber(L, 2, "diameter");
  const sdk::CircleAnnotation defaults;
  const double thickness = readNumber(L, 2, "thickness", defaults.thickness);
  const sdk::ColorRGBA outline = readColor(L, 2, "outline_color", defaults.color);
  return chain(
      L, builder.addCircle(center, diameter, thickness, outline, readColor(L, 2, "fill_color", defaults.fill_color)));
}

/// `:text{position=, text=, font_size=, text_color=}`; the SDK has no text background, so an opaque
/// `background_color` is refused rather than dropped.
int lAnnotationsText(lua_State* L) {
  native::ImageAnnotationsBuilder& builder = liveWithOptions(L, annotationsSlot(L));
  const auto position = readPoint<sdk::Point2, 2>(L, 2, "position");
  std::string text = readString(L, 2, "text");
  const sdk::TextAnnotation defaults;
  const double font_size = readNumber(L, 2, "font_size", defaults.font_size);
  const sdk::ColorRGBA color = readColor(L, 2, "text_color", defaults.color);
  if (readColor(L, 2, "background_color", kTransparent).a != 0) {
    raise(L, "text: 'background_color' is not supported: the annotations have no text background");
  }
  return chain(L, builder.addText(position, std::move(text), font_size, color));
}

/// `:finish()`, paired with the image input `image_topic` names (`ScriptMedia::pairing`).
int lAnnotationsFinish(lua_State* L) {
  AnnotationsSlot& slot = annotationsSlot(L);
  auto done = std::move(live(L, slot)).finish();
  slot.reset();
  auto [topic, stamp] = runMedia(L).pairing(done.value.image_topic);
  done.value.image_topic = std::move(topic);
  done.value.timestamp = stamp;
  return pushCharged(L, std::move(done));
}

int lAnnotationsCount(lua_State* L) {
  const sdk::ImageAnnotations* annotations = objectBoxAt(L, 1).anchor.object.get<sdk::ImageAnnotations>();
  if (annotations == nullptr) {
    luaL_typeerror(L, 1, "image annotations");
  }
  lua_pushnumber(L, static_cast<double>(native::countAnnotationPoints(*annotations)));
  return 1;
}

// ---- builders: pj.cloud and pj.voxels; cloud:downsample ----------------------------

/// One past the widest preset's values, so an extra value still reaches the builder's arity check.
constexpr int kMaxPointValues = 7;

/// `value` as an integer when it is a whole number, clamped to +-2^62 (outside any grid, and safe to shift by one).
std::optional<int64_t> wholeNumber(double value) {
  if (std::floor(value) != value) {  // a fraction or NaN
    return std::nullopt;
  }
  constexpr double kLimit = 4.6e18;
  return static_cast<int64_t>(std::clamp(value, -kLimit, kLimit));
}

/// `pj.cloud.new{preset=, frame_id=}`.
int lCloudNew(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  const std::optional<native::CloudPreset> preset = native::parseCloudPreset(readString(L, 1, "preset"));
  if (!preset) {
    raise(L, "'preset' must be \"xyz\", \"xyzi\" or \"xyzrgb\"");
  }
  std::string frame_id = readString(L, 1, "frame_id");
  auto builder = native::PointCloudBuilder::create(*preset, std::move(frame_id), runMedia(L).limits().budget);
  newBox(L, kCloudBuilderTag, CloudSlot(nativeResult(L, std::move(builder))));
  return 1;
}

/// `:add(x, y, z[, intensity | r, g, b])`.
int lCloudAdd(lua_State* L) {
  native::PointCloudBuilder& builder = live(L, cloudSlot(L));
  const int count = std::min(lua_gettop(L) - 1, kMaxPointValues);
  std::array<double, kMaxPointValues> values{};
  for (int position = 0; position < count; ++position) {
    values[static_cast<std::size_t>(position)] = luaL_checknumber(L, position + 2);
  }
  return chain(L, builder.add(std::span<const double>(values.data(), static_cast<std::size_t>(count))));
}

/// `:add_points{{x, y, z, ...}, ...}`: the whole list is charged before it is copied, and its entries are read raw.
int lCloudAddPoints(lua_State* L) {
  native::PointCloudBuilder& builder = live(L, cloudSlot(L));
  if (!lua_istable(L, 2)) {
    raise(L, "'points' must be an array of points");
  }
  const int count = lua_objlen(L, 2);
  if (Status reserved = builder.reserve(static_cast<std::size_t>(count)); !reserved) {
    raise(L, reserved.error());
  }
  std::array<double, kMaxPointValues> values{};
  for (int position = 1; position <= count; ++position) {
    lua_rawgeti(L, 2, position);
    const int entry = lua_gettop(L);
    if (!lua_istable(L, entry)) {
      raise(L, "'points' entries must be arrays of numbers");
    }
    const int size = std::min(lua_objlen(L, entry), kMaxPointValues);
    for (int value = 0; value < size; ++value) {
      lua_rawgeti(L, entry, value + 1);
      if (lua_type(L, -1) != LUA_TNUMBER) {
        raise(L, "'points' entries must be arrays of numbers");
      }
      values[static_cast<std::size_t>(value)] = lua_tonumber(L, -1);
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (Status added = builder.add(std::span<const double>(values.data(), static_cast<std::size_t>(size))); !added) {
      raise(L, added.error());
    }
  }
  lua_pushvalue(L, 1);
  return 1;
}

int lCloudFinish(lua_State* L) {
  return finishBuilder(L, cloudSlot(L));
}

/// `pj.voxels.new{frame_id=, origin={x, y, z}, cell_size=, dims={nx, ny, nz}}`.
int lVoxelsNew(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  std::string frame_id = readString(L, 1, "frame_id");
  const auto origin = readPoint<sdk::Vector3, 3>(L, 1, "origin");
  const double cell_size = readNumber(L, 1, "cell_size");
  const auto counts = readArray<3>(L, 1, "dims");
  std::array<int64_t, 3> dims{};
  for (std::size_t axis = 0; axis < dims.size(); ++axis) {
    const std::optional<int64_t> count = wholeNumber(counts[axis]);
    if (!count) {
      raise(L, "'dims' must be 3 integers greater than 0");
    }
    dims[axis] = *count;
  }
  auto builder =
      native::VoxelGridBuilder::create(std::move(frame_id), origin, cell_size, dims, runMedia(L).limits().budget);
  newBox(L, kVoxelsBuilderTag, VoxelsSlot(nativeResult(L, std::move(builder))));
  return 1;
}

/// `:set(i, j, k, value)`, the indices 1-based.
int lVoxelsSet(lua_State* L) {
  native::VoxelGridBuilder& builder = live(L, voxelsSlot(L));
  std::array<int64_t, 3> cell{};
  for (std::size_t axis = 0; axis < cell.size(); ++axis) {
    const std::optional<int64_t> index = wholeNumber(luaL_checknumber(L, static_cast<int>(axis) + 2));
    if (!index) {
      raise(L, "set: i, j and k must be integers");
    }
    cell[axis] = *index - 1;
  }
  const double value = luaL_checknumber(L, 5);
  return chain(L, builder.set(cell[0], cell[1], cell[2], value));
}

int lVoxelsFinish(lua_State* L) {
  return finishBuilder(L, voxelsSlot(L));
}

/// `cloud:downsample(voxel_size)`.
int lCloudDownsample(lua_State* L) {
  const sdk::PointCloud& cloud = checkCloud(L);
  const double voxel_size = luaL_checknumber(L, 2);
  return pushCharged(L, nativeResult(L, retryAfterCollect(L, [&] {
                                       return native::downsample(cloud, voxel_size, runMedia(L).limits());
                                     })));
}

// ---- images: decode, decoded pixels, depth unprojection ---------------------------------------------------------

/// The receiver of an image method: exactly one of `image` and `depth` is set.
struct ImageReceiver {
  const ObjectBox* box = nullptr;
  const sdk::Image* image = nullptr;
  const sdk::DepthImage* depth = nullptr;
};

/// The image or depth image at index 1; any other receiver (a method borrowed onto another object,
/// `image.decode(camera)`) is a script error.
ImageReceiver checkImageReceiver(lua_State* L) {
  const ObjectBox& box = objectBoxAt(L, 1);
  ImageReceiver receiver{&box, box.anchor.object.get<sdk::Image>(), box.anchor.object.get<sdk::DepthImage>()};
  if (receiver.image == nullptr && receiver.depth == nullptr) {
    luaL_typeerror(L, 1, "image or depth image");
  }
  return receiver;
}

/// The receiver's pixels, decoded once per run.
Expected<DecodedPixels> decodeReceiver(lua_State* L, ScriptMedia& media, const ImageReceiver& receiver) {
  if (receiver.image != nullptr) {
    return retryAfterCollect(L, [&] { return media.decode(receiver.image, *receiver.image); });
  }
  return retryAfterCollect(L, [&] { return media.decode(receiver.depth, native::depthAsImage(*receiver.depth)); });
}

DecodedPixels& checkDecoded(lua_State* L) {
  return checkBox<DecodedPixels>(L, 1, kDecodedTag, "decoded image");
}

void pushDecoded(lua_State* L, DecodedPixels pixels) {
  newBox(L, kDecodedTag, std::move(pixels));
}

/// A pixel coordinate or size: a non-negative integer.
uint32_t checkPixelInteger(lua_State* L, int index) {
  const double value = luaL_checknumber(L, index);
  if (!(value >= 0 && value <= double(UINT32_MAX)) || std::floor(value) != value) {
    luaL_error(L, "expected a non-negative pixel integer");
  }
  return static_cast<uint32_t>(value);
}

/// `image:representation()`.
int lImageRepresentation(lua_State* L) {
  const ImageReceiver receiver = checkImageReceiver(L);
  const std::string representation = receiver.image != nullptr ? native::imageRepresentation(*receiver.image)
                                                               : native::imageRepresentation(*receiver.depth);
  lua_pushlstring(L, representation.data(), representation.size());
  return 1;
}

/// `image:decode()` -> decoded pixels, or `nil, reason`.
int lImageDecode(lua_State* L) {
  Expected<DecodedPixels> pixels = decodeReceiver(L, runMedia(L), checkImageReceiver(L));
  if (!pixels) {
    lua_pushnil(L);
    lua_pushlstring(L, pixels.error().data(), pixels.error().size());
    return 2;
  }
  pushDecoded(L, std::move(*pixels));
  return 1;
}

/// `image:to_point_cloud(camera_info[, {step=, max_depth=}])` -> a cloud, or `pj.unavailable(reason)`.
int lImageToPointCloud(lua_State* L) {
  ScriptMedia& media = runMedia(L);
  const ImageReceiver receiver = checkImageReceiver(L);
  const sdk::BuiltinObject* camera_object = toObject(L, 2);
  const sdk::CameraInfo* camera = camera_object != nullptr ? camera_object->get<sdk::CameraInfo>() : nullptr;
  if (camera == nullptr) {
    luaL_typeerror(L, 2, "camera info");
  }
  native::DepthOptions options;
  if (!lua_isnoneornil(L, 3)) {
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_rawgetfield(L, 3, "step");
    if (!lua_isnil(L, -1)) {
      options.step = checkPixelInteger(L, -1);
    }
    lua_pop(L, 1);
    options.max_depth = readNumber(L, 3, "max_depth", options.max_depth);
  }
  if (receiver.depth != nullptr && !receiver.depth->distortion_model.empty()) {
    pushUnavailable(L, "depth must be rectified before unprojection");
    return 1;
  }
  Expected<DecodedPixels> pixels = decodeReceiver(L, media, receiver);
  if (!pixels) {
    pushUnavailable(L, pixels.error());
    return 1;
  }
  const std::string& encoding = receiver.image != nullptr ? receiver.image->encoding : receiver.depth->encoding;
  const Timestamp stamp = ownStamp(receiver.box->anchor.object).value_or(0);
  Expected<sdk::PointCloud> cloud = retryAfterCollect(
      L, [&] { return native::depthToPointCloud(*pixels, encoding, stamp, *camera, options, media.limits()); });
  if (!cloud && cloud.error() != native::kCancelledError) {
    pushUnavailable(L, cloud.error());
    return 1;
  }
  pushObject(L, sdk::BuiltinObject(nativeResult(L, std::move(cloud))));
  return 1;
}

/// `decoded:pixel(x, y)` -> the sample, or `{r, g, b[, a]}`.
int lDecodedPixel(lua_State* L) {
  const std::vector<float> pixel =
      nativeResult(L, native::imagePixel(checkDecoded(L), checkPixelInteger(L, 2), checkPixelInteger(L, 3)));
  if (pixel.size() == 1) {
    lua_pushnumber(L, pixel[0]);
    return 1;
  }
  constexpr const char* kNames[] = {"r", "g", "b", "a"};
  lua_createtable(L, 0, static_cast<int>(pixel.size()));
  for (std::size_t channel = 0; channel < pixel.size(); ++channel) {
    setNumber(L, kNames[channel], pixel[channel]);
  }
  return 1;
}

int lDecodedToGray(lua_State* L) {
  const DecodedPixels& image = checkDecoded(L);
  ScriptMedia& media = runMedia(L);
  pushDecoded(L, nativeResult(L, retryAfterCollect(L, [&] { return native::imageToGray(image, media.limits()); })));
  return 1;
}

/// `decoded:crop(x, y, width, height)`.
int lDecodedCrop(lua_State* L) {
  const DecodedPixels& image = checkDecoded(L);
  const uint32_t x = checkPixelInteger(L, 2);
  const uint32_t y = checkPixelInteger(L, 3);
  const uint32_t width = checkPixelInteger(L, 4);
  const uint32_t height = checkPixelInteger(L, 5);
  ScriptMedia& media = runMedia(L);
  pushDecoded(L, nativeResult(L, retryAfterCollect(L, [&] {
                                return native::imageCrop(image, x, y, width, height, media.limits());
                              })));
  return 1;
}

/// `decoded:threshold(op, value[, channel])`: channel 0-3 or "r", "g", "b", "a"; matches over the budget are
/// `pj.unavailable`.
int lDecodedThreshold(lua_State* L) {
  const DecodedPixels& image = checkDecoded(L);
  const native::CompareOp op = checkComparison(L, 2, "threshold");
  const double value = luaL_checknumber(L, 3);
  uint32_t channel = 0;
  if (lua_type(L, 4) == LUA_TSTRING) {
    channel = nativeResult(L, thresholdChannel(lua_tostring(L, 4)));
  } else if (!lua_isnoneornil(L, 4)) {
    channel = checkPixelInteger(L, 4);
  }
  ScriptMedia& media = runMedia(L);
  auto annotations =
      retryAfterCollect(L, [&] { return native::imageThreshold(image, op, value, channel, media.limits()); });
  if (!annotations && annotations.error().starts_with(native::kNativeLimitError)) {
    pushUnavailable(L, annotations.error());
    return 1;
  }
  return pushCharged(L, nativeResult(L, std::move(annotations)));
}

/// `decoded:stats()` -> `{min, max, mean}` over the finite samples.
int lDecodedStats(lua_State* L) {
  const DecodedPixels& image = checkDecoded(L);
  const native::ImageStats stats = nativeResult(L, native::imageStats(image, runMedia(L).limits()));
  lua_createtable(L, 0, 3);
  setNumber(L, "min", stats.min);
  setNumber(L, "max", stats.max);
  setNumber(L, "mean", stats.mean);
  return 1;
}

// ---- video: frame_at -------------------------------------------------------------------------------------------

/// `video:frame_at(t)` -> `{image, presentation_ns}`, or `nil, reason`: the frame presented at `t`, an integer number
/// of nanoseconds or a `pj.int64`.
int lVideoFrameAt(lua_State* L) {
  const sdk::VideoFrame* video = objectBoxAt(L, 1).anchor.object.get<sdk::VideoFrame>();
  if (video == nullptr) {
    luaL_typeerror(L, 1, "video frame");
  }
  Timestamp t = 0;
  if (const std::optional<int64_t> boxed = toInt64(L, 2)) {
    t = *boxed;
  } else {
    const double ns = lua_type(L, 2) == LUA_TNUMBER ? lua_tonumber(L, 2) : std::nan("");
    if (!(std::abs(ns) < 9.2e18) || std::floor(ns) != ns) {
      luaL_error(L, "frame_at: time must be an integer number of nanoseconds or a pj.int64");
    }
    t = static_cast<Timestamp>(ns);
  }
  ScriptMedia& media = runMedia(L);
  Expected<sdk::BuiltinObject> image = retryAfterCollect(L, [&] { return media.frameAt(video, t); });
  if (!image) {
    lua_pushnil(L);
    lua_pushlstring(L, image.error().data(), image.error().size());
    return 2;
  }
  const Timestamp presentation = image->get<sdk::Image>()->timestamp_ns;
  lua_createtable(L, 0, 2);
  pushObject(L, std::move(*image));
  lua_setfield(L, -2, "image");
  pushInt64(L, presentation);
  lua_setfield(L, -2, "presentation_ns");
  return 1;
}

/// `__index` of decoded pixels: `width`, `height`, `channels`, then the methods (upvalue 1).
int lDecodedIndex(lua_State* L) {
  const DecodedPixels& image = checkDecoded(L);
  const char* key = luaL_checkstring(L, 2);
  const std::string_view name(key);
  if (name == "width" || name == "height" || name == "channels") {
    lua_pushnumber(L, name == "width" ? image.width : name == "height" ? image.height : image.channels);
    return 1;
  }
  lua_pushvalue(L, 2);
  lua_rawget(L, lua_upvalueindex(1));
  return 1;
}

/// Builds the registry metatable `name`, locked against `getmetatable`, and leaves it on the stack.
void newLockedMetatable(lua_State* L, const char* name) {
  luaL_newmetatable(L, name);  // the registry keeps it alive
  lua_pushstring(L, "locked");
  lua_setfield(L, -2, "__metatable");
}

/// `newLockedMetatable`, also made the metatable of every userdata tagged `tag`.
void newTagMetatable(lua_State* L, const char* name, Tag tag) {
  newLockedMetatable(L, name);
  lua_pushvalue(L, -1);
  lua_setuserdatametatable(L, tag);
}

}  // namespace

void setFunctions(
    lua_State* L, std::initializer_list<std::pair<const char*, lua_CFunction>> functions, const void* upvalue) {
  for (const auto& [name, function] : functions) {
    if (upvalue != nullptr) {
      lua_pushlightuserdata(L, const_cast<void*>(upvalue));
      lua_pushcclosure(L, function, name, 1);
    } else {
      lua_pushcfunction(L, function, name);
    }
    lua_setfield(L, -2, name);
  }
}

void setRunMedia(lua_State* L, ScriptMedia& media) {
  lua_setthreaddata(L, &media);
}

void registerBinder(lua_State* L) {
  lua_setuserdatadtor(L, kObjectTag, &destroy<ObjectBox>);
  lua_setuserdatadtor(L, kViewTag, &destroy<View>);
  lua_setuserdatadtor(L, kPointIteratorTag, &destroy<PointIterator>);
  lua_setuserdatadtor(L, kSceneBuilderTag, &destroy<SceneSlot>);
  lua_setuserdatadtor(L, kAnnotationsBuilderTag, &destroy<AnnotationsSlot>);
  lua_setuserdatadtor(L, kDecodedTag, &destroy<DecodedPixels>);
  lua_setuserdatadtor(L, kCloudBuilderTag, &destroy<CloudSlot>);
  lua_setuserdatadtor(L, kVoxelsBuilderTag, &destroy<VoxelsSlot>);

  newTagMetatable(L, "pj.object", kObjectTag);
  lua_createtable(L, 0, 1);  // methods by type
  lua_createtable(L, 0, 8);
  setFunctions(
      L, {{"count", lCloudCount},
          {"downsample", lCloudDownsample},
          {"bounds", lCloudBounds},
          {"extreme_point", lCloudExtremePoint},
          {"crop_box", lCloudCropBox},
          {"filter", lCloudFilter},
          {"transform", lCloudTransform},
          {"xyz_points", lCloudXyzPoints}});
  lua_rawseti(L, -2, static_cast<int>(sdk::BuiltinObjectType::kPointCloud));
  for (const sdk::BuiltinObjectType image : {sdk::BuiltinObjectType::kImage, sdk::BuiltinObjectType::kDepthImage}) {
    lua_createtable(L, 0, 3);
    setFunctions(
        L,
        {{"representation", lImageRepresentation}, {"decode", lImageDecode}, {"to_point_cloud", lImageToPointCloud}});
    lua_rawseti(L, -2, static_cast<int>(image));
  }
  lua_createtable(L, 0, 1);
  setFunctions(L, {{"count", lAnnotationsCount}});
  lua_rawseti(L, -2, static_cast<int>(sdk::BuiltinObjectType::kImageAnnotations));
  lua_createtable(L, 0, 1);
  setFunctions(L, {{"frame_at", lVideoFrameAt}});
  lua_rawseti(L, -2, static_cast<int>(sdk::BuiltinObjectType::kVideoFrame));
  lua_pushcclosure(L, lObjectIndex, "__index", 1);
  lua_setfield(L, -2, "__index");
  setFunctions(L, {{"__newindex", lReadOnly}});
  lua_pop(L, 1);

  newTagMetatable(L, "pj.view", kViewTag);
  setFunctions(L, {{"__index", lViewIndex}, {"__newindex", lReadOnly}, {"__len", lViewLen}, {"__iter", lViewIter}});
  lua_pop(L, 1);

  newTagMetatable(L, "pj.int64", kInt64Tag);
  lua_createtable(L, 0, 2);
  setFunctions(L, {{"diff_s", lInt64DiffS}, {"to_number", lInt64ToNumber}});
  lua_setfield(L, -2, "__index");
  setFunctions(L, {{"__tostring", lInt64ToString}, {"__eq", lInt64Eq}, {"__lt", lInt64Lt}, {"__le", lInt64Le}});
  lua_pop(L, 1);

  newTagMetatable(L, "pj.point_iterator", kPointIteratorTag);
  lua_pop(L, 1);

  newTagMetatable(L, "pj.scene_builder", kSceneBuilderTag);
  lua_createtable(L, 0, 5);
  setFunctions(
      L, {{"sphere", lSceneSphere},
          {"cube", lSceneCube},
          {"text", lSceneText},
          {"line_strip", lSceneLineStrip},
          {"finish", lSceneFinish}});
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  newTagMetatable(L, "pj.annotations_builder", kAnnotationsBuilderTag);
  lua_createtable(L, 0, 4);
  setFunctions(
      L, {{"points", lAnnotationsPoints},
          {"circle", lAnnotationsCircle},
          {"text", lAnnotationsText},
          {"finish", lAnnotationsFinish}});
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  newTagMetatable(L, "pj.cloud_builder", kCloudBuilderTag);
  lua_createtable(L, 0, 3);
  setFunctions(L, {{"add", lCloudAdd}, {"add_points", lCloudAddPoints}, {"finish", lCloudFinish}});
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  newTagMetatable(L, "pj.voxels_builder", kVoxelsBuilderTag);
  lua_createtable(L, 0, 2);
  setFunctions(L, {{"set", lVoxelsSet}, {"finish", lVoxelsFinish}});
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  newTagMetatable(L, "pj.decoded_image", kDecodedTag);
  lua_createtable(L, 0, 5);
  setFunctions(
      L, {{"pixel", lDecodedPixel},
          {"to_gray", lDecodedToGray},
          {"crop", lDecodedCrop},
          {"threshold", lDecodedThreshold},
          {"stats", lDecodedStats}});
  lua_pushcclosure(L, lDecodedIndex, "__index", 1);
  lua_setfield(L, -2, "__index");
  setFunctions(L, {{"__newindex", lReadOnly}});
  lua_pop(L, 1);

  newLockedMetatable(L, kUnavailableMetatable);
  lua_pop(L, 1);
}

void installBuilders(lua_State* L) {
  lua_createtable(L, 0, 1);
  setFunctions(L, {{"new", lSceneNew}});
  lua_setfield(L, -2, "scene");
  lua_createtable(L, 0, 1);
  setFunctions(L, {{"new", lAnnotationsNew}});
  lua_setfield(L, -2, "annotations");
  lua_createtable(L, 0, 1);
  setFunctions(L, {{"new", lCloudNew}});
  lua_setfield(L, -2, "cloud");
  lua_createtable(L, 0, 1);
  setFunctions(L, {{"new", lVoxelsNew}});
  lua_setfield(L, -2, "voxels");
}

void pushObject(lua_State* L, sdk::BuiltinObject object, std::shared_ptr<const native::MemoryReservation> charge) {
  const sdk::FieldTableView* table = sdk::describe(object.type());
  const void* data = structPointer(object);
  newBox(L, kObjectTag, ObjectBox{Anchor{std::move(object), std::move(charge)}, table, data});
}

const sdk::BuiltinObject* toObject(lua_State* L, int idx) {
  const auto* box = static_cast<const ObjectBox*>(lua_touserdatatagged(L, idx, kObjectTag));
  return box != nullptr ? &box->anchor.object : nullptr;
}

std::shared_ptr<const void> toCharge(lua_State* L, int idx) {
  const auto* box = static_cast<const ObjectBox*>(lua_touserdatatagged(L, idx, kObjectTag));
  return box != nullptr ? box->anchor.charge : nullptr;
}

void pushStructView(lua_State* L, const sdk::FieldTableView& table, const void* owner, sdk::BuiltinObject keep) {
  newBox(L, kViewTag, View{Anchor{std::move(keep), nullptr}, &table, nullptr, owner});
}

void pushInt64(lua_State* L, int64_t value) {
  newBox(L, kInt64Tag, value);
}

std::optional<int64_t> toInt64(lua_State* L, int idx) {
  const auto* value = static_cast<const int64_t*>(lua_touserdatatagged(L, idx, kInt64Tag));
  return value != nullptr ? std::optional<int64_t>(*value) : std::nullopt;
}

void pushUnavailable(lua_State* L, std::string_view reason) {
  lua_createtable(L, 0, 1);
  lua_pushlstring(L, reason.data(), std::min(reason.size(), kMaxReasonBytes));
  lua_setfield(L, -2, "reason");
  luaL_getmetatable(L, kUnavailableMetatable);
  lua_setmetatable(L, -2);
}

std::optional<std::string> toUnavailable(lua_State* L, int idx) {
  idx = lua_absindex(L, idx);
  if (!lua_istable(L, idx) || lua_getmetatable(L, idx) == 0) {
    return std::nullopt;
  }
  luaL_getmetatable(L, kUnavailableMetatable);
  const bool marker = lua_rawequal(L, -1, -2) != 0;
  lua_pop(L, 2);
  if (!marker) {
    return std::nullopt;
  }
  lua_rawgetfield(L, idx, "reason");
  std::size_t length = 0;
  const char* reason = lua_tolstring(L, -1, &length);
  std::string out(reason != nullptr ? reason : "", length);
  lua_pop(L, 1);
  return out;
}

void pushArray(lua_State* L, std::initializer_list<double> values) {
  lua_createtable(L, static_cast<int>(values.size()), 0);
  int position = 1;
  for (const double value : values) {
    lua_pushnumber(L, value);
    lua_rawseti(L, -2, position++);
  }
}

}  // namespace PJ::scripting::detail

namespace PJ::scripting {

namespace {

/// The bytes a described struct holds beyond its fixed members: strings, list elements and record buffers,
/// walked through its field table. A scalar and every list element count 8 bytes, so a list of empty
/// elements (metadata pairs) is not free.
std::size_t describedBytes(const sdk::FieldTableView& table, const void* owner) {
  std::size_t total = 0;
  for (const sdk::FieldDescriptor& field : table.fields) {
    switch (field.kind) {
      case sdk::FieldKind::kString:
        total += field.get_string(owner).size();
        break;
      case sdk::FieldKind::kStruct:
        total += describedBytes(*field.nested, field.struct_ptr(owner));
        break;
      case sdk::FieldKind::kList: {
        const std::size_t count = field.list_size(owner);
        total += count * sizeof(double);
        for (std::size_t index = 0; field.nested != nullptr && index < count; ++index) {
          total += describedBytes(*field.nested, field.list_at(owner, index));
        }
        break;
      }
      case sdk::FieldKind::kBuffer:
        total += field.buffer(owner).bytes.size();
        break;
      default:
        total += sizeof(double);
        break;
    }
  }
  return total;
}

}  // namespace

std::size_t estimateBytes(const ScriptValue& value) {
  if (const auto* object = std::get_if<sdk::BuiltinObject>(&value)) {
    const sdk::FieldTableView* table = sdk::describe(object->type());
    const void* data = detail::structPointer(*object);
    return sizeof(sdk::BuiltinObject) + (table != nullptr && data != nullptr ? describedBytes(*table, data) : 0);
  }
  if (const auto* text = std::get_if<std::string>(&value)) {
    return text->size();
  }
  if (const auto* unavailable = std::get_if<Unavailable>(&value)) {
    return unavailable->reason.size();
  }
  return sizeof(double);
}

}  // namespace PJ::scripting
