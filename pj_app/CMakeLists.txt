find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Svg Test)
find_package(nlohmann_json REQUIRED)

# backward-cpp: symbolized crash backtraces (main.cpp installs the handler). Backward::Backward carries the
# BACKWARD_HAS_* defines and the libdw backend transitively.
if(NOT EMSCRIPTEN)
    find_package(Backward REQUIRED)
endif()

# The shell as an OBJECT library so tests link the same MainWindow objects; the executable wraps main.cpp.
add_library(pj_app_shell OBJECT
    src/BuiltinPlugins.cpp
    src/BuiltinPlugins.h
    src/Splashscreen.cpp
    src/Splashscreen.h
    src/DialogPresenter.cpp
    src/DialogPresenter.h
    src/DialogWatch.h
    src/FanoutConfig.cpp
    src/FanoutConfig.h
    src/BrowserFileStore.cpp
    src/BrowserFileStore.h
    src/BrowserPersistence.cpp
    src/BrowserPersistence.h
    src/FileSelectionService.cpp
    src/FileSelectionService.h
    src/LoadInput.h
    src/FileLoader.cpp
    src/FileLoader.h
    src/SourcePromotionHost.cpp
    src/SourcePromotionHost.h
    src/PresetController.cpp
    src/PresetController.h
    src/WelcomePresetsDialog.cpp
    src/WelcomePresetsDialog.h
    src/WelcomePresetsDialog.ui
    src/HeadlessDescriptorProviderSession.cpp
    src/HeadlessDescriptorProviderSession.h
    src/ToolboxHostWiring.h
    src/ToolboxChrome.cpp
    src/ToolboxChrome.h
    src/ToolboxPresenter.cpp
    src/ToolboxPresenter.h
    src/PendingDisplayBinder.cpp
    src/PendingDisplayBinder.h
    src/TopicDemandController.cpp
    src/TopicDemandController.h
    src/StreamingSourceManager.cpp
    src/StreamingSourceManager.h
    src/PluginPlotTabsController.cpp
    src/PluginPlotTabsController.h
    src/PluginSceneViewsController.cpp
    src/PluginSceneViewsController.h
    src/SourceTimelineController.cpp
    src/SourceTimelineController.h
    src/DatasetMergeActions.cpp
    src/DatasetMergeActions.h
    src/DiagnosticDump.cpp
    src/DiagnosticDump.h
    src/LayoutExitWatch.cpp
    src/LayoutExitWatch.h
    src/LayoutXml.cpp
    src/LayoutXml.h
    src/LayoutImportBatch.cpp
    src/LayoutImportBatch.h
    src/IngestProgressController.cpp
    src/IngestProgressController.h
    src/SeriesProgressController.cpp
    src/SeriesProgressController.h
    src/IngestProgressWiring.cpp
    src/IngestProgressWiring.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/MainWindow.ui
    src/ToolboxBanner.ui
    src/scene_object_classification.cpp
    src/scene_object_classification.h
    src/WidgetTuner.cpp
    src/WidgetTuner.h
    src/KeySequence.cpp
    src/KeySequence.h
    src/DebugMode.cpp
    src/DebugMode.h
    src/PreferencesDialog.cpp
    src/PreferencesDialog.h
    src/PreferencesDialog.ui
    src/PreferencesNavRow.cpp
    src/PreferencesNavRow.h
    src/Theme.cpp
    src/Theme.h
    src/DebugUi.cpp
    src/DebugUi.h
    src/TitleBar.cpp
    src/TitleBar.h
    src/TitleBar.ui
    src/ui/CurveListPanel.cpp
    src/ui/CurveListPanel.h
    src/ui/CurveListPanel.ui
    src/CustomWidgetController.cpp
    src/CustomWidgetController.h
    src/ui/CustomWidgetConfigPanel.cpp
    src/ui/CustomWidgetConfigPanel.h
    src/ui/CustomWidgetConfigPanel.ui
    src/ui/CustomWidgetPickerMenu.cpp
    src/ui/CustomWidgetPickerMenu.h
    src/ui/LeftPanel.cpp
    src/ui/LeftPanel.h
    src/ui/LeftPanel.ui
    src/ui/DatasetInfoDialog.cpp
    src/ui/DatasetInfoDialog.h
    src/ui/DatasetInfoDialog.ui
    src/ui/DiagnosticsDialog.cpp
    src/ui/DiagnosticsDialog.h
    src/ui/DiagnosticsDialog.ui
    src/ui/AboutDialog.cpp
    src/ui/AboutDialog.h
    src/ui/AboutDialog.ui
    src/ui/BatchLoadDialog.cpp
    src/ui/BatchLoadDialog.h
    src/ui/BatchLoadDialog.ui
    src/ui/LayoutDatasetDialog.cpp
    src/ui/LayoutDatasetDialog.h
    src/ui/LayoutDatasetDialog.ui
    src/ui/McpJobsDialog.cpp
    src/ui/McpJobsDialog.h
    src/ui/McpJobsDialog.ui
    src/ui/TimelineWidget.cpp
    src/ui/TimelineWidget.h
    src/ui/TimelineWidget.ui
    src/RasterKeyMap.cpp
    src/RasterKeyMap.h
)

if(TARGET pj_scene2d_widgets)
    target_sources(pj_app_shell PRIVATE
        src/SceneImageDecoder.cpp
        src/SceneImageDecoder.h
        src/SceneVideoFrameSource.cpp
        src/SceneVideoFrameSource.h
        src/ui/Scene2DConfigPanel.cpp
        src/ui/Scene2DConfigPanel.h)
endif()
if(TARGET pj_scene3d_widgets)
    target_sources(pj_app_shell PRIVATE
        src/ui/Scene3DConfigPanel.cpp
        src/ui/Scene3DConfigPanel.h)
endif()
# The MCP server (docs/MCP_SERVER.md). The browser build cannot listen, so there it answers only
# what its relay link brings in (McpBrowserLink); the stdio bridge needs a process, so it is desktop
# only. The agent tools are pj_runtime's pj_agent_tools.
target_sources(pj_app_shell PRIVATE
    src/mcp/McpBrowserLink.cpp
    src/mcp/McpBrowserLink.h
    src/mcp/McpProtocol.cpp
    src/mcp/McpProtocol.h
    src/mcp/McpServer.cpp
    src/mcp/McpServer.h
    src/mcp/McpServerTools.cpp)
if(NOT EMSCRIPTEN)
    # The UI tools are desktop only: only a command-line flag turns them on. Qt6::GuiPrivate:
    # <qpa/qwindowsysteminterface.h>, their real input path.
    target_sources(pj_app_shell PRIVATE src/mcp/McpBridge.cpp src/mcp/McpUiTools.cpp src/mcp/McpUiTools.h)
    target_link_libraries(pj_app_shell PRIVATE Qt6::GuiPrivate)
endif()
target_link_libraries(pj_app_shell PUBLIC pj_agent_tools)

