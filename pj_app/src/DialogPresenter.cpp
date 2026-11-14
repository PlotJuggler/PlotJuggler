// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "DialogPresenter.h"

#include <QLoggingCategory>
#include <QPointer>
#include <QString>
#include <memory>
#include <optional>
#include <utility>

#include "FileSelectionService.h"
#include "pj_base/data_source_protocol.h"
#include "pj_plugins/host/data_source_handle.hpp"
#include "pj_plugins/host/data_source_library.hpp"
#include "pj_plugins/host/dialog_handle.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host_qt/dialog_engine.hpp"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PluginRuntimeCatalog.h"
#include "pj_widgets/MessageBox.h"

using namespace Qt::StringLiterals;

namespace PJ::dialog_presenter {

namespace {

Q_LOGGING_CATEGORY(lcDialogPresenter, "pj.app.dialogpresenter")

// Adapter from ExtensionCatalogService::findParserByEncoding to the
// QueryParserDialogFn shape DialogEngine expects. Returns the parser's
// dialog vtable for a given encoding, or nullptr when the parser doesn't
// exist or doesn't expose a dialog. Captures the catalog by reference —
// callers must keep the catalog alive for the duration of the dialog.
QueryParserDialogFn makeParserDialogProvider(const ExtensionCatalogService& catalog) {
  return [&catalog](const std::string& encoding) -> const PJ_dialog_vtable_t* {
    const auto* parser = catalog.findParserByEncoding(QString::fromStdString(encoding));
    if (parser == nullptr) {
      return nullptr;
    }
    auto vtable = parser->library.resolveDialogVtable();
    return vtable ? *vtable : nullptr;
  };
}

#ifdef PJ_TARGET_WASM
DialogFileSelector makeBrowserFileSelector(std::shared_ptr<BrowserFileStore> store) {
  if (!store) {
    return {};
  }
  return [store = std::move(store)](
             QWidget* parent, const std::string& name_filter, const std::string& /*title*/,
             DialogFileSelectionCompletion completion) {
    FileSelectionService::selectAndStageFile(
        parent, QString::fromStdString(name_filter), store,
        [guarded_parent = QPointer<QWidget>(parent),
         completion = std::move(completion)](FileSelectionService::StagedSelection staged) mutable {
          if (!staged.input.has_value()) {
            if (!staged.error.isEmpty()) {
              qCWarning(lcDialogPresenter).noquote() << staged.error;
              // Staging failure is distinct from a user cancel (empty error, empty
              // input) and must be visible — otherwise it silently looks like the
              // user backed out of the picker. The plugin dialog stays open behind
              // this so the user can retry the file selection. The requesting
              // dialog may have been destroyed by the time this late completion
              // fires (async staging chain outlives it); QPointer detects that so
              // we don't dereference a dangling parent — the warning above still
              // surfaces the failure, we just skip the MessageBox that would
              // otherwise anchor a retry to a widget that's gone.
              if (QWidget* parent = guarded_parent.data(); parent != nullptr) {
                auto* box = new MessageBox(parent);
                box->setAttribute(Qt::WA_DeleteOnClose);
                box->setWindowModality(Qt::ApplicationModal);
                box->setTitle(QObject::tr("File selection failed"));
                const QString display_name =
                    staged.browser_name.isEmpty() ? QObject::tr("selected file") : staged.browser_name;
                box->setText(u"%1: %2"_s.arg(display_name, staged.error));
                box->addButton(QObject::tr("OK"), MessageBox::kPrimaryRole);
                box->show();
              }
            }
            completion(std::nullopt);
            return;
          }
          completion(
              DialogSelectedFile{
                  .backing_path = staged.input->backing_path.toStdString(),
                  .lease = std::move(staged.input->lease),
              });
        });
  };
}
#endif

DataSourceResult contractViolation(const std::string& plugin_name, const std::string& reason) {
  qCWarning(lcDialogPresenter).noquote() << "Plugin" << QString::fromStdString(plugin_name)
                                         << "advertises kCapabilityHasDialog but" << QString::fromStdString(reason);
  return {.outcome = Outcome::kPluginContractViolation, .error = "plugin '" + plugin_name + "': " + reason};
}

// Shared prologue of both entry points below. `prepared` engaged => run the
// dialog with these engine inputs; otherwise `immediate` is the result the
// caller returns/completes without showing anything (no dialog capability, or
// a plugin contract violation).
struct DialogPrologue {
  struct Prepared {
    DialogHandle handle;
    DialogEngineConfig config;
  };
  std::optional<Prepared> prepared;
  DataSourceResult immediate;
};

DialogPrologue prepareDialog(const DataSourceRequest& req) {
  if ((req.source.capabilities & PJ_DATA_SOURCE_CAPABILITY_HAS_DIALOG) == 0) {
    return {};
  }

  auto vtable_result = req.source.library.resolveDialogVtable();
  if (!vtable_result) {
    return {
        .prepared = std::nullopt,
        .immediate = contractViolation(req.source.name, "resolveDialogVtable failed: " + vtable_result.error())};
  }

  const PJ_borrowed_dialog_t borrowed = req.handle.getDialog();
  if (borrowed.ctx == nullptr) {
    return {
        .prepared = std::nullopt, .immediate = contractViolation(req.source.name, "getDialog() returned null context")};
  }

  DialogEngineConfig engine_config;
  engine_config.parser_dialog_provider = makeParserDialogProvider(req.catalog);
  engine_config.section_band_metrics = req.section_band_metrics;
  engine_config.title_prefix = req.title_prefix;
  // string-from-string_view ctor (C++17) handles a default-empty view safely;
  // raw .assign(data(), size()) would be UB when data() is nullptr.
  engine_config.initial_parser_config = std::string(req.initial_parser_config);
#ifdef PJ_TARGET_WASM
  engine_config.file_selector = makeBrowserFileSelector(req.browser_file_store);
#endif

  return {
      .prepared =
          DialogPrologue::Prepared{
              .handle = DialogHandle::borrowed(*vtable_result, borrowed.ctx),
              .config = std::move(engine_config),
          },
      .immediate = {}};
}

}  // namespace

DataSourceResult showDataSourceDialog(const DataSourceRequest& req) {
  DialogPrologue prologue = prepareDialog(req);
  if (!prologue.prepared.has_value()) {
    return std::move(prologue.immediate);
  }

  // Shared only so the closer handed to on_dialog can outlive this call: whoever kept a copy (a
  // queued call) then finds the engine gone instead of a dead stack frame.
  const auto engine =
      std::make_shared<DialogEngine>(std::move(prologue.prepared->handle), std::move(prologue.prepared->config));
  if (req.on_dialog) {
    req.on_dialog([weak = std::weak_ptr<DialogEngine>(engine)] {
      if (const std::shared_ptr<DialogEngine> alive = weak.lock()) {
        alive->cancelActiveDialog();  // a no-op until showDialog opened it, and once it completed
      }
    });
  }
  const DialogResult shown = engine->showDialog(req.parent);
  if (req.on_dialog) {
    req.on_dialog({});
  }
  if (shown == DialogResult::kRejected) {
    return {.outcome = Outcome::kRejected};
  }

  return {
      .outcome = Outcome::kAccepted,
      .payload = AcceptedPayload{.saved_config = engine->savedConfig(), .parser_config = engine->parserConfig()},
  };
}

DialogCancelFn showDataSourceDialogAsync(const DataSourceRequest& req, DataSourceCompletion completion) {
  if (!completion) {
    completion = [](DataSourceResult) {};
  }
  DialogPrologue prologue = prepareDialog(req);
  if (!prologue.prepared.has_value()) {
    completion(std::move(prologue.immediate));
    return {};
  }

  auto engine =
      std::make_shared<DialogEngine>(std::move(prologue.prepared->handle), std::move(prologue.prepared->config));
  // Set once the dialog completed, which openDialog may do before it returns.
  const auto completed = std::make_shared<bool>(false);
  engine->openDialog(
      req.parent,
      [engine, completed, on_dialog = req.on_dialog, completion = std::move(completion)](DialogResult result) mutable {
        *completed = true;
        if (on_dialog) {
          on_dialog({});
        }
        if (result == DialogResult::kRejected) {
          completion({.outcome = Outcome::kRejected});
          return;
        }
        completion({
            .outcome = Outcome::kAccepted,
            .payload =
                AcceptedPayload{
                    .saved_config = engine->savedConfig(),
                    .parser_config = engine->parserConfig(),
                },
        });
      });
  // The canceler co-owns the engine so a shutdown-time cancel finds it alive
  // even after the completion (and its engine reference) is gone; cancelling a
  // completed dialog is a documented no-op.
  DialogCancelFn cancel = [engine]() { engine->cancelActiveDialog(); };
  if (req.on_dialog && !*completed) {
    req.on_dialog(cancel);
  }
  return cancel;
}

}  // namespace PJ::dialog_presenter
