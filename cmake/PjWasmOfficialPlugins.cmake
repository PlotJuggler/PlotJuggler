# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# The official plugins built into the browser app. Each plugin directory of the pinned
# pj-official-plugins tree (cmake/PjWasmDependencies.cmake) has a wasm.cmake that builds it as a
# static library; its cmake/PjWasmPlugin.cmake states what this build provides for them. Each
# entry here is `<plugin dir>` or `<plugin dir>=<NAME>` and adds the ON-by-default option
# PJ_WASM_WITH_<NAME> (by default the directory without its data_load_/data_stream_/parser_/
# toolbox_/widget_ prefix, uppercased). The order is the registration order.
include_guard(GLOBAL)

set(PJ_WASM_OFFICIAL_PLUGIN_DIRS
    data_load_csv
    data_load_mcap
    parser_ros
    parser_protobuf
    toolbox_transform_editor
    data_stream_dummy=DUMMY_STREAM
    data_stream_pj_bridge
    data_stream_foxglove_bridge
    parser_json
    parser_data_tamer
    data_load_3d=3D_LOADER
    toolbox_quaternion
    toolbox_fft
    toolbox_anomaly_detector
    widget_log=LOG_WIDGET
    parser_influx
    parser_navmap
    data_load_ulog
    data_load_candump
    data_load_blf
    data_load_mf4
    parser_arrow=ARROW_PARSER
    data_load_parquet
    toolbox_data_exporter)

# The plugins that make up the side module plugins/arrow.wasm when PJ_WASM_ARROW_SIDE_MODULE is on
# (PjWasmDependencies.cmake): built from their wasm.cmake like the others, then linked into that module
# (pj_app/CMakeLists.txt) instead of into the app, and registered when it loads, not at startup.
set(PJ_WASM_ARROW_SIDE_MODULE_DIRS parser_arrow data_load_parquet toolbox_data_exporter)

set(_pj_wasm_enabled_plugins "")
foreach(_entry IN LISTS PJ_WASM_OFFICIAL_PLUGIN_DIRS)
    string(REPLACE "=" ";" _entry "${_entry}")
    list(GET _entry 0 _dir)
    list(LENGTH _entry _len)
    if(_len EQUAL 2)
        list(GET _entry 1 _name)
    else()
        string(REGEX REPLACE "^(data_load|data_stream|parser|toolbox|widget)_" "" _name "${_dir}")
        string(TOUPPER "${_name}" _name)
    endif()
    option(PJ_WASM_WITH_${_name} "Build the official ${_dir} plugin into the wasm app" ON)
    if(PJ_WASM_WITH_${_name})
        list(APPEND _pj_wasm_enabled_plugins "${_dir}=${_name}")
    endif()
endforeach()

# Whether the plugin in `dir` goes into the side module rather than the app.
function(_pj_wasm_in_side_module dir out)
    set(${out} OFF PARENT_SCOPE)
    if(PJ_WASM_ARROW_SIDE_MODULE AND dir IN_LIST PJ_WASM_ARROW_SIDE_MODULE_DIRS)
        set(${out} ON PARENT_SCOPE)
    endif()
endfunction()

# Includes the enabled plugins' wasm.cmake, after the dependencies they link exist. A macro:
# a plugin may call enable_language(), which must run at file scope. The side module's plugin
# targets are also listed in the GLOBAL property PJ_WASM_ARROW_SIDE_PLUGINS.
macro(pj_wasm_include_official_plugins)
    foreach(_pj_wasm_entry IN LISTS _pj_wasm_enabled_plugins)
        string(REGEX REPLACE "=.*" "" _pj_wasm_dir "${_pj_wasm_entry}")
        get_property(_pj_wasm_before GLOBAL PROPERTY PJ_WASM_OFFICIAL_PLUGINS)
        include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/${_pj_wasm_dir}/wasm.cmake")
        _pj_wasm_in_side_module(${_pj_wasm_dir} _pj_wasm_side)
        if(_pj_wasm_side)
            get_property(_pj_wasm_added GLOBAL PROPERTY PJ_WASM_OFFICIAL_PLUGINS)
            list(REMOVE_ITEM _pj_wasm_added ${_pj_wasm_before})
            set_property(GLOBAL APPEND PROPERTY PJ_WASM_ARROW_SIDE_PLUGINS ${_pj_wasm_added})
        endif()
    endforeach()
