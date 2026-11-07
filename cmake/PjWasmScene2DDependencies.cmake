# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# Exact, cross-compiled image dependencies for the Qt-free Scene2D core.
#
# The desktop graph continues to consume Conan's libjpeg-turbo/libpng packages.
# The browser graph cannot use those native binaries, so build the same codec
# releases from immutable, hash-verified archives with the Emscripten toolchain.
include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_SCENE2D)
    return()
endif()

include(FetchContent)
include(ExternalProject)

# The root project is otherwise C++-only. These two upstream libraries are C,
# and enabling C only in the Scene2D/WASM configuration keeps desktop and the
# default Scene2D-off browser graph byte-for-byte unchanged.
enable_language(C)

# libpng's only dependency is Emscripten's sysroot zlib. Fail closed if the
# pinned Qt/Emscripten toolchain stops providing the version used by the native
# dependency graph.
find_package(ZLIB 1.3.1 EXACT REQUIRED)

# Match conanfile.txt exactly. libjpeg-turbo deliberately rejects integration
# through add_subdirectory(), so populate its verified source at configure time
# and use the isolated build recommended by upstream. Only the static TurboJPEG
# API target is built; the shared libraries, tools, tests, Java bindings, and
# architecture-specific assembly remain disabled.
find_file(_pj_wasm_qt_jpeg_config
    NAMES QtJpeg/jconfig.h
    PATH_SUFFIXES include
    NO_CACHE)
if(NOT _pj_wasm_qt_jpeg_config)
    message(FATAL_ERROR
        "WASM Scene2D requires the Qt kit's static JPEG ABI declaration")
endif()
file(STRINGS "${_pj_wasm_qt_jpeg_config}" _pj_wasm_qt_jpeg_abi_line
    REGEX "^#define JPEG_LIB_VERSION +[0-9]+$"
    LIMIT_COUNT 1)
if(NOT _pj_wasm_qt_jpeg_abi_line MATCHES "JPEG_LIB_VERSION +80$")
    message(FATAL_ERROR
        "WASM Scene2D requires Qt's JPEG v8 ABI; found '${_pj_wasm_qt_jpeg_abi_line}' "
        "in ${_pj_wasm_qt_jpeg_config}")
endif()

FetchContent_Declare(pj_wasm_libjpeg_turbo
    URL https://codeload.github.com/libjpeg-turbo/libjpeg-turbo/tar.gz/refs/tags/3.1.4.1
    URL_HASH SHA256=a7da42b640377c2a9a9665e2c4b0ea60cd5599afb48c2521e6df0c9dc9d15a25
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _pj_source_only_no_cmakelists
    SYSTEM)
FetchContent_MakeAvailable(pj_wasm_libjpeg_turbo)

get_filename_component(_pj_wasm_emscripten_root "${CMAKE_C_COMPILER}" DIRECTORY)
set(_pj_wasm_emscripten_toolchain
    "${_pj_wasm_emscripten_root}/cmake/Modules/Platform/Emscripten.cmake")
if(NOT EXISTS "${_pj_wasm_emscripten_toolchain}")
    message(FATAL_ERROR
        "Cannot locate Emscripten.cmake beside compiler ${CMAKE_C_COMPILER}")
endif()

set(_pj_wasm_turbojpeg_binary_dir
    "${CMAKE_BINARY_DIR}/_deps/pj_wasm_libjpeg_turbo-isolated-build")
set(_pj_wasm_turbojpeg_archive
    "${_pj_wasm_turbojpeg_binary_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}turbojpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
ExternalProject_Add(pj_wasm_libjpeg_turbo_build
    SOURCE_DIR "${pj_wasm_libjpeg_turbo_SOURCE_DIR}"
    BINARY_DIR "${_pj_wasm_turbojpeg_binary_dir}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_CACHE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${_pj_wasm_emscripten_toolchain}"
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}"
        "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
        "-DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS} -pthread"
        "-DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON"
        "-DENABLE_SHARED:BOOL=OFF"
        "-DENABLE_STATIC:BOOL=ON"
        "-DWITH_TOOLS:BOOL=OFF"
        "-DWITH_TESTS:BOOL=OFF"
        "-DWITH_JAVA:BOOL=OFF"
        "-DWITH_SIMD:BOOL=OFF"
        "-DWITH_TURBOJPEG:BOOL=ON"
        # Qt's static image plugin and TurboJPEG share process-global jpeg_*
        # symbols in a monolithic WASM module. Match the enclosing Qt kit's
        # checked ABI so either archive can safely satisfy those references.
        "-DWITH_JPEG8:BOOL=ON"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target turbojpeg-static
    BUILD_BYPRODUCTS "${_pj_wasm_turbojpeg_archive}"
    INSTALL_COMMAND ""
    TEST_COMMAND ""
    EXCLUDE_FROM_ALL TRUE)

add_library(pj_wasm_turbojpeg STATIC IMPORTED GLOBAL)
set_target_properties(pj_wasm_turbojpeg PROPERTIES
    IMPORTED_LOCATION "${_pj_wasm_turbojpeg_archive}"
    INTERFACE_INCLUDE_DIRECTORIES
        "${pj_wasm_libjpeg_turbo_SOURCE_DIR}/src;${_pj_wasm_turbojpeg_binary_dir}")
add_dependencies(pj_wasm_turbojpeg pj_wasm_libjpeg_turbo_build)

# Present the same target contract consumed by pj_scene2d_core on desktop.
add_library(libjpeg-turbo::libjpeg-turbo INTERFACE IMPORTED GLOBAL)
target_link_libraries(libjpeg-turbo::libjpeg-turbo INTERFACE pj_wasm_turbojpeg)
unset(_pj_wasm_turbojpeg_archive)
unset(_pj_wasm_turbojpeg_binary_dir)
unset(_pj_wasm_emscripten_root)
unset(_pj_wasm_emscripten_toolchain)
unset(_pj_wasm_qt_jpeg_abi_line)
unset(_pj_wasm_qt_jpeg_config)

# Match conanfile.txt exactly. Use the generic C implementation: the upstream
# hardware selectors target native CPU instruction sets, not wasm SIMD.
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
# Keep the deprecated compatibility switch at its upstream default. Setting it
# OFF emits a deprecation warning and would only duplicate PNG_TOOLS=OFF.
set(PNG_EXECUTABLES ON CACHE BOOL "" FORCE)
set(PNG_HARDWARE_OPTIMIZATIONS OFF CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(pj_wasm_libpng
    URL https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.58
    URL_HASH SHA256=a9d4df463d36a6e5f9c29bd6f4967312d17e996c1854f3511f833924eb1993cf
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(pj_wasm_libpng)

if(NOT TARGET png_static)
    message(FATAL_ERROR "WASM Scene2D requires libpng's png_static target")
endif()
set_target_properties(png_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(PNG::PNG ALIAS png_static)

message(STATUS
    "WASM Scene2D codecs: libjpeg-turbo 3.1.4.1, libpng 1.6.58, zlib ${ZLIB_VERSION}")

include(${CMAKE_CURRENT_LIST_DIR}/PjWasmFfmpeg.cmake)
