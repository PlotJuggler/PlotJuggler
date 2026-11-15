# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# Cross-compiled FFmpeg for Scene2D video in the browser.
#
# Mirrors the desktop graph's LGPL-lean, decode-only option set (conanfile.txt's
# ffmpeg/* block and recipes/ffmpeg): the same FFmpeg release, the same decoder,
# parser and bitstream-filter allowlist, dav1d for AV1, --disable-autodetect so
# nothing unaudited links. Browser differences: static archives, no hardware
# acceleration (no native decode API exists), no assembly (it targets native
# CPUs, not wasm), and no libavformat (the Scene2D core decodes elementary
# streams; only its desktop tests demux containers).
include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_SCENE2D OR NOT PJ_WASM_WITH_VIDEO)
    return()
endif()

include(FetchContent)
include(ExternalProject)

find_program(PJ_WASM_MESON meson REQUIRED)
find_program(PJ_WASM_MAKE NAMES gmake make REQUIRED)
find_program(PJ_WASM_PKG_CONFIG NAMES pkg-config pkgconf REQUIRED)

# Same release and archive as recipes/ffmpeg/recipe.yaml.
FetchContent_Declare(pj_wasm_ffmpeg
    URL https://ffmpeg.org/releases/ffmpeg-9.0.1.tar.xz
    URL_HASH SHA256=cf38e0e28c7e5605942c4a77755349b0145804a397af37eb1fb4c77cb237f635
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _pj_source_only_no_cmakelists)
# Same release as conan.lock's dav1d, pinned to the tag's commit.
FetchContent_Declare(pj_wasm_dav1d
    GIT_REPOSITORY https://github.com/videolan/dav1d.git
    GIT_TAG 54706fc6bc0cdecab7e9593974a4039cc038fca7  # 1.5.4
    GIT_SHALLOW FALSE
    SOURCE_SUBDIR _pj_source_only_no_cmakelists)
FetchContent_MakeAvailable(pj_wasm_ffmpeg pj_wasm_dav1d)

get_filename_component(_pj_wasm_em_bin "${CMAKE_C_COMPILER}" DIRECTORY)
set(_pj_wasm_emcc "${_pj_wasm_em_bin}/emcc")
set(_pj_wasm_emxx "${_pj_wasm_em_bin}/em++")
set(_pj_wasm_emar "${_pj_wasm_em_bin}/emar")
set(_pj_wasm_emranlib "${_pj_wasm_em_bin}/emranlib")
set(_pj_wasm_emnm "${_pj_wasm_em_bin}/emnm")
foreach(_tool IN ITEMS _pj_wasm_emcc _pj_wasm_emxx _pj_wasm_emar _pj_wasm_emranlib _pj_wasm_emnm)
    if(NOT EXISTS "${${_tool}}")
        message(FATAL_ERROR "Cannot locate ${${_tool}} beside compiler ${CMAKE_C_COMPILER}")
    endif()
endforeach()

