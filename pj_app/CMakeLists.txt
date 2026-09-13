find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Svg Test)

# backward-cpp (Conan): symbolized crash backtraces. The package exports the
# `Backward` config and the `Backward::Backward` target, which propagates the
# BACKWARD_HAS_* defines and links the stack-detail backend (elfutils/libdw)
# transitively. main.cpp installs the signal handler.
if(NOT EMSCRIPTEN)
    find_package(Backward REQUIRED)
endif()

# The whole shell as an OBJECT library so a headless test can link the exact
# same MainWindow object files (see main_window_source_layout_test) instead of
# duplicating the widget tree. The executable below is a thin main.cpp wrapper.
add_library(pj_app_shell OBJECT
    src/BuiltinPlugins.cpp
    src/BuiltinPlugins.h
    src/Splashscreen.cpp
    src/Splashscreen.h
    src/DialogPresenter.cpp
    src/DialogPresenter.h
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
    src/HeadlessDescriptorProviderSession.cpp
    src/HeadlessDescriptorProviderSession.h
    src/ToolboxHostWiring.h
    src/PendingDisplayBinder.cpp
    src/PendingDisplayBinder.h
    src/TopicDemandController.cpp
    src/TopicDemandController.h
    src/StreamingSourceManager.cpp
    src/StreamingSourceManager.h
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
    src/IngestProgressWiring.cpp
    src/IngestProgressWiring.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/MainWindow.ui
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
    src/ui/TimelineWidget.cpp
    src/ui/TimelineWidget.h
    src/ui/TimelineWidget.ui
    src/RasterKeyMap.cpp
    src/RasterKeyMap.h
)

if(TARGET pj_scene2d_widgets)
    target_sources(pj_app_shell PRIVATE
        src/ui/Scene2DConfigPanel.cpp
        src/ui/Scene2DConfigPanel.h)
endif()
if(TARGET pj_scene3d_widgets)
    target_sources(pj_app_shell PRIVATE
        src/ui/Scene3DConfigPanel.cpp
        src/ui/Scene3DConfigPanel.h)
endif()

if(EMSCRIPTEN)
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
        endif()
        if(TARGET pj_wasm_mcap_source AND NOT TARGET pj_wasm_ros_parser)
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

if(TARGET pj_wasm_csv_source)
    target_link_libraries(pj_app_shell PUBLIC pj_wasm_csv_source)
    target_compile_definitions(pj_app_shell PRIVATE PJ_WASM_WITH_CSV_PLUGIN)
endif()
if(TARGET pj_wasm_mcap_source)
    target_link_libraries(pj_app_shell PUBLIC pj_wasm_mcap_source)
    target_compile_definitions(pj_app_shell PRIVATE PJ_WASM_WITH_MCAP_PLUGIN)
endif()
if(TARGET pj_wasm_ros_parser)
    target_link_libraries(pj_app_shell PUBLIC pj_wasm_ros_parser)
    target_compile_definitions(pj_app_shell PRIVATE PJ_WASM_WITH_ROS_PLUGIN)
endif()
if(TARGET pj_wasm_protobuf_parser)
    target_link_libraries(pj_app_shell PUBLIC pj_wasm_protobuf_parser)
    target_compile_definitions(pj_app_shell PRIVATE PJ_WASM_WITH_PROTOBUF_PLUGIN)
endif()
if(TARGET pj_wasm_transform_editor)
    target_link_libraries(pj_app_shell PUBLIC pj_wasm_transform_editor)
    target_compile_definitions(pj_app_shell PRIVATE PJ_WASM_WITH_TRANSFORM_EDITOR_PLUGIN)
endif()
if(TARGET pj_wasm_dummy_stream)
    target_link_libraries(pj_app_shell PUBLIC pj_wasm_dummy_stream)
    target_compile_definitions(pj_app_shell PRIVATE PJ_WASM_WITH_DUMMY_STREAM_PLUGIN)
endif()

if(EMSCRIPTEN)
    qt_add_executable(pj_app
        src/main.cpp
        ${CMAKE_SOURCE_DIR}/resources/resources.qrc)
else()
    add_executable(pj_app src/main.cpp)
endif()
set_target_properties(pj_app PROPERTIES
    # The CMake target id stays `pj_app` (internal module name), but the installed
    # binary is `plotjuggler4` — the user-facing command and what packaging ships
    # (e.g. /usr/bin/plotjuggler4). Keeps the source module rename a separate concern.
    OUTPUT_NAME plotjuggler4
)
target_include_directories(pj_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pj_app PRIVATE
    pj_app_shell
    $<$<NOT:$<BOOL:${EMSCRIPTEN}>>:pj_resources>
    $<$<NOT:$<BOOL:${EMSCRIPTEN}>>:Backward::Backward>)

