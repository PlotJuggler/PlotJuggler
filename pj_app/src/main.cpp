// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QScreen>
#include <QSplashScreen>
#include <QThread>
#include <QTimer>
#include <Qt>
#ifndef PJ_TARGET_WASM
#include <backward.hpp>
#endif
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
#ifndef PJ_TARGET_WASM
#include <string>
#endif

#include "BrowserPersistence.h"
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
#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/processor_detail.hpp"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_plotting/RasterTextEngine.h"
#ifndef PJ_TARGET_WASM
#include "pj_runtime/PluginRuntimeCatalog.h"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/python_engine.h"
#endif
#ifdef PJ_WITH_SCENE3D
#include "pj_scene3d_widgets/scene_view_widget.h"  // --screenshot grabs the 3D view
#endif
#include "pj_version.h"
#include "pj_widgets/Style.h"
using namespace Qt::StringLiterals;

namespace {
// One process-wide crash handler. Its constructor (run at static-init, before
// main) registers handlers for SIGSEGV/SIGABRT/SIGFPE/... that dump a
// symbolized stack trace to stderr. We instantiate it explicitly rather than
// relying on the global defined inside backward-cpp's compiled backward.cpp,
// which the linker drops from the static archive when nothing references it.
// backward-cpp recommends exactly one such instance per program.
#ifndef PJ_TARGET_WASM
backward::SignalHandling g_crash_handler;

// Boots the embedded Python backend the way a Data Processor does, inside the
// packaged binary. Nothing else in any release flow ever launches the AppImage, so a
// Python backend that cannot reach its own stdlib ships undetected — which is exactly
// how the "undefined symbol: PyFloat_Type" report happened.
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

#if defined(_WIN32)
  catalog.setPluginDir(std::filesystem::path(plugin_dir.toStdWString()));
#else
  catalog.setPluginDir(std::filesystem::path(plugin_dir.toStdString()));
#endif
  catalog.setHostVersion(QCoreApplication::applicationVersion().toStdString());
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
#endif
}  // namespace

int main(int argc, char* argv[]) {
#ifndef PJ_TARGET_WASM
  // Handled before any Q(Core)Application exists: the selftest needs nothing
  // from Qt, while a QApplication would demand a platform plugin — and the
  // packaged AppImage ships only xcb, useless on the display-less CI
  // containers this flag exists for.
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--selftest-python") == 0) {
      return selftestPython();
    }
  }
#endif

  // Pin to Fusion (under our Style proxy) before constructing
  // QApplication so widgets that read the style at construction time
  // don't end up with the platform's native style (KDE Breeze, GNOME
  // Adwaita, etc.) which silently overrides QSS on QMenu and other
  // popups. Style additionally suppresses default dialog-button icons
  // and the underline-mnemonic decoration.
  QApplication::setStyle(new PJ::Style(u"Fusion"_s));

  // NOTE: we deliberately do NOT set Qt::AA_ShareOpenGLContexts. It was once set
  // so a 3D scene's GL resources would survive a QOpenGLWidget context
  // recreation on ADS reparent — but it put every SceneViewWidget's context into
  // a single share group, and destroying one view's context (closing/splitting a
  // 3D dock) corrupted the VAO/FBO state of the sibling views still on screen
  // (a glBindVertexArray(non-gen name) flood + the map texture vanishing in the
  // surviving view). With each view's GL context fully independent, tearing one
  // down can no longer touch the others. The original "survive a context
  // recreation" concern is handled instead inside pj_scene3D: every render pass
  // and layer implements releaseGL(), and SceneViewWidget rebuilds its GL state
  // in initializeGL() — so a recreated context self-heals rather than relying on
  // a process-wide share group.

  // GL-safe on-canvas text (legend/tracker): must run before ANY QwtText is
  // constructed — Qwt deletes the engines this replaces while existing
  // QwtText objects still hold raw pointers to them. See RasterTextEngine.h.
  PJ::installRasterTextEngines();

  QApplication app(argc, argv);
#ifdef PJ_TARGET_WASM
  // Qt's browser-backed exec() never returns, including on application exit.
  // Let File -> Quit close the window (and run MainWindow::closeEvent's
  // cooperative worker shutdown) without also asking QEventLoop to exit. The
  // browser owns the runtime lifetime and releases it when the page closes.
  app.setQuitOnLastWindowClosed(false);
