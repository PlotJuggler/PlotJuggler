// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The Luau object-script engine: compile, run, output binding, field reads, the point-cloud methods
// and pj.tf (docs/derived_objects/SCENARIOS.md, rows REC, LUA, CACHE, TFL, BUD).

#include "pj_scripting/object_script.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "lua.h"
#include "luacode.h"
#include "luau_sandbox_state.h"
#include "object_binder.h"
#include "pj_base/buffer_anchor.hpp"
#include "pj_base/builtin/camera_info.hpp"
#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_annotations.hpp"
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/scene_entities.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_tf/tf_buffer.h"

namespace PJ::scripting {
namespace {

using sdk::PointCloud;
using sdk::PointField;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr int64_t kSecond = 1'000'000'000;

// ---- fixtures ----------------------------------------------------------------

// A flat cloud: x/y/z/intensity as float32 at offsets 0/4/8/12, `point_step` bytes per record (>= 16;
// the rest is padding, as a lidar's extra channels would be).
PointCloud makeCloud(const std::vector<std::array<float, 4>>& points, uint32_t point_step = 16) {
  PointCloud cloud;
  cloud.width = static_cast<uint32_t>(points.size());
  cloud.height = 1;
  cloud.point_step = point_step;
  cloud.row_step = cloud.point_step * cloud.width;
  cloud.is_dense = true;
  cloud.frame_id = "base_link";
  cloud.fields = {
      PointField{.name = "x", .offset = 0, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "y", .offset = 4, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "z", .offset = 8, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "intensity", .offset = 12, .datatype = PointField::Datatype::kFloat32, .count = 1},
  };
  cloud.timestamp_ns = kSecond;
  std::vector<uint8_t> bytes(static_cast<size_t>(cloud.point_step) * points.size());
  for (size_t index = 0; index < points.size(); ++index) {
    std::memcpy(bytes.data() + index * cloud.point_step, points[index].data(), sizeof(float) * 4);
  }
  sdk::PayloadView view = sdk::makePayloadView(std::move(bytes));
  cloud.data = view.bytes;
  cloud.anchor = view.anchor;
  return cloud;
}

// `count` points along x, at x = 0, 1, 2, ...
PointCloud makeLineCloud(size_t count, uint32_t point_step = 16) {
  std::vector<std::array<float, 4>> points(count);
  for (size_t index = 0; index < count; ++index) {
    points[index] = {static_cast<float>(index), 0, 0, 0};
  }
  return makeCloud(points, point_step);
}

// A 2x2 organized cloud whose rows carry 8 bytes of padding (row_step 40, not 32).
// Row 0: (0,0,0), (1,1,1). Row 1: (2,2,2), (3,3,3).
PointCloud makeOrganizedCloud() {
  PointCloud cloud = makeCloud({});
  cloud.width = 2;
  cloud.height = 2;
  cloud.row_step = 40;
  cloud.frame_id = "camera";
  std::vector<uint8_t> bytes(static_cast<size_t>(cloud.row_step) * cloud.height, 0);
  const float points[4][4] = {{0, 0, 0, 0}, {1, 1, 1, 0}, {2, 2, 2, 0}, {3, 3, 3, 0}};
  for (uint32_t row = 0; row < cloud.height; ++row) {
    for (uint32_t col = 0; col < cloud.width; ++col) {
      std::memcpy(
          bytes.data() + row * cloud.row_step + col * cloud.point_step, points[row * cloud.width + col],
          sizeof(float) * 4);
    }
  }
  sdk::PayloadView view = sdk::makePayloadView(std::move(bytes));
  cloud.data = view.bytes;
  cloud.anchor = view.anchor;
  return cloud;
}

NamedValue object(std::string name, sdk::BuiltinObject value, Timestamp stamp_shift = 0) {
  return NamedValue{std::move(name), ScriptValue{std::move(value)}, stamp_shift};
}

std::vector<OutputSpec> specs(std::initializer_list<const char*> declarations) {
  std::vector<OutputSpec> out;
  for (const char* declaration : declarations) {
    auto spec = parseOutputSpec(declaration);
    EXPECT_TRUE(spec.has_value()) << declaration;
    if (spec) {
      out.push_back(*spec);
    }
  }
  return out;
}

Expected<ScriptBundle, ScriptError> run(
    std::string_view source, const std::vector<NamedValue>& inputs = {}, const std::vector<OutputSpec>& outputs = {},
    std::string_view params_json = "{}", const RunOptions& options = {}) {
  Expected<CompiledScript> compiled = compileOnDemandScript(source);
  if (!compiled) {
    return unexpected(ScriptError{compiled.error()});
  }
  return runOnDemandScript(*compiled, inputs, params_json, outputs, options);
}

RunOptions withNativeBudget(std::size_t bytes) {
  RunOptions options;
  options.native_budget_bytes = bytes;
  return options;
}

const ScriptValue* find(const ScriptBundle& bundle, std::string_view name) {
  for (const NamedValue& output : bundle.outputs) {
    if (output.name == name) {
      return &output.value;
    }
  }
  return nullptr;
}

double number(const ScriptBundle& bundle, std::string_view name) {
  const ScriptValue* value = find(bundle, name);
  return value != nullptr && std::holds_alternative<double>(*value) ? std::get<double>(*value) : std::nan("");
}

std::string text(const ScriptBundle& bundle, std::string_view name) {
  const ScriptValue* value = find(bundle, name);
  return value != nullptr && std::holds_alternative<std::string>(*value) ? std::get<std::string>(*value)
                                                                         : std::string("<missing>");
}

bool contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

bool contains(const ScriptError& error, std::string_view needle) {
  return contains(error.message, needle);
}

// "name:type,..." over a bundle's outputs, in order; the type is the value's, "unknown" when unavailable.
std::string describe(const ScriptBundle& bundle) {
  std::string out;
  for (const NamedValue& output : bundle.outputs) {
    out += (out.empty() ? "" : ",") + output.name + ":";
    if (std::holds_alternative<double>(output.value)) {
      out += "number";
    } else if (std::holds_alternative<std::string>(output.value)) {
      out += "string";
    } else if (const auto* obj = std::get_if<sdk::BuiltinObject>(&output.value)) {
      out += std::string(sdk::name(obj->type()));
    } else {
      out += "unknown";
    }
  }
  return out;
}

// ---- TF fixtures -------------------------------------------------------------

pj::tf::TimePoint at(int64_t ns) {
  return PJ::fromRaw(ns);
}

/// Adds `parent <- child` to `buffer`: header `stamp`, entry time `entry`.
void addTf(
    pj::tf::TransformBuffer& buffer, const std::string& parent, const std::string& child, int64_t stamp, int64_t entry,
    glm::dvec3 translation, glm::dquat rotation = glm::dquat(1, 0, 0, 0)) {
  const pj::tf::StampedTransform tf{at(stamp), parent, child, pj::tf::Transform(translation, rotation)};
  ASSERT_TRUE(buffer.setTransform(tf, at(entry)).has_value());
}

/// A snapshot holding one static `map <- base_link` edge, translation {1, 2, 3}, stamped at 1 s.
std::shared_ptr<const pj::tf::TransformSnapshot> mapFromBaseLink() {
  pj::tf::TransformBuffer buffer(pj::tf::TransformBuffer::kKeepAll);
  addTf(buffer, "map", "base_link", kSecond, kSecond, {1, 2, 3});
  return buffer.snapshot();
}

RunOptions hostAt(Timestamp query_time, std::shared_ptr<const pj::tf::TransformSnapshot> tf = mapFromBaseLink()) {
  RunOptions options;
  options.tf = std::move(tf);
  options.query_time = query_time;
  return options;
}

}  // namespace

std::ostream& operator<<(std::ostream& out, const ScriptError& error) {
  return out << error.message;
}

// ---- output declarations (REC-06, GAP-42) --------------------------------------

TEST(ObjectScript, ParseOutputSpecAcceptsAndRejects) {
  auto number_spec = parseOutputSpec("max_z:number");
  ASSERT_TRUE(number_spec.has_value()) << number_spec.error();
  EXPECT_EQ(number_spec->name, "max_z");
  EXPECT_EQ(number_spec->type, OutputType::kNumber);

  auto string_spec = parseOutputSpec("report:string");
  ASSERT_TRUE(string_spec.has_value());
  EXPECT_EQ(string_spec->type, OutputType::kString);

  auto cloud = parseOutputSpec("cropped:kPointCloud");
  ASSERT_TRUE(cloud.has_value());
  EXPECT_EQ(cloud->type, OutputType::kObject);
  EXPECT_EQ(cloud->object_type, sdk::BuiltinObjectType::kPointCloud);

  auto scene = parseOutputSpec(" witness : kSceneEntities ");  // surrounding whitespace trimmed
  ASSERT_TRUE(scene.has_value());
  EXPECT_EQ(scene->name, "witness");
  EXPECT_EQ(scene->object_type, sdk::BuiltinObjectType::kSceneEntities);

  EXPECT_FALSE(parseOutputSpec("max_z").has_value());       // missing ':type'
  EXPECT_FALSE(parseOutputSpec("a:b:c").has_value());       // more than one ':'
  EXPECT_FALSE(parseOutputSpec(":number").has_value());     // empty name
  EXPECT_FALSE(parseOutputSpec("x:kNope").has_value());     // unknown type
  EXPECT_FALSE(parseOutputSpec("x:kNone").has_value());     // kNone is not declarable
  EXPECT_FALSE(parseOutputSpec("x y:number").has_value());  // whitespace inside the name
  EXPECT_FALSE(parseOutputSpec("x:").has_value());          // empty type
}

// GAP-42: an output may only name a type a script can produce: one with a field table.
TEST(ObjectScript, UnproducibleOutputTypeIsRejected) {
  for (const char* producible :
       {"a:kPointCloud", "a:kSceneEntities", "a:kImageAnnotations", "a:kImage", "a:kDepthImage", "a:kCameraInfo"}) {
    EXPECT_TRUE(parseOutputSpec(producible).has_value()) << producible;
  }
  for (const char* unproducible :
       {"grid:kOccupancyGrid", "voxels:kVoxelGrid", "mesh:kMesh3D", "z:kCompressedPointCloud"}) {
    auto spec = parseOutputSpec(unproducible);
    ASSERT_FALSE(spec.has_value()) << unproducible;
    EXPECT_TRUE(contains(spec.error(), "cannot produce")) << spec.error();
  }
  // An inferred output follows the same rule.
  auto inferred = run("local inputs = ...\nreturn inputs.tf", {object("tf", sdk::FrameTransforms{})});
  ASSERT_FALSE(inferred.has_value());
  EXPECT_TRUE(contains(inferred.error(), "cannot produce")) << inferred.error();
}

// ---- output binding: inference with no declared outputs (REC-07) ------------------

TEST(ObjectScriptInferred, NonTableNumberIsValue) {
  auto result = run("return 3.5");
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(describe(*result), "value:number");
  EXPECT_DOUBLE_EQ(number(*result, "value"), 3.5);
}

TEST(ObjectScriptInferred, ObjectIsValueOfItsType) {
  auto result = run("local inputs = ...; return inputs.cloud", {object("cloud", makeCloud({{0, 0, 0, 0}}))});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(describe(*result), "value:kPointCloud");
}

TEST(ObjectScriptInferred, KeyedTableIsSortedByName) {
  auto result = run("return { zeta = 1, alpha = 'x', mid = 2 }");
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(describe(*result), "alpha:string,mid:number,zeta:number");
}

TEST(ObjectScriptInferred, PositionalNamesByType) {
  auto mixed = run("local inputs = ...; return { 3.0, inputs.cloud }", {object("cloud", makeCloud({{0, 0, 0, 0}}))});
  ASSERT_TRUE(mixed.has_value()) << mixed.error();
  EXPECT_EQ(describe(*mixed), "value:number,cloud:kPointCloud");
  auto repeated = run("return { 1, 2, 'a', 'b' }");
  ASSERT_TRUE(repeated.has_value()) << repeated.error();
  EXPECT_EQ(describe(*repeated), "value:number,value_2:number,text:string,text_2:string");
}

TEST(ObjectScriptInferred, MixedAndEmptyAreErrors) {
  auto mixed = run("return { 1, a = 2 }");
  ASSERT_FALSE(mixed.has_value());
  EXPECT_TRUE(contains(mixed.error(), "either named or positional")) << mixed.error();
  auto empty = run("return {}");
  ASSERT_FALSE(empty.has_value());
  EXPECT_TRUE(contains(empty.error(), "no values")) << empty.error();
}

TEST(ObjectScriptInferred, UnavailableIsUnknown) {
  auto result = run("return pj.unavailable('nothing here')");
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(describe(*result), "value:unknown");
  const auto* unavailable = std::get_if<Unavailable>(find(*result, "value"));
  ASSERT_NE(unavailable, nullptr);
  EXPECT_EQ(unavailable->reason, "nothing here");
}

TEST(ObjectScriptInferred, UnsupportedValueNamesOutputAndType) {
  auto result = run("return { flag = true }");
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "flag")) << result.error();
  EXPECT_TRUE(contains(result.error(), "boolean")) << result.error();
}