endmacro()

# Links every plugin into `target`, defines PJ_WASM_WITH_<NAME>_PLUGIN for each, and writes
# `inc`: the static getters' declarations and pj_wasm_official_plugins(PJ::StaticPluginSet&),
# which registers them in list order. The side module's plugins are left out.
function(pj_wasm_register_official_plugins target inc)
    foreach(_entry IN LISTS _pj_wasm_enabled_plugins)
        string(REGEX REPLACE "=.*" "" _dir "${_entry}")
        string(REGEX REPLACE ".*=" "" _name "${_entry}")
        _pj_wasm_in_side_module(${_dir} _side)
        if(NOT _side)
            target_compile_definitions(${target} PRIVATE PJ_WASM_WITH_${_name}_PLUGIN)
        endif()
    endforeach()
    get_property(_plugins GLOBAL PROPERTY PJ_WASM_OFFICIAL_PLUGINS)
    get_property(_side_plugins GLOBAL PROPERTY PJ_WASM_ARROW_SIDE_PLUGINS)
    if(_side_plugins)
        list(REMOVE_ITEM _plugins ${_side_plugins})
    endif()
    set(_decls "")
    set(_calls "")
    foreach(_plugin IN LISTS _plugins)
        target_link_libraries(${target} PUBLIC ${_plugin})
        get_target_property(_kind ${_plugin} PJ_WASM_PLUGIN_KIND)
        get_target_property(_symbol ${_plugin} PJ_WASM_PLUGIN_SYMBOL)
        get_target_property(_dialog ${_plugin} PJ_WASM_PLUGIN_DIALOG)
        set(_getter "pj_static_get_${_kind}_vtable_${_symbol}")
        set(_field "${_kind}s")
        if(_kind STREQUAL "toolbox")
            set(_field "toolboxes")
        endif()
        string(APPEND _decls "extern const PJ_${_kind}_vtable_t* ${_getter}() noexcept;\n")
        if(_dialog)
            set(_dialog_getter "pj_static_get_dialog_vtable_${_dialog}")
            string(APPEND _decls "extern const PJ_dialog_vtable_t* ${_dialog_getter}() noexcept;\n")
            string(APPEND _calls "  plugins.${_field}.emplace_back(${_getter}(), ${_dialog_getter}());\n")
        else()
            string(APPEND _calls "  plugins.${_field}.emplace_back(${_getter}());\n")
        endif()
    endforeach()
    # file(CONFIGURE) rewrites only on a change, so a reconfigure does not recompile its includer.
    file(CONFIGURE OUTPUT "${inc}" CONTENT
        "// Generated by cmake/PjWasmOfficialPlugins.cmake from PJ_WASM_OFFICIAL_PLUGIN_DIRS.\n${_decls}
static void pj_wasm_official_plugins([[maybe_unused]] PJ::StaticPluginSet& plugins) {\n${_calls}}\n" @ONLY)
endfunction()

# The side module plugins/arrow.wasm (PJ_WASM_ARROW_SIDE_MODULE): `target`, built from `entry` (its one
# export) and the side module's plugins, into `output_dir`. Its whole link closure is position
# independent with symbols hidden, Arrow too (PjWasmArrowDependencies.cmake); the rules it depends on:
# docs/WASM_DEPLOYMENT.md.
function(pj_wasm_add_arrow_side_module target entry output_dir)
    get_property(_side_plugins GLOBAL PROPERTY PJ_WASM_ARROW_SIDE_PLUGINS)
    add_executable(${target} "${entry}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_link_libraries(${target} PRIVATE ${_side_plugins} plotjuggler_sdk::plugin_sdk)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME arrow
        SUFFIX ".wasm"
        RUNTIME_OUTPUT_DIRECTORY "${output_dir}")
    pj_wasm_finalize_target(${target})
    _pj_wasm_hide_link_closure(${target})
    # Exports only its EMSCRIPTEN_KEEPALIVE entry point.
    target_link_options(${target} PRIVATE -sSIDE_MODULE=2)
endfunction()

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