if(EMSCRIPTEN)
    # Lazily read browser picks; the page side is browser_picked_file.js, linked into pj_app below.
    target_sources(pj_app_shell PRIVATE
        src/BrowserPickedFile.cpp
        src/BrowserPickedFile.h)
    # DiagnosticHistory consumes this QObject bridge, normally supplied by the
    # marketplace library that is intentionally absent in a static web build.
    target_sources(pj_app_shell PRIVATE
        ${CMAKE_SOURCE_DIR}/pj_marketplace/src/core/QtDiagnosticBridge.cpp
        ${CMAKE_SOURCE_DIR}/pj_marketplace/include/pj_marketplace/qt_diagnostic_bridge.hpp)
    target_include_directories(pj_app_shell PRIVATE
        ${CMAKE_SOURCE_DIR}/pj_marketplace/include)

    if(PJ_WASM_ENABLE_INGRESS_PROBE)
        target_sources(pj_app_shell PRIVATE
            tests/wasm_acceptance_probes.inc
            tests/wasm_ingress_probe_source.cpp)
        if(TARGET pj_scene3d_widgets)
            target_sources(pj_app_shell PRIVATE
                tests/wasm_scene3d_acceptance_probes.cpp
                tests/wasm_scene3d_acceptance_probes.h)
            target_link_libraries(pj_app_shell PRIVATE pj_scene3d_widgets_testing)
        endif()
        if(PJ_WASM_WITH_MCAP AND NOT PJ_WASM_WITH_ROS)
            target_sources(pj_app_shell PRIVATE tests/wasm_mcap_probe_parser.cpp)
            target_compile_definitions(pj_app_shell PRIVATE PJ_WASM_ENABLE_MCAP_PROBE_PARSER)
        endif()
        target_compile_definitions(pj_app_shell PRIVATE PJ_WASM_ENABLE_INGRESS_PROBE)
        target_link_libraries(pj_app_shell PUBLIC plotjuggler_sdk::plugin_sdk)
    endif()
endif()

# LayoutXml.cpp is also compiled directly into focused test executables. Its
# plot-range vocabulary is a header-only contract owned by pj_plotting_core, so
# expose that one include tree without linking those DOM-only tests to widgets.
set_property(SOURCE src/LayoutXml.cpp APPEND PROPERTY
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/pj_plotting/core/include
)

