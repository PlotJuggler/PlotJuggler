# ---------------------------------------------------------------------------
# pj_scene2d_widgets — Qt widget layer for media display (requires the configured Qt floor)
# ---------------------------------------------------------------------------

if(NOT DEFINED PJ_QT_REQUIRED_VERSION)
  if(DEFINED PJ_QT_VERSION)
    set(PJ_QT_REQUIRED_VERSION ${PJ_QT_VERSION})
  else()
    set(PJ_QT_REQUIRED_VERSION 6.12.0)
  endif()
endif()

# GuiPrivate (for QRhi private headers used by MediaViewerWidget) must be
# requested in THIS scope: Qt 6.11 no longer leaks the Qt6::GuiPrivate imported
# target in from another subdirectory's find_package the way 6.8 did.
find_package(Qt6 ${PJ_QT_REQUIRED_VERSION} QUIET COMPONENTS Widgets Gui GuiPrivate)
if(NOT Qt6_FOUND)
  message(STATUS "pj_scene2d_widgets: Qt ${PJ_QT_REQUIRED_VERSION}+ not found, skipping")
  return()
endif()
message(STATUS "pj_scene2d_widgets: using Qt ${Qt6_VERSION}")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(EMSCRIPTEN)
  # The packs are committed (desktop kits ship no qsb). The browser configure has
  # the host qsb: re-bake with shaders/bake.sh, the single source of the qsb
  # targets, and require identical bytes. That catches both a missing WebGL2
  # dialect and a source/blob mismatch.
  include(${CMAKE_SOURCE_DIR}/cmake/PjQsb.cmake)
  pj_find_host_qsb(_pj_scene2d_qsb "WASM Scene2D")

  set(_pj_scene2d_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
  set(_pj_scene2d_shader_verify_dir "${CMAKE_CURRENT_BINARY_DIR}/verified_shaders")
  file(REMOVE_RECURSE "${_pj_scene2d_shader_verify_dir}")
  file(MAKE_DIRECTORY "${_pj_scene2d_shader_verify_dir}")
  execute_process(
    COMMAND bash "${_pj_scene2d_shader_dir}/bake.sh" "${_pj_scene2d_qsb}" "${_pj_scene2d_shader_verify_dir}"
    RESULT_VARIABLE _pj_scene2d_qsb_result
    ERROR_VARIABLE _pj_scene2d_qsb_error)
  if(NOT _pj_scene2d_qsb_result EQUAL 0)
    message(FATAL_ERROR "Failed to bake the Scene2D shaders: ${_pj_scene2d_qsb_error}")
  endif()
  file(GLOB _pj_scene2d_rebuilt_packs RELATIVE "${_pj_scene2d_shader_verify_dir}"
    "${_pj_scene2d_shader_verify_dir}/*.qsb")
  file(GLOB _pj_scene2d_committed_packs RELATIVE "${_pj_scene2d_shader_dir}" "${_pj_scene2d_shader_dir}/*.qsb")
  if(NOT _pj_scene2d_committed_packs STREQUAL _pj_scene2d_rebuilt_packs)
    message(FATAL_ERROR "Scene2D shader packs out of sync with shaders/bake.sh: committed "
      "[${_pj_scene2d_committed_packs}], baked [${_pj_scene2d_rebuilt_packs}]")
  endif()
  foreach(_pj_scene2d_pack IN LISTS _pj_scene2d_rebuilt_packs)
    file(SHA256 "${_pj_scene2d_shader_dir}/${_pj_scene2d_pack}" _pj_scene2d_committed_hash)
    file(SHA256 "${_pj_scene2d_shader_verify_dir}/${_pj_scene2d_pack}" _pj_scene2d_rebuilt_hash)
    if(NOT _pj_scene2d_committed_hash STREQUAL _pj_scene2d_rebuilt_hash)
      message(FATAL_ERROR "Stale Scene2D shader pack ${_pj_scene2d_pack}; re-bake with shaders/bake.sh")
    endif()
  endforeach()
  # Re-check when bake.sh, a source or a committed pack changes, or one appears.
  file(GLOB _pj_scene2d_shader_inputs CONFIGURE_DEPENDS
    "${_pj_scene2d_shader_dir}/*.vert" "${_pj_scene2d_shader_dir}/*.frag" "${_pj_scene2d_shader_dir}/*.qsb")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_pj_scene2d_shader_dir}/bake.sh" ${_pj_scene2d_shader_inputs})
  unset(_pj_scene2d_committed_hash)
  unset(_pj_scene2d_committed_packs)
  unset(_pj_scene2d_pack)
  unset(_pj_scene2d_qsb)
  unset(_pj_scene2d_qsb_error)
  unset(_pj_scene2d_qsb_result)
  unset(_pj_scene2d_rebuilt_hash)
  unset(_pj_scene2d_rebuilt_packs)
  unset(_pj_scene2d_shader_dir)
  unset(_pj_scene2d_shader_inputs)
  unset(_pj_scene2d_shader_verify_dir)
