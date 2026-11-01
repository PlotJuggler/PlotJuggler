find_package(Qt6 REQUIRED COMPONENTS Core Xml)

add_library(pj_custom_widgets_core STATIC
    src/SettingsSchema.cpp
    src/WidgetBindings.cpp
    src/WireSample.cpp
    include/pj_custom_widgets/SettingsSchema.h
    include/pj_custom_widgets/WidgetBindings.h
    include/pj_custom_widgets/WireSample.h
)

target_include_directories(pj_custom_widgets_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(pj_custom_widgets_core PUBLIC cxx_std_20)
target_compile_options(pj_custom_widgets_core PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(pj_custom_widgets_core PUBLIC
    pj_runtime
    pj_datastore
    pj_fnv1a
    plotjuggler_sdk::base
    plotjuggler_sdk::plugin_host
    Qt6::Core
    Qt6::Xml
)
