# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# parser_arrow, data_load_parquet and toolbox_data_exporter on the Arrow C++
# libraries that PjWasmArrowDependencies.cmake builds (target pj_wasm_arrow).
include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_ARROW)
    return()
endif()

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

if(PJ_WASM_WITH_ARROW_PARSER)
    pj_wasm_require_official_plugin_sources(_pj_arrow_dir parser_arrow src/arrow_parser.cpp manifest.json)
    add_library(pj_wasm_arrow_parser STATIC
        "${_pj_arrow_dir}/src/mosaico_objects.cpp"
        "${_pj_arrow_dir}/src/ipc_decoder.cpp"
        "${_pj_arrow_dir}/src/table_shaper.cpp"
        "${_pj_arrow_dir}/src/arrow_parser.cpp")
    target_compile_features(pj_wasm_arrow_parser PRIVATE cxx_std_20)
    target_include_directories(pj_wasm_arrow_parser PRIVATE "${_pj_arrow_dir}/src")
    target_link_libraries(pj_wasm_arrow_parser PRIVATE
        plotjuggler_sdk::plugin_sdk nanoarrow::nanoarrow nlohmann_json::nlohmann_json)
    target_link_libraries(pj_wasm_arrow_parser PUBLIC pj_wasm_arrow)
    pj_embed_manifest(pj_wasm_arrow_parser
        MANIFEST_FILE "${_pj_arrow_dir}/manifest.json"
        HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/arrow_manifest.hpp"
        VAR_NAME kArrowManifest)
    pj_wasm_finalize_official_plugin_targets(pj_wasm_arrow_parser)
endif()

if(PJ_WASM_WITH_PARQUET)
    pj_wasm_require_official_plugin_sources(_pj_parquet_dir data_load_parquet
        parquet_source.cpp dataload_parquet.ui manifest.json)
    add_library(pj_wasm_parquet_source STATIC
        "${_pj_parquet_dir}/parquet_source.cpp"
        "${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/common/arrow_helpers/arrow_helpers.cpp"
        # date::locate_zone needs the tz library, which PJ4's wasm build leaves out.
        # Built here against the (absent) OS tz database, no remote download.
        "${date_SOURCE_DIR}/src/tz.cpp")
    target_compile_features(pj_wasm_parquet_source PRIVATE cxx_std_20)
    target_compile_definitions(pj_wasm_parquet_source PRIVATE USE_OS_TZDB=1 HAS_REMOTE_API=0)
    target_include_directories(pj_wasm_parquet_source PRIVATE
        "${_pj_parquet_dir}" "${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/common/arrow_helpers/include")
    target_link_libraries(pj_wasm_parquet_source PRIVATE
        plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json date::date)
    target_link_libraries(pj_wasm_parquet_source PUBLIC pj_wasm_arrow)
    pj_embed_ui(pj_wasm_parquet_source
        UI_FILE "${_pj_parquet_dir}/dataload_parquet.ui"
        HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/dataload_parquet_ui.hpp"
        VAR_NAME kDataLoadParquetUi)
    pj_embed_manifest(pj_wasm_parquet_source
        MANIFEST_FILE "${_pj_parquet_dir}/manifest.json"
        HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/parquet_manifest.hpp"
        VAR_NAME kParquetManifest)
    pj_wasm_finalize_official_plugin_targets(pj_wasm_parquet_source)
endif()

if(PJ_WASM_WITH_DATA_EXPORTER)
    pj_wasm_require_official_plugin_sources(_pj_exporter_dir toolbox_data_exporter
        plugin_entry.cpp exporter_dialog.ui manifest.json)
    add_library(pj_wasm_data_exporter STATIC
        "${_pj_exporter_dir}/arrow_series_reader.cpp"
        "${_pj_exporter_dir}/data_exporter.cpp"
        "${_pj_exporter_dir}/export_core.cpp"
        "${_pj_exporter_dir}/plugin_entry.cpp")
    target_compile_features(pj_wasm_data_exporter PRIVATE cxx_std_20)
    target_include_directories(pj_wasm_data_exporter PRIVATE "${_pj_exporter_dir}")
    target_link_libraries(pj_wasm_data_exporter PRIVATE
        plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json)
    target_link_libraries(pj_wasm_data_exporter PUBLIC pj_wasm_arrow)
    pj_embed_ui(pj_wasm_data_exporter
        UI_FILE "${_pj_exporter_dir}/exporter_dialog.ui"
        HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/exporter_dialog_ui.hpp"
        VAR_NAME kDataExporterDialogUi)
    pj_embed_manifest(pj_wasm_data_exporter
        MANIFEST_FILE "${_pj_exporter_dir}/manifest.json"
        HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/data_exporter_manifest.hpp"
        VAR_NAME kDataExporterManifest)
    pj_wasm_finalize_official_plugin_targets(pj_wasm_data_exporter)
