find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(curve_tree_view_test
    curve_tree_view_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_link_libraries(curve_tree_view_test PRIVATE
    pj_widgets
    GTest::gtest
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(curve_tree_view_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(curve_tree_view_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME CurveTreeViewTest COMMAND curve_tree_view_test)

add_executable(svg_button_test
    svg_button_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_link_libraries(svg_button_test PRIVATE
    pj_widgets
    GTest::gtest_main
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(svg_button_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(svg_button_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME SvgButtonTest COMMAND svg_button_test)

add_executable(file_dialog_test
    file_dialog_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_link_libraries(file_dialog_test PRIVATE
    pj_widgets
    GTest::gtest
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(file_dialog_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(file_dialog_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME FileDialogTest COMMAND file_dialog_test)

add_executable(credentials_editor_test
    credentials_editor_test.cpp
)

target_link_libraries(credentials_editor_test PRIVATE
    pj_widgets
    GTest::gtest
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(credentials_editor_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(credentials_editor_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME CredentialsEditorTest COMMAND credentials_editor_test)

find_package(Qt6 REQUIRED COMPONENTS Test)

# Protocol peer for the stream-view test: publishes one frame, no engine.
add_executable(raster_stub_helper raster_stub_helper.cpp)
target_include_directories(raster_stub_helper PRIVATE ${CMAKE_SOURCE_DIR}/raster/ipc)
target_link_libraries(raster_stub_helper PRIVATE Qt6::Core Qt6::Network)
set_target_properties(raster_stub_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(raster_frame_test raster_frame_test.cpp)
target_link_libraries(raster_frame_test PRIVATE pj_widgets GTest::gtest_main)
if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(raster_frame_test PRIVATE ${PJ_WARNING_FLAGS})
endif()
set_target_properties(raster_frame_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME RasterFrame COMMAND raster_frame_test)

add_executable(raster_stream_view_test raster_stream_view_test.cpp)
set_target_properties(raster_stream_view_test PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(raster_stream_view_test PRIVATE pj_widgets Qt6::Test GTest::gtest)
target_compile_definitions(raster_stream_view_test PRIVATE
    PJ_STUB_HELPER_PATH="$<TARGET_FILE:raster_stub_helper>")
add_dependencies(raster_stream_view_test raster_stub_helper)
add_test(NAME RasterStreamView COMMAND raster_stream_view_test)

add_executable(layer_list_view_test
    layer_list_view_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_link_libraries(layer_list_view_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(layer_list_view_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(layer_list_view_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME LayerListViewTest COMMAND layer_list_view_test)

add_executable(scrubber_base_test scrubber_base_test.cpp)

target_link_libraries(scrubber_base_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(scrubber_base_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(scrubber_base_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME ScrubberBase COMMAND scrubber_base_test)

add_executable(range_slider_test range_slider_test.cpp)

target_link_libraries(range_slider_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(range_slider_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(range_slider_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME RangeSliderTest COMMAND range_slider_test)

add_executable(coalescing_trigger_test coalescing_trigger_test.cpp)

target_link_libraries(coalescing_trigger_test PRIVATE
    pj_widgets
    GTest::gtest
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(coalescing_trigger_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(coalescing_trigger_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME CoalescingTrigger COMMAND coalescing_trigger_test)

add_executable(check_button_test
    check_button_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_link_libraries(check_button_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(check_button_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(check_button_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME CheckButton COMMAND check_button_test)

add_executable(dual_options_widget_test
    dual_options_widget_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_link_libraries(dual_options_widget_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(dual_options_widget_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(dual_options_widget_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME DualOptionsWidget COMMAND dual_options_widget_test)

add_executable(eliding_label_test eliding_label_test.cpp)

target_link_libraries(eliding_label_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(eliding_label_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(eliding_label_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME ElidingLabel COMMAND eliding_label_test)

add_executable(toggle_switch_test toggle_switch_test.cpp)

target_link_libraries(toggle_switch_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(toggle_switch_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(toggle_switch_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME ToggleSwitch COMMAND toggle_switch_test)

add_executable(color_picker_widget_test color_picker_widget_test.cpp)

target_link_libraries(color_picker_widget_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(color_picker_widget_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(color_picker_widget_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME ColorPickerWidget COMMAND color_picker_widget_test)

add_executable(style_input_height_test style_input_height_test.cpp)

target_link_libraries(style_input_height_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(style_input_height_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(style_input_height_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME StyleInputHeight COMMAND style_input_height_test)

add_executable(marker_timeline_test marker_timeline_test.cpp)

target_link_libraries(marker_timeline_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(marker_timeline_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(marker_timeline_test PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME MarkerTimeline COMMAND marker_timeline_test)

add_executable(progress_bar_test progress_bar_test.cpp)

target_link_libraries(progress_bar_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(progress_bar_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(progress_bar_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME ProgressBar COMMAND progress_bar_test)

add_executable(colormap_test colormap_test.cpp)

target_link_libraries(colormap_test PRIVATE
    pj_widgets
    GTest::gtest_main
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(colormap_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(colormap_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME Colormap COMMAND colormap_test)

add_executable(messagebox_test
    messagebox_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_link_libraries(messagebox_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(messagebox_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

# The layout margins/spacing + button wrap budget now resolve from the framework
# palette (pj_widgets FrameworkTokens), so the stylesheet resource must be linked.
set_target_properties(messagebox_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME MessageBox COMMAND messagebox_test)

add_executable(ingest_progress_widget_test ingest_progress_widget_test.cpp)

target_link_libraries(ingest_progress_widget_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(ingest_progress_widget_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(ingest_progress_widget_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME IngestProgressWidget COMMAND ingest_progress_widget_test)

# Headless: StateColors is constexpr hash/palette math (QColor only at the rim).
add_executable(state_colors_test state_colors_test.cpp)

target_link_libraries(state_colors_test PRIVATE
    pj_widgets
    GTest::gtest_main
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(state_colors_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(state_colors_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME StateColorsTest COMMAND state_colors_test)

# Headless: StateTransitionsScene is display-free math (no QApplication).
add_executable(state_transitions_scene_test state_transitions_scene_test.cpp)

target_link_libraries(state_transitions_scene_test PRIVATE
    pj_widgets
    GTest::gtest_main
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(state_transitions_scene_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(state_transitions_scene_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME StateTransitionsSceneTest COMMAND state_transitions_scene_test)

# Headless: TimelineScene is Qt-free math (no QApplication). Links pj_widgets
# only for the Timeline.h include path / TimelineScene symbols.
add_executable(timeline_scene_test timeline_scene_test.cpp)

target_link_libraries(timeline_scene_test PRIVATE
    pj_widgets
    GTest::gtest_main
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(timeline_scene_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(timeline_scene_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME TimelineSceneTest COMMAND timeline_scene_test)

# Widget test: needs QApplication (xvfb under CI).
add_executable(state_transitions_view_test state_transitions_view_test.cpp)
set_target_properties(state_transitions_view_test PROPERTIES AUTOMOC ON)

target_link_libraries(state_transitions_view_test PRIVATE
    pj_widgets
    GTest::gtest_main
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(state_transitions_view_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(state_transitions_view_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME StateTransitionsViewTest COMMAND state_transitions_view_test)

# Widget test: needs QApplication (xvfb under CI).
add_executable(timeline_widget_test timeline_widget_test.cpp)
set_target_properties(timeline_widget_test PROPERTIES AUTOMOC ON)

target_link_libraries(timeline_widget_test PRIVATE
    pj_widgets
    GTest::gtest_main
    Qt6::Test
    Qt6::Widgets
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(timeline_widget_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(timeline_widget_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME TimelineWidgetTest COMMAND timeline_widget_test)

add_executable(toast_test toast_test.cpp)
set_target_properties(toast_test PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(toast_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
    Qt6::Widgets
)
if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(toast_test PRIVATE ${PJ_WARNING_FLAGS})
endif()
add_test(NAME ToastTest COMMAND toast_test)

# Drift guard: C++ FrameworkTokens vs the QSS palette blocks. Links the qrc so
# the stylesheets are readable as `:/resources/...` at runtime.
add_executable(framework_tokens_test
    framework_tokens_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
target_link_libraries(framework_tokens_test PRIVATE
    pj_widgets
    GTest::gtest_main
)
if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(framework_tokens_test PRIVATE ${PJ_WARNING_FLAGS})
endif()
set_target_properties(framework_tokens_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
add_test(NAME FrameworkTokens COMMAND framework_tokens_test)

add_executable(scrollbar_test
    scrollbar_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
set_target_properties(scrollbar_test PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(scrollbar_test PRIVATE
    pj_widgets
    GTest::gtest_main
    Qt6::Test
    Qt6::Widgets
)
if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(scrollbar_test PRIVATE ${PJ_WARNING_FLAGS})
endif()
add_test(NAME ScrollbarTest COMMAND scrollbar_test)

add_executable(search_test
    search_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
set_target_properties(search_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(search_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
)
if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(search_test PRIVATE ${PJ_WARNING_FLAGS})
endif()
add_test(NAME SearchTest COMMAND search_test)

add_executable(section_header_band_test
    section_header_band_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
set_target_properties(section_header_band_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(section_header_band_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
)
if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(section_header_band_test PRIVATE ${PJ_WARNING_FLAGS})
endif()
add_test(NAME SectionHeaderBandTest COMMAND section_header_band_test)

add_executable(header_divider_highlight_test
    header_divider_highlight_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_link_libraries(header_divider_highlight_test PRIVATE
    pj_widgets
    GTest::gtest_main
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(header_divider_highlight_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(header_divider_highlight_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME HeaderDividerHighlightTest COMMAND header_divider_highlight_test)

add_executable(header_resize_policy_test
    header_resize_policy_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_link_libraries(header_resize_policy_test PRIVATE
    pj_widgets
    GTest::gtest_main
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(header_resize_policy_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(header_resize_policy_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME HeaderResizePolicyTest COMMAND header_resize_policy_test)
