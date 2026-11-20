// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QMainWindow>
#include <QPixmap>
#include <QScreen>
#include <QSettings>
#include <QSplashScreen>
#include <QStringList>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <Qt>
#ifndef PJ_TARGET_WASM
#include <DockManager.h>
#include <DockWidget.h>

#include <backward.hpp>
#else
#include <emscripten.h>
#include <emscripten/val.h>
#endif
#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#ifndef PJ_TARGET_WASM
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#endif
#include <memory>
#include <string>

#include "AppLanguage.h"
#include "BrowserPersistence.h"
#include "BundledFont.h"
#include "DebugMode.h"
#ifndef PJ_TARGET_WASM
#include "DiagnosticDump.h"
#endif
#include "KeySequence.h"
#ifndef PJ_TARGET_WASM
#include "LayoutExitWatch.h"
#endif
#include "MainWindow.h"
#include "Splashscreen.h"
#include "WidgetTuner.h"
#ifndef PJ_TARGET_WASM
#include "mcp/McpProtocol.h"
#endif
#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/processor_detail.hpp"
#include "pj_plotting/PlotWidgetBase.h"
#ifndef PJ_TARGET_WASM
#include "pj_runtime/PluginRuntimeCatalog.h"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/python_engine.h"
#endif
#include "pj_scripting/python_object_script.h"
#include "pj_widgets/GraphicsApi.h"
#ifdef PJ_WITH_SCENE3D
#include "pj_scene3d_widgets/scene_view_widget.h"
#endif
#include "pj_version.h"
#include "pj_widgets/Style.h"
#if defined(_WIN32)
#include <windows.h>  // backward.hpp already pulls it in (with NOMINMAX)
#endif
using namespace Qt::StringLiterals;

namespace {

#if defined(_WIN32)
// Ctrl+C / Ctrl+Break in the attached terminal would otherwise end the process at once, skipping MainWindow's
// close handling. Runs on a thread the console creates, hence the queued call.
BOOL WINAPI onConsoleInterrupt(DWORD event) {
  if ((event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) || qApp == nullptr) {
    return FALSE;
  }
  QMetaObject::invokeMethod(qApp, [] { QApplication::closeAllWindows(); }, Qt::QueuedConnection);
  return TRUE;
}

// A GUI-subsystem exe has no console: attach to the launching terminal's, so CLI output and Qt's log (stderr
// only while a console is attached) appear there. A stream the launcher redirected is kept. Must run before the
// first Qt log message: Qt caches the console check.
void attachParentConsole() {
  const auto is_unset = [](DWORD which) {
    const HANDLE handle = GetStdHandle(which);
    return handle == nullptr || handle == INVALID_HANDLE_VALUE;
  };
  const bool reopen_stdout = is_unset(STD_OUTPUT_HANDLE);
  const bool reopen_stderr = is_unset(STD_ERROR_HANDLE);
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
    return;
  }
  SetConsoleCtrlHandler(onConsoleInterrupt, TRUE);
  FILE* reopened = nullptr;
  if (reopen_stdout) {
    freopen_s(&reopened, "CONOUT$", "w", stdout);
  }
  if (reopen_stderr) {
    freopen_s(&reopened, "CONOUT$", "w", stderr);
  }
}
#endif

// The crash handler: at static init it installs signal handlers that print a symbolized stack trace. Defined here
// because the linker drops backward-cpp's own global from the static archive when nothing references it;
// backward-cpp wants exactly one per program. Left out of the sanitizer lanes (pj_app/CMakeLists.txt says why).
#ifndef PJ_TARGET_WASM
#ifndef PJ_NO_CRASH_HANDLER
backward::SignalHandling g_crash_handler;
#endif

// Boots the embedded Python backend the way a Data Processor does, inside the packaged binary: nothing else in a
// release flow launches it, so a backend that cannot reach its own stdlib would ship undetected.
int selftestPython() {
  constexpr const char* kSource = R"PY(# pj-script: python
import math

class T:
    id = "selftest"
    name = "Selftest"
    output = "double"
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        return math.sqrt(value)
)PY";

  const PJ::scripting::FilterCatalogue catalogue(PJ::scripting::makePythonEngine());
  auto processor = catalogue.makeProcessorFromSource(kSource, "selftest", "{}");
  if (!processor.has_value()) {
    std::fprintf(stderr, "[selftest-python] FAILED: %s\n", processor.error().c_str());
    return EXIT_FAILURE;
  }
  const auto out = (*processor)->calculateNextPoint(PJ::proc::Sample::scalar(0, PJ::VarValue{144.0}));
  if (!out.has_value() || PJ::proc::detail::toDouble(out->value()) != 12.0) {
    std::fprintf(stderr, "[selftest-python] FAILED: sqrt(144) did not yield 12\n");
    return EXIT_FAILURE;
  }
  // Packaging must not hand the bundle back the full stdlib: silently regaining
  // `os` is as much a regression as losing `math`.
  if (catalogue.makeProcessorFromSource("# pj-script: python\nimport os\n", "selftest", "{}").has_value()) {
    std::fprintf(stderr, "[selftest-python] FAILED: 'import os' was accepted\n");
    return EXIT_FAILURE;
  }
  std::fprintf(stdout, "[selftest-python] OK\n");
  return EXIT_SUCCESS;
}