endif()

# PJ_WASM_ARROW_SIDE_MODULE: the three plugins and Arrow become the Emscripten side module plugins/arrow.wasm,
# which the app fetches and dlopen()s on first use (pj_app/src/wasm/ArrowSideModule.h) instead of linking them.
# The module links its own copies of the plugin SDK, Arrow and its codecs, and imports libc, libc++ and the
# Emscripten runtime from the main module, whose link names it (pj_app/CMakeLists.txt).
if(PJ_WASM_ARROW_SIDE_MODULE)
    if(NOT (TARGET pj_wasm_arrow_parser AND TARGET pj_wasm_parquet_source AND TARGET pj_wasm_data_exporter))
        message(FATAL_ERROR "PJ_WASM_ARROW_SIDE_MODULE needs PJ_WASM_WITH_ARROW_PARSER, _PARQUET and _DATA_EXPORTER")
    endif()
    find_program(PJ_WASM_PYTHON NAMES python3 REQUIRED)
    get_filename_component(_pj_emscripten_root "${CMAKE_C_COMPILER}" DIRECTORY)
    add_executable(pj_wasm_arrow_side "${CMAKE_SOURCE_DIR}/pj_app/src/wasm/arrow_side_module.cpp")
    target_compile_features(pj_wasm_arrow_side PRIVATE cxx_std_20)
    target_link_libraries(pj_wasm_arrow_side PRIVATE
        pj_wasm_arrow_parser pj_wasm_parquet_source pj_wasm_data_exporter plotjuggler_sdk::plugin_sdk)
    set_target_properties(pj_wasm_arrow_side PROPERTIES
        OUTPUT_NAME arrow
        SUFFIX ".wasm"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pj_app/plugins")
    pj_wasm_finalize_official_plugin_targets(pj_wasm_arrow_side)
    # Every archive a side module links must be position independent (nanoarrow's are not by default) and hide its
    # symbols: the module exports every one it does not, and those would win over the main module's own copies of
    # libc++ and the SDK at its link, which names the module. Arrow gets the same flags (PjWasmArrowDependencies).
    function(_pj_wasm_hide_link_closure target)
        get_target_property(_aliased ${target} ALIASED_TARGET)
        if(_aliased)
            set(target ${_aliased})
        endif()
        get_property(_seen GLOBAL PROPERTY _PJ_WASM_SIDE_CLOSURE)
        get_target_property(_imported ${target} IMPORTED)
        if(${target} IN_LIST _seen OR _imported)
            return()
        endif()
        set_property(GLOBAL APPEND PROPERTY _PJ_WASM_SIDE_CLOSURE ${target})
        get_target_property(_type ${target} TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON
                C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
            # libc++ gives its templates' typeinfo and vtables default visibility, which -fvisibility=hidden keeps.
            set_property(TARGET ${target} APPEND PROPERTY COMPILE_DEFINITIONS _LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS)
            get_target_property(_deps ${target} LINK_LIBRARIES)
        else()
            set(_deps "")
        endif()
        get_target_property(_interface ${target} INTERFACE_LINK_LIBRARIES)
        foreach(_dep IN LISTS _deps _interface)
            string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _dep "${_dep}")
            if(TARGET ${_dep})
                _pj_wasm_hide_link_closure(${_dep})
            endif()
        endforeach()
    endfunction()
    _pj_wasm_hide_link_closure(pj_wasm_arrow_side)
    target_link_options(pj_wasm_arrow_side PRIVATE -sSIDE_MODULE=2 -sEXPORTED_FUNCTIONS=_pj_arrow_side_plugins)
    set(PJ_WASM_EMSCRIPTEN_ROOT "${_pj_emscripten_root}")
endif()
