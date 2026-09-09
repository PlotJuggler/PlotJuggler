# ---------------------------------------------------------------------------
# pj_datastore — columnar storage engine for the PlotJuggler application.
#
# Depends on pj_base + fmt (both provided by the plotjuggler_sdk SDK submodule,
# which the root CMakeLists adds BEFORE this module so their targets exist) plus
# tsl::robin_map + nanoarrow.
#
# This module was previously part of the plotjuggler_sdk submodule. The
# external-dependency detection below (tsl-robin-map / benchmark / nanoarrow /
# nanoarrow_ipc) was relocated here from that submodule's root CMakeLists when
# the engine moved into the application repo.
# ---------------------------------------------------------------------------

option(
  PJ_BUILD_PARQUET_IMPORT_EXAMPLE
  "Build parquet_import example (requires full Arrow C++ and Parquet)"
  OFF
)

find_package(tsl-robin-map REQUIRED)
if(PJ_BUILD_TESTS)
  find_package(benchmark CONFIG REQUIRED)
endif()

# --- nanoarrow core ---
find_package(nanoarrow CONFIG QUIET)

set(PJ_NANOARROW_TARGET "")
if(TARGET nanoarrow::nanoarrow)
  set(PJ_NANOARROW_TARGET nanoarrow::nanoarrow)
elseif(TARGET nanoarrow::nanoarrow_static)
  set(PJ_NANOARROW_TARGET nanoarrow::nanoarrow_static)
elseif(TARGET nanoarrow::nanoarrow_shared)
  set(PJ_NANOARROW_TARGET nanoarrow::nanoarrow_shared)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(NANOARROW QUIET nanoarrow)
    if(NANOARROW_FOUND)
      add_library(pj_nanoarrow INTERFACE)
      target_include_directories(pj_nanoarrow INTERFACE
        ${NANOARROW_INCLUDE_DIRS}
      )
      target_link_libraries(pj_nanoarrow INTERFACE ${NANOARROW_LIBRARIES})
      set(PJ_NANOARROW_TARGET pj_nanoarrow)
    endif()
  endif()
endif()

if(PJ_NANOARROW_TARGET STREQUAL "")
  message(FATAL_ERROR
    "nanoarrow is required for pj_datastore. "
    "Install nanoarrow and expose a CMake target "
    "(nanoarrow::nanoarrow / nanoarrow::nanoarrow_static / nanoarrow::nanoarrow_shared) "
    "or provide a pkg-config entry named 'nanoarrow'."
  )
endif()

# --- nanoarrow IPC ---
set(PJ_NANOARROW_IPC_TARGET "")
if(TARGET nanoarrow::nanoarrow_ipc)
  set(PJ_NANOARROW_IPC_TARGET nanoarrow::nanoarrow_ipc)
elseif(TARGET nanoarrow::nanoarrow_ipc_static)
  set(PJ_NANOARROW_IPC_TARGET nanoarrow::nanoarrow_ipc_static)
elseif(TARGET nanoarrow::nanoarrow_ipc_shared)
  set(PJ_NANOARROW_IPC_TARGET nanoarrow::nanoarrow_ipc_shared)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(NANOARROW_IPC QUIET nanoarrow_ipc)
    if(NANOARROW_IPC_FOUND)
      add_library(pj_nanoarrow_ipc INTERFACE)
      target_include_directories(pj_nanoarrow_ipc INTERFACE
        ${NANOARROW_IPC_INCLUDE_DIRS}
      )
      target_link_libraries(pj_nanoarrow_ipc INTERFACE ${NANOARROW_IPC_LIBRARIES})
      set(PJ_NANOARROW_IPC_TARGET pj_nanoarrow_ipc)
    endif()
  endif()
endif()

