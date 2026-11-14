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
        dock_widget_discard_test.cpp
        dock_fullscreen_test.cpp
        tabbed_widget_tab_test.cpp
        plot_widget_replace_curve_test.cpp
        plot_scale_draw_test.cpp
        plot_widget_default_style_test.cpp
        plot_widget_xy_marker_test.cpp
        plot_widget_global_markers_test.cpp
        plot_widget_marker_status_test.cpp
        curve_editor_global_markers_test.cpp
        legend_text_diagnostics_test.cpp
        plot_widget_legend_toggle_test.cpp
        plot_widget_range_offset_test.cpp
        plot_widget_max_zoom_offset_test.cpp
        plot_widget_flat_curve_range_test.cpp
        plot_widget_first_rows_fit_test.cpp
        plot_magnifier_clamp_test.cpp
        plot_state_transaction_test.cpp
        # Links pj_resources so :/filters/builtin_filters.luau is embedded and
        # auto-registered — SessionManager reads it to install the Luau filter catalogue.
        # Without it the panel would have no filters to list or apply.
        builtin_filter_presentation_test.cpp
        filter_editor_panel_test.cpp
        parameter_form_test.cpp
    LIBS
        pj_plotting
        pj_widgets
        pj_resources  # framework token palette (DataBackdrop test)
        Qt6::Test
)
set_target_properties(pj_plotting_widget_tests PROPERTIES AUTOMOC ON)

# PlotRhiCanvas against Qwt's own rendering, on every QRhi backend of the
# platform plus OpenGL at device-pixel ratio 2 (RHI_BACKENDS, see
# cmake/PjTests.cmake). Needs an RHI-capable QPA (wayland/xcb/windows/cocoa;
# xvfb in Linux CI), never `offscreen`, where every case skips (fails under
# PJ_REQUIRE_GPU_TESTS=1). OpenGLWidgets: a QOpenGLWidget sibling pins a window
# to OpenGL in one case.
find_package(Qt6 REQUIRED COMPONENTS OpenGLWidgets)
pj_add_test_runner(plot_rhi_render_test
    MAIN gui WARNINGS RHI_BACKENDS DPR2
    SOURCES plot_rhi_render_test.cpp
    LIBS pj_plotting pj_widgets pj_resources Qt6::OpenGLWidgets Qt6::Test
    TIMEOUT 60
)