// Checks the linked docking library (the 3rdparty fork, or a system one under PJ_SYSTEM_ADS) keeps a hidden tab
// parented (the fork's "keep inactive tabs parented" patch). Upstream ADS unparents it, so a QRhiWidget tab rebuilds
// its GPU resources on every tab switch; nothing else would catch a system ADS without the patch.
int selftestDocking(int argc, char* argv[]) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  QMainWindow window;
  auto* manager = new ads::CDockManager(&window);
  auto* first = new ads::CDockWidget(manager, u"first"_s);
  first->setWidget(new QWidget());
  auto* second = new ads::CDockWidget(manager, u"second"_s);
  second->setWidget(new QWidget());
  auto* area = manager->addDockWidget(ads::CenterDockWidgetArea, first);
  manager->addDockWidgetTabToArea(second, area);
  window.show();
  first->setAsCurrentTab();
  QApplication::processEvents();
  if (second->parentWidget() == nullptr || second->window() != &window) {
    std::fprintf(
        stderr,
        "[selftest-docking] FAILED: the hidden dock tab lost its parent (ADS without the fork's tab-parenting "
        "patch)\n");
    return EXIT_FAILURE;
  }
  std::fprintf(stdout, "[selftest-docking] OK\n");
  return EXIT_SUCCESS;
}

int validatePlugins(const QString& plugin_dir, const QStringList& expected_specs) {
  if (expected_specs.isEmpty()) {
    std::fprintf(stderr, "[plugin-validation] no --expect-plugin values were provided\n");
    return EXIT_FAILURE;
  }

  bool saw_error = false;
  PJ::PluginRuntimeCatalog catalog({}, [&](const PJ::Diagnostic& diagnostic) {
    const char* level = "info";
    if (diagnostic.level == PJ::DiagnosticLevel::kError) {
      level = "error";
      saw_error = true;
    } else if (diagnostic.level == PJ::DiagnosticLevel::kWarning) {
      level = "warning";
    }
    std::fprintf(
        diagnostic.level == PJ::DiagnosticLevel::kError ? stderr : stdout, "[plugin-validation][%s] %s%s%s\n", level,
        diagnostic.id.c_str(), diagnostic.id.empty() ? "" : ": ", diagnostic.message.c_str());
  });

  catalog.setPluginDir(std::filesystem::path(plugin_dir.toStdU16String()));
  catalog.scanDirectory();

  std::map<std::string, std::string> loaded;
  const auto record_loaded = [&](const auto& plugins) {
    for (const auto& plugin : plugins) {
      const bool inserted = loaded.emplace(plugin.id, plugin.version).second;
      if (!inserted) {
        saw_error = true;
        std::fprintf(stderr, "[plugin-validation] duplicate loaded plugin id: %s\n", plugin.id.c_str());
      }
    }
  };
  record_loaded(catalog.dataSources());
  record_loaded(catalog.messageParsers());
  record_loaded(catalog.toolboxes());
  record_loaded(catalog.widgets());

  std::map<std::string, std::string> expected;
  for (const QString& spec : expected_specs) {
    const qsizetype separator = spec.indexOf(u'=');
    if (separator <= 0 || separator == spec.size() - 1) {
      saw_error = true;
      std::fprintf(stderr, "[plugin-validation] invalid --expect-plugin value: %s\n", qPrintable(spec));
      continue;
    }
    const std::string id = spec.first(separator).toStdString();
    const std::string version = spec.sliced(separator + 1).toStdString();
    if (!expected.emplace(id, version).second) {
      saw_error = true;
      std::fprintf(stderr, "[plugin-validation] duplicate expected plugin id: %s\n", id.c_str());
    }
  }

  for (const auto& [id, version] : expected) {
    const auto found = loaded.find(id);
    if (found == loaded.end()) {
      saw_error = true;
      std::fprintf(stderr, "[plugin-validation] expected plugin did not load: %s=%s\n", id.c_str(), version.c_str());
    } else if (found->second != version) {
      saw_error = true;
      std::fprintf(
          stderr, "[plugin-validation] version mismatch for %s: expected %s, loaded %s\n", id.c_str(), version.c_str(),
          found->second.c_str());
    }
  }
  for (const auto& [id, version] : loaded) {
    if (!expected.contains(id)) {
      saw_error = true;
      std::fprintf(
          stderr, "[plugin-validation] loaded plugin is not whitelisted: %s=%s\n", id.c_str(), version.c_str());
    }
  }

  if (saw_error || loaded.size() != expected.size()) {
    std::fprintf(
        stderr, "[plugin-validation] FAILED: loaded %zu plugin(s), expected %zu\n", loaded.size(), expected.size());
    return EXIT_FAILURE;
  }
  std::printf("[plugin-validation] OK: loaded all %zu whitelisted plugin(s)\n", loaded.size());
  return EXIT_SUCCESS;
}

