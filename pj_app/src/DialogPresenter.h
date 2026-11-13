#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QtCore/QtGlobal>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "DialogWatch.h"
#include "pj_plugins/dialog_protocol.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_widgets/ChromeMetrics.h"

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace PJ {

class DataSourceHandle;
class BrowserFileStore;

namespace dialog_presenter {

// Outcome of presenting a plugin's configuration dialog.
//
// kNoDialog and kPluginContractViolation are NOT interchangeable:
//   * kNoDialog: the plugin doesn't advertise kCapabilityHasDialog.
//     Caller should proceed with whatever config it had on entry.
//   * kPluginContractViolation: the plugin advertised kCapabilityHasDialog
//     but didn't ship a usable vtable, or getDialog() returned a null ctx.
//     This is a plugin bug — silently proceeding produces wrong data with
//     no user-visible error. Caller should fail the operation and surface
//     `error` to the user so the broken plugin can be reinstalled / fixed.
enum class Outcome { kNoDialog, kAccepted, kRejected, kPluginContractViolation };

// Configs returned by an accepted dialog. parser_config is empty when
// the dialog has no pj_parser_slot widget.
struct AcceptedPayload {
  std::string saved_config;
  std::string parser_config;
};

// Result of a data-source dialog. payload is engaged iff outcome == kAccepted;
// error is non-empty iff outcome == kPluginContractViolation.
// NSDMIs keep partial designated-init construction warning-clean under
// -Wmissing-field-initializers (pj_app builds with PJ_WARNING_FLAGS).
struct DataSourceResult {
  Outcome outcome = Outcome::kNoDialog;
  std::optional<AcceptedPayload> payload = std::nullopt;
  std::string error = {};
};

// Inputs for showDataSourceDialog. Preconditions (NOT checked by the helper):
//   * `handle` has been bind()-ed.
//   * Any runtime-host callbacks the plugin needs (message-box, etc.) are
//     installed on the runtime host before bind() — see RuntimeHost in
//     FileLoader.cpp for the pattern.
// The async helper does not own `handle` or `catalog`; both must outlive its
// completion. FileLoader keeps them in its pending-load context.
struct DataSourceRequest {
  const LoadedDataSource& source;
  DataSourceHandle& handle;
  const ExtensionCatalogService& catalog;
  QWidget* parent = nullptr;
  std::string_view initial_parser_config{};
  std::shared_ptr<BrowserFileStore> browser_file_store = {};
  // The app's live chrome metrics, used to size any SectionHeaderBand in the
  // plugin dialog to the canonical band height. Leave unset for callers whose
  // dialogs have no bands (e.g. file loaders) or that lack live app metrics.
  std::optional<ChromeMetrics> section_band_metrics{};
  // Put before the plugin dialog's own title (who asked for it); empty leaves it alone.
  QString title_prefix{};
  DialogWatch on_dialog{};
};

// Caller decides what to persist and whether to re-loadConfig before start().
DataSourceResult showDataSourceDialog(const DataSourceRequest& req);

using DataSourceCompletion = std::function<void(DataSourceResult)>;

// Browser-safe presentation path. Returns immediately and completes after the
// dialog is accepted, rejected, destroyed with its parent, or cancelled via the
// returned functor: that one synchronously closes the dialog, delivers exactly
// one on_rejected to the plugin, and runs the completion with kRejected before
// returning. Callers invoke it to tear the dialog down while the plugin context
// is still alive (shutdown). It is empty on the no-dialog / contract-error paths.
[[nodiscard]] DialogCancelFn showDataSourceDialogAsync(const DataSourceRequest& req, DataSourceCompletion completion);

}  // namespace dialog_presenter

}  // namespace PJ
