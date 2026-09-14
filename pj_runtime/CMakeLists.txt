find_package(Qt6 REQUIRED COMPONENTS Core Network Xml)
find_package(nlohmann_json REQUIRED)
find_package(tsl-robin-map REQUIRED)
# Only the recording SINK is platform-bound: PjWasmDependencies provides no
# mcap package, and a browser has no recordings folder to write into.
if(NOT EMSCRIPTEN)
    find_package(mcap REQUIRED)  # session recorder: MCAP writer (header-only; Conan supplies lz4/zstd)
endif()
if(PJ_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()

add_library(pj_runtime STATIC
    src/AppSession.cpp
    src/CatalogModel.cpp
    src/CurveColorRegistry.cpp
    src/DataProcessorService.cpp
    src/DatasetQualifiedName.cpp
    src/DataProcessorsKindRouter.cpp
    src/DataProcessorsRuntimeHost.cpp
    src/DataSourceRuntimeHost.cpp
    src/MarkerService.cpp
    src/MarkerTopics.cpp
    src/MarkersRuntimeHost.cpp
    src/ObjectIngestTap.cpp
    src/DiagnosticHistory.cpp
    src/ExtensionCatalogService.cpp
    src/HttpGet.cpp
    src/PluginRuntimeCatalog.cpp
    src/PlaybackControlHost.cpp
    src/PlaybackEngine.cpp
    src/ParserRoutingService.cpp
    src/PlotTabsRuntimeHost.cpp
    src/QSettingsBackend.cpp
    src/Recorder.cpp
    src/RecordingService.cpp
    src/ServiceRegistration.cpp
    src/SessionManager.cpp
    src/TelemetryPing.cpp
    src/ToolboxRuntimeHost.cpp
    src/TopicDemandTracker.cpp
    src/UpdateChecker.cpp
    src/ViewportRuntimeHost.cpp
    include/pj_runtime/AppSession.h
    include/pj_runtime/CatalogModel.h
    include/pj_runtime/CurveColorRegistry.h
    include/pj_runtime/CurveDescriptor.h
    include/pj_runtime/DataProcessorService.h
    include/pj_runtime/HistoryScope.h
    include/pj_runtime/DataSourceRuntimeHost.h
    include/pj_runtime/DiagnosticHistory.h
    include/pj_runtime/ExtensionCatalogService.h
    include/pj_runtime/HttpGet.h
    include/pj_runtime/PluginRuntimeCatalog.h
    include/pj_runtime/SourceCacheStore.h
    include/pj_runtime/IDataWidget.h
    include/pj_runtime/MarkerService.h
    include/pj_runtime/MarkersRuntimeHost.h
    include/pj_runtime/ObjectIngestTap.h
    include/pj_runtime/PlaybackControlHost.h
    include/pj_runtime/PlaybackEngine.h
    include/pj_runtime/ParserRoutingService.h
    include/pj_runtime/PlotTabsRuntimeHost.h
    include/pj_runtime/QSettingsBackend.h
    include/pj_runtime/RecordTap.h
    include/pj_runtime/Recorder.h
    include/pj_runtime/RecordingFormat.h
    include/pj_runtime/RecordingService.h
    include/pj_runtime/RecordingSink.h
    include/pj_runtime/RecordingTypes.h
    include/pj_runtime/ServiceRegistration.h
    include/pj_runtime/SessionManager.h
    include/pj_runtime/TelemetryPing.h
    include/pj_runtime/ToolboxRuntimeHost.h
    include/pj_runtime/TopicDemandTracker.h
    include/pj_runtime/UpdateChecker.h
    include/pj_runtime/UpdateVersion.h
    include/pj_runtime/ViewportRuntimeHost.h
)

set_target_properties(pj_runtime PROPERTIES
    AUTOMOC ON
)

target_include_directories(pj_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(NOT EMSCRIPTEN)
    # The source cache is desktop-only (no persistent disk in the browser).
    target_sources(pj_runtime PRIVATE src/SourceCacheStore.cpp)
endif()

if(EMSCRIPTEN)
    # DiagnosticHistory uses the lightweight QtDiagnosticBridge type. The wasm
    # app compiles that bridge directly while the rest of marketplace is absent.
    target_include_directories(pj_runtime PRIVATE ${CMAKE_SOURCE_DIR}/pj_marketplace/include)
endif()

target_link_libraries(pj_runtime PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::Xml
    pj_datastore
    $<$<NOT:$<BOOL:${EMSCRIPTEN}>>:pj_marketplace>
    plotjuggler_sdk::plugin_host
    nlohmann_json::nlohmann_json
  PRIVATE
    # Source-cache mechanics (per-identity locks, lease-then-validate lookup,
    # atomic publish, LRU cleanup) come from the SDK; SourceCacheStore wraps them.
    $<$<NOT:$<BOOL:${EMSCRIPTEN}>>:plotjuggler_sdk::source>
    tsl::robin_map
    pj_internal_fmt
    $<$<NOT:$<BOOL:${EMSCRIPTEN}>>:mcap::mcap>
    pj_scripting  # DataProcessorService routes by-id applyFilter through the Luau FilterCatalogue
)

# Only the MCAP sink is platform-bound (it links mcap and writes to a real
# filesystem). RecordingService and the recording interfaces are built
# everywhere; where no sink is compiled in, RecordingService::isSupported()
# reports false and the shell drops its Record affordances at runtime.
if(NOT EMSCRIPTEN)
    target_sources(pj_runtime PRIVATE
        src/McapRecordingWriter.cpp
        include/pj_runtime/McapRecordingWriter.h
        src/SourceCaptureService.cpp
        include/pj_runtime/SourceCaptureService.h
    )
endif()

if(PJ_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    # Discovery gives every case its own process while the static runtime
    # dependency closure is linked only once.
    set(PJ_RUNTIME_TEST_SOURCES
        tests/source_cache_store_test.cpp
        tests/object_ingest_tap_test.cpp
        tests/time_test.cpp
        # M1: re-prove the streaming gate through a Luau LuaSisoTransform
        tests/data_processor_service_test.cpp
        # links the shared marker engine (runMarkerScript) the service drives
        tests/marker_service_test.cpp
        # links the shared marker engine the service drives
        tests/markers_runtime_host_test.cpp
        tests/data_processors_kind_router_test.cpp
        # Dataset-qualified input names ("dataset_source:topic/field") coming from a
        # plugin: the parse, and the ambiguity a bare topic leaves behind.
        tests/dataset_qualified_name_test.cpp
        # M4.2: plugin-created transforms compile a Luau script via the catalogue
        tests/data_processor_service_transform_test.cpp
        # M4.2: the bridge compiles a Luau transform script through the service
        tests/data_processors_runtime_host_test.cpp
        # Pure header-only version math (UpdateVersion.h) — no networking, no lib.
        tests/update_version_test.cpp
        # Drives UpdateChecker's reply state machine against a local QTcpServer.
        # Qt6::Test supplies QSignalSpy; Qt6::Network comes transitively via pj_runtime.
        tests/update_checker_test.cpp
        # Drives TelemetryPing's POST + reply handling against a local capturing
        # QTcpServer — enforces the payload privacy contract on the wire (the
        # allowed-key set + salted-hash user id), no real network.
        tests/telemetry_ping_test.cpp
        tests/app_session_test.cpp
        tests/playback_engine_test.cpp
        # Qt6::Test supplies QSignalSpy for the displayOffsetChanged emission test.
        tests/session_manager_test.cpp
        # Qt6::Test supplies QSignalSpy for signal verification tests.
        tests/session_manager_ingest_test.cpp
        # Qt6::Test supplies QSignalSpy for the advertised-topics idempotence test.
        tests/catalog_model_test.cpp
        # Qt6::Test supplies QSignalSpy for the activeTopicsChanged emission tests.
        tests/topic_demand_tracker_test.cpp
        tests/payload_anchor_test.cpp
        tests/curve_color_registry_test.cpp
        tests/qsettings_backend_test.cpp
        tests/toolbox_runtime_host_test.cpp
        # Env-gated integration test against the REAL parser_ros plugin (SKIPs
        # unless PJ_REAL_ROS_PARSER_DIR points at the built parser_ros .so dir).
        tests/toolbox_parser_ingest_real_ros_test.cpp
        tests/data_source_runtime_host_object_ingest_test.cpp
        tests/data_source_runtime_host_record_tap_test.cpp
        tests/data_source_runtime_host_stream_swap_test.cpp
        # M5: notify_available_topics threading + a-priori classification fallback.
        tests/data_source_runtime_host_advertise_test.cpp
        # Route-aware parser selection (SDK 0.22): claim admission, lazy exact-claim
        # discovery, per-route winners, divergent-winner bindings, and the legacy
        # fallback for encodings outside the SDK claim registry.
        tests/extension_catalog_parser_routes_test.cpp
        tests/plugin_runtime_catalog_test.cpp
        # Bundled-plugin seeding (share -> marketplace sync) + scan-tier precedence.
        tests/extension_catalog_seed_test.cpp
        # SDK 0.28: the runtime-side hosts a toolbox binding drives. Playback is
        # global; viewport and plot-tab operations are scoped to the calling
        # plugin's owned tabs.
        tests/playback_control_host_test.cpp
        tests/viewport_runtime_host_test.cpp
        tests/plot_tabs_runtime_host_test.cpp
        # D4: an ephemeral (history-exempt) transform must still be visible in the
        # ABI catalog snapshot a plugin reads, and must vanish from it on remove.
        # pj_scripting: the Luau transform compiles via the transient-catalogue fallback.
        tests/data_processor_ephemeral_abi_visibility_test.cpp
    )

    if(NOT EMSCRIPTEN)
        list(APPEND PJ_RUNTIME_TEST_SOURCES
            tests/mcap_recording_writer_test.cpp
            # Qt6::Test supplies QSignalSpy; mcap::mcap only for reading back what
            # the service recorded.
            tests/recording_service_test.cpp
            # The whole recording pipeline end to end: the same host + real parser
            # plugin the record-tap test builds on, driven through RecordingService
            # into an MCAP file that is then read back. Desktop-only (mcap + the writer).
            tests/recording_pipeline_test.cpp
            # M3 source capture: the completion-contract gate + cache publish +
            # cache-first resolve, over the same real host/parser fixture.
            tests/source_capture_service_test.cpp
            # M3 owner wiring: ToolboxRuntimeHost's parser-ingest lifecycle
            # (create/release/discard/teardown) driving SourceCaptureService.
            tests/toolbox_capture_wiring_test.cpp
        )
    endif()

    # The Recorder is format-agnostic (it drives a fake RecordingSink) and links
    # no Qt, so the test compiles Recorder.cpp on its own against the SDK rather
    # than linking pj_runtime: it needs no mcap, builds for wasm, and runs in
    # the TSan job (build.sh TSAN_TESTS), which must compile no Qt code. The
    # ctest name equals the target name because that job filters on it.
    add_executable(recorder_test
        tests/recorder_test.cpp
        src/Recorder.cpp
    )
    target_compile_options(recorder_test PRIVATE ${PJ_WARNING_FLAGS})
    target_include_directories(recorder_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(recorder_test PRIVATE
        plotjuggler_sdk::base
        GTest::gtest_main
    )
    add_test(NAME recorder_test COMMAND recorder_test)
    set_tests_properties(recorder_test PROPERTIES TIMEOUT 60)

    add_library(runtime_host_object_parser_plugin SHARED
        tests/runtime_host_object_parser_plugin.cpp
    )
    target_compile_features(runtime_host_object_parser_plugin PRIVATE cxx_std_20)
    target_compile_options(runtime_host_object_parser_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(runtime_host_object_parser_plugin PRIVATE
        plotjuggler_sdk::plugin_sdk
    )

    # Streaming pause/resume two-engine lockstep, exercised at the
    # DataSourceRuntimeHost integration layer — the runtime-level companion to
    # pj_datastore/tests/streaming_mirror_test.cpp. Those unit tests verify the
    # createTopicField/setSecondaryEngine MECHANISM; these verify the host
    # actually WIRES it (setSecondaryEngine on the source + each parser binding)
    # and that a plugin's cached TopicHandle/FieldHandle survives a
    # setDataEngineTarget pause/resume swap end to end. The caching parser
    # plugin covers the parser-path variant (parser_protobuf-style cached
    # FieldHandle) that the unit tests do not exercise.
    add_library(streaming_caching_parser_plugin SHARED
        tests/streaming_caching_parser_plugin.cpp
    )
    target_compile_features(streaming_caching_parser_plugin PRIVATE cxx_std_20)
    target_compile_options(streaming_caching_parser_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(streaming_caching_parser_plugin PRIVATE
        plotjuggler_sdk::plugin_sdk
    )

    add_executable(extension_catalog_service_thread_safety_test
        tests/extension_catalog_service_thread_safety_test.cpp
    )
    target_compile_options(extension_catalog_service_thread_safety_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(extension_catalog_service_thread_safety_test PRIVATE
        pj_runtime
        GTest::gtest
    )
    # Test name matches the target so build.sh's TSAN_TESTS ctest -R filter
    # (which lists target names) picks it up under ./build.sh --tsan.
    add_test(NAME extension_catalog_service_thread_safety_test COMMAND extension_catalog_service_thread_safety_test)

    # SDK example/test plugins the host tests dlopen. The SDK packages the
    # sources and builds them through pj_add_sdk_test_fixture(); the aliases
    # keep the <name>_plugin spelling every consumer here and in
    # pj_marketplace / pj_app already uses. An in-tree SDK build defines the
    # real targets itself, so the guard stays.
    if(NOT TARGET mock_data_source_plugin)
      foreach(name IN ITEMS mock_data_source mock_file_source mock_toolbox missing_id_data_source)
        pj_add_sdk_test_fixture(${name} fixture_target)
        add_library(${name}_plugin ALIAS ${fixture_target})
      endforeach()
    endif()

    # PluginRuntimeCatalog — duplicate-resolution policy (version/compat/authoritative)
    # relocated from the SDK into the host. These mocks moved with the tests from the
    # SDK; mock_data_source_plugin (v1) is the SDK example fixture above.
    add_library(mock_data_source_v2_plugin SHARED tests/mock_data_source_v2_plugin.cpp)
    target_compile_features(mock_data_source_v2_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_v2_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(mock_data_source_v2_plugin PRIVATE plotjuggler_sdk::base)

    # v1/v2 builds of a mock whose image stays resident after dlclose. Two
    # mechanisms: -z nodelete (DF_1_NODELETE, compiler-independent) and a bound
    # STB_GNU_UNIQUE anchor (models real GCC plugin builds; needs default
    # symbol visibility). Consumed by extension_manager_test's staged-promotion
    # regression coverage.
    add_library(mock_data_source_pinning_plugin SHARED tests/mock_data_source_pinning_plugin.cpp)
    target_compile_features(mock_data_source_pinning_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_pinning_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_compile_definitions(mock_data_source_pinning_plugin PRIVATE PIN_MOCK_VERSION="1.0.0")
    target_link_libraries(mock_data_source_pinning_plugin PRIVATE plotjuggler_sdk::base)

    add_library(mock_data_source_pinning_v2_plugin SHARED tests/mock_data_source_pinning_plugin.cpp)
    target_compile_features(mock_data_source_pinning_v2_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_pinning_v2_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_compile_definitions(mock_data_source_pinning_v2_plugin PRIVATE PIN_MOCK_VERSION="2.0.0")
    target_link_libraries(mock_data_source_pinning_v2_plugin PRIVATE plotjuggler_sdk::base)

    if(NOT WIN32)
      target_link_options(mock_data_source_pinning_plugin PRIVATE -Wl,-z,nodelete)
      target_link_options(mock_data_source_pinning_v2_plugin PRIVATE -Wl,-z,nodelete)
    endif()

    add_library(mock_data_source_incompatible_plugin SHARED tests/mock_data_source_incompatible_plugin.cpp)
    target_compile_features(mock_data_source_incompatible_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_incompatible_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(mock_data_source_incompatible_plugin PRIVATE plotjuggler_sdk::base)

    add_library(mock_data_source_compatible_floor_plugin SHARED tests/mock_data_source_compatible_floor_plugin.cpp)
    target_compile_features(mock_data_source_compatible_floor_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_compatible_floor_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(mock_data_source_compatible_floor_plugin PRIVATE plotjuggler_sdk::base)

    add_library(mock_data_source_incompatible_low_plugin SHARED tests/mock_data_source_incompatible_low_plugin.cpp)
    target_compile_features(mock_data_source_incompatible_low_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_incompatible_low_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(mock_data_source_incompatible_low_plugin PRIVATE plotjuggler_sdk::base)

    add_library(mock_data_source_malformed_floor_plugin SHARED tests/mock_data_source_malformed_floor_plugin.cpp)
    target_compile_features(mock_data_source_malformed_floor_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_malformed_floor_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(mock_data_source_malformed_floor_plugin PRIVATE plotjuggler_sdk::base)

    add_library(mock_data_source_malformed_version_plugin SHARED tests/mock_data_source_malformed_version_plugin.cpp)
    target_compile_features(mock_data_source_malformed_version_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_malformed_version_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(mock_data_source_malformed_version_plugin PRIVATE plotjuggler_sdk::base)

    add_library(mock_data_source_newer_sdk_plugin SHARED tests/mock_data_source_newer_sdk_plugin.cpp)
    target_compile_features(mock_data_source_newer_sdk_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_newer_sdk_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(mock_data_source_newer_sdk_plugin PRIVATE plotjuggler_sdk::base)

    # The parser-ingest tests in toolbox_runtime_host_test load the same stub
    # parser plugin the DataSource ingest test uses. Shared path definitions
    # keep every consumer pointed at the same fixture image.
    # no WARNINGS: 4 of 35 merged tests never compiled with -Werror
    pj_add_test_runner(pj_runtime_tests MAIN core
        SOURCES ${PJ_RUNTIME_TEST_SOURCES}
        LIBS
            pj_runtime
            pj_scripting
            # mcap::mcap: recording_test_utils.h (the IsolatedQtSettings sandbox the
            # settings tests use) includes mcap/reader.hpp, which pj_runtime keeps
            # PRIVATE.
            mcap::mcap
            Qt6::Test
        DEFINES
            PJ_RUNTIME_HOST_OBJECT_PARSER_PATH="$<TARGET_FILE:runtime_host_object_parser_plugin>"
            PJ_STREAMING_CACHING_PARSER_PATH="$<TARGET_FILE:streaming_caching_parser_plugin>"
            PJ_MOCK_DATA_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_plugin>"
            PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_v2_plugin>"
            PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_incompatible_plugin>"
            PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_LOW_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_incompatible_low_plugin>"
            PJ_MOCK_DATA_SOURCE_MALFORMED_FLOOR_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_malformed_floor_plugin>"
            PJ_MOCK_DATA_SOURCE_MALFORMED_VERSION_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_malformed_version_plugin>"
            PJ_MOCK_DATA_SOURCE_NEWER_SDK_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_newer_sdk_plugin>"
            PJ_MOCK_TOOLBOX_PLUGIN_PATH="$<TARGET_FILE:mock_toolbox_plugin>"
    )
    set_target_properties(pj_runtime_tests PROPERTIES AUTOMOC ON)
    add_dependencies(pj_runtime_tests
        runtime_host_object_parser_plugin streaming_caching_parser_plugin
        mock_data_source_plugin mock_data_source_v2_plugin
        mock_data_source_incompatible_plugin mock_data_source_incompatible_low_plugin
        mock_data_source_malformed_floor_plugin mock_data_source_malformed_version_plugin
        mock_data_source_newer_sdk_plugin mock_toolbox_plugin
    )
endif()
