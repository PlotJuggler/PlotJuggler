# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# THIRD_PARTY_NOTICES.txt of the browser build: every third-party library linked statically into plotjuggler4.wasm,
# with its SPDX licence expression and its licence text, copied from its source at configure time (Qt's from the kit's
# SBOM). scripts/package_wasm.mjs refuses a build without it. tests/wasm/wasm_notices.test.mjs fails when a
# FetchContent dependency of the WASM build has no pj_wasm_notice() entry here: a new one needs its entry.
include_guard(GLOBAL)
include(FetchContent)

set(_PJ_NOTICE_RULE "==============================================================================")

# The text of `spec` for a notice: a file, `HEADER|<file>` for its leading comment block (a C /* */ block or its //
# lines), `FROM|<marker>|<file>` for the file from the first line holding the marker.
function(_pj_notice_text spec out)
  string(REPLACE "|" ";" parts "${spec}")
  list(LENGTH parts count)
  list(GET parts -1 file)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "Third-party notices: ${file} does not exist")
  endif()
  file(READ "${file}" text)
  # peglib.h starts with a UTF-8 byte order mark, which would hide its leading comment block.
  string(ASCII 239 187 191 bom)
  if(text MATCHES "^${bom}")
    string(SUBSTRING "${text}" 3 -1 text)
  endif()
  if(count EQUAL 1)
    set(${out} "${text}" PARENT_SCOPE)
    return()
  endif()
  list(GET parts 0 mode)
  string(STRIP "${text}" stripped)
  if(mode STREQUAL "HEADER" AND stripped MATCHES "^/\\*")
    string(FIND "${stripped}" "*/" end)
    math(EXPR end "${end} + 2")
    string(SUBSTRING "${stripped}" 0 ${end} text)
    set(${out} "${text}" PARENT_SCOPE)
  elseif(mode STREQUAL "HEADER" AND stripped MATCHES "^((//[^\n]*\n)+)")
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  elseif(mode STREQUAL "FROM")
    list(GET parts 1 marker)
    string(FIND "${text}" "${marker}" start)
    if(start LESS 0)
      message(FATAL_ERROR "Third-party notices: '${marker}' is not in ${file}")
    endif()
    string(SUBSTRING "${text}" ${start} -1 text)
    set(${out} "${text}" PARENT_SCOPE)
  else()
    message(FATAL_ERROR "Third-party notices: no ${mode} section in ${file}")
  endif()
endfunction()

# Appends one section: a title, its SPDX expression and the texts of `ARGN` (absolute specs).
function(_pj_notice_section output title spdx)
  file(APPEND "${output}" "\n${_PJ_NOTICE_RULE}\n${title}\nSPDX-License-Identifier: ${spdx}\n${_PJ_NOTICE_RULE}\n")
  foreach(spec IN LISTS ARGN)
    _pj_notice_text("${spec}" text)
    file(APPEND "${output}" "\n${text}\n")
  endforeach()
endfunction()

# One FetchContent dependency, its licence files relative to its source tree (an absolute path, such as a text of
# cmake/wasm_notices/ for a header that only names its licence, is used as is). Nothing when this configuration does
# not populate it (its option is off).
function(pj_wasm_notice output name title spdx)
  FetchContent_GetProperties(${name} POPULATED populated SOURCE_DIR source)
  if(NOT populated)
    return()
  endif()
  set(specs "")
  foreach(spec IN LISTS ARGN)
    string(REGEX MATCH "[^|]+$" file "${spec}")
    if(NOT IS_ABSOLUTE "${file}")
      string(REGEX REPLACE "([^|]+)$" "${source}/\\1" spec "${spec}")
    endif()
    list(APPEND specs "${spec}")
  endforeach()
  _pj_notice_section("${output}" "${title}" "${spdx}" ${specs})
endfunction()

# Qt: the modules and plugins on pj_app's link line, and the third-party code the SBOM attributes to them, with each
# one's licence and copyright. A LicenseRef's text comes from the SBOM; the text of every standard SPDX id (an exception
# after WITH included) any of them names is cmake/wasm_notices/<id>.txt, so a new id fails the configure until its text
# is added.
set(_PJ_QT_NOTICE_PACKAGES
    Core Concurrent Gui Network OpenGL OpenGLWidgets PrintSupport Widgets Xml
    BundledFreetype BundledHarfbuzz BundledLibjpeg BundledLibjpeg12bits BundledLibjpeg16bits BundledLibpng
    BundledPcre2 BundledZLIB
    QWasmIntegrationPlugin QGifPlugin QICOPlugin QJpegPlugin QTlsBackendCertOnlyPlugin
    Svg SvgWidgets QSvgPlugin QSvgIconPlugin
    UiTools)