endif()

# --- pj_scene2d_widgets: image viewer (QRhiWidget), no mpv dependency ---
set(PJ_SCENE2D_WIDGETS_SOURCES
  src/Scene2DDockWidget.cpp
  src/layers/depth_image_layer.cpp
  src/layers/image_layer.cpp
  src/layers/scene2d_layer.cpp
  src/layers/scene_decoder_layer.cpp
  src/media_viewer_widget.cpp
  src/pixel_inspector.cpp
  src/scene2d_pipelines.cpp
  include/pj_scene2d_widgets/Scene2DDockWidget.h
  include/pj_scene2d_widgets/layers/depth_image_layer.h
  include/pj_scene2d_widgets/layers/image_layer.h
  include/pj_scene2d_widgets/layers/scene2d_layer.h
  include/pj_scene2d_widgets/layers/scene_decoder_layer.h
  include/pj_scene2d_widgets/media_viewer_widget.h
  include/pj_scene2d_widgets/pixel_inspector.h
  include/pj_scene2d_widgets/scene2d_pipelines.h
  shaders/shaders.qrc
)

get_target_property(_pj_scene2d_core_public_defs pj_scene2d_core INTERFACE_COMPILE_DEFINITIONS)
set(PJ_SCENE2D_WIDGETS_HAS_FFMPEG FALSE)
if(_pj_scene2d_core_public_defs AND "PJ_HAS_FFMPEG" IN_LIST _pj_scene2d_core_public_defs)
  set(PJ_SCENE2D_WIDGETS_HAS_FFMPEG TRUE)
  list(APPEND PJ_SCENE2D_WIDGETS_SOURCES
    src/layers/video_layer.cpp
    include/pj_scene2d_widgets/layers/video_layer.h
  )
endif()

add_library(pj_scene2d_widgets STATIC ${PJ_SCENE2D_WIDGETS_SOURCES})
target_include_directories(pj_scene2d_widgets PUBLIC include)
target_compile_features(pj_scene2d_widgets PUBLIC cxx_std_20)
target_compile_options(pj_scene2d_widgets PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_scene2d_widgets
  PUBLIC pj_scene_common pj_scene2d_core pj_runtime pj_widgets Qt6::Widgets Qt6::Gui Qt6::GuiPrivate
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
  # Dispatch-table unit test: verifies makeScene2DPipelineFor() returns the right
  # built-in pipeline (or nullptr) per object type. Does not construct the
  # widget, so it does not require an RHI context.
  add_executable(scene2d_dock_widget_dispatch_test
    ../tests/scene2d_dock_widget_dispatch_test.cpp
  )
  target_link_libraries(scene2d_dock_widget_dispatch_test PRIVATE
    pj_scene2d_widgets
    GTest::gtest
  )
  target_compile_options(scene2d_dock_widget_dispatch_test PRIVATE ${PJ_WARNING_FLAGS})
  add_test(NAME scene2d_dock_widget_dispatch_test COMMAND scene2d_dock_widget_dispatch_test)

  add_executable(pixel_inspector_test
    ../tests/pixel_inspector_test.cpp
  )
  target_link_libraries(pixel_inspector_test PRIVATE
    pj_scene2d_widgets
    GTest::gtest_main
  )
  target_compile_options(pixel_inspector_test PRIVATE ${PJ_WARNING_FLAGS})
  add_test(NAME PixelInspector COMMAND pixel_inspector_test)

  add_executable(scene2d_dock_widget_test
    ../tests/scene2d_dock_widget_test.cpp
  )
  target_link_libraries(scene2d_dock_widget_test PRIVATE
    pj_scene2d_widgets
    GTest::gtest
  )
  target_compile_options(scene2d_dock_widget_test PRIVATE ${PJ_WARNING_FLAGS})
  add_test(NAME scene2d_dock_widget_test COMMAND scene2d_dock_widget_test)

  # Pixel checks of the viewer, a QRhi display runner: it needs an RHI-capable QPA
  # (never `offscreen`) and registers once per backend of the platform
  # (RHI_BACKENDS, cmake/PjTests.cmake).
  if(NOT EMSCRIPTEN)
    find_package(Qt6 ${PJ_QT_REQUIRED_VERSION} REQUIRED COMPONENTS Test)
    pj_add_test_runner(media_viewer_render_test
      MAIN gui WARNINGS RHI_BACKENDS
      SOURCES ../tests/media_viewer_render_test.cpp
      LIBS pj_scene2d_widgets Qt6::Test
      TIMEOUT 60
    )
  endif()
endif()
