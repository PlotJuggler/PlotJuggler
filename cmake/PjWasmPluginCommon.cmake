# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

# Resolve one directory from the immutable official-plugin source population
# and fail at configure time if the reviewed source contract has drifted.
function(pj_wasm_require_official_plugin_sources out_var relative_dir)
    if(NOT PJ_OFFICIAL_PLUGINS_SOURCE_DIR)
        message(FATAL_ERROR "WASM official plugins require the pinned source population")
    endif()
    set(_plugin_dir "${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/${relative_dir}")
    foreach(_required IN LISTS ARGN)
        if(NOT EXISTS "${_plugin_dir}/${_required}")
            message(FATAL_ERROR "Pinned ${relative_dir} source is missing ${_plugin_dir}/${_required}")
        endif()
    endforeach()
    set(${out_var} "${_plugin_dir}" PARENT_SCOPE)
endfunction()

function(pj_wasm_finalize_official_plugin_targets)
    foreach(_target IN LISTS ARGN)
        target_compile_options(${_target} PRIVATE ${PJ_WARNING_FLAGS})
        set_target_properties(${_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endforeach()
endfunction()

if(NOT EMSCRIPTEN)
    return()
endif()

set(_pj_wasm_common_dir "${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/common")

if(PJ_WASM_WITH_ROS OR PJ_WASM_WITH_DATA_TAMER)
    # Header-only DataTamer schema/snapshot parser shared by parser_ros and
    # parser_data_tamer.
    add_library(pj_wasm_data_tamer_parser INTERFACE)
    target_include_directories(pj_wasm_data_tamer_parser SYSTEM INTERFACE
        "${data_tamer_SOURCE_DIR}/data_tamer_cpp/include")
endif()

if(PJ_WASM_WITH_ANOMALY_DETECTOR)
    # UI-free "preview a plot + run a generator" engine of the generator toolboxes.
    add_library(pj_wasm_toolbox_preview STATIC
        "${_pj_wasm_common_dir}/toolbox_preview/series_catalog.cpp"
        "${_pj_wasm_common_dir}/toolbox_preview/series_types.cpp"
        "${_pj_wasm_common_dir}/toolbox_preview/ephemeral_preview.cpp")
    target_compile_features(pj_wasm_toolbox_preview PUBLIC cxx_std_20)
    target_include_directories(pj_wasm_toolbox_preview PUBLIC
        "${_pj_wasm_common_dir}/toolbox_preview/include")
    target_link_libraries(pj_wasm_toolbox_preview PUBLIC
        plotjuggler_sdk::plugin_sdk
        nlohmann_json::nlohmann_json)
    pj_wasm_finalize_official_plugin_targets(pj_wasm_toolbox_preview)
endif()

if(NOT (PJ_WASM_WITH_ROS OR PJ_WASM_WITH_PROTOBUF))
    return()
endif()

# Shared by parser_ros and parser_protobuf. Keep these targets independent of
# either feature so protobuf remains buildable with ROS disabled and vice versa.
add_library(pj_wasm_laser_scan STATIC
    "${_pj_wasm_common_dir}/laser_scan/laser_scan_projector.cpp")
target_compile_features(pj_wasm_laser_scan PUBLIC cxx_std_20)
target_include_directories(pj_wasm_laser_scan PUBLIC
    "${_pj_wasm_common_dir}/laser_scan/include")
target_link_libraries(pj_wasm_laser_scan PUBLIC plotjuggler_sdk::plugin_sdk)
pj_wasm_finalize_official_plugin_targets(pj_wasm_laser_scan)

add_library(pj_wasm_grid_map STATIC
    "${_pj_wasm_common_dir}/grid_map/grid_map_transcoder.cpp")
target_compile_features(pj_wasm_grid_map PUBLIC cxx_std_20)
target_include_directories(pj_wasm_grid_map PUBLIC
    "${_pj_wasm_common_dir}/grid_map/include")
target_link_libraries(pj_wasm_grid_map PUBLIC plotjuggler_sdk::plugin_sdk)
pj_wasm_finalize_official_plugin_targets(pj_wasm_grid_map)

add_library(pj_wasm_pointcloud_color INTERFACE)
target_compile_features(pj_wasm_pointcloud_color INTERFACE cxx_std_20)
target_include_directories(pj_wasm_pointcloud_color INTERFACE
    "${_pj_wasm_common_dir}/pointcloud_color/include")
target_link_libraries(pj_wasm_pointcloud_color INTERFACE plotjuggler_sdk::plugin_sdk)
