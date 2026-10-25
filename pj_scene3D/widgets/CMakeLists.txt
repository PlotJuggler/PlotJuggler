set(CMAKE_AUTOMOC ON)

# Shared cube-appearance GLSL (shaders/cube/*.glslinc), one source for every shader
# that draws cubes: the .vert/.frag sources #include these files directly (qsb honours
# GL_GOOGLE_include_directive, resolving against the including file's directory). The
# .qsb staleness check below runs at configure time, so a changed helper has to
# re-trigger configure for the check to see it.
file(GLOB _pj_cube_glsl_sources "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube/*.glslinc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_pj_cube_glsl_sources})

# The QRhi renderer, on the desktop and in the browser (WebGL2). The browser-only
# policy it carries (file picking, bounded model and URL loading) is gated on
# PJ_TARGET_WASM.
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
if(EMSCRIPTEN)
  target_compile_options(pj_scene3d_widgets PRIVATE -fexceptions)
endif()
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
if(EMSCRIPTEN)
  target_compile_options(pj_scene3d_widgets_testing PRIVATE -fexceptions)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

# The WASM build has no tests.
if(PJ_BUILD_TESTS AND NOT EMSCRIPTEN)
  # The QRhi display runners need an RHI-capable QPA (xcb/wayland/windows/cocoa),
  # never `offscreen` (see scene3d_render_test_support.h). Registered on every
  # backend of the platform (RHI_BACKENDS); each needs a software or real driver
  # (llvmpipe / lavapipe / WARP / Metal in CI), and PJ_REQUIRE_GPU_TESTS=1 turns a
  # missing one into a failure.
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
  # Frame budgets of the view: a smoke at its default (tiny) sizes; larger ones
  # come from its environment variables.
  pj_add_test_runner(scene3d_frame_benchmark
    MAIN gui WARNINGS RHI_BACKENDS
    SOURCES tests/scene3d_frame_benchmark.cpp
    LIBS pj_scene3d_widgets pj_scene3d_widgets_testing pj_resources pj_bench_probes
  )
  # Point-pass behaviour the golden corpus does not pin (fan faces, cube LOD,
  # sphere sizing, faded-range occlusion, frustum-edge quads); same display needs.
  pj_add_test_runner(scene3d_rhi_point_test
    MAIN gui WARNINGS RHI_BACKENDS
    SOURCES tests/scene3d_rhi_point_test.cpp
    LIBS pj_scene3d_widgets pj_scene3d_widgets_testing pj_resources Qt6::Test
    TIMEOUT 60
  )
  # The failure state and the optional-stage fallbacks, driven by the fault
  # injection of builds with tests (PJ_RHI_TEST_HOOKS); same display needs.
  pj_add_test_runner(scene3d_rhi_failure_test
    MAIN gui WARNINGS RHI_BACKENDS
    SOURCES tests/scene3d_rhi_failure_test.cpp
    LIBS pj_scene3d_widgets pj_scene3d_widgets_testing pj_resources Qt6::OpenGLWidgets Qt6::Test
    TIMEOUT 60
  )
  # The marker instance cache, the model mesh/instance caches and the platform
  # limits; same display needs.
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

  # Shared test scaffolding (mock_parser_support.h: makeBoundHandle /
  # CountingObjectParser / registerObjectTopic / pumpUntil) lives in tests/; put
  # it on every test target's include path (directory-scoped — tests only).
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tests)

  # Test hooks add accessors in public library headers, so every test TU in a
  # runner must agree on PJ_SCENE3D_TEST_HOOKS to avoid differing class definitions.
  set(PJ_SCENE3D_WIDGET_TESTS)
  set(PJ_SCENE3D_WIDGET_PLAIN_TESTS)

  # The grid geometry tests use glm directly for their expected vertices.
  find_package(glm REQUIRED)

  # Pure (no GL/Qt) ground-grid geometry: full-checkerboard cell coverage +
  # per-vertex parity, and the line-grid vertex counts.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/grid_geometry_test.cpp
    tests/rhi_conventions_test.cpp
  )

  # FixedFrameMemory: the per-dataset manual fixed-frame preference (session cache,
  # QSettings persistence by source identity). No GL.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/fixed_frame_memory_test.cpp
  )

  # resolveObject() canonical fallback: with no MessageParser bound, decode a
  # serialized pj_base blob via the canonical codec (the parser-less / toolbox
  # path). Pure unit test — no session, no GL.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/resolve_object_test.cpp
  )

  # Keep this executable separate: PJ_SCENE3D_SHADER_DIR is unique to its source.
  # Reads the shader sources and packs as data: fails if a shader stops including the
  # shared cube GLSL, re-inlines a literal appearance constant, or the GLSL drifts from
  # cube_mesh.h's C++ mirrors. No GL context needed.
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

  # Geometry invariants of the shared cube: the CCW-outward winding every culling
  # pipeline depends on, and the hexagon fan's coverage of the 3 camera-facing faces
  # in all eight sign octants. Pure math — no GL context needed.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/cube_mesh_test.cpp
  )

  # Pure (non-GL) URDF parser + package-resolver unit tests. They reach the
  # private src/ headers (urdf_parser.h / urdf_package_resolver.h) and use small
  # hand-written fixtures under tests/fixtures/ (NOT the external DATA/ datasets).
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
  find_package(cloudini REQUIRED)

  # PointCloudLayer sample-identity caches: reset on dataset replace (stale
  # decoded/failed/pushed ids under a stable topic id), stamp-0 is data not a
  # sentinel, and wanted_-gating of stale async decodes on mixed-mode topics.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/pointcloud_layer_cache_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/pointcloud_layer_fast_path_test.cpp
  )

  # Copy/paste/apply-to-family: serializeLayerParams/applyLayerParams must
  # round-trip every PointCloudLayer parameter (the data path the Scene3D config
  # panel's copy/paste toolbar drives).
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/pointcloud_layer_param_transfer_test.cpp
  )

  # Config-panel range coupling: the manual "Range Min" / "Range Max" scrubbers
  # must stay ordered (min <= max) — pushing one past the other drags the other
  # along. Drives the real DoubleScrubber widgets, so it needs a QApplication.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/pointcloud_layer_range_coupling_test.cpp
  )

  # DepthCloudLayer: a depth-encoded sdk::Image + CameraInfo (matched by frame_id)
  # back-project into points via the kImage path; color images opt out; the
  # encoding gate (isDepthEncoding) is pinned.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/depth_cloud_layer_test.cpp
  )

  # Dock-level depth/color gate: Scene3DDockWidget::addTopic accepts a depth-
  # encoded kImage as a DepthCloud layer and refuses a color kImage (the
  # "droppable into 3D" guarantee behind drag-drop absorption). The dock is a
  # QWidget, so this needs the shared QApplication main.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/scene3d_dock_depth_gate_test.cpp
  )

  # M.65: pin PointCloudLayer's per-use parser binding (raw renderAt path still
  # calls parseLocked on the UI thread per tick — a reintroduced cached pointer is
  # a same-file-reload use-after-free). Mirrors occupancy_grid_layer_rebind_test.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    ../tests/pointcloud_layer_rebind_test.cpp
  )

  # RGB-direct colour mode: a cloud carrying per-point colour (foxglove
  # red/green/blue/alpha or a packed rgba field) collapses the channels to a single
  # RGB mode, defaults to it, and round-trips through layout XML.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    ../tests/pointcloud_layer_rgb_test.cpp
  )

  # M.66: drive the latest-wins compressed-decode coalescing state machine
  # (pending_ collapse, wanted_ staleness, failed_id_ memo) over the real
  # QtConcurrent + QFutureWatcher path. Encodes valid fixtures with the Cloudini
  # encoder (PRIVATE in pj_scene3d_core), so it links cloudini explicitly like
  # pointcloud_codecs_test does.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    ../tests/pointcloud_layer_coalescing_test.cpp
  )

  # Layer-level coverage for the '<base>_updates' patch window: UID-based store
  # traversal vs an independent cell-by-cell replay, the retroactive-ingest
  # (behind-live-edge) invalidate path, and the base-keyframe parse memo.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/occupancy_grid_layer_updates_test.cpp
  )

  # VoxelGridLayer behaviour: sample-UID memo (re-scrub does no re-parse — the
  # zero-per-voxel-CPU contract), new-sample re-decode, streaming attach + self-heal,
  # per-use parser rebind, XML round-trip, worldBounds. Mock VoxelGrid parser.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/voxel_grid_layer_test.cpp
  )

  # PosesInFrameLayer: tracker-time decode -> triad expansion, scrub coalescing,
  # size/opacity re-expansion, empty/orphan edges, the per-use parseLocked rebind
  # guard, and the copy/paste/apply-to-family param round-trip (serializeLayerParams).
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/poses_in_frame_layer_test.cpp
  )

  # The per-view budget latch of every budgeted layer, driven by tracker moves
  # and prepareForRender() (QRhi): a rejected sample never blocks a later one
  # that fits.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/layer_budget_latch_test.cpp
  )

  # Camera "follow a frame": the active camera's pivot tracks a followed frame's
  # origin each tracker tick (Position-only), no-jump on enable, gate-safe render
  # key, and per-dock layout persistence. Drives a bare SceneViewWidget against a
  # hand-built TransformBuffer + the dock's public surface; needs a QApplication.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/scene3d_follow_test.cpp
  )

  # TF R6 wiring of a bare SceneViewWidget, admitted without a display
  # (SceneViewTestAccess::admitFrame()): a stamped layer is placed at its own stamp
  # with its placement hold, the TF triads at the cursor in the entry clock.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/scene_view_placement_test.cpp
  )

  # Scene3DDockWidget: persistence, TF binding, fixed frame, and the layers'
  # XML/config surfaces. The view needs no GPU until it is shown.

  # Regression: a TF-only 3D dock (FrameTransforms consumed as config, zero
  # layers) must survive revalidateObjects (H.4), and the TF/dataset binding must
  # reset+rebind when the bound dataset's topics are removed (M.17). Needs the
  # PJ_SCENE3D_TEST_HOOKS accessors and a QApplication (the dock is a QWidget).
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/scene3d_dock_streaming_test.cpp
  )

  # Layers on derived (on-demand) topics: the row warning and lag, the refresh on
  # publication, the inputs' clock range, snapshots and findings. Runs the
  # on-demand engine with pj_runtime's fake script run.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/scene3d_dock_derived_topics_test.cpp
  )

  # Regression: layout persistence round-trips FrameTransforms config topics
  # (M.18), re-resolves dataset identity by stable source name across sessions
  # (M.55), and forces the lazily-created view so a saved explicit fixed frame
  # survives a zero-layer restore (M.19). Needs the PJ_SCENE3D_TEST_HOOKS
  # accessors and a QApplication (the dock is a QWidget).
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/scene3d_dock_persistence_test.cpp
  )

  # The camera's scene bounds the dock pushes to the view: every layer's
  # worldBounds(), hidden ones included, poses none. No GPU (the view is never shown).
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/scene3d_dock_camera_bounds_test.cpp
  )

  # feat: a newly-created 3D dock defaults its fixed frame to the last one the user
  # picked by hand for the same dataset's TransformBuffer (in-session + cross-restart
  # via TransformService). Drives the dock headless through a mock FrameTransforms
  # parser; needs a QApplication (the dock is a QWidget).
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/scene3d_dock_fixed_frame_test.cpp
  )

  # Settings-panel row pitch: every layer's createConfigWidget() must use the
  # same vertical spacing (Space::Snug) as the built-in Grid / Transforms grids
  # in Scene3DConfigPanel, so the panel doesn't visibly loosen when a different
  # topic type is selected. Builds real config widgets, so it needs a QApplication;
  # links pj_resources so PJ::theme::space() resolves real QSS values
  # instead of the -1/style-default fallback that would mask the Snug/Comfortable gap.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/layer_config_widget_row_spacing_test.cpp
  )

  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/scene3d_layer_xml_validation_test.cpp
  )

  # Mouse gestures on a bare SceneViewWidget: the drag mapping, and the self-heal
  # when a right-click context menu's nested QMenu::exec() loop swallows the
  # mouseRelease (see CLAUDE.md).
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/scene_view_widget_mouse_gesture_test.cpp
  )

  # CPU-rasterized HUD panel (perf HUD + TF hover label): correct sizing,
  # DPR-independent logical layout, actual glyph pixels, multi-line growth, and
  # the empty->null contract. Needs a QApplication for the font system; no GPU.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/hud_overlay_test.cpp
  )

  # Regression: a file reload re-registers each surviving topic's parser slot;
  # layers must re-resolve the parser through SessionManager per use instead of
  # caching the raw pointer from attach() (use-after-free on the next tick).
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    ../tests/occupancy_grid_layer_rebind_test.cpp
  )

  # GridMapLayer behaviour: parser AND canonical-codec routes, field auto-pick vs
  # requested-with-fallback, staged upload only on sample/selector change, NaN
  # excluded from the auto range, renderKey without a payload resolve, source-frame
  # and fixed-frame bounds, XML round-trip, dataset replace / detach reset.
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/grid_map_layer_test.cpp
  )

  # Headless TrailLayer unit tests (renderKey coalescing, XML style-only load,
  # sync rebuild + live append against a hand-fed TransformBuffer, the renderer's
  # fixed-frame adoption and async rebuild).
  list(APPEND PJ_SCENE3D_WIDGET_TESTS
    tests/trail_layer_test.cpp
  )

  # The model pass's draw policy, GPU-free: texture colour space and cache
  # key, mesh and placed bounds, visual/collision gating and opacity, blend
  # routing, per-view shading.
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
  # The SceneEntities expansion (MarkerGeometry): unexpanded stream
  # batches, per-segment/per-triangle validation and batch colours. Pure; no GPU.
  list(APPEND PJ_SCENE3D_WIDGET_PLAIN_TESTS
    tests/marker_geometry_test.cpp
  )
  pj_add_test_runner(pj_scene3d_widget_plain_tests
    MAIN gui WARNINGS
    SOURCES ${PJ_SCENE3D_WIDGET_PLAIN_TESTS}
    LIBS pj_scene3d_widgets pj_scene3d_widgets_testing pj_scene_common pj_widgets pj_resources glm::glm Qt6::Network
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
