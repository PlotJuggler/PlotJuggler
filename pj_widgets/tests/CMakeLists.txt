find_package(Qt6 REQUIRED COMPONENTS Test)

# Each discovered case runs in its own process, keeping widget state isolated
# while sharing the static module and resource objects in one executable.
# WARNINGS: all 32 merged tests carried PJ_WARNING_FLAGS before
pj_add_test_runner(pj_widgets_tests
    MAIN gui WARNINGS
    SOURCES
        curve_tree_view_test.cpp
        svg_button_test.cpp
        file_dialog_test.cpp
        credentials_editor_test.cpp
        raster_frame_test.cpp
        layer_list_view_test.cpp
        scrubber_base_test.cpp
        range_slider_test.cpp
        coalescing_trigger_test.cpp
        check_button_test.cpp
        dual_options_widget_test.cpp
        eliding_label_test.cpp
        toggle_switch_test.cpp
        color_picker_widget_test.cpp
        style_input_height_test.cpp
        marker_timeline_test.cpp
        progress_bar_test.cpp
        colormap_test.cpp
        messagebox_test.cpp
        ingest_progress_widget_test.cpp
        # Headless: StateColors is constexpr hash/palette math (QColor only at the rim).
        state_colors_test.cpp
        # Headless: StateTransitionsScene is display-free math (no QApplication).
        state_transitions_scene_test.cpp
        # Headless: TimelineScene is Qt-free math (no QApplication). Links pj_widgets
        # only for the Timeline.h include path / TimelineScene symbols.
        timeline_scene_test.cpp
        # Widget test: needs QApplication (xvfb under CI).
        state_transitions_view_test.cpp
        # Widget test: needs QApplication (xvfb under CI).
        timeline_widget_test.cpp
        toast_test.cpp
        # Drift guard: C++ FrameworkTokens vs the QSS palette blocks. Links the qrc so
        # the stylesheets are readable as `:/resources/...` at runtime.
        framework_tokens_test.cpp
        scrollbar_test.cpp
        search_test.cpp
        section_header_band_test.cpp
        header_divider_highlight_test.cpp
        header_resize_policy_test.cpp
    # The layout margins/spacing + button wrap budget now resolve from the framework
    # palette (pj_widgets FrameworkTokens), so the stylesheet resource must be linked.
    LIBS pj_widgets pj_resources Qt6::Test
)
set_target_properties(pj_widgets_tests PROPERTIES AUTOMOC ON)

# Protocol peer for the stream-view test: publishes one frame, no engine.
add_executable(raster_stub_helper raster_stub_helper.cpp)
target_include_directories(raster_stub_helper PRIVATE ${CMAKE_SOURCE_DIR}/raster/ipc)
target_link_libraries(raster_stub_helper PRIVATE Qt6::Core Qt6::Network)

# Keep the helper-process lifecycle assertion and its target-specific helper path
# isolated from the widget runner.
add_executable(raster_stream_view_test raster_stream_view_test.cpp)
set_target_properties(raster_stream_view_test PROPERTIES AUTOMOC ON)
target_link_libraries(raster_stream_view_test PRIVATE pj_widgets Qt6::Test GTest::gtest)
target_compile_definitions(raster_stream_view_test PRIVATE
    PJ_STUB_HELPER_PATH="$<TARGET_FILE:raster_stub_helper>")
add_dependencies(raster_stream_view_test raster_stub_helper)
add_test(NAME RasterStreamView COMMAND raster_stream_view_test)
