# ---------------------------------------------------------------------------
# Standalone build support
#
# When this file is the root of the build (standalone), we set up the project
# and find all dependencies here. When used as a subdirectory of plotjuggler_sdk,
# the parent already provides CXX standard, warning flags, GTest, and testing.
# ---------------------------------------------------------------------------

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.22)
  project(pj_marketplace LANGUAGES CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  if(MSVC)
    set(PJ_WARNING_FLAGS /W4 /WX /permissive-)
  else()
    set(PJ_WARNING_FLAGS -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor
      -Wold-style-cast -Wcast-qual -Wconversion -Woverloaded-virtual -Wpedantic
    )
  endif()

  find_package(GTest REQUIRED)
  enable_testing()
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Test Concurrent)
find_package(LibArchive REQUIRED)

# ---------------------------------------------------------------------------
# pj_marketplace — package download, checksum verification and zip extraction
# ---------------------------------------------------------------------------

add_library(pj_marketplace STATIC
  src/core/DownloadManager.cpp
  src/core/archive_limits.cpp
  src/core/ArchiveExtractor.cpp
  src/core/ArtifactStore.cpp
  src/core/Durability.cpp
  src/core/StoreLease.cpp
  src/core/StoreSession.cpp
  src/core/ExtensionManager.cpp
  src/core/PlatformUtils.cpp
  src/core/PluginCheckRunner.cpp
  src/core/QtDiagnosticBridge.cpp
  src/core/RegistryResolver.cpp
  src/core/RegistryManager.cpp
  src/models/content_manifest.cpp
  src/models/generation_digest.cpp
  src/models/loaded_profile.cpp
  src/models/store_policy.cpp
  include/pj_marketplace/artifact_store.hpp
  include/pj_marketplace/content_manifest.hpp
  include/pj_marketplace/download_manager.hpp
  include/pj_marketplace/fs_walk.hpp
  include/pj_marketplace/generation_digest.hpp
  include/pj_marketplace/loaded_profile.hpp
  include/pj_marketplace/sha256.hpp
  include/pj_marketplace/store_lease.hpp
  include/pj_marketplace/store_policy.hpp
  include/pj_marketplace/store_rejection.hpp
  include/pj_marketplace/store_session.hpp
  include/pj_marketplace/stored_file.hpp
  include/pj_marketplace/extension_manager.hpp
  include/pj_marketplace/plugin_check_runner.hpp
  include/pj_marketplace/plugin_compatibility.hpp
  include/pj_marketplace/qt_diagnostic_bridge.hpp
  include/pj_marketplace/registry_resolver.hpp
  include/pj_marketplace/registry_manager.hpp
)

set_target_properties(pj_marketplace PROPERTIES
  AUTOMOC ON
  AUTOUIC ON
  AUTORCC ON
  AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
)

target_include_directories(pj_marketplace PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# src/ carries headers that are implementation detail of this library — internal/
# helpers a consumer has no business including. PRIVATE keeps them off the public
# interface while the sources here can still reach them by their own path.
target_include_directories(pj_marketplace PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pj_marketplace PUBLIC
  Qt6::Core
  Qt6::Network
  Qt6::Widgets
  Qt6::Concurrent
  LibArchive::LibArchive
)

add_library(pj_marketplace_ui STATIC
  src/ui/marketplace_window.cpp
  include/pj_marketplace/marketplace_window.hpp
)

set_target_properties(pj_marketplace_ui PROPERTIES
  AUTOMOC ON
  AUTOUIC ON
  AUTORCC ON
  AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
)

target_include_directories(pj_marketplace_ui PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pj_marketplace_ui PUBLIC
  pj_marketplace
  pj_widgets
  Qt6::Widgets
)

# Needed in both build modes: pj-plugin-check links nlohmann_json directly rather
# than picking it up through pj_base or pj_plugin_catalog, which it deliberately
# does not link so its packaging closure stays Qt-free and headless.
find_package(nlohmann_json REQUIRED)

# Standalone configure (this directory as the CMake root) resolves the SDK
# itself; in the PJ4 tree the root CMakeLists.txt already did.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  find_package(plotjuggler_sdk CONFIG REQUIRED)
endif()
target_link_libraries(pj_marketplace PUBLIC plotjuggler_sdk::plugin_host)

target_compile_options(pj_marketplace PRIVATE ${PJ_WARNING_FLAGS})

# ---------------------------------------------------------------------------
# pj_marketplace — main executable
# ---------------------------------------------------------------------------

add_executable(pj_marketplace_app
  main.cpp
)
set_target_properties(pj_marketplace_app PROPERTIES
  AUTOMOC ON
  AUTORCC ON
  AUTOUIC ON
  AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
)


target_link_libraries(pj_marketplace_app PRIVATE
  pj_marketplace
  pj_marketplace_ui
)

target_compile_options(pj_marketplace_app PRIVATE ${PJ_WARNING_FLAGS})

# ---------------------------------------------------------------------------
# pj-plugin-check — Qt-free admission helper
#
# One-shot child process invoked by PluginCheckRunner. Loading the plugin DSO
# happens INSIDE this process, so a plugin that crashes on dlopen or during
# manifest read never touches the caller's process image. Stays Qt-free so the
# crash surface it exposes is the plugin's own and nothing else.
# ---------------------------------------------------------------------------
add_executable(pj-plugin-check
  helper/main.cpp
)
target_link_libraries(pj-plugin-check PRIVATE
  plotjuggler_sdk::plugin_host
  nlohmann_json::nlohmann_json
)
target_compile_options(pj-plugin-check PRIVATE ${PJ_WARNING_FLAGS})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)