# One prefix holds both libraries, so FFmpeg's configure finds dav1d through
# pkg-config there and nowhere else.
set(_pj_wasm_video_prefix "${CMAKE_BINARY_DIR}/_deps/pj_wasm_video-prefix")
set(_pj_wasm_dav1d_cross "${CMAKE_BINARY_DIR}/_deps/pj_wasm_dav1d-emscripten.cross")
file(WRITE "${_pj_wasm_dav1d_cross}"
"[binaries]
c = '${_pj_wasm_emcc}'
cpp = '${_pj_wasm_emxx}'
ar = '${_pj_wasm_emar}'
ranlib = '${_pj_wasm_emranlib}'

[built-in options]
c_args = ['-pthread']
c_link_args = ['-pthread']

[host_machine]
system = 'emscripten'
cpu_family = 'wasm32'
cpu = 'wasm32'
endian = 'little'
")

set(_pj_wasm_dav1d_archive "${_pj_wasm_video_prefix}/lib/libdav1d.a")
ExternalProject_Add(pj_wasm_dav1d_build
    SOURCE_DIR "${pj_wasm_dav1d_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/pj_wasm_dav1d-isolated-build"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    CONFIGURE_COMMAND "${PJ_WASM_MESON}" setup --reconfigure <BINARY_DIR> <SOURCE_DIR>
        --cross-file "${_pj_wasm_dav1d_cross}"
        --prefix "${_pj_wasm_video_prefix}" --libdir lib
        --buildtype release --default-library static
        -Denable_asm=false -Denable_tools=false -Denable_tests=false
        -Denable_examples=false -Denable_docs=false -Dlogging=false
        -Dbitdepths=8,16
    BUILD_COMMAND "${PJ_WASM_MESON}" compile -C <BINARY_DIR>
    INSTALL_COMMAND "${PJ_WASM_MESON}" install -C <BINARY_DIR> --quiet
    BUILD_BYPRODUCTS "${_pj_wasm_dav1d_archive}"
    TEST_COMMAND ""
    EXCLUDE_FROM_ALL TRUE)

set(_pj_wasm_ffmpeg_libs avcodec swscale avutil)
set(_pj_wasm_ffmpeg_archives "")
foreach(_lib IN LISTS _pj_wasm_ffmpeg_libs)
    list(APPEND _pj_wasm_ffmpeg_archives "${_pj_wasm_video_prefix}/lib/lib${_lib}.a")
endforeach()
ExternalProject_Add(pj_wasm_ffmpeg_build
    SOURCE_DIR "${pj_wasm_ffmpeg_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/pj_wasm_ffmpeg-isolated-build"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    DEPENDS pj_wasm_dav1d_build
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
        "PKG_CONFIG_LIBDIR=${_pj_wasm_video_prefix}/lib/pkgconfig" "PKG_CONFIG_PATH="
        <SOURCE_DIR>/configure
        "--prefix=${_pj_wasm_video_prefix}"
        --enable-cross-compile --target-os=none --arch=x86_32 --cpu=generic
        "--cc=${_pj_wasm_emcc}" "--cxx=${_pj_wasm_emxx}" "--ar=${_pj_wasm_emar}"
        "--ranlib=${_pj_wasm_emranlib}" "--nm=${_pj_wasm_emnm}" "--pkg-config=${PJ_WASM_PKG_CONFIG}"
        --disable-asm --disable-inline-asm --disable-x86asm --disable-runtime-cpudetect
        --enable-pthreads --disable-w32threads --disable-os2threads
        "--extra-cflags=-pthread -O3" "--extra-ldflags=-pthread"
        # The LGPL-critical set shared with recipes/ffmpeg/build_ffmpeg.sh.
        --disable-gpl --disable-nonfree --disable-autodetect --disable-iconv
        --enable-static --disable-shared --disable-programs --disable-doc --disable-debug
        --enable-avcodec --enable-swscale --disable-avformat
        --disable-avdevice --disable-avfilter --disable-swresample --disable-network
        --enable-libdav1d
        --disable-encoders --disable-muxers --disable-demuxers --disable-protocols
        --disable-decoders
        --enable-decoder=h264 --enable-decoder=hevc --enable-decoder=mjpeg
        --enable-decoder=av1 --enable-decoder=libdav1d
        --disable-parsers
        --enable-parser=h264 --enable-parser=hevc --enable-parser=mjpeg --enable-parser=av1
        --disable-bsfs --enable-bsf=h264_mp4toannexb --enable-bsf=hevc_mp4toannexb
    BUILD_COMMAND "${PJ_WASM_MAKE}" -j4
    INSTALL_COMMAND "${PJ_WASM_MAKE}" install
    BUILD_BYPRODUCTS ${_pj_wasm_ffmpeg_archives}
    TEST_COMMAND ""
    EXCLUDE_FROM_ALL TRUE)

# Imported targets with the names the native Conan graph provides, so
# pj_scene2d_core links them unchanged. Static archives carry no dependency
# metadata: spell out avcodec/swscale -> avutil and avcodec -> dav1d.
file(MAKE_DIRECTORY "${_pj_wasm_video_prefix}/include")
add_library(pj_wasm_dav1d STATIC IMPORTED GLOBAL)
set_target_properties(pj_wasm_dav1d PROPERTIES IMPORTED_LOCATION "${_pj_wasm_dav1d_archive}")
add_dependencies(pj_wasm_dav1d pj_wasm_dav1d_build)
foreach(_lib IN LISTS _pj_wasm_ffmpeg_libs)
    add_library(ffmpeg::${_lib} STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg::${_lib} PROPERTIES
        IMPORTED_LOCATION "${_pj_wasm_video_prefix}/lib/lib${_lib}.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_pj_wasm_video_prefix}/include")
    add_dependencies(ffmpeg::${_lib} pj_wasm_ffmpeg_build)
endforeach()
set_target_properties(ffmpeg::avcodec PROPERTIES INTERFACE_LINK_LIBRARIES "ffmpeg::avutil;pj_wasm_dav1d")
set_target_properties(ffmpeg::swscale PROPERTIES INTERFACE_LINK_LIBRARIES "ffmpeg::avutil")

unset(_lib)
unset(_tool)
unset(_pj_wasm_ffmpeg_archives)
unset(_pj_wasm_ffmpeg_libs)
unset(_pj_wasm_dav1d_archive)
unset(_pj_wasm_dav1d_cross)
unset(_pj_wasm_video_prefix)
unset(_pj_wasm_em_bin)
unset(_pj_wasm_emcc)
unset(_pj_wasm_emxx)
unset(_pj_wasm_emar)
unset(_pj_wasm_emranlib)
unset(_pj_wasm_emnm)
