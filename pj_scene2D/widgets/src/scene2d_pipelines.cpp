// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/scene2d_pipelines.h"

#include "pj_scene2d_core/codecs.h"

namespace PJ {

std::unique_ptr<CodecPipeline> makeScene2DPipelineFor(sdk::BuiltinObjectType object_type) {
  switch (object_type) {
    case sdk::BuiltinObjectType::kImage:
      return makeJpegPipeline();
    case sdk::BuiltinObjectType::kDepthImage:
      return nullptr;
    case sdk::BuiltinObjectType::kNone:
    case sdk::BuiltinObjectType::kPointCloud:
    case sdk::BuiltinObjectType::kImageAnnotations:
    case sdk::BuiltinObjectType::kFrameTransforms:
    case sdk::BuiltinObjectType::kOccupancyGrid:
    case sdk::BuiltinObjectType::kCompressedPointCloud:
    case sdk::BuiltinObjectType::kMesh3D:
    case sdk::BuiltinObjectType::kVideoFrame:
    case sdk::BuiltinObjectType::kSceneEntities:
    case sdk::BuiltinObjectType::kRobotDescription:
    case sdk::BuiltinObjectType::kCameraInfo:
    case sdk::BuiltinObjectType::kOccupancyGridUpdate:
    case sdk::BuiltinObjectType::kLog:
    // kPosesInFrame is a 3D pose-array geometry type with no 2D decode path;
    // listed to keep this exhaustive switch -Werror=switch clean.
    case sdk::BuiltinObjectType::kPosesInFrame:
    // kPlotMarkers renders in pj_plotting (time-series overlay), not the 2D
    // scene; listed to keep this exhaustive switch -Werror=switch clean.
    case sdk::BuiltinObjectType::kPlotMarkers:
    // kVoxelGrid is a dense 3D volume rendered only by pj_scene3D; no 2D path.
    case sdk::BuiltinObjectType::kVoxelGrid:
    // kGridMap is a heightfield rendered only by pj_scene3D; no 2D path.
    case sdk::BuiltinObjectType::kGridMap:
      return nullptr;
  }
  return nullptr;
}

}  // namespace PJ