if(PJ_NANOARROW_IPC_TARGET STREQUAL "")
  message(FATAL_ERROR
    "nanoarrow IPC is required for pj_datastore. "
    "Install nanoarrow with IPC support and expose a CMake target "
    "(nanoarrow::nanoarrow_ipc / nanoarrow::nanoarrow_ipc_static / nanoarrow::nanoarrow_ipc_shared) "
    "or provide a pkg-config entry named 'nanoarrow_ipc'."
  )
endif()

# nanoarrow IPC internally depends on the flatcc runtime (libflatccrt).
# Some package managers (e.g. Conan) bundle it alongside the IPC library
# but don't declare it as a transitive link dependency. Since the IPC target
# already sets INTERFACE_LINK_DIRECTORIES, linking by name suffices.
# Upstream nanoarrow configs expose the preferred name as an ALIAS, which
# target_link_libraries rejects — resolve to the underlying target first.
get_target_property(_pj_nanoarrow_ipc_real ${PJ_NANOARROW_IPC_TARGET} ALIASED_TARGET)
if(NOT _pj_nanoarrow_ipc_real)
  set(_pj_nanoarrow_ipc_real ${PJ_NANOARROW_IPC_TARGET})
endif()
target_link_libraries(${_pj_nanoarrow_ipc_real} INTERFACE flatccrt)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(pj_datastore STATIC
  src/buffer.cpp
  src/column_buffer.cpp
  src/encoding.cpp
  src/chunk.cpp
  src/topic_storage.cpp
  src/type_registry.cpp
  src/query.cpp
  src/writer.cpp
  src/reader.cpp
  src/engine.cpp
  src/arrow_import.cpp
  src/derived_engine.cpp
  src/builtin_transforms.cpp
  src/plugin_data_host.cpp
  src/colormap_registry.cpp
  src/colormap_registry_host.cpp
  src/sequential_uid.cpp
  src/ordered_entries.cpp
  src/object_store.cpp
  src/resident_payload_pool.cpp
  # Data-processor execution substrate (the DataProcessor base + ProcessorSisoAdapter
  # that runs a processor as a DerivedEngine node — moved here from the retired pj_proc
  # module; lives WITH the ISISOTransform interface it adapts to).
  src/data_processor.cpp
  src/processor_siso_adapter.cpp
)
target_include_directories(pj_datastore PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(pj_datastore PUBLIC cxx_std_20)
target_compile_options(pj_datastore PRIVATE
  ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS}
)
set_target_properties(pj_datastore PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
if(PJ_ASSERT_THROWS)
  target_compile_definitions(pj_datastore PUBLIC PJ_ASSERT_THROWS)
endif()
target_link_libraries(pj_datastore
  PUBLIC
    plotjuggler_sdk::base
  PRIVATE
    tsl::robin_map
    $<BUILD_INTERFACE:pj_internal_fmt>
    ${PJ_NANOARROW_TARGET}
    ${PJ_NANOARROW_IPC_TARGET}
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
  set(PJ_DATASTORE_TESTS
    tests/type_registry_test.cpp
    tests/buffer_test.cpp
    tests/column_buffer_test.cpp
    tests/encoding_test.cpp
    tests/topic_storage_test.cpp
    tests/query_test.cpp
    tests/series_reader_test.cpp
    tests/engine_integration_test.cpp
    tests/out_of_order_ingest_test.cpp
    tests/derived_engine_test.cpp
    tests/array_expansion_test.cpp
    tests/regression_test.cpp
    tests/sequential_uid_test.cpp
    tests/merge_test.cpp
    tests/ordered_entries_test.cpp
    tests/object_store_test.cpp
    tests/resident_payload_pool_test.cpp
    tests/object_merge_test.cpp
    tests/clear_dataset_test.cpp
    tests/plugin_data_host_object_test.cpp
    tests/plugin_data_host_object_read_test.cpp
    tests/plugin_host_dataset_test.cpp
    tests/streaming_mirror_test.cpp
    tests/sample_test.cpp
    tests/data_processor_test.cpp
    tests/processor_siso_adapter_test.cpp
    # tests/plugin_host_read_test.cpp  # disabled until Phase 1b lands
    #   (exercises v3 toolbox read path; rewrite for read_series_arrow)

    # plugin_parser_object_write_test uses MessageParserPluginBase, so it needs
    # the plugin SDK for message_parser_plugin_base.hpp and builtin object types.
    tests/plugin_parser_object_write_test.cpp

    # Arrow import test (needs nanoarrow explicitly for building test IPC data)
    tests/arrow_import_test.cpp

    # v4 Arrow C Data Interface round-trip test (Phase 1b).
    tests/arrow_stream_round_trip_test.cpp
  )

  # Share the dependency closure while ctest still runs each case in its own process.
  # no WARNINGS: 29 of 29 merged tests never compiled with -Werror
  pj_add_test_runner(pj_datastore_tests
    MAIN core
    SOURCES ${PJ_DATASTORE_TESTS}
    LIBS
      pj_datastore plotjuggler_sdk::plugin_sdk
      ${PJ_NANOARROW_TARGET} ${PJ_NANOARROW_IPC_TARGET}
  )

  # Death tests need a separate executable; the concurrency target names are
  # also the entry points used by build.sh and CI for ThreadSanitizer runs.
  set(PJ_DATASTORE_SEPARATE_TESTS
    tests/chunk_test.cpp
    tests/engine_concurrency_test.cpp
    tests/engine_thread_safety_test.cpp
  )
  foreach(test_src ${PJ_DATASTORE_SEPARATE_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE pj_datastore GTest::gtest_main)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()

  # The concurrency / thread-safety tests must fail LOUDLY on a deadlock rather
  # than hang the whole ctest run — a lock-ordering regression (e.g. an ABBA in
  # the write-host secondary mirror) is exactly the bug class they guard. This is
  # the per-test floor; CI also passes a global --timeout.
  set_tests_properties(engine_concurrency_test engine_thread_safety_test
    PROPERTIES TIMEOUT 90)

  # Plugin host write test — DISABLED for v4 ABI migration.
  # Exercises v3 appendArrowIpc/readSeries; rewrite in Phase 1b when the
  # Arrow-stream write path and read_series_arrow are implemented.
  # add_executable(plugin_host_write_test tests/plugin_host_write_test.cpp)
  # target_link_libraries(plugin_host_write_test PRIVATE
  #   pj_datastore
  #   ${PJ_NANOARROW_TARGET}
  #   ${PJ_NANOARROW_IPC_TARGET}
  #   GTest::gtest_main)
  # add_test(NAME plugin_host_write_test COMMAND plugin_host_write_test)

  # ---------------------------------------------------------------------------
  # Benchmarks
  # ---------------------------------------------------------------------------

  add_executable(read_benchmark benchmarks/read_benchmark.cpp)
  target_link_libraries(read_benchmark PRIVATE pj_datastore benchmark::benchmark)

  add_executable(ingest_benchmark benchmarks/ingest_benchmark.cpp)
  target_link_libraries(ingest_benchmark PRIVATE pj_datastore benchmark::benchmark)

  # Threaded contention probe for the reader-writer lock (plain main(), not the
  # google-benchmark micro-loop model — contention is duration-based). Threads
  # for the writer/reader; no benchmark::benchmark dependency.
  find_package(Threads REQUIRED)
  add_executable(concurrency_benchmark benchmarks/concurrency_benchmark.cpp)
  target_link_libraries(concurrency_benchmark PRIVATE pj_datastore Threads::Threads)
endif() # PJ_BUILD_TESTS

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(PJ_BUILD_PARQUET_IMPORT_EXAMPLE)
  if(NOT TARGET Arrow::arrow_static)
    find_package(Arrow REQUIRED)
  endif()
  if(NOT TARGET Parquet::parquet_static)
    find_package(Parquet REQUIRED)
  endif()
  add_executable(parquet_import examples/parquet_import.cpp)
  target_link_libraries(parquet_import PRIVATE
    pj_datastore Arrow::arrow_static Parquet::parquet_static
  )
endif()