set_target_properties(pj_app_shell PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(pj_app_shell PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/raster/ipc
)

target_link_libraries(pj_app_shell PUBLIC
    pj_runtime
    pj_widgets
    pj_plotting
    pj_custom_widgets
    pj_log
    pj_scripting
    pj_scene_common
    $<$<TARGET_EXISTS:pj_scene2d_widgets>:pj_scene2d_widgets>
    $<$<TARGET_EXISTS:pj_scene3d_widgets>:pj_scene3d_widgets>
    $<$<TARGET_EXISTS:pj_marketplace_ui>:pj_marketplace_ui>
    pj_dialog_engine_qt
    qt_advanced_docking
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Xml
    Qt6::Svg
)

if(EMSCRIPTEN)
    # The official plugins built into the module (cmake/PjWasmOfficialPlugins.cmake), registered
    # by BuiltinPlugins.cpp through the generated include.
    pj_wasm_register_official_plugins(pj_app_shell "${CMAKE_BINARY_DIR}/generated/pj_wasm_official_plugins.inc")
    qt_add_executable(pj_app
        src/main.cpp
        src/BundledFont.cpp
        ${CMAKE_SOURCE_DIR}/resources/resources.qrc)
else()
    add_executable(pj_app src/main.cpp src/BundledFont.cpp)
endif()
set_target_properties(pj_app PROPERTIES
    # The user-facing command packaging ships; the target id stays pj_app.
    OUTPUT_NAME plotjuggler4
)
if(WIN32)
    # GUI subsystem: no console window next to the app when it starts from the
    # Start menu or Explorer. main() stays the entry point, and main.cpp attaches
    # to the parent console when the app is started from a terminal. PowerShell's
    # `&` does not wait for a GUI-subsystem exe (scripts/windows/run.ps1 pipes it).
    set_target_properties(pj_app PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(pj_app PRIVATE /ENTRY:mainCRTStartup)
    endif()
    # Explorer, the installer's shortcuts and the taskbar read the icon and the
    # version details from the exe's own resources: the Qt window icon
    # (resources.qrc) never reaches them.
    set(PJ_APP_ICON "${CMAKE_SOURCE_DIR}/resources/plotjuggler.ico")
    string(REPLACE "." ";" _pj_rc_parts "${PJ_PROJECT_VERSION}.0.0.0")
    list(SUBLIST _pj_rc_parts 0 4 _pj_rc_parts)
    list(JOIN _pj_rc_parts "," PJ_RC_VERSION)
    configure_file(plotjuggler.rc.in ${CMAKE_CURRENT_BINARY_DIR}/plotjuggler.rc @ONLY)
    target_sources(pj_app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/plotjuggler.rc)
    # The build tracks the .rc's #includes, not the icon file it embeds.
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/plotjuggler.rc PROPERTIES OBJECT_DEPENDS "${PJ_APP_ICON}")
endif()
target_include_directories(pj_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pj_app PRIVATE
    pj_app_shell
    $<$<NOT:$<BOOL:${EMSCRIPTEN}>>:pj_resources>
    $<$<NOT:$<BOOL:${EMSCRIPTEN}>>:Backward::Backward>)
# Sanitizer lanes keep the sanitizer's own report. backward-cpp's handler would replace it, and when LeakSanitizer
# aborts at exit the handler runs on the signal stack its own static destructor already freed.
if(PJ_ENABLE_SANITIZERS OR PJ_ENABLE_TSAN OR PJ_ENABLE_MSAN)
    target_compile_definitions(pj_app PRIVATE PJ_NO_CRASH_HANDLER)
endif()

# PluginCheckRunner::locateHelper looks for the admission helper next to the executable. An install puts both
# in bin/; a build tree does not, so copy it next to the app (else only PATH finds it). Done from the consumer
# so the standalone marketplace build, which has no app target, stays untouched.
if(TARGET pj-plugin-check)
    add_dependencies(pj_app pj-plugin-check)
    add_custom_command(TARGET pj_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:pj-plugin-check> $<TARGET_FILE_DIR:pj_app>
        COMMENT "Placing pj-plugin-check next to the application binary")
    install(TARGETS pj-plugin-check RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# `cmake --install`: the app and its admission helper in bin/, on Linux the desktop entry and icons. The bundled
# plugins folder ExtensionCatalogService seeds from (<bindir>/../lib/plotjuggler/plugins, macOS
# <bindir>/../Resources/plugins) stays empty; bundled plugins, CPython and Qt's files belong to packaging/.
if(NOT EMSCRIPTEN)
    install(TARGETS pj_app RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
    if(UNIX AND NOT APPLE)
        install(FILES ${CMAKE_SOURCE_DIR}/packaging/appimage/plotjuggler4.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(FILES ${CMAKE_SOURCE_DIR}/resources/svg/plotjuggler4.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
        install(FILES ${CMAKE_SOURCE_DIR}/resources/svg/plotjuggler.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
            RENAME plotjuggler4.svg)
    endif()
endif()

# PJ_VERSION_STRING comes from generated/pj_version.h, which main.cpp includes: a -D here would redefine it
# under -Werror.

# -Werror: a merge that drops wiring leaves an unused variable behind; this makes it a build failure.
if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_app_shell PRIVATE ${PJ_WARNING_FLAGS})
    # CXX only: pj_app also compiles plotjuggler.rc on Windows, and rc.exe rejects
    # MSVC warning options such as /W4.
    target_compile_options(pj_app PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${PJ_WARNING_FLAGS}>")
endif()
# MainWindow.cpp outgrows COFF's default section limit (C1128 in the pixi win-64 build).
if(MSVC)
    target_compile_options(pj_app_shell PRIVATE /bigobj)
endif()

# Keep static-archive symbols (Conan libpng, libjpeg, ...) out of the dynamic symbol table: exported, they
# interpose ahead of the system copies GTK loads, and a libpng ABI skew crashes QFileDialog. GNU/LLD spelling.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE AND NOT WIN32 AND NOT EMSCRIPTEN)
    target_link_options(pj_app PRIVATE "LINKER:--exclude-libs,ALL")
endif()

if(EMSCRIPTEN)
    set_target_properties(pj_app PROPERTIES
        AUTORCC ON
        QT_WASM_INITIAL_MEMORY "256MB")
    target_link_options(pj_app PRIVATE -sALLOW_MEMORY_GROWTH=1
        "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/src/browser_picked_file.js")
    set_property(TARGET pj_app APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/browser_picked_file.js")
    # Link-time patch of two Qt 6.12.0 backing-store flush functions: without it, playback at HiDPI uploads
    # nearly the whole window as a texture every frame. See the file's header; remove it once Qt has the fix.
    option(PJ_WASM_QT_FLUSH_OVERRIDE "Link-time patch of Qt 6.12 paintAndFlush/toTexture (HiDPI playback)" ON)
    if(PJ_WASM_QT_FLUSH_OVERRIDE)
        find_package(Qt6 REQUIRED COMPONENTS GuiPrivate WidgetsPrivate)
        target_sources(pj_app PRIVATE src/wasm/qt612_rhi_flush_override.cpp)
        # Verbatim Qt code: keep the project's -Werror set off it.
        set_source_files_properties(src/wasm/qt612_rhi_flush_override.cpp PROPERTIES COMPILE_OPTIONS "-w")
        target_link_libraries(pj_app PRIVATE Qt6::GuiPrivate Qt6::WidgetsPrivate)
        # ponytail: applies to the whole pj_app link, so it also silences any other duplicate symbol;
        # it goes away with this file.
        target_link_options(pj_app PRIVATE "LINKER:--allow-multiple-definition")
    endif()
    if(PJ_WASM_WITH_PYTHON)
        # Three more pre-spawned workers: the Python helper, its watchdog and a script lane. A worker created while
        # the main thread waits on it never starts.
        set_target_properties(pj_app PROPERTIES QT_WASM_PTHREAD_POOL_SIZE 7)
        # The stdlib, beside the module under the name the first Python use downloads (python_sandbox.cpp) and
        # scripts/package_wasm.mjs packages: not in the startup download.
        add_custom_command(TARGET pj_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PJ_WASM_PYTHON_STDLIB}"
                "$<TARGET_FILE_DIR:pj_app>/python_stdlib.zip"
            VERBATIM)
        target_compile_definitions(pj_app_shell PRIVATE PJ_WASM_WITH_PYTHON)
        if(PJ_WASM_ENABLE_INGRESS_PROBE)
            # The stdlib probe runs Python outside the sandbox; the symbols come through pj_scripting's link.
            target_sources(pj_app_shell PRIVATE tests/wasm_python_stdlib_probe.cpp)
            target_include_directories(pj_app_shell SYSTEM PRIVATE
                $<TARGET_PROPERTY:cpython::embed,INTERFACE_INCLUDE_DIRECTORIES>)
        endif()
    endif()
    # Next to plotjuggler4.js, where scripts/package_wasm.mjs packages it with the module.
    include(${CMAKE_SOURCE_DIR}/cmake/PjWasmNotices.cmake)
    pj_wasm_write_notices("${CMAKE_CURRENT_BINARY_DIR}/THIRD_PARTY_NOTICES.txt" "${PJ_WASM_PYTHON_NOTICES}")
endif()

if(PJ_BUILD_TESTS)
    find_package(GTest REQUIRED)

    # Qt-only helpers, compiled without the shell's dependency closure. No WARNINGS: not every test here builds
    # under -Werror.
    pj_add_test_runner(pj_app_unit_tests
        MAIN gui
        SOURCES
            tests/browser_file_store_test.cpp
            tests/bundled_font_test.cpp
            tests/browser_persistence_test.cpp
            tests/file_selection_service_test.cpp
            tests/fanout_config_test.cpp
            tests/layout_xml_test.cpp
            tests/key_sequence_matcher_test.cpp
            tests/key_sequence_watcher_test.cpp
            tests/raster_ipc_protocol_test.cpp
            tests/raster_key_map_test.cpp
            src/BrowserFileStore.cpp
            src/BrowserFileStore.h
            src/BundledFont.cpp
            src/LoadInput.h
            src/BrowserPersistence.cpp
            src/BrowserPersistence.h
            src/FileSelectionService.cpp
            src/FileSelectionService.h
            src/FanoutConfig.cpp
            src/LayoutXml.cpp
            src/LayoutXml.h
            src/KeySequence.cpp
            src/RasterKeyMap.cpp
        LIBS Qt6::Widgets Qt6::Xml
    )
    set_target_properties(pj_app_unit_tests PROPERTIES AUTOMOC ON)
    target_compile_definitions(pj_app_unit_tests PRIVATE
        PJ_BUNDLED_FONT_DIR="${CMAKE_SOURCE_DIR}/resources/fonts/PlotJugglerSans")
    # The MCP server's HTTP and JSON-RPC gates (desktop only, like the server).
    if(NOT EMSCRIPTEN)
        target_sources(pj_app_unit_tests PRIVATE tests/mcp_protocol_test.cpp src/mcp/McpProtocol.cpp)
        target_link_libraries(pj_app_unit_tests PRIVATE pj_agent_tools)
    endif()
    # pj_runtime's include tree only for the header-only XmlCdata.h and
    # SavedSeriesPath.h LayoutXml uses; the runner does not link pj_runtime.
    target_include_directories(pj_app_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/raster/ipc
        ${CMAKE_SOURCE_DIR}/pj_runtime/include
    )

    # The codecs on-demand scripts decode through, compiled standalone over Scene2D's core; turbojpeg and libpng
    # encode the image fixtures, FFmpeg demuxes the video one (run from the source root, where it lives).
    if(TARGET pj_scene2d_widgets AND NOT EMSCRIPTEN)
        find_package(libjpeg-turbo REQUIRED)
        find_package(PNG REQUIRED)
        include(${CMAKE_SOURCE_DIR}/pj_scene2D/cmake/PjScene2DFfmpeg.cmake)
        pj_scene2d_find_ffmpeg(PJ_APP_HAS_FFMPEG PJ_APP_FFMPEG_TARGETS PJ_APP_FFMPEG_SOURCE)
        set(PJ_APP_MEDIA_TEST_SOURCES
            tests/scene_image_decoder_test.cpp src/SceneImageDecoder.cpp src/SceneImageDecoder.h
            src/SceneVideoFrameSource.cpp src/SceneVideoFrameSource.h)
        if(PJ_APP_HAS_FFMPEG)
            list(APPEND PJ_APP_MEDIA_TEST_SOURCES tests/scene_video_frame_source_test.cpp)
        endif()
        pj_add_test_runner(pj_app_media_codec_tests
            MAIN plain WARNINGS
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            SOURCES ${PJ_APP_MEDIA_TEST_SOURCES}
            LIBS pj_scene2d_core pj_scripting libjpeg-turbo::libjpeg-turbo PNG::PNG ${PJ_APP_FFMPEG_TARGETS})
        target_include_directories(pj_app_media_codec_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/pj_scene2D/tests)
        if(MSVC)
            target_compile_options(pj_app_media_codec_tests PRIVATE /wd4611)  # test_png_io.h's libpng setjmp
        endif()
    endif()

    # A meme in resources/memes/ but not in memes.qrc is invisible at runtime (rcc has no globbing), and an
    # oversized one must be bounded on both axes; inspects the shipped pool against the source directory.
    add_executable(splash_meme_pool_test
        tests/splash_meme_pool_test.cpp
        src/Splashscreen.cpp
        src/Splashscreen.h)
    target_include_directories(splash_meme_pool_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(splash_meme_pool_test PRIVATE
        PJ_MEMES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/memes")
    target_link_libraries(splash_meme_pool_test PRIVATE pj_widgets pj_resources Qt6::Widgets Qt6::Svg GTest::gtest)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(splash_meme_pool_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME SplashMemePoolTest COMMAND splash_meme_pool_test)

    # Proves --exclude-libs,ALL (above) keeps static-archive symbols out of pj_app's dynamic symbol table.
    # `nm` is resolved at configure time so a toolchain without binutils says the guard is off, instead of a
    # test that passes without checking.
    if(NOT WIN32 AND NOT APPLE AND NOT EMSCRIPTEN)
        find_program(PJ_NM_EXECUTABLE nm)
        if(PJ_NM_EXECUTABLE)
            add_test(NAME ExportedSymbolsGuard COMMAND ${CMAKE_COMMAND}
                -DTARGET=$<TARGET_FILE:pj_app>
                -DNM=${PJ_NM_EXECUTABLE}
                -DSANITIZERS_ACTIVE=${PJ_SANITIZERS_ACTIVE}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_exported_symbols.cmake)
        else()
            message(STATUS "pj_app: 'nm' not found — ExportedSymbolsGuard disabled (install binutils to enable it)")
        endif()
    endif()

    # GUI cases link the real shell and pj_resources so the window's icons
    # resolve. MainWindow teardown leaks process-global state, so discovery
    # runs each case in its own process while sharing one linked executable.
    set(PJ_APP_GUI_TEST_SOURCES
        tests/slider_states_test.cpp
        tests/test_process_isolation_test.cpp)

    # Browser presets: PresetController's bounded download against a loopback server.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/preset_controller_test.cpp)

    # Browser presets: a layout opened by URL replays its relative file sources
    # without a picker, all or nothing (mock source plugin staged per case).
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/remote_preset_replay_test.cpp)
    set_source_files_properties(tests/remote_preset_replay_test.cpp PROPERTIES
        COMPILE_DEFINITIONS PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>")

    list(APPEND PJ_APP_GUI_TEST_SOURCES tests/main_window_startup_test.cpp)
    set_source_files_properties(tests/main_window_startup_test.cpp PROPERTIES
        COMPILE_DEFINITIONS PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>")

    # The Reload button comes back on every way a reload request ends, including the exits the
    # fileLoaded / fileLoadFailed signals stay silent on (rejected config dialog, queued cancel).
    list(APPEND PJ_APP_GUI_TEST_SOURCES tests/main_window_reload_button_test.cpp)
    set_source_files_properties(tests/main_window_reload_button_test.cpp PROPERTIES
        COMPILE_DEFINITIONS PJ_DIALOG_PROBE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:dialog_probe_source_plugin>")

    # A batch of files asks once how the loaders' dialogs behave (show all / first / skip).
    list(APPEND PJ_APP_GUI_TEST_SOURCES tests/main_window_batch_load_test.cpp tests/modal_answerer_test.cpp)
    set_source_files_properties(tests/main_window_batch_load_test.cpp PROPERTIES
        COMPILE_DEFINITIONS
        "PJ_DIALOG_PROBE_SOURCE_PLUGIN_PATH=\"$<TARGET_FILE:dialog_probe_source_plugin>\";PJ_MOCK_FILE_SOURCE_PLUGIN_PATH=\"$<TARGET_FILE:mock_file_source_plugin>\"")

    # Browser presets: a layout's streaming sources start from their saved
    # configuration inside the same all-or-nothing replay, and their views bind
    # as topics arrive (test stream plugin staged per case).
    add_library(mock_layout_stream_plugin SHARED tests/mock_layout_stream_plugin.cpp)
    target_compile_features(mock_layout_stream_plugin PRIVATE cxx_std_20)
    target_link_libraries(mock_layout_stream_plugin PRIVATE plotjuggler_sdk::plugin_sdk)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(mock_layout_stream_plugin PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/streaming_preset_test.cpp)
    set_source_files_properties(tests/streaming_preset_test.cpp PROPERTIES
        COMPILE_DEFINITIONS
        "PJ_MOCK_FILE_SOURCE_PLUGIN_PATH=\"$<TARGET_FILE:mock_file_source_plugin>\";PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH=\"$<TARGET_FILE:mock_layout_stream_plugin>\"")

    # An interactive stream start that does not launch (dialog cancelled, plugin
    # contract violation, refused configuration, failed start) leaves no dataset
    # behind (test stream staged per case).
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/streaming_dialog_exit_test.cpp)
    set_source_files_properties(tests/streaming_dialog_exit_test.cpp PROPERTIES
        COMPILE_DEFINITIONS PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH="$<TARGET_FILE:mock_layout_stream_plugin>")

    # Browser presets: the welcome dialog's selection, upload, cancel/retry and
    # thumbnail fallback (built on the desktop too; only its wiring is browser-only).
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/welcome_presets_dialog_test.cpp)

    # Bell dialog: severity / source / text filters, live rebuild on record,
    # cap eviction and Clear, driven straight through a DiagnosticHistory.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/diagnostics_dialog_test.cpp)

    # Preferences' segmented selectors keep their sizeHint width on every page, so
    # a long label on a page cannot squeeze one until its option labels overlap.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/preferences_selector_size_test.cpp)

    # A source added while streaming is paused writes into the secondary tail buffer, not the frozen primary
    # (applyWriteTargetsForCurrentPauseState).
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/streaming_pause_target_test.cpp)

    # One streaming poll tick through the ingestTickForTests seam: eager
    # processors advance over every topic of the dataset and the ingest
    # announcement names them all.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/streaming_ingest_tick_test.cpp)

    # Curve-list drag policy: an object topic no view can display (CameraInfo,
    # ...) must be drag-inert with a "why" tooltip; displayable ones keep
    # dragging.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/curve_list_panel_test.cpp)

    # A layout's restored streaming source and buffer reach StreamingSourceManager without touching QSettings.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/left_panel_streaming_test.cpp)

    # Dataset "Info" dialog: name/file rows plus a domain-neutral provenance
    # tree from the SourceRecord (descriptor JSON as generic key/value nodes,
    # 64-bit timestamps printed exactly); no record -> no provenance section.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/dataset_info_dialog_test.cpp)

    # Source-bound layout seam: one replayable file that fans out into several
    # datasets must persist/restore each independent timeline track.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_source_layout_test.cpp)

    # Fan-out timeline matcher: when a saved same-named fan-out shape no longer
    # matches the loaded one, no offset may bind.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_fanout_ambiguous_test.cpp)

    # A layout's marker generators are replayed — and their scripts run — on a
    # session where no toolbox panel was ever opened.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_marker_generator_restore_test.cpp)

    # A running marker rule puts a computing note on the plots that display it.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_marker_status_test.cpp)

    # A precision chosen in Preferences redraws the plots' tracker on OK.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_value_precision_test.cpp)

    # Generators and transform inputs carry their dataset like a curve: a
    # layout-relative path in a source-bound layout, nothing in a generic one.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_layout_dataset_paths_test.cpp)

    # A layout aimed at one dataset over several matching ones: asked, duplicated per dataset, or the first one.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_layout_dataset_choice_test.cpp)
    set_source_files_properties(tests/main_window_layout_dataset_choice_test.cpp PROPERTIES
        COMPILE_DEFINITIONS PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>")

    # The Custom Series list is derived from DataProcessorService, not accumulated
    # from plugin announcements — the paths that announce nothing are a layout
    # restore and a withdrawal through the data-processors ABI.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_custom_series_test.cpp)

    # The Custom Topics panel through the shell: listed recipes, row names, owner gates on edit and delete, the
    # declared editor's "+", the row glyph following the node state, and the coalesced refresh.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_custom_topics_test.cpp)

    # Layouts saved by a release-4.1 build restore through the real load paths and
    # save back to the same <data_processors> block.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_release41_layout_test.cpp)
    set_source_files_properties(tests/main_window_release41_layout_test.cpp PROPERTIES
        COMPILE_DEFINITIONS PJ_RELEASE41_LAYOUT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/layouts/release-4.1")
    # ... and so do the on-demand recipes of layouts saved by the derived-objects prototype.
    set_property(SOURCE tests/main_window_release41_layout_test.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS PJ_PROTOTYPE_LAYOUT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/layouts/prototype")

    # On-demand recipes through undo and redo: deletion, exemption, previews.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_on_demand_restore_test.cpp)

    # The Custom Topics editor through the shell: the toolbox declaring `custom_topics_editor` opens from "+", its
    # preview frame embeds a scene view, and a drop onto its panel delivers names the plugin addresses. The fixture
    # toolbox is staged into the test's extensions directory.
    add_library(custom_topics_editor_fixture_plugin SHARED tests/custom_topics_editor_fixture.cpp)
    target_compile_features(custom_topics_editor_fixture_plugin PRIVATE cxx_std_20)
    target_link_libraries(custom_topics_editor_fixture_plugin PRIVATE plotjuggler_sdk::plugin_sdk)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(custom_topics_editor_fixture_plugin PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_editor_integration_test.cpp)
    set_source_files_properties(tests/main_window_editor_integration_test.cpp PROPERTIES
        COMPILE_DEFINITIONS PJ_CUSTOM_TOPICS_EDITOR_FIXTURE_PATH="$<TARGET_FILE:custom_topics_editor_fixture_plugin>")

    # A hand-made 3D view filled by a user drop frames its first content; a
    # restored layout keeps its saved fixed frame and camera.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_scene3d_drop_framing_test.cpp)

    # The Log panel through the shell: the placeholder button, a drop on an empty area, the time format, layouts.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_log_panel_test.cpp)

    # The 2D scene panel's layer rows carry each layer's warning from the start.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/scene2d_config_panel_test.cpp)

    # Window frame: the system title bar by default, the frameless custom one
    # only when Preferences -> Appearance opts in (bar contents, the double-click
    # seam, MainWindow flags and the toggle's persistence).
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_frame_test.cpp)

    # Preferences' footer reset button resets only the chrome-metric scrubbers,
    # which ship hidden outside --debug-mode, so the button follows them.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/preferences_reset_button_test.cpp)

    # A plugin folder that no longer exists stays listed, in a grey that reads in both themes.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/item_view_rows_theme_test.cpp
        tests/label_link_theme_test.cpp
        tests/preferences_missing_folder_test.cpp)

    # The first GPU widget does not recreate the main window's native window.
    # Desktop only, like the hidden QRhiWidget that ensures it.
    if(NOT EMSCRIPTEN)
        list(APPEND PJ_APP_GUI_TEST_SOURCES
            tests/main_window_rhi_window_test.cpp)
        # A file drag over a curve drop target still reaches MainWindow (desktop only).
        list(APPEND PJ_APP_GUI_TEST_SOURCES
            tests/main_window_file_drop_test.cpp)
    endif()

    # Viewport re-frame after a layout's timeline offsets settle: the sync path and the progressive-drain
    # pending-timeline seam.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_viewport_reframe_test.cpp)

    # Plugin-owned views: tab ids stay stable across renames and reordering,
    # and displayTimeForSource refuses a topic that two datasets both advertise.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_plugin_views_test.cpp)

    # Transactional workspace history uses one MainWindow per ctest process;
    # the shell leaks process-global widget state across instances.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_history_test.cpp)

    # History-exempt transforms and generators round-trip through the layout
    # file but remain live across undo/redo.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_history_exempt_test.cpp)

    # Layout-import batch restore, with isolated ctest processes:
    #   alive  — a materialize MISS import keeps the restore alive across the
    #            catalog-empty guard; curves bind at the batch-gated drain.
    #   policy — a non-interactive restore opens zero dialogs; unresolved
    #            intents are retained + diagnosed, never cleared.
    #   cancel — cancel mid-import rolls back to the pre-layout workspace.
    # Keep the alive/cancel/lifecycle scenarios standalone to preserve their
    # import lifetime, rollback, and supersession boundaries.
    add_executable(main_window_layout_import_alive_test tests/main_window_layout_import_alive_test.cpp)
    target_include_directories(main_window_layout_import_alive_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(main_window_layout_import_alive_test PRIVATE pj_app_shell pj_resources GTest::gtest)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(main_window_layout_import_alive_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME MainWindowLayoutImportAliveTest COMMAND main_window_layout_import_alive_test)
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_layout_import_policy_test.cpp)
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_layout_apply_reason_test.cpp)
    # Keyboard focus after a workspace rebuild deletes the focused plot.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_restore_focus_test.cpp)
    add_executable(main_window_layout_import_cancel_test tests/main_window_layout_import_cancel_test.cpp)
    target_include_directories(main_window_layout_import_cancel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(main_window_layout_import_cancel_test PRIVATE pj_app_shell pj_resources GTest::gtest)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(main_window_layout_import_cancel_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME MainWindowLayoutImportCancelTest COMMAND main_window_layout_import_cancel_test)
    #   lifecycle — explicit progressive policy on a mixed automated restore, apply-failure diagnostics, full
    #              supersession by a sync-path load, and automated plain-source failure dialog suppression.
    add_executable(main_window_layout_import_lifecycle_test tests/main_window_layout_import_lifecycle_test.cpp)
    target_include_directories(main_window_layout_import_lifecycle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(main_window_layout_import_lifecycle_test PRIVATE pj_app_shell pj_resources GTest::gtest)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(main_window_layout_import_lifecycle_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME MainWindowLayoutImportLifecycleTest COMMAND main_window_layout_import_lifecycle_test)
    target_compile_definitions(main_window_layout_import_lifecycle_test PRIVATE
        PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>"
    )
    add_dependencies(main_window_layout_import_lifecycle_test mock_file_source_plugin)
    #   binder — the ingest strip's displayed ownership, batch-scoped SessionManager observation, the batch
    #            job's keep-partial Stop route, toolbox panels vs headless jobs (no fold; busy pinned panels
    #            survive the drain), mid-import curve binding, on_dataset/ingestBegan order, the real promotion.
    # This suite's plugin-path definition must not propagate to other sources.
    add_executable(main_window_layout_import_binder_test tests/main_window_layout_import_binder_test.cpp)
    target_include_directories(main_window_layout_import_binder_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(main_window_layout_import_binder_test PRIVATE pj_app_shell pj_resources GTest::gtest)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(main_window_layout_import_binder_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME MainWindowLayoutImportBinderTest COMMAND main_window_layout_import_binder_test)
    target_compile_definitions(main_window_layout_import_binder_test PRIVATE
        PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>"
    )
    add_dependencies(main_window_layout_import_binder_test mock_file_source_plugin)

    #   settle — layoutRestoreSettled fires exactly once per load
    #            terminal (open-failure false + dump composition, plain sync
    #            true, batch true only at batch finish — never mid-import).
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_layout_settle_test.cpp)
    #   e2e    — live cross-repo test: real plugin DSOs from $MCAP_CLOUD_E2E_EXTENSIONS, a live server at
    #            $MCAP_CLOUD_E2E_URL, descriptor vectors from $MCAP_CLOUD_E2E_VECTORS. SKIPs without that env;
    #            the e2e-layout-import.sh harness provides it and treats SKIP as failure.
    # The private HOME/XDG sandbox must exist before QApplication construction.
    add_executable(main_window_layout_import_e2e_test tests/main_window_layout_import_e2e_test.cpp)
    target_include_directories(main_window_layout_import_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(main_window_layout_import_e2e_test PRIVATE pj_app_shell pj_resources GTest::gtest)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(main_window_layout_import_e2e_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME MainWindowLayoutImportE2ETest COMMAND main_window_layout_import_e2e_test)
    #   exit   — LayoutExitWatch (the --exit-after-layout exit-code decision):
    #            load-failed/success/timeout mapping, decide-once latching,
    #            and the parked-batch teardown staying crash-free.
    add_executable(main_window_layout_exit_test tests/main_window_layout_exit_test.cpp)
    target_include_directories(main_window_layout_exit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(main_window_layout_exit_test PRIVATE pj_app_shell pj_resources GTest::gtest)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(main_window_layout_exit_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME MainWindowLayoutExitTest COMMAND main_window_layout_exit_test)

    # --dump-diagnostics collector: full-run accumulation from the queued bridge (pre-attach records included),
    # the five-field JSON shape, no truncation at the DiagnosticHistory ring cap. No MainWindow.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/diagnostic_dump_test.cpp)

    # The left column's width belongs to the user: neither a window resize nor a
    # chart-area takeover may move the separator they dragged.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_panel_geometry_test.cpp)

    # A toolbox panel with a download in flight folds into a background tab
    # instead of being destroyed: the banner X, the folded tab's own close, the
    # auto-fold on import start, layout-save exclusion, and completion-close
    # suppression.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/toolbox_panel_fold_test.cpp)

    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/pending_display_binder_test.cpp)

    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/multi_dataset_rebind_test.cpp)

    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/topic_demand_controller_test.cpp)

    # Custom Widget panel through the shell: restore, save and the missing-plugin element, plus the
    # destroy-before-session order at window teardown. Uses the in-process plugin of pj_custom_widgets.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_custom_widget_test.cpp
        tests/custom_widget_panels_test.cpp)

    # Headless runs of the real executable on a layout with a Custom Widget (with and without its
    # plugin library); the fixtures are built by pj_custom_widgets/tests.
    include(${CMAKE_SOURCE_DIR}/pj_custom_widgets/tests/E2E.cmake)

    # Test-only DataSource plugin that calls the host's message box from the import worker thread and records
    # its askContinue outcome to PJ_MSGBOX_PROBE_FILE (the shutdown gate answers -1). Links only pj_base.
    add_library(msgbox_mock_source_plugin SHARED tests/msgbox_mock_source.cpp)
    target_compile_features(msgbox_mock_source_plugin PRIVATE cxx_std_20)
    target_link_libraries(msgbox_mock_source_plugin PRIVATE plotjuggler_sdk::base)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(msgbox_mock_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
    endif()

    # Test-only DataSource plugin that deterministically leaves a single-load
    # progress metacall queued while FileLoader joins its worker for shutdown.
    add_library(progress_shutdown_source_plugin SHARED tests/progress_shutdown_source.cpp)
    target_compile_features(progress_shutdown_source_plugin PRIVATE cxx_std_20)
    target_link_libraries(progress_shutdown_source_plugin PRIVATE plotjuggler_sdk::base)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(progress_shutdown_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
    endif()

    # Test-only DataSource plugin with an embedded config dialog (kCapabilityHasDialog).
    # Shutdown while suspended at the plugin dialog: the dialog is rejected before the plugin is destroyed.
    add_library(dialog_probe_source_plugin SHARED tests/dialog_probe_source.cpp)
    target_compile_features(dialog_probe_source_plugin PRIVATE cxx_std_20)
    target_link_libraries(dialog_probe_source_plugin PRIVATE plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(dialog_probe_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
    endif()

    # The loader TUs, compiled once for the standalone FileLoader-pipeline suites (file_loader_test,
    # source_promotion_host_test, headless_descriptor_provider_session_test). Their plugin-path definitions
    # differ, so each keeps its own executable. Header side: tests/support/loader_test_support.h.
    add_library(pj_app_loader_testlib OBJECT
        src/SourcePromotionHost.cpp
        src/SourcePromotionHost.h
        src/HeadlessDescriptorProviderSession.cpp
        src/HeadlessDescriptorProviderSession.h
        src/ToolboxHostWiring.h
        src/FileLoader.cpp
        src/FileLoader.h
        src/BrowserFileStore.cpp
        src/BrowserFileStore.h
        src/FileSelectionService.cpp
        src/FileSelectionService.h
        src/LoadInput.h
        src/DialogPresenter.cpp
        src/DialogPresenter.h
        src/DialogWatch.h
        src/FanoutConfig.cpp
        src/FanoutConfig.h
        # FileLoader's same-source match uses LayoutXml::isSamePath for full-path
        # dataset identity; the standalone suites must link its definition.
        src/LayoutXml.cpp
        src/LayoutXml.h
    )
    set_target_properties(pj_app_loader_testlib PROPERTIES AUTOMOC ON)
    target_include_directories(pj_app_loader_testlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(pj_app_loader_testlib PUBLIC
        pj_runtime
        pj_widgets
        pj_scene3d_widgets
        pj_dialog_engine_qt
        Qt6::Widgets
        GTest::gtest
    )
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(pj_app_loader_testlib PRIVATE ${PJ_WARNING_FLAGS})
    endif()

    # End-to-end FileLoader pipeline: same-file reload, the worker-thread message-box marshal. The mock plugins
    # are staged into a temp extensions dir at runtime.
    add_executable(file_loader_test
        tests/file_loader_test.cpp
        tests/file_loader_shutdown_progress_test.cpp
    )
    set_target_properties(file_loader_test PROPERTIES AUTOMOC ON)
    target_compile_definitions(file_loader_test PRIVATE
        PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>"
        PJ_MSGBOX_MOCK_SOURCE_PLUGIN_PATH="$<TARGET_FILE:msgbox_mock_source_plugin>"
        PJ_DIALOG_PROBE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:dialog_probe_source_plugin>"
        PJ_PROGRESS_SHUTDOWN_SOURCE_PLUGIN_PATH="$<TARGET_FILE:progress_shutdown_source_plugin>"
    )
    # $<TARGET_FILE:...> in a compile definition does not create a build-order
    # dependency; the plugins must exist when the test runs.
    add_dependencies(file_loader_test
        mock_file_source_plugin
        msgbox_mock_source_plugin
        dialog_probe_source_plugin
        progress_shutdown_source_plugin
    )
    target_link_libraries(file_loader_test PRIVATE pj_app_loader_testlib)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(file_loader_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME FileLoaderTest COMMAND file_loader_test)

    # Host side of the SDK's "pj.source_promotion.v1" service: drives the real
    # FileLoader replace pipeline through the C ABI. Same standalone shape and
    # scaffolding as file_loader_test above.
    add_executable(source_promotion_host_test
        tests/source_promotion_host_test.cpp
    )
    set_target_properties(source_promotion_host_test PROPERTIES AUTOMOC ON)
    target_compile_definitions(source_promotion_host_test PRIVATE
        PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>"
    )
    # $<TARGET_FILE:...> in a compile definition does not create a build-order
    # dependency; the plugin must exist when the test runs.
    add_dependencies(source_promotion_host_test mock_file_source_plugin)
    target_link_libraries(source_promotion_host_test PRIVATE pj_app_loader_testlib)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(source_promotion_host_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME SourcePromotionHostTest COMMAND source_promotion_host_test)

    # Headless per-batch owner of one descriptor-import provider toolbox instance: manifest-id resolution, bind,
    # extension view, promotion service, queued-only callback marshal, quiescence teardown. Fake provider
    # registered statically (no DSO).
    add_executable(headless_descriptor_provider_session_test
        tests/headless_descriptor_provider_session_test.cpp
    )
    set_target_properties(headless_descriptor_provider_session_test PROPERTIES AUTOMOC ON)
    target_link_libraries(headless_descriptor_provider_session_test PRIVATE pj_app_loader_testlib)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(headless_descriptor_provider_session_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME HeadlessDescriptorProviderSessionTest COMMAND headless_descriptor_provider_session_test)

    # The rewrite-then-classify restore coordinator: classify matrix, trust gate, guard-3 ceiling,
    # provider-absent fallback, job chaining, cancel/rollback; a descriptor-scripted fake provider plus the real
    # FileLoader. Desktop only: SourceCacheStore/SourceCaptureService do not compile in the browser.
    if(NOT EMSCRIPTEN)
        add_executable(layout_import_batch_test
            tests/layout_import_batch_test.cpp
            src/LayoutImportBatch.cpp
            src/LayoutImportBatch.h
        )
        set_target_properties(layout_import_batch_test PROPERTIES AUTOMOC ON)
        target_compile_definitions(layout_import_batch_test PRIVATE
            PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>"
            # The capture-mode fake provider pushes through this real parser
            # plugin (staged into the fixture's extensions dir), so a wired
            # capture publishes an actual cache artifact.
            PJ_RUNTIME_HOST_OBJECT_PARSER_PATH="$<TARGET_FILE:runtime_host_object_parser_plugin>"
        )
        # $<TARGET_FILE:...> in a compile definition does not create a build-order
        # dependency; the plugins must exist when the test runs.
        add_dependencies(layout_import_batch_test mock_file_source_plugin runtime_host_object_parser_plugin)
        target_link_libraries(layout_import_batch_test PRIVATE pj_app_loader_testlib)
        if(DEFINED PJ_WARNING_FLAGS)
            target_compile_options(layout_import_batch_test PRIVATE ${PJ_WARNING_FLAGS})
        endif()
        add_test(NAME LayoutImportBatchTest COMMAND layout_import_batch_test)
    endif()

    # The Source Timeline binding: the bars track data growth (samplesIngested), not just new catalog topics.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/source_timeline_controller_test.cpp
        tests/timeline_widget_test.cpp)

    # Reuse the shell's controller objects and meta-objects for timer and
    # progress-row tests, alongside the other runtime-backed GUI cases.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/ingest_progress_controller_test.cpp)

    # A series replay as a progress entry of its topic: the linger, and the stop by topic.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/series_progress_controller_test.cpp)
    # The MCP server over loopback HTTP: held replies that wait for the user, the dialogs an agent
    # opens for the user (a file loader's, a stream's), the gates (desktop only).
    if(NOT EMSCRIPTEN)
        list(APPEND PJ_APP_GUI_TEST_SOURCES tests/mcp_server_test.cpp)
        set_source_files_properties(tests/mcp_server_test.cpp PROPERTIES
            COMPILE_DEFINITIONS
            "PJ_MOCK_FILE_SOURCE_PLUGIN_PATH=\"$<TARGET_FILE:mock_file_source_plugin>\";PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH=\"$<TARGET_FILE:mock_layout_stream_plugin>\";PJ_DIALOG_PROBE_SOURCE_PLUGIN_PATH=\"$<TARGET_FILE:dialog_probe_source_plugin>\"")
    endif()
    # The feature catalog resource, whose entry points must name the live
    # widget labels. Desktop only, like FeatureCatalogHost.
    if(NOT EMSCRIPTEN)
        list(APPEND PJ_APP_GUI_TEST_SOURCES
            tests/feature_catalog_test.cpp)
        set_source_files_properties(tests/feature_catalog_test.cpp PROPERTIES
            COMPILE_DEFINITIONS PJ_FEATURE_CATALOG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    endif()

    pj_add_test_runner(pj_app_gui_tests
        MAIN gui WARNINGS
        SOURCES ${PJ_APP_GUI_TEST_SOURCES} tests/support/gui_test_env.cpp
        # Qt6::Test: QTest::qWaitForWindowExposed, for the drawer-splitter cases in
        # toolbox_panel_fold_test that need a real window-system resize event.
        LIBS pj_app_shell pj_resources plotjuggler_sdk::plugin_sdk Qt6::Test
    )
    set_target_properties(pj_app_gui_tests PROPERTIES AUTOMOC ON)
    # $<TARGET_FILE:...> in a compile definition creates no build-order dependency.
    add_dependencies(pj_app_gui_tests custom_topics_editor_fixture_plugin
                     mock_file_source_plugin mock_layout_stream_plugin dialog_probe_source_plugin)
    # on_demand_test_utils.h: the fake script run and clouds shared with pj_runtime's tests.
    # preset_controller_test serves canned responses with pj_runtime's loopback test server.
    target_include_directories(pj_app_gui_tests PRIVATE ${CMAKE_SOURCE_DIR}/pj_runtime/tests)
    target_include_directories(pj_app_gui_tests PRIVATE ${CMAKE_SOURCE_DIR}/pj_custom_widgets/tests)
    # The MCP UI tools again at scale 2: the input they queue must land on the same widgets at
    # native pixels as at logical ones.
    if(NOT EMSCRIPTEN)
        gtest_discover_tests(pj_app_gui_tests
            TEST_PREFIX "pj_app_gui_tests.dpr2."
            TEST_FILTER "McpServerTest.UiTools*"
            DISCOVERY_MODE PRE_TEST
            PROPERTIES TIMEOUT 120 RESOURCE_LOCK pj_app_gui_tests ENVIRONMENT_MODIFICATION "QT_SCALE_FACTOR=set:2")
    endif()

    # QRhiWidgets in ADS docks across float/redock, split, sibling destruction,
    # floating-window close and DPR changes, plus the mixed-API negative case.
    # Needs a real display (skips on offscreen). Runs on every
    # backend of the platform plus OpenGL at DPR 2 (RHI_BACKENDS, cmake/PjTests.cmake).
    if(NOT EMSCRIPTEN)
        find_package(Qt6 ${PJ_QT_REQUIRED_VERSION} REQUIRED COMPONENTS OpenGLWidgets)
        pj_add_test_runner(rhi_dock_lifecycle_test
            MAIN gui WARNINGS RHI_BACKENDS DPR2
            SOURCES tests/rhi_dock_lifecycle_test.cpp
            LIBS qt_advanced_docking pj_widgets $<TARGET_NAME_IF_EXISTS:pj_scene2d_widgets> pj_resources
                 Qt6::OpenGLWidgets Qt6::Test
            TIMEOUT 60
        )
        # The same matrix with the real widgets: a plot, a Scene3D point cloud and
        # the Scene2D media viewer (plus its failure state) in one dock manager.
        if(TARGET pj_scene2d_widgets AND TARGET pj_scene3d_widgets)
            pj_add_test_runner(rhi_mixed_dock_test
                MAIN gui WARNINGS RHI_BACKENDS DPR2
                SOURCES tests/rhi_mixed_dock_test.cpp
                LIBS pj_mixed_layout_fixture pj_scene3d_widgets_testing pj_resources Qt6::Test
                TIMEOUT 60
            )
        endif()
        # The packaging smokes' render check (packaging/smoke/render_check.sh) on this
        # tree's app: the fixture's plots, Scene3D view and media viewer must all come
        # up on OpenGL. Needs a display, hence the qrhi-opengl label, and runs the
        # built app, so build pj_app first (CI does). Linux only: Windows runs the
        # same check on the installed app (windows-release.yml).
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET pj_scene2d_widgets AND TARGET pj_scene3d_widgets)
            add_test(NAME scene3d_rhi_app_smoke
                COMMAND sh ${CMAKE_SOURCE_DIR}/packaging/smoke/render_check.sh $<TARGET_FILE:pj_app>)
            # Skipped (77) unless QT_QPA_PLATFORM names a real QPA, as the gtest
            # display passes are; a failure instead under PJ_REQUIRE_GPU_TESTS=1.
            set_tests_properties(scene3d_rhi_app_smoke PROPERTIES
                LABELS qrhi-opengl TIMEOUT 120 SKIP_RETURN_CODE 77
                ENVIRONMENT "PJ_RENDER_BACKEND=OpenGL;PJ_RENDER_SKIP_WITHOUT_DISPLAY=1")
        endif()
    endif()
endif()

# An agent's view end to end: the built app's --mcp-bridge over stdio in front of an offscreen
# `plotjuggler4 --mcp`, under a private HOME (XDG paths isolate settings and discovery, hence Linux).
if(PJ_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pj_add_test_runner(mcp_bridge_test
        MAIN core WARNINGS
        SOURCES tests/mcp_bridge_test.cpp
        LIBS Qt6::Core Qt6::Network nlohmann_json::nlohmann_json
        TIMEOUT 120
    )
    target_compile_definitions(mcp_bridge_test PRIVATE
        PJ_APP_PATH="$<TARGET_FILE:pj_app>"
        PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>")
    add_dependencies(mcp_bridge_test pj_app mock_file_source_plugin)
endif()

# The mixed-layout fixture (tests/mixed_layout_fixture.h): a plot, a Scene3D view
# and a media viewer in one dock manager, through public APIs only. EXCLUDE_FROM_ALL: built for its consumers only,
# rhi_mixed_dock_test above and mixed_layout_benchmark below.
if(NOT EMSCRIPTEN AND TARGET pj_scene2d_widgets AND TARGET pj_scene3d_widgets)
    add_library(pj_mixed_layout_fixture STATIC EXCLUDE_FROM_ALL
        tests/mixed_layout_fixture.cpp
        tests/mixed_layout_fixture.h
    )
    target_include_directories(pj_mixed_layout_fixture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(pj_mixed_layout_fixture PUBLIC
        qt_advanced_docking pj_plotting pj_scene2d_widgets pj_scene3d_widgets Qt6::Widgets Qt6::Xml)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(pj_mixed_layout_fixture PRIVATE ${PJ_WARNING_FLAGS})
    endif()

    # Mixed-layout budgets (a dev demo, no ctest), same gate as the root demos.
    if(PJ_BUILD_DEMOS)
        add_executable(mixed_layout_benchmark benchmarks/mixed_layout_benchmark.cpp)
        target_link_libraries(mixed_layout_benchmark PRIVATE
            pj_mixed_layout_fixture pj_scene3d_widgets_testing pj_resources pj_bench_probes)
        if(DEFINED PJ_WARNING_FLAGS)
            target_compile_options(mixed_layout_benchmark PRIVATE ${PJ_WARNING_FLAGS})
        endif()
    endif()
endif()
