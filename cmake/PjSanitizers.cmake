# SPDX-License-Identifier: MPL-2.0
option(PJ_ENABLE_SANITIZERS "Enable AddressSanitizer instrumentation (opt-in, any build type)" OFF)
option(PJ_ENABLE_TSAN "Enable ThreadSanitizer instrumentation (opt-in, any build type)" OFF)
option(PJ_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer with AddressSanitizer" ON)
# OFF by default, and that default is measured rather than cautious. The libstdc++
# container annotations are inline in <vector>, so they are weak symbols: enabling
# them here while prebuilt Conan C++ packages (luau, assimp, draco, cloudini) were
# compiled without them lets the linker hand one instantiation to code built under
# the other assumption. The instrumented AppImage reported exactly that — a
# container-overflow inside Luau::BytecodeBuilder::emitAux, in a vector Luau owns
# end to end, reached via pj_scripting's luau_engine.cpp:341. That is a false
# positive from the ODR mismatch, not a defect in Luau or in PJ4.
#
# Turn it ON only for a run where every C++ translation unit sharing a std::vector
# is built with it — then the container-overflow self-test in pj_datastore proves
# it is active.
option(PJ_SANITIZE_CONTAINERS "Enable libstdc++ container annotations with AddressSanitizer (unsafe with prebuilt C++ deps)" OFF)

option(PJ_ENABLE_MSAN "Enable MemorySanitizer (Clang + instrumented libc++ only; Qt-free targets only)" OFF)

# Qt-Advanced-Docking is vendored third-party code we do not maintain. Its own
# teardown dereferences a QPointer<CDockManager> from the CDockContainerWidget
# base destructor while CDockManager is mid-destruction, which UBSan's vptr check
# reports in every test that destroys a dock manager. OFF compiles that library
# without instrumentation, the same standing as prebuilt Qt: unchecked, not fixed.
option(PJ_SANITIZE_QT_ADS "Instrument the vendored Qt-Advanced-Docking library in sanitizer lanes" OFF)

if(PJ_ENABLE_SANITIZERS AND PJ_ENABLE_TSAN)
  message(FATAL_ERROR "PJ_ENABLE_SANITIZERS (ASan) and PJ_ENABLE_TSAN are mutually exclusive")
endif()
if(PJ_ENABLE_MSAN AND (PJ_ENABLE_SANITIZERS OR PJ_ENABLE_TSAN))
  message(FATAL_ERROR "PJ_ENABLE_MSAN is mutually exclusive with PJ_ENABLE_SANITIZERS (ASan) and PJ_ENABLE_TSAN")
endif()

if(PJ_ENABLE_MSAN)
  # MSan reports reads of memory it never observed being WRITTEN, so any
  # uninstrumented code that writes a buffer makes the read of it look like a bug
  # — the report lands in our instrumented code, naming a value the uninstrumented
  # library initialised correctly. That is the opposite of ASan (uninstrumented
  # code is merely unchecked) and worse than TSan (whose noise is filterable):
  # MSan registers NO runtime suppressions option at all. So the whole in-process
  # closure must be instrumented, which rules out prebuilt Qt and the GL stack,
  # and confines this lane to Qt-free targets. build.sh enforces that target list.
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
      "PJ_ENABLE_MSAN requires Clang: MemorySanitizer does not exist in GCC "
      "(gcc rejects -fsanitize=memory). Got ${CMAKE_CXX_COMPILER_ID}.")
  endif()
endif()

# Clang links its sanitizer runtimes statically into executables by default, which
# fails twice here. pj_app links --exclude-libs,ALL, which hides a static runtime's
# symbols from the dynamic symbol table, so instrumented plugins fail to dlopen with
# "undefined symbol: __asan_report_load4". And the SDK links every plugin and parser
# module with -z defs, which rejects a module whose __asan_*/__tsan_* symbols only
# the executable would provide. The shared runtime satisfies both, the model GCC's
# libasan.so and libtsan.so already use. Build-tree binaries get an rpath to it;
# packaging bundles it and linuxdeploy rewrites that rpath to the AppDir.
function(pj_use_clang_shared_sanitizer_runtime runtime)
  set(_name "libclang_rt.${runtime}-${CMAKE_SYSTEM_PROCESSOR}.so")
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${_name}
    OUTPUT_VARIABLE _path
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT EXISTS "${_path}")
    message(FATAL_ERROR
      "Clang sanitizer runtime ${_name} not found (${CMAKE_CXX_COMPILER} -print-file-name "
      "returned '${_path}'). Install the libclang-rt development package for this Clang.")
  endif()
  get_filename_component(_dir "${_path}" DIRECTORY)
  add_link_options(-shared-libsan "-Wl,-rpath,${_dir}")
  message(STATUS "Sanitizers: Clang shared runtime ${_path}")
