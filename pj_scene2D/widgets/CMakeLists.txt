# ---------------------------------------------------------------------------
# pj_scene2d_widgets — Qt widget layer for media display (requires the configured Qt floor)
# ---------------------------------------------------------------------------

if(NOT DEFINED PJ_QT_REQUIRED_VERSION)
  if(DEFINED PJ_QT_VERSION)
    set(PJ_QT_REQUIRED_VERSION ${PJ_QT_VERSION})
  else()
    set(PJ_QT_REQUIRED_VERSION 6.11.1)
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
  # The browser backend needs GLSL ES 300 inside every serialized shader pack.
  # Re-bake to the build tree and compare hashes at configure time so CI catches
  # both a missing WebGL2 dialect and a source/blob mismatch.
  if(NOT TARGET Qt6::qsb)
    message(FATAL_ERROR "WASM Scene2D shader verification requires Qt6::qsb")
  endif()
  get_target_property(_pj_scene2d_qsb Qt6::qsb IMPORTED_LOCATION)
  if(NOT _pj_scene2d_qsb OR _pj_scene2d_qsb MATCHES "-NOTFOUND$")
    get_target_property(_pj_scene2d_qsb Qt6::qsb IMPORTED_LOCATION_RELWITHDEBINFO)
  endif()
  if(NOT EXISTS "${_pj_scene2d_qsb}")
    message(FATAL_ERROR "WASM Scene2D cannot locate the host qsb executable")
  endif()

  set(_pj_scene2d_shader_verify_dir "${CMAKE_CURRENT_BINARY_DIR}/verified_shaders")
  file(MAKE_DIRECTORY "${_pj_scene2d_shader_verify_dir}")
  foreach(_pj_scene2d_shader IN ITEMS
      scene_lines.vert scene_lines.frag
      scene_quads.vert scene_quads.frag
      scene_text.vert scene_text.frag
      yuv_to_rgb.vert yuv_to_rgb.frag)
    if(_pj_scene2d_shader MATCHES "^yuv_to_rgb\\.")
      # Preserve the image pass's existing desktop GLSL 440 target while
      # adding only the WebGL2 dialect needed by WASM.
      set(_pj_scene2d_glsl_targets "300 es,440")
    else()
      # Preserve the overlay packs' existing targets and add WebGL2.
      set(_pj_scene2d_glsl_targets "100 es,120,150,300 es")
    endif()
    set(_pj_scene2d_shader_source
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_pj_scene2d_shader}")
    set(_pj_scene2d_shader_pack "${_pj_scene2d_shader_source}.qsb")
    set(_pj_scene2d_shader_rebuilt
        "${_pj_scene2d_shader_verify_dir}/${_pj_scene2d_shader}.qsb")
    execute_process(
      COMMAND "${_pj_scene2d_qsb}"
        --glsl "${_pj_scene2d_glsl_targets}" --hlsl 50 --msl 12
        -o "${_pj_scene2d_shader_rebuilt}" "${_pj_scene2d_shader_source}"
      RESULT_VARIABLE _pj_scene2d_qsb_result
      ERROR_VARIABLE _pj_scene2d_qsb_error)
    if(NOT _pj_scene2d_qsb_result EQUAL 0)
      message(FATAL_ERROR
        "Failed to bake ${_pj_scene2d_shader}: ${_pj_scene2d_qsb_error}")
    endif()
    file(SHA256 "${_pj_scene2d_shader_pack}" _pj_scene2d_committed_hash)
    file(SHA256 "${_pj_scene2d_shader_rebuilt}" _pj_scene2d_rebuilt_hash)
    if(NOT _pj_scene2d_committed_hash STREQUAL _pj_scene2d_rebuilt_hash)
      message(FATAL_ERROR
        "Stale Scene2D shader pack ${_pj_scene2d_shader_pack}; re-bake with the documented qsb command")
    endif()
  endforeach()
  unset(_pj_scene2d_committed_hash)
  unset(_pj_scene2d_glsl_targets)
  unset(_pj_scene2d_qsb)
  unset(_pj_scene2d_qsb_error)
  unset(_pj_scene2d_qsb_result)
  unset(_pj_scene2d_rebuilt_hash)
  unset(_pj_scene2d_shader)
  unset(_pj_scene2d_shader_pack)
  unset(_pj_scene2d_shader_rebuilt)
  unset(_pj_scene2d_shader_source)
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
endif()