// P3 folded (REC-07): lenient binding is the only binding. A non-table binds to the single declared output,
// an array by position, a keyed table by name; an undeclared key and a missing output stay errors.
TEST(ObjectScriptInferred, DeclaredOutputsBindLeniently) {
  const auto one = specs({"total:number"});
  auto scalar = run("return 4", {}, one);
  ASSERT_TRUE(scalar.has_value()) << scalar.error();
  EXPECT_EQ(describe(*scalar), "total:number");
  EXPECT_DOUBLE_EQ(number(*scalar, "total"), 4.0);

  auto positional = run("return { 1, 'a' }", {}, specs({"first:number", "label:string"}));
  ASSERT_TRUE(positional.has_value()) << positional.error();
  EXPECT_EQ(describe(*positional), "first:number,label:string");

  auto gap = run("return { 1, pj.unavailable('no points') }", {}, specs({"value:number", "cloud:kPointCloud"}));
  ASSERT_TRUE(gap.has_value()) << gap.error();
  EXPECT_EQ(describe(*gap), "value:number,cloud:unknown");

  auto extra_key = run("return { total = 1, other = 2 }", {}, one);
  ASSERT_FALSE(extra_key.has_value());
  EXPECT_TRUE(contains(extra_key.error(), "unexpected output 'other'")) << extra_key.error();

  auto extra_position = run("return { 1, 2 }", {}, one);
  ASSERT_FALSE(extra_position.has_value());
  EXPECT_TRUE(contains(extra_position.error(), "unexpected output")) << extra_position.error();

  auto missing = run("return {}", {}, one);
  ASSERT_FALSE(missing.has_value());
  EXPECT_TRUE(contains(missing.error(), "'total' missing")) << missing.error();

  auto wrong = run("return { total = 'x' }", {}, one);
  ASSERT_FALSE(wrong.has_value());
  EXPECT_TRUE(contains(wrong.error(), "must be a number")) << wrong.error();
}

// ---- output contract (LUA-08) -----------------------------------------------------

TEST(ObjectScript, MissingDeclaredOutputIsError) {
  auto result = run("return { foo = nil }", {}, specs({"foo:number"}));
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "'foo' missing")) << result.error();
}

TEST(ObjectScript, WrongOutputTypeIsError) {
  auto result = run("return { foo = \"not a number\" }", {}, specs({"foo:number"}));
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "must be a number")) << result.error();
  auto cloud =
      run("local inputs = ...; return { foo = inputs.cloud }", {object("cloud", makeCloud({{0, 0, 0, 0}}))},
          specs({"foo:kSceneEntities"}));
  ASSERT_FALSE(cloud.has_value());
  EXPECT_TRUE(contains(cloud.error(), "must be a kSceneEntities")) << cloud.error();
}

TEST(ObjectScript, ExtraOutputIsError) {
  auto result = run("return { foo = 1, bar = 2 }", {}, specs({"foo:number"}));
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "unexpected output 'bar'")) << result.error();
}

// ---- inputs, params, compilation ------------------------------------------------------

TEST(ObjectScript, ParamsJsonIsVisible) {
  auto result =
      run("local inputs, params = ...\nreturn { z = params.zmin, a = params.nested.a, f = params.flag and 1 or 0 }", {},
          specs({"z:number", "a:number", "f:number"}), R"({"zmin": -2.5, "nested": {"a": 3}, "flag": true})");
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "z"), -2.5);
  EXPECT_DOUBLE_EQ(number(*result, "a"), 3.0);
  EXPECT_DOUBLE_EQ(number(*result, "f"), 1.0);
}

TEST(ObjectScript, ScalarInputsAreVisible) {
  auto result =
      run("local inputs = ...\nreturn { doubled = inputs.threshold * 2, mode = inputs.mode .. '!' }",
          {NamedValue{"threshold", ScriptValue{2.5}}, NamedValue{"mode", ScriptValue{std::string("auto")}}},
          specs({"doubled:number", "mode:string"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "doubled"), 5.0);
  EXPECT_EQ(text(*result, "mode"), "auto!");
}

// An input type with no field table never enters the VM (REC-03 at the engine).
TEST(ObjectScript, InputWithoutAFieldTableIsAnError) {
  auto result = run("return 1", {object("grid", sdk::OccupancyGrid{})});
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "'grid'")) << result.error();
  EXPECT_TRUE(contains(result.error(), "cannot read")) << result.error();
}

// LUA-10: the syntax check is the compile; the error names the line.
TEST(ObjectScript, CompileRejectsSyntaxError) {
  EXPECT_TRUE(compileOnDemandScript("local inputs = ...\nreturn {}").has_value());
  auto bad = compileOnDemandScript("local x = 1\nthis is not (( valid luau");
  ASSERT_FALSE(bad.has_value());
  EXPECT_TRUE(contains(bad.error(), ":2:")) << bad.error();
}

// ---- object reads (LUA-07) and lazy lists ---------------------------------------------