// Nobody can answer a question at startup: --exit-after-layout, --screenshot, --dump-diagnostics (the headless
// acceptance flags), or a platform without a screen. Such a run reports instead of asking.
bool isUnattendedRun(
    const QCommandLineParser& parser, const QCommandLineOption& exit_after_layout_option,
    const QCommandLineOption& screenshot_option, const QCommandLineOption& dump_diagnostics_option) {
  const QString platform = QGuiApplication::platformName();
  return parser.isSet(exit_after_layout_option) || parser.isSet(screenshot_option) ||
         parser.isSet(dump_diagnostics_option) || platform == u"offscreen"_s || platform == u"minimal"_s;
}
#endif
}  // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
  attachParentConsole();
#endif
#ifndef PJ_TARGET_WASM
  // Before any QApplication, which would demand a platform plugin and a display the CI containers lack.
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--selftest-python") == 0) {
      return selftestPython();
    }
    if (std::strcmp(argv[i], "--selftest-docking") == 0) {
      return selftestDocking(argc, argv);
    }
    if (std::strcmp(argv[i], "--mcp-bridge") == 0) {
      return PJ::mcp::runBridge(argc, argv, QStringLiteral(PJ_VERSION_STRING));
    }
  }
#endif

  // Before QApplication, so no widget reads the platform's native style (Breeze, Adwaita), which overrides QSS on
  // QMenu and other popups.
  QApplication::setStyle(new PJ::Style(u"Fusion"_s));

  // ADS calls winId() on every floating dock container, a window parented to the dock manager, and Qt then makes
  // all the manager's children native. Under Vulkan on Wayland that child surface stops getting frame callbacks,
  // so the docks stop repainting (or crash in the swapchain) once any dock floats.
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
#if defined(Q_OS_MACOS)
  // macOS gives an OpenGL QRhi a legacy 2.1 context unless the window asks for core, and the shader packs need
  // GLSL 330+. Metal ignores it.
  QSurfaceFormat core_format = QSurfaceFormat::defaultFormat();
  core_format.setVersion(4, 1);
  core_format.setProfile(QSurfaceFormat::CoreProfile);
  QSurfaceFormat::setDefaultFormat(core_format);
#endif
  QApplication app(argc, argv);
  // QApplication adopts the user's locale for the C library too. Luau and the plugins use strtod / printf and expect
  // a decimal point, so a decimal-comma LC_NUMERIC would stop every script from compiling. QLocale is unaffected.
  std::setlocale(LC_NUMERIC, "C");
#ifdef PJ_TARGET_WASM
  // Qt 6.12 (QTBUG-144692) prevents a pointerdown's default only when a widget accepts the press. On any other press
  // (a rectangle zoom on a plot, for one) the browser moves focus to <body>, and keys stop reaching Qt until the focus
  // object changes. Upstream 3a3021f80f prevents every pointerdown on the window instead; remove with Qt >= 6.13.
  // clang-format off
  EM_ASM({
    document.addEventListener('pointerdown', event => {
      const target = event.composedPath()[0];
      if (target instanceof Element && target.classList.contains('qt-window')) {
        event.preventDefault();
      }
    }, true);
  });
  // clang-format on
