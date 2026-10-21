# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# PJ_WASM_WITH_PYTHON: a static CPython for wasm32-emscripten (pthreads), linked into the browser executable for the
# on-demand Python lane. Python filters stay unavailable in the browser (pj_scripting/CMakeLists.txt).
#
# CPython builds at configure time from the pinned release tarball through wasm_python/build_cpython.sh (two local
# patches and a Setup.local, documented there) into PJ_WASM_PYTHON_STAGE:
#   include/python3.14/      headers and the host pyconfig.h
#   lib/*.a                  libpython and its builtins' native code (HACL* SHA-2, libmpdec)
#   lib/python314.zip        the pyc-only stdlib (wasm_python/stdlib_zip.py), embedded at /lib/python314.zip
#   THIRD_PARTY_NOTICES.txt  PSF-2.0, CPython's incorporated-software notices (libmpdec's BSD among them), HACL*'s MIT
# The stage is named by a key over the tarball hash, the Emscripten release and every wasm_python/ file, and is built
# (about 4 minutes) only when no stage of that key exists. CI keeps PJ_WASM_PYTHON_CACHE_DIR in its cache.
#
# Defines cpython::embed and pybind11::embed, the names the desktop graph uses, and PJ_WASM_PYTHON_STAGE.
include_guard(GLOBAL)

# CPython 3.14 is the first release with a supported (tier 3) Emscripten target. Bumping it: the tarball's hash, the
# patch (it must still apply) and the browser acceptance run (tests/wasm/python.spec.js).
set(_pj_cpython_version 3.14.8)
set(_pj_cpython_sha256 c2215904f02b175596dc49351585104f4bc20341e1c47378b26a2c274360ce73)

set(PJ_WASM_PYTHON_CACHE_DIR "${CMAKE_BINARY_DIR}/_deps/pj_wasm_cpython"
    CACHE PATH "Where the wasm CPython build keeps its stages; outside the build tree (as CI) they survive a clean build")

set(_pj_cpython_recipe "${CMAKE_CURRENT_LIST_DIR}/wasm_python")
get_filename_component(_pj_cpython_em_dir "${CMAKE_C_COMPILER}" DIRECTORY)
if(NOT EMSCRIPTEN_VERSION)
    message(FATAL_ERROR "PJ_WASM_WITH_PYTHON: the Emscripten toolchain did not report EMSCRIPTEN_VERSION")
endif()
# An edit to a recipe file, or a new one, reconfigures on the next build and so rebuilds the stage.
file(GLOB _pj_cpython_recipe_files CONFIGURE_DEPENDS "${_pj_cpython_recipe}/*")
list(SORT _pj_cpython_recipe_files)
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_pj_cpython_recipe_files})
set(_pj_cpython_key "${_pj_cpython_sha256};${EMSCRIPTEN_VERSION}")
foreach(_file IN LISTS _pj_cpython_recipe_files)
    file(SHA256 "${_file}" _hash)
    list(APPEND _pj_cpython_key "${_hash}")
endforeach()
string(SHA256 _pj_cpython_key "${_pj_cpython_key}")

