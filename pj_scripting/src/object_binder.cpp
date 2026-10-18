// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "object_binder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include "lualib.h"
#include "luau_sandbox_state.h"
#include "native/memory_budget.h"
#include "pj_base/builtin/field_table_registry.hpp"

namespace PJ::scripting::detail {
namespace {

// Userdata tags: a tag both identifies a boxed value and selects its destructor.
enum Tag : int {
  kObjectTag = 1,
  kViewTag,
  kInt64Tag,
  kPointIteratorTag,
};

constexpr const char* kUnavailableMetatable = "pj.unavailable";
constexpr std::size_t kMaxReasonBytes = 1024;

/// A script object: the canonical value, its field table and the struct behind it, resolved once.
struct ObjectBox {
  sdk::BuiltinObject object;
  const sdk::FieldTableView* table = nullptr;
  const void* data = nullptr;
};

/// A read-only window onto part of an object: a struct (`table`) or a list field (`list`, read on `owner`).
/// `keep` shares the object the window reads.
struct View {
  sdk::BuiltinObject keep;
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

/// Calls `visit` with the typed struct behind `object`, for each type with an SDK field table.
template <class... Types, class Visit>
bool visitAs(const sdk::BuiltinObject& object, Visit&& visit) {
  return ((object.get<Types>() != nullptr && (visit(*object.get<Types>()), true)) || ...);
}
template <class Visit>
bool visitDescribed(const sdk::BuiltinObject& object, Visit&& visit) {
  return visitAs<
      sdk::PointCloud, sdk::SceneEntities, sdk::ImageAnnotations, sdk::Image, sdk::DepthImage, sdk::CameraInfo,
      sdk::VideoFrame, sdk::FrameTransforms>(object, visit);
}

const void* structPointer(const sdk::BuiltinObject& object) {
  const void* pointer = nullptr;
  visitDescribed(object, [&](const auto& value) { pointer = &value; });
  return pointer;
}

const sdk::FieldDescriptor* stampField(sdk::BuiltinObjectType type) {
  const sdk::FieldTableView* table = sdk::describe(type);
  if (table != nullptr) {
    for (const sdk::FieldDescriptor& field : table->fields) {
      if (field.kind == sdk::FieldKind::kInt64 && (field.name == "timestamp_ns" || field.name == "timestamp")) {
        return &field;
      }
    }
  }
  return nullptr;
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

void pushField(lua_State* L, const sdk::FieldDescriptor& field, const void* owner, const sdk::BuiltinObject& keep) {
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
    lua_State* L, const sdk::FieldTableView& table, const void* owner, const sdk::BuiltinObject& keep, int key_index) {
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
  lua_rawgeti(L, lua_upvalueindex(1), static_cast<int>(box.object.type()));
  if (lua_istable(L, -1)) {
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) {
      return 1;
    }
  }
  return pushNamedField(L, *box.table, box.data, box.object, 2);
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
  const sdk::PointCloud* cloud = objectBoxAt(L, 1).object.get<sdk::PointCloud>();
  if (cloud == nullptr) {
    luaL_typeerror(L, 1, "point cloud");
  }
  return *cloud;
}

/// Runs an allocating native operation and pushes its cloud. A failure on the native budget runs one
/// full collection, which frees the bytes still charged to dead outputs, then retries once.
template <class Operation>
int pushNativeCloud(lua_State* L, Operation&& operation) {
  Expected<sdk::PointCloud> result = operation();
  if (!result && result.error().starts_with(native::kNativeLimitError)) {
    lua_gc(L, LUA_GCCOLLECT, 0);
    result = operation();
  }
  pushObject(L, sdk::BuiltinObject(nativeResult(L, std::move(result))));
  return 1;
}

/// The required array field `key` of the table at `table`: N numbers.
template <std::size_t N>
std::array<double, N> readArray(lua_State* L, int table, const char* key) {
  lua_getfield(L, table, key);
  std::array<double, N> values{};
  for (std::size_t index = 0; index < N; ++index) {
    if (lua_istable(L, -1)) {
      lua_rawgeti(L, -1, static_cast<int>(index) + 1);
    } else {
      lua_pushnil(L);
    }
    if (lua_type(L, -1) != LUA_TNUMBER) {
      luaL_error(L, "'%s' must be an array of %d numbers", key, static_cast<int>(N));
    }
    values[index] = lua_tonumber(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return values;
}

sdk::Vector3 readVector(lua_State* L, int table, const char* key) {
  const auto values = readArray<3>(L, table, key);
  return {values[0], values[1], values[2]};
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
  const std::string_view which = luaL_checkstring(L, 3);
  if (which != "max" && which != "min") {
    luaL_error(L, "extreme_point: the second argument must be 'max' or 'min'");
  }
  const auto best = nativeResult(
      L, native::extremePoint(cloud, channel, which == "max" ? native::Extreme::kMax : native::Extreme::kMin));
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
  const sdk::Vector3 min = readVector(L, 2, "min");
  const sdk::Vector3 max = readVector(L, 2, "max");
  return pushNativeCloud(L, [&] { return native::cropBox(cloud, min, max, upvalueAs<native::NativeLimits>(L)); });
}

/// `cloud:filter(channel, op, value)`, op one of `<`, `<=`, `>`, `>=`, `==`, `~=`.
int lCloudFilter(lua_State* L) {
  using Comparison = std::pair<std::string_view, native::CompareOp>;
  constexpr std::array<Comparison, 6> kComparisons{{
      {"<", native::CompareOp::kLt},
      {"<=", native::CompareOp::kLe},
      {">", native::CompareOp::kGt},
      {">=", native::CompareOp::kGe},
      {"==", native::CompareOp::kEq},
      {"~=", native::CompareOp::kNe},
  }};
  const sdk::PointCloud& cloud = checkCloud(L);
  const char* channel = luaL_checkstring(L, 2);
  const char* op = luaL_checkstring(L, 3);
  const double threshold = luaL_checknumber(L, 4);
  const auto comparison = std::ranges::find(kComparisons, std::string_view(op), &Comparison::first);
  if (comparison == kComparisons.end()) {
    luaL_error(L, "filter: unknown comparison '%s' (expected <, <=, >, >=, ==, ~=)", op);
  }
  return pushNativeCloud(L, [&] {
    return native::filterByField(cloud, channel, comparison->second, threshold, upvalueAs<native::NativeLimits>(L));
  });
}

/// `cloud:transform(tf)`, `tf` as `pj.tf.lookup` returns it: the result is in `tf.target_frame` when set.
int lCloudTransform(lua_State* L) {
  const sdk::PointCloud& cloud = checkCloud(L);
  luaL_checktype(L, 2, LUA_TTABLE);
  sdk::FrameTransform tf;
  tf.translation = readVector(L, 2, "translation");
  const auto rotation = readArray<4>(L, 2, "rotation");
  tf.rotation = {rotation[0], rotation[1], rotation[2], rotation[3]};
  lua_getfield(L, 2, "target_frame");
  if (lua_type(L, -1) == LUA_TSTRING) {
    std::size_t length = 0;
    const char* frame = lua_tolstring(L, -1, &length);
    tf.parent_frame_id.assign(frame, length);  // charged with the output by transformXyz
  }
  lua_pop(L, 1);
  return pushNativeCloud(L, [&] { return native::transformXyz(cloud, tf, upvalueAs<native::NativeLimits>(L)); });
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
  newBox(L, kPointIteratorTag, PointIterator{objectBoxAt(L, 1).object, access, 0});
  lua_pushnil(L);
  return 3;
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

void registerBinder(lua_State* L, const native::NativeLimits& limits) {
  lua_setuserdatadtor(L, kObjectTag, &destroy<ObjectBox>);
  lua_setuserdatadtor(L, kViewTag, &destroy<View>);
  lua_setuserdatadtor(L, kPointIteratorTag, &destroy<PointIterator>);

  newTagMetatable(L, "pj.object", kObjectTag);
  lua_createtable(L, 0, 1);  // methods by type
  lua_createtable(L, 0, 7);
  setFunctions(
      L,
      {{"count", lCloudCount},
       {"bounds", lCloudBounds},
       {"extreme_point", lCloudExtremePoint},
       {"crop_box", lCloudCropBox},
       {"filter", lCloudFilter},
       {"transform", lCloudTransform},
       {"xyz_points", lCloudXyzPoints}},
      &limits);
  lua_rawseti(L, -2, static_cast<int>(sdk::BuiltinObjectType::kPointCloud));
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
  newLockedMetatable(L, kUnavailableMetatable);
  lua_pop(L, 1);
}

bool canEnterScript(const sdk::BuiltinObject& object) {
  return sdk::describe(object.type()) != nullptr && structPointer(object) != nullptr;
}

void pushObject(lua_State* L, sdk::BuiltinObject object) {
  const sdk::FieldTableView* table = sdk::describe(object.type());
  const void* data = structPointer(object);
  newBox(L, kObjectTag, ObjectBox{std::move(object), table, data});
}

const sdk::BuiltinObject* toObject(lua_State* L, int idx) {
  const auto* box = static_cast<const ObjectBox*>(lua_touserdatatagged(L, idx, kObjectTag));
  return box != nullptr ? &box->object : nullptr;
}

void pushStructView(lua_State* L, const sdk::FieldTableView& table, const void* owner, sdk::BuiltinObject keep) {
  newBox(L, kViewTag, View{std::move(keep), &table, nullptr, owner});
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

std::optional<Timestamp> ownStamp(const sdk::BuiltinObject& object) {
  const sdk::FieldDescriptor* field = stampField(object.type());
  const void* pointer = structPointer(object);
  return field != nullptr && pointer != nullptr ? std::optional<Timestamp>(field->get_int64(pointer)) : std::nullopt;
}

sdk::BuiltinObject withStampShifted(const sdk::BuiltinObject& object, Timestamp shift) {
  const sdk::FieldDescriptor* field = stampField(object.type());
  if (shift == 0 || field == nullptr) {
    return object;
  }
  sdk::BuiltinObject shifted = object;
  visitDescribed(object, [&](const auto& value) {
    const Timestamp stamp = field->get_int64(&value);
    if (stamp == 0) {
      return;
    }
    // Saturates like pj_datastore's saturatingAdd: a stamp near the limits must not wrap around.
    constexpr Timestamp kMax = std::numeric_limits<Timestamp>::max();
    constexpr Timestamp kMin = std::numeric_limits<Timestamp>::min();
    const Timestamp moved = shift > 0 && stamp > kMax - shift   ? kMax
                            : shift < 0 && stamp < kMin - shift ? kMin
                                                                : stamp + shift;
    auto copy = value;  // shares the payload bytes through their anchor
    field->set_int64(&copy, moved);
    shifted = sdk::BuiltinObject(std::move(copy));
  });
  return shifted;
}

}  // namespace PJ::scripting::detail