#endif
#if defined(Q_OS_MACOS)
  // The bundle carries the embedded CPython's stdlib at Contents/Resources/python (packaging/macos); the build-time
  // prefix is absent on the user's Mac. Python starts lazily, after this; a user PYTHONHOME wins.
  if (qEnvironmentVariableIsEmpty("PYTHONHOME")) {
    const QString bundled_python = QDir::cleanPath(QCoreApplication::applicationDirPath() + u"/../Resources/python"_s);
    if (QFileInfo(bundled_python).isDir()) {
      qputenv("PYTHONHOME", QFile::encodeName(bundled_python));
    }
  }
#endif
  // Before any QRhiWidget exists: each top-level can pick its graphics API only once.
  (void)PJ::preferredGraphicsApi();
#ifdef PJ_TARGET_WASM
  // The browser's exec() never returns: File -> Quit only closes the window (running closeEvent's worker
  // shutdown); the page owns the runtime's lifetime.
  app.setQuitOnLastWindowClosed(false);
#endif
  QCoreApplication::setOrganizationName(u"PlotJuggler"_s);
  QCoreApplication::setApplicationName(u"PlotJuggler4"_s);
  // PJ_VERSION_STRING comes from the generated pj_version.h (PJ_VERSION); the About box shows it and the update
  // check compares it against the latest release.
  QCoreApplication::setApplicationVersion(QStringLiteral(PJ_VERSION_STRING));
  QApplication::setApplicationDisplayName(u"PlotJuggler 4"_s);
  // Wayland compositors ignore the window icon and look up <app_id>.desktop, so the desktop file name must equal
  // the basename of the one the .deb / AppImage install (packaging/appimage/plotjuggler4.desktop).
  QGuiApplication::setWindowIcon(QIcon(u":/resources/plotjuggler.svg"_s));
  QGuiApplication::setDesktopFileName(u"plotjuggler4"_s);

#ifdef PJ_TARGET_WASM
  // The deny-by-default browser persistence boundary, before the first QSettings consumer (WidgetTuner, splash,
  // Theme, MainWindow, a plugin host).
  auto* browser_persistence = new PJ::BrowserPersistence(&app);
  Q_UNUSED(browser_persistence);
  // Before a validate's bounded wait on the Python helper: see prestartPythonHelper().
  PJ::scripting::prestartPythonHelper();
#endif

  // Before any window is built, so every widget and plugin dialog inherits it; the QSS styles only font-size,
  // never font-family. Its Bold is a 600 (resources/fonts/PlotJugglerSans/README.md).
  if (const QString family = PJ::registerBundledFont(u":/resources/fonts/PlotJugglerSans"_s); !family.isEmpty()) {
    QFont app_font = QApplication::font();
    app_font.setFamily(family);
    QApplication::setFont(app_font);
  }
#ifdef PJ_TARGET_WASM
  // Browsers need an explicit CJK fallback for both Chinese UI catalogs and user text.
  if (QFontDatabase::addApplicationFont(u":/resources/fonts/NotoSansSC/NotoSansSC-Subset.otf"_s) < 0) {
    qWarning("Failed to load bundled Noto Sans SC font");
  }
  if (QFontDatabase::addApplicationFont(u":/resources/fonts/NotoSansSymbols2/NotoSansSymbols2-Subset.ttf"_s) < 0) {
    qWarning("Failed to load bundled Noto Sans Symbols 2 font");
  }
  QFont app_font = QApplication::font();
  QStringList families = app_font.families();
  families.append(u"Noto Sans SC"_s);
  families.append(u"Noto Sans Symbols 2"_s);
  app_font.setFamilies(families);
  QApplication::setFont(app_font);
#endif

  // Tags menus and palette-paints combo popups on Polish, where QSS specificity cannot reach.
  auto* tuner = new PJ::WidgetTuner(&app);
  qApp->installEventFilter(tuner);

  QCommandLineParser parser;
  parser.setApplicationDescription(u"PlotJuggler 4"_s);
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption language_option(
      u"language"_s,
      QCoreApplication::translate("AppLanguage", "Interface language for this session (zh_CN, zh_TW or en_US)."),
      u"language"_s);
  parser.addOption(language_option);
  const QCommandLineOption test_data_option(
      u"test-data"_s, u"Populate the datastore with generated sin/cos samples and one image."_s);
  parser.addOption(test_data_option);
  const QCommandLineOption plugin_dir_option(
      u"plugin-dir"_s, u"Override the directory where extensions are discovered and managed."_s, u"path"_s);
  parser.addOption(plugin_dir_option);