endfunction()

set(PJ_SANITIZERS_ACTIVE FALSE)
if(PJ_ENABLE_SANITIZERS)
  set(PJ_SANITIZERS_ACTIVE TRUE)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)

  if(PJ_ENABLE_UBSAN)
    # UBSan reports must fail tests instead of allowing a successful exit.
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all)
    add_link_options(-fsanitize=undefined -fno-sanitize-recover=all)
    message(STATUS "Sanitizers: AddressSanitizer + UndefinedBehaviorSanitizer")
  else()
    message(STATUS "Sanitizers: AddressSanitizer")
  endif()

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    pj_use_clang_shared_sanitizer_runtime(asan)
  endif()

  if(PJ_SANITIZE_CONTAINERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # These macros must be consistent across translation units that exchange a std::vector.
    # Prebuilt C++ Conan packages were built without them; PJ_SANITIZE_CONTAINERS=OFF
    # is the escape hatch if boundary false positives appear.
    add_compile_definitions(_GLIBCXX_SANITIZE_STD_ALLOCATOR=1 _GLIBCXX_SANITIZE_VECTOR=1)
  endif()
elseif(PJ_ENABLE_TSAN)
  set(PJ_SANITIZERS_ACTIVE TRUE)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    pj_use_clang_shared_sanitizer_runtime(tsan)
  endif()
  message(STATUS "Sanitizers: ThreadSanitizer")
elseif(PJ_ENABLE_MSAN)
  set(PJ_SANITIZERS_ACTIVE TRUE)
  # track-origins=2 reports WHERE the uninitialised value came from, not just
  # where it was read. Without it an MSan report names a symptom far from its
  # cause and is usually unactionable; the extra slowdown is worth it in a lane
  # this narrow. -fno-omit-frame-pointer keeps those origin stacks readable.
  add_compile_options(-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer)
  add_link_options(-fsanitize=memory)
  # MSan places its shadow at fixed addresses and needs the executable loaded
  # inside the range it reserves. A non-PIE binary sits outside it and the
  # process dies before main() with "can not mmap the shadow memory" — which
  # looks like a broken lane, not a missing link flag. -pie goes on the EXE
  # linker flags rather than add_link_options so it cannot reach shared
  # libraries, where it is invalid.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  string(APPEND CMAKE_EXE_LINKER_FLAGS " -pie")
  # The instrumented libc++ must be used for BOTH compiling and linking: an MSan
  # binary against the system libstdc++ reports every std:: container read as
  # uninitialised, because that library was never instrumented.
  if(PJ_MSAN_LIBCXX_ROOT)
    add_compile_options(-stdlib=libc++ -nostdinc++ "-isystem${PJ_MSAN_LIBCXX_ROOT}/include/c++/v1")
    add_link_options(-stdlib=libc++ "-L${PJ_MSAN_LIBCXX_ROOT}/lib"
                     "-Wl,-rpath,${PJ_MSAN_LIBCXX_ROOT}/lib")
    message(STATUS "Sanitizers: MemorySanitizer (instrumented libc++ at ${PJ_MSAN_LIBCXX_ROOT})")
  else()
    message(FATAL_ERROR
      "PJ_ENABLE_MSAN needs -DPJ_MSAN_LIBCXX_ROOT=<prefix> pointing at a libc++ "
      "built with -fsanitize=memory. The distro libc++ is NOT instrumented, and "
      "linking it makes every std:: read report as uninitialised. "
      "Build one with scripts/build_msan_libcxx.sh.")
  endif()
else()
  message(STATUS "Sanitizers: disabled")
endif()

if(PJ_SANITIZERS_ACTIVE)
  # Instrumentation changes inlining and value-range propagation, so warnings fire
  # on code that is clean at -O2. The cache option also applies to the in-tree SDK.
  list(REMOVE_ITEM PJ_WARNING_FLAGS -Werror)
  set(PJ_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
endif()
