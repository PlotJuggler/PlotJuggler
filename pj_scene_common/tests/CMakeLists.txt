if(PJ_BUILD_TESTS)
    find_package(GTest REQUIRED)

    # WARNINGS: 1 merged test carried PJ_WARNING_FLAGS before
    pj_add_test_runner(pj_scene_common_tests MAIN gui WARNINGS
        SOURCES scene_common_test.cpp
        LIBS pj_scene_common
    )

    set_target_properties(pj_scene_common_tests PROPERTIES AUTOMOC ON)
endif()