#endif
  QCoreApplication::setOrganizationName(u"PlotJuggler"_s);
  QCoreApplication::setApplicationName(u"PlotJuggler4"_s);
  // PJ_VERSION_STRING comes from the root project(VERSION) via pj_app's
  // target_compile_definitions — the single source of truth read by the About
  // box and compared against the latest GitHub release.
  QCoreApplication::setApplicationVersion(QStringLiteral(PJ_VERSION_STRING));
  QApplication::setApplicationDisplayName(u"PlotJuggler 4"_s);
  // The window icon covers X11 / Windows decorations and task bars. Wayland
  // compositors ignore it and resolve the dock icon through the app_id →
  // <app_id>.desktop lookup instead, so the app_id must equal the basename of
  // the desktop file the .deb / AppImage install (appimage/plotjuggler4.desktop).
  QGuiApplication::setWindowIcon(QIcon(u":/resources/plotjuggler.svg"_s));
  QGuiApplication::setDesktopFileName(u"plotjuggler4"_s);

#ifdef PJ_TARGET_WASM
  // Install the deny-by-default browser persistence boundary before the first
  // app-owned QSettings consumer (WidgetTuner, splash, Theme, MainWindow, or a
  // plugin host). Native builds do not execute or instantiate this code and
  // therefore retain their existing settings backend and behavior.
  auto* browser_persistence = new PJ::BrowserPersistence(&app);
  Q_UNUSED(browser_persistence);
#endif

  // Register the bundled Noto Sans and apply it as the application font, before
  // any window is built so every widget — and every plugin dialog, which
  // inherits the app font — uses it. The variable font spans the whole weight
  // axis, so font-weight 400/600/700 resolve to real masters (not synthesized
  // bold). The QSS never declares a font-family, so this family flows through
  // untouched; only font-size is styled.
  if (QFontDatabase::addApplicationFont(u":/resources/fonts/NotoSans/NotoSans-Variable.ttf"_s) < 0) {
    qWarning("Failed to load bundled Noto Sans font");
  }
  QFont app_font = QApplication::font();
  app_font.setFamily(u"Noto Sans"_s);
  QApplication::setFont(app_font);

  // WidgetTuner: app-wide Polish-event filter that side-steps QSS
  // specificity battles by directly tagging menus and palette-painting
  // combo popups.
  auto* tuner = new PJ::WidgetTuner(&app);
  qApp->installEventFilter(tuner);

  QCommandLineParser parser;
  parser.setApplicationDescription(u"PlotJuggler 4"_s);
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption test_data_option(
      u"test-data"_s, u"Populate the datastore with generated sin/cos samples."_s);
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
#endif
  const QCommandLineOption layout_option(
      u"layout"_s, u"Load a layout file on startup, reloading its data source(s)."_s, u"path"_s);
  parser.addOption(layout_option);
  const QCommandLineOption autoplay_option(
      u"autoplay"_s, QStringLiteral(
                         "Start looping playback automatically once a data source provides a time range "
                         "(useful with --layout / --test-data for demos and profiling)."));
  parser.addOption(autoplay_option);
  const QCommandLineOption nosplash_option(
      QStringList() << u"n"_s << u"nosplash"_s, u"Don't display the splashscreen on startup."_s);
  parser.addOption(nosplash_option);
  // Dev-only splash preview, disabled but kept for future tweaks: renders the
  // configured splash to a PNG and exits (see the matching handler below).
  // const QCommandLineOption dump_splash_option(
  //     u"dump-splash"_s,
  //     u"Render the configured startup splashscreen to a PNG and exit (dev preview)."_s,
  //     u"path"_s);
  // parser.addOption(dump_splash_option);
  const QCommandLineOption debug_mode_option(
      u"debug-mode"_s, u"Reveal developer-only preferences and tooling that are hidden in normal runs."_s);
  parser.addOption(debug_mode_option);
  const QCommandLineOption disable_opengl_option(
      u"disable-opengl"_s,
      QStringLiteral(
          "Force plots onto the software raster canvas for this session, overriding the saved OpenGL "
          "preference (does not change it)."));
  parser.addOption(disable_opengl_option);
  // Headless 3D capture for verification: after --screenshot-delay ms (enough for an
  // async --layout load + a couple seconds of --autoplay to pose the robot), grab the
  // first SceneViewWidget's framebuffer to a PNG and quit. GNOME Wayland blocks
  // external screen-capture tools, so the app must grab itself.
  const QCommandLineOption screenshot_option(
      u"screenshot"_s, u"Grab the first 3D view to a PNG after --screenshot-delay, then exit."_s, u"path"_s);
  parser.addOption(screenshot_option);
  const QCommandLineOption screenshot_delay_option(
      u"screenshot-delay"_s, u"ms to wait before the screenshot grab (default 7000)."_s, u"ms"_s, u"7000"_s);
  parser.addOption(screenshot_delay_option);