#ifndef PJ_TARGET_WASM
  const QCommandLineOption validate_plugins_option(
      u"validate-plugins"_s, u"Load and validate every whitelisted plugin in this directory, then exit."_s, u"path"_s);
  parser.addOption(validate_plugins_option);
  const QCommandLineOption expect_plugin_option(
      u"expect-plugin"_s, u"Expected plugin as id=version; repeat once per whitelisted plugin."_s, u"id=version"_s);
  parser.addOption(expect_plugin_option);
  // Registered only so --help lists it; handled at the top of main().
  parser.addOption(
      QCommandLineOption(u"selftest-python"_s, u"Compile and run a Python Data Processor headlessly, then exit."_s));
  parser.addOption(QCommandLineOption(
      u"selftest-docking"_s, u"Check that hidden dock tabs keep their parent (offscreen by default), then exit."_s));
#endif
  const QCommandLineOption layout_option(
      QStringList{u"l"_s, u"layout"_s}, u"Load a layout on startup. Explicit data files replace its saved sources."_s,
      u"path"_s);
  parser.addOption(layout_option);
#ifndef PJ_TARGET_WASM
  const QCommandLineOption datafile_option(
      QStringList{u"d"_s, u"datafile"_s}, u"Load a data file before the layout; repeat for multiple files."_s,
      u"path"_s);
  parser.addOption(datafile_option);
  parser.addPositionalArgument(u"files"_s, u"Data files to load before the layout."_s, u"[files...]"_s);
#endif
  const QCommandLineOption autoplay_option(
      u"autoplay"_s, QStringLiteral(
                         "Start looping playback automatically once a data source provides a time range "
                         "(useful with --layout / --test-data for demos and profiling)."));
  parser.addOption(autoplay_option);
  const QCommandLineOption nosplash_option(
      QStringList() << u"n"_s << u"nosplash"_s, u"Don't display the splashscreen on startup."_s);
  parser.addOption(nosplash_option);
  const QCommandLineOption debug_mode_option(
      u"debug-mode"_s, u"Reveal developer-only preferences and tooling that are hidden in normal runs."_s);
  parser.addOption(debug_mode_option);
  const QCommandLineOption disable_gpu_canvas_option(
      u"disable-opengl"_s,
      QStringLiteral(
          "Force plots onto the software raster canvas for this session, overriding the saved GPU canvas "
          "preference (does not change it)."));
  parser.addOption(disable_gpu_canvas_option);
  // The app grabs itself because GNOME Wayland blocks external screen-capture tools.
  const QCommandLineOption screenshot_option(
      u"screenshot"_s,
      u"Grab the first 3D view to a PNG after --screenshot-delay, then exit (non-zero on a missing or blank capture)."_s,
      u"path"_s);
  parser.addOption(screenshot_option);
  const QCommandLineOption screenshot_delay_option(
      u"screenshot-delay"_s, u"ms to wait before the screenshot grab (default 7000)."_s, u"ms"_s, u"7000"_s);
  parser.addOption(screenshot_delay_option);
#ifndef PJ_TARGET_WASM
  // Headless acceptance flags: --dump-diagnostics writes the whole run's diagnostics (not the bell's bounded
  // history); --exit-after-layout turns the restore settlement into an exit code.
  const QCommandLineOption dump_diagnostics_option(
      u"dump-diagnostics"_s,
      u"Write every diagnostic reported during this run to a JSON file on exit (full run, never truncated)."_s,
      u"json-path"_s);
  parser.addOption(dump_diagnostics_option);
  const QCommandLineOption exit_after_layout_option(
      u"exit-after-layout"_s,
      QStringLiteral(
          "With --layout: quit when the layout restore settles. Exit codes: 0 = restore committed, "
          "1 = layout load failed, 2 = timed out (see --exit-after-layout-timeout); usage errors exit 64."));
  parser.addOption(exit_after_layout_option);
  const QCommandLineOption exit_after_layout_timeout_option(
      u"exit-after-layout-timeout"_s,
      u"Seconds --exit-after-layout waits for settlement before exiting with code 2 (default 300)."_s, u"seconds"_s,
      u"300"_s);
  parser.addOption(exit_after_layout_timeout_option);
  const QCommandLineOption mcp_option(
      u"mcp"_s,
      u"Let AI agents control this window for this run (MCP server on a free 127.0.0.1 port; see --mcp-bridge), "
      "whatever the Preferences setting."_s);
  parser.addOption(mcp_option);
  const QCommandLineOption mcp_ui_tools_option(
      u"mcp-ui-tools"_s,
      u"With the MCP server on (implies --mcp): also give agents tools that read the widget tree and click and type "
      "like the user, under the user's dialogs too. For automated QA runs, best in a virtual display."_s);
  parser.addOption(mcp_ui_tools_option);
  const QCommandLineOption mcp_relay_option(
      u"mcp-relay"_s,
      u"Link this window to an AI agent through the relay at <url>, its secret in the url's key (pj_mcp_relay.py, "
      "as the browser build's ?mcp=<url> does)."_s,
      u"url"_s);
  parser.addOption(mcp_relay_option);
  // Registered only so --help lists it; handled at the top of main().
  parser.addOption(QCommandLineOption(
      u"mcp-bridge"_s, u"Speak MCP over stdio, forwarding to a running PlotJuggler with MCP on (for agents)."_s));