TEST(ObjectScript, FieldAccess) {
  auto cloud = makeCloud({{0, 0, 0, 0}});
  cloud.timestamp_ns = 9'007'199'254'740'993LL;  // 2^53 + 1
  auto zero = makeCloud({{0, 0, 0, 0}});
  zero.timestamp_ns = 0;
  constexpr const char* kScript = R"LUAU(
    local inputs = ...
    return {
      frame = inputs.cloud.frame_id,
      first_field_name = inputs.cloud.fields[1].name,
      field_count = #inputs.cloud.fields,
      past_the_end = inputs.cloud.fields[5] == nil and 1 or 0,
      dense_num = inputs.cloud.is_dense and 1 or 0,
      ts_str = tostring(inputs.cloud.timestamp_ns),
      diff_s = inputs.cloud.timestamp_ns:diff_s(inputs.zero.timestamp_ns),
      names = (function()
        local names = {}
        for i, field in inputs.cloud.fields do names[i] = field.name end
        return table.concat(names, ",")
      end)(),
    }
  )LUAU";
  auto result =
      run(kScript, {object("cloud", cloud), object("zero", zero)},
          specs(
              {"frame:string", "first_field_name:string", "field_count:number", "past_the_end:number",
               "dense_num:number", "ts_str:string", "diff_s:number", "names:string"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(text(*result, "frame"), "base_link");
  EXPECT_EQ(text(*result, "first_field_name"), "x");
  EXPECT_DOUBLE_EQ(number(*result, "field_count"), 4.0);
  EXPECT_DOUBLE_EQ(number(*result, "past_the_end"), 1.0);
  EXPECT_DOUBLE_EQ(number(*result, "dense_num"), 1.0);
  // A double would round 2^53+1 down to ...992: exactness proves the field went through pj.int64.
  EXPECT_EQ(text(*result, "ts_str"), "9007199254740993");
  EXPECT_NEAR(number(*result, "diff_s"), 9'007'199'254'740'993.0 / 1e9, 1e-3);
  EXPECT_EQ(text(*result, "names"), "x,y,z,intensity");  // `for i, v in list do` iterates a list
}

// A fixed-size list of numbers reads like any list; an absent optional number reads nil.
TEST(ObjectScript, ScalarListsAndOptionalFields) {
  sdk::CameraInfo camera;
  camera.K = {500, 0, 320, 0, 500, 240, 0, 0, 1};
  sdk::Image image;
  sdk::Image compressed_depth;
  compressed_depth.compressed_depth_min = 0.5;
  constexpr const char* kScript = R"LUAU(
    local inputs = ...
    return { k = #inputs.camera.K, fx = inputs.camera.K[1], cy = inputs.camera.K[6],
             absent = inputs.image.compressed_depth_min == nil and 1 or 0,
             present = inputs.depth.compressed_depth_min }
  )LUAU";
  auto result =
      run(kScript, {object("camera", camera), object("image", image), object("depth", compressed_depth)},
          specs({"k:number", "fx:number", "cy:number", "absent:number", "present:number"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "k"), 9.0);
  EXPECT_DOUBLE_EQ(number(*result, "fx"), 500.0);
  EXPECT_DOUBLE_EQ(number(*result, "cy"), 240.0);
  EXPECT_DOUBLE_EQ(number(*result, "absent"), 1.0);
  EXPECT_DOUBLE_EQ(number(*result, "present"), 0.5);
}

TEST(ObjectScript, ReadOnlyObjects) {
  for (const char* write :
       {"inputs.cloud.frame_id = 'x'", "inputs.cloud.fields[1].name = 'q'", "inputs.cloud.fields[1] = 3"}) {
    auto result = run(std::string("local inputs = ...\n") + write + "\nreturn 1", {object("cloud", makeCloud({}))});
    ASSERT_FALSE(result.has_value()) << write;
    EXPECT_TRUE(contains(result.error(), "read-only")) << write << ": " << result.error();
  }
}

TEST(ObjectScript, RawBufferNotExposed) {
  auto result = run("local inputs = ...\nlocal d = inputs.cloud.data\nreturn 1", {object("cloud", makeCloud({}))});
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "raw buffer")) << result.error();
}

namespace {

struct Item {
  double value = 0.0;
};
struct Bag {
  std::vector<Item> items;
};

}  // namespace
}  // namespace PJ::scripting

template <>
struct PJ::sdk::FieldTable<PJ::scripting::Item> {
  static constexpr std::array<FieldDescriptor, 1> fields{field<&PJ::scripting::Item::value>("value")};
  static constexpr FieldTableView view{"Item", Span<const FieldDescriptor>(fields)};
};

namespace PJ::scripting {
namespace {

int list_reads = 0;

const void* countingListAt(const void* owner, size_t index) {
  ++list_reads;
  return &static_cast<const Bag*>(owner)->items[index];
}

// Runs `source` as `chunk(bag)` and returns its number; `bag.items` is read through a counting `list_at`.
double runOverBag(const Bag& bag, const std::string& source) {
  static const std::array<sdk::FieldDescriptor, 1> kFields = [] {
    sdk::FieldDescriptor items = sdk::field<&Bag::items>("items");
    items.list_at = &countingListAt;
    return std::array<sdk::FieldDescriptor, 1>{items};
  }();
  static const sdk::FieldTableView kTable{"Bag", Span<const sdk::FieldDescriptor>(kFields)};

  detail::BudgetState budget;
  lua_State* L = detail::newSandboxedState(&budget, BudgetLimits{});
  const native::NativeLimits limits;
  detail::registerBinder(L, limits);
  size_t size = 0;
  char* bytecode = luau_compile(source.data(), source.size(), nullptr, &size);
  double sum = std::nan("");
  if (luau_load(L, "=test", bytecode, size, 0) == 0) {
    detail::pushStructView(L, kTable, &bag, {});
    if (lua_pcall(L, 1, 1, 0) == 0) {
      sum = lua_tonumber(L, -1);
    }
  }
  std::free(bytecode);
  lua_close(L);
  return sum;
}

}  // namespace

// Lists are read element by element (ARCHITECTURE §6): iterating N elements reads each one once, so the
// cost is linear (a binder that materializes the list on every field access reads N * N times).
TEST(ObjectScript, ListElementsAreReadLazily) {
  for (const size_t count : {size_t{1000}, size_t{2000}}) {
    Bag bag;
    for (size_t index = 0; index < count; ++index) {
      bag.items.push_back(Item{1.0});
    }
    list_reads = 0;
    EXPECT_DOUBLE_EQ(
        runOverBag(bag, "local bag = ...\nlocal list = bag.items\nreturn #list"), static_cast<double>(count));
    EXPECT_EQ(list_reads, 0);  // the list and its length read no element
    EXPECT_DOUBLE_EQ(runOverBag(bag, "local bag = ...\nreturn bag.items[1].value"), 1.0);
    EXPECT_EQ(list_reads, 1);
    list_reads = 0;
    EXPECT_DOUBLE_EQ(
        runOverBag(
            bag, "local bag = ...\nlocal s = 0\nfor i = 1, #bag.items do s = s + bag.items[i].value end\nreturn s"),
        static_cast<double>(count));
    EXPECT_EQ(list_reads, static_cast<int>(count));
  }
}

// ---- point-cloud methods (LUA-01, -02, -03, -11, -13) ----------------------------------------

TEST(ObjectScript, CountBoundsExtremeOnSyntheticCloud) {
  auto cloud = makeCloud({{0, 0, 0, 0.1f}, {1, 2, 3, 0.5f}, {-1, -2, -3, 0.9f}});
  constexpr const char* kScript = R"LUAU(
    local inputs = ...
    local b = inputs.cloud:bounds()
    local ep = inputs.cloud:extreme_point("z", "max")
    return { count = inputs.cloud:count(), min_x = b.min[1], max_x = b.max[1], ep_index = ep.index, ep_z = ep.z }
  )LUAU";
  auto result =
      run(kScript, {object("cloud", cloud)},
          specs({"count:number", "min_x:number", "max_x:number", "ep_index:number", "ep_z:number"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "count"), 3.0);
  EXPECT_DOUBLE_EQ(number(*result, "min_x"), -1.0);
  EXPECT_DOUBLE_EQ(number(*result, "max_x"), 1.0);
  EXPECT_DOUBLE_EQ(number(*result, "ep_index"), 2.0);  // 1-based
  EXPECT_DOUBLE_EQ(number(*result, "ep_z"), 3.0);
}

TEST(ObjectScript, CropBoxPreservesWholeRecords) {
  auto cloud = makeCloud({{0, 0, 0, 0.1f}, {10, 10, 10, 0.5f}, {1, 1, 1, 0.9f}});
  auto result =
      run("local inputs = ...\nreturn { cropped = inputs.cloud:crop_box{min={-5,-5,-5}, max={5,5,5}} }",
          {object("cloud", cloud)}, specs({"cropped:kPointCloud"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  const PointCloud* out = std::get<sdk::BuiltinObject>(*find(*result, "cropped")).get<PointCloud>();
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->width, 2u);  // (10,10,10) is cropped out
  EXPECT_EQ(out->frame_id, "base_link");
  float intensity = 0.0f;  // the surviving (1,1,1) record keeps its intensity
  std::memcpy(&intensity, out->data.data() + out->point_step + 12, sizeof(float));
  EXPECT_FLOAT_EQ(intensity, 0.9f);
}

TEST(ObjectScript, FilterKeepsMatchingRecords) {
  auto cloud = makeCloud({{0, 0, 0, 0.1f}, {1, 0, 0, 0.5f}, {2, 0, 0, 0.9f}});
  auto result =
      run("local inputs = ...\nreturn { n = inputs.cloud:filter('intensity', '>=', 0.5):count() }",
          {object("cloud", cloud)}, specs({"n:number"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "n"), 2.0);
  for (const char* op : {"<>", "!="}) {
    auto bad =
        run(std::string("local inputs = ...\nreturn inputs.cloud:filter('x', '") + op + "', 1):count()",
            {object("cloud", cloud)});
    ASSERT_FALSE(bad.has_value()) << op;
    EXPECT_TRUE(contains(bad.error(), "comparison")) << bad.error();
  }
}

TEST(ObjectScript, OrganizedCloudHonoursRowStep) {
  auto result = run(
      "local inputs = ...\nlocal ep = inputs.cloud:extreme_point('x', 'max')\nreturn { index = ep.index, x = ep.x }",
      {object("cloud", makeOrganizedCloud())}, specs({"index:number", "x:number"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "index"), 4.0);  // row 1, col 1, at row_step + point_step
  EXPECT_DOUBLE_EQ(number(*result, "x"), 3.0);
}

TEST(ObjectScript, InvalidLayoutIsAnError) {
  auto cloud = makeCloud({{0, 0, 0, 0}});
  cloud.width = 5;  // claims 80 bytes over 16
  cloud.row_step = cloud.point_step * cloud.width;
  auto result = run("local inputs = ...\nreturn inputs.cloud:bounds().count", {object("cloud", cloud)});
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "smaller than its declared layout")) << result.error();
}

TEST(ObjectScript, BigEndianIsRejected) {
  auto cloud = makeCloud({{0, 0, 0, 0}});
  cloud.is_bigendian = true;
  auto result = run("local inputs = ...\nreturn inputs.cloud:bounds().count", {object("cloud", cloud)});
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "big-endian")) << result.error();
}

TEST(ObjectScript, EmptyCropYieldsZeroCountAndUnavailable) {
  constexpr const char* kScript = R"LUAU(
    local inputs = ...
    local cropped = inputs.cloud:crop_box{min={-5,-5,-5}, max={5,5,5}}
    local top = cropped:extreme_point("z", "max")
    if top == nil then
      return { count = cropped:count(), max_z = pj.unavailable("no finite points in region") }
    end
    return { count = cropped:count(), max_z = top.z }
  )LUAU";
  auto result = run(kScript, {object("cloud", makeCloud({}))}, specs({"count:number", "max_z:number"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "count"), 0.0);
  const auto* max_z = std::get_if<Unavailable>(find(*result, "max_z"));
  ASSERT_NE(max_z, nullptr);
  EXPECT_EQ(max_z->reason, "no finite points in region");
}

// ---- builders: the US1 + US5 reference script (LUA-04), pj.annotations (LUA-05), pj.scene (LUA-06) ----

namespace {

template <class T>
const T* objectOf(const ScriptBundle& bundle, std::string_view name) {
  const ScriptValue* value = find(bundle, name);
  const auto* object = value != nullptr ? std::get_if<sdk::BuiltinObject>(value) : nullptr;
  return object != nullptr ? object->get<T>() : nullptr;
}

// US1 + US5: crop a region, measure its highest point, report it in words and draw a witness there.
constexpr const char* kReferenceScript = R"LUAU(
  local inputs, params = ...
  local cropped = inputs.cloud:crop_box{min={-5,-5,params.zmin}, max={5,5,inputs.threshold}}
  local top = cropped:extreme_point("z", "max")
  if top == nil then
    return { cropped = cropped, max_z = pj.unavailable("no finite points in region"),
             report = "empty", witness = pj.scene.new{frame_id=inputs.cloud.frame_id, id="witness"}:finish() }
  end
  local w = pj.scene.new{frame_id=inputs.cloud.frame_id, id="witness"}
    :sphere{center={top.x, top.y, top.z}, diameter=0.2, color={255,0,0,255}}
    :text{position={top.x, top.y, top.z + 0.3}, text=string.format("%.2f", top.z), font_size=0.2}
  return { cropped = cropped, max_z = top.z,
           report = string.format("%d points; max z %.2f", cropped:count(), top.z), witness = w:finish() }
)LUAU";

Expected<ScriptBundle, ScriptError> runReferenceScript(PointCloud cloud) {
  return run(
      kReferenceScript, {object("cloud", std::move(cloud)), NamedValue{"threshold", ScriptValue{5.0}}},
      specs({"cropped:kPointCloud", "max_z:number", "report:string", "witness:kSceneEntities"}), R"({"zmin": -5})");
}

// Runs `source` (which must fail) and expects its error to name `field`.
void expectScriptErrorNaming(const std::string& source, const char* field) {
  auto result = run(source);
  ASSERT_FALSE(result.has_value()) << source;
  EXPECT_TRUE(contains(result.error(), field)) << source << " -> " << result.error();
}

}  // namespace

