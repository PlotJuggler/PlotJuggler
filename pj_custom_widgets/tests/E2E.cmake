# SPDX-License-Identifier: MPL-2.0
# Headless end to end through the real executable (included from pj_app/CMakeLists.txt, where the
# pj_app target exists). The widget library and the SDK's mock file
# source (it provides the data: topic mock/file_data, field value) are found through --plugin-dir.
# The layout restores a Custom Widget wired to that field; the run must exit 0 without an error
# diagnostic. The variant without the widget library must still exit 0 and warn that the widget is
# missing (the element itself is kept; the GUI test of pj_app proves the re-save). after_kill first
# kills a run with the widget live, then checks that the next run reports it (and not the record of
# a process that is still alive) and still restores it.
if(TARGET pj_app AND TARGET custom_widget_test_plugin AND TARGET mock_file_source_plugin)
    get_target_property(PJ_CUSTOM_WIDGET_FIXTURE_DIR custom_widget_test_plugin LIBRARY_OUTPUT_DIRECTORY)
    set(PJ_CUSTOM_WIDGET_DATA_ONLY_DIR "${CMAKE_BINARY_DIR}/pj_custom_widgets/tests/fixture_data_only")
    set(PJ_CUSTOM_WIDGET_TESTS_DIR "${CMAKE_SOURCE_DIR}/pj_custom_widgets/tests")
    add_custom_target(custom_widget_e2e_fixtures ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PJ_CUSTOM_WIDGET_DATA_ONLY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:mock_file_source_plugin> "${PJ_CUSTOM_WIDGET_FIXTURE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:mock_file_source_plugin> "${PJ_CUSTOM_WIDGET_DATA_ONLY_DIR}"
        COMMENT "Staging the mock file source for the Custom Widget end-to-end runs")
    add_dependencies(custom_widget_e2e_fixtures mock_file_source_plugin custom_widget_test_plugin pj_app)

    # Every run redirects the user profile (settings, the extension store and the active-widgets
    # record in AppLocalDataLocation) into its work directory through XDG_CONFIG_HOME,
    # XDG_DATA_HOME and HOME. Only Linux honours those: elsewhere a run would read and write the
    # real user's settings and crash record (a hard kill would leave a record there). So every
    # variant is Linux-only until the app has a profile-directory override.
    set(variants)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(variants with_plugin without_plugin after_kill)
    endif()
    foreach(variant IN LISTS variants)
        if(variant STREQUAL "without_plugin")
            set(plugin_dir "${PJ_CUSTOM_WIDGET_DATA_ONLY_DIR}")
        else()
            set(plugin_dir "${PJ_CUSTOM_WIDGET_FIXTURE_DIR}")
        endif()
        add_test(NAME CustomWidgetE2E.${variant}
            COMMAND ${CMAKE_COMMAND}
                -DAPP=$<TARGET_FILE:pj_app>
                -DLAYOUT=${PJ_CUSTOM_WIDGET_TESTS_DIR}/custom_widget.pj4.xml
                -DPLUGIN_DIR=${plugin_dir}
                -DWORK_DIR=${CMAKE_BINARY_DIR}/pj_custom_widgets/tests/e2e_${variant}
                -DVARIANT=${variant}
                -P ${PJ_CUSTOM_WIDGET_TESTS_DIR}/run_custom_widget_e2e.cmake)
        set_tests_properties(CustomWidgetE2E.${variant} PROPERTIES TIMEOUT 120)
    endforeach()
endif()