#ifndef PJ_TARGET_WASM
  // Headless acceptance flags (the layout-import E2E observation channel):
  // --dump-diagnostics serializes the WHOLE run's diagnostics (never the
  // 200-record bell ring) and --exit-after-layout turns the restore
  // settlement boundary into a failure-aware process exit code.
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
#endif
  parser.process(app);

#ifndef PJ_TARGET_WASM
  if (parser.isSet(validate_plugins_option)) {
    return validatePlugins(parser.value(validate_plugins_option), parser.values(expect_plugin_option));
  }

  // --exit-after-layout is meaningless without a layout to settle: fail fast
  // (a headless harness must never sit in an unobserved GUI session). Usage
  // errors exit 64 (EX_USAGE, the sysexits convention) so scripts can always
  // distinguish them from the run's own codes (1 = load failed, 2 = timeout).
  constexpr int kExitUsage = 64;
  int exit_after_layout_timeout_s = 0;
  if (parser.isSet(exit_after_layout_option)) {
    if (!parser.isSet(layout_option)) {
      std::fprintf(stderr, "[exit-after-layout] --exit-after-layout requires --layout\n");
      return kExitUsage;
    }
    bool timeout_ok = false;
    exit_after_layout_timeout_s = parser.value(exit_after_layout_timeout_option).toInt(&timeout_ok);
    // Upper bound: the deadline rides QTimer, whose interval is int
    // milliseconds — a seconds value whose milliseconds would overflow int
    // must be a loud usage error, never a silent wrap.
    if (!timeout_ok || exit_after_layout_timeout_s <= 0 ||
        exit_after_layout_timeout_s > std::numeric_limits<int>::max() / 1000) {
      std::fprintf(
          stderr, "[exit-after-layout] invalid --exit-after-layout-timeout value: %s\n",
          qPrintable(parser.value(exit_after_layout_timeout_option)));
      return kExitUsage;
    }
  }
#endif

  // Latch the launch-time debug gate before any UI is built (PreferencesDialog
  // reads it to decide whether to show the chrome-metric scrubbers).
  PJ::setDebugMode(parser.isSet(debug_mode_option));

  // Session-only OpenGL override: applied before any plot is constructed so the
  // first plot already honours it. Leaves Preferences::use_opengl untouched.
  PJ::PlotWidgetBase::setOpenGlDisabledOverride(parser.isSet(disable_opengl_option));

  // Dev preview (disabled, kept for future tweaks): render the configured splash
  // to a PNG and exit — lets us inspect the "serious" splash without launching
  // (and without screen-capture, which GNOME Wayland blocks). Re-enable together
  // with the dump_splash_option declaration above.
  // if (parser.isSet(dump_splash_option)) {
  //   const QString path = parser.value(dump_splash_option);
  //   const bool ok = PJ::makeStartupSplash().save(path);
  //   std::fprintf(ok ? stdout : stderr, "[dump-splash] %s: %s\n", ok ? "saved" : "FAILED", qPrintable(path));
  //   return ok ? EXIT_SUCCESS : EXIT_FAILURE;
  // }

  // The funny splashscreen: a random meme that covers the (slow) MainWindow
  // construction below. Skipped with --nosplash and when launching straight
  // into a layout (--layout), where the user wants data, not a meme. Shown
  // before the MainWindow ctor so it's already on screen while the ctor runs.
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
  // --dump-diagnostics: attached IMMEDIATELY after MainWindow construction,
  // before ANYTHING can pump the loop — the splash wait and --test-data below
  // both processEvents(), which would deliver the queued construction-time
  // bridge re-emits (e.g. plugin-load failures) to zero subscribers and lose
  // them for good. Serialized on aboutToQuit, which fires on EVERY event-loop
  // exit — including QCoreApplication::exit(nonzero) — so failure paths get
  // the file too.
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

  // App-wide gesture watcher. Observes key presses without consuming them and
  // calls the entry point when the fixed sequence completes.
#ifndef PJ_TARGET_WASM
  auto* gesture_watcher =
      new PJ::KeySequenceWatcher(PJ::unlockSteps(), [&window]() { window.openEmbeddedConsole(); }, &app);
  qApp->installEventFilter(gesture_watcher);
