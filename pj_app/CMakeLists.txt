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
    src/ui/DiagnosticsCard.cpp
    src/ui/DiagnosticsCard.h
    src/ui/DiagnosticsCard.ui
    src/ui/DiagnosticsPopup.cpp
    src/ui/DiagnosticsPopup.h
    src/ui/DiagnosticsPopup.ui
    src/ui/DiagnosticsDetailDialog.cpp
    src/ui/DiagnosticsDetailDialog.h
    src/ui/DiagnosticsDetailDialog.ui
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
        target_link_libraries(pj_app_shell PUBLIC pj_dialog_sdk)
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
    ${CMAKE_SOURCE_DIR}/raster_ipc
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
    add_executable(pj_app
        src/main.cpp
        ${CMAKE_SOURCE_DIR}/resources/resources.qrc)
endif()
set_target_properties(pj_app PROPERTIES
    # The CMake target id stays `pj_app` (internal module name), but the installed
    # binary is `plotjuggler4` — the user-facing command and what packaging ships
    # (e.g. /usr/bin/plotjuggler4). Keeps the source module rename a separate concern.
    OUTPUT_NAME plotjuggler4
    AUTORCC ON
)
target_include_directories(pj_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pj_app PRIVATE
    pj_app_shell
    $<$<NOT:$<BOOL:${EMSCRIPTEN}>>:Backward::Backward>)

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
    set_target_properties(pj_app PROPERTIES QT_WASM_INITIAL_MEMORY "256MB")
    target_compile_options(pj_app PRIVATE -fexceptions)
    target_link_options(pj_app PRIVATE -fexceptions -sALLOW_MEMORY_GROWTH=1)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
    find_package(GTest REQUIRED)

    add_executable(browser_file_store_test
        tests/browser_file_store_test.cpp
        src/BrowserFileStore.cpp
        src/BrowserFileStore.h
        src/LoadInput.h)
    target_include_directories(browser_file_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(browser_file_store_test PRIVATE Qt6::Core GTest::gtest_main)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(browser_file_store_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME BrowserFileStoreTest COMMAND browser_file_store_test)

    add_executable(browser_persistence_test
        tests/browser_persistence_test.cpp
        src/BrowserPersistence.cpp
        src/BrowserPersistence.h)
    target_include_directories(browser_persistence_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(browser_persistence_test PRIVATE Qt6::Core Qt6::Xml GTest::gtest_main)
    target_compile_options(browser_persistence_test PRIVATE ${PJ_WARNING_FLAGS})
    add_test(NAME BrowserPersistenceTest COMMAND browser_persistence_test)

    add_executable(file_selection_service_test
        tests/file_selection_service_test.cpp
        src/BrowserFileStore.cpp
        src/FileSelectionService.cpp
        src/FileSelectionService.h)
    target_include_directories(file_selection_service_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(file_selection_service_test PRIVATE Qt6::Widgets GTest::gtest_main)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(file_selection_service_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME FileSelectionServiceTest COMMAND file_selection_service_test)

    # Guards the meme splash pool against its two silent failure modes: a meme
    # present in resources/memes/ but absent from resources.qrc is invisible at
    # runtime (rcc has no globbing), and an oversized meme must be bounded on
    # both axes, not just width. Compiles resources.qrc so the test inspects the
    # real shipped pool, and is handed the source directory to compare against.
    add_executable(splash_meme_pool_test
        tests/splash_meme_pool_test.cpp
        src/Splashscreen.cpp
        src/Splashscreen.h
        ${CMAKE_SOURCE_DIR}/resources/resources.qrc)
    set_target_properties(splash_meme_pool_test PROPERTIES AUTORCC ON)
    target_include_directories(splash_meme_pool_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(splash_meme_pool_test PRIVATE
        PJ_MEMES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/memes")
    target_link_libraries(splash_meme_pool_test PRIVATE pj_widgets Qt6::Widgets Qt6::Svg GTest::gtest)
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

    # A GUI test binary that drives the real MainWindow through a friend peer: it
    # links the whole shell (pj_app_shell) and compiles resources.qrc (AUTORCC) so
    # the window's icons resolve. One MainWindow per binary — its dtor leaks
    # process state — hence one target (and one add_test) per scenario file.
    #   target   the executable + CTest command name
    #   test     the human-readable CTest NAME
    #   source   the test's .cpp under tests/
    function(add_pj_app_gui_test target test source)
        add_executable(${target}
            ${source}
            ${CMAKE_SOURCE_DIR}/resources/resources.qrc
        )
        set_target_properties(${target} PROPERTIES AUTORCC ON)
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(${target} PRIVATE pj_app_shell GTest::gtest)
        if(DEFINED PJ_WARNING_FLAGS)
            target_compile_options(${target} PRIVATE ${PJ_WARNING_FLAGS})
        endif()
        add_test(NAME ${test} COMMAND ${target})
    endfunction()

    add_pj_app_gui_test(slider_states_test SliderStates tests/slider_states_test.cpp)

    # MR !261 depth guard: a source added while streaming is paused must write
    # into the secondary tail buffer, not the frozen primary. Exercises the
    # extracted pause-target seam (applyWriteTargetsForCurrentPauseState) headlessly.
    add_pj_app_gui_test(streaming_pause_target_test StreamingPauseTargetTest
        tests/streaming_pause_target_test.cpp)

    # Curve-list drag policy: an object topic no view can display (CameraInfo,
    # ...) must be drag-inert with a "why" tooltip; displayable ones keep
    # dragging.
    add_pj_app_gui_test(curve_list_panel_test CurveListPanelTest tests/curve_list_panel_test.cpp)

    # Source-bound layout seam: one replayable file that fans out into several
    # datasets must persist/restore each independent timeline track.
    add_pj_app_gui_test(main_window_source_layout_test MainWindowSourceLayoutTest
        tests/main_window_source_layout_test.cpp)

    # Fan-out timeline matcher: when a saved same-named fan-out shape no longer
    # matches the loaded one, no offset may bind (FIX 4).
    add_pj_app_gui_test(main_window_fanout_ambiguous_test MainWindowFanoutAmbiguousTest
        tests/main_window_fanout_ambiguous_test.cpp)

    # A layout's marker generators are replayed — and their scripts run — on a
    # session where no toolbox panel was ever opened.
    add_pj_app_gui_test(main_window_marker_generator_restore_test MainWindowMarkerGeneratorRestoreTest
        tests/main_window_marker_generator_restore_test.cpp)

    # Viewport re-frame after a layout's timeline offsets settle: sync leg (FIX 1)
    # and the progressive-drain pending-timeline seam (FIX 2).
    add_pj_app_gui_test(main_window_viewport_reframe_test MainWindowViewportReframeTest
        tests/main_window_viewport_reframe_test.cpp)

    # Transactional workspace history. One MainWindow is shared by all cases in
    # this binary; the shell leaks process-global widget state across instances.
    add_pj_app_gui_test(main_window_history_test MainWindowHistoryTest
        tests/main_window_history_test.cpp)

    # Layout-import batch restore (T6), one scenario per binary:
    #   alive  — a materialize MISS import keeps the restore alive across the
    #            catalog-empty guard; curves bind at the batch-gated drain.
    #   policy — D5: a non-interactive restore opens zero dialogs; unresolved
    #            intents are retained + diagnosed, never cleared.
    #   cancel — cancel mid-import rolls back to the pre-layout workspace.
    add_pj_app_gui_test(main_window_layout_import_alive_test MainWindowLayoutImportAliveTest
        tests/main_window_layout_import_alive_test.cpp)
    add_pj_app_gui_test(main_window_layout_import_policy_test MainWindowLayoutImportPolicyTest
        tests/main_window_layout_import_policy_test.cpp)
    add_pj_app_gui_test(main_window_layout_import_cancel_test MainWindowLayoutImportCancelTest
        tests/main_window_layout_import_cancel_test.cpp)
    #   lifecycle — Codex r1 F2/F3/F4/F6: explicit progressive policy on a
    #              mixed automated restore, apply-failure diagnostics, full
    #              supersession by a sync-path load, and automated plain-source
    #              failure dialog suppression (with the interactive pin).
    #              Stages the mock source plugin so plain replays load/fail
    #              for real.
    add_pj_app_gui_test(main_window_layout_import_lifecycle_test MainWindowLayoutImportLifecycleTest
        tests/main_window_layout_import_lifecycle_test.cpp)
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
    add_pj_app_gui_test(main_window_layout_import_binder_test MainWindowLayoutImportBinderTest
        tests/main_window_layout_import_binder_test.cpp)
    target_compile_definitions(main_window_layout_import_binder_test PRIVATE
        PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>"
    )
    add_dependencies(main_window_layout_import_binder_test mock_file_source_plugin)

    #   settle — stage-5 E5: layoutRestoreSettled fires exactly once per load
    #            terminal (open-failure false + dump composition, plain sync
    #            true, batch true only at batch finish — never mid-import).
    add_pj_app_gui_test(main_window_layout_settle_test MainWindowLayoutSettleTest
        tests/main_window_layout_settle_test.cpp)
    #   e2e    — stage-5 E1(a): the LIVE cross-repo gui-test — real MainWindow,
    #            REAL plugin DSOs (mcap-cloud + mcap-loader + ros-parser) from
    #            $MCAP_CLOUD_E2E_EXTENSIONS, live server at $MCAP_CLOUD_E2E_URL,
    #            frozen descriptor vectors from $MCAP_CLOUD_E2E_VECTORS. SKIPs
    #            cleanly (all five scenarios) when that env is absent; the
    #            e2e-layout-import.sh harness provides it and treats SKIP as
    #            failure (smoke's pattern).
    add_pj_app_gui_test(main_window_layout_import_e2e_test MainWindowLayoutImportE2ETest
        tests/main_window_layout_import_e2e_test.cpp)
    #   exit   — LayoutExitWatch (the --exit-after-layout exit-code decision):
    #            load-failed/success/timeout mapping, decide-once latching,
    #            and the parked-batch teardown staying crash-free.
    add_pj_app_gui_test(main_window_layout_exit_test MainWindowLayoutExitTest
        tests/main_window_layout_exit_test.cpp)

    # --dump-diagnostics collector (stage-5 E2): full-run accumulation from the
    # queued bridge (incl. pre-attach records), the stable five-field JSON
    # shape, and no truncation at the DiagnosticHistory ring cap. No MainWindow
    # is constructed; the target links the shell for DiagnosticDump + the
    # bridge and supplies its own QCoreApplication main.
    add_pj_app_gui_test(diagnostic_dump_test DiagnosticDumpTest tests/diagnostic_dump_test.cpp)

    # The left column's width belongs to the user: neither a window resize nor a
    # chart-area takeover may move the separator they dragged.
    add_pj_app_gui_test(main_window_panel_geometry_test MainWindowPanelGeometryTest
        tests/main_window_panel_geometry_test.cpp)

    # A toolbox panel with a download in flight folds into a background tab
    # instead of being destroyed: the banner X, the folded tab's own close, the
    # auto-fold on import start, layout-save exclusion, and completion-close
    # suppression.
    add_pj_app_gui_test(toolbox_panel_fold_test ToolboxPanelFoldTest
        tests/toolbox_panel_fold_test.cpp)

    # Pure config-parsing logic (no GUI), compiled standalone so it's linkable
    # from a plain gtest binary. FanoutConfig.cpp is also part of pj_app itself.
    add_executable(fanout_config_test
        tests/fanout_config_test.cpp
        src/FanoutConfig.cpp
    )
    target_include_directories(fanout_config_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(fanout_config_test PRIVATE
        Qt6::Core
        GTest::gtest_main
    )
    add_test(NAME FanoutConfig COMMAND fanout_config_test)

    # LayoutXml is a self-contained DOM helper module (no Qt::Widgets, no
    # MainWindow) so we compile the source directly into the test target
    # instead of linking against the pj_app executable.
    add_executable(layout_xml_test
        tests/layout_xml_test.cpp
        src/LayoutXml.cpp
        src/LayoutXml.h
    )
    target_include_directories(layout_xml_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(layout_xml_test PRIVATE
        Qt6::Core
        Qt6::Xml
        GTest::gtest_main
    )
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(layout_xml_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME LayoutXmlTest COMMAND layout_xml_test)

    add_executable(pending_display_binder_test
        tests/pending_display_binder_test.cpp
        tests/dataset_test_helpers.h
        src/PendingDisplayBinder.cpp
        src/PendingDisplayBinder.h
        src/LayoutXml.cpp
        src/LayoutXml.h
    )
    target_include_directories(pending_display_binder_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(pending_display_binder_test PRIVATE
        pj_runtime
        pj_plotting
        pj_scene_common
        Qt6::Widgets
        Qt6::Xml
        GTest::gtest
    )
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(pending_display_binder_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME PendingDisplayBinderTest COMMAND pending_display_binder_test)

    add_executable(multi_dataset_rebind_test
        tests/multi_dataset_rebind_test.cpp
        tests/dataset_test_helpers.h
        src/PendingDisplayBinder.cpp
        src/PendingDisplayBinder.h
        src/LayoutXml.cpp
        src/LayoutXml.h
    )
    target_include_directories(multi_dataset_rebind_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(multi_dataset_rebind_test PRIVATE
        pj_runtime
        pj_plotting
        pj_scene_common
        Qt6::Widgets
        Qt6::Xml
        GTest::gtest
    )
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(multi_dataset_rebind_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME MultiDatasetRebindTest COMMAND multi_dataset_rebind_test)

    add_executable(topic_demand_controller_test
        tests/topic_demand_controller_test.cpp
        src/PendingDisplayBinder.cpp
        src/PendingDisplayBinder.h
        src/TopicDemandController.cpp
        src/TopicDemandController.h
        src/LayoutXml.cpp
        src/LayoutXml.h
    )
    set_target_properties(topic_demand_controller_test PROPERTIES AUTOMOC ON)
    target_include_directories(topic_demand_controller_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(topic_demand_controller_test PRIVATE
        pj_runtime
        pj_plotting
        pj_scene_common
        pj_scripting
        Qt6::Widgets
        Qt6::Xml
        GTest::gtest
    )
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(topic_demand_controller_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME TopicDemandControllerTest COMMAND topic_demand_controller_test)

    add_executable(key_sequence_matcher_test
        tests/key_sequence_matcher_test.cpp
        src/KeySequence.cpp
    )
    set_target_properties(key_sequence_matcher_test PROPERTIES AUTOMOC ON)
    target_include_directories(key_sequence_matcher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(key_sequence_matcher_test PRIVATE
        Qt6::Core
        Qt6::Gui
        GTest::gtest_main
    )
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(key_sequence_matcher_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME KeySequenceMatcher COMMAND key_sequence_matcher_test)

    add_executable(key_sequence_watcher_test
        tests/key_sequence_watcher_test.cpp
        src/KeySequence.cpp
    )
    set_target_properties(key_sequence_watcher_test PROPERTIES AUTOMOC ON)
    target_include_directories(key_sequence_watcher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(key_sequence_watcher_test PRIVATE
        Qt6::Core
        Qt6::Gui
        GTest::gtest
    )
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(key_sequence_watcher_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME KeySequenceWatcher COMMAND key_sequence_watcher_test)

    add_executable(raster_ipc_protocol_test tests/raster_ipc_protocol_test.cpp)
    target_include_directories(raster_ipc_protocol_test PRIVATE ${CMAKE_SOURCE_DIR}/raster_ipc)
    target_link_libraries(raster_ipc_protocol_test PRIVATE GTest::gtest_main)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(raster_ipc_protocol_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME RasterIpcProtocol COMMAND raster_ipc_protocol_test)

    add_executable(raster_key_map_test
        tests/raster_key_map_test.cpp
        src/RasterKeyMap.cpp
    )
    target_include_directories(raster_key_map_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(raster_key_map_test PRIVATE Qt6::Core GTest::gtest_main)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(raster_key_map_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME RasterKeyMap COMMAND raster_key_map_test)

    # Test-only DataSource plugin that calls the runtime host's message box from
    # the import worker thread (guards the cross-thread-QMessageBox regression;
    # also records its askContinue outcome to PJ_MSGBOX_PROBE_FILE so a test can
    # observe the shutdown gate's -1 answer). Built like the SDK's mock plugins:
    # a SHARED lib linking only pj_base.
    add_library(msgbox_mock_source_plugin SHARED tests/msgbox_mock_source.cpp)
    target_compile_features(msgbox_mock_source_plugin PRIVATE cxx_std_20)
    target_link_libraries(msgbox_mock_source_plugin PRIVATE pj_base)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(msgbox_mock_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
    endif()

    # Test-only DataSource plugin that deterministically leaves a single-load
    # progress metacall queued while FileLoader joins its worker for shutdown.
    add_library(progress_shutdown_source_plugin SHARED tests/progress_shutdown_source.cpp)
    target_compile_features(progress_shutdown_source_plugin PRIVATE cxx_std_20)
    target_link_libraries(progress_shutdown_source_plugin PRIVATE pj_base)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(progress_shutdown_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
    endif()

    # Test-only DataSource plugin with an embedded config dialog (kCapabilityHasDialog).
    # Drives FileLoader's shutdown-while-suspended-at-the-plugin-dialog path so the
    # reject-before-destroy ordering (the UAF guard) can be asserted deterministically.
    # Links pj_dialog_sdk for the DialogPluginTyped/WidgetData symbols.
    add_library(dialog_probe_source_plugin SHARED tests/dialog_probe_source.cpp)
    target_compile_features(dialog_probe_source_plugin PRIVATE cxx_std_20)
    target_link_libraries(dialog_probe_source_plugin PRIVATE pj_base pj_dialog_sdk)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(dialog_probe_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
    endif()

    # Shared scaffolding for the standalone FileLoader-pipeline suites
    # (file_loader_test, source_promotion_host_test,
    # headless_descriptor_provider_session_test): the loader TUs compiled ONCE
    # and reused by all three binaries (FileLoader has no MainWindow dependency
    # by design — see setFilePicker). The one-binary-per-suite pattern stays;
    # per-suite compile definitions (mock-plugin paths) live on each
    # executable. The header-side twin is tests/support/loader_test_support.h.
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
        $<$<TARGET_EXISTS:pj_scene3d_widgets>:pj_scene3d_widgets>
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
    add_executable(layout_import_batch_test
        tests/layout_import_batch_test.cpp
        src/LayoutImportBatch.cpp
        src/LayoutImportBatch.h
    )
    set_target_properties(layout_import_batch_test PROPERTIES AUTOMOC ON)
    target_compile_definitions(layout_import_batch_test PRIVATE
        PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>"
    )
    # $<TARGET_FILE:...> in a compile definition does not create a build-order
    # dependency; the plugin must exist when the test runs.
    add_dependencies(layout_import_batch_test mock_file_source_plugin)
    target_link_libraries(layout_import_batch_test PRIVATE pj_app_loader_testlib)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(layout_import_batch_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME LayoutImportBatchTest COMMAND layout_import_batch_test)

    # Binds the Source Timeline widget to the runtime; the regression test pins that
    # the bars track data growth (samplesIngested), not just new catalog topics.
    add_executable(source_timeline_controller_test
        tests/source_timeline_controller_test.cpp
        src/SourceTimelineController.cpp
        src/SourceTimelineController.h
        src/DatasetMergeActions.cpp
        src/DatasetMergeActions.h
    )
    set_target_properties(source_timeline_controller_test PROPERTIES AUTOMOC ON)
    target_include_directories(source_timeline_controller_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(source_timeline_controller_test PRIVATE
        pj_runtime
        pj_widgets
        Qt6::Widgets
        GTest::gtest_main
    )
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(source_timeline_controller_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME SourceTimelineControllerTest COMMAND source_timeline_controller_test)
endif()