#endif
  parser.process(app);

  // The browser's settings boundary is already installed. Keep translators
  // alive until every window is destroyed; a CLI override never changes the
  // saved preference or the locale used to parse and format numeric data.
  const QSettings language_settings;
  if (parser.isSet(language_option) && parser.value(language_option) != "en_US"_L1 &&
      PJ::AppLanguage::normalizedLanguage(parser.value(language_option)) == "en_US"_L1) {
    qWarning(
        "Unsupported --language '%s' (use zh_CN, zh_TW or en_US); using English",
        qUtf8Printable(parser.value(language_option)));
  }
  const PJ::AppLanguage app_language(
      app, parser.isSet(language_option) ? parser.value(language_option)
                                         : PJ::AppLanguage::preferredLanguage(language_settings));

#ifndef PJ_TARGET_WASM
  // Qt-free plugins inherit only the canonical UI locale, before any DSO loads.
  // English remains the fallback; neither data locale nor serialized keys change.
  qputenv("PLOTJUGGLER_UI_LANGUAGE", app_language.language().toUtf8());
  if (parser.isSet(validate_plugins_option)) {
    return validatePlugins(parser.value(validate_plugins_option), parser.values(expect_plugin_option));
  }

  // Without a layout a headless harness would sit in an unobserved GUI session. Usage errors exit 64 (EX_USAGE),
  // distinct from the run's own codes (1 = load failed, 2 = timeout).
  constexpr int kExitUsage = 64;
  int exit_after_layout_timeout_s = 0;
  if (parser.isSet(exit_after_layout_option)) {
    if (!parser.isSet(layout_option)) {
      std::fprintf(stderr, "[exit-after-layout] --exit-after-layout requires --layout\n");
      return kExitUsage;
    }
    bool timeout_ok = false;
    exit_after_layout_timeout_s = parser.value(exit_after_layout_timeout_option).toInt(&timeout_ok);
    // The deadline is a QTimer interval in int milliseconds: overflow is a usage error, not a silent wrap.
    if (!timeout_ok || exit_after_layout_timeout_s <= 0 ||
        exit_after_layout_timeout_s > std::numeric_limits<int>::max() / 1000) {
      std::fprintf(
          stderr, "[exit-after-layout] invalid --exit-after-layout-timeout value: %s\n",
          qPrintable(parser.value(exit_after_layout_timeout_option)));
      return kExitUsage;
    }
  }
#endif

  // Before any UI is built.
  PJ::setDebugMode(parser.isSet(debug_mode_option));

  // Before any plot is built. Leaves Preferences::use_opengl untouched.
  PJ::PlotWidgetBase::setGpuCanvasDisabledOverride(parser.isSet(disable_gpu_canvas_option));

  // Covers the slow MainWindow construction. Skipped with --layout, where the user wants data, not a meme.
  std::unique_ptr<QSplashScreen> splash;
  if (!parser.isSet(nosplash_option) && !parser.isSet(layout_option)) {
    const QPixmap pixmap = PJ::makeStartupSplash();
    if (!pixmap.isNull()) {
      splash = std::make_unique<QSplashScreen>(pixmap, Qt::WindowStaysOnTopHint);
      if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        splash->move(screen->availableGeometry().center() - splash->rect().center());
      }
      splash->show();
      app.processEvents();
    }
  }

  PJ::MainWindow window(parser.value(plugin_dir_option));

#ifndef PJ_TARGET_WASM
  // Attached before anything pumps the loop: the splash wait and --test-data call processEvents(), which would
  // deliver the queued construction-time diagnostics (plugin-load failures) to no subscriber. Written on
  // aboutToQuit, which also fires on QCoreApplication::exit(nonzero).
  if (parser.isSet(dump_diagnostics_option)) {
    auto* dump = new PJ::DiagnosticDump(parser.value(dump_diagnostics_option), &window);
    dump->attachTo(window.diagnosticBridge());
    QObject::connect(&app, &QCoreApplication::aboutToQuit, dump, [dump]() {
      if (dump->write()) {
        std::printf("[dump-diagnostics] wrote %d record(s): %s\n", dump->size(), qPrintable(dump->outputPath()));
      } else {
        std::fprintf(stderr, "[dump-diagnostics] write FAILED: %s\n", qPrintable(dump->outputPath()));
      }
    });
  }