TEST(ObjectScript, ReferenceScriptHappyPath) {
  // (10,10,10) falls outside the box and is cropped out; (0,0,0) and (1,1,1) survive.
  auto result = runReferenceScript(makeCloud({{0, 0, 0, 0.1f}, {10, 10, 10, 0.5f}, {1, 1, 1, 0.9f}}));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "max_z"), 1.0);
  EXPECT_EQ(text(*result, "report"), "2 points; max z 1.00");
  const auto* cropped = objectOf<PointCloud>(*result, "cropped");
  ASSERT_NE(cropped, nullptr);
  EXPECT_EQ(cropped->width, 2u);

  // The witness is the snapshot idiom: a clear of everything, then the entity, all unstamped (the store entry
  // takes the result's time).
  const auto* witness = objectOf<sdk::SceneEntities>(*result, "witness");
  ASSERT_NE(witness, nullptr);
  ASSERT_EQ(witness->deletions.size(), 1u);
  EXPECT_EQ(witness->deletions[0].type, sdk::SceneEntityDeletion::Type::kAll);
  EXPECT_EQ(witness->deletions[0].timestamp, 0);
  ASSERT_EQ(witness->entities.size(), 1u);
  const auto& entity = witness->entities[0];
  EXPECT_EQ(entity.frame_id, "base_link");  // the cloud's
  EXPECT_EQ(entity.id, "witness");
  EXPECT_EQ(entity.timestamp, 0);
  ASSERT_EQ(entity.spheres.size(), 1u);
  EXPECT_EQ(entity.spheres[0].pose.position, (sdk::Vector3{1, 1, 1}));
  EXPECT_DOUBLE_EQ(entity.spheres[0].size.x, 0.2);
  EXPECT_EQ(entity.spheres[0].color.r, 255);
  ASSERT_EQ(entity.texts.size(), 1u);
  EXPECT_EQ(entity.texts[0].text, "1.00");
  EXPECT_EQ(entity.texts[0].color, (sdk::ColorRGBA{255, 255, 255, 255}));  // no colour: opaque white
}

// LUA-03 through the reference script: nothing in the region leaves max_z unavailable, with its reason, and
// the witness clears the topic.
TEST(ObjectScript, ReferenceScriptOnAnEmptyRegion) {
  auto result = runReferenceScript(makeCloud({{10, 10, 10, 0.5f}}));
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto* max_z = std::get_if<Unavailable>(find(*result, "max_z"));
  ASSERT_NE(max_z, nullptr);
  EXPECT_EQ(max_z->reason, "no finite points in region");
  EXPECT_EQ(text(*result, "report"), "empty");
  const auto* cropped = objectOf<PointCloud>(*result, "cropped");
  ASSERT_NE(cropped, nullptr);
  EXPECT_EQ(cropped->width, 0u);
  const auto* witness = objectOf<sdk::SceneEntities>(*result, "witness");
  ASSERT_NE(witness, nullptr);
  ASSERT_EQ(witness->deletions.size(), 1u);
  EXPECT_TRUE(witness->entities.empty());
}

// An empty builder is just the clear, not a bare entity; with a primitive, the clear and then the entity.
TEST(ObjectScript, SceneBuilderProducesSnapshotIdiom) {
  auto cleared = run("return pj.scene.new{frame_id='map', id='e'}:finish()");
  ASSERT_TRUE(cleared.has_value()) << cleared.error();
  const auto* clear_only = objectOf<sdk::SceneEntities>(*cleared, "value");
  ASSERT_NE(clear_only, nullptr);
  ASSERT_EQ(clear_only->deletions.size(), 1u);
  EXPECT_TRUE(clear_only->entities.empty());

  auto drawn = run("return pj.scene.new{frame_id='map', id='e'}:sphere{center={0,0,0}, diameter=1}:finish()");
  ASSERT_TRUE(drawn.has_value()) << drawn.error();
  const auto* snapshot = objectOf<sdk::SceneEntities>(*drawn, "value");
  ASSERT_NE(snapshot, nullptr);
  ASSERT_EQ(snapshot->deletions.size(), 1u);
  EXPECT_EQ(snapshot->deletions[0].type, sdk::SceneEntityDeletion::Type::kAll);
  EXPECT_EQ(snapshot->entities.size(), 1u);
}

