if(PJ_BUILD_TESTS)
    find_package(GTest REQUIRED)

    # WARNINGS: 1 merged test carried PJ_WARNING_FLAGS before
    pj_add_test_runner(pj_scene_common_tests MAIN gui WARNINGS
        SOURCES scene_common_test.cpp
        LIBS pj_scene_common
    )

    set_target_properties(pj_scene_common_tests PROPERTIES AUTOMOC ON)
    # The on-demand engine's fake script run (test::FakeRun), for the derived-topic tests.
    target_include_directories(pj_scene_common_tests PRIVATE ${CMAKE_SOURCE_DIR}/pj_runtime/tests)
endif()
