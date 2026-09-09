# ---------------------------------------------------------------------------
# pj_scripting — Luau scripting engine for PJ4.
#
# Dual-mode:
#  - IN-TREE (PJ4 app): builds pj_scripting_core (the Luau engine + whole-series
#    marker analysis) AND pj_scripting (the SISO DataProcessor/DerivedEngine
#    adapter) AND the tests.
#  - STANDALONE (Conan, `conan create`): builds ONLY pj_scripting_core against the
#    Conan plotjuggler_sdk::base + Luau + kissfft, and installs it. The SDK-only
#    headless runner and the GUI plugin link this same engine, so a rule runs
#    identically GUI <-> headless.
#
# Qt-free. core: pj_base + Luau (+ kissfft) — NOT pj_datastore. MPL-2.0.
# ---------------------------------------------------------------------------

if(NOT TARGET plotjuggler_sdk::base)
  # Standalone Conan build of pj_scripting_core only.
  cmake_minimum_required(VERSION 3.20)
  project(pj_scripting_core CXX)
  find_package(plotjuggler_sdk CONFIG REQUIRED)
  set(_PJS_STANDALONE TRUE)
else()
  set(_PJS_STANDALONE FALSE)
endif()
set(_PJS_BASE plotjuggler_sdk::base)

find_package(Luau REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(kissfft REQUIRED)

# Conan's generated kissfft config defines kissfft::kissfft directly; upstream's
# installed config (conda/pixi, recipes/kissfft) only defines the datatype-suffixed
# target unless a datatype COMPONENT is requested — and requesting one would break
# the Conan side, which validates components. Bridge the float variant instead
# (same pattern as the cpython::embed bridge below).
if(NOT TARGET kissfft::kissfft)
  add_library(kissfft::kissfft INTERFACE IMPORTED)
  target_link_libraries(kissfft::kissfft INTERFACE kissfft::kissfft-float)
endif()
if(NOT EMSCRIPTEN)
  # Conan's cpython package ships a build module that runs its own bin/python3.12 to
  # cross-check the version. That interpreter carries no RUNPATH, so with a shared
  # libpython the loader hands it the SYSTEM libpython3.12.so.1.0 (a different patch
  # level) and the check aborts the configure. Put the package's own lib dir on the
  # search path first — only that one, so no other Conan library becomes visible to
  # the tools CMake spawns.
  if(UNIX AND DEFINED CONAN_RUNTIME_LIB_DIRS)
    foreach(_dir IN LISTS CONAN_RUNTIME_LIB_DIRS)
      file(GLOB _pj_libpython "${_dir}/libpython3*.so*")
      if(_pj_libpython)
        set(ENV{LD_LIBRARY_PATH} "${_dir}:$ENV{LD_LIBRARY_PATH}")
        break()
      endif()
    endforeach()
    unset(_pj_libpython)
  endif()
  find_package(Python REQUIRED)    # conan-provided embedded CPython (cpython::embed)
  find_package(pybind11 REQUIRED)  # embedded CPython backend (python_engine.cpp)
endif()

# Outside Conan (e.g. a conda/pixi environment) there is no cpython::embed —
# resolve the embed library through CMake's stock FindPython and bridge it to
# the same target name.
if(NOT EMSCRIPTEN AND NOT TARGET cpython::embed)
  find_package(Python REQUIRED COMPONENTS Interpreter Development.Embed)
  add_library(cpython::embed INTERFACE IMPORTED)
  target_link_libraries(cpython::embed INTERFACE Python::Python)
endif()

# Bake the CPython prefix so the embedded interpreter finds its stdlib at runtime
# (PYTHONHOME = <prefix>, with the stdlib at <prefix>/lib/pythonX.Y). Prefer Conan's
# cpython_PACKAGE_FOLDER_<CONFIG> (always emitted); fall back to deriving it from the
# Python include dir or executable if needed.
set(_pj_python_home "")
set(_pj_python_home_is_conan FALSE)
foreach(_cfg RELWITHDEBINFO RELEASE DEBUG MINSIZEREL NONE)
  if(DEFINED cpython_PACKAGE_FOLDER_${_cfg})
    set(_pj_python_home "${cpython_PACKAGE_FOLDER_${_cfg}}")
    set(_pj_python_home_is_conan TRUE)
    break()
  endif()
endforeach()
if(NOT _pj_python_home AND Python_INCLUDE_DIRS)
  list(GET Python_INCLUDE_DIRS 0 _pj_py_inc)
  if(WIN32)
    # Non-Conan Windows Python packages (e.g. conda-forge) install headers
    # directly at <prefix>/include, with no version-suffixed subdir — one hop
    # already reaches <prefix>. The Unix two-hop rule below would overshoot.
    get_filename_component(_pj_python_home "${_pj_py_inc}" DIRECTORY)
  else()
    # <prefix>/include/pythonX.Y -> <prefix> (two hops; the stdlib lives at
    # <prefix>/lib/pythonX.Y).
    get_filename_component(_pj_py_inc_parent "${_pj_py_inc}" DIRECTORY)
    get_filename_component(_pj_python_home "${_pj_py_inc_parent}" DIRECTORY)
  endif()
endif()
if(NOT _pj_python_home AND DEFINED _Python_EXECUTABLE)
  get_filename_component(_pj_python_home "${_Python_EXECUTABLE}" DIRECTORY)
  get_filename_component(_pj_python_home "${_pj_python_home}" DIRECTORY)
endif()
if(NOT EMSCRIPTEN AND NOT _pj_python_home)
  message(WARNING "pj_scripting: could not determine the embedded Python home; the Python backend may fail to init at runtime")
endif()
if(NOT EMSCRIPTEN)
  message(STATUS "pj_scripting: embedded Python home = ${_pj_python_home}")
endif()

# PYTHONHOME the embedded interpreter is pointed at. On Windows the Conan CPython
# package nests its runtime — python3XX.dll, Lib/ (stdlib), DLLs/ — under
# <prefix>/bin, so PYTHONHOME must point there; on Linux (and non-Conan Windows
# toolchains, e.g. conda-forge) the stdlib lives directly under the package root.
set(_pj_python_home_define "${_pj_python_home}")
if(WIN32 AND _pj_python_home_is_conan)
  set(_pj_python_home_define "${_pj_python_home}/bin")
endif()

# CPython is linked shared on every platform (conanfile.txt `cpython/*:shared=True`;
# on Windows the recipe forces it anyway, having disabled the static MSVC build for
# >=3.10). Windows has no RPATH, so python_engine_test needs pythonXY.dll resolvable
# via PATH at launch, else STATUS_DLL_NOT_FOUND (0xc0000135). Export the CPython
# runtime dir(s) so the CI Test step can prepend them — mirrors pj_scene2D/core's
# FFmpeg bin-dir export. Linux needs none of this: Conan bakes a DT_RUNPATH into the
# binaries, and the AppImage resolves via AppRun.sh's LD_LIBRARY_PATH.
if(WIN32 AND _pj_python_home)
  file(WRITE "${CMAKE_BINARY_DIR}/pj_python_bin_dirs.txt" "${_pj_python_home}/bin\n${_pj_python_home}\n")
endif()

# Record the CPython prefix so the AppImage packager can bundle the stdlib
# (<prefix>/lib/pythonX.Y) into the AppDir and point PYTHONHOME at it, instead of
# relying on PJ_PYTHON_HOME — the build-machine Conan path baked into the binary,
# which does not exist on any other machine.
if(NOT WIN32 AND _pj_python_home)
  file(WRITE "${CMAKE_BINARY_DIR}/pj_python_home.txt" "${_pj_python_home}\n")
endif()

# --- pj_scripting_core: the Luau engine + whole-series marker analysis ---------
# Depends ONLY on pj_base + Luau (+ kissfft for bandPower) — NOT pj_datastore. This
# is the shareable unit.
add_library(pj_scripting_core STATIC
  src/luau_engine.cpp
  src/marker_engine.cpp
)
target_include_directories(pj_scripting_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(pj_scripting_core PUBLIC cxx_std_20)
set_target_properties(pj_scripting_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(PJ_ASSERT_THROWS)
  target_compile_definitions(pj_scripting_core PUBLIC PJ_ASSERT_THROWS)
endif()
# NOTE: PJ_WARNING_FLAGS (-Werror) is intentionally NOT applied here — the Luau
# 3rd-party headers trip -Wextra at instantiation points.
target_link_libraries(pj_scripting_core
  PUBLIC
    ${_PJS_BASE}
    nlohmann_json::nlohmann_json  # filter_class.h exposes nlohmann::json in its public API
  PRIVATE
    Luau::VM                      # Luau backend (hand-bound) — luau_engine.cpp / marker_engine.cpp
    Luau::Compiler
    kissfft::kissfft              # FFT for marker_engine's bandPower primitive
)

if(_PJS_STANDALONE)
  # Conan package: install the lib + the core public headers (the marker-analysis
  # surface and the engine seam; the SISO/datastore headers are not shipped).
  install(TARGETS pj_scripting_core ARCHIVE DESTINATION lib)
  install(FILES
    include/pj_scripting/marker_engine.h
    include/pj_scripting/script_engine.h
    include/pj_scripting/filter_class.h
    include/pj_scripting/sandbox.h
    DESTINATION include/pj_scripting)
  return()
endif()

# --- pj_scripting: the DataProcessor/DerivedEngine adapter (in-tree only) ------
# The SISO streaming path that couples the engine to pj_datastore's DerivedEngine.
# Marker analysis does NOT use this; it stays out of the shareable core.
# Luau lives in pj_scripting_core (linked PUBLIC below); python is desktop-only.
set(PJ_SCRIPTING_SOURCES
  src/lua_siso_transform.cpp
  src/lua_mimo_transform.cpp
  src/filter_catalogue.cpp
)
if(EMSCRIPTEN)
  list(APPEND PJ_SCRIPTING_SOURCES src/python_engine_unavailable.cpp)
else()
  list(APPEND PJ_SCRIPTING_SOURCES src/python_engine.cpp)
endif()

add_library(pj_scripting STATIC ${PJ_SCRIPTING_SOURCES})
target_compile_features(pj_scripting PUBLIC cxx_std_20)
set_target_properties(pj_scripting PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(PJ_ASSERT_THROWS)
  target_compile_definitions(pj_scripting PUBLIC PJ_ASSERT_THROWS)
endif()
target_link_libraries(pj_scripting
  PUBLIC
    pj_scripting_core             # Luau engine + marker analysis (brings Luau::VM/Compiler transitively)
    pj_datastore  # DataProcessor base + ProcessorSisoAdapter + ISISOTransform (formerly pj_proc)
)

if(NOT EMSCRIPTEN)
  target_link_libraries(pj_scripting PRIVATE
    pybind11::embed               # Python backend (embedded CPython) — python_engine.cpp only
    cpython::embed                # libpython for the embedded interpreter
  )
endif()

if(NOT EMSCRIPTEN)
  target_compile_definitions(pj_scripting PRIVATE PJ_PYTHON_HOME="${_pj_python_home_define}")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
  # --- Luau filter engine (Approach A) ---
  # no WARNINGS: 6 of 6 merged tests never compiled with -Werror
  pj_add_test_runner(pj_scripting_tests
    MAIN core
    SOURCES
      tests/luau_engine_test.cpp
      tests/python_engine_test.cpp
      tests/lua_siso_transform_test.cpp
      tests/lua_time_precision_test.cpp
      # --- M2: sandbox + watchdog ---
      tests/luau_sandbox_test.cpp
      tests/luau_watchdog_test.cpp
    LIBS pj_scripting
    PREFIX "pj_scripting.")

  # --- Phase 1: whole-series marker engine (parity spike) ---
  # Links ONLY pj_scripting_core (no pj_datastore) — proves the marker engine is
  # self-contained and shareable with the SDK-only headless runner.
  add_executable(marker_engine_test tests/marker_engine_test.cpp)
  target_link_libraries(marker_engine_test PRIVATE pj_scripting_core GTest::gtest_main)
  add_test(NAME marker_engine_test COMMAND marker_engine_test)

  # --- M9: golden behavioral coverage relocated from the retired C++ builtin specs ---
  add_executable(builtin_golden_test tests/builtin_golden_test.cpp)
  target_link_libraries(builtin_golden_test PRIVATE pj_scripting GTest::gtest_main)
  target_compile_definitions(builtin_golden_test PRIVATE
    BUILTIN_FILTERS_PATH="${CMAKE_SOURCE_DIR}/resources/filters/builtin_filters.luau")
  add_test(NAME BuiltinGoldenTest COMMAND builtin_golden_test)

  # --- M5: filter catalogue ---
  add_executable(filter_catalogue_test tests/filter_catalogue_test.cpp)
  target_link_libraries(filter_catalogue_test PRIVATE pj_scripting GTest::gtest_main)
  target_compile_definitions(filter_catalogue_test PRIVATE
    BUILTIN_FILTERS_PATH="${CMAKE_SOURCE_DIR}/resources/filters/builtin_filters.luau")
  add_test(NAME filter_catalogue_test COMMAND filter_catalogue_test)
endif()
