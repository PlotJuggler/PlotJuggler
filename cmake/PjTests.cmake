# SPDX-License-Identifier: MPL-2.0
# One gtest runner per module family instead of one executable per test file.
# Sizes: every static test binary re-links the module's whole dependency
# closure and its DWARF (200-450 MB each, 327 of them = 32 GB); a runner
# pays that once. ctest granularity and per-case process isolation are kept
# by gtest_discover_tests, which registers one ctest entry per TEST and runs
# each with --gtest_filter in its own process.
include(GoogleTest)

set(PJ_TEST_MAINS_DIR "${CMAKE_CURRENT_LIST_DIR}/test_mains")

# RHI_BACKENDS: a QRhi display runner. Its cases register once per graphics
# backend of the platform, as <runner>.<backend>.* with PJ_GRAPHICS_API set to
# match and the ctest label qrhi-<backend>: opengl and vulkan on Linux, d3d11 on
# Windows, metal and opengl on macOS. DPR2 adds <runner>.opengl_dpr2.* at
# QT_SCALE_FACTOR=2 where OpenGL is registered (label qrhi-opengl), limited to
# the gtest DPR2_FILTER glob when given. The runner also joins the pj_qrhi_tests
# target, so CI builds every one with `--target pj_qrhi_tests` and picks their
# passes with `-L qrhi-<backend>` (every pass: `-L qrhi`; none: `-LE qrhi`).
function(pj_add_test_runner target)
    set(options WARNINGS RHI_BACKENDS DPR2)
    set(oneValueArgs MAIN PREFIX TIMEOUT WORKING_DIRECTORY DPR2_FILTER)
    # PROPERTIES: extra ctest properties for every discovered case (DISABLED, ENVIRONMENT, ...).
    set(multiValueArgs SOURCES LIBS DEFINES PROPERTIES)
    cmake_parse_arguments(R "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    # plain = no Qt at all. It exists because core was previously the floor, so a
    # module with no Qt in any of its test sources still linked Qt6::Core purely to
    # satisfy this parameter. Pick the least the runner actually needs.
    if(NOT R_MAIN MATCHES "^(plain|core|gui)$")
        message(FATAL_ERROR "pj_add_test_runner(${target}): MAIN must be plain, core or gui")
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
    # "plain" deliberately falls through linking no Qt target: an else() branch
    # here is what silently gave every runner a Qt dependency.
    set(lock)
    if(R_MAIN STREQUAL "core")
        target_link_libraries(${target} PRIVATE Qt6::Core)
    elseif(R_MAIN STREQUAL "gui")
        # pj_gpu_test_gate.h: PJ_SKIP_WITHOUT_GPU and the display-test helpers.
        target_include_directories(${target} PRIVATE "${PJ_TEST_MAINS_DIR}")
        target_link_libraries(${target} PRIVATE Qt6::Widgets plotjuggler_sdk::base)
        # Every case of a gui runner shares one test-mode config dir (named after
        # the runner), which the main clears and the extension store locks with a
        # non-waiting lease: under `ctest -j` they must not overlap. Cases of
        # different runners still run in parallel.
        set(lock RESOURCE_LOCK ${target})
    endif()
    if(R_DEFINES)
        target_compile_definitions(${target} PRIVATE ${R_DEFINES})
    endif()
    # Opt in only for uniformly strict sources so merging tests does not introduce -Werror failures.
    if(R_WARNINGS AND DEFINED PJ_WARNING_FLAGS)
        target_compile_options(${target} PRIVATE ${PJ_WARNING_FLAGS})
    endif()

    if(R_RHI_BACKENDS)
        if(WIN32)
            set(backends d3d11)
        elseif(APPLE)
            set(backends metal opengl)
        else()
            set(backends opengl vulkan)
        endif()
        # macOS OpenGL without a GL-capable GPU (the CI VMs) is Apple's software
        # renderer: a float/redock case that takes 1-2 s on Metal takes 40-60 s+.
        set(opengl_timeout ${R_TIMEOUT})
        if(APPLE)
            math(EXPR opengl_timeout "${R_TIMEOUT} * 5")
        endif()
        # One list element per property value: gtest_discover_tests splits a ';'
        # inside one, hence ENVIRONMENT_MODIFICATION for the scale factor.
        foreach(api IN LISTS backends)
            set(timeout ${R_TIMEOUT})
            if(api STREQUAL "opengl")
                set(timeout ${opengl_timeout})
            endif()
            gtest_discover_tests(${target}
                TEST_PREFIX "${target}.${api}."
                DISCOVERY_MODE PRE_TEST
                PROPERTIES TIMEOUT ${timeout} LABELS "qrhi-${api}" ENVIRONMENT "PJ_GRAPHICS_API=${api}" ${lock})
        endforeach()
        if(R_DPR2 AND "opengl" IN_LIST backends)
            set(filter)
            if(R_DPR2_FILTER)
                set(filter TEST_FILTER "${R_DPR2_FILTER}")
            endif()
            gtest_discover_tests(${target}
                TEST_PREFIX "${target}.opengl_dpr2."
                ${filter}
                DISCOVERY_MODE PRE_TEST
                PROPERTIES TIMEOUT ${opengl_timeout} LABELS "qrhi-opengl" ENVIRONMENT "PJ_GRAPHICS_API=opengl"
                           ENVIRONMENT_MODIFICATION "QT_SCALE_FACTOR=set:2" ${lock})
        endif()
        if(NOT TARGET pj_qrhi_tests)
            add_custom_target(pj_qrhi_tests)
        endif()
        add_dependencies(pj_qrhi_tests ${target})
        return()
    endif()
    set(discover_args
        TEST_PREFIX "${R_PREFIX}"
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT ${R_TIMEOUT} ${lock} ${R_PROPERTIES})
    if(R_WORKING_DIRECTORY)
        list(APPEND discover_args WORKING_DIRECTORY "${R_WORKING_DIRECTORY}")
    endif()
    # PRE_TEST: discovery runs at ctest time, not right after the link. POST_BUILD
    # would execute every runner at build time, on Windows without the Qt/FFmpeg
    # DLLs on PATH. Discovery is lazy, so parallel ctest invocations on one freshly
    # rebuilt tree race on it (scripts/perf/gate_par.sh discovers once first).
    gtest_discover_tests(${target} ${discover_args})
endfunction()
