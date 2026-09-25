# SPDX-License-Identifier: MPL-2.0
# One gtest runner per module family instead of one executable per test file.
# Sizes: every static test binary re-links the module's whole dependency
# closure and its DWARF (200-450 MB each, 327 of them = 32 GB); a runner
# pays that once. ctest granularity and per-case process isolation are kept
# by gtest_discover_tests, which registers one ctest entry per TEST and runs
# each with --gtest_filter in its own process.
include(GoogleTest)

set(PJ_TEST_MAINS_DIR "${CMAKE_CURRENT_LIST_DIR}/test_mains")

# Header-only: pj_gpu_test_gate.h (PJ_SKIP_WITHOUT_GPU, gl45ContextUnavailable) for
# any test target, GL or not. A function (not inlined here) because this file is
# include()'d before Qt6/plotjuggler_sdk are found; every caller triggers the
# guarded, one-time creation on first use instead.
function(pj_ensure_gpu_test_gate)
    if(NOT TARGET pj_gpu_test_gate)
        add_library(pj_gpu_test_gate INTERFACE)
        target_include_directories(pj_gpu_test_gate INTERFACE "${PJ_TEST_MAINS_DIR}")
        target_link_libraries(pj_gpu_test_gate INTERFACE plotjuggler_sdk::base GTest::gtest Qt6::Gui)
    endif()
endfunction()

# The legacy OpenGL renderer's suite (PJ_SCENE3D_RHI=OFF, the rollback kept for
# one release): `target` joins pj_gl_rollback_tests and the ctest cases named in
# ARGN get the label gl-rollback, so linux-ci builds one target and runs
# `-L gl-rollback`. Does nothing in the QRhi build.
function(pj_gl_rollback_test target)
    if(PJ_SCENE3D_RHI)
        return()
    endif()
    if(NOT TARGET pj_gl_rollback_tests)
        add_custom_target(pj_gl_rollback_tests)
    endif()
    add_dependencies(pj_gl_rollback_tests ${target})
    if(ARGN)
        set_tests_properties(${ARGN} PROPERTIES LABELS gl-rollback)
    endif()
endfunction()

# GL_ROLLBACK: the runner belongs to the legacy OpenGL renderer's suite (see
# pj_gl_rollback_test()); its cases get the label gl-rollback in that build.
#
# RHI_BACKENDS: a QRhi display runner. Its cases register once per graphics
# backend of the platform, as <runner>.<backend>.* with PJ_GRAPHICS_API set to
# match and the ctest label qrhi-<backend>: opengl and vulkan on Linux, d3d11 on
# Windows, metal and opengl on macOS. DPR2 adds <runner>.opengl_dpr2.* at
# QT_SCALE_FACTOR=2 where OpenGL is registered (label qrhi-opengl), limited to
# the gtest DPR2_FILTER glob when given. The runner also joins the pj_qrhi_tests
# target, so CI builds every one with `--target pj_qrhi_tests` and picks their
# passes with `-L qrhi-<backend>` (every pass: `-L qrhi`; none: `-LE qrhi`).
function(pj_add_test_runner target)
    set(options WARNINGS RHI_BACKENDS DPR2 GL_ROLLBACK)
    set(oneValueArgs MAIN PREFIX TIMEOUT WORKING_DIRECTORY DPR2_FILTER)
    # PROPERTIES: extra ctest properties for every discovered case (DISABLED, ENVIRONMENT, ...).
    set(multiValueArgs SOURCES LIBS DEFINES PROPERTIES)
    cmake_parse_arguments(R "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    # plain = no Qt at all. It exists because core was previously the floor, so a
    # module with no Qt in any of its test sources still linked Qt6::Core purely to
    # satisfy this parameter. Pick the least the runner actually needs.
    if(NOT R_MAIN MATCHES "^(plain|core|gui|gl)$")
        message(FATAL_ERROR "pj_add_test_runner(${target}): MAIN must be plain, core, gui or gl")
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
    if(R_MAIN STREQUAL "core")
        target_link_libraries(${target} PRIVATE Qt6::Core)
    elseif(R_MAIN STREQUAL "gui")
        pj_ensure_gpu_test_gate()
        target_link_libraries(${target} PRIVATE Qt6::Widgets pj_gpu_test_gate)
    elseif(R_MAIN STREQUAL "gl")
        pj_ensure_gpu_test_gate()
        target_link_libraries(${target} PRIVATE Qt6::Gui pj_gpu_test_gate)
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
                PROPERTIES TIMEOUT ${timeout} LABELS "qrhi-${api}" ENVIRONMENT "PJ_GRAPHICS_API=${api}")
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
                           ENVIRONMENT_MODIFICATION "QT_SCALE_FACTOR=set:2")
        endif()
        if(NOT TARGET pj_qrhi_tests)
            add_custom_target(pj_qrhi_tests)
        endif()
        add_dependencies(pj_qrhi_tests ${target})
        return()
    endif()
    if(R_GL_ROLLBACK AND NOT PJ_SCENE3D_RHI)
        list(APPEND R_PROPERTIES LABELS gl-rollback)
        pj_gl_rollback_test(${target})
    endif()
    set(discover_args
        TEST_PREFIX "${R_PREFIX}"
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT ${R_TIMEOUT} ${R_PROPERTIES})
    if(R_WORKING_DIRECTORY)
        list(APPEND discover_args WORKING_DIRECTORY "${R_WORKING_DIRECTORY}")
    endif()
    # PRE_TEST: discovery runs at ctest time, inside the same environment the
    # tests get (xvfb / offscreen), so a GUI runner never has to start a
    # QApplication at build time. Discovery only lists cases; it constructs
    # no windows.
    gtest_discover_tests(${target} ${discover_args})
endfunction()
