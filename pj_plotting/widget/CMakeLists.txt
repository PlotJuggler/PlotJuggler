if(EMSCRIPTEN)
    # ShaderTools: the host qsb that verifies the committed shader packs.
    find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Widgets Xml ShaderTools)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Widgets Xml)
endif()

add_library(pj_plotting STATIC
    src/CurveEditor.cpp
    src/CurveEditor.ui
    src/FilterEditorPanel.cpp
    src/FilterEditorPanel.ui
    src/ParameterForm.cpp
    src/CurveTracker.cpp
    src/DockToolbar.cpp
    src/DockToolbar.ui
    src/DockWidget.cpp
    src/PlotCurve.cpp
    src/PlotDocker.cpp
    src/PlotFocusOverlay.cpp
    src/PlotLegend.cpp
    src/PlotMagnifier.cpp
    src/MarkerPainter.cpp
    src/PlotMarkersItem.cpp
    src/PlotPanner.cpp
    src/PlotRhiCanvas.cpp
    src/PlotRhiGeometry.cpp
    src/PlotRhiGeometry.h
    src/PlotRhiTextAtlas.cpp
    src/PlotRhiTextAtlas.h
    src/PlotSampleReducer.cpp
    src/PlotScaleDraw.cpp
    src/PlotWidget.cpp
    src/PlotWidgetBase.cpp
    src/PlotZoomer.cpp
    src/StateTransitionsController.cpp
    src/StateTransitionsDockWidget.cpp
    src/TabbedPlotWidget.cpp
    src/WidgetClipboard.cpp
    src/XYCurveDialog.cpp
    src/XYCurveDialog.ui
    include/pj_plotting/CurveEditor.h
    include/pj_plotting/PlotCurve.h
    include/pj_plotting/XYCurveDialog.h
    include/pj_plotting/FilterEditorPanel.h
    include/pj_plotting/ParameterForm.h
    include/pj_plotting/CurveTracker.h
    include/pj_plotting/CustomWidgetSupport.h
    include/pj_plotting/DockToolbar.h
    include/pj_plotting/DockWidget.h
    include/pj_plotting/PlotDocker.h
    include/pj_plotting/PlotFocusOverlay.h
    include/pj_plotting/PlotLegend.h
    include/pj_plotting/PlotMagnifier.h
    include/pj_plotting/MarkerPainter.h
    include/pj_plotting/PlotMarkersItem.h
    include/pj_plotting/PlotPanner.h
    include/pj_plotting/PlotRhiCanvas.h
    include/pj_plotting/PlotSampleReducer.h
    include/pj_plotting/PlotScaleDraw.h
    include/pj_plotting/PlotWidget.h
    include/pj_plotting/PlotWidgetBase.h
    include/pj_plotting/PlotZoomer.h
    include/pj_plotting/StateTransitionsController.h
    include/pj_plotting/StateTransitionsDockWidget.h
    include/pj_plotting/TabbedPlotWidget.h
)

set_target_properties(pj_plotting PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

target_include_directories(pj_plotting PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(pj_plotting PUBLIC
    pj_plotting_core
    pj_runtime
    plotjuggler_qwt
    qt_advanced_docking
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Xml
    pj_scripting  # PUBLIC: public headers (FilterEditorPanel.h, ParameterForm.h) expose pj_scripting types
    pj_widgets    # PUBLIC: XYCurveDialog.h (a public header) derives from PJ::Dialog
  PRIVATE
    pj_datastore  # DataProcessor (applyBatch preview)
)

target_sources(pj_plotting PRIVATE shaders/pj_plot_rhi_shaders.qrc)
target_link_libraries(pj_plotting PRIVATE Qt6::GuiPrivate)

if(EMSCRIPTEN)
  if(PJ_WASM_ENABLE_INGRESS_PROBE)
    target_compile_definitions(pj_plotting PRIVATE PJ_WASM_ENABLE_INGRESS_PROBE)
  endif()
  # The packs are committed (desktop kits ship no qsb). The browser configure
  # has the host qsb: re-bake with shaders/bake.sh and require identical bytes.
  include(${CMAKE_SOURCE_DIR}/cmake/PjQsb.cmake)
  pj_find_host_qsb(_pj_plot_qsb "WASM plot canvas")
  set(_pj_plot_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
  set(_pj_plot_shader_verify_dir "${CMAKE_CURRENT_BINARY_DIR}/verified_shaders")
  file(MAKE_DIRECTORY "${_pj_plot_shader_verify_dir}")
  execute_process(
    COMMAND bash "${_pj_plot_shader_dir}/bake.sh" "${_pj_plot_qsb}" "${_pj_plot_shader_verify_dir}"
    RESULT_VARIABLE _pj_plot_qsb_result
    ERROR_VARIABLE _pj_plot_qsb_error)
  if(NOT _pj_plot_qsb_result EQUAL 0)
    message(FATAL_ERROR "Failed to bake the plot shaders: ${_pj_plot_qsb_error}")
  endif()
  file(GLOB _pj_plot_sources RELATIVE "${_pj_plot_shader_dir}" CONFIGURE_DEPENDS
    "${_pj_plot_shader_dir}/*.vert" "${_pj_plot_shader_dir}/*.frag")
  foreach(_pj_plot_source IN LISTS _pj_plot_sources)
    file(SHA256 "${_pj_plot_shader_dir}/${_pj_plot_source}.qsb" _pj_plot_committed_hash)
    file(SHA256 "${_pj_plot_shader_verify_dir}/${_pj_plot_source}.qsb" _pj_plot_rebuilt_hash)
    if(NOT _pj_plot_committed_hash STREQUAL _pj_plot_rebuilt_hash)
      message(FATAL_ERROR "Stale plot shader pack ${_pj_plot_source}.qsb; re-bake with widget/shaders/bake.sh")
    endif()
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pj_plot_shader_dir}/bake.sh")
  unset(_pj_plot_committed_hash)
  unset(_pj_plot_qsb)
  unset(_pj_plot_qsb_error)
  unset(_pj_plot_qsb_result)
  unset(_pj_plot_rebuilt_hash)
  unset(_pj_plot_shader_dir)
  unset(_pj_plot_shader_verify_dir)
  unset(_pj_plot_source)
  unset(_pj_plot_sources)
endif()
