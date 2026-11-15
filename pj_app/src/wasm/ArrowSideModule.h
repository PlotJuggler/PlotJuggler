#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QStringList>
#include <functional>
#include <string>
#include <vector>

class QObject;

namespace PJ {
class ExtensionCatalogService;
}

// With PJ_WASM_ARROW_SIDE_MODULE the browser app does not link the Arrow plugin family (Parquet loader, Arrow IPC
// parser, Data Exporter) into its main module: it ships as the Emscripten side module plugins/arrow.wasm beside
// plotjuggler4.wasm, fetched and linked the first time a Parquet file, an Arrow channel or the exporter needs it.
// Until then the app knows the family only from the manifests it keeps (file extensions, encodings, toolbox name).
namespace pj_app::arrow_side_module {

// File extensions (".parquet") a load needs the module for, lower case.
[[nodiscard]] const QStringList& fileExtensions();
// Their file-dialog patterns ("*.parquet").
[[nodiscard]] const QString& filePatterns();
// Encodings of the parsers the module carries.
[[nodiscard]] const std::vector<std::string>& parserEncodings();
// Toolbox the module carries: its id and display name.
[[nodiscard]] const QString& toolboxId();
[[nodiscard]] const QString& toolboxName();

// Whether registerWith() has run: the catalog then has the family. Main thread.
[[nodiscard]] bool isRegistered();

// Fetches plugins/arrow.wasm into the in-memory filesystem and dlopen()s it. Empty on success, else why it failed.
// A failed download (HTTP or network error, 60 s timeout) is retried by the next call; once dlopen() has run, later
// calls return its outcome without doing anything. Blocks for the download and the link.
// NEVER on the main thread: the browser refuses to compile a module this large synchronously there, and dlopen()
// waits for every thread to link the module too, which deadlocks a main thread that cannot return to the browser.
QString loadBlocking();

// Registers the loaded family with the catalog (once; a no-op before a successful loadBlocking()). Main thread.
void registerWith(PJ::ExtensionCatalogService& catalog);

// Main thread: loadBlocking() on a worker thread, then registerWith(), then done(error) on the main thread, unless
// `context` was destroyed first.
void ensureLoaded(PJ::ExtensionCatalogService& catalog, QObject* context, std::function<void(const QString&)> done);

// Hands the catalog the deferred parsers, so a source's poll thread that meets one of their encodings loads the
// module (registering on the main thread) before resolving it.
void installDeferredParsers(PJ::ExtensionCatalogService& catalog);

}  // namespace pj_app::arrow_side_module