# The admission helper is resolved at runtime as a sibling of the executable
# (PluginCheckRunner::locateHelper -> applicationDirPath), which is what an
# install layout gives for free: both land in bin/. A build tree does not — each
# target writes into its own module directory — so mirror the helper next to the
# app after linking. Without this the runner falls back to a bare name and only
# finds the helper if it happens to be on PATH.
#
# Driven from here rather than by overriding the helper's output directory: this
# keeps the dependency pointing from the consumer to the tool, and leaves the
# standalone marketplace build (where no app target exists) untouched.
if(TARGET pj-plugin-check)
    add_dependencies(pj_app pj-plugin-check)
    add_custom_command(TARGET pj_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:pj-plugin-check> $<TARGET_FILE_DIR:pj_app>
        COMMENT "Placing pj-plugin-check next to the application binary")
endif()

# App version: single source of truth is versions.env, stamped by the root
# CMakeLists into generated/pj_version.h (PJ_VERSION_STRING). main.cpp — the only
# user — includes that header directly, so no per-target compile definition is
# needed (a duplicate -D here would redefine the macro under -Werror).

# -Werror guards against the bug class that bit the reload path: a merge that
# drops wiring leaves an unused variable behind, which these flags turn into a
# build failure instead of a silent regression.
if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_app_shell PRIVATE ${PJ_WARNING_FLAGS})
    target_compile_options(pj_app PRIVATE ${PJ_WARNING_FLAGS})
endif()

# Don't re-export symbols pulled in from static archive members. Without
# this, Conan-built libpng/libjpeg/etc. symbols are exported globally from
# pj_app and the dynamic linker can interpose them ahead of the system
# copies that GTK/glib load — see for example the libpng 1.6.43 (system)
# vs 1.6.58 (Conan) ABI skew that crashes QGtk3Interface::fileIcon during
# QFileDialog with an "invalid pointer" free. --exclude-libs,ALL is the
# GNU/LLD spelling; harmless on other linkers but only meaningful here.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE AND NOT WIN32 AND NOT EMSCRIPTEN)
    target_link_options(pj_app PRIVATE "LINKER:--exclude-libs,ALL")
endif()

