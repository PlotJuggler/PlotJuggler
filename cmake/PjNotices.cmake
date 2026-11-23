# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# THIRD_PARTY_NOTICES.txt: the third-party software a PlotJuggler build contains, each with its SPDX licence expression
# and its licence text. The browser build's is written by cmake/PjWasmNotices.cmake on top of these helpers; the
# desktop build's by pj_desktop_write_notices() below. cmake/notices/<SPDX id>.txt holds the standard licence texts;
# cmake/notices/conan/<package>.txt the texts a Conan package does not carry itself.
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

# Appends one section: a title, its SPDX expression (none when empty) and the texts of `ARGN` (absolute specs).
function(_pj_notice_section output title spdx)
  if(spdx)
    set(title "${title}\nSPDX-License-Identifier: ${spdx}")
  endif()
  file(APPEND "${output}" "\n${_PJ_NOTICE_RULE}\n${title}\n${_PJ_NOTICE_RULE}\n")
  foreach(spec IN LISTS ARGN)
    _pj_notice_text("${spec}" text)
    file(APPEND "${output}" "\n${text}\n")
  endforeach()
endfunction()

# One FetchContent dependency, its licence files relative to its source tree (an absolute path, such as a text of
# cmake/notices/ for a header that only names its licence, is used as is). Nothing when this configuration does not
# populate it (its option is off) and a licence file is relative to its tree. A dependency declared only for the
# source offer (never populated) names absolute texts and is guarded by its caller instead.
function(pj_wasm_notice output name title spdx)
  FetchContent_GetProperties(${name} POPULATED populated SOURCE_DIR source)
  set(specs "")
  foreach(spec IN LISTS ARGN)
    string(REGEX MATCH "[^|]+$" file "${spec}")
    if(NOT IS_ABSOLUTE "${file}")
      if(NOT populated)
        return()
      endif()
      string(REGEX REPLACE "([^|]+)$" "${source}/\\1" spec "${spec}")
    endif()
    list(APPEND specs "${spec}")
  endforeach()
  _pj_notice_section("${output}" "${title}" "${spdx}" ${specs})
endfunction()

# Qt: the SBOM packages named in `packages` (the modules and plugins a build ships, and the third-party code the SBOM
# attributes to them) of the kit's SBOMs `ARGN` (qtbase, qtsvg...), each with its licence and copyright. A LicenseRef's
# text comes from the SBOM; the text of every standard SPDX id (an exception after WITH included) any of them names is
# cmake/notices/<id>.txt, so a new id fails the configure until its text is added.
function(_pj_qt_notices output spdx packages)
  get_filename_component(sbom_dir "${Qt6_DIR}/../../../sbom" ABSOLUTE)
  file(APPEND "${output}" "\n${_PJ_NOTICE_RULE}\nQt ${Qt6_VERSION}\n"
      "SPDX-License-Identifier: ${spdx}\n"
      "${_PJ_NOTICE_RULE}\n\n"
      "Copyright (C) The Qt Company Ltd. and other contributors. Modules and the third-party code they contain, from\n"
      "the Qt kit's SBOM (sbom/<module>-${Qt6_VERSION}.spdx.json); the text of each standard licence they name\n"
      "follows the list, once.\n")
  set(license_refs "")
  set(license_ids "")
  foreach(module IN LISTS ARGN)
    set(sbom "${sbom_dir}/${module}-${Qt6_VERSION}.spdx.json")
    if(NOT EXISTS "${sbom}")
      message(FATAL_ERROR "Third-party notices: the Qt kit has no SBOM ${sbom}")
    endif()
    file(READ "${sbom}" json)
    string(JSON sbom_packages GET "${json}" packages)
    string(JSON count LENGTH "${sbom_packages}")
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
      string(JSON package GET "${sbom_packages}" ${index})
      string(JSON name GET "${package}" name)
      string(REGEX REPLACE "_Attribution_.*$" "" owner "${name}")
      if(NOT owner IN_LIST packages)
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
    set(text "${CMAKE_SOURCE_DIR}/cmake/notices/${id}.txt")
    if(NOT EXISTS "${text}")
      message(FATAL_ERROR "Third-party notices: Qt's SBOM names the licence ${id}; add its text as ${text}")
    endif()
    _pj_notice_text("${text}" body)
    file(APPEND "${output}" "\n--- ${id}\n${body}\n")
  endforeach()
endfunction()