#endif

  // Arm autoplay BEFORE any data loads, so its one-shot listener catches the first
  // range — whether --test-data sets it synchronously below or --layout's async
  // load sets it once the worker finishes.
  if (parser.isSet(autoplay_option)) {
    window.enableAutoplay();
  }

  if (parser.isSet(test_data_option)) {
    if (!window.populateTestData()) {
      return EXIT_FAILURE;
    }
  }
  if (splash) {
    // Keep the meme up briefly so it's actually seen, but let a click dismiss
    // it early: QSplashScreen hides itself on mousePressEvent, so once the user
    // clicks it isHidden() flips and we stop waiting. msleep keeps the spin off
    // the CPU while still pumping events so the click is delivered.
    //
    // The main window is shown only AFTER this loop: a still-hidden main window
    // can't be stacked above the splash, so the meme stays on top. (Wayland
    // ignores WindowStaysOnTopHint / raise() once the main window is up, which
    // is exactly how the splash ended up behind it.)
    const QDateTime deadline = QDateTime::currentDateTime().addMSecs(4000);
    while (QDateTime::currentDateTime() < deadline && !splash->isHidden()) {
      app.processEvents();
      QThread::msleep(20);
    }
  }

  window.show();

  if (splash) {
    // Close the splash once the main window is up.
    splash->finish(&window);
  }

  // Deferred so the load runs after the event loop starts (the file loads on a
  // worker; the progressive layout restore needs a running loop).
  if (parser.isSet(layout_option)) {
    const QString layout_path = parser.value(layout_option);
    QTimer::singleShot(0, &window, [&window, layout_path]() { window.loadLayoutAtStartup(layout_path); });
  }

#ifndef PJ_TARGET_WASM
  // --exit-after-layout: quit at the restore settlement boundary with a
  // failure-aware exit code (0 committed / 1 load failed / 2 timeout — the
  // LayoutExitWatch constants). Settlement is emitted only after the import
  // batch finished and every restore waiter cleared, so this quit can never
  // tear down a mid-flight import. Composes with --screenshot: whichever
  // quit fires first wins (the screenshot timer stays an unconditional
  // watchdog reporting 0 — it is never the success oracle; use THIS flag's
  // exit code for that).
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

  // One-shot GitHub release check, opt-out via Preferences (default on) and
  // skipped for headless --screenshot runs. Deferred to the running event loop
  // (QNetworkAccessManager needs it); failures/no-release are silent.
  if (!parser.isSet(screenshot_option) &&
      QSettings().value(u"Preferences::check_updates_on_startup"_s, true).toBool()) {
    QTimer::singleShot(0, &window, [&window]() { window.checkForUpdates(/*interactive=*/false); });
    // Same gate: scan the marketplace registry for installed-extension updates
    // and reveal the title-bar "Update" badge if any are available (silent on
    // failure). Deferred for the event loop just like the release check.
    QTimer::singleShot(0, &window, [&window]() { window.checkExtensionUpdates(); });
  }

  // Anonymous daily-user ping (see docs/TELEMETRY.md): opt-out via Preferences
  // (default on) and skipped for headless --screenshot runs. Also skipped in
  // automated environments: CI runners have ephemeral machine ids, so every
  // run would be counted as a brand-new user (`CI` is set by GitHub Actions
  // and virtually every CI system; PJ_DISABLE_TELEMETRY covers other
  // automation, e.g. local docker/test harnesses). Deferred like the update
  // check; all failures are silent.
  const bool automated_environment =
      qEnvironmentVariableIsSet("CI") || qEnvironmentVariableIsSet("PJ_DISABLE_TELEMETRY");
  if (!parser.isSet(screenshot_option) && !automated_environment &&
      QSettings().value(u"Preferences::send_anonymous_stats"_s, true).toBool()) {
    QTimer::singleShot(0, &window, [&window]() { window.sendTelemetryPing(QStringLiteral(PJ_INSTALLATION_STRING)); });
  }

  if (parser.isSet(screenshot_option)) {
    const QString path = parser.value(screenshot_option);
    const int delay_ms = parser.value(screenshot_delay_option).toInt();
    QTimer::singleShot(delay_ms, &window, [&window, path]() {
#ifdef PJ_WITH_SCENE3D
      const QList<pj::scene3d::SceneViewWidget*> views = window.findChildren<pj::scene3d::SceneViewWidget*>();
      if (views.isEmpty()) {
        std::fprintf(stderr, "[screenshot] no 3D SceneViewWidget found\n");
      } else {
        // grabFramebuffer() renders paintGL() on demand, so it returns a fresh frame
        // with no separate update()/second timer needed.
        const QImage img = views.first()->grabFramebuffer();
        if (img.save(path)) {
          std::printf("[screenshot] saved: %s (%dx%d)\n", qPrintable(path), img.width(), img.height());
        } else {
          std::fprintf(stderr, "[screenshot] save FAILED: %s\n", qPrintable(path));
        }
      }
#else
      Q_UNUSED(window);
      Q_UNUSED(path);
      std::fprintf(stderr, "[screenshot] Scene3D is not available in this build\n");
#endif
      QCoreApplication::quit();
    });
  }

  return app.exec();
}
