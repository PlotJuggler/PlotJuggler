# SPDX-License-Identifier: MPL-2.0
# Debug-information policy for RelWithDebInfo trees. Sizes measured 2026-09-08
# (workspace findings): with plain -g every executable is ~94 % DWARF and the
# same module DWARF is copied into each of 300+ test binaries. Split DWARF
# stores it once per object (.dwo); type units dedupe Qt template types within
# a link; zlib halves what remains. MSVC keeps /Z7 (PDB duplication is a
# separate problem this file does not address).
set(PJ_DEBUG_INFO "full" CACHE STRING "Debug info level: none|lines|full|split")
set_property(CACHE PJ_DEBUG_INFO PROPERTY STRINGS none lines full split)
option(PJ_COMPRESS_DEBUG "Compress ELF debug sections with zlib (-gz)" OFF)

if(NOT PJ_DEBUG_INFO MATCHES "^(none|lines|full|split)$")
    message(FATAL_ERROR "PJ_DEBUG_INFO must be none, lines, full or split (got '${PJ_DEBUG_INFO}')")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT EMSCRIPTEN)
    # Strip CMake's stock RelWithDebInfo -g so the policy below is the single
    # source of debug flags for that configuration.
    string(REPLACE "-g " "" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} ")
    string(REPLACE "-g " "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ")
    string(STRIP "${CMAKE_C_FLAGS_RELWITHDEBINFO}" CMAKE_C_FLAGS_RELWITHDEBINFO)
    string(STRIP "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}" CMAKE_CXX_FLAGS_RELWITHDEBINFO)

    set(_pj_dbg "$<CONFIG:RelWithDebInfo>")
    if(PJ_DEBUG_INFO STREQUAL "none")
        add_compile_options("$<${_pj_dbg}:-g0>")
    elseif(PJ_DEBUG_INFO STREQUAL "lines")
        add_compile_options("$<${_pj_dbg}:-g1>")
    elseif(PJ_DEBUG_INFO STREQUAL "full")
        add_compile_options(
            "$<${_pj_dbg}:-g>"
            "$<${_pj_dbg}:-gdwarf-5>"
            "$<${_pj_dbg}:-fdebug-types-section>")
    else() # split
        if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
            message(FATAL_ERROR "PJ_DEBUG_INFO=split cannot be combined with LTO: GCC silently drops -gsplit-dwarf under -flto")
        endif()
        # No -fdebug-types-section here: gdb 12 (the builder image's) cannot
        # resolve type-unit signatures that live in .dwo files ("Cannot find
        # signatured DIE") and then fails to set breakpoints at all. The .dwo
        # files already store each object's DWARF once, so type units buy
        # little in split mode.
        add_compile_options(
            "$<${_pj_dbg}:-g>"
            "$<${_pj_dbg}:-gdwarf-5>"
            "$<${_pj_dbg}:-gsplit-dwarf>")
        # No --gdb-index: GNU ld (bfd, the linker in the builder image) rejects it;
        # only gold/lld accept it.
    endif()
    if(PJ_COMPRESS_DEBUG AND NOT PJ_DEBUG_INFO STREQUAL "none")
        add_compile_options("$<${_pj_dbg}:-gz=zlib>")
        add_link_options("$<${_pj_dbg}:-gz=zlib>")
    endif()
    # Objects reference sources as ./<path>, so a ccache hit from another
    # worktree is byte-identical; gdb resolves paths through `directory`/`set
    # substitute-path` or by being run from the source root.
    add_compile_options("-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=.")
    message(STATUS "PJ_DEBUG_INFO=${PJ_DEBUG_INFO} PJ_COMPRESS_DEBUG=${PJ_COMPRESS_DEBUG}")

    # Thin archives reference their objects instead of copying them, avoiding
    # 0.87 GB of duplicate object bytes (including DWARF relocations) in the
    # measured tree. AppImage staging uses binaries directly, but ADS defines
    # archive install/export rules; pj_scripting also installs in standalone mode.
    # Configure with -DPJ_INSTALL_STATIC_LIBS=ON before installing static libraries
    # so their archives remain usable outside the original build tree.
    if(NOT PJ_INSTALL_STATIC_LIBS)
        set(CMAKE_C_ARCHIVE_CREATE   "<CMAKE_AR> qcT <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qcT <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_C_ARCHIVE_APPEND   "<CMAKE_AR> qT  <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_CXX_ARCHIVE_APPEND "<CMAKE_AR> qT  <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_C_ARCHIVE_FINISH   "<CMAKE_RANLIB> <TARGET>")
        set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> <TARGET>")
    endif()
endif()
