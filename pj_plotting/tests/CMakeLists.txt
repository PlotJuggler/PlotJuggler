find_package(GTest REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test)

# no WARNINGS: 3 of 3 merged tests never compiled with -Werror
pj_add_test_runner(pj_plotting_core_tests
    MAIN core
    SOURCES
        datastore_curve_adapter_test.cpp
        state_series_adapter_test.cpp
        filtered_curve_adapter_test.cpp
    LIBS pj_plotting_core
)

# no WARNINGS: 2 of 20 merged tests never compiled with -Werror
pj_add_test_runner(pj_plotting_widget_tests
    MAIN gui
    SOURCES
        plot_sample_reducer_test.cpp
        # GUI test: the controller drives a real StateTransitionsView (QApplication).
        state_transitions_controller_test.cpp
        # GUI test: dock-widget persistence + drop seeding (QApplication).
        state_transitions_dock_widget_test.cpp
        dock_widget_placeholder_test.cpp
        dock_widget_context_menu_test.cpp
        dock_fullscreen_test.cpp
        tabbed_widget_tab_test.cpp
        plot_widget_replace_curve_test.cpp
        plot_scale_draw_test.cpp
        plot_widget_default_style_test.cpp
        plot_widget_xy_marker_test.cpp
        plot_widget_global_markers_test.cpp
        curve_editor_global_markers_test.cpp
        legend_text_diagnostics_test.cpp
        raster_text_engine_test.cpp
        plot_widget_range_offset_test.cpp
        plot_widget_max_zoom_offset_test.cpp
        plot_widget_flat_curve_range_test.cpp
        plot_magnifier_clamp_test.cpp
        plot_state_transaction_test.cpp
        # Links pj_resources so :/filters/builtin_filters.luau is embedded and
        # auto-registered — SessionManager reads it to install the Luau filter catalogue.
        # Without it the panel would have no filters to list or apply.
        filter_editor_panel_test.cpp
        parameter_form_test.cpp
    LIBS
        pj_plotting
        pj_widgets
        pj_resources  # framework token palette (DataBackdrop test)
        Qt6::Test
)
set_target_properties(pj_plotting_widget_tests PROPERTIES AUTOMOC ON)

# These regressions need QApplication on a real GL platform and force the GL
# canvas. Keep their startup separate from the offscreen-capable GUI runner
# and its raster-only test environments.
add_executable(plot_canvas_context_recreation_gl_test
    plot_canvas_context_recreation_gl_test.cpp
)

target_compile_options(plot_canvas_context_recreation_gl_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(plot_canvas_context_recreation_gl_test PRIVATE
    pj_plotting
    GTest::gtest
)

add_test(NAME PlotCanvasContextRecreationGlTest COMMAND plot_canvas_context_recreation_gl_test)

add_executable(raster_text_gl_test
    raster_text_gl_test.cpp
)

target_compile_options(raster_text_gl_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(raster_text_gl_test PRIVATE
    pj_plotting
    GTest::gtest
)

add_test(NAME RasterTextGlTest COMMAND raster_text_gl_test)