TEST(ObjectScript, SceneBuilderChainsAllPrimitives) {
  constexpr const char* kScript = R"LUAU(
    return pj.scene.new{frame_id="map", id="e1"}
      :sphere{center={0,0,0}, diameter=1, color={255,0,0}}
      :text{position={1,1,1}, text="hi", font_size=0.5, billboard=true}
      :cube{center={2,2,2}, size={1,2,3}, color={0,255,0,128}}
      :line_strip{points={{0,0,0},{1,0,0},{1,1,0},{1,1,1}}, thickness=0.05}
      :finish()
  )LUAU";
  auto result = run(kScript, {}, specs({"witness:kSceneEntities"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto* scene = objectOf<sdk::SceneEntities>(*result, "witness");
  ASSERT_NE(scene, nullptr);
  ASSERT_EQ(scene->entities.size(), 1u);
  const auto& entity = scene->entities[0];
  ASSERT_EQ(entity.spheres.size(), 1u);
  EXPECT_DOUBLE_EQ(entity.spheres[0].size.x, 1.0);
  EXPECT_EQ(entity.spheres[0].color.a, 255);  // no alpha: opaque
  ASSERT_EQ(entity.texts.size(), 1u);
  EXPECT_TRUE(entity.texts[0].billboard);
  ASSERT_EQ(entity.cubes.size(), 1u);
  EXPECT_EQ(entity.cubes[0].size, (sdk::Vector3{1, 2, 3}));
  EXPECT_EQ(entity.cubes[0].color.a, 128);
  ASSERT_EQ(entity.lines.size(), 1u);
  EXPECT_EQ(entity.lines[0].type, sdk::LineType::kLineStrip);
  EXPECT_EQ(entity.lines[0].points.size(), 4u);
}

// The binder reads the arguments, naming the field it cannot read; the geometry checks are native_ops_test's.
TEST(ObjectScript, SceneBuilderRejectsBadArgs) {
  const std::string head = "local b = pj.scene.new{frame_id='map', id='e1'}\n";
  expectScriptErrorNaming(head + "b:sphere{center={0,0}, diameter=1}", "'center'");
  expectScriptErrorNaming(head + "b:sphere{center={0,0,0}}", "'diameter'");
  expectScriptErrorNaming(head + "b:sphere{center={0,0,0}, diameter=1, color={300,0,0}}", "'color'");
  expectScriptErrorNaming(head + "b:sphere{center={0,0,0}, diameter=1, color={1,2}}", "'color'");
  expectScriptErrorNaming(head + "b:cube{center={0,0,0}, size={1,1}}", "'size'");
  expectScriptErrorNaming(head + "b:text{position={0,0,0}, font_size=1}", "'text'");
  expectScriptErrorNaming(head + "b:line_strip{points={{0,0,0},{1,1}}, thickness=1}", "'points'");
  expectScriptErrorNaming("pj.scene.new{id='e1'}", "'frame_id'");
  expectScriptErrorNaming("pj.scene.new{frame_id='map'}", "'id'");
  EXPECT_FALSE(run("pj.scene.new()").has_value());
  // A native rejection reaches the script as its error.
  expectScriptErrorNaming(head + "b:sphere{center={0/0,0,0}, diameter=1}", "'center'");
}

TEST(ObjectScript, AnnotationsBuilderProducesPrimitives) {
  constexpr const char* kScript = R"LUAU(
    return pj.annotations.new{image_topic="/camera"}
      :points{points={{1,2},{5,2},{5,8},{1,8}}, type="line_loop", outline_color={255,0,0,128}}
      :circle{center={3,4}, diameter=10, fill_color={0,255,0,90}}
      :text{position={1,2}, text="max_z", font_size=20, text_color={3,4,5}}
      :finish()
  )LUAU";
  auto result = run(kScript, {}, specs({"overlay:kImageAnnotations"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto* annotations = objectOf<sdk::ImageAnnotations>(*result, "overlay");
  ASSERT_NE(annotations, nullptr);
  EXPECT_EQ(annotations->image_topic, "/camera");
  // The run knows its inputs by name, not by topic, so it cannot find the annotated image's stamp: the
  // annotations stay unstamped and the store entry takes the result's time, the newest input's.
  EXPECT_EQ(annotations->timestamp, 0);
  ASSERT_EQ(annotations->points.size(), 1u);
  EXPECT_EQ(annotations->points[0].topology, sdk::AnnotationTopology::kLineLoop);
  EXPECT_EQ(annotations->points[0].points.size(), 4u);
  EXPECT_EQ(annotations->points[0].color, (sdk::ColorRGBA{255, 0, 0, 128}));
  EXPECT_EQ(annotations->points[0].fill_color.a, 0);  // no fill by default
  EXPECT_DOUBLE_EQ(annotations->points[0].thickness, 2.0);
  ASSERT_EQ(annotations->circles.size(), 1u);
  EXPECT_DOUBLE_EQ(annotations->circles[0].radius, 5.0);  // the script gives a diameter
  EXPECT_EQ(annotations->circles[0].color, (sdk::ColorRGBA{0, 255, 0, 255}));
  EXPECT_EQ(annotations->circles[0].fill_color.a, 90);
  ASSERT_EQ(annotations->texts.size(), 1u);
  EXPECT_EQ(annotations->texts[0].text, "max_z");
  EXPECT_EQ(annotations->texts[0].color, (sdk::ColorRGBA{3, 4, 5, 255}));
  EXPECT_DOUBLE_EQ(annotations->texts[0].font_size, 20.0);
}

TEST(ObjectScript, AnnotationsRequireImageTopic) {
  expectScriptErrorNaming("return pj.annotations.new{}:finish()", "'image_topic'");
  expectScriptErrorNaming("return pj.annotations.new{image_topic=''}:finish()", "'image_topic'");
}

TEST(ObjectScript, EmptyAnnotationsFinishToNoPrimitives) {
  auto result = run("return pj.annotations.new{image_topic='/cam'}:finish()", {}, specs({"a:kImageAnnotations"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto* annotations = objectOf<sdk::ImageAnnotations>(*result, "a");
  ASSERT_NE(annotations, nullptr);
  EXPECT_TRUE(annotations->empty());
  EXPECT_EQ(annotations->image_topic, "/cam");
}

// The binder reads the arguments and refuses an opaque text background; the geometry checks are native_ops_test's.
TEST(ObjectScript, AnnotationsRejectInvalidGeometryAndUnsupportedBackground) {
  const std::string head = "local a = pj.annotations.new{image_topic='/cam'}\n";
  expectScriptErrorNaming(head + "a:points{points={{1,2},{3}}, type='line_strip'}", "'points'");
  expectScriptErrorNaming(head + "a:points{points={{1,2}}}", "'type'");
  expectScriptErrorNaming(head + "a:circle{center={0,1}, diameter=1, outline_color={256,0,0}}", "'outline_color'");
  expectScriptErrorNaming(head + "a:text{position={0,1}}", "'text'");
  expectScriptErrorNaming(
      head + "a:text{position={0,1}, text='x', background_color={0,0,0,255}}", "'background_color'");
  EXPECT_TRUE(run(head + "a:text{position={0,1}, text='x', background_color={0,0,0,0}}\nreturn 1").has_value());
  // A native rejection reaches the script as its error.
  expectScriptErrorNaming(head + "a:circle{center={0,1}, diameter=-1}", "'diameter'");
}

TEST(ObjectScript, BuilderOutputsAreInferredByType) {
  auto result =
      run("return { pj.scene.new{frame_id='map', id='e'}:finish(), pj.annotations.new{image_topic='/c'}:finish() }");
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(describe(*result), "scene:kSceneEntities,annotations:kImageAnnotations");
}

// ---- builders: hostile scripts and budgets -----------------------------------------------------------

// A builder method called on another kind of value raises an error; it never dereferences it.
TEST(ObjectScript, BuilderMethodsRejectForeignValues) {
  const std::string head =
      "local inputs = ...\nlocal scene = pj.scene.new{frame_id='map', id='e'}\n"
      "local notes = pj.annotations.new{image_topic='/c'}\n";
  const std::vector<NamedValue> inputs{object("cloud", makeCloud({{0, 0, 0, 0}}))};
  ASSERT_TRUE(run(head + "return 1", inputs).has_value());  // the builders work
  for (const char* call :
       {"scene.sphere(nil, {center={0,0,0}, diameter=1})", "scene.sphere(notes, {center={0,0,0}, diameter=1})",
        "scene.sphere(inputs.cloud, {center={0,0,0}, diameter=1})", "scene.sphere({}, {center={0,0,0}, diameter=1})",
        "scene.finish(notes)", "notes.finish(scene)", "notes.circle(scene, {center={0,0}, diameter=1})",
        "notes.finish(inputs.cloud)"}) {
    EXPECT_FALSE(run(head + "return " + call, inputs).has_value()) << call;
  }
}

TEST(ObjectScript, BuilderMetatableIsUnreachable) {
  const std::string head =
      "local scene = pj.scene.new{frame_id='map', id='e'}\nlocal notes = pj.annotations.new{image_topic='/c'}\n";
  auto locked = run(head + "return { scene = getmetatable(scene), notes = getmetatable(notes) }");
  ASSERT_TRUE(locked.has_value()) << locked.error();  // the builders work, and their metatables are locked
  EXPECT_EQ(text(*locked, "scene"), "locked");
  EXPECT_EQ(text(*locked, "notes"), "locked");
  EXPECT_FALSE(run(head + "return getmetatable(scene).__index.sphere(0, {center={0,0,0}, diameter=1})").has_value());
  EXPECT_FALSE(run(head + "setmetatable(scene, {})\nreturn 1").has_value());
  EXPECT_FALSE(run(head + "setmetatable(notes, {})\nreturn 1").has_value());
  EXPECT_FALSE(run(head + "scene.sphere = nil\nreturn 1").has_value());
}

// No script code runs inside a builder call: an argument table's metamethods are not consulted, so they cannot
// finish the builder the call is using.
TEST(ObjectScript, ArgumentMetamethodsCannotFinishABuilderInUse) {
  const std::string head =
      "local ran = 0\nlocal scene = pj.scene.new{frame_id='map', id='e'}\n"
      "local notes = pj.annotations.new{image_topic='/c'}\n"
      "local function trap(fields, builder) return setmetatable(fields, "
      "{__index = function() ran = 1; builder:finish() end}) end\n";
  for (const char* call :
       {"scene:sphere(trap({center={0,0,0}, diameter=1}, scene))",
        "scene:text(trap({position={0,0,0}, text='x', font_size=1}, scene))",
        "scene:line_strip(trap({points={{0,0,0},{1,1,1}}, thickness=1}, scene))",
        "notes:points(trap({points={{0,0}}, type='points'}, notes))",
        "notes:circle(trap({center={0,0}, diameter=1}, notes))", "notes:text(trap({position={0,0}, text='x'}, notes))",
        "scene:sphere({center=setmetatable({0,0,0}, {__index = function() ran = 1; scene:finish() end}), "
        "diameter=1})"}) {
    auto result = run(head + call + "\nreturn ran");
    ASSERT_TRUE(result.has_value()) << call << " -> " << result.error();
    EXPECT_DOUBLE_EQ(number(*result, "value"), 0.0) << call;
  }
}

TEST(ObjectScript, AFinishedBuilderCannotBeUsedAgain) {
  const std::string head = "local scene = pj.scene.new{frame_id='map', id='e'}\nscene:finish()\n";
  expectScriptErrorNaming(head + "scene:sphere{center={0,0,0}, diameter=1}", "finished");
  expectScriptErrorNaming(head + "scene:finish()", "finished");
  expectScriptErrorNaming(
      "local notes = pj.annotations.new{image_topic='/c'}\nnotes:finish()\nnotes:finish()", "finished");
}

// Strings a script hands to a builder are charged to the native budget, like the strings it returns.
TEST(ObjectScript, BuilderStringsAreChargedToTheNativeBudget) {
  for (const char* source :
       {"pj.scene.new{frame_id='map', id='e'}:text{position={0,0,0}, text=string.rep('x', 2097152), font_size=1}",
        "pj.scene.new{frame_id=string.rep('f', 2097152), id='e'}",
        "pj.scene.new{frame_id='map', id=string.rep('i', 2097152)}",
        "pj.annotations.new{image_topic='/c'}:text{position={0,0}, text=string.rep('x', 2097152)}",
        "pj.annotations.new{image_topic=string.rep('t', 2097152)}"}) {
    auto result = run(std::string(source) + "\nreturn 1", {}, {}, "{}", withNativeBudget(std::size_t{1} << 20));
    ASSERT_FALSE(result.has_value()) << source;
    EXPECT_TRUE(contains(result.error(), "native memory limit")) << source << " -> " << result.error();
  }
}

TEST(ObjectScript, ABuilderGrownInALoopStopsAtTheNativeBudget) {
  auto result =
      run("local b = pj.scene.new{frame_id='map', id='e'}\n"
          "for i = 1, 1000000 do b:sphere{center={0,0,0}, diameter=1} end\nreturn 1",
          {}, {}, "{}", withNativeBudget(std::size_t{64} << 10));
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "native memory limit")) << result.error();
}

// A finished scene keeps its charge for as long as the script holds it, however soon its builder dies.
TEST(ObjectScript, RetainedBuilderOutputsKeepCounting) {
  auto result =
      run("local kept = {}\nfor i = 1, 200 do\n"
          "  kept[i] = pj.scene.new{frame_id='map', id='e'}:text{position={0,0,0}, text=string.rep('x', 16384), "
          "font_size=1}:finish()\nend\nreturn #kept",
          {}, {}, "{}", withNativeBudget(std::size_t{1} << 20));
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "native memory limit")) << result.error();
}

// A view into a finished scene keeps its charge too, after the box that was the scene's root is collected:
// 100 retained texts of about 8.5 KiB leave a 1 MiB budget no room for a crop of 20,000 points, and the full
// collection the failed crop triggers must not free what the views still hold.
TEST(ObjectScript, ViewsIntoABuilderOutputKeepItsCharge) {
  auto result =
      run("local inputs = ...\nlocal kept = {}\nfor i = 1, 100 do\n"
          "  kept[i] = pj.scene.new{frame_id='map', id='e'}:text{position={0,0,0}, text=string.rep('x', 8192), "
          "font_size=1}:finish().entities[1].texts\nend\n"
          "return inputs.cloud:crop_box{min={-1,-1,-1}, max={100000,1,1}}:count() + #kept",
          {object("cloud", makeLineCloud(20000))}, {}, "{}", withNativeBudget(std::size_t{1} << 20));
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "native memory limit")) << result.error();
}

TEST(ObjectScript, XyzPointsIteratorSkipsNaN) {
  auto cloud = makeCloud({{1, 0, 0, 0}, {kNaN, 1, 1, 0}, {3, 0, 0, 0}, {kNaN, kNaN, kNaN, 0}});
  constexpr const char* kScript = R"LUAU(
    local inputs = ...
    local n, sumx, last = 0, 0, 0
    for i, x, y, z in inputs.cloud:xyz_points() do
      n = n + 1
      sumx = sumx + x
      last = i
    end
    return { n = n, sumx = sumx, last = last }
  )LUAU";
  auto result = run(kScript, {object("cloud", cloud)}, specs({"n:number", "sumx:number", "last:number"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "n"), 2.0);
  EXPECT_DOUBLE_EQ(number(*result, "sumx"), 4.0);
  EXPECT_DOUBLE_EQ(number(*result, "last"), 3.0);  // 1-based, NaN records keep their index
}

TEST(ObjectScript, CloudTransformAppliesRigidMotion) {
  auto cloud = makeCloud({{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}});
  cloud.frame_id = "lidar";
  constexpr double kHalfSqrt2 = 0.70710678118654752440;
  pj::tf::TransformBuffer buffer(pj::tf::TransformBuffer::kKeepAll);
  addTf(buffer, "map", "lidar", kSecond, kSecond, {10, 0, 0}, glm::dquat(kHalfSqrt2, 0, 0, kHalfSqrt2));  // yaw 90
  constexpr const char* kScript = R"LUAU(
    local inputs = ...
    local tf = assert(pj.tf.lookup("map", inputs.cloud.frame_id))
    local moved = inputs.cloud:transform(tf)
    local pts = {}
    for i, x, y, z in moved:xyz_points() do pts[i] = {x, y, z} end
    return { frame = moved.frame_id, x1 = pts[1][1], y1 = pts[1][2], x2 = pts[2][1], y2 = pts[2][2], z3 = pts[3][3] }
  )LUAU";
  auto result =
      run(kScript, {object("cloud", cloud)},
          specs({"frame:string", "x1:number", "y1:number", "x2:number", "y2:number", "z3:number"}), "{}",
          hostAt(2 * kSecond, buffer.snapshot()));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(text(*result, "frame"), "map");
  EXPECT_NEAR(number(*result, "x1"), 10.0, 1e-6);  // (1,0,0) -> (0,1,0) + (10,0,0)
  EXPECT_NEAR(number(*result, "y1"), 1.0, 1e-6);
  EXPECT_NEAR(number(*result, "x2"), 9.0, 1e-6);  // (0,1,0) -> (-1,0,0) + (10,0,0)
  EXPECT_NEAR(number(*result, "y2"), 0.0, 1e-6);
  EXPECT_NEAR(number(*result, "z3"), 1.0, 1e-6);  // on the axis
}

