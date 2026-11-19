find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Xml)

add_library(pj_log STATIC
    src/LogDockWidget.cpp
    src/LogTableModel.cpp
    src/LogView.cpp
    src/LogView.ui
    include/pj_log/LogDockWidget.h
    include/pj_log/LogTableModel.h
    include/pj_log/LogView.h
)

set_target_properties(pj_log PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(pj_log PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(pj_log PUBLIC cxx_std_20)
target_compile_options(pj_log PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(pj_log PUBLIC
    pj_log_core
    pj_runtime
    pj_datastore
    pj_widgets
    plotjuggler_sdk::base
    Qt6::Widgets
    Qt6::Xml
)