function(_pj_qt_notices output)
  get_filename_component(sbom_dir "${Qt6_DIR}/../../../sbom" ABSOLUTE)
  file(APPEND "${output}" "\n${_PJ_NOTICE_RULE}\nQt ${Qt6_VERSION}\n"
      "SPDX-License-Identifier: LGPL-3.0-only (the qwasm platform plugin: GPL-3.0-only; also offered commercially)\n"
      "${_PJ_NOTICE_RULE}\n\n"
      "Copyright (C) The Qt Company Ltd. and other contributors. Modules and the third-party code they contain, from\n"
      "the Qt kit's SBOM (sbom/<module>-${Qt6_VERSION}.spdx.json); the text of each standard licence they name\n"
      "follows the list, once.\n")
  set(license_refs "")
  set(license_ids "")
  foreach(module IN ITEMS qtbase qtsvg qttools)
    set(sbom "${sbom_dir}/${module}-${Qt6_VERSION}.spdx.json")
    if(NOT EXISTS "${sbom}")
      message(FATAL_ERROR "Third-party notices: the Qt kit has no SBOM ${sbom}")
    endif()
    file(READ "${sbom}" json)
    string(JSON packages GET "${json}" packages)
    string(JSON count LENGTH "${packages}")
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
      string(JSON package GET "${packages}" ${index})
      string(JSON name GET "${package}" name)
      string(REGEX REPLACE "_Attribution_.*$" "" owner "${name}")
      if(NOT owner IN_LIST _PJ_QT_NOTICE_PACKAGES)
        continue()
      endif()
      string(JSON license GET "${package}" licenseConcluded)
      string(JSON copyright GET "${package}" copyrightText)
      string(JSON version ERROR_VARIABLE no_version GET "${package}" versionInfo)
      if(no_version)
        set(version "")
      endif()
      file(APPEND "${output}" "\n--- ${name} ${version}\nLicense: ${license}\n${copyright}\n")
      string(REGEX MATCHALL "LicenseRef-[A-Za-z0-9.-]+" refs "${license}")
      list(APPEND license_refs ${refs})
      string(REGEX MATCHALL "[A-Za-z0-9.+-]+" tokens "${license}")
      foreach(token IN LISTS tokens)
        if(NOT token MATCHES "^(OR|AND|WITH)$" AND NOT token MATCHES "^LicenseRef-")
          list(APPEND license_ids "${token}")
        endif()
      endforeach()
    endforeach()
    string(JSON extracted ERROR_VARIABLE no_extracted GET "${json}" hasExtractedLicensingInfos)
    if(NOT no_extracted)
      string(JSON extracted_count LENGTH "${extracted}")
      math(EXPR extracted_last "${extracted_count} - 1")
      foreach(index RANGE ${extracted_last})
        string(JSON id GET "${extracted}" ${index} licenseId)
        if(id IN_LIST license_refs AND NOT id STREQUAL "LicenseRef-Qt-Commercial")
          string(JSON text GET "${extracted}" ${index} extractedText)
          file(APPEND "${output}" "\n--- ${id}\n${text}\n")
          list(REMOVE_ITEM license_refs "${id}")
        endif()
      endforeach()
    endif()
  endforeach()
  list(REMOVE_DUPLICATES license_ids)
  list(SORT license_ids)
  foreach(id IN LISTS license_ids)
    set(text "${CMAKE_SOURCE_DIR}/cmake/wasm_notices/${id}.txt")
    if(NOT EXISTS "${text}")
      message(FATAL_ERROR "Third-party notices: Qt's SBOM names the licence ${id}; add its text as ${text}")
    endif()
    _pj_notice_text("${text}" body)
    file(APPEND "${output}" "\n--- ${id}\n${body}\n")
  endforeach()
endfunction()

