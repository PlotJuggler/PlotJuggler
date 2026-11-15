// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "wasm/ArrowSideModule.h"

#include <dlfcn.h>
#include <emscripten.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QLoggingCategory>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "arrow_manifest.hpp"
#include "data_exporter_manifest.hpp"
#include "parquet_manifest.hpp"
#include "pj_plugins/host/plugin_catalog.hpp"
#include "pj_runtime/ExtensionCatalogService.h"
#include "wasm/arrow_side_module_abi.h"

using namespace Qt::StringLiterals;

namespace pj_app::arrow_side_module {
namespace {

Q_LOGGING_CATEGORY(lcSideModule, "pj.app.sidemodule")

// Relative to the worker's script, plotjuggler4.js, so it resolves inside the deployment's bundle directory.
constexpr const char* kModuleUrl = "plugins/arrow.wasm";
constexpr const char* kModuleDir = "/pj_side_modules";
constexpr const char* kModulePath = "/pj_side_modules/arrow.wasm";

struct Manifests {
  QStringList file_extensions;
  QString file_patterns;
  std::vector<std::string> encodings;
  QString toolbox_id;
  QString toolbox_name;
};

// The SDK's decoder, as the catalog reads a plugin's manifest: a malformed one fails here, not in the user's hands.
PJ::PluginDescriptor decode(PJ::PluginFamily family, const char* manifest_json) {
  auto descriptor = PJ::decodeManifest(std::filesystem::path{kModuleUrl}, family, manifest_json);
  if (!descriptor) {
    qFatal("%s: bad embedded manifest: %s", kModuleUrl, descriptor.error().c_str());
  }
  return std::move(*descriptor);
}

const Manifests& manifests() {
  static const Manifests kManifests = []() {
    Manifests m;
    QStringList patterns;
    for (const auto& ext : decode(PJ::PluginFamily::kDataSource, kParquetManifest).file_extensions) {
      // As the catalog normalises it (leading '.', ASCII case fold): what FileLoader looks up.
      QString normalised = QString::fromStdString(ext).toLower();
      if (!normalised.startsWith(u'.')) {
        normalised.prepend(u'.');
      }
      m.file_extensions << normalised;
      patterns << u"*"_s + normalised;
    }
    m.file_patterns = patterns.join(u' ');
    m.encodings = decode(PJ::PluginFamily::kMessageParser, kArrowManifest).encoding;
    const auto exporter = decode(PJ::PluginFamily::kToolbox, kDataExporterManifest);
    m.toolbox_id = QString::fromStdString(exporter.id);
    m.toolbox_name = QString::fromStdString(exporter.name);
    return m;
  }();
  return kManifests;
}

std::mutex g_load_mutex;
std::optional<QString> g_outcome;  // guarded by g_load_mutex
PJ::StaticPluginSet g_plugins;     // written once under g_load_mutex before g_outcome is set
QString g_timing;                  // ditto: what the load cost, for the log and the probe
bool g_registered = false;         // main thread

// clang-format off
EM_JS_DEPS(pj_arrow_side_module, "malloc");
// A synchronous XHR on the calling worker (only a worker may give one a timeout). The body in a malloc()ed buffer and
// its size in *size; or null, with *size = -status (an HTTP error), -1 (a network error or the timeout) or -2 (out
// of memory).
EM_JS(char*, pj_arrow_side_fetch, (const char* url, int* size), {
  let result = -1;
  try {
    const request = new XMLHttpRequest();
    request.open('GET', locateFile(UTF8ToString(url)), false);
    request.responseType = 'arraybuffer';
    request.timeout = 60000;
    request.send(null);
    if (request.status === 200) {
      const body = new Uint8Array(request.response);
      const buffer = _malloc(Math.max(body.length, 1)) >>> 0;
      if (buffer) {
        HEAPU8.set(body, buffer);
        HEAP32[size >>> 2] = body.length;
        return buffer;
      }
      result = -2;
    } else {
      result = request.status ? -request.status : -1;
    }
  } catch (error) {
  }
  HEAP32[size >>> 2] = result;
  return 0;
});
// clang-format on

// Downloads the module into MEMFS: dlopen() then copies it into shared memory once, where every thread links it
// from, instead of each thread fetching the URL again. Every error is retryable (loadBlocking does not cache it).
QString fetchModule(size_t& bytes) {
  int size = 0;
  const std::unique_ptr<char, decltype(&std::free)> body(pj_arrow_side_fetch(kModuleUrl, &size), &std::free);
  const QLatin1StringView url(kModuleUrl);
  if (!body) {
    if (size == -2) {
      return u"out of memory downloading %1"_s.arg(url);
    }
    if (size == -1) {
      return u"downloading %1 failed (network error or timeout)"_s.arg(url);
    }
    return u"downloading %1 failed (HTTP %2)"_s.arg(url).arg(-size);
  }
  bytes = static_cast<size_t>(size);
  mkdir(kModuleDir, 0755);
  FILE* file = std::fopen(kModulePath, "wb");
  QString error;
  if (file == nullptr || std::fwrite(body.get(), 1, bytes, file) != bytes) {
    error = u"could not stage %1"_s.arg(QLatin1StringView(kModulePath));
  }
  if (file != nullptr) {
    std::fclose(file);
  }
  return error;
}

QString linkModule() {
  // RTLD_NOW: resolve every import now, so a missing main-module export fails here rather than at first call.
  void* handle = dlopen(kModulePath, RTLD_NOW | RTLD_LOCAL);
  // dlopen() copied the bytes; the staged file is no longer needed.
  unlink(kModulePath);
  if (handle == nullptr) {
    return u"linking %1 failed: %2"_s.arg(QLatin1StringView(kModuleUrl), QString::fromUtf8(dlerror()));
  }
  // Never dlclose()d: the vtables live in it, and Emscripten 5.0.5 deadlocks re-opening a module.
  auto entry = reinterpret_cast<PjArrowSideEntry>(dlsym(handle, PJ_ARROW_SIDE_ENTRY));
  if (entry == nullptr) {
    return u"%1 has no %2"_s.arg(QLatin1StringView(kModuleUrl), QLatin1StringView(PJ_ARROW_SIDE_ENTRY));
  }
  const PjArrowSidePlugins plugins = entry();
  g_plugins.data_sources.emplace_back(plugins.parquet_source, plugins.parquet_dialog);
  g_plugins.message_parsers.emplace_back(plugins.arrow_parser);
  g_plugins.toolboxes.emplace_back(plugins.data_exporter, plugins.data_exporter_dialog);
  return {};
}

}  // namespace

const QStringList& fileExtensions() {
  return manifests().file_extensions;
}

const QString& filePatterns() {
  return manifests().file_patterns;
}

const std::vector<std::string>& parserEncodings() {
  return manifests().encodings;
}

const QString& toolboxId() {
  return manifests().toolbox_id;
}

const QString& toolboxName() {
  return manifests().toolbox_name;
}

bool isRegistered() {
  return g_registered;
}

namespace {

// Fetches plugins/arrow.wasm and dlopen()s it. A failed download is not cached: the next call downloads again.
// Once dlopen() has run, later calls return its outcome. Blocks for the download and the link, so only on the
// loader thread: dlopen() waits for every thread to link the module, the main thread included, which can only do
// so once it is back in the browser's event loop.
QString loadBlocking() {
  const std::scoped_lock lock(g_load_mutex);
  if (g_outcome.has_value()) {
    return *g_outcome;
  }
  const double start = emscripten_get_now();
  size_t bytes = 0;
  if (QString error = fetchModule(bytes); !error.isEmpty()) {
    qCWarning(lcSideModule).noquote() << "PJ_WASM_SIDE_MODULE_FAILED" << kModuleUrl << error;
    return error;
  }
  const double fetched = emscripten_get_now();
  // Cached either way from here: dlopen() was attempted, and Emscripten cannot retry or undo a partial link.
  const QString error = linkModule();
  const double linked = emscripten_get_now();
  g_timing = u"bytes=%1 fetch_ms=%2 link_ms=%3"_s.arg(static_cast<qulonglong>(bytes))
                 .arg(qRound(fetched - start))
                 .arg(qRound(linked - fetched));
  if (error.isEmpty()) {
    qCInfo(lcSideModule).noquote() << "PJ_WASM_SIDE_MODULE_LOADED" << kModuleUrl << g_timing;
  } else {
    qCWarning(lcSideModule).noquote() << "PJ_WASM_SIDE_MODULE_FAILED" << kModuleUrl << error;
  }
  g_outcome = error;
  return error;
}

// Main thread from here on.
bool g_loading = false;  // a loader thread runs
struct Waiter {
  QPointer<QObject> context;
  std::function<void(const QString&)> done;
};
std::vector<Waiter> g_waiters;

void finishLoad(PJ::ExtensionCatalogService& catalog, const QString& error) {
  g_loading = false;
  if (error.isEmpty()) {
    g_registered = true;
    if (!catalog.registerLatePlugins(g_plugins)) {
      qCWarning(lcSideModule) << "Some plugins of" << kModuleUrl << "failed to register";
    }
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
    // Read by the browser tests: Firefox can drop the console line.
    const QByteArray timing = g_timing.toUtf8();
    EM_ASM({ globalThis.pjWasmSideModule = UTF8ToString($0); }, timing.constData());
#endif
  } else {
    catalog.deferredParsersFailed();
  }
  for (const Waiter& waiter : std::exchange(g_waiters, {})) {
    if (waiter.context != nullptr) {
      waiter.done(error);
    }
  }
}

// Starts the self-ending loader thread, unless the module is registered or loading. Nothing joins that thread:
// its dlopen() finishes only once the main thread is back in the browser's event loop.
void startLoad(PJ::ExtensionCatalogService& catalog) {
  if (g_registered || g_loading) {
    return;
  }
  g_loading = true;
  auto error = std::make_shared<QString>();  // written by the thread before `finished`, read after it
  QThread* thread = QThread::create([error]() { *error = loadBlocking(); });
  QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  QObject::connect(
      thread, &QThread::finished, &catalog, [&catalog, error]() { finishLoad(catalog, *error); }, Qt::QueuedConnection);
  thread->start();
}

}  // namespace

void ensureLoaded(PJ::ExtensionCatalogService& catalog, QObject* context, std::function<void(const QString&)> done) {
  if (g_registered) {
    QMetaObject::invokeMethod(context, [done = std::move(done)]() { done({}); }, Qt::QueuedConnection);
    return;
  }
  g_waiters.push_back({context, std::move(done)});
  startLoad(catalog);
}

void installDeferredParsers(PJ::ExtensionCatalogService& catalog) {
  // A poll thread only asks the main thread to start the load: it must never wait on it (setDeferredParsers).
  catalog.setDeferredParsers(parserEncodings(), [&catalog]() {
    if (QThread::isMainThread()) {
      startLoad(catalog);
    } else {
      QMetaObject::invokeMethod(&catalog, [&catalog]() { startLoad(catalog); }, Qt::QueuedConnection);
    }
  });
}

}  // namespace pj_app::arrow_side_module
