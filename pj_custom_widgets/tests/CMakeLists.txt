find_package(GTest REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Widget plugin with real Qt, built like a shipped widget library: a SHARED library alone in its own
# directory so --plugin-dir can point at it (the scan dlopens every library it finds there).
add_library(custom_widget_test_plugin SHARED test_widget_plugin.cpp)
target_compile_features(custom_widget_test_plugin PRIVATE cxx_std_20)
target_compile_options(custom_widget_test_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(custom_widget_test_plugin PRIVATE plotjuggler_sdk::plugin_sdk Qt6::Widgets)
set(PJ_CUSTOM_WIDGET_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixture_plugins")
set_target_properties(custom_widget_test_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PJ_CUSTOM_WIDGET_FIXTURE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${PJ_CUSTOM_WIDGET_FIXTURE_DIR}")

pj_add_test_runner(pj_custom_widgets_tests
    MAIN gui
    WARNINGS
    SOURCES
        settings_schema_test.cpp
        wire_sample_test.cpp
        widget_bindings_test.cpp
        custom_widget_dock_test.cpp
    LIBS
        pj_custom_widgets
        pj_custom_widgets_core
        pj_runtime
        plotjuggler_sdk::plugin_sdk
        Qt6::Widgets
)
set_target_properties(pj_custom_widgets_tests PROPERTIES AUTOMOC ON)

# The end-to-end runs of the real executable are registered from pj_app/CMakeLists.txt (E2E.cmake here):
# the pj_app target does not exist yet when this directory is configured.
