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

// Main thread. Calls done(error) on the main thread, unless `context` was destroyed first, once the module is
// registered with the catalog (error empty) or its load failed. The load runs on a loader thread that nothing
// joins: a failed download (HTTP or network error, 60 s timeout) is retried by the next request; a failed link
// is final.
void ensureLoaded(PJ::ExtensionCatalogService& catalog, QObject* context, std::function<void(const QString&)> done);

// Hands the catalog the deferred parsers: a source's poll thread that meets one of their encodings starts the load
// and waits for it only until its source stops (ExtensionCatalogService::setDeferredParsers).
void installDeferredParsers(PJ::ExtensionCatalogService& catalog);

}  // namespace pj_app::arrow_side_module
