# Standalone visual demos for pj_widgets: dev tools, not part of the shipped app, depending only on pj_widgets and Qt.

# The demo-internal QSS preprocessor shared by the demos that parse the stylesheet_*.qss palette and body; PJ_QSS_DIR
# is set on this OBJECT library, so the absolute resources path bakes in once.
add_library(pj_widgets_demos_qss OBJECT qss_preprocessor.cpp)
target_compile_features(pj_widgets_demos_qss PRIVATE cxx_std_20)
target_compile_options(pj_widgets_demos_qss PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(pj_widgets_demos_qss PRIVATE
    PJ_QSS_DIR="${CMAKE_SOURCE_DIR}/resources"
)
target_link_libraries(pj_widgets_demos_qss PUBLIC
    Qt6::Core
    Qt6::Widgets
)

add_executable(pj_widgets_messagebox_demo
    messagebox_demo.cpp
)
target_compile_features(pj_widgets_messagebox_demo PRIVATE cxx_std_20)
target_compile_options(pj_widgets_messagebox_demo PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_widgets_messagebox_demo PRIVATE
    pj_widgets
    pj_resources
    pj_widgets_demos_qss
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
)

add_executable(pj_widgets_combobox_demo
    combobox_demo.cpp
)
target_compile_features(pj_widgets_combobox_demo PRIVATE cxx_std_20)
target_compile_options(pj_widgets_combobox_demo PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_widgets_combobox_demo PRIVATE
    pj_widgets
    pj_resources
    pj_widgets_demos_qss
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
)

# Source Timeline with fabricated tracks and no runtime. Links resources.qrc so the name-column header icons render.
add_executable(pj_widgets_timeline_demo
    timeline_demo.cpp
)
target_compile_features(pj_widgets_timeline_demo PRIVATE cxx_std_20)
target_compile_options(pj_widgets_timeline_demo PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_widgets_timeline_demo PRIVATE
    pj_widgets
    pj_resources
    Qt6::Widgets
)

# Dialog building blocks: RangeSlider and DateRangePicker. No QSS or resources.
add_executable(pj_widgets_dialog_blocks_demo dialog_blocks_demo.cpp)
target_compile_features(pj_widgets_dialog_blocks_demo PRIVATE cxx_std_20)
target_compile_options(pj_widgets_dialog_blocks_demo PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_widgets_dialog_blocks_demo PRIVATE
    pj_widgets
    Qt6::Widgets
)

add_executable(pj_widgets_check_button_demo
    check_button_demo.cpp
)
target_compile_features(pj_widgets_check_button_demo PRIVATE cxx_std_20)
target_compile_options(pj_widgets_check_button_demo PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_widgets_check_button_demo PRIVATE
    pj_widgets
    pj_resources
    pj_widgets_demos_qss
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
)

# Toast notifications (the "new release available" surface). Links resources.qrc for the success_kid.png icon.
# --screenshot <path> captures headless.
add_executable(pj_widgets_toast_demo
    toast_demo.cpp
)
target_compile_features(pj_widgets_toast_demo PRIVATE cxx_std_20)
target_compile_options(pj_widgets_toast_demo PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_widgets_toast_demo PRIVATE
    pj_widgets
    pj_resources
    pj_widgets_demos_qss
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
)

# PJ::Scrollbar reserved-gutter placement: a table with trailing per-row action buttons beside an auto-hide scroll page.
add_executable(pj_widgets_scrollbar_demo
    scrollbar_demo.cpp
)
target_compile_features(pj_widgets_scrollbar_demo PRIVATE cxx_std_20)
target_compile_options(pj_widgets_scrollbar_demo PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_widgets_scrollbar_demo PRIVATE
    pj_widgets
    pj_resources
    pj_widgets_demos_qss
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
)
