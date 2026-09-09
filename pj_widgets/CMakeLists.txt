find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Svg Widgets)

add_library(pj_widgets STATIC
    src/CheckButton.cpp
    src/DualOptionsWidget.cpp
    src/ColorPickerPopup.cpp
    src/ColorPickerWidget.cpp
    src/Colormap.cpp
    src/ComboBox.cpp
    src/ComboBoxGradientDelegate.cpp
    src/ConfigPanelHost.cpp
    src/CredentialsEditor.cpp
    src/CurveTreeView.cpp
    src/DateRangePicker.cpp
    src/Dialog.cpp
    src/Dialog.ui
    src/DoubleScrubber.cpp
    src/FileDialog.cpp
    src/FrameworkTokens.cpp
    src/FlowLayout.cpp
    src/HeaderDividerHighlight.cpp
    src/HeaderResizePolicy.cpp
    src/IngestProgressWidget.cpp
    src/IntScrubber.cpp
    src/LayerListView.cpp
    src/MarkerTimeline.cpp
    src/MessageBox.cpp
    src/Search.cpp
    src/ProgressBar.cpp
    src/ProgressDialog.cpp
    src/RangeSlider.cpp
    src/RasterFrame.cpp
    src/RecentFilesMenu.cpp
    src/ScrubberBase.cpp
    src/SectionHeaderBand.cpp
    src/StateTransitionsView.cpp
    src/SvgButton.cpp
    src/Scrollbar.cpp
    src/Timeline.cpp
    src/ToastManager.cpp
    src/ToastNotification.cpp
    src/ToggleSwitch.cpp
    src/VisualizationPlaceholderWidget.cpp
    include/pj_widgets/CheckButton.h
    include/pj_widgets/ChromeMetrics.h
    include/pj_widgets/DualOptionsWidget.h
    include/pj_widgets/ColorPickerPopup.h
    include/pj_widgets/ColorPickerWidget.h
    include/pj_widgets/Colormap.h
    include/pj_widgets/ComboBox.h
    include/pj_widgets/ComboBoxGradientDelegate.h
    include/pj_widgets/ConfigPanelHost.h
    include/pj_widgets/CredentialsEditor.h
    include/pj_widgets/CurveTreeView.h
    include/pj_widgets/DateRangePicker.h
    include/pj_widgets/Dialog.h
    include/pj_widgets/DoubleScrubber.h
    include/pj_widgets/ElidingLabel.h
    include/pj_widgets/FileDialog.h
    include/pj_widgets/FlowLayout.h
    include/pj_widgets/FrameworkTokens.h
    include/pj_widgets/HeaderDividerHighlight.h
    include/pj_widgets/HeaderResizePolicy.h
    include/pj_widgets/IngestProgressWidget.h
    include/pj_widgets/IntScrubber.h
    include/pj_widgets/LayerListView.h
    include/pj_widgets/MarkerTimeline.h
    include/pj_widgets/MessageBox.h
    include/pj_widgets/Search.h
    include/pj_widgets/ProgressBar.h
    include/pj_widgets/ProgressDialog.h
    include/pj_widgets/RealSlider.h
    include/pj_widgets/RangeSlider.h
    include/pj_widgets/RasterFrame.h
    include/pj_widgets/RecentFilesMenu.h
    include/pj_widgets/ScrubberBase.h
    include/pj_widgets/SectionHeaderBand.h
    include/pj_widgets/StateColors.h
    include/pj_widgets/StateTransitionsView.h
    include/pj_widgets/Style.h
    include/pj_widgets/SvgButton.h
    include/pj_widgets/SvgUtil.h
    include/pj_widgets/Scrollbar.h
    include/pj_widgets/Timeline.h
    include/pj_widgets/ToastManager.h
    include/pj_widgets/ToastNotification.h
    include/pj_widgets/ToggleSwitch.h
    include/pj_widgets/VisualizationPlaceholderWidget.h
)

# The retro helper uses QProcess, QLocalSocket, and QSharedMemory, none of which
# represent a browser capability. Browser rendering stays in-process.
if(NOT EMSCRIPTEN)
    target_sources(pj_widgets PRIVATE
        src/RasterStreamView.cpp
        include/pj_widgets/RasterStreamView.h)
endif()

set_target_properties(pj_widgets PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(pj_widgets PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/raster/ipc>
)

target_link_libraries(pj_widgets PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Svg
    Qt6::Widgets
)

if(PJ_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Both demo trees are pj_widgets demo apps: demos/ (the gallery of standalone
# widget demos) and demo/ (the framework demo). PJ_BUILD_WIDGET_DEMOS governs
# both, so --no-widget-demos removes all of them; demos/ additionally follows
# the global PJ_BUILD_DEMOS like every other module's demos.
option(PJ_BUILD_WIDGET_DEMOS "Build pj_widgets demo apps" ${PJ_BUILD_TESTS})
if(PJ_BUILD_DEMOS AND PJ_BUILD_WIDGET_DEMOS AND NOT EMSCRIPTEN)
    add_subdirectory(demos)
endif()

if(PJ_BUILD_WIDGET_DEMOS)
    add_subdirectory(demo)
endif()