#endif

  // Observes key presses without consuming them.
#ifndef PJ_TARGET_WASM
  auto* gesture_watcher =
      new PJ::KeySequenceWatcher(PJ::unlockSteps(), [&window]() { window.openEmbeddedConsole(); }, &app);
  qApp->installEventFilter(gesture_watcher);
#endif

  // Before any data loads, so the one-shot listener catches the first range.
  if (parser.isSet(autoplay_option)) {
    window.enableAutoplay();
  }

  if (parser.isSet(test_data_option)) {
    if (!window.populateTestData()) {
      return EXIT_FAILURE;
    }
  }
  if (splash) {
    // A click dismisses it early (QSplashScreen hides on mousePressEvent). The main window is shown only after
    // this loop: Wayland ignores WindowStaysOnTopHint / raise() once it is up, and the splash would go behind it.
    const QDateTime deadline = QDateTime::currentDateTime().addMSecs(4000);
    while (QDateTime::currentDateTime() < deadline && !splash->isHidden()) {
      app.processEvents();
      QThread::msleep(20);
    }
  }

#ifndef PJ_TARGET_WASM
  // Before show(), so the window maps directly at the saved place and state.
  window.restoreGeometry(QSettings().value(u"MainWindow.geometry"_s).toByteArray());
#endif
  window.show();

  if (splash) {
    splash->finish(&window);
  }

#ifndef PJ_TARGET_WASM
  const bool attended = !isUnattendedRun(parser, exit_after_layout_option, screenshot_option, dump_diagnostics_option);
  // Tests and headless runs must not repoint the agent plugin's launcher at a build tree.
  if (attended) {
    PJ::mcp::recordExecutable();
  }
  PJ::mcp::setUiToolsEnabled(parser.isSet(mcp_ui_tools_option));
  // On by default for an attended run; tests and headless runs open it only with --mcp, so they never show up as
  // an instance an agent could pick.
  if (parser.isSet(mcp_option) || parser.isSet(mcp_ui_tools_option) ||
      (attended && QSettings().value(PJ::MainWindow::kMcpServerKey, true).toBool())) {
    window.openMcpPublicAccess();
  }
  if (parser.isSet(mcp_relay_option)) {
    window.connectMcpRelay(QUrl(parser.value(mcp_relay_option)));
  }

  // Before any layout restore.
  window.reviewUncleanShutdown(/*interactive=*/attended);
#endif

  // Deferred: the progressive layout restore needs a running event loop.
  QStringList startup_files;
#ifndef PJ_TARGET_WASM
  startup_files = parser.values(datafile_option) + parser.positionalArguments();
#endif
  if (parser.isSet(layout_option) || !startup_files.isEmpty()) {
    const QString layout_path = parser.value(layout_option);
#ifdef PJ_TARGET_WASM
    constexpr bool interactive = false;
#else
    const bool interactive = attended;
#endif
    QTimer::singleShot(0, &window, [&window, startup_files, layout_path, interactive]() {
      window.loadAtStartup(startup_files, layout_path, interactive);
    });
  }

#ifdef PJ_TARGET_WASM
  const QUrl page_url(QString::fromStdString(emscripten::val::global("location")["href"].as<std::string>()));
  window.setDeploymentPageUrl(page_url);
  // ?mcp=<relay>: the agent plugin's relay opened this page.
  if (const QString relay = QUrlQuery(page_url).queryItemValue(u"mcp"_s, QUrl::FullyDecoded); !relay.isEmpty()) {
    window.connectMcpRelay(QUrl(relay));
  }
  QTimer::singleShot(0, &window, [&window, page_url]() { window.startBrowserPresets(page_url); });
#endif

