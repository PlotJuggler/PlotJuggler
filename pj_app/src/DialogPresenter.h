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
#include <vector>

#include "DialogWatch.h"
#include "LoadInput.h"
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

// kNoDialog: the plugin has no dialog; proceed with the config it had. kPluginContractViolation: it advertises a
// dialog but has no usable vtable or context; fail the load and show `error` (proceeding would load wrong data
// silently).
enum class Outcome { kNoDialog, kAccepted, kRejected, kPluginContractViolation };

struct AcceptedPayload {
  std::string saved_config;
  std::string parser_config;  // empty without a pj_parser_slot widget
  // Browser builds: the staged copies of files picked in the dialog. Keep them while the plugin reads them.
  std::vector<std::shared_ptr<void>> selected_files = {};
};

// payload iff kAccepted; error iff kPluginContractViolation. The member initializers keep partial designated
// initialization clean under -Wmissing-field-initializers.
struct DataSourceResult {
  Outcome outcome = Outcome::kNoDialog;
  std::optional<AcceptedPayload> payload = std::nullopt;
  std::string error = {};
};

// Unchecked preconditions: `handle` is bound, and the runtime-host callbacks the plugin needs (message box) were
// installed before bind(). `handle` and `catalog` are borrowed and must outlive the completion.
struct DataSourceRequest {
  const LoadedDataSource& source;
  DataSourceHandle& handle;
  const ExtensionCatalogService& catalog;
  QWidget* parent = nullptr;
  std::string_view initial_parser_config{};
  std::shared_ptr<BrowserFileStore> browser_file_store = {};
  // Sizes the dialog's SectionHeaderBands; unset when there are none or no live metrics.
  std::optional<ChromeMetrics> section_band_metrics{};
  // Put before the plugin dialog's own title (who asked for it); empty leaves it alone.
  QString title_prefix{};
  DialogWatch on_dialog{};
  // Browser builds: told of every file the dialog's picker stages, before the plugin sees it.
  std::function<void(const LoadInput&)> on_browser_file_staged{};
};

// Blocks in a nested event loop (desktop only). Caller decides what to persist and whether to re-loadConfig.
DataSourceResult showDataSourceDialog(const DataSourceRequest& req);

using DataSourceCompletion = std::function<void(DataSourceResult)>;

// Returns at once (the no-dialog and contract-error paths complete before returning, with an empty functor). The
// returned functor closes the dialog synchronously: one on_rejected to the plugin, then the completion with
// kRejected. Call it while the plugin context is alive (shutdown).
[[nodiscard]] DialogCancelFn showDataSourceDialogAsync(const DataSourceRequest& req, DataSourceCompletion completion);

}  // namespace dialog_presenter

}  // namespace PJ
