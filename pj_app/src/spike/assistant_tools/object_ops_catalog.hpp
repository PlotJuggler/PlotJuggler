// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once
//
// The Luau operations a script (create_derived_object/evaluate) may call on a
// derived-object BuiltinObject of a given type, advertised by describe_topic
// alongside the type's field table. This table mirrors PJ4's
// pj_scripting/src/object_binder.cpp -- the generic Luau <-> sdk::BuiltinObject
// binder that actually exposes these calls to a running script -- so the
// assistant never advertises a method a script cannot really call. A type with
// no entry here (FrameTransforms, CameraInfo, and every type with no
// field table at all) exposes its field table only; there is nothing to add.
//
// Mirrors PJ4's Luau/Python object binder by hand; no cross-repo test pins it -- keep in sync when the
// host's operations change.
//

#include <pj_base/builtin/builtin_object.hpp>
#include <string_view>
#include <vector>

namespace assistant_agent {

// One callable operation: its Luau call shape and a one-line description.
struct ObjectOperation {
  std::string_view name;
  std::string_view doc;
};

// The operations a script may call on an object of `type`, or empty when the
// type exposes only its field table (see the file comment).
[[nodiscard]] inline std::vector<ObjectOperation> objectOperationsFor(PJ::sdk::BuiltinObjectType type) {
  using PJ::sdk::BuiltinObjectType;
  switch (type) {
    case BuiltinObjectType::kPointCloud:
      return {
          {"count()", "number of points in the cloud"},
          {"bounds()", "axis-aligned {min={x,y,z}, max={x,y,z}, count} over finite points, or nil if empty"},
          {"crop_box{min={x,y,z}, max={x,y,z}}",
           "new cloud keeping only points inside the box; min/max are positional {x,y,z} arrays"},
          {"filter(channel, op, value)",
           "new cloud keeping points where channel <op> value; op is one of <, <=, >, >=, ==, ~="},
          {"extreme_point(channel, \"max\"|\"min\")", "the point with the extreme value of channel, or nil if empty"},
          {"xyz_points()", "iterator over the cloud's finite {x, y, z} points"},
          {"transform(tf)",
           "a copy of the cloud rigidly moved by a pj.tf.lookup() result (frame_id becomes its target)"},
      };
    case BuiltinObjectType::kSceneEntities:
      return {
          {"pj.scene.new{frame_id=, id=}:sphere{center={x,y,z},diameter=,color={r,g,b,a}?}"
           ":cube{center={x,y,z},size={x,y,z},color={r,g,b,a}?}"
           ":line_strip{points={{x,y,z},...},thickness=,color={r,g,b,a}?}:finish()",
           "builder: chain shape calls off pj.scene.new(...), then :finish() to materialize a SceneEntities object"},
      };
    case BuiltinObjectType::kImage:
    case BuiltinObjectType::kDepthImage: {
      std::vector<ObjectOperation> operations = {
          {"representation()", "image storage representation"},
          {"decode()", "decoded pixels, or nil plus a reason when decoding is unavailable"},
          {"decode():pixel(x,y)", "zero-based pixel coordinates; returns the channel values"},
          {"decode():to_gray()", "new single-channel decoded pixels"},
          {"decode():crop(x,y,width,height)", "crop decoded pixels, retaining original image coordinates"},
          {"decode():threshold(op,value[,channel])",
           "ImageAnnotations points in original image coordinates; channel is zero-based or r/g/b/a"},
          {"decode():stats()", "decoded pixel statistics {min,max,mean}"},
      };
      operations.push_back(
          {"to_point_cloud(camera_info[, {step=n,max_depth=m}])",
           "project mono16/16UC1 depth (millimeters) or 32FC1 depth (meters) to a PointCloud; pj.unavailable "
           "otherwise"});
      return operations;
    }
    case BuiltinObjectType::kVideoFrame:
      return {
          {"frame_at(t_ns)",
           "decoded {image,presentation_ns}, or nil plus a reason; pass an exact integer or a pj.int64 timestamp "
           "(timestamp_ns fields and pj.tf.now are pj.int64; scripts cannot construct one)"}};
    case BuiltinObjectType::kImageAnnotations:
      return {
          {"pj.annotations.new{image_topic=}:points{points={{x,y},...},type=,thickness=,outline_color=,fill_color=}:"
           "finish()",
           "build points, line_list, line_strip or line_loop in original image coordinates"},
          {"pj.annotations.new{image_topic=}:circle{center={x,y},diameter=,thickness=,outline_color=,fill_color=}:"
           "finish()",
           "build a circle overlay"},
          {"pj.annotations.new{image_topic=}:text{position={x,y},text=,font_size=,text_color=}:finish()",
           "build a text overlay; nontransparent background_color is unsupported"},
          {"count()", "total number of points over all point annotations (no copy)"}};
    case BuiltinObjectType::kFrameTransforms:
    case BuiltinObjectType::kNone:
    case BuiltinObjectType::kOccupancyGrid:
    case BuiltinObjectType::kCompressedPointCloud:
    case BuiltinObjectType::kMesh3D:
    case BuiltinObjectType::kRobotDescription:
    case BuiltinObjectType::kCameraInfo:
    case BuiltinObjectType::kOccupancyGridUpdate:
    case BuiltinObjectType::kLog:
    case BuiltinObjectType::kPosesInFrame:
    case BuiltinObjectType::kVoxelGrid:
    case BuiltinObjectType::kPlotMarkers:
    case BuiltinObjectType::kGridMap:
      return {};
  }
  return {};
}

}  // namespace assistant_agent
