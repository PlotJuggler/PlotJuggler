# ---------------------------------------------------------------------------
# pj_datastore — columnar storage engine for the PlotJuggler application.
# The root CMakeLists provides the plotjuggler_sdk and fmt targets before it adds this module.
# ---------------------------------------------------------------------------

option(
  PJ_BUILD_PARQUET_IMPORT_EXAMPLE
  "Build parquet_import example (requires full Arrow C++ and Parquet)"
  OFF
)

find_package(tsl-robin-map REQUIRED)
find_package(nlohmann_json REQUIRED)  # marker_topic_metadata.cpp
# log_entries.cpp: sealed log chunks. The browser build provides the targets itself (cmake/PjWasmDependencies.cmake).
# zstd's config defines libzstd_static and/or libzstd_shared; the libzstd alias exists only from 1.5.6 (Nix ships
# the shared one alone).
if(NOT TARGET zstd::libzstd_static AND NOT TARGET zstd::libzstd_shared AND NOT TARGET zstd::libzstd)
  find_package(zstd CONFIG REQUIRED)
endif()
foreach(_pj_zstd zstd::libzstd_static zstd::libzstd_shared zstd::libzstd)
  if(TARGET ${_pj_zstd})
    set(PJ_DATASTORE_ZSTD ${_pj_zstd})
    break()
  endif()
endforeach()
# Google Benchmark is skipped in the MemorySanitizer lane, which builds no benchmarks: requiring it would mean
# instrumenting a library nothing there links, and its regex-backend detection fails against an instrumented libc++.
if(PJ_BUILD_TESTS AND NOT PJ_ENABLE_MSAN)
  find_package(benchmark CONFIG REQUIRED)
endif()