# download_manager_test has its own main() to initialise QCoreApplication
add_executable(download_manager_test tests/download_manager_test.cpp)
target_link_libraries(download_manager_test PRIVATE pj_marketplace GTest::gtest Qt6::Test LibArchive::LibArchive)
target_compile_options(download_manager_test PRIVATE ${PJ_WARNING_FLAGS})
set_target_properties(download_manager_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests
)
add_test(NAME download_manager_test COMMAND download_manager_test)

# plugin_check_runner_test — exercises the real pj-plugin-check helper in a
# child process. Its path is injected at compile time so the runner-under-test
# uses the freshly-built binary, not a system-installed one.
add_executable(plugin_check_runner_test tests/plugin_check_runner_test.cpp)
target_link_libraries(plugin_check_runner_test PRIVATE pj_marketplace GTest::gtest Qt6::Core)
target_compile_options(plugin_check_runner_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(plugin_check_runner_test PRIVATE
  PJ_PLUGIN_CHECK_HELPER_PATH="$<TARGET_FILE:pj-plugin-check>"
)
add_dependencies(plugin_check_runner_test pj-plugin-check)
set_target_properties(plugin_check_runner_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests
)
add_test(NAME plugin_check_runner_test COMMAND plugin_check_runner_test)

# content_manifest_test — the pure identity functions. No Qt, no network: they
# take a directory and return a digest, so they are exercised directly instead of
# through the install pipeline.
add_executable(content_manifest_test tests/content_manifest_test.cpp)
target_link_libraries(content_manifest_test PRIVATE pj_marketplace GTest::gtest GTest::gtest_main)
target_compile_options(content_manifest_test PRIVATE ${PJ_WARNING_FLAGS})
set_target_properties(content_manifest_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests
)
add_test(NAME content_manifest_test COMMAND content_manifest_test)

# artifact_store_test — the content-addressed store primitives. Builds its ZIP
# fixtures with libarchive directly, so it links it in addition to the library.
add_executable(artifact_store_test tests/artifact_store_test.cpp)
target_link_libraries(artifact_store_test PRIVATE pj_marketplace GTest::gtest GTest::gtest_main LibArchive::LibArchive)
target_compile_options(artifact_store_test PRIVATE ${PJ_WARNING_FLAGS})
set_target_properties(artifact_store_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests
)
add_test(NAME artifact_store_test COMMAND artifact_store_test)

add_executable(registry_manager_test
  tests/registry_manager_test.cpp
)

target_link_libraries(registry_manager_test PRIVATE
  pj_marketplace
  Qt6::Network
  Qt6::Test
  GTest::gtest
)
set_target_properties(registry_manager_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests
)
target_compile_options(registry_manager_test PRIVATE ${PJ_WARNING_FLAGS})
add_test(NAME registry_manager_test COMMAND registry_manager_test)

add_executable(registry_resolver_test tests/registry_resolver_test.cpp)
target_link_libraries(registry_resolver_test PRIVATE pj_marketplace GTest::gtest_main)
target_compile_options(registry_resolver_test PRIVATE ${PJ_WARNING_FLAGS})
set_target_properties(registry_resolver_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests
)
add_test(NAME registry_resolver_test COMMAND registry_resolver_test)

add_executable(version_compare_test tests/version_compare_test.cpp)
target_link_libraries(version_compare_test PRIVATE pj_marketplace GTest::gtest_main)
target_compile_options(version_compare_test PRIVATE ${PJ_WARNING_FLAGS})
set_target_properties(version_compare_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests
)
add_test(NAME version_compare_test COMMAND version_compare_test)

if(TARGET mock_data_source_plugin AND TARGET mock_file_source_plugin
   AND TARGET mock_data_source_v2_plugin AND TARGET missing_id_data_source_plugin
   AND TARGET mock_data_source_pinning_plugin AND TARGET mock_data_source_pinning_v2_plugin
   AND TARGET mock_data_source_incompatible_plugin AND TARGET mock_data_source_compatible_floor_plugin
   AND TARGET mock_data_source_malformed_floor_plugin AND TARGET mock_data_source_malformed_version_plugin
   AND TARGET mock_data_source_newer_sdk_plugin)
  add_executable(extension_manager_test
    tests/extension_manager_test.cpp
  )
  target_compile_definitions(extension_manager_test PRIVATE
    PJ_MARKETPLACE_TESTING
    PJ_MOCK_DATA_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_plugin>"
    PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_v2_plugin>"
    PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>"
    PJ_MISSING_ID_PLUGIN_PATH="$<TARGET_FILE:missing_id_data_source_plugin>"
    PJ_MOCK_PINNING_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_pinning_plugin>"
    PJ_MOCK_PINNING_V2_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_pinning_v2_plugin>"
    # Declares min_plotjuggler_version 5.0.0 — the only fixture with a host-version
    # floor, so it is what the local-ZIP compatibility gate is tested against.
    PJ_MOCK_INCOMPATIBLE_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_incompatible_plugin>"
    PJ_MOCK_COMPATIBLE_FLOOR_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_compatible_floor_plugin>"
    PJ_MOCK_MALFORMED_FLOOR_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_malformed_floor_plugin>"
    PJ_MOCK_MALFORMED_VERSION_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_malformed_version_plugin>"
    PJ_MOCK_NEWER_SDK_PLUGIN_PATH="$<TARGET_FILE:mock_data_source_newer_sdk_plugin>"
  )
  add_dependencies(extension_manager_test mock_data_source_plugin mock_file_source_plugin
                   mock_data_source_v2_plugin missing_id_data_source_plugin
                   mock_data_source_pinning_plugin mock_data_source_pinning_v2_plugin
                   mock_data_source_incompatible_plugin mock_data_source_compatible_floor_plugin
                   mock_data_source_malformed_floor_plugin mock_data_source_malformed_version_plugin
                   mock_data_source_newer_sdk_plugin)

  target_link_libraries(extension_manager_test PRIVATE
    pj_marketplace
    Qt6::Network
    Qt6::Test
    GTest::gtest
    LibArchive::LibArchive
  )
  set_target_properties(extension_manager_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests
  )
  target_compile_options(extension_manager_test PRIVATE ${PJ_WARNING_FLAGS})
  # The admission path exercised here starts pj-plugin-check, which the runner
  # locates next to the running binary (QCoreApplication::applicationDirPath()).
  # Place the freshly-built helper beside the test so installs are admitted
  # instead of failing with execve: No such file or directory.
  add_dependencies(extension_manager_test pj-plugin-check)
  add_custom_command(TARGET extension_manager_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:pj-plugin-check> $<TARGET_FILE_DIR:extension_manager_test>
    COMMENT "Placing pj-plugin-check next to extension_manager_test")
  add_test(NAME extension_manager_test COMMAND extension_manager_test)
else()
  # There is no legitimate configuration where tests are wanted but these fixtures
  # are absent, so silence here means a real problem — most likely that a top-level
  # add_subdirectory reorder moved pj_marketplace ahead of the module defining one
  # of them. Without this the whole suite drops out of ctest and everything still
  # reports green, which is exactly how it went unnoticed once already.
  message(WARNING "extension_manager_test skipped: mock plugin fixtures are not configured yet")
endif()

# Integration test — requires network access, excluded from CTest by default.
# Run manually: ./tests/extension_manager_integration_test
add_executable(extension_manager_check_plugin_management
  tests/extension_manager_check_plugin_management.cpp
)

target_link_libraries(extension_manager_check_plugin_management PRIVATE
  pj_marketplace
  Qt6::Network
  Qt6::Test
  GTest::gtest
)
set_target_properties(extension_manager_check_plugin_management PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests
)
target_compile_definitions(extension_manager_check_plugin_management PRIVATE
  RESULTS_DIR="${CMAKE_CURRENT_BINARY_DIR}/tests/results"
)
target_compile_options(extension_manager_check_plugin_management PRIVATE ${PJ_WARNING_FLAGS})

endif() # PJ_BUILD_TESTS
