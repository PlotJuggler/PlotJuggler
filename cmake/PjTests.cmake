# SPDX-License-Identifier: MPL-2.0
# One gtest runner per module family instead of one executable per test file.
# Sizes: every static test binary re-links the module's whole dependency
# closure and its DWARF (200-450 MB each, 327 of them = 32 GB); a runner
# pays that once. ctest granularity and per-case process isolation are kept
# by gtest_discover_tests, which registers one ctest entry per TEST and runs
# each with --gtest_filter in its own process.
include(GoogleTest)

set(PJ_TEST_MAINS_DIR "${CMAKE_CURRENT_LIST_DIR}/test_mains")

function(pj_add_test_runner target)
    set(options WARNINGS)
    set(oneValueArgs MAIN PREFIX TIMEOUT WORKING_DIRECTORY)
    set(multiValueArgs SOURCES LIBS DEFINES)
    cmake_parse_arguments(R "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(NOT R_MAIN MATCHES "^(core|gui|gl)$")
        message(FATAL_ERROR "pj_add_test_runner(${target}): MAIN must be core, gui or gl")
    endif()
    if(NOT R_SOURCES)
        message(FATAL_ERROR "pj_add_test_runner(${target}): SOURCES is empty")
    endif()
    if(NOT R_PREFIX)
        set(R_PREFIX "${target}.")
    endif()
    if(NOT R_TIMEOUT)
        set(R_TIMEOUT 120)
    endif()

    add_executable(${target} ${R_SOURCES} "${PJ_TEST_MAINS_DIR}/pj_test_main_${R_MAIN}.cpp")
    target_link_libraries(${target} PRIVATE ${R_LIBS} GTest::gtest)
    if(R_MAIN STREQUAL "core")
        target_link_libraries(${target} PRIVATE Qt6::Core)
    elseif(R_MAIN STREQUAL "gui")
        target_link_libraries(${target} PRIVATE Qt6::Widgets)
    else()
        target_link_libraries(${target} PRIVATE Qt6::Gui)
    endif()
    if(R_DEFINES)
        target_compile_definitions(${target} PRIVATE ${R_DEFINES})
    endif()
    # Opt in only for uniformly strict sources so merging tests does not introduce -Werror failures.
    if(R_WARNINGS AND DEFINED PJ_WARNING_FLAGS)
        target_compile_options(${target} PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    if(DEFINED PJ_SANITIZER_FLAGS)
        target_compile_options(${target} PRIVATE ${PJ_SANITIZER_FLAGS})
    endif()

    set(discover_args
        TEST_PREFIX "${R_PREFIX}"
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT ${R_TIMEOUT})
    if(R_WORKING_DIRECTORY)
        list(APPEND discover_args WORKING_DIRECTORY "${R_WORKING_DIRECTORY}")
    endif()
    # PRE_TEST: discovery runs at ctest time, inside the same environment the
    # tests get (xvfb / offscreen), so a GUI runner never has to start a
    # QApplication at build time. Discovery only lists cases; it constructs
    # no windows.
    gtest_discover_tests(${target} ${discover_args})
endfunction()