# One directory per key, published whole by a rename under a per-key lock: build trees sharing the cache never see a
# partial stage nor have theirs replaced. ponytail: stages of old keys stay until the cache directory is cleared.
set(PJ_WASM_PYTHON_STAGE "${PJ_WASM_PYTHON_CACHE_DIR}/stage-${_pj_cpython_key}")
if(NOT IS_DIRECTORY "${PJ_WASM_PYTHON_STAGE}")
    file(MAKE_DIRECTORY "${PJ_WASM_PYTHON_CACHE_DIR}")
    file(LOCK "${PJ_WASM_PYTHON_STAGE}.lock" TIMEOUT 3600)
    if(NOT IS_DIRECTORY "${PJ_WASM_PYTHON_STAGE}")  # another configure may have published it while this one waited
        set(_pj_cpython_tarball "${CMAKE_BINARY_DIR}/_deps/Python-${_pj_cpython_version}.tar.xz")
        set(_hash "")
        if(EXISTS "${_pj_cpython_tarball}")
            file(SHA256 "${_pj_cpython_tarball}" _hash)
        endif()
        if(NOT _hash STREQUAL _pj_cpython_sha256)
            file(DOWNLOAD "https://www.python.org/ftp/python/${_pj_cpython_version}/Python-${_pj_cpython_version}.tar.xz"
                "${_pj_cpython_tarball}" EXPECTED_HASH SHA256=${_pj_cpython_sha256} STATUS _status)
            list(GET _status 0 _code)
            if(NOT _code EQUAL 0)
                message(FATAL_ERROR "Downloading CPython ${_pj_cpython_version} failed: ${_status}")
            endif()
        endif()
        if(DEFINED ENV{PJ_BUILD_JOBS})
            set(_jobs "$ENV{PJ_BUILD_JOBS}")
        else()
            cmake_host_system_information(RESULT _jobs QUERY NUMBER_OF_LOGICAL_CORES)
        endif()
        set(_log "${CMAKE_BINARY_DIR}/_deps/pj_wasm_cpython-build.log")
        message(STATUS "Building CPython ${_pj_cpython_version} for wasm32-emscripten (a few minutes, log: ${_log})")
        execute_process(
            COMMAND bash "${_pj_cpython_recipe}/build_cpython.sh" "${_pj_cpython_tarball}"
                "${CMAKE_BINARY_DIR}/_deps/pj_wasm_cpython-work" "${PJ_WASM_PYTHON_STAGE}.partial" "${_jobs}"
                "${_pj_cpython_em_dir}"
            RESULT_VARIABLE _result
            OUTPUT_FILE "${_log}"
            ERROR_FILE "${_log}")
        if(NOT _result EQUAL 0)
            file(READ "${_log}" _tail)
            string(LENGTH "${_tail}" _length)
            if(_length GREATER 6000)
                math(EXPR _start "${_length} - 6000")
                string(SUBSTRING "${_tail}" ${_start} -1 _tail)
            endif()
            message(FATAL_ERROR "Building CPython for wasm failed (${_result}); the end of ${_log}:\n${_tail}")
        endif()
        file(RENAME "${PJ_WASM_PYTHON_STAGE}.partial" "${PJ_WASM_PYTHON_STAGE}")
    endif()
    file(LOCK "${PJ_WASM_PYTHON_STAGE}.lock" RELEASE)
endif()

string(REGEX MATCH "^[0-9]+\\.[0-9]+" _pj_cpython_minor "${_pj_cpython_version}")
string(REPLACE "." "" _pj_cpython_nodot "${_pj_cpython_minor}")
add_library(cpython::embed INTERFACE IMPORTED)
target_include_directories(cpython::embed SYSTEM INTERFACE "${PJ_WASM_PYTHON_STAGE}/include/python${_pj_cpython_minor}")
# zlib: the sysroot port the app already links (CPython's zlib module).
target_link_libraries(cpython::embed INTERFACE
    "${PJ_WASM_PYTHON_STAGE}/lib/libpython${_pj_cpython_minor}.a"
    "${PJ_WASM_PYTHON_STAGE}/lib/libHacl_Hash_SHA2.a"
    "${PJ_WASM_PYTHON_STAGE}/lib/libmpdec.a"
    -sUSE_ZLIB=1)
target_link_options(cpython::embed INTERFACE
    "SHELL:--embed-file \"${PJ_WASM_PYTHON_STAGE}/lib/python${_pj_cpython_nodot}.zip@/lib/python${_pj_cpython_nodot}.zip\"")
set(PJ_WASM_PYTHON_NOTICES "${PJ_WASM_PYTHON_STAGE}/THIRD_PARTY_NOTICES.txt")

# pybind11 2.13 (the desktop pin) predates CPython 3.14; 3.0 supports it. Headers only.
FetchContent_Declare(pj_wasm_pybind11
    URL https://codeload.github.com/pybind/pybind11/tar.gz/refs/tags/v3.0.4
    URL_HASH SHA256=74b6a2c2b4573a400cafb6ecbf60c98df300cd3d0041296b913d02b2cbbb2676
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _pj_headers_only)
FetchContent_MakeAvailable(pj_wasm_pybind11)
add_library(pybind11::embed INTERFACE IMPORTED)
target_include_directories(pybind11::embed SYSTEM INTERFACE "${pj_wasm_pybind11_SOURCE_DIR}/include")
target_link_libraries(pybind11::embed INTERFACE cpython::embed)

unset(_file)
unset(_hash)
unset(_jobs)
unset(_log)
unset(_result)
unset(_status)
unset(_code)
unset(_tail)
unset(_length)
unset(_start)
unset(_pj_cpython_em_dir)
unset(_pj_cpython_key)
unset(_pj_cpython_minor)
unset(_pj_cpython_nodot)
unset(_pj_cpython_recipe)
unset(_pj_cpython_recipe_files)
unset(_pj_cpython_sha256)
unset(_pj_cpython_tarball)
unset(_pj_cpython_version)