// ---- native budget (BUD-15) and the garbage-collection retry ------------------------------------

// The default budget fits the transform of a 128-beam lidar scan (128 x 2048 records of 48 bytes, about
// 12.6 MB), which an 8 MB budget does not.
TEST(ObjectScript, DefaultNativeBudgetTransformsA128BeamScan) {
  const auto scan = makeLineCloud(128 * 2048, 48);
  constexpr const char* kScript =
      "local inputs = ...\nreturn inputs.cloud:transform{translation={0, 0, 1}, rotation={0, 0, 0, 1}}:count()";
  auto fits = run(kScript, {object("cloud", scan)});
  ASSERT_TRUE(fits.has_value()) << fits.error();
  EXPECT_DOUBLE_EQ(number(*fits, "value"), 128.0 * 2048.0);
  auto too_small = run(kScript, {object("cloud", scan)}, {}, "{}", withNativeBudget(std::size_t{8} << 20));
  ASSERT_FALSE(too_small.has_value());
  EXPECT_TRUE(contains(too_small.error(), "native memory limit")) << too_small.error();
}

TEST(ObjectScript, AggregateNativeCloudBudgetRejectsRetainedSecondOutput) {
  const auto cloud = makeLineCloud(100'000);  // 1.6 MB
  constexpr std::size_t kBudget = 2'500'000;  // one crop of everything fits, two do not
  auto single = run(
      "local inputs = ...\nlocal a = inputs.cloud:crop_box{min={-1e9,-1e9,-1e9}, max={1e9,1e9,1e9}}\nreturn a:count()",
      {object("cloud", cloud)}, {}, "{}", withNativeBudget(kBudget));
  ASSERT_TRUE(single.has_value()) << single.error();
  auto retained =
      run("local inputs = ...\nlocal box = {min={-1e9,-1e9,-1e9}, max={1e9,1e9,1e9}}\n"
          "local a = inputs.cloud:crop_box(box)\nlocal b = inputs.cloud:crop_box(box)\nreturn a:count() + b:count()",
          {object("cloud", cloud)}, {}, "{}", withNativeBudget(kBudget));
  ASSERT_FALSE(retained.has_value());
  EXPECT_TRUE(contains(retained.error(), "native memory limit")) << retained.error();
}

// The collector cannot see native bytes: a dead crop still holds its charge until it is collected. A
// native operation that fails on the budget runs one full collection and is retried once.
TEST(ObjectScript, ANativeOpRetriesOnceAfterAGarbageCollection) {
  const auto cloud = makeLineCloud(100'000);
  constexpr const char* kScript = R"LUAU(
    local inputs = ...
    local n = 0
    for i = 1, 3 do
      n = n + inputs.cloud:crop_box{min={-1e9,-1e9,-1e9}, max={1e9,1e9,1e9}}:count()
    end
    return n
  )LUAU";
  auto result = run(kScript, {object("cloud", cloud)}, {}, "{}", withNativeBudget(2'500'000));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "value"), 300'000.0);
}

// ---- pj.tf (TFL-12, GAP-29) -----------------------------------------------------------------

TEST(ObjectScript, TfLookupReturnsTransformOrReason) {
  const RunOptions host = hostAt(2 * kSecond);
  auto found = run(
      R"LUAU(
        local tf = pj.tf.lookup("map", "base_link")
        return { tx = tf.translation[1], ty = tf.translation[2], tz = tf.translation[3], rw = tf.rotation[4],
                 target = tf.target_frame }
      )LUAU",
      {}, specs({"tx:number", "ty:number", "tz:number", "rw:number", "target:string"}), "{}", host);
  ASSERT_TRUE(found.has_value()) << found.error();
  EXPECT_DOUBLE_EQ(number(*found, "tx"), 1.0);
  EXPECT_DOUBLE_EQ(number(*found, "ty"), 2.0);
  EXPECT_DOUBLE_EQ(number(*found, "tz"), 3.0);
  EXPECT_DOUBLE_EQ(number(*found, "rw"), 1.0);
  EXPECT_EQ(text(*found, "target"), "map");

  auto missing =
      run("local tf, why = pj.tf.lookup('map', 'unknown_frame')\nreturn { ok = tf == nil and 1 or 0, why = why }", {},
          specs({"ok:number", "why:string"}), "{}", host);
  ASSERT_TRUE(missing.has_value()) << missing.error();
  EXPECT_DOUBLE_EQ(number(*missing, "ok"), 1.0);
  EXPECT_EQ(text(*missing, "why"), "unknown source frame");
}

TEST(ObjectScript, TfNowIsTheRequestedInstant) {
  constexpr Timestamp kT = 1'234'567'890'123'456'789LL;  // exact past 2^53
  auto result = run("return { now = tostring(pj.tf.now) }", {}, specs({"now:string"}), "{}", hostAt(kT, nullptr));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(text(*result, "now"), std::to_string(kT));
}

