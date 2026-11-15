# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# The CAN log importers (data_load_candump, data_load_blf, data_load_mf4) and
# their shared DBC decoder (common/can_dbc). Third-party readers are built by
# the plugins' own CMake functions, so WASM gets the same vendored fixes.
include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT (PJ_WASM_WITH_CANDUMP OR PJ_WASM_WITH_BLF OR PJ_WASM_WITH_MF4))
    return()
endif()

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

pj_wasm_require_official_plugin_sources(_pj_can_dbc_dir common/can_dbc can_decoder.cpp cmake/dbc_parser.cmake)
include("${_pj_can_dbc_dir}/cmake/dbc_parser.cmake")
pj_add_dbc_parser_library(pj_wasm_dbc_parser "${dbc_parser_cpp_SOURCE_DIR}" "${dbc_fast_float_SOURCE_DIR}")
add_library(pj_wasm_can_dbc STATIC
    "${_pj_can_dbc_dir}/can_decoder.cpp"
    "${_pj_can_dbc_dir}/can_topic.cpp"
    "${_pj_can_dbc_dir}/dictionary_paths.cpp"
    "${_pj_can_dbc_dir}/signal_row.cpp")
target_compile_features(pj_wasm_can_dbc PUBLIC cxx_std_20)
target_include_directories(pj_wasm_can_dbc PUBLIC "${_pj_can_dbc_dir}/include")
target_link_libraries(pj_wasm_can_dbc PUBLIC plotjuggler_sdk::base PRIVATE pj_wasm_dbc_parser)
pj_wasm_finalize_official_plugin_targets(pj_wasm_can_dbc)

if(PJ_WASM_WITH_BLF OR PJ_WASM_WITH_MF4)
    # Emscripten's sysroot zlib, as for Scene2D's libpng.
    find_package(ZLIB 1.3.1 EXACT REQUIRED)
endif()

# One CAN importer: <prefix>_source.cpp + <prefix>_dialog.cpp + extra sources,
# its dialog .ui and manifest, linked to the DBC decoder and extra libraries.
function(_pj_wasm_can_source target dir prefix manifest_var ui_var)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    pj_wasm_require_official_plugin_sources(_dir ${dir}
        ${prefix}_source.cpp ${prefix}_dialog.cpp ${prefix}_dialog.ui manifest.json)
    set(_sources "${_dir}/${prefix}_source.cpp" "${_dir}/${prefix}_dialog.cpp")
    foreach(_s IN LISTS ARG_SOURCES)
        list(APPEND _sources "${_dir}/${_s}")
    endforeach()
    add_library(${target} STATIC ${_sources})
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${_dir}")
    target_link_libraries(${target} PRIVATE
        plotjuggler_sdk::plugin_sdk pj_wasm_can_dbc nlohmann_json::nlohmann_json ${ARG_LIBS})
    pj_embed_manifest(${target}
        MANIFEST_FILE "${_dir}/manifest.json"
        HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/${prefix}_manifest.hpp"
        VAR_NAME ${manifest_var})
    pj_embed_ui(${target}
        UI_FILE "${_dir}/${prefix}_dialog.ui"
        HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/${prefix}_dialog_ui.hpp"
        VAR_NAME ${ui_var})
    pj_wasm_finalize_official_plugin_targets(${target})
endfunction()

if(PJ_WASM_WITH_CANDUMP)
    _pj_wasm_can_source(pj_wasm_candump_source data_load_candump candump kCandumpManifest kCandumpDialogUi
        SOURCES candump_parser.cpp csv_dictionary.cpp)
endif()

if(PJ_WASM_WITH_BLF)
    include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/data_load_blf/cmake/lblf.cmake")
    pj_add_lblf_library(pj_wasm_lblf "${lblf_SOURCE_DIR}")
    _pj_wasm_can_source(pj_wasm_blf_source data_load_blf blf kBlfManifest kBlfDialogUi
        SOURCES blf_frames.cpp LIBS pj_wasm_lblf)
endif()

if(PJ_WASM_WITH_MF4)
    include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/data_load_mf4/cmake/mdflib.cmake")
    pj_add_mdflib_library(pj_wasm_mdflib "${mdflib_SOURCE_DIR}")
    _pj_wasm_can_source(pj_wasm_mf4_source data_load_mf4 mf4 kMf4Manifest kMf4DialogUi
        SOURCES mf4_reader.cpp LIBS pj_wasm_mdflib)
endif()
