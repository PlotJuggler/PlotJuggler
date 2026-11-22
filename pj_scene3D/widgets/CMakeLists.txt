set(CMAKE_AUTOMOC ON)

# Shared cube-appearance GLSL (shaders/cube/*.glslinc): the .vert/.frag sources #include these files directly (qsb
# honours GL_GOOGLE_include_directive, resolving against the including file's directory). The .qsb staleness check
# below runs at configure time, so a changed helper has to re-trigger configure for the check to see it.
file(GLOB _pj_cube_glsl_sources "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube/*.glslinc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_pj_cube_glsl_sources})

# Browser-only policy (file picking, bounded model and URL loading) is gated on PJ_TARGET_WASM.
set(CMAKE_AUTORCC ON)
find_package(Qt6 ${PJ_QT_REQUIRED_VERSION} REQUIRED COMPONENTS Widgets Gui GuiPrivate Xml Concurrent Network)
if(NOT EMSCRIPTEN)
  find_package(assimp REQUIRED)
endif()

# The committed packs are verified in the browser build only, whose Qt kit
# carries the host qsb (Qt ShaderTools); desktop kits don't install it.
if(EMSCRIPTEN)
  include(${CMAKE_SOURCE_DIR}/cmake/PjQsb.cmake)
  pj_find_host_qsb(_pj_scene3d_qsb "WASM Scene3D")

  # bake.sh is the single source of the qsb targets; re-run it and require the
  # committed packs to match byte for byte.
  set(_pj_scene3d_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/rhi/shaders")
  set(_pj_scene3d_shader_verify_dir "${CMAKE_CURRENT_BINARY_DIR}/verified_shaders")
  file(REMOVE_RECURSE "${_pj_scene3d_shader_verify_dir}")
  file(MAKE_DIRECTORY "${_pj_scene3d_shader_verify_dir}")
  execute_process(
    COMMAND bash "${_pj_scene3d_shader_dir}/bake.sh" "${_pj_scene3d_qsb}" "${_pj_scene3d_shader_verify_dir}"
    RESULT_VARIABLE _pj_scene3d_qsb_result
    ERROR_VARIABLE _pj_scene3d_qsb_error)
  if(NOT _pj_scene3d_qsb_result EQUAL 0)
    message(FATAL_ERROR "Failed to bake the Scene3D shaders: ${_pj_scene3d_qsb_error}")
  endif()
  file(GLOB _pj_scene3d_rebuilt_packs RELATIVE "${_pj_scene3d_shader_verify_dir}"
    "${_pj_scene3d_shader_verify_dir}/*.qsb")
  file(GLOB _pj_scene3d_committed_packs RELATIVE "${_pj_scene3d_shader_dir}" "${_pj_scene3d_shader_dir}/*.qsb")
  if(NOT _pj_scene3d_committed_packs STREQUAL _pj_scene3d_rebuilt_packs)
    message(FATAL_ERROR "Scene3D shader packs out of sync with rhi/shaders/bake.sh: committed "
      "[${_pj_scene3d_committed_packs}], baked [${_pj_scene3d_rebuilt_packs}]")
  endif()
  foreach(_pj_scene3d_pack IN LISTS _pj_scene3d_rebuilt_packs)
    file(SHA256 "${_pj_scene3d_shader_dir}/${_pj_scene3d_pack}" _pj_scene3d_committed_hash)
    file(SHA256 "${_pj_scene3d_shader_verify_dir}/${_pj_scene3d_pack}" _pj_scene3d_rebuilt_hash)
    if(NOT _pj_scene3d_committed_hash STREQUAL _pj_scene3d_rebuilt_hash)
      message(FATAL_ERROR
        "Stale Scene3D shader pack ${_pj_scene3d_pack}; re-bake with rhi/shaders/bake.sh")
    endif()
  endforeach()
  file(GLOB _pj_scene3d_shader_sources "${_pj_scene3d_shader_dir}/*.vert" "${_pj_scene3d_shader_dir}/*.frag"
    "${_pj_scene3d_shader_dir}/*.glslinc")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_pj_scene3d_shader_dir}/bake.sh" ${_pj_scene3d_shader_sources})
endif()

add_library(pj_scene3d_widgets STATIC
  rhi/scene_view_widget_rhi.cpp
  rhi/scene_view_widget_rhi_quality.cpp
  rhi/scene_view_widget_rhi_quality_p.h
  rhi/rhi_scene_pass.cpp
  rhi/rhi_scene_pass.h
  rhi/passes/marker_rhi_pass.cpp
  rhi/passes/marker_rhi_pass.h
  rhi/passes/model_rhi_pass.cpp
  rhi/passes/model_rhi_pass.h
  rhi/passes/occupancy_rhi_pass.cpp
  rhi/passes/occupancy_rhi_pass.h
  rhi/passes/grid_rhi_pass.cpp
  rhi/passes/grid_rhi_pass.h
  rhi/passes/overlay_rhi_pass.cpp
  rhi/passes/overlay_rhi_pass.h
  rhi/passes/gridmap_rhi_pass.cpp
  rhi/passes/gridmap_rhi_pass.h
  rhi/passes/lit_surface_uniforms.h
  rhi/passes/meshmap_rhi_pass.cpp
  rhi/passes/meshmap_rhi_pass.h
  rhi/passes/trail_rhi_pass.cpp
  rhi/passes/trail_rhi_pass.h
  src/hud_overlay.cpp
  rhi/passes/pose_rhi_pass.cpp
  rhi/passes/pose_rhi_pass.h
  rhi/passes/voxel_rhi_pass.cpp
  rhi/passes/voxel_rhi_pass.h
  rhi/passes/point_rhi_pass.cpp
  rhi/passes/point_rhi_pass.h
  src/Scene3DDockWidget.cpp
  src/layers/depth_cloud_layer.cpp
  src/layers/pointcloud_layer.cpp
  src/layers/poses_in_frame_layer.cpp
  src/layers/occupancy_grid_layer.cpp
  src/layers/voxel_grid_layer.cpp
  src/layers/scene_entities_layer.cpp
  src/layers/robot_model_layer.cpp
  src/layers/trail_layer.cpp
  src/layers/grid_map_layer.cpp
  src/layers/mesh_map_layer.cpp
  src/channel_coloring.cpp
  src/marker_geometry.cpp
  src/mesh_loader.cpp
  src/mesh_load_queue.cpp
  src/url_fetcher.cpp
  src/urdf_parser.cpp
  src/urdf_package_resolver.cpp
  src/FixedFrameMemory.cpp
  src/layout_resources.cpp
  src/object_topic_metadata.cpp
  src/resolve_object.cpp
  src/scene_state_xml.cpp
  src/scene3d_layer.cpp
  src/grid_geometry.cpp
  src/gizmos/arrow_mesh.cpp
  include/pj_scene3d_widgets/scene_view_widget.h
  include/pj_scene3d_widgets/grid_geometry.h
  include/pj_scene3d_widgets/Scene3DDockWidget.h
  include/pj_scene3d_widgets/layers/depth_cloud_layer.h
  include/pj_scene3d_widgets/layers/pointcloud_layer.h
  include/pj_scene3d_widgets/scene3d_layer.h
  include/pj_scene3d_widgets/layer_style.h
  include/pj_scene3d_widgets/point_shading.h
  include/pj_scene3d_widgets/point_renderable.h
  include/pj_scene3d_widgets/retained_upload.h
  include/pj_scene3d_widgets/model_renderable.h
  include/pj_scene3d_widgets/layers/poses_in_frame_layer.h
  include/pj_scene3d_widgets/layers/occupancy_grid_layer.h
  include/pj_scene3d_widgets/layers/voxel_grid_layer.h
  include/pj_scene3d_widgets/layers/scene_entities_layer.h
  include/pj_scene3d_widgets/layers/robot_model_layer.h
  include/pj_scene3d_widgets/layers/trail_layer.h
  include/pj_scene3d_widgets/layers/grid_map_layer.h
  include/pj_scene3d_widgets/layers/mesh_map_layer.h
  include/pj_scene3d_widgets/channel_coloring.h
  include/pj_scene3d_widgets/marker_geometry.h
  include/pj_scene3d_widgets/gizmos/arrow_mesh.h
  include/pj_scene3d_widgets/FixedFrameMemory.h
  include/pj_scene3d_widgets/layout_resources.h
  include/pj_scene3d_widgets/object_topic_metadata.h
  include/pj_scene3d_widgets/resolve_object.h
  rhi/shaders/scene3d_rhi_shaders.qrc
)
target_include_directories(pj_scene3d_widgets
  PUBLIC include
  PRIVATE src)
target_compile_features(pj_scene3d_widgets PUBLIC cxx_std_20)
# The view queries Vulkan's 3D-texture limit only with the Vulkan headers
# (the renderer's QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>) guard);
# without them voxel grids fall back to the guaranteed 256 per dimension.
if(NOT EMSCRIPTEN AND QT_FEATURE_vulkan AND NOT TARGET WrapVulkanHeaders::WrapVulkanHeaders)
  message(WARNING "Vulkan headers not found (install libvulkan-dev / the Vulkan SDK): the QRhi Scene3D view "
    "cannot query the Vulkan 3D-texture limit and caps voxel grids at 256 cells per dimension.")
endif()
target_compile_options(pj_scene3d_widgets PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_scene3d_widgets
  PUBLIC
    pj_scene3d_core
    pj_scene_common
    pj_runtime
    pj_widgets
    Qt6::Widgets
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Xml
  PRIVATE
    Qt6::Concurrent
    Qt6::Network
    assimp::assimp
)

# SceneViewTestAccess (testing/): test-only access to the view's internals for
# the tests, the benchmarks and the browser acceptance probes (the WASM
# acceptance build links it with tests off). The app never links it.
add_library(pj_scene3d_widgets_testing STATIC EXCLUDE_FROM_ALL testing/scene_view_test_access.cpp)
# The tests read the passes and the quality stages through it (rhi/passes/*.h,
# rhi/scene_view_widget_rhi_quality_p.h).
target_include_directories(pj_scene3d_widgets_testing PUBLIC testing rhi)
target_link_libraries(pj_scene3d_widgets_testing PUBLIC pj_scene3d_widgets)
target_compile_options(pj_scene3d_widgets_testing PRIVATE ${PJ_WARNING_FLAGS})

if(PJ_BUILD_TESTS AND NOT EMSCRIPTEN)
  # The QRhi display runners need an RHI-capable QPA (xcb/wayland/windows/cocoa), never `offscreen` (see
  # scene3d_render_test_support.h). They register on every backend of the platform (RHI_BACKENDS), each needing a
  # software or real driver (llvmpipe / lavapipe / WARP / Metal in CI); PJ_REQUIRE_GPU_TESTS=1 turns a missing one
  # into a failure.
  find_package(Qt6 ${PJ_QT_REQUIRED_VERSION} REQUIRED COMPONENTS Test)
  pj_add_test_runner(scene3d_rhi_render_test
    MAIN gui WARNINGS RHI_BACKENDS
    # The presentation cases also run at device-pixel ratio 2: overlay placement,
    # picking and render_scale must hold in logical pixels. So do the TF-line
    # visibility and floor-shadow cases.
    DPR2 DPR2_FILTER "Scene3dRhiRenderTest.Presentation*:Scene3dRhiRenderTest.MagentaLines*:Scene3dRhiRenderTest.FloorShadow*"
    SOURCES tests/scene3d_rhi_render_test.cpp
    LIBS pj_scene3d_widgets pj_scene3d_widgets_testing pj_resources Qt6::Test
    TIMEOUT 60
  )
  # The golden corpus (tests/fixtures/golden/pointcloud/README.md).
  pj_add_test_runner(pointcloud_golden_test
    MAIN gui WARNINGS RHI_BACKENDS
    SOURCES tests/pointcloud_golden_test.cpp
    LIBS pj_scene3d_widgets pj_resources pj_bench_probes
    DEFINES PJ_POINTCLOUD_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden/pointcloud"
  )
  pj_add_test_runner(scene3d_frame_benchmark
    MAIN gui WARNINGS RHI_BACKENDS
    SOURCES tests/scene3d_frame_benchmark.cpp
    LIBS pj_scene3d_widgets pj_scene3d_widgets_testing pj_resources pj_bench_probes
  )
  pj_add_test_runner(scene3d_rhi_point_test
    MAIN gui WARNINGS RHI_BACKENDS
    SOURCES tests/scene3d_rhi_point_test.cpp
    LIBS pj_scene3d_widgets pj_scene3d_widgets_testing pj_resources Qt6::Test
    TIMEOUT 60
  )
  pj_add_test_runner(scene3d_rhi_failure_test
    MAIN gui WARNINGS RHI_BACKENDS
    SOURCES tests/scene3d_rhi_failure_test.cpp
    LIBS pj_scene3d_widgets pj_scene3d_widgets_testing pj_resources pj_bench_probes Qt6::OpenGLWidgets Qt6::Test
    TIMEOUT 60
  )
  foreach(_pj_rhi_runner scene3d_rhi_marker_cache_test scene3d_rhi_model_cache_test scene3d_rhi_platform_test)
    pj_add_test_runner(${_pj_rhi_runner}
      MAIN gui WARNINGS RHI_BACKENDS
      SOURCES tests/${_pj_rhi_runner}.cpp
      LIBS pj_scene3d_widgets pj_scene3d_widgets_testing pj_resources pj_bench_probes Qt6::Test
      DEFINES PJ_SCENE3D_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
      TIMEOUT 60
    )
  endforeach()
  unset(_pj_rhi_runner)

  # Shared test scaffolding (mock_parser_support.h) lives in tests/: on every test target's include path
  # (directory-scoped, tests only).
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tests)

  # Test hooks add accessors in public library headers, so every test TU in a
  # runner must agree on PJ_SCENE3D_TEST_HOOKS to avoid differing class definitions.
  set(PJ_SCENE3D_WIDGET_TESTS)
  set(PJ_SCENE3D_WIDGET_PLAIN_TESTS)

  find_package(glm REQUIRED)

  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/grid_geometry_test.cpp
    tests/rhi_conventions_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/fixed_frame_memory_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/resolve_object_test.cpp
  )

  # Keep this executable separate: PJ_SCENE3D_SHADER_DIR is unique to its source. No GL context needed.
  add_executable(scene3d_shader_constants_test
    tests/shader_constants_test.cpp
  )
  # Qt6::GuiPrivate: <rhi/qshader.h>, to read the baked packs' reflection.
  target_link_libraries(scene3d_shader_constants_test PRIVATE
    pj_scene3d_widgets
    Qt6::GuiPrivate
    GTest::gtest
  )
  target_compile_definitions(scene3d_shader_constants_test PRIVATE
    PJ_SCENE3D_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/rhi/shaders"
  )
  target_compile_options(scene3d_shader_constants_test PRIVATE ${PJ_WARNING_FLAGS})
  add_test(NAME scene3d_shader_constants_test COMMAND scene3d_shader_constants_test)

  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/cube_mesh_test.cpp
  )

  # Reaches the private src/ headers (urdf_parser.h / urdf_package_resolver.h); fixtures are hand-written under
  # tests/fixtures/.
  # Keep this definition confined to the sources that already share its value.
  set(PJ_SCENE3D_WIDGET_FIXTURE_TESTS
    tests/urdf_parser_test.cpp
    tests/urdf_package_resolver_test.cpp
    tests/mesh_loader_test.cpp
    tests/mesh_load_queue_test.cpp
    tests/url_fetcher_test.cpp
    tests/scene_entities_layer_model_test.cpp
    tests/robot_model_layer_test.cpp
    tests/layout_resources_test.cpp
  )

  # The coalescing test encodes valid fixtures with the Cloudini encoder, which
  # pj_scene3d_core links PRIVATE — re-resolve the imported target here so the
  # test can link it directly (same as pointcloud_codecs_test in core/).
  find_package(cloudini CONFIG QUIET)
  if(NOT TARGET cloudini::cloudini)
    # cloudini's upstream package (see pj_scene3D/core/CMakeLists.txt)
    find_package(cloudini_lib CONFIG REQUIRED)
    add_library(cloudini::cloudini ALIAS cloudini::cloudini_lib)
  endif()

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/pointcloud_layer_cache_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/pointcloud_layer_fast_path_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/pointcloud_layer_param_transfer_test.cpp
  )

  # Drives the real DoubleScrubber widgets, so it needs a QApplication.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/pointcloud_layer_range_coupling_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/depth_cloud_layer_test.cpp
  )

  # The dock is a QWidget: needs the shared QApplication main.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/scene3d_dock_depth_gate_test.cpp
  )

  # Pins PointCloudLayer's per-use parser binding (the raw renderAt path calls parseLocked on the UI thread per tick);
  # mirrors occupancy_grid_layer_rebind_test.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    ../tests/pointcloud_layer_rebind_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    ../tests/pointcloud_layer_rgb_test.cpp
  )

  # Encodes valid fixtures with the Cloudini encoder (PRIVATE in pj_scene3d_core), so it links cloudini explicitly like
  # pointcloud_codecs_test does.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    ../tests/pointcloud_layer_coalescing_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/occupancy_grid_layer_updates_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/voxel_grid_layer_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/poses_in_frame_layer_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/layer_budget_latch_test.cpp
  )

  # Needs a QApplication.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/scene3d_follow_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/scene_view_placement_test.cpp
  )

  # The view needs no GPU until it is shown.

  # Needs the PJ_SCENE3D_TEST_HOOKS accessors and a QApplication (the dock is a QWidget).
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/scene3d_dock_streaming_test.cpp
  )

  # Runs the on-demand engine with pj_runtime's fake script run.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/scene3d_dock_derived_topics_test.cpp
  )

  # Needs the PJ_SCENE3D_TEST_HOOKS accessors and a QApplication (the dock is a QWidget).
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/scene3d_dock_persistence_test.cpp
  )

  # No GPU (the view is never shown).
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/scene3d_dock_camera_bounds_test.cpp
  )

  # Drives the dock headless through a mock FrameTransforms parser; needs a QApplication.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/scene3d_dock_fixed_frame_test.cpp
    tests/scene3d_dock_package_folders_test.cpp
  )

  # Builds real config widgets, so it needs a QApplication; links pj_resources so PJ::theme::space() resolves real QSS
  # values (the -1 fallback would mask the Snug/Comfortable gap).
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/layer_config_widget_row_spacing_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/scene3d_layer_xml_validation_test.cpp
    tests/scene3d_dock_xml_options_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/scene_view_widget_mouse_gesture_test.cpp
  )

  # Needs a QApplication for the font system; no GPU.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/hud_overlay_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    ../tests/occupancy_grid_layer_rebind_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/grid_map_layer_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/mesh_map_layer_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/trail_layer_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/model_rhi_policy_test.cpp
  )

  pj_add_test_runner(pj_scene3d_widget_tests
    MAIN gui WARNINGS
    SOURCES ${PJ_SCENE3D_WIDGET_TESTS}
    LIBS pj_scene3d_widgets pj_scene_common pj_widgets cloudini::cloudini
    DEFINES PJ_SCENE3D_TEST_HOOKS
  )
  target_include_directories(pj_scene3d_widget_tests PRIVATE ${CMAKE_SOURCE_DIR}/pj_runtime/tests)
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/marker_geometry_test.cpp
  )
  pj_add_test_runner(pj_scene3d_widget_plain_tests
    MAIN gui WARNINGS
    SOURCES ${PJ_SCENE3D_WIDGET_PLAIN_TESTS}
    LIBS pj_scene3d_widgets pj_scene3d_widgets_testing pj_scene_common pj_widgets pj_resources glm::glm Qt6::Network
         Qt6::Test
  )
  # These tests stand up in-process QTcpServer endpoints / talk Qt networking
  # directly; pj_scene3d_widgets links Qt6::Network only PRIVATEly.
  pj_add_test_runner(pj_scene3d_widget_fixture_tests
    MAIN gui WARNINGS
    SOURCES ${PJ_SCENE3D_WIDGET_FIXTURE_TESTS}
    LIBS pj_scene3d_widgets Qt6::Network
    DEFINES PJ_SCENE3D_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures" PJ_SCENE3D_TEST_HOOKS
  )
  target_include_directories(pj_scene3d_widget_fixture_tests PRIVATE src)
endif()
