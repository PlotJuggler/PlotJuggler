# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "The Scene3D capability probe is WebAssembly-only")
endif()

find_package(Qt6 ${PJ_QT_REQUIRED_VERSION} REQUIRED COMPONENTS Widgets Gui GuiPrivate)

include(${CMAKE_SOURCE_DIR}/cmake/PjQsb.cmake)
pj_find_host_qsb(_pj_cap_qsb "The Scene3D capability probe")

set(_pj_cap_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(_pj_cap_verify_dir "${CMAKE_CURRENT_BINARY_DIR}/verified_shaders")
file(MAKE_DIRECTORY "${_pj_cap_verify_dir}")
foreach(_pj_cap_shader IN ITEMS
        background.vert background.frag
        point.vert point.frag
        indexed.vert indexed.frag)
    set(_pj_cap_source "${_pj_cap_shader_dir}/${_pj_cap_shader}")
    set(_pj_cap_pack "${_pj_cap_source}.qsb")
    set(_pj_cap_rebuilt "${_pj_cap_verify_dir}/${_pj_cap_shader}.qsb")
    execute_process(
        COMMAND "${_pj_cap_qsb}" --glsl "300 es" -o "${_pj_cap_rebuilt}" "${_pj_cap_source}"
        RESULT_VARIABLE _pj_cap_qsb_result
        ERROR_VARIABLE _pj_cap_qsb_error)
    if(NOT _pj_cap_qsb_result EQUAL 0)
        message(FATAL_ERROR "Failed to bake ${_pj_cap_shader}: ${_pj_cap_qsb_error}")
    endif()
    file(SHA256 "${_pj_cap_pack}" _pj_cap_committed_hash)
    file(SHA256 "${_pj_cap_rebuilt}" _pj_cap_rebuilt_hash)
    if(NOT _pj_cap_committed_hash STREQUAL _pj_cap_rebuilt_hash)
        message(FATAL_ERROR
            "Stale capability shader ${_pj_cap_pack}; re-bake with the documented qsb command")
    endif()
endforeach()

qt_add_executable(pj_scene3d_wasm_capability_probe
    capability_probe.cpp
    shaders/scene3d_capability_shaders.qrc)
set_target_properties(pj_scene3d_wasm_capability_probe PROPERTIES
    AUTORCC ON
    OUTPUT_NAME scene3d_capability_probe
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pj_app"
    QT_WASM_INITIAL_MEMORY "64MB")
target_compile_features(pj_scene3d_wasm_capability_probe PRIVATE cxx_std_20)
target_compile_options(pj_scene3d_wasm_capability_probe PRIVATE ${PJ_WARNING_FLAGS})
target_link_options(pj_scene3d_wasm_capability_probe PRIVATE -sALLOW_MEMORY_GROWTH=1)
target_link_libraries(pj_scene3d_wasm_capability_probe PRIVATE
    Qt6::Widgets
    Qt6::Gui
    Qt6::GuiPrivate)

unset(_pj_cap_committed_hash)
unset(_pj_cap_pack)
unset(_pj_cap_qsb)
unset(_pj_cap_qsb_error)
unset(_pj_cap_qsb_result)
unset(_pj_cap_rebuilt)
unset(_pj_cap_rebuilt_hash)
unset(_pj_cap_shader)
unset(_pj_cap_shader_dir)
unset(_pj_cap_source)
unset(_pj_cap_verify_dir)
