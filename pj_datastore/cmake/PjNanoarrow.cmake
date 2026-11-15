# SPDX-License-Identifier: MPL-2.0
#
# nanoarrow::nanoarrow + nanoarrow::nanoarrow_ipc (Apache-2.0, C), 0.9.0.
#  - An installed 0.9.0 wins: conda-forge's libnanoarrow, a distro or Nix package, or
#    Conan once ConanCenter ships 0.9 (conan-center-index#30738 for the version,
#    #31313 for the IPC/flatcc component). Its config demands an exact version match.
#  - Otherwise the pinned Apache source release is built here, statically. Its IPC
#    extension compiles the flatcc runtime vendored in the same tarball: no second
#    download, so -DFETCHCONTENT_SOURCE_DIR_NANOARROW=<unpacked tarball> builds offline.
# Drop the fetch when ConanCenter ships nanoarrow 0.9 with IPC: add it back to
# conanfile.txt (+ conan.lock) and keep only the find_package.
include_guard(GLOBAL)

find_package(nanoarrow 0.9.0 CONFIG QUIET)
if(nanoarrow_FOUND)
  # conda-forge's IPC target links zstd::libzstd and lz4::lz4, but its config looks
  # neither up, and some lz4 packages ship no CMake config: provide whichever is missing.
  find_package(zstd CONFIG QUIET)
  find_package(lz4 CONFIG QUIET)
  foreach(_pj_codec IN ITEMS zstd lz4)
    if(_pj_codec STREQUAL "zstd")
      set(_pj_codec_target zstd::libzstd)
    else()
      set(_pj_codec_target lz4::lz4)
    endif()
    if(NOT TARGET ${_pj_codec_target})
      find_library(PJ_${_pj_codec}_LIBRARY NAMES ${_pj_codec} lib${_pj_codec})
      find_path(PJ_${_pj_codec}_INCLUDE_DIR NAMES ${_pj_codec}.h)
      if(PJ_${_pj_codec}_LIBRARY AND PJ_${_pj_codec}_INCLUDE_DIR)
        add_library(${_pj_codec_target} UNKNOWN IMPORTED)
        set_target_properties(${_pj_codec_target} PROPERTIES
          IMPORTED_LOCATION "${PJ_${_pj_codec}_LIBRARY}"
          INTERFACE_INCLUDE_DIRECTORIES "${PJ_${_pj_codec}_INCLUDE_DIR}")
      endif()
    endif()
  endforeach()
else()
  # The archive timestamps policy; set here too because the browser build includes this
  # file before the root CMakeLists sets it.
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  include(FetchContent)
  set(NANOARROW_IPC ON CACHE BOOL "" FORCE)
  FetchContent_Declare(nanoarrow
    URL https://github.com/apache/arrow-nanoarrow/releases/download/apache-arrow-nanoarrow-0.9.0/apache-arrow-nanoarrow-0.9.0.tar.gz
    URL_HASH SHA256=801200a0e95e869d5c4bdeb5b535dba58551482bb782b7dc8bd599c8b6e8cacf)
  FetchContent_MakeAvailable(nanoarrow)
  # Build only what is linked: upstream also defines shared variants and its tests'
  # targets. Treat its headers as system headers so PJ's warning flags skip them.
  set_property(DIRECTORY "${nanoarrow_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL ON)
  foreach(_pj_target IN ITEMS nanoarrow_static nanoarrow_ipc_static)
    get_target_property(_pj_includes ${_pj_target} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${_pj_target} PROPERTIES
      POSITION_INDEPENDENT_CODE ON
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_pj_includes}")
  endforeach()
endif()