# Writes `output` for this configuration; `python_notices` is CPython's (PjWasmPython.cmake), empty without Python.
function(pj_wasm_write_notices output python_notices)
  get_filename_component(emscripten "${CMAKE_C_COMPILER}" DIRECTORY)
  file(WRITE "${output}"
      "Third-party software in PlotJuggler 4's browser build\n\n"
      "plotjuggler4.wasm, plotjuggler4.js and qtloader.js contain the libraries below, linked statically. PlotJuggler\n"
      "itself is MPL-2.0 (https://github.com/PlotJuggler/PJ4). Each section names a library, its licence as an SPDX\n"
      "expression, and its copyright and licence text as its source distributes them.\n\n"
      "Licence of this bundle. The browser build (plotjuggler4.wasm, plotjuggler4.js, qtloader.js) is a combined work\n"
      "conveyed under the GNU General Public License version 3, because it statically links Qt's WebAssembly platform\n"
      "plugin, which Qt provides only under the GPL version 3 or a commercial licence. The GPL version 3 text is the\n"
      "GPL-3.0-only entry of the Qt section below. PlotJuggler 4's own source is MPL-2.0, which permits this\n"
      "combination; the desktop application remains MPL-2.0. sources/SOURCE-OFFER.txt, next to this deployment,\n"
      "names the complete corresponding source, PlotJuggler 4's included, and where each part is; it is also\n"
      "available on written request via https://github.com/PlotJuggler/PJ4/issues.\n\n"
      "The libraries below keep their own licences. Qt is used under the GNU LGPL version 3, except its WebAssembly\n"
      "platform plugin, as above. FFmpeg (libavcodec, libavutil, libswscale), built LGPL-only, and the Qt Advanced\n"
      "Docking System, PlotJuggler's fork, are used under the LGPL version 2.1 or later. PlotJuggler 4's source pins\n"
      "the toolchain and the commands that build this module (versions.env, .github/workflows/wasm-ci.yml), so it can\n"
      "be rebuilt against a modified copy of any of them.\n")
  _pj_qt_notices("${output}")

  # The Emscripten runtime: its own code, musl and LLVM's C++ runtime, and the sysroot zlib.
  _pj_notice_section("${output}" "Emscripten ${EMSCRIPTEN_VERSION} runtime" "MIT OR NCSA" "${emscripten}/LICENSE")
  _pj_notice_section("${output}" "musl libc (Emscripten's libc)" "MIT" "${emscripten}/system/lib/libc/musl/COPYRIGHT")
  _pj_notice_section("${output}" "LLVM libc++, libc++abi, libunwind and compiler-rt (Emscripten's)"
      "Apache-2.0 WITH LLVM-exception" "${emscripten}/system/lib/libcxx/LICENSE.TXT")
  if(DEFINED ZLIB_INCLUDE_DIR OR PJ_WASM_WITH_PYTHON)
    find_file(_pj_notice_zlib_h zlib.h PATHS "${emscripten}/cache/sysroot/include" NO_DEFAULT_PATH NO_CACHE REQUIRED)
    _pj_notice_section("${output}" "zlib (Emscripten's sysroot port)" "Zlib" "HEADER|${_pj_notice_zlib_h}")
  endif()

  # Vendored under 3rdparty/ and resources/.
  _pj_notice_section("${output}" "Qt Advanced Docking System" "LGPL-2.1-or-later"
      "${CMAKE_SOURCE_DIR}/3rdparty/Qt-Advanced-Docking/LICENSE")
  _pj_notice_section("${output}" "nanocdr" "Apache-2.0" "HEADER|${CMAKE_SOURCE_DIR}/3rdparty/nanocdr/include/nanocdr/nanocdr.hpp")
  _pj_notice_section("${output}" "Noto Sans (font)" "OFL-1.1" "${CMAKE_SOURCE_DIR}/resources/fonts/NotoSans/OFL.txt")

  # Fetched: the name is the FetchContent name, the files are in its source tree.
  pj_wasm_notice("${output}" plotjuggler_sdk "PlotJuggler SDK (pj_base, pj_plugins)" "Apache-2.0" LICENSE LICENSE-APACHE)
  pj_wasm_notice("${output}" pj_official_plugins "PlotJuggler official plugins" "MIT" LICENSE)
  pj_wasm_notice("${output}" pj_official_plugins "liblzf decompressor (in the PCD/PLY loader)" "BSD-2-Clause"
      data_load_3d/contrib/liblzf/LICENSE)
  pj_wasm_notice("${output}" pj_official_plugins "mcap (in the MCAP loader)" "MIT" data_load_mcap/contrib/mcap/LICENSE)
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
      "HEADER|include/rosx_introspection/contrib/peglib.h" "${CMAKE_SOURCE_DIR}/cmake/wasm_notices/MIT.txt")
  pj_wasm_notice("${output}" rosx_introspection "LLVM SmallVector (in rosx_introspection)" "NCSA"
      "HEADER|include/rosx_introspection/contrib/SmallVector.h" "${CMAKE_SOURCE_DIR}/cmake/wasm_notices/NCSA.txt")
  pj_wasm_notice("${output}" rosx_introspection "nanocdr (in rosx_introspection)" "Apache-2.0"
      "HEADER|include/rosx_introspection/contrib/nanocdr.hpp")
  pj_wasm_notice("${output}" data_tamer "Data Tamer" "MIT" LICENSE)
  pj_wasm_notice("${output}" lz4 "LZ4 library" "BSD-2-Clause" lib/LICENSE)
  pj_wasm_notice("${output}" zstd "Zstandard" "BSD-3-Clause" LICENSE)
  pj_wasm_notice("${output}" Luau "Luau" "MIT" LICENSE.txt)
  pj_wasm_notice("${output}" kissfft "KISS FFT" "BSD-3-Clause" COPYING LICENSES/BSD-3-Clause)
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
  pj_wasm_notice("${output}" pj_wasm_pybind11 "pybind11" "BSD-3-Clause" LICENSE)
  if(python_notices)
    _pj_notice_section("${output}" "CPython and the software it incorporates" "PSF-2.0" "${python_notices}")
  endif()
endfunction()
