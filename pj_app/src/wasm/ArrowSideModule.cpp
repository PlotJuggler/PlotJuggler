// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "wasm/ArrowSideModule.h"

#include <dlfcn.h>
#include <emscripten.h>
#include <emscripten/fetch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QLoggingCategory>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

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

// Downloads the module into MEMFS: dlopen() then copies it into shared memory once, where every thread links it
// from, instead of each thread fetching the URL again.
QString fetchModule(size_t& bytes) {
  emscripten_fetch_attr_t attr;
  emscripten_fetch_attr_init(&attr);
  std::strcpy(attr.requestMethod, "GET");
  attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;
  attr.timeoutMSecs = 60000;  // a stalled download fails (and is retried by the next use) instead of hanging the load
  emscripten_fetch_t* fetch = emscripten_fetch(&attr, kModuleUrl);
  if (fetch == nullptr) {
    return u"could not request %1"_s.arg(QLatin1StringView(kModuleUrl));
  }
  QString error;
  if (fetch->status == 0) {
    error = u"downloading %1 failed (network error or timeout)"_s.arg(QLatin1StringView(kModuleUrl));
  } else if (fetch->status != 200) {
    error = u"downloading %1 failed (HTTP %2)"_s.arg(QLatin1StringView(kModuleUrl)).arg(fetch->status);
  } else {
    bytes = static_cast<size_t>(fetch->numBytes);
    mkdir(kModuleDir, 0755);
    FILE* file = std::fopen(kModulePath, "wb");
    if (file == nullptr || std::fwrite(fetch->data, 1, bytes, file) != bytes) {
      error = u"could not stage %1"_s.arg(QLatin1StringView(kModulePath));
    }
    if (file != nullptr) {
      std::fclose(file);
    }
  }
  emscripten_fetch_close(fetch);
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

QString loadBlocking() {
  const std::scoped_lock lock(g_load_mutex);
  if (g_outcome.has_value()) {
    return *g_outcome;
  }
  Q_ASSERT(!QThread::isMainThread());
  const double start = emscripten_get_now();
  size_t bytes = 0;
  if (QString error = fetchModule(bytes); !error.isEmpty()) {
    // Not cached: nothing was linked, so the next use downloads again (a 503, a dropped connection, a timeout).
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

void registerWith(PJ::ExtensionCatalogService& catalog) {
  if (g_registered) {
    return;
  }
  {
    const std::scoped_lock lock(g_load_mutex);
    if (!g_outcome.has_value() || !g_outcome->isEmpty()) {
      return;
    }
  }
  g_registered = true;
  if (!catalog.registerLatePlugins(g_plugins)) {
    qCWarning(lcSideModule) << "Some plugins of" << kModuleUrl << "failed to register";
  }
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  // Read by the browser tests: Firefox can drop the console line above.
  const QByteArray timing = g_timing.toUtf8();
  EM_ASM({ globalThis.pjWasmSideModule = UTF8ToString($0); }, timing.constData());
#endif
}

void ensureLoaded(PJ::ExtensionCatalogService& catalog, QObject* context, std::function<void(const QString&)> done) {
  // Written by the worker before `finished`, read after it: a failed download is not cached, so it cannot be asked
  // for again here, on the main thread.
  auto error = std::make_shared<QString>();
  QThread* thread = QThread::create([error]() { *error = loadBlocking(); });
  QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  QObject::connect(
      thread, &QThread::finished, context,
      [&catalog, error, done = std::move(done)]() {
        if (error->isEmpty()) {
          registerWith(catalog);
        }
        done(*error);
      },
      Qt::QueuedConnection);
  thread->start();
}

void installDeferredParsers(PJ::ExtensionCatalogService& catalog) {
  catalog.setDeferredParsers(parserEncodings(), [&catalog]() {
    if (QThread::isMainThread()) {
      qCWarning(lcSideModule) << "An Arrow parser was needed on the main thread, where" << kModuleUrl
                              << "cannot be linked";
      return;
    }
    if (loadBlocking().isEmpty()) {
      QMetaObject::invokeMethod(&catalog, [&catalog]() { registerWith(catalog); }, Qt::BlockingQueuedConnection);
    }
  });
}

}  // namespace pj_app::arrow_side_module