if(EMSCRIPTEN)
    set_target_properties(pj_app PROPERTIES
        AUTORCC ON
        QT_WASM_INITIAL_MEMORY "256MB")
    target_compile_options(pj_app PRIVATE -fexceptions)
    target_link_options(pj_app PRIVATE -fexceptions -sALLOW_MEMORY_GROWTH=1)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
    find_package(GTest REQUIRED)

    # Qt-only helpers compile their implementation once without the shell's
    # dependency closure. Discovery keeps a fresh QApplication per test case.
    # no WARNINGS: 1 of 9 merged tests never compiled with -Werror
    pj_add_test_runner(pj_app_unit_tests
        MAIN gui
        SOURCES
            tests/browser_file_store_test.cpp
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
            src/LoadInput.h
            src/BrowserPersistence.cpp
            src/BrowserPersistence.h
            src/FileSelectionService.cpp
            src/FileSelectionService.h
            # Pure config-parsing logic (no GUI), compiled standalone so it's
            # linkable without pj_app_shell. FanoutConfig.cpp is also part of pj_app.
            src/FanoutConfig.cpp
            # LayoutXml is a self-contained DOM helper module (no Qt::Widgets,
            # no MainWindow), so compile it directly into this test runner.
            src/LayoutXml.cpp
            src/LayoutXml.h
            src/KeySequence.cpp
            src/RasterKeyMap.cpp
        LIBS Qt6::Widgets Qt6::Xml
    )
    set_target_properties(pj_app_unit_tests PROPERTIES AUTOMOC ON)
    target_include_directories(pj_app_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/raster/ipc
    )

    # Guards the meme splash pool against its two silent failure modes: a meme
    # present in resources/memes/ but absent from resources.qrc is invisible at
    # runtime (rcc has no globbing), and an oversized meme must be bounded on
    # both axes, not just width. Links pj_resources so the test inspects the
    # real shipped pool, and is handed the source directory to compare against.
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

    # Proves --exclude-libs,ALL (above) still keeps third-party static-archive
    # symbols out of pj_app's dynamic symbol table. Needs a real ELF and binutils,
    # so it is a script test rather than a gtest binary.
    #
    # `nm` is resolved HERE rather than inside the script so the "no binutils"
    # case is decided once, visibly, at configure time: a bare toolchain gets a
    # STATUS line saying the guard is off instead of either a mystery build
    # failure or a test that silently passes without checking anything. Every CI
    # image has binutils, so the guard always runs where it matters.
    if(NOT WIN32 AND NOT APPLE AND NOT EMSCRIPTEN)
        find_program(PJ_NM_EXECUTABLE nm)
        if(PJ_NM_EXECUTABLE)
            add_test(NAME ExportedSymbolsGuard COMMAND ${CMAKE_COMMAND}
                -DTARGET=$<TARGET_FILE:pj_app>
                -DNM=${PJ_NM_EXECUTABLE}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_exported_symbols.cmake)
        else()
            message(STATUS "pj_app: 'nm' not found — ExportedSymbolsGuard disabled (install binutils to enable it)")
        endif()
    endif()

    # GUI cases link the real shell and pj_resources so the window's icons
    # resolve. MainWindow teardown leaks process-global state, so discovery
    # runs each case in its own process while sharing one linked executable.
    set(PJ_APP_GUI_TEST_SOURCES
        tests/slider_states_test.cpp)

    # Bell dialog: severity / source / text filters, live rebuild on record,
    # cap eviction and Clear, driven straight through a DiagnosticHistory.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/diagnostics_dialog_test.cpp)

    # MR !261 depth guard: a source added while streaming is paused must write
    # into the secondary tail buffer, not the frozen primary. Exercises the
    # extracted pause-target seam (applyWriteTargetsForCurrentPauseState) headlessly.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/streaming_pause_target_test.cpp)

    # Curve-list drag policy: an object topic no view can display (CameraInfo,
    # ...) must be drag-inert with a "why" tooltip; displayable ones keep
    # dragging.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/curve_list_panel_test.cpp)

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
    # matches the loaded one, no offset may bind (FIX 4).
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_fanout_ambiguous_test.cpp)

    # A layout's marker generators are replayed — and their scripts run — on a
    # session where no toolbox panel was ever opened.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_marker_generator_restore_test.cpp)

    # The Custom Series list is derived from DataProcessorService, not accumulated
    # from plugin announcements — the paths that announce nothing are a layout
    # restore and a withdrawal through the data-processors ABI.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_custom_series_test.cpp)

    # Viewport re-frame after a layout's timeline offsets settle: sync leg (FIX 1)
    # and the progressive-drain pending-timeline seam (FIX 2).
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_viewport_reframe_test.cpp)

    # SDK 0.28 owned views: tab ids stay stable across renames and reordering,
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

    # Layout-import batch restore (T6), with isolated ctest processes:
    #   alive  — a materialize MISS import keeps the restore alive across the
    #            catalog-empty guard; curves bind at the batch-gated drain.
    #   policy — D5: a non-interactive restore opens zero dialogs; unresolved
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
    add_executable(main_window_layout_import_cancel_test tests/main_window_layout_import_cancel_test.cpp)
    target_include_directories(main_window_layout_import_cancel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(main_window_layout_import_cancel_test PRIVATE pj_app_shell pj_resources GTest::gtest)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(main_window_layout_import_cancel_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME MainWindowLayoutImportCancelTest COMMAND main_window_layout_import_cancel_test)
    #   lifecycle — Codex r1 F2/F3/F4/F6: explicit progressive policy on a
    #              mixed automated restore, apply-failure diagnostics, full
    #              supersession by a sync-path load, and automated plain-source
    #              failure dialog suppression (with the interactive pin).
    #              Stages the mock source plugin so plain replays load/fail
    #              for real.
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
    #   binder — T7 (D2/D4/D6/D8/D9): the ingest strip's displayed ownership +
    #            batch-scoped SessionManager observation, the batch job's
    #            keep-partial Stop route, #498 coexistence (no panel folds
    #            from headless jobs; busy pinned panels survive the batch
    #            drain), and the D6 real-surface legs — mid-import curve
    #            binding, on_dataset/ingestBegan ordering, and the real
    #            promotion transaction (mock source plugin staged for it).
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

    #   settle — stage-5 E5: layoutRestoreSettled fires exactly once per load
    #            terminal (open-failure false + dump composition, plain sync
    #            true, batch true only at batch finish — never mid-import).
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/main_window_layout_settle_test.cpp)
    #   e2e    — stage-5 E1(a): the LIVE cross-repo gui-test — real MainWindow,
    #            REAL plugin DSOs (mcap-cloud + mcap-loader + ros-parser) from
    #            $MCAP_CLOUD_E2E_EXTENSIONS, live server at $MCAP_CLOUD_E2E_URL,
    #            frozen descriptor vectors from $MCAP_CLOUD_E2E_VECTORS. SKIPs
    #            cleanly (all five scenarios) when that env is absent; the
    #            e2e-layout-import.sh harness provides it and treats SKIP as
    #            failure (smoke's pattern).
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

    # --dump-diagnostics collector (stage-5 E2): full-run accumulation from the
    # queued bridge (incl. pre-attach records), the stable five-field JSON
    # shape, and no truncation at the DiagnosticHistory ring cap. No MainWindow
    # is constructed; the runner links the shell for DiagnosticDump + the
    # bridge and supplies the Qt event loop.
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

    # Test-only DataSource plugin that calls the runtime host's message box from
    # the import worker thread (guards the cross-thread-QMessageBox regression;
    # also records its askContinue outcome to PJ_MSGBOX_PROBE_FILE so a test can
    # observe the shutdown gate's -1 answer). Built like the SDK's mock plugins:
    # a SHARED lib linking only pj_base.
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
    # Drives FileLoader's shutdown-while-suspended-at-the-plugin-dialog path so the
    # reject-before-destroy ordering (the UAF guard) can be asserted deterministically.
    # Links plugin_sdk for the DialogPluginTyped/WidgetData symbols.
    add_library(dialog_probe_source_plugin SHARED tests/dialog_probe_source.cpp)
    target_compile_features(dialog_probe_source_plugin PRIVATE cxx_std_20)
    target_link_libraries(dialog_probe_source_plugin PRIVATE plotjuggler_sdk::plugin_sdk)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(dialog_probe_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
    endif()

    # Shared scaffolding for the standalone FileLoader-pipeline suites
    # (file_loader_test, source_promotion_host_test,
    # headless_descriptor_provider_session_test): the loader TUs compiled ONCE
    # and reused by all three binaries (FileLoader has no MainWindow dependency
    # by design — see setFilePicker). The one-binary-per-suite pattern stays;
    # per-suite compile definitions (mock-plugin paths) live on each
    # executable. Their plugin-path definitions differ, so they cannot share
    # a runner. The header-side twin is tests/support/loader_test_support.h.
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

    # End-to-end FileLoader pipeline test (same-file reload semantics + the
    # worker-thread message-box marshal regression). Ingests through the SDK's
    # mock_file_source_plugin (and the local msgbox_mock_source_plugin), staged
    # into a temp extensions dir at runtime.
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

    # Headless per-batch owner of ONE descriptor-import provider toolbox
    # instance (T5b): manifest-id resolution, bind, extension view, promotion
    # service, queued-only callback marshal, and the quiescence teardown pin.
    # Driven against a fake provider toolbox registered statically (no DSO);
    # same standalone shape as source_promotion_host_test above.
    add_executable(headless_descriptor_provider_session_test
        tests/headless_descriptor_provider_session_test.cpp
    )
    set_target_properties(headless_descriptor_provider_session_test PROPERTIES AUTOMOC ON)
    target_link_libraries(headless_descriptor_provider_session_test PRIVATE pj_app_loader_testlib)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(headless_descriptor_provider_session_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME HeadlessDescriptorProviderSessionTest COMMAND headless_descriptor_provider_session_test)

    # The §6.2 rewrite-then-classify restore coordinator (T6): classify
    # matrix, trust gate, guard-3 ceiling, provider-absent fallback,
    # sequential job chaining, cancel/rollback. Driven against a
    # descriptor-scripted fake provider toolbox (registerStaticToolbox) plus
    # the real FileLoader + mock_file_source_plugin for stock cache-hit loads.
    # Desktop-only: the fixture stands up the M3 source cache
    # (SourceCacheStore/SourceCaptureService refuse to compile in the
    # browser), same gate as pj_runtime's capture tests.
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

    # Binds the Source Timeline widget to the runtime; the regression test pins that
    # the bars track data growth (samplesIngested), not just new catalog topics.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/source_timeline_controller_test.cpp)

    # Reuse the shell's controller objects and meta-objects for timer and
    # progress-row tests, alongside the other runtime-backed GUI cases.
    list(APPEND PJ_APP_GUI_TEST_SOURCES
        tests/ingest_progress_controller_test.cpp)

    # WARNINGS: all 22 merged tests carried PJ_WARNING_FLAGS before
    pj_add_test_runner(pj_app_gui_tests
        MAIN gui WARNINGS
        SOURCES ${PJ_APP_GUI_TEST_SOURCES} tests/support/gui_test_env.cpp
        # Qt6::Test: QTest::qWaitForWindowExposed, for the drawer-splitter cases in
        # toolbox_panel_fold_test that need a real window-system resize event.
        LIBS pj_app_shell pj_resources Qt6::Test
    )
    set_target_properties(pj_app_gui_tests PROPERTIES AUTOMOC ON)
endif()
