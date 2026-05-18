find_package(Arrow QUIET CONFIG)
if(NOT TARGET ArrowFlight::arrow_flight_static)
    find_package(ArrowFlight QUIET CONFIG)
endif()

# Plugins always link Arrow/Flight statically (see CLAUDE / project policy).
set(MOSAICO_ARROW_FLIGHT_TARGET ArrowFlight::arrow_flight_static)
set(MOSAICO_ARROW_TARGET Arrow::arrow_static)

if(NOT Arrow_FOUND OR NOT TARGET ${MOSAICO_ARROW_FLIGHT_TARGET} OR NOT TARGET ${MOSAICO_ARROW_TARGET})
    message("[ToolboxMosaico] Arrow/Flight static targets not found. Skipping plugin.")
    return()
endif()

# Static Arrow Flight embeds Flight's object code into the plugin .so but
# does NOT add libgrpc++ as a NEEDED entry — static archives don't carry
# NEEDED records and Arrow's CMake target doesn't expose gRPC as a usage
# requirement. At dlopen time the dynamic linker can't resolve gRPC symbols
# (e.g. grpc::internal::ClientReactor's vtable) and PlotJuggler fails the
# plugin load with: undefined symbol: _ZN4grpc6g_glipE.
# Find gRPC++ explicitly so its symbols are resolvable — either as a
# libgrpc++.so NEEDED entry (apt / AppImage) or pulled in from a static
# gRPC archive (Conan).
set(MOSAICO_GRPCPP_TARGET "")
find_package(gRPC QUIET CONFIG)
if(TARGET gRPC::grpc++)
    set(MOSAICO_GRPCPP_TARGET gRPC::grpc++)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MOSAICO_GRPCPP QUIET IMPORTED_TARGET grpc++)
        if(TARGET PkgConfig::MOSAICO_GRPCPP)
            set(MOSAICO_GRPCPP_TARGET PkgConfig::MOSAICO_GRPCPP)
        endif()
    endif()
endif()
if(NOT MOSAICO_GRPCPP_TARGET)
    message("[ToolboxMosaico] gRPC++ not found — install libgrpc++-dev or "
            "use a Conan profile that provides it. Skipping plugin.")
    return()
endif()

# Combine Arrow + Flight as a single propagated target for non-Conan builds.
# (Conan's CMakeDeps already creates a lowercase `arrow::arrow` component
# target, so this branch is skipped under Conan and the static-mode defines
# are added directly to mosaico_sdk in its CMakeLists instead.)
if(NOT TARGET arrow::arrow)
    add_library(arrow_combined INTERFACE)
    target_link_libraries(arrow_combined INTERFACE
        ${MOSAICO_ARROW_TARGET}
        ${MOSAICO_ARROW_FLIGHT_TARGET})
    target_compile_definitions(arrow_combined INTERFACE
        ARROW_STATIC
        ARROW_FLIGHT_STATIC)
    add_library(arrow::arrow ALIAS arrow_combined)
endif()

# Vendored Mosaico SDK. Lives at 3rdparty/mosaico_sdk/.
# Built as a static library and linked into the plugin.
set(BUILD_TESTING_SAVED ${BUILD_TESTING})
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/mosaico_sdk EXCLUDE_FROM_ALL)
set(BUILD_TESTING ${BUILD_TESTING_SAVED} CACHE BOOL "" FORCE)

include_directories(../)

set(MOSAICO_SOURCES
    src/core/time_format.cpp
    src/ui/download_stats_dialog.cpp
    src/ui/main_window.cpp
    src/ui/fetch_worker.cpp
    src/ui/server_history.cpp
    src/ui/sequence/sequence_panel.cpp
    src/ui/topic/topic_panel.cpp
    src/ui/metadata/data_view_panel.cpp
    src/ui/picker/calendar_widget.cpp
    src/ui/picker/dual_calendar_widget.cpp
    src/ui/picker/time_picker_widget.cpp
    src/ui/picker/sequence_picker_widget.cpp
    src/ui/metadata/query_bar.cpp
    src/ui/metadata/completer.cpp
    src/ui/security/cert_dialog.cpp
    toolbox_mosaico.cpp
)

qt5_add_resources(MOSAICO_RESOURCES resources/mosaico_resources.qrc)

add_library(ToolboxMosaico SHARED ${MOSAICO_SOURCES} ${MOSAICO_RESOURCES})

target_include_directories(ToolboxMosaico PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/query
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
)

target_link_libraries(ToolboxMosaico PRIVATE
    Qt5::Widgets
    Qt5::Xml
    plotjuggler_base
    mosaico_sdk
    ${MOSAICO_ARROW_FLIGHT_TARGET}
    ${MOSAICO_GRPCPP_TARGET}
    lua::lua
    QCodeEditor
)

target_compile_definitions(ToolboxMosaico PRIVATE QT_PLUGIN)

install(TARGETS ToolboxMosaico DESTINATION ${PJ_PLUGIN_INSTALL_DIRECTORY})

# Tests (optional — only built when BUILD_TESTING is ON)
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
