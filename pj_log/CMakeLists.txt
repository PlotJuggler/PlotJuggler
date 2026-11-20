add_subdirectory(core)
add_subdirectory(widget)

if(PJ_BUILD_DEMOS AND NOT EMSCRIPTEN)
    add_subdirectory(benchmarks)
endif()

if(PJ_BUILD_TESTS)
    add_subdirectory(tests)
endif()