#ifndef PJ_TARGET_WASM
  // Composes with --screenshot: whichever quit fires first wins, and only this flag's exit code says whether the
  // restore committed.
  if (parser.isSet(exit_after_layout_option)) {
    const int timeout_s = exit_after_layout_timeout_s;
    new PJ::LayoutExitWatch(
        window, std::chrono::seconds(timeout_s),
        [timeout_s](int code) {
          if (code == PJ::LayoutExitWatch::kExitCodeSuccess) {
            std::printf("[exit-after-layout] layout restore settled: success\n");
          } else if (code == PJ::LayoutExitWatch::kExitCodeLoadFailed) {
            std::fprintf(stderr, "[exit-after-layout] layout load FAILED\n");
          } else {
            std::fprintf(stderr, "[exit-after-layout] TIMED OUT after %d s without settlement\n", timeout_s);
          }
          QCoreApplication::exit(code);
        },
        &window);
  }
#endif

  // Desktop only: a browser tab is a page view, not an installed user, and cannot self-update. Nor can the
  // environment guards exclude it: Emscripten's getenv() never sees the host's `CI`, so every headless-browser
  // suite would count as real users.
#ifndef PJ_TARGET_WASM
  // Picks the update channel (apt, the Windows maintenance tool, GitHub).
  window.setInstallation(QStringLiteral(PJ_INSTALLATION_STRING));
  // Deferred: QNetworkAccessManager needs the running event loop.
  if (!parser.isSet(screenshot_option) &&
      QSettings().value(u"Preferences::check_updates_on_startup"_s, true).toBool()) {
    QTimer::singleShot(0, &window, [&window]() { window.checkForUpdates(/*interactive=*/false); });
    QTimer::singleShot(0, &window, [&window]() { window.checkExtensionUpdates(); });
  } else {
    window.noteRegistryScanSkipped();
  }

  // docs/TELEMETRY.md. Skipped under automation: CI runners have ephemeral machine ids, so every run would count
  // as a new user (`CI` covers most CI systems, PJ_DISABLE_TELEMETRY other harnesses). These gates cover a packaged
  // build; TelemetryPing itself refuses an unpackaged one.
  const bool automated = parser.isSet(screenshot_option) || qEnvironmentVariableIsSet("CI") ||
                         qEnvironmentVariableIsSet("PJ_DISABLE_TELEMETRY");
  if (!automated && QSettings().value(u"Preferences::send_anonymous_stats"_s, true).toBool()) {
    QTimer::singleShot(0, &window, [&window]() { window.sendTelemetryPing(QStringLiteral(PJ_INSTALLATION_STRING)); });
  }
  // The daily usage report has its own opt-out (read by startUsageReporting) and the same automation gates.
  window.startUsageReporting(QStringLiteral(PJ_INSTALLATION_STRING), /*may_report=*/!automated);
#endif

  // Exits non-zero when nothing usable was captured (no view, a null or single-colour
  // image, a failed save), so package smoke tests can rely on the status.
  if (parser.isSet(screenshot_option)) {
    const QString path = parser.value(screenshot_option);
    const int delay_ms = parser.value(screenshot_delay_option).toInt();
    QTimer::singleShot(delay_ms, &window, [&window, path]() {
#ifdef PJ_WITH_SCENE3D
      const QList<pj::scene3d::SceneViewWidget*> views = window.findChildren<pj::scene3d::SceneViewWidget*>();
      if (views.isEmpty()) {
        std::fprintf(stderr, "[screenshot] no 3D SceneViewWidget found\n");
      } else {
        // Possibly after returning to the event loop.
        pj::scene3d::SceneViewWidget* view = views.first();
        view->grabPresentedFrame([path, view](const QImage& img) {
          std::printf("[screenshot] graphics backend: %s\n", qPrintable(view->rhiBackendName()));
          const QImage rgb = img.convertToFormat(QImage::Format_RGB32);
          const QRgb first = rgb.isNull() ? 0 : rgb.pixel(0, 0);
          bool blank = true;
          for (int y = 0; blank && y < rgb.height(); ++y) {
            const auto* line = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
            blank = std::all_of(line, line + rgb.width(), [first](QRgb pixel) { return pixel == first; });
          }
          if (blank) {
            std::fprintf(stderr, "[screenshot] FAILED: the captured image is %s\n", rgb.isNull() ? "null" : "blank");
          } else if (img.save(path)) {
            std::printf("[screenshot] saved: %s (%dx%d)\n", qPrintable(path), img.width(), img.height());
            QCoreApplication::exit(0);
            return;
          } else {
            std::fprintf(stderr, "[screenshot] save FAILED: %s\n", qPrintable(path));
          }
          QCoreApplication::exit(1);
        });
        return;
      }
#else
      Q_UNUSED(window);
      Q_UNUSED(path);
      std::fprintf(stderr, "[screenshot] Scene3D is not available in this build\n");
#endif
      QCoreApplication::exit(1);
    });
  }

  return app.exec();
}
