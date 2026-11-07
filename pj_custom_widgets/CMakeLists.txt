add_subdirectory(core)
add_subdirectory(widget)

if(PJ_BUILD_TESTS)
    add_subdirectory(tests)
endif()