TEST(ObjectScript, NoSnapshotYieldsReason) {
  constexpr const char* kScript = R"LUAU(
    local tf, why = pj.tf.lookup("map", "base_link")
    return { ok = tf == nil and 1 or 0, why = why, nframes = #pj.tf.frames(), now = tostring(pj.tf.now) }
  )LUAU";
  auto result = run(kScript, {}, specs({"ok:number", "why:string", "nframes:number", "now:string"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "ok"), 1.0);
  EXPECT_EQ(text(*result, "why"), "no tf snapshot");
  EXPECT_DOUBLE_EQ(number(*result, "nframes"), 0.0);
  EXPECT_EQ(text(*result, "now"), "0");
}

TEST(ObjectScript, TfFramesListsTheSnapshot) {
  auto result =
      run("local f = pj.tf.frames()\ntable.sort(f)\nreturn { frames = table.concat(f, ',') }", {},
          specs({"frames:string"}), "{}", hostAt(0));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(text(*result, "frames"), "base_link,map");
}

// GAP-29: a lookup with no time reads the entry clock at the query instant; a lookup at an object reads
// the header clock at the object's own stamp, on the dataset clock: a merge's payload_stamp_shift moved
// the TF header stamps at ingest, and the engine moves the object's stamp by the same shift.
TEST(ObjectScript, TfLookupUsesTheInputsTimeBase) {
  constexpr int64_t kShift = 1000 * kSecond;  // the merge moved this dataset's clock by 1000 s
  pj::tf::TransformBuffer buffer(pj::tf::TransformBuffer::kKeepAll);
  // On the dataset clock, as TransformService ingests them: header stamps already shifted, logged later.
  addTf(buffer, "map", "lidar", 1100 * kSecond, 1300 * kSecond, {1, 0, 0});
  addTf(buffer, "map", "lidar", 1200 * kSecond, 1310 * kSecond, {2, 0, 0});
  auto cloud = makeCloud({{0, 0, 0, 0}});
  cloud.frame_id = "lidar";
  cloud.timestamp_ns = 100 * kSecond;  // the payload's own stamp, before the merge

  constexpr const char* kScript = R"LUAU(
    local inputs = ...
    local placed, why = pj.tf.lookup("map", "lidar", inputs.cloud)
    local queried = pj.tf.lookup("map", "lidar")
    return { stamp_s = inputs.cloud.timestamp_ns:to_number() / 1e9, placed_x = placed and placed.translation[1] or -1,
             queried_x = queried.translation[1] }
  )LUAU";
  auto result =
      run(kScript, {object("cloud", cloud, kShift)}, specs({"stamp_s:number", "placed_x:number", "queried_x:number"}),
          "{}", hostAt(1305 * kSecond, buffer.snapshot()));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "stamp_s"), 1100.0);  // the script reads the dataset clock
  EXPECT_DOUBLE_EQ(number(*result, "placed_x"), 1.0);    // header clock at 1100 s
  // Entry clock at 1305 s: only the first sample had arrived (a header-clock read would give 2).
  EXPECT_DOUBLE_EQ(number(*result, "queried_x"), 1.0);
}

// ---- observation flags (CACHE-07, CACHE-08) ------------------------------------------------------

namespace {

ScriptBundle runObserving(const char* script, const RunOptions& host) {
  auto result = run(script, {}, {}, "{}", host);
  EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
  return result ? *result : ScriptBundle{};
}

}  // namespace

TEST(ObjectScript, ReadingTfNowMarksTheClockObserved) {
  const ScriptBundle bundle = runObserving("return pj.tf.now:to_number()", hostAt(42));
  EXPECT_DOUBLE_EQ(number(bundle, "value"), 42.0);
  EXPECT_TRUE(bundle.observed_query_clock);
}

TEST(ObjectScript, LookupWithoutTimeMarksTheClockObserved) {
  const ScriptBundle bundle =
      runObserving("return pj.tf.lookup('map', 'base_link') ~= nil and 1 or 0", hostAt(2 * kSecond));
  EXPECT_DOUBLE_EQ(number(bundle, "value"), 1.0);
  EXPECT_TRUE(bundle.observed_query_clock);
}

TEST(ObjectScript, LookupWithExplicitTimeDoesNotMarkIt) {
  const ScriptBundle bundle =
      runObserving("return pj.tf.lookup('map', 'base_link', 2e9) ~= nil and 1 or 0", hostAt(5 * kSecond));
  EXPECT_DOUBLE_EQ(number(bundle, "value"), 1.0);
  EXPECT_FALSE(bundle.observed_query_clock);
  EXPECT_TRUE(bundle.observed_tf);
}

TEST(ObjectScript, ScriptWithoutTfLeavesTheFlagFalse) {
  const ScriptBundle bundle = runObserving("return 1", hostAt(42));
  EXPECT_FALSE(bundle.observed_query_clock);
  EXPECT_FALSE(bundle.observed_tf);
}

TEST(ObjectScript, ObservationInsideABranchIsRecordedOnlyWhenTaken) {
  constexpr const char* kScript = R"LUAU(
    local inputs = ...
    if inputs.x:count() > 2 then
      return pj.tf.now:to_number()
    end
    return 0
  )LUAU";
  auto one_point = run(kScript, {object("x", makeLineCloud(1))}, {}, "{}", hostAt(99));
  ASSERT_TRUE(one_point.has_value()) << one_point.error();
  EXPECT_DOUBLE_EQ(number(*one_point, "value"), 0.0);
  EXPECT_FALSE(one_point->observed_query_clock);
  auto three_points = run(kScript, {object("x", makeLineCloud(3))}, {}, "{}", hostAt(99));
  ASSERT_TRUE(three_points.has_value()) << three_points.error();
  EXPECT_DOUBLE_EQ(number(*three_points, "value"), 99.0);
  EXPECT_TRUE(three_points->observed_query_clock);
}

TEST(ObjectScript, TfLookupMarksTheTfObserved) {
  EXPECT_TRUE(runObserving("pj.tf.lookup('map', 'base_link', 2e9) return 1", hostAt(0)).observed_tf);
  EXPECT_TRUE(runObserving("pj.tf.lookup('map', 'base_link') return 1", hostAt(0)).observed_tf);
}

TEST(ObjectScript, TfFramesMarksTheTfObserved) {
  EXPECT_TRUE(runObserving("return #pj.tf.frames()", hostAt(0)).observed_tf);
}

TEST(ObjectScript, QueryingAnEmptyTreeStillObservesIt) {
  EXPECT_TRUE(runObserving("pj.tf.lookup('map', 'base_link', 2e9) return 1", hostAt(0, nullptr)).observed_tf);
  EXPECT_TRUE(runObserving("return #pj.tf.frames()", hostAt(0, nullptr)).observed_tf);
}

TEST(ObjectScript, ClockAndPlainScriptsDoNotObserveTheTf) {
  EXPECT_FALSE(runObserving("return pj.tf.now:to_number()", hostAt(0)).observed_tf);
  EXPECT_FALSE(runObserving("return 1", hostAt(0)).observed_tf);
}

// ---- sandbox and limits (LUA-09, LUA-14, BUD-13, BUD-14, GAP-27, GAP-38) ---------------------------

TEST(ObjectScript, GlobalsDoNotLeakBetweenRuns) {
  constexpr const char* kScript = R"LUAU(
    if x == nil then x = 1 else x = x + 1 end
    pcall(function() getmetatable("").__index.leaked = true end)
    return { seen = x, leaked = (string.leaked ~= nil) and 1 or 0 }
  )LUAU";
  auto compiled = compileOnDemandScript(kScript);
  ASSERT_TRUE(compiled.has_value()) << compiled.error();
  for (int call = 0; call < 2; ++call) {
    auto result = runOnDemandScript(*compiled, {}, "{}", {});
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_DOUBLE_EQ(number(*result, "seen"), 1.0);  // a fresh VM each call
    EXPECT_DOUBLE_EQ(number(*result, "leaked"), 0.0);
  }
}

TEST(ObjectScript, SandboxRandomIsDeterministicAcrossRuns) {
  constexpr const char* kScript = "return math.random() * 1e6 + math.random(1, 1000)";
  auto first = run(kScript);
  auto second = run(kScript);
  ASSERT_TRUE(first.has_value()) << first.error();
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_DOUBLE_EQ(number(*first, "value"), number(*second, "value"));
}

TEST(ObjectScript, SandboxExposesNoClock) {
  auto result = run("return (os ~= nil) and 1 or 0");
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "value"), 0.0);
}

TEST(ObjectScript, FuelTripsOnHostileLoop) {
  RunOptions options;
  options.limits.setup_fuel = 10'000;
  auto result = run("while true do end\nreturn 1", {}, {}, "{}", options);
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "instruction budget")) << result.error();
}

TEST(ObjectScript, OutOfMemoryNamesTheScriptLimit) {
  RunOptions options;
  options.limits.mem_bytes = std::size_t{8} << 20;
  auto result = run("local t = {}\nfor i = 1, 50000000 do t[i] = i end\nreturn 1", {}, {}, "{}", options);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().message, "script memory limit (8 MB) exceeded");
}

// GAP-27: the interpreter polls the cancel check, and a script's pcall cannot swallow the cancellation.
TEST(ObjectScript, LuauLoopHonoursTheCancelToken) {
  int polls = 0;
  RunOptions options;
  options.cancelled = [&polls] { return ++polls > 1000; };
  auto result = run("pcall(function() while true do end end)\nreturn 1", {}, {}, "{}", options);
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "cancelled")) << result.error();
}

// ---- hostile scripts and edge values (review round) ------------------------------------------------

// A binder function called on a value of another kind raises an error; it never dereferences it.
TEST(ObjectScript, PointIteratorRejectsAForeignState) {
  auto result =
      run("local inputs = ...\nlocal next_point = inputs.cloud:xyz_points()\nreturn next_point(nil)",
          {object("cloud", makeCloud({{0, 0, 0, 0}}))});
  ASSERT_FALSE(result.has_value());
}

// The binder's metatables are out of a script's reach, so their functions cannot be called on other values.
TEST(ObjectScript, ObjectMetatableIsUnreachable) {
  auto result =
      run("local inputs = ...\nreturn getmetatable(inputs.cloud).__index(0, 'frame_id')",
          {object("cloud", makeCloud({{0, 0, 0, 0}}))});
  ASSERT_FALSE(result.has_value());
}