# The sections both builds carry: vendored code under 3rdparty/ and resources/.
function(_pj_vendored_notices output)
  _pj_notice_section("${output}" "Qt Advanced Docking System (PlotJuggler's fork)" "LGPL-2.1-or-later"
      "${CMAKE_SOURCE_DIR}/3rdparty/Qt-Advanced-Docking/LICENSE")
  _pj_notice_section("${output}" "nanocdr" "Apache-2.0" "HEADER|${CMAKE_SOURCE_DIR}/3rdparty/nanocdr/include/nanocdr/nanocdr.hpp")
  _pj_notice_section("${output}" "PlotJuggler Sans (font, derived from Noto Sans)" "OFL-1.1" "${CMAKE_SOURCE_DIR}/resources/fonts/PlotJugglerSans/OFL.txt")
  _pj_notice_section("${output}" "Material Symbols (Google; most icons in resources/svg)" "Apache-2.0"
      "${CMAKE_SOURCE_DIR}/cmake/notices/Apache-2.0.txt")
endfunction()

# Desktop: the Qt SBOM packages of the modules the app links and the platform, image, icon and TLS plugins the desktop
# packages deploy.
set(_PJ_QT_DESKTOP_NOTICE_PACKAGES
    Core Concurrent DBus Gui Network OpenGL OpenGLWidgets PrintSupport Widgets Xml XcbQpaPrivate WaylandClient
    BundledFreetype BundledHarfbuzz BundledLibjpeg BundledLibjpeg12bits BundledLibjpeg16bits BundledLibpng
    BundledPcre2 BundledZLIB
    QXcbIntegrationPlugin QWaylandIntegrationPlugin QWindowsIntegrationPlugin QCocoaIntegrationPlugin
    QGifPlugin QICOPlugin QJpegPlugin QTlsBackendCertOnlyPlugin QTlsBackendOpenSSLPlugin QSchannelBackendPlugin
    QSecureTransportBackendPlugin
    Svg SvgWidgets QSvgPlugin QSvgIconPlugin
    UiTools)

# Conan packages CMakeDeps writes no file for, so their licences are committed in cmake/notices/conan/<name>.txt:
# compiled into a shared Conan library (CPython's extension modules, FFmpeg) or into a static one through headers
# only (assimp).
set(_PJ_CONAN_HIDDEN_NOTICES dav1d expat libffi mpdecimal openssl rapidjson sqlite3 utfcpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND _PJ_CONAN_HIDDEN_NOTICES libdrm libpciaccess libxcrypt util-linux-libuuid)
endif()
# In conan.lock but not shipped: test frameworks, kernel headers, the system libva the recipe only wraps.
set(_PJ_CONAN_UNSHIPPED benchmark gtest linux-headers-generic vaapi)

