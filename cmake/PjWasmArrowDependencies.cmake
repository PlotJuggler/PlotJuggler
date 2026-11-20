# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# Apache Arrow C++ with Parquet, for parser_arrow, data_load_parquet and
# toolbox_data_exporter: about 1.4 MB of Brotli download for the three
# (docs/WASM_DEPLOYMENT.md). Arrow does not build as a CMake subproject, so it is
# built once into an install prefix with ExternalProject. Its CMake supports
# Emscripten (its own `ninja-release-emscripten` preset): single-threaded
# compute, no SIMD, static libraries.
include_guard(GLOBAL)
include(ExternalProject)
include(FetchContent)

FetchContent_Declare(arrow
    URL https://archive.apache.org/dist/arrow/arrow-26.0.1/apache-arrow-26.0.1.tar.gz
    URL_HASH SHA256=1dd1b6701d9de58a4ed82b107539682f46ef6b9e22747e18e03204125c05a987
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _pj_source_only_no_cmakelists)
FetchContent_MakeAvailable(arrow)

# Arrow downloads the sources it bundles while it builds, each pinned with a
# SHA256 in cpp/thirdparty/versions.txt. The ones compiled into the app are
# fetched here at those same pins too, for their notices and the source offer
# (scripts/wasm_source_offer.mjs reads these declarations as written, so they
# stay literal); the check below fails the configure if an Arrow bump moves one.
# simdjson is the exception: it runs ahead of Arrow's pin (ARROW_SIMDJSON_BUILD_VERSION), and ARROW_SIMDJSON_URL
# below makes Arrow build that newer copy.
# Boost contributes headers only to Thrift: it is declared for the source offer
# and never populated. RapidJSON is left out: only ARROW_JSON, off here, uses it.
set(_pj_thrift_sha256 e0fa5839a4c5c1d631b0931cf2c554ebbfa4e2fee3a9fb3ffd4f82ce4396c6e4)
set(_pj_snappy_sha256 90f74bc1fbf78a6c56b3c4a082a05103b3a56bb17bca1a27e052ea11723292dc)
set(_pj_simdjson_sha256 499176afd4b32fbe6f65551ef573b880407280072e1c19e22df0b45da58cdf64)
set(_pj_boost_sha256 f51707c27359a0df0cac1beada86de31bb5eed5e8285592dadec384df99c2984)
FetchContent_Declare(arrow_thrift
    URL https://dlcdn.apache.org/thrift/0.24.0/thrift-0.24.0.tar.gz
    URL_HASH SHA256=${_pj_thrift_sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _pj_source_only_no_cmakelists)
FetchContent_Declare(arrow_snappy
    URL https://github.com/google/snappy/archive/1.2.2.tar.gz
    URL_HASH SHA256=${_pj_snappy_sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _pj_source_only_no_cmakelists)
FetchContent_Declare(arrow_simdjson
    URL https://github.com/simdjson/simdjson/archive/refs/tags/v5.2.0.tar.gz
    URL_HASH SHA256=${_pj_simdjson_sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _pj_source_only_no_cmakelists)
FetchContent_Declare(arrow_boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.gz
    URL_HASH SHA256=${_pj_boost_sha256})
FetchContent_MakeAvailable(arrow_thrift arrow_snappy arrow_simdjson)

file(READ "${arrow_SOURCE_DIR}/cpp/thirdparty/versions.txt" _pj_arrow_versions)
foreach(_pj_name IN ITEMS THRIFT SNAPPY BOOST)
    string(TOLOWER "${_pj_name}" _pj_lower)
    if(NOT _pj_arrow_versions MATCHES "ARROW_${_pj_name}_BUILD_SHA256_CHECKSUM=${_pj_${_pj_lower}_sha256}")
        message(FATAL_ERROR "Arrow's bundled ${_pj_name} is no longer the one pinned in ${CMAKE_CURRENT_LIST_FILE}")
    endif()
endforeach()

# Arrow takes zstd and lz4 from the app (they are already linked for MCAP) and
# zlib from the Emscripten sysroot, rather than bundling second copies.
find_package(ZLIB 1.3.1 EXACT REQUIRED)
if(NOT TARGET libzstd_static OR NOT TARGET lz4_static)
    message(FATAL_ERROR "Arrow reuses the app's zstd and lz4 targets")
endif()

# As for Scene2D's codecs: the toolchain beside the compiler, never an empty
# one (which would silently build Arrow for the host).
get_filename_component(_pj_arrow_emscripten_root "${CMAKE_C_COMPILER}" DIRECTORY)
set(_pj_arrow_toolchain "${_pj_arrow_emscripten_root}/cmake/Modules/Platform/Emscripten.cmake")
if(NOT EXISTS "${_pj_arrow_toolchain}")
    message(FATAL_ERROR "Cannot locate Emscripten.cmake beside compiler ${CMAKE_C_COMPILER}")
endif()

set(_pj_arrow_prefix "${CMAKE_BINARY_DIR}/_deps/arrow-install")
set(_pj_arrow_libs
    "${_pj_arrow_prefix}/lib/libparquet.a"
    "${_pj_arrow_prefix}/lib/libarrow.a"
    "${_pj_arrow_prefix}/lib/libarrow_bundled_dependencies.a")
ExternalProject_Add(pj_wasm_arrow_ep
    SOURCE_DIR "${arrow_SOURCE_DIR}"
    SOURCE_SUBDIR cpp
    PREFIX "${CMAKE_BINARY_DIR}/_deps/arrow-ep"
    INSTALL_DIR "${_pj_arrow_prefix}"
    DEPENDS libzstd_static lz4_static
    CMAKE_GENERATOR Ninja
    # Arrow's bundled Thrift, Snappy and simdjson come from the copies fetched
    # above (a directory URL is copied, not downloaded) instead of a second
    # download. Boost is still Arrow's own: nothing here populates it.
    CMAKE_COMMAND ${CMAKE_COMMAND} -E env
        ARROW_THRIFT_URL=${arrow_thrift_SOURCE_DIR}
        ARROW_SNAPPY_URL=${arrow_snappy_SOURCE_DIR}
        ARROW_SIMDJSON_URL=${arrow_simdjson_SOURCE_DIR}
        ${CMAKE_COMMAND}
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${_pj_arrow_toolchain}
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
        -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
        # The app is threaded: every object it links must use shared memory.
        "-DCMAKE_C_FLAGS=-pthread"
        "-DCMAKE_CXX_FLAGS=-pthread"
        -DCMAKE_C_BYTE_ORDER=LITTLE_ENDIAN
        -DARROW_BUILD_STATIC=ON
        -DARROW_BUILD_SHARED=OFF
        -DARROW_BUILD_TESTS=OFF
        -DARROW_BUILD_UTILITIES=OFF
        -DARROW_TESTING=OFF
        -DARROW_DEPENDENCY_SOURCE=BUNDLED
        -DARROW_DEPENDENCY_USE_SHARED=OFF
        -DARROW_ENABLE_THREADING=OFF
        -DARROW_SIMD_LEVEL=NONE
        -DARROW_RUNTIME_SIMD_LEVEL=NONE
        -DARROW_JEMALLOC=OFF
        -DARROW_MIMALLOC=OFF
        -DARROW_IPC=ON
        -DARROW_PARQUET=ON
        -DPARQUET_REQUIRE_ENCRYPTION=OFF
        -DARROW_COMPUTE=OFF
        -DARROW_CSV=OFF
        -DARROW_JSON=OFF
        -DARROW_ACERO=OFF
        -DARROW_DATASET=OFF
        -DARROW_FILESYSTEM=OFF
        -DARROW_FLIGHT=OFF
        -DARROW_ORC=OFF
        -DARROW_SUBSTRAIT=OFF
        # The codecs the plugins' Conan recipe enables.
        -DARROW_WITH_SNAPPY=ON
        -DARROW_WITH_LZ4=ON
        -DARROW_WITH_ZSTD=ON
        -DARROW_WITH_ZLIB=OFF
        -DARROW_WITH_BROTLI=OFF
        -DARROW_WITH_BZ2=OFF
        -DARROW_WITH_UTF8PROC=OFF
        -DARROW_WITH_RE2=OFF
        -DARROW_WITH_OPENTELEMETRY=OFF
        -DZLIB_SOURCE=SYSTEM
        -Dzstd_SOURCE=SYSTEM
        -DZSTD_LIB=$<TARGET_FILE:libzstd_static>
        -DZSTD_INCLUDE_DIR=${zstd_SOURCE_DIR}/lib
        -DZSTD_ROOT=${zstd_SOURCE_DIR}/lib
        -Dlz4_SOURCE=SYSTEM
        -DLZ4_LIB=$<TARGET_FILE:lz4_static>
        -DLZ4_INCLUDE_DIR=${lz4_SOURCE_DIR}/lib
        -DLZ4_ROOT=${lz4_SOURCE_DIR}/lib
    BUILD_BYPRODUCTS ${_pj_arrow_libs})

file(MAKE_DIRECTORY "${_pj_arrow_prefix}/include")
add_library(pj_wasm_arrow INTERFACE)
add_dependencies(pj_wasm_arrow pj_wasm_arrow_ep)
target_include_directories(pj_wasm_arrow SYSTEM INTERFACE "${_pj_arrow_prefix}/include")
target_compile_definitions(pj_wasm_arrow INTERFACE ARROW_STATIC PARQUET_STATIC)
target_link_libraries(pj_wasm_arrow INTERFACE ${_pj_arrow_libs} zstd::libzstd_static LZ4::lz4_static ZLIB::ZLIB)