# nanoarrow::nanoarrow + nanoarrow::nanoarrow_ipc: an installed package, else the pinned source.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/PjNanoarrow.cmake)

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
  src/incremental_engine.cpp
  src/builtin_transforms.cpp
  src/plugin_data_host.cpp
  src/colormap_registry.cpp
  src/sequential_uid.cpp
  src/ordered_entries.cpp
  src/log_entries.cpp
  src/object_store.cpp
  src/marker_topic_metadata.cpp
  src/resident_payload_pool.cpp
  # PJ::proc, next to the ISISOTransform interface ProcessorSisoAdapter implements.
  src/data_processor.cpp
  src/processor_siso_adapter.cpp
)
target_include_directories(pj_datastore PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(pj_datastore PUBLIC cxx_std_20)
target_compile_options(pj_datastore PRIVATE
  ${PJ_WARNING_FLAGS}
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
    nlohmann_json::nlohmann_json
    ${PJ_DATASTORE_ZSTD}
    $<BUILD_INTERFACE:pj_internal_fmt>
    nanoarrow::nanoarrow
    nanoarrow::nanoarrow_ipc
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
  set(PJ_DATASTORE_TESTS
    tests/crash_handler_selftest.cpp
    tests/type_registry_test.cpp
    tests/buffer_test.cpp
    tests/column_buffer_test.cpp
    tests/encoding_test.cpp
    tests/topic_storage_test.cpp
    tests/chunk_growth_test.cpp
    tests/query_test.cpp
    tests/series_reader_test.cpp
    tests/engine_integration_test.cpp
    tests/out_of_order_ingest_test.cpp
    tests/incremental_engine_test.cpp
    tests/array_expansion_test.cpp
    tests/regression_test.cpp
    tests/sequential_uid_test.cpp
    tests/merge_test.cpp
    tests/ordered_entries_test.cpp
    tests/log_entries_test.cpp
    tests/object_store_log_test.cpp
    tests/object_store_test.cpp
    tests/marker_topic_metadata_test.cpp
    tests/resident_payload_pool_test.cpp
    tests/object_merge_test.cpp
    tests/clear_dataset_test.cpp
    tests/plugin_data_host_object_test.cpp
    tests/plugin_data_host_object_read_test.cpp
    tests/plugin_data_host_catalog_v2_test.cpp
    tests/plugin_host_dataset_test.cpp
    tests/streaming_mirror_test.cpp
    tests/ingest_type_coercion_test.cpp
    tests/sample_test.cpp
    tests/data_processor_test.cpp
    tests/processor_siso_adapter_test.cpp
    # Not built: tests/plugin_host_read_test.cpp and tests/plugin_host_write_test.cpp target an ABI the SDK no
    # longer has.

    # plugin_parser_object_write_test uses MessageParserPluginBase, so it needs
    # the plugin SDK for message_parser_plugin_base.hpp and builtin object types.
    tests/plugin_parser_object_write_test.cpp

    # Builds its IPC input with nanoarrow, linked below.
    tests/arrow_import_test.cpp

    tests/arrow_stream_round_trip_test.cpp
  )

  # No WARNINGS: these tests have never been built with -Werror. MAIN plain: no test here uses Qt.
  pj_add_test_runner(pj_datastore_tests
    MAIN plain
    SOURCES ${PJ_DATASTORE_TESTS}
    LIBS
      pj_datastore plotjuggler_sdk::plugin_sdk
      nanoarrow::nanoarrow nanoarrow::nanoarrow_ipc
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

  # A deadlock must fail these tests, not hang ctest: lock-ordering regressions are what they guard. CI also passes a
  # global --timeout.
  set_tests_properties(engine_concurrency_test engine_thread_safety_test
    PROPERTIES TIMEOUT 90)

  # Inherit the module's sanitizer settings so losing instrumentation makes the self-tests fail.
  if(PJ_ENABLE_SANITIZERS)
    pj_add_test_runner(sanitizer_selftest
      MAIN plain
      SOURCES tests/sanitizer_selftest.cpp
      LIBS pj_datastore
    )
  endif()

  if(PJ_ENABLE_MSAN)
    # Plain executable, no gtest: the point is the process exit status, and
    # linking a test framework would add code paths between the fault and exit.
    add_executable(sanitizer_msan_selftest tests/sanitizer_msan_selftest.cpp)
    target_compile_features(sanitizer_msan_selftest PRIVATE cxx_std_20)
    add_test(NAME sanitizer_msan_selftest COMMAND sanitizer_msan_selftest)
    # PASS only on the MSan report itself. WILL_FAIL would also accept a loader
    # error or a runtime startup abort that never reached the deliberate read,
    # and a lane that cannot detect that read proves nothing about the tests that pass beside it.
    set_tests_properties(sanitizer_msan_selftest PROPERTIES
      PASS_REGULAR_EXPRESSION "MemorySanitizer: use-of-uninitialized-value"
      ENVIRONMENT "MSAN_OPTIONS=halt_on_error=1:exitcode=77:abort_on_error=0"
      TIMEOUT 30
    )
  endif()

  if(PJ_ENABLE_TSAN)
    find_package(Threads REQUIRED)
    add_executable(sanitizer_race_selftest tests/sanitizer_race_selftest.cpp)
    target_compile_features(sanitizer_race_selftest PRIVATE cxx_std_20)
    target_link_libraries(sanitizer_race_selftest PRIVATE Threads::Threads)
    add_test(NAME sanitizer_race_selftest COMMAND sanitizer_race_selftest)
    # PASS only on the race report itself: a clean exit (uninstrumented) and a
    # startup abort ("unexpected memory mapping") must both fail.
    set_tests_properties(sanitizer_race_selftest PROPERTIES
      PASS_REGULAR_EXPRESSION "ThreadSanitizer: data race"
      ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:exitcode=66:abort_on_error=0"
      TIMEOUT 30
    )
  endif()

  # Plugin host write test — DISABLED for v4 ABI migration.
  # Exercises v3 appendArrowIpc/readSeries; rewrite in Phase 1b when the
  # Arrow-stream write path and read_series_arrow are implemented.
  # add_executable(plugin_host_write_test tests/plugin_host_write_test.cpp)
  # target_link_libraries(plugin_host_write_test PRIVATE
  #   pj_datastore
  #   nanoarrow::nanoarrow
  #   nanoarrow::nanoarrow_ipc
  #   GTest::gtest_main)
  # add_test(NAME plugin_host_write_test COMMAND plugin_host_write_test)

  # ---------------------------------------------------------------------------
  # Benchmarks
  # ---------------------------------------------------------------------------

  if(NOT PJ_ENABLE_MSAN)
    add_executable(read_benchmark benchmarks/read_benchmark.cpp)
    target_link_libraries(read_benchmark PRIVATE pj_datastore benchmark::benchmark)

    add_executable(ingest_benchmark benchmarks/ingest_benchmark.cpp)
    target_link_libraries(ingest_benchmark PRIVATE pj_datastore benchmark::benchmark)
  endif()

  # Contention probe for the engine lock: plain main(), since contention is measured over time, not in a micro-loop.
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
