# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# THIRD_PARTY_NOTICES.txt of the browser build: every third-party library linked statically into plotjuggler4.wasm,
# with its SPDX licence expression and its licence text, copied from its source at configure time (Qt's from the kit's
# SBOM). scripts/package_wasm.mjs refuses a build without it. tests/wasm/wasm_notices.test.mjs fails when a
# FetchContent dependency of the WASM build has no pj_wasm_notice() entry here: a new one needs its entry. The helpers
# are shared with the desktop notices (cmake/PjNotices.cmake).
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/PjNotices.cmake)

# Qt: the modules and plugins on pj_app's link line, and the third-party code the SBOM attributes to them.
set(_PJ_QT_NOTICE_PACKAGES
    Core Concurrent Gui Network OpenGL OpenGLWidgets PrintSupport Widgets Xml
    BundledFreetype BundledHarfbuzz BundledLibjpeg BundledLibjpeg12bits BundledLibjpeg16bits BundledLibpng
    BundledPcre2 BundledZLIB
    QWasmIntegrationPlugin QGifPlugin QICOPlugin QJpegPlugin QTlsBackendCertOnlyPlugin
    Svg SvgWidgets QSvgPlugin QSvgIconPlugin
    UiTools
    Translations)

# Writes `output` for this configuration; `python_notices` is CPython's (PjWasmPython.cmake), empty without Python.
function(pj_wasm_write_notices output python_notices)
  get_filename_component(emscripten "${CMAKE_C_COMPILER}" DIRECTORY)
  file(WRITE "${output}"
      "Third-party software in PlotJuggler 4's browser build\n\n"
      "plotjuggler4.wasm, plotjuggler4.js and qtloader.js contain the libraries below, linked statically. PlotJuggler\n"
      "itself is MPL-2.0 (https://github.com/PlotJuggler/PlotJuggler). Each section names a library, its licence as an SPDX\n"
      "expression, and its copyright and licence text as its source distributes them.\n\n"
      "Licence of this bundle. The browser build (plotjuggler4.wasm, plotjuggler4.js, qtloader.js) is a combined work\n"
      "conveyed under the GNU General Public License version 3, because it statically links Qt's WebAssembly platform\n"
      "plugin, which Qt provides only under the GPL version 3 or a commercial licence. The GPL version 3 text is the\n"
      "GPL-3.0-only entry of the Qt section below. PlotJuggler 4's own source is MPL-2.0, which permits this\n"
      "combination; the desktop application remains MPL-2.0. sources/SOURCE-OFFER.txt, next to this deployment,\n"
      "names the complete corresponding source, PlotJuggler 4's included, and where each part is; it is also\n"
      "available on written request via https://github.com/PlotJuggler/PlotJuggler/issues.\n\n"
      "The libraries below keep their own licences. Qt is used under the GNU LGPL version 3, except its WebAssembly\n"
      "platform plugin, as above. FFmpeg (libavcodec, libavutil, libswscale), built LGPL-only, and the Qt Advanced\n"
      "Docking System, PlotJuggler's fork, are used under the LGPL version 2.1 or later. PlotJuggler 4's source pins\n"
      "the toolchain and the commands that build this module (versions.env, .github/workflows/wasm-ci.yml), so it can\n"
      "be rebuilt against a modified copy of any of them.\n")
  _pj_qt_notices("${output}" "LGPL-3.0-only (the qwasm platform plugin: GPL-3.0-only; also offered commercially)"
      "${_PJ_QT_NOTICE_PACKAGES}" qtbase qtsvg qttools qttranslations)

  # The Emscripten runtime: its own code, musl and LLVM's C++ runtime, and the sysroot zlib.
  _pj_notice_section("${output}" "Emscripten ${EMSCRIPTEN_VERSION} runtime" "MIT OR NCSA" "${emscripten}/LICENSE")
  _pj_notice_section("${output}" "musl libc (Emscripten's libc)" "MIT" "${emscripten}/system/lib/libc/musl/COPYRIGHT")
  _pj_notice_section("${output}" "LLVM libc++, libc++abi, libunwind and compiler-rt (Emscripten's)"
      "Apache-2.0 WITH LLVM-exception" "${emscripten}/system/lib/libcxx/LICENSE.TXT")
  if(DEFINED ZLIB_INCLUDE_DIR OR PJ_WASM_WITH_PYTHON)
    find_file(_pj_notice_zlib_h zlib.h PATHS "${emscripten}/cache/sysroot/include" NO_DEFAULT_PATH NO_CACHE REQUIRED)
    _pj_notice_section("${output}" "zlib (Emscripten's sysroot port)" "Zlib" "HEADER|${_pj_notice_zlib_h}")
  endif()

  _pj_vendored_notices("${output}")
  _pj_notice_section("${output}" "Noto Sans SC (browser CJK subset)" "OFL-1.1" "${CMAKE_SOURCE_DIR}/resources/fonts/NotoSansSC/OFL.txt")
  _pj_notice_section("${output}" "Noto Sans Symbols 2 (browser symbol subset)" "OFL-1.1" "${CMAKE_SOURCE_DIR}/resources/fonts/NotoSansSymbols2/OFL.txt")

  # Fetched: the name is the FetchContent name, the files are in its source tree.
  pj_wasm_notice("${output}" plotjuggler_sdk "PlotJuggler SDK (pj_base, pj_plugins)" "Apache-2.0" LICENSE LICENSE-APACHE)
  pj_wasm_notice("${output}" pj_official_plugins "PlotJuggler official plugins" "MIT" LICENSE)
  pj_wasm_notice("${output}" pj_official_plugins "liblzf decompressor (in the PCD/PLY loader)" "BSD-2-Clause"
      data_load_3d/contrib/liblzf/LICENSE)
  pj_wasm_notice("${output}" pj_official_plugins "mcap (in the MCAP loader)" "MIT" data_load_mcap/contrib/mcap/LICENSE)
  pj_wasm_notice("${output}" pj_official_plugins "nanocdr (in the NavMap parser)" "Apache-2.0"
      parser_navmap/contrib/nanocdr/LICENSE)
  pj_wasm_notice("${output}" qwt_upstream "Qwt 6.3.0" "LGPL-2.1-only WITH Qwt-exception-1.0" COPYING)
  pj_wasm_notice("${output}" nlohmann_json "nlohmann/json" "MIT" LICENSE.MIT)
  pj_wasm_notice("${output}" fmt "{fmt}" "MIT" LICENSE)
  pj_wasm_notice("${output}" FastFloat "fast_float" "MIT OR Apache-2.0 OR BSL-1.0" LICENSE-MIT)
  pj_wasm_notice("${output}" tsl-robin-map "tsl::robin_map" "MIT" LICENSE)
  pj_wasm_notice("${output}" nanoarrow "Apache Arrow nanoarrow (with flatcc)" "Apache-2.0" LICENSE.txt NOTICE.txt)
  pj_wasm_notice("${output}" glm "GLM" "MIT" copying.txt)
  pj_wasm_notice("${output}" date "date (Howard Hinnant)" "MIT" LICENSE.txt)
  pj_wasm_notice("${output}" protobuf "Protocol Buffers" "BSD-3-Clause" LICENSE)
  pj_wasm_notice("${output}" protobuf "utf8_range (in Protocol Buffers)" "MIT" third_party/utf8_range/LICENSE)
  pj_wasm_notice("${output}" absl "Abseil (Protocol Buffers' dependency)" "Apache-2.0" LICENSE)
  pj_wasm_notice("${output}" rosx_introspection "rosx_introspection" "MPL-2.0" LICENSE)
  # Its contrib/ headers are compiled in; the first two name their licence without carrying it.
  pj_wasm_notice("${output}" rosx_introspection "cpp-peglib (in rosx_introspection)" "MIT"
      "HEADER|include/rosx_introspection/contrib/peglib.h" "${CMAKE_SOURCE_DIR}/cmake/notices/MIT.txt")
  pj_wasm_notice("${output}" rosx_introspection "LLVM SmallVector (in rosx_introspection)" "NCSA"
      "HEADER|include/rosx_introspection/contrib/SmallVector.h" "${CMAKE_SOURCE_DIR}/cmake/notices/NCSA.txt")
  pj_wasm_notice("${output}" rosx_introspection "nanocdr (in rosx_introspection)" "Apache-2.0"
      "HEADER|include/rosx_introspection/contrib/nanocdr.hpp")
  pj_wasm_notice("${output}" data_tamer "Data Tamer" "MIT" LICENSE)
  pj_wasm_notice("${output}" ulog_cpp "ulog_cpp (PX4)" "BSD-3-Clause" LICENSE)
  pj_wasm_notice("${output}" dbc_parser_cpp "dbc_parser_cpp (in the CAN loaders)" "MIT" LICENSE)
  pj_wasm_notice("${output}" dbc_fast_float "fast_float 3.8 (in dbc_parser_cpp)" "MIT OR Apache-2.0 OR BSL-1.0" LICENSE-MIT)
  pj_wasm_notice("${output}" lblf "lblf (in the BLF loader)" "MIT" LICENSE)
  pj_wasm_notice("${output}" mdflib "mdflib (in the MF4 loader)" "MIT" LICENSE)
  pj_wasm_notice("${output}" expat "Expat" "MIT" COPYING)
  # Arrow's LICENSE.txt also carries the code it vendors (fast_float, double-conversion, flatbuffers...).
  pj_wasm_notice("${output}" arrow "Apache Arrow C++ and Parquet" "Apache-2.0" LICENSE.txt NOTICE.txt)
  pj_wasm_notice("${output}" arrow_thrift "Apache Thrift (in Parquet)" "Apache-2.0" LICENSE NOTICE)
  pj_wasm_notice("${output}" arrow_snappy "Snappy (in Arrow)" "BSD-3-Clause" COPYING)
  pj_wasm_notice("${output}" arrow_simdjson "simdjson (in Arrow)" "Apache-2.0" LICENSE)
  pj_wasm_notice("${output}" arrow_uriparser "uriparser (in Arrow)" "BSD-3-Clause" src/COPYING)
  if(PJ_WASM_WITH_ARROW)
    # Headers only, through Thrift; declared for the source offer and never populated.
    pj_wasm_notice("${output}" arrow_boost "Boost (headers, in Thrift)" "BSL-1.0"
                   "${CMAKE_SOURCE_DIR}/cmake/notices/BSL-1.0.txt")
  endif()
  pj_wasm_notice("${output}" lz4 "LZ4 library" "BSD-2-Clause" lib/LICENSE)
  pj_wasm_notice("${output}" zstd "Zstandard" "BSD-3-Clause" LICENSE)
  pj_wasm_notice("${output}" Luau "Luau" "MIT" LICENSE.txt)
  pj_wasm_notice("${output}" pocketfft "pocketfft" "BSD-3-Clause" LICENSE.md)
  pj_wasm_notice("${output}" pj_wasm_libjpeg_turbo "libjpeg-turbo" "IJG AND BSD-3-Clause AND Zlib" LICENSE.md README.ijg)
  pj_wasm_notice("${output}" pj_wasm_libpng "libpng" "libpng-2.0" LICENSE)
  pj_wasm_notice("${output}" pj_wasm_ffmpeg "FFmpeg (libavcodec, libavutil, libswscale)" "LGPL-2.1-or-later"
      LICENSE.md COPYING.LGPLv2.1)
  pj_wasm_notice("${output}" pj_wasm_dav1d "dav1d" "BSD-2-Clause" COPYING)
  pj_wasm_notice("${output}" assimp "Open Asset Import Library (assimp)" "BSD-3-Clause" LICENSE)
  pj_wasm_notice("${output}" assimp "Clipper (in assimp)" "BSL-1.0" contrib/clipper/License.txt)
  pj_wasm_notice("${output}" assimp "OpenDDL-Parser (in assimp)" "MIT" contrib/openddlparser/LICENSE)
  pj_wasm_notice("${output}" assimp "poly2tri (in assimp)" "BSD-3-Clause" contrib/poly2tri/LICENSE)
  pj_wasm_notice("${output}" assimp "minizip unzip (in assimp)" "Zlib" "HEADER|contrib/unzip/unzip.h")
  pj_wasm_notice("${output}" assimp "pugixml (in assimp)" "MIT" contrib/pugixml/readme.txt)
  pj_wasm_notice("${output}" assimp "RapidJSON (in assimp)" "MIT" contrib/rapidjson/license.txt)
  pj_wasm_notice("${output}" assimp "stb_image (in assimp)" "MIT OR Unlicense"
      "FROM|This software is available under 2 licenses|contrib/stb/stb_image.h")
  pj_wasm_notice("${output}" assimp "UTF8-CPP (in assimp)" "BSL-1.0" contrib/utf8cpp/doc/LICENSE)
  pj_wasm_notice("${output}" draco "Draco" "Apache-2.0" LICENSE)
  pj_wasm_notice("${output}" cloudini "Cloudini" "Apache-2.0" LICENSE)
  pj_wasm_notice("${output}" cloudini "unordered_dense (in Cloudini)" "MIT"
      "HEADER|cloudini_lib/include/cloudini_lib/contrib/ankerl/unordered_dense.h")
  pj_wasm_notice("${output}" pj_wasm_pybind11 "pybind11" "BSD-3-Clause" LICENSE)
  if(python_notices)
    _pj_notice_section("${output}" "CPython and the software it incorporates" "PSF-2.0" "${python_notices}")
  endif()
endfunction()
