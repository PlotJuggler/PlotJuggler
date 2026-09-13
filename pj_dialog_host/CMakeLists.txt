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
    add_executable(dialog_engine_test
        tests/dialog_engine_test.cpp
        ${plotjuggler_sdk_SOURCE_DIR}/pj_plugins/dialog_protocol/examples/mock_dialog.cpp
    )
    target_compile_options(dialog_engine_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(dialog_engine_test
        PRIVATE
            pj_dialog_engine_qt
            plotjuggler_sdk::plugin_sdk
            GTest::gtest
            Qt6::Test
    )
    add_test(NAME dialog_engine_test COMMAND dialog_engine_test)
    set_tests_properties(dialog_engine_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(panel_engine_test
        tests/panel_engine_test.cpp
        tests/mock_panel_plugin.cpp
    )
    set_target_properties(panel_engine_test PROPERTIES AUTOMOC ON)
    target_compile_options(panel_engine_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(panel_engine_test
        PRIVATE
            pj_dialog_engine_qt
            plotjuggler_sdk::plugin_sdk
            pj_plotting
            pj_runtime
            GTest::gtest
            GTest::gtest_main
    )
    add_test(NAME panel_engine_test COMMAND panel_engine_test)
    set_tests_properties(panel_engine_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(widget_binding_test
        tests/widget_binding_test.cpp
    )
    set_target_properties(widget_binding_test PROPERTIES AUTOMOC ON)
    target_compile_options(widget_binding_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(widget_binding_test
        PRIVATE
            pj_dialog_engine_qt
            plotjuggler_sdk::plugin_sdk
            GTest::gtest
            GTest::gtest_main
            Qt6::Test
    )
    add_test(NAME widget_binding_test COMMAND widget_binding_test)
    set_tests_properties(widget_binding_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(combo_rules_test
        tests/combo_rules_test.cpp
    )
    set_target_properties(combo_rules_test PROPERTIES AUTOMOC ON)
    target_compile_options(combo_rules_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(combo_rules_test
        PRIVATE
            pj_dialog_engine_qt
            plotjuggler_sdk::plugin_sdk
            GTest::gtest
            GTest::gtest_main
            Qt6::Test
    )
    add_test(NAME combo_rules_test COMMAND combo_rules_test)
    set_tests_properties(combo_rules_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()
