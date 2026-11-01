find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Xml)

add_library(pj_custom_widgets STATIC
    src/CustomWidgetDockWidget.cpp
    src/MissingWidgetPlaceholder.cpp
    include/pj_custom_widgets/CustomWidgetDockWidget.h
    include/pj_custom_widgets/MissingWidgetPlaceholder.h
)

set_target_properties(pj_custom_widgets PROPERTIES AUTOMOC ON)

target_include_directories(pj_custom_widgets PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(pj_custom_widgets PUBLIC cxx_std_20)
target_compile_options(pj_custom_widgets PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(pj_custom_widgets PUBLIC
    pj_custom_widgets_core
    pj_runtime
    pj_datastore
    pj_widgets
    plotjuggler_sdk::plugin_host
    Qt6::Widgets
    Qt6::Xml
)