# The Conan packages of this configuration: one section each, with the licence files the package carries
# (<package>/licenses/) and cmake/notices/conan/<name>.txt when it exists. On Linux, where the graph holds every
# package of conan.lock, a package with neither fails the configure until its text is added.
function(_pj_conan_notices output)
  get_filename_component(generators "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
  file(GLOB data_files "${generators}/*-data.cmake")
  set(names "")
  foreach(data IN LISTS data_files)
    file(STRINGS "${data}" lines REGEX "_PACKAGE_FOLDER_[A-Z]+ \"")
    foreach(line IN LISTS lines)
      if(line MATCHES "set\\(([A-Za-z0-9_.+-]+)_PACKAGE_FOLDER_[A-Z]+ \"([^\"]+)\"\\)")
        set(folder_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        list(APPEND names "${CMAKE_MATCH_1}")
      endif()
    endforeach()
  endforeach()
  list(APPEND names ${_PJ_CONAN_HIDDEN_NOTICES})
  list(REMOVE_DUPLICATES names)
  list(REMOVE_ITEM names ${_PJ_CONAN_UNSHIPPED})
  list(SORT names)

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(READ "${CMAKE_SOURCE_DIR}/conan.lock" lock)
    string(JSON requires GET "${lock}" requires)
    string(JSON count LENGTH "${requires}")
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
      string(JSON ref GET "${requires}" ${index})
      string(REGEX REPLACE "/.*$" "" name "${ref}")
      if(NOT name IN_LIST names AND NOT name IN_LIST _PJ_CONAN_UNSHIPPED)
        message(FATAL_ERROR "Third-party notices: conan.lock's ${ref} has no CMakeDeps file; add its licence as "
                            "cmake/notices/conan/${name}.txt and its name to _PJ_CONAN_HIDDEN_NOTICES in ${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
      endif()
    endforeach()
  endif()

  foreach(name IN LISTS names)
    set(specs "")
    if(DEFINED folder_${name})
      file(GLOB_RECURSE carried LIST_DIRECTORIES false "${folder_${name}}/licenses/*")
      list(SORT carried)
      list(APPEND specs ${carried})
    endif()
    set(committed "${CMAKE_SOURCE_DIR}/cmake/notices/conan/${name}.txt")
    if(EXISTS "${committed}")
      list(APPEND specs "${committed}")
    endif()
    if(NOT specs)
      message(FATAL_ERROR "Third-party notices: the Conan package ${name} carries no licence; add it as ${committed}")
    endif()
    _pj_notice_section("${output}" "${name}" "" ${specs})
  endforeach()
endfunction()

# Writes `output`, the desktop build's notices, embedded in the executable (the About box shows it) and shipped beside
# it by every package (packaging/).
function(pj_desktop_write_notices output)
  file(WRITE "${output}"
      "Third-party software in PlotJuggler 4\n\n"
      "PlotJuggler 4 is MPL-2.0 (https://github.com/PlotJuggler/PlotJuggler). This program contains or ships with the\n"
      "software below. Each section names it, its licence, and its copyright and licence text as its source\n"
      "distributes them.\n\n"
      "LGPL components. Qt and FFmpeg (libavcodec, libavformat, libavutil, libswscale, built LGPL-only) are shared\n"
      "libraries beside the executable: replace them with compatible builds of your own to run PlotJuggler with a\n"
      "modified version. The Qt Advanced Docking System (PlotJuggler's fork,\n"
      "https://github.com/PlotJuggler/Qt-Advanced-Docking-System, at the commit the 3rdparty/Qt-Advanced-Docking\n"
      "submodule pins), Qwt and, on Linux, elfutils' libdw and libxcrypt are linked statically: PlotJuggler's\n"
      "complete source at the release tag, with the pinned dependencies (conan.lock, versions.env) and the build\n"
      "commands (build.sh, docs/BUILDING.md), lets you rebuild it against a modified copy of any of them.\n\n"
      "Source code. The source of every component under the GNU LGPL or GPL, at the exact version shipped, is\n"
      "available from the locations its section and PlotJuggler's repository name, and on written request via\n"
      "https://github.com/PlotJuggler/PlotJuggler/issues, for three years after the release, at no more than the\n"
      "cost of physically performing the distribution. On Linux the request also covers the distribution libraries\n"
      "packaged with the program (share/doc/<package>/copyright).\n")
  if(EXISTS "${Qt6_DIR}/../../../sbom/qtbase-${Qt6_VERSION}.spdx.json")
    _pj_qt_notices("${output}" "LGPL-3.0-only (also offered under GPL-2.0-only, GPL-3.0-only or commercially)"
        "${_PJ_QT_DESKTOP_NOTICE_PACKAGES}" qtbase qtsvg qttools)
  else()
    # A Qt from a system or conda package (PJ_SYSTEM_* builds) carries no SBOM; that distribution ships its notices.
    message(STATUS "Third-party notices: this Qt kit has no SBOM; the Qt section names the licence only")
    _pj_notice_section("${output}" "Qt ${Qt6_VERSION}" "LGPL-3.0-only"
        "${CMAKE_SOURCE_DIR}/cmake/notices/LGPL-3.0-only.txt" "${CMAKE_SOURCE_DIR}/cmake/notices/GPL-3.0-only.txt")
  endif()
  # Linux kits ship Qt's own ICU build, which the SBOM lists without a licence.
  file(GLOB icu "${Qt6_DIR}/../../libicuuc.so.*")
  if(icu)
    _pj_notice_section("${output}" "ICU 73 (shipped with Qt)" "Unicode-DFS-2016 AND ICU"
        "${CMAKE_SOURCE_DIR}/cmake/notices/icu4c-73.txt")
  endif()
  _pj_vendored_notices("${output}")
  pj_wasm_notice("${output}" qwt_upstream "Qwt 6.3.0" "LGPL-2.1-only WITH Qwt-exception-1.0" COPYING)
  if(CMAKE_TOOLCHAIN_FILE MATCHES "conan_toolchain\\.cmake$")
    _pj_conan_notices("${output}")
  endif()
endfunction()