TEST(ObjectScript, ListIteratorRejectsAStructView) {
  auto result =
      run("local inputs = ...\nlocal list = inputs.cloud.fields\nlocal next_element = getmetatable(list).__iter(list)\n"
          "return next_element(list[1], 0)",
          {object("cloud", makeCloud({{0, 0, 0, 0}}))});
  ASSERT_FALSE(result.has_value());
}

// Strings leaving the VM are charged to the native budget: one interned 1 MiB string returned 100 times.
TEST(ObjectScript, OutputStringsAreChargedToTheNativeBudget) {
  auto result = run("return table.create(100, string.rep('x', 1048576))");
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "native memory limit")) << result.error();
}

TEST(ObjectScript, TransformChargesItsFrameName) {
  auto result =
      run("local inputs = ...\nreturn inputs.cloud:transform{translation={0, 0, 0}, rotation={0, 0, 0, 1}, "
          "target_frame=string.rep('f', 2097152)}:count()",
          {object("cloud", makeCloud({{0, 0, 0, 0}}))}, {}, "{}", withNativeBudget(std::size_t{1} << 20));
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "native memory limit")) << result.error();
}

TEST(ObjectScript, UnavailableReasonIsCutTo1KiB) {
  auto result = run("return { why = pj.unavailable(string.rep('x', 5000)) }", {}, specs({"why:string"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto* why = std::get_if<Unavailable>(find(*result, "why"));
  ASSERT_NE(why, nullptr);
  EXPECT_EQ(why->reason.size(), 1024u);
}

// An unavailable marker keeps its reason inside the VM, under its memory cap: 20,000 retained markers with
// distinct 1 KB reasons exceed an 8 MiB cap.
TEST(ObjectScript, UnavailableMarkersStayUnderTheVmCap) {
  RunOptions options;
  options.limits.mem_bytes = std::size_t{8} << 20;
  auto result =
      run("local keep = {}\nfor i = 1, 20000 do keep[i] = pj.unavailable(tostring(i) .. string.rep('x', 1000)) end\n"
          "return #keep",
          {}, {}, "{}", options);
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "script memory limit")) << result.error();
}

// Params nested deeper than 32 levels are refused before the VM exists (the push would overflow its stack).
TEST(ObjectScript, DeeplyNestedParamsAreAnError) {
  std::string params;
  for (int level = 0; level < 256; ++level) {
    params += "{\"a\":";
  }
  params += "1" + std::string(256, '}');
  auto result = run("return 1", {}, {}, params);
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "nested")) << result.error();
}

// A cancellation a native operation observes fails the run even when the script catches the error.
TEST(ObjectScript, NativeCancellationCannotBeSwallowed) {
  const auto cloud = makeLineCloud(2'000'000);  // the crop polls every 65536 records
  int polls = 0;
  RunOptions options;
  options.cancelled = [&polls] { return ++polls == 10; };  // seen once, inside the crop
  auto result =
      run("local inputs = ...\nlocal ok = pcall(function()\n"
          "  return inputs.cloud:crop_box{min={-1e9,-1e9,-1e9}, max={1e9,1e9,1e9}}\nend)\nreturn ok and 1 or 0",
          {object("cloud", cloud)}, {}, "{}", options);
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "cancelled")) << result.error();
}

TEST(ObjectScript, Int64DifferenceDoesNotOverflow) {
  auto high = makeCloud({});
  high.timestamp_ns = std::numeric_limits<int64_t>::max();
  auto low = makeCloud({});
  low.timestamp_ns = std::numeric_limits<int64_t>::min();
  auto result =
      run("local inputs = ...\nreturn inputs.high.timestamp_ns:diff_s(inputs.low.timestamp_ns)",
          {object("high", high), object("low", low)});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(
      number(*result, "value"), (static_cast<double>(std::numeric_limits<int64_t>::max()) -
                                 static_cast<double>(std::numeric_limits<int64_t>::min())) /
                                    1e9);
}

// A shifted stamp saturates at the int64 limits, as the store's own time shifts do.
TEST(ObjectScript, ShiftedStampSaturates) {
  auto late = makeCloud({});
  late.timestamp_ns = std::numeric_limits<int64_t>::max() - 5;
  auto early = makeCloud({});
  early.timestamp_ns = std::numeric_limits<int64_t>::min() + 5;
  auto result =
      run("local inputs = ...\nreturn { late = tostring(inputs.late.timestamp_ns), early = "
          "tostring(inputs.early.timestamp_ns) }",
          {object("late", late, 10), object("early", early, -10)}, specs({"late:string", "early:string"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(text(*result, "late"), std::to_string(std::numeric_limits<int64_t>::max()));
  EXPECT_EQ(text(*result, "early"), std::to_string(std::numeric_limits<int64_t>::min()));
}

// Adjacent stamps above 2^53 differ by exactly 1 ns, and separately boxed values compare by value.
TEST(ObjectScript, Int64ValuesCompareAndSubtractExactly) {
  auto first = makeCloud({});
  first.timestamp_ns = 9'007'199'254'740'993LL;  // 2^53 + 1
  auto second = makeCloud({});
  second.timestamp_ns = 9'007'199'254'740'994LL;
  constexpr const char* kScript = R"LUAU(
    local inputs = ...
    local a, b = inputs.first, inputs.second
    return { diff = b.timestamp_ns:diff_s(a.timestamp_ns),
             eq = (a.timestamp_ns == a.timestamp_ns) and 1 or 0, ne = (a.timestamp_ns ~= b.timestamp_ns) and 1 or 0,
             lt = (a.timestamp_ns < b.timestamp_ns) and 1 or 0, le = (b.timestamp_ns <= b.timestamp_ns) and 1 or 0,
             gt = (a.timestamp_ns > b.timestamp_ns) and 1 or 0 }
  )LUAU";
  auto result =
      run(kScript, {object("first", first), object("second", second)},
          specs({"diff:number", "eq:number", "ne:number", "lt:number", "le:number", "gt:number"}));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_DOUBLE_EQ(number(*result, "diff"), 1e-9);
  EXPECT_DOUBLE_EQ(number(*result, "eq"), 1.0);
  EXPECT_DOUBLE_EQ(number(*result, "ne"), 1.0);
  EXPECT_DOUBLE_EQ(number(*result, "lt"), 1.0);
  EXPECT_DOUBLE_EQ(number(*result, "le"), 1.0);
  EXPECT_DOUBLE_EQ(number(*result, "gt"), 0.0);
}

// A failed run reports what it observed: a failure after reading TF may not repeat at another revision.
TEST(ObjectScript, AFailedRunReportsWhatItObserved) {
  auto result = run("pj.tf.lookup('map', 'base_link')\nerror('boom')", {}, {}, "{}", hostAt(2 * kSecond));
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "boom")) << result.error();
  EXPECT_TRUE(result.error().observed_tf);
  EXPECT_TRUE(result.error().observed_query_clock);
  auto plain = run("error('boom')", {}, {}, "{}", hostAt(2 * kSecond));
  ASSERT_FALSE(plain.has_value());
  EXPECT_FALSE(plain.error().observed_tf);
}

// Positional names would shift with availability, so an inferred array cannot hold pj.unavailable.
TEST(ObjectScriptInferred, ArrayCannotHoldUnavailable) {
  auto result = run("return { 1, pj.unavailable('no points') }");
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "name the output")) << result.error();
}

// D25: the caller does not run a node whose input is unavailable.
TEST(ObjectScript, UnavailableInputIsAnError) {
  auto result = run("return 1", {NamedValue{"threshold", ScriptValue{Unavailable{"no sample"}}}});
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "'threshold'")) << result.error();
  EXPECT_TRUE(contains(result.error(), "unavailable")) << result.error();
}

// ---- measurements ----------------------------------------------------------------------------------

// PJ_BENCH=1 prints the per-call cost of a fresh VM over compiled bytecode, and of iterating a
// 100,000-element list field.
TEST(ObjectScriptBench, SetupAndListIteration) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  const auto micros = [](auto begin, auto end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
  };
  {
    auto compiled = compileOnDemandScript("local inputs, params = ...\nreturn { value = 1 }");
    ASSERT_TRUE(compiled.has_value());
    constexpr int kCalls = 2000;
    const auto begin = std::chrono::steady_clock::now();
    for (int call = 0; call < kCalls; ++call) {
      ASSERT_TRUE(runOnDemandScript(*compiled, {}, "{}", {}).has_value());
    }
    std::printf(
        "per-call setup (fresh VM, compiled bytecode): %.1f us\n",
        micros(begin, std::chrono::steady_clock::now()) / kCalls);
  }
  sdk::FrameTransforms transforms;
  transforms.transforms.resize(100'000);
  for (size_t index = 0; index < transforms.transforms.size(); ++index) {
    transforms.transforms[index].translation.x = 1.0;
  }
  const std::vector<NamedValue> inputs{object("tf", sdk::BuiltinObject(std::move(transforms)))};
  for (const char* script :
       {"local inputs = ...\nlocal list, s = inputs.tf.transforms, 0\n"
        "for i = 1, #list do s = s + list[i].translation.x end\nreturn s",
        "local inputs = ...\nlocal s = 0\n"
        "for i = 1, #inputs.tf.transforms do s = s + inputs.tf.transforms[i].translation.x end\nreturn s"}) {
    auto compiled = compileOnDemandScript(script);
    ASSERT_TRUE(compiled.has_value());
    const auto begin = std::chrono::steady_clock::now();
    auto result = runOnDemandScript(*compiled, inputs, "{}", {});
    const double elapsed = micros(begin, std::chrono::steady_clock::now());
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_DOUBLE_EQ(number(*result, "value"), 100'000.0);
    std::printf(
        "100k-element list iteration: %.2f ms (%.0f ns per element)\n%s\n", elapsed / 1000.0,
        elapsed * 1000.0 / 100'000.0, script);
  }
}

}  // namespace PJ::scripting
