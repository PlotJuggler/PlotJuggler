find_package(Qt6 REQUIRED COMPONENTS Core Network Xml)
find_package(nlohmann_json REQUIRED)
find_package(tsl-robin-map REQUIRED)
if(PJ_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()

add_library(pj_runtime STATIC
    src/AppSession.cpp
    src/CatalogModel.cpp
    src/CurveColorRegistry.cpp
    src/DataProcessorService.cpp
    src/DataProcessorsKindRouter.cpp
    src/DataProcessorsRuntimeHost.cpp
    src/DataSourceRuntimeHost.cpp
    src/MarkerService.cpp
    src/MarkersRuntimeHost.cpp
    src/DiagnosticHistory.cpp
    src/ExtensionCatalogService.cpp
    src/HttpGet.cpp
    src/PluginRuntimeCatalog.cpp
    src/PlaybackEngine.cpp
    src/QSettingsBackend.cpp
    src/ServiceRegistration.cpp
    src/SessionManager.cpp
    src/TelemetryPing.cpp
    src/ToolboxRuntimeHost.cpp
    src/TopicDemandTracker.cpp
    src/UpdateChecker.cpp
    include/pj_runtime/AppSession.h
    include/pj_runtime/CatalogModel.h
    include/pj_runtime/CurveColorRegistry.h
    include/pj_runtime/CurveDescriptor.h
    include/pj_runtime/DataProcessorService.h
    include/pj_runtime/DataSourceRuntimeHost.h
    include/pj_runtime/DiagnosticHistory.h
    include/pj_runtime/ExtensionCatalogService.h
    include/pj_runtime/HttpGet.h
    include/pj_runtime/PluginRuntimeCatalog.h
    include/pj_runtime/IDataWidget.h
    include/pj_runtime/MarkerService.h
    include/pj_runtime/MarkersRuntimeHost.h
    include/pj_runtime/PlaybackEngine.h
    include/pj_runtime/QSettingsBackend.h
    include/pj_runtime/ServiceRegistration.h
    include/pj_runtime/SessionManager.h
    include/pj_runtime/TelemetryPing.h
    include/pj_runtime/ToolboxRuntimeHost.h
    include/pj_runtime/TopicDemandTracker.h
    include/pj_runtime/UpdateChecker.h
    include/pj_runtime/UpdateVersion.h
)

set_target_properties(pj_runtime PROPERTIES
    AUTOMOC ON
)

target_include_directories(pj_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

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
    pj_data_source_host
    pj_message_parser_host
    pj_toolbox_host
    pj_plugin_catalog
    pj_base
    nlohmann_json::nlohmann_json
  PRIVATE
    tsl::robin_map
    pj_internal_fmt
    pj_scripting  # DataProcessorService routes by-id applyFilter through the Luau FilterCatalogue
)

if(PJ_BUILD_TESTS)
    add_executable(time_test
        tests/time_test.cpp
    )
    target_compile_options(time_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(time_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME TimeTest COMMAND time_test)

    add_executable(data_processor_service_test
        tests/data_processor_service_test.cpp
    )
    target_compile_options(data_processor_service_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(data_processor_service_test PRIVATE
        pj_runtime
        pj_scripting  # M1: re-prove the streaming gate through a Luau LuaSisoTransform
        GTest::gtest_main
    )
    add_test(NAME DataProcessorServiceTest COMMAND data_processor_service_test)

    add_executable(marker_service_test
        tests/marker_service_test.cpp
    )
    target_compile_options(marker_service_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(marker_service_test PRIVATE
        pj_runtime
        pj_scripting  # links the shared marker engine (runMarkerScript) the service drives
        GTest::gtest_main
    )
    add_test(NAME MarkerServiceTest COMMAND marker_service_test)

    add_executable(markers_runtime_host_test
        tests/markers_runtime_host_test.cpp
    )
    target_compile_options(markers_runtime_host_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(markers_runtime_host_test PRIVATE
        pj_runtime
        pj_scripting  # links the shared marker engine the service drives
        GTest::gtest_main
    )
    add_test(NAME MarkersRuntimeHostTest COMMAND markers_runtime_host_test)

    add_executable(data_processors_kind_router_test
        tests/data_processors_kind_router_test.cpp
    )
    target_compile_options(data_processors_kind_router_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(data_processors_kind_router_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME DataProcessorsKindRouterTest COMMAND data_processors_kind_router_test)

    add_executable(data_processor_service_transform_test
        tests/data_processor_service_transform_test.cpp
    )
    target_compile_options(data_processor_service_transform_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(data_processor_service_transform_test PRIVATE
        pj_runtime
        pj_scripting  # M4.2: plugin-created transforms compile a Luau script via the catalogue
        GTest::gtest_main
    )
    add_test(NAME DataProcessorTransformTest COMMAND data_processor_service_transform_test)

    add_executable(data_processors_runtime_host_test
        tests/data_processors_runtime_host_test.cpp
    )
    target_compile_options(data_processors_runtime_host_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(data_processors_runtime_host_test PRIVATE
        pj_runtime
        pj_scripting  # M4.2: the bridge compiles a Luau transform script through the service
        GTest::gtest_main
    )
    add_test(NAME DataProcessorsRuntimeHostTest COMMAND data_processors_runtime_host_test)

    # Pure header-only version math (UpdateVersion.h) — no networking, no lib.
    add_executable(update_version_test
        tests/update_version_test.cpp
    )
    target_compile_options(update_version_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(update_version_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME UpdateVersionTest COMMAND update_version_test)

    # Drives UpdateChecker's reply state machine against a local QTcpServer.
    # Qt6::Test supplies QSignalSpy; Qt6::Network comes transitively via pj_runtime.
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(update_checker_test
        tests/update_checker_test.cpp
    )
    set_target_properties(update_checker_test PROPERTIES AUTOMOC ON)
    target_compile_options(update_checker_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(update_checker_test PRIVATE
        pj_runtime
        Qt6::Test
        GTest::gtest
    )
    add_test(NAME UpdateCheckerTest COMMAND update_checker_test)

    # Drives TelemetryPing's POST + reply handling against a local capturing
    # QTcpServer — enforces the payload privacy contract on the wire (the
    # allowed-key set + salted-hash user id), no real network.
    add_executable(telemetry_ping_test
        tests/telemetry_ping_test.cpp
    )
    set_target_properties(telemetry_ping_test PROPERTIES AUTOMOC ON)
    target_compile_options(telemetry_ping_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(telemetry_ping_test PRIVATE
        pj_runtime
        Qt6::Test
        GTest::gtest
    )
    add_test(NAME TelemetryPingTest COMMAND telemetry_ping_test)

    add_executable(app_session_test
        tests/app_session_test.cpp
    )
    target_link_libraries(app_session_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME AppSessionTest COMMAND app_session_test)

    add_executable(playback_engine_test
        tests/playback_engine_test.cpp
    )
    target_link_libraries(playback_engine_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME PlaybackEngineTest COMMAND playback_engine_test)

    add_executable(session_manager_test
        tests/session_manager_test.cpp
    )
    # Qt6::Test supplies QSignalSpy for the displayOffsetChanged emission test.
    find_package(Qt6 REQUIRED COMPONENTS Test)
    target_link_libraries(session_manager_test PRIVATE
        pj_runtime
        Qt6::Test
        GTest::gtest_main
    )
    add_test(NAME SessionManagerTest COMMAND session_manager_test)

    add_executable(catalog_model_test
        tests/catalog_model_test.cpp
    )
    target_compile_options(catalog_model_test PRIVATE ${PJ_WARNING_FLAGS})
    # Qt6::Test supplies QSignalSpy for the advertised-topics idempotence test.
    target_link_libraries(catalog_model_test PRIVATE
        pj_runtime
        GTest::gtest_main
        Qt6::Test
    )
    add_test(NAME CatalogModelTest COMMAND catalog_model_test)

    add_executable(topic_demand_tracker_test
        tests/topic_demand_tracker_test.cpp
    )
    target_compile_options(topic_demand_tracker_test PRIVATE ${PJ_WARNING_FLAGS})
    # Qt6::Test supplies QSignalSpy for the activeTopicsChanged emission tests.
    target_link_libraries(topic_demand_tracker_test PRIVATE
        pj_runtime
        GTest::gtest_main
        Qt6::Test
    )
    add_test(NAME TopicDemandTrackerTest COMMAND topic_demand_tracker_test)

    add_executable(payload_anchor_test
        tests/payload_anchor_test.cpp
    )
    target_compile_options(payload_anchor_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(payload_anchor_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME PayloadAnchorTest COMMAND payload_anchor_test)

    add_executable(curve_color_registry_test
        tests/curve_color_registry_test.cpp
    )
    target_compile_options(curve_color_registry_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(curve_color_registry_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME CurveColorRegistryTest COMMAND curve_color_registry_test)

    add_executable(qsettings_backend_test
        tests/qsettings_backend_test.cpp
    )
    target_compile_options(qsettings_backend_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(qsettings_backend_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME QSettingsBackendTest COMMAND qsettings_backend_test)

    add_executable(toolbox_runtime_host_test
        tests/toolbox_runtime_host_test.cpp
    )
    target_compile_options(toolbox_runtime_host_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(toolbox_runtime_host_test PRIVATE
        pj_runtime
        GTest::gtest
    )
    add_test(NAME ToolboxRuntimeHostTest COMMAND toolbox_runtime_host_test)

    # Env-gated integration test against the REAL parser_ros plugin (SKIPs
    # unless PJ_REAL_ROS_PARSER_DIR points at the built parser_ros .so dir).
    add_executable(toolbox_parser_ingest_real_ros_test
        tests/toolbox_parser_ingest_real_ros_test.cpp
    )
    target_compile_options(toolbox_parser_ingest_real_ros_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(toolbox_parser_ingest_real_ros_test PRIVATE
        pj_runtime
        GTest::gtest
    )
    add_test(NAME ToolboxParserIngestRealRosTest COMMAND toolbox_parser_ingest_real_ros_test)

    add_library(runtime_host_object_parser_plugin SHARED
        tests/runtime_host_object_parser_plugin.cpp
    )
    target_compile_features(runtime_host_object_parser_plugin PRIVATE cxx_std_20)
    target_compile_options(runtime_host_object_parser_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(runtime_host_object_parser_plugin PRIVATE
        pj_plugin_sdk
    )

    # The parser-ingest tests in toolbox_runtime_host_test load the same stub
    # parser plugin the DataSource ingest test uses (defined just above, hence
    # the wiring lives here rather than next to the test target).
    target_compile_definitions(toolbox_runtime_host_test PRIVATE
        PJ_RUNTIME_HOST_OBJECT_PARSER_PATH="$<TARGET_FILE:runtime_host_object_parser_plugin>"
    )
    add_dependencies(toolbox_runtime_host_test runtime_host_object_parser_plugin)

    add_executable(data_source_runtime_host_object_ingest_test
        tests/data_source_runtime_host_object_ingest_test.cpp
    )
    target_compile_definitions(data_source_runtime_host_object_ingest_test PRIVATE
        PJ_RUNTIME_HOST_OBJECT_PARSER_PATH="$<TARGET_FILE:runtime_host_object_parser_plugin>"
    )
    target_compile_options(data_source_runtime_host_object_ingest_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(data_source_runtime_host_object_ingest_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_dependencies(data_source_runtime_host_object_ingest_test runtime_host_object_parser_plugin)
    add_test(NAME DataSourceRuntimeHostObjectIngestTest COMMAND data_source_runtime_host_object_ingest_test)

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
        pj_plugin_sdk
    )

    add_executable(data_source_runtime_host_stream_swap_test
        tests/data_source_runtime_host_stream_swap_test.cpp
    )
    target_compile_definitions(data_source_runtime_host_stream_swap_test PRIVATE
        PJ_STREAMING_CACHING_PARSER_PATH="$<TARGET_FILE:streaming_caching_parser_plugin>"
    )
    target_compile_options(data_source_runtime_host_stream_swap_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(data_source_runtime_host_stream_swap_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_dependencies(data_source_runtime_host_stream_swap_test streaming_caching_parser_plugin)
    add_test(NAME DataSourceRuntimeHostStreamSwapTest COMMAND data_source_runtime_host_stream_swap_test)

    # M5: notify_available_topics threading + a-priori classification fallback.
    add_executable(data_source_runtime_host_advertise_test
        tests/data_source_runtime_host_advertise_test.cpp
    )
    target_compile_options(data_source_runtime_host_advertise_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(data_source_runtime_host_advertise_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME DataSourceRuntimeHostAdvertiseTest COMMAND data_source_runtime_host_advertise_test)

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

    # PluginRuntimeCatalog — duplicate-resolution policy (version/compat/authoritative)
    # relocated from the SDK into the host. These mocks moved with the tests from the
    # SDK; mock_data_source_plugin (v1) is the SDK example fixture, reused from the
    # SDK subdir.
    add_library(mock_data_source_v2_plugin SHARED tests/mock_data_source_v2_plugin.cpp)
    target_compile_features(mock_data_source_v2_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_v2_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(mock_data_source_v2_plugin PRIVATE pj_base)

    # v1/v2 builds of a mock whose image stays resident after dlclose. Two
    # mechanisms: -z nodelete (DF_1_NODELETE, compiler-independent) and a bound
    # STB_GNU_UNIQUE anchor (models real GCC plugin builds; needs default
    # symbol visibility). Consumed by extension_manager_test's staged-promotion
    # regression coverage.
    add_library(mock_data_source_pinning_plugin SHARED tests/mock_data_source_pinning_plugin.cpp)
    target_compile_features(mock_data_source_pinning_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_pinning_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_compile_definitions(mock_data_source_pinning_plugin PRIVATE PIN_MOCK_VERSION="1.0.0")
    target_link_libraries(mock_data_source_pinning_plugin PRIVATE pj_base)

    add_library(mock_data_source_pinning_v2_plugin SHARED tests/mock_data_source_pinning_plugin.cpp)
    target_compile_features(mock_data_source_pinning_v2_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_pinning_v2_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_compile_definitions(mock_data_source_pinning_v2_plugin PRIVATE PIN_MOCK_VERSION="2.0.0")
    target_link_libraries(mock_data_source_pinning_v2_plugin PRIVATE pj_base)

    # -z is an ELF linker option; Mach-O has no equivalent spelling and Apple's ld
    # rejects it. dyld already declines to unload most images on dlclose(), so the
    # residency this mock models is the platform default on macOS.
    if(NOT WIN32 AND NOT APPLE)
      target_link_options(mock_data_source_pinning_plugin PRIVATE -Wl,-z,nodelete)
      target_link_options(mock_data_source_pinning_v2_plugin PRIVATE -Wl,-z,nodelete)
    endif()

    add_library(mock_data_source_incompatible_plugin SHARED tests/mock_data_source_incompatible_plugin.cpp)
    target_compile_features(mock_data_source_incompatible_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_incompatible_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(mock_data_source_incompatible_plugin PRIVATE pj_base)

    add_library(mock_data_source_incompatible_low_plugin SHARED tests/mock_data_source_incompatible_low_plugin.cpp)
    target_compile_features(mock_data_source_incompatible_low_plugin PRIVATE cxx_std_20)
    target_compile_options(mock_data_source_incompatible_low_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(mock_data_source_incompatible_low_plugin PRIVATE pj_base)

    add_executable(plugin_runtime_catalog_test tests/plugin_runtime_catalog_test.cpp)
    target_compile_definitions(plugin_runtime_catalog_test PRIVATE
        PJ_MOCK_DATA_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_plugin>"
        PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_v2_plugin>"
        PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_incompatible_plugin>"
        PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_LOW_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_incompatible_low_plugin>"
        PJ_MOCK_TOOLBOX_PLUGIN_PATH="$<TARGET_FILE:mock_toolbox_plugin>"
    )
    target_compile_options(plugin_runtime_catalog_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(plugin_runtime_catalog_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_dependencies(plugin_runtime_catalog_test
        mock_data_source_plugin mock_data_source_v2_plugin
        mock_data_source_incompatible_plugin mock_data_source_incompatible_low_plugin
        mock_toolbox_plugin)
    add_test(NAME plugin_runtime_catalog_test COMMAND plugin_runtime_catalog_test)

    # Bundled-plugin seeding (share -> marketplace sync) + scan-tier precedence.
    add_executable(extension_catalog_seed_test tests/extension_catalog_seed_test.cpp)
    target_compile_definitions(extension_catalog_seed_test PRIVATE
        PJ_MOCK_DATA_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_plugin>"
        PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_v2_plugin>"
    )
    target_compile_options(extension_catalog_seed_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(extension_catalog_seed_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_dependencies(extension_catalog_seed_test mock_data_source_plugin mock_data_source_v2_plugin)
    add_test(NAME extension_catalog_seed_test COMMAND extension_catalog_seed_test)
endif()
