# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0
#
# Guards the --exclude-libs,ALL link option in pj_app/CMakeLists.txt, which keeps symbols from static archive
# members (Conan's libpng, zstd, bzip2, CPython, ...) out of pj_app's dynamic symbol table. Without it the loader
# can interpose them ahead of the system copies GTK/glib load (libpng 1.6.43 vs 1.6.58 ABI skew crashed
# QGtk3Interface::fileIcon during QFileDialog).
#
# Run standalone as:
#   cmake -DTARGET=<path/to/plotjuggler4> -DNM=<path/to/nm> -P check_exported_symbols.cmake

# Script mode (-P) starts with no policies set, so IN_LIST below would be read as a
# bare variable name rather than as an operator.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED TARGET)
  message(FATAL_ERROR "check_exported_symbols: -DTARGET=<executable> is required")
endif()
if(NOT EXISTS "${TARGET}")
  message(FATAL_ERROR "check_exported_symbols: no such file: ${TARGET}")
endif()
if(NOT DEFINED NM)
  set(NM "nm")
endif()

# An instrumented build exports parts of the sanitizer runtime interface from the
# executable itself (e.g. __asan_option_detect_stack_use_after_return,
# __ubsan_vptr_type_cache). They are not a static third-party library leaking,
# and no uninstrumented build carries them, so they are only skipped there.
set(_sanitizer_runtime_regex "^__(asan|lsan|ubsan|tsan|msan|sanitizer)_")

execute_process(
  COMMAND "${NM}" -D --defined-only "${TARGET}"
  OUTPUT_VARIABLE _nm_output
  ERROR_VARIABLE _nm_error
  RESULT_VARIABLE _nm_result
)
if(NOT _nm_result EQUAL 0)
  message(FATAL_ERROR "check_exported_symbols: nm failed on ${TARGET}: ${_nm_error}")
endif()

# `nm -D --defined-only` prints "<addr> <type> <name>"; the address column is blank
# for some weak/unique symbols, so match on the type letter rather than on columns.
set(_exported "")
string(REPLACE "\n" ";" _nm_lines "${_nm_output}")
foreach(_line IN LISTS _nm_lines)
  if(_line MATCHES "^[0-9a-fA-F]* +[A-Za-z] +([^ ]+)$")
    set(_name "${CMAKE_MATCH_1}")
    if(SANITIZERS_ACTIVE AND _name MATCHES "${_sanitizer_runtime_regex}")
      continue()
    endif()
    list(APPEND _exported "${_name}")
  endif()
endforeach()
list(LENGTH _exported _exported_count)

# The rule is STRUCTURAL, not a list of banned libraries.
#
# A correctly-linked pj_app exports only compiler/ABI artifacts: Itanium-mangled C++ names (_Z...) plus a handful
# of libc data objects that copy-relocate into the executable. Third-party C libraries (libpng, zstd, bzip2, lzma,
# lz4, CPython, ...) export PLAIN C identifiers. A blocklist would miss any library nobody enumerated; pinning
# the symbol set would fail on every refactor, since C++ artifacts come and go.
set(_libc_data_allowlist
    stdin stdout stderr environ __environ __libc_single_threaded)

set(_leaked "")
foreach(_sym IN LISTS _exported)
  if(_sym MATCHES "^_Z")
    continue()  # Itanium C++ ABI artifact
  endif()
  # Symbols may carry a version suffix, e.g. "stderr@GLIBC_2.2.5".
  string(REGEX REPLACE "@.*$" "" _bare "${_sym}")
  if(_bare IN_LIST _libc_data_allowlist)
    continue()
  endif()
  list(APPEND _leaked "${_sym}")
endforeach()

# Backstop for a mass leak of C++ symbols (Luau, assimp and friends are C++ too), which the structural rule
# waves through. Generous on purpose: it must fire only when --exclude-libs has stopped working (baseline 40).
set(_ceiling 200)

if(_leaked)
  string(REPLACE ";" "\n  " _leaked_text "${_leaked}")
  message(FATAL_ERROR
      "check_exported_symbols: ${TARGET} exports non-C++ symbol(s) — a third-party static\n"
      "library is leaking into the dynamic symbol table and can interpose the system copy\n"
      "GTK/glib loads (see the --exclude-libs,ALL comment in pj_app/CMakeLists.txt):\n"
      "  ${_leaked_text}\n"
      "If one of these is a legitimate libc data object, add it to _libc_data_allowlist "
      "in this file with a note on why.")
endif()

if(_exported_count GREATER _ceiling)
  message(FATAL_ERROR
      "check_exported_symbols: ${TARGET} exports ${_exported_count} dynamic symbols, over the "
      "${_ceiling} ceiling (baseline is 40). Every name is C++-mangled, so this is most likely a "
      "whole third-party C++ archive leaking rather than one stray symbol — check that "
      "--exclude-libs,ALL still reaches this target.")
endif()

message(STATUS "check_exported_symbols: OK — ${_exported_count} exported symbol(s), all C++ ABI artifacts or allowed libc data")
