if(EMSCRIPTEN)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Widgets Xml ShaderTools)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets Xml)
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
    src/PlotMarkersItem.cpp
    src/PlotPanner.cpp
    src/PlotSampleReducer.cpp
    src/PlotScaleDraw.cpp
    src/PlotWidget.cpp
    src/PlotWidgetBase.cpp
    src/PlotZoomer.cpp
    src/RasterTextEngine.cpp
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
    include/pj_plotting/DockToolbar.h
    include/pj_plotting/DockWidget.h
    include/pj_plotting/PlotDocker.h
    include/pj_plotting/PlotFocusOverlay.h
    include/pj_plotting/PlotLegend.h
    include/pj_plotting/PlotMagnifier.h
    include/pj_plotting/PlotMarkersItem.h
    include/pj_plotting/PlotPanner.h
    include/pj_plotting/PlotSampleReducer.h
    include/pj_plotting/PlotScaleDraw.h
    include/pj_plotting/PlotWidget.h
    include/pj_plotting/PlotWidgetBase.h
    include/pj_plotting/PlotZoomer.h
    include/pj_plotting/RasterTextEngine.h
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
    pj_datastore  # DataProcessor (applyBatch preview) — formerly pj_proc
)

if(EMSCRIPTEN)
  target_sources(pj_plotting PRIVATE
    src/PlotRhiCanvas.cpp
    include/pj_plotting/PlotRhiCanvas.h
  )
  if(PJ_WASM_ENABLE_INGRESS_PROBE)
    target_compile_definitions(pj_plotting PRIVATE PJ_WASM_ENABLE_INGRESS_PROBE)
  endif()
  target_link_libraries(pj_plotting PRIVATE Qt6::GuiPrivate)
  qt_add_shaders(pj_plotting pj_plot_rhi_shaders
    PREFIX "/pj_plotting/shaders"
    BASE shaders
    GLSL "300 es,120,150"
    NOHLSL
    NOMSL
    FILES
      shaders/plot_color.vert
      shaders/plot_color.frag
      shaders/plot_text.vert
      shaders/plot_text.frag
  )
else()
  target_link_libraries(pj_plotting PUBLIC Qt6::OpenGLWidgets)
endif()
