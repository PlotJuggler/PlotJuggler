find_package(Qt6 REQUIRED COMPONENTS Widgets UiTools SvgWidgets)

# Workaround: some Qt packages still publish AGL in transitive OpenGL links,
# but that framework was removed in newer macOS SDKs.
foreach(_target WrapOpenGL::WrapOpenGL OpenGL::GL)
    if(TARGET ${_target})
        get_target_property(_libs ${_target} INTERFACE_LINK_LIBRARIES)
        if(_libs)
            string(REPLACE "-framework AGL" "" _libs "${_libs}")
            list(REMOVE_ITEM _libs "AGL")
            foreach(_lib ${_libs})
                if(_lib MATCHES "AGL")
                    list(REMOVE_ITEM _libs "${_lib}")
                endif()
            endforeach()
            set_target_properties(${_target} PROPERTIES INTERFACE_LINK_LIBRARIES "${_libs}")
        endif()
    endif()
endforeach()

add_library(pj_dialog_engine_qt STATIC
    src/widget_binding.cpp
    src/widget_adapters.cpp
    src/dialog_engine.cpp
    src/panel_engine.cpp
    src/pj_ui_loader.cpp
    src/chart_preview_widget.cpp
    src/chart_placeholder_overlay.hpp
    include/pj_plugins/host_qt/chart_preview_widget.hpp
    include/pj_plugins/host_qt/dialog_engine.hpp
    include/pj_plugins/host_qt/drop_event_filter.hpp
    include/pj_plugins/host_qt/panel_engine.hpp
    include/pj_plugins/host_qt/pj_ui_loader.hpp
    include/pj_plugins/host_qt/widget_adapters.hpp
    include/pj_plugins/host_qt/widget_binding.hpp
)

set_target_properties(pj_dialog_engine_qt PROPERTIES
    AUTOMOC ON
)

target_compile_features(pj_dialog_engine_qt PUBLIC cxx_std_20)
target_compile_options(pj_dialog_engine_qt PRIVATE ${PJ_WARNING_FLAGS})

target_include_directories(pj_dialog_engine_qt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(pj_dialog_engine_qt
    PUBLIC
        plotjuggler_sdk::plugin_host
        pj_widgets
        plotjuggler_qwt   # backs ChartPreviewWidget (replaced Qt6::Charts)
        Qt6::Widgets
        Qt6::UiTools
        Qt6::SvgWidgets
    PRIVATE
        pj_plotting       # PlotWidget for embedded chart containers
        pj_runtime        # AppSession + CatalogModel
)

if(PJ_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    # Two runners on the shared gui main rather than one executable per file.
    # mock_dialog.cpp and mock_panel_plugin.cpp both define the plugin entry point
    # PJ_get_dialog_vtable, and each engine test calls it expecting its own mock,
    # so those two cannot share a binary.
    pj_add_test_runner(pj_dialog_host_tests MAIN gui WARNINGS
        SOURCES
            tests/dialog_engine_test.cpp
            ${plotjuggler_sdk_SOURCE_DIR}/pj_plugins/dialog_protocol/examples/mock_dialog.cpp
            tests/widget_binding_test.cpp
            tests/combo_rules_test.cpp
        LIBS pj_dialog_engine_qt plotjuggler_sdk::plugin_sdk Qt6::Test
    )
    set_target_properties(pj_dialog_host_tests PROPERTIES AUTOMOC ON)

    pj_add_test_runner(pj_dialog_host_panel_tests MAIN gui WARNINGS
        SOURCES
            tests/panel_engine_test.cpp
            tests/mock_panel_plugin.cpp
        LIBS pj_dialog_engine_qt plotjuggler_sdk::plugin_sdk pj_plotting pj_runtime
    )
    set_target_properties(pj_dialog_host_panel_tests PROPERTIES AUTOMOC ON)
endif()
