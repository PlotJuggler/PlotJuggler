# ---------------------------------------------------------------------------
# pj_scene3d_core - geometry/scene core, no Qt or GL dependency
# ---------------------------------------------------------------------------

find_package(glm REQUIRED)
find_package(nlohmann_json REQUIRED)

if(EMSCRIPTEN)
  # Expose only the backend-neutral slice used by the product QRhi view and its
  # canonical point-cloud, pose-array, occupancy, voxel, marker, and robot-model
  # layers. Compressed canonical clouds add only the existing transcoder when
  # their explicit browser dependency option is enabled. The native branch
  # below is unchanged.
  set(_pj_scene3d_core_sources
    src/camera/camera.cpp
    src/camera/camera_math.cpp
    src/depth_backproject.cpp
    src/occupancy_grid_reconstructor.cpp
    src/pointcloud_convert.cpp
    src/poses_in_frame_render.cpp
    src/robot_model_bridges.cpp
    src/scene_entities_decode.cpp
    src/scene_entities_model_state.cpp
    src/shadow_camera.cpp
    src/tf/tf_buffer.cpp
    src/tf/tf_connections.cpp
    src/version.cpp
    src/voxel_grid_view.cpp
    src/voxel_grid_value.cpp
  )
  if(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
    list(APPEND _pj_scene3d_core_sources src/pointcloud_codecs.cpp)
  endif()
  add_library(pj_scene3d_core STATIC ${_pj_scene3d_core_sources})
  target_include_directories(pj_scene3d_core PUBLIC include)
  target_compile_features(pj_scene3d_core PUBLIC cxx_std_20)
  target_compile_options(pj_scene3d_core PRIVATE
    ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS}
  )
  target_link_libraries(pj_scene3d_core
    PUBLIC glm::glm plotjuggler_sdk::base nlohmann_json::nlohmann_json
  )
  if(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
    if(NOT TARGET draco::draco OR NOT TARGET cloudini::cloudini)
      message(FATAL_ERROR "WASM compressed point clouds require the pinned Draco and Cloudini targets")
    endif()
    target_compile_definitions(pj_scene3d_core PUBLIC PJ_WASM_WITH_COMPRESSED_POINTCLOUDS=1)
    target_link_libraries(pj_scene3d_core PRIVATE draco::draco cloudini::cloudini)
  endif()
  unset(_pj_scene3d_core_sources)
  return()
endif()

# Compressed-point-cloud decoders (Draco + Cloudini). Linked PRIVATE: the codecs are
# an implementation detail of pj_scene3d_core, mirroring how pj_scene2d_core links
# libjpeg-turbo / libpng PRIVATE for image decode.
find_package(draco REQUIRED)
find_package(cloudini REQUIRED)

add_library(pj_scene3d_core STATIC
  src/camera/camera.cpp
  src/camera/camera_math.cpp
  src/depth_backproject.cpp
  src/occupancy_grid_reconstructor.cpp
  src/pointcloud_codecs.cpp
  src/pointcloud_convert.cpp
  src/poses_in_frame_render.cpp
  src/robot_model_bridges.cpp
  src/scene_entities_decode.cpp
  src/scene_entities_model_state.cpp
  src/shadow_camera.cpp
  src/tf/tf_buffer.cpp
  src/tf/tf_connections.cpp
  src/trail_sampling.cpp
  src/version.cpp
  src/grid_map_view.cpp
  src/voxel_grid_view.cpp
  src/voxel_grid_value.cpp
)
target_include_directories(pj_scene3d_core PUBLIC include)
target_compile_features(pj_scene3d_core PUBLIC cxx_std_20)
target_compile_options(pj_scene3d_core PRIVATE
  ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS}
)
target_link_libraries(pj_scene3d_core
  PUBLIC glm::glm plotjuggler_sdk::base nlohmann_json::nlohmann_json
  PRIVATE draco::draco cloudini::cloudini
)

if(PJ_BUILD_TESTS)
  set(PJ_SCENE3D_CORE_TESTS
    ../tests/occupancy_grid_reconstructor_test.cpp
    ../tests/occupancy_grid_bounds_test.cpp
    ../tests/occupancy_grid_budget_test.cpp
    ../tests/depth_backproject_test.cpp
    ../tests/pointcloud_convert_test.cpp
    ../tests/pointcloud_budget_test.cpp
    ../tests/pointcloud_fast_path_predicate_test.cpp
    ../tests/pointcloud_bounds_scan_equivalence_test.cpp
    ../tests/aabb_gpu_key_test.cpp
    ../tests/aabb_axis_range_test.cpp
    ../tests/frame_picking_test.cpp
    ../tests/camera_near_far_test.cpp
    ../tests/camera_zoom_to_cursor_test.cpp
    ../tests/camera_state_transfer_test.cpp
    ../tests/camera_follow_test.cpp
    ../tests/poses_in_frame_render_test.cpp
    ../tests/poses_budget_test.cpp
    ../tests/model_budget_test.cpp
    ../tests/scene_entities_budget_test.cpp
    ../tests/scene_entities_decode_test.cpp
    ../tests/scene_entities_model_state_test.cpp
    ../tests/frustum_cull_test.cpp
    ../tests/cube_draw_policy_test.cpp
    ../tests/shadow_camera_test.cpp
    ../tests/tf_buffer_test.cpp
    ../tests/tf_buffer_hierarchy_test.cpp
    ../tests/tf_connections_test.cpp
    ../tests/trail_sampling_test.cpp
    ../tests/robot_model_bridges_test.cpp
    ../tests/grid_map_view_test.cpp
    ../tests/voxel_grid_view_test.cpp
    ../tests/voxel_grid_value_test.cpp
    ../tests/voxel_grid_budget_test.cpp
  )
  # pointcloud_codecs_test drives the Draco + Cloudini *encoders* directly to build
  # fixtures, so it links them explicitly (pj_scene3d_core links them PRIVATE).
  # WARNINGS: all 34 merged tests carried PJ_WARNING_FLAGS before
  pj_add_test_runner(pj_scene3d_core_tests
    MAIN core WARNINGS
    SOURCES ${PJ_SCENE3D_CORE_TESTS} ../tests/pointcloud_codecs_test.cpp
    LIBS pj_scene3d_core draco::draco cloudini::cloudini glm::glm
  )

  # Reproducibility helper for the checked browser MCAP. EXCLUDE_FROM_ALL keeps
  # the native application and ordinary test build unchanged; W19a invokes this
  # target explicitly before the Python ROS2 CDR/MCAP packager.
  add_executable(wasm_compressed_pointcloud_payload_generator EXCLUDE_FROM_ALL
    ../../tests/wasm/fixtures/generate_compressed_pointcloud_payloads.cpp)
  target_link_libraries(wasm_compressed_pointcloud_payload_generator PRIVATE
    draco::draco cloudini::cloudini)
  target_compile_options(wasm_compressed_pointcloud_payload_generator PRIVATE
    ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
endif()
