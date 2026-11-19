find_package(GTest REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test Widgets)

pj_add_test_runner(pj_log_tests
    MAIN gui
    WARNINGS
    SOURCES
        log_model_test.cpp
        log_view_test.cpp
        log_dock_widget_test.cpp
    # The table model's colours come from FrameworkTokens, which reads the QSS palette resource.
    LIBS
        pj_log
        pj_resources
        pj_runtime
        pj_datastore
        Qt6::Test
        Qt6::Widgets
)
set_target_properties(pj_log_tests PROPERTIES AUTOMOC ON)
