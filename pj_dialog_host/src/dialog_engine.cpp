// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <pj_widgets/Dialog.h>
#include <pj_widgets/FileDialog.h>
#include <pj_widgets/SectionHeaderBand.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QBoxLayout>
#include <QBuffer>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#ifndef Q_OS_WASM
#include <QEventLoop>
#endif
#include <QGroupBox>
#include <QLayout>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSettings>
#ifdef Q_OS_WASM
#include <QScreen>
#endif
#include <QSpacerItem>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <pj_plugins/host/widget_data_view.hpp>
#include <pj_plugins/host_qt/dialog_engine.hpp>
#include <pj_plugins/host_qt/drop_event_filter.hpp>
#include <pj_plugins/host_qt/pj_ui_loader.hpp>
#include <pj_plugins/host_qt/widget_adapters.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>
#include <utility>
#include <vector>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

DialogEngine::DialogEngine(PJ::DialogHandle handle, DialogEngineConfig config)
    : handle_(std::move(handle)), config_(config) {}

namespace {

// Fire-and-forget coroutine used only to retain the dialog setup stack until
// QDialog::finished. final_suspend=suspend_never destroys the coroutine frame
// immediately after the completion callback returns.
struct DetachedDialogTask {
  struct promise_type {
    DetachedDialogTask get_return_object() const noexcept {
      return {};
    }
    std::suspend_never initial_suspend() const noexcept {
      return {};
    }
    std::suspend_never final_suspend() const noexcept {
      return {};
    }
    void return_void() const noexcept {}
    void unhandled_exception() const noexcept {
      std::terminate();
    }
  };
};

// Await QDialog completion without QDialog::exec() or QEventLoop::exec(). The
// destroyed connection covers a parent window disappearing while the dialog is
// pending; QPointer lets the continuation avoid touching the deleted widget.
class DialogFinishedAwaiter {
 public:
  explicit DialogFinishedAwaiter(QDialog* dialog) : dialog_(dialog) {}

  bool await_ready() const noexcept {
    return dialog_.isNull();
  }

  void await_suspend(std::coroutine_handle<> continuation) {
    state_ = std::make_shared<State>();
    state_->continuation = continuation;

    auto resume_once = [state = state_](int result) {
      if (state->resumed) {
        return;
      }
      state->resumed = true;
      state->result = result;
      state->continuation.resume();
    };
    QObject::connect(dialog_, &QDialog::finished, qApp, resume_once);
    QObject::connect(dialog_, &QObject::destroyed, qApp, [resume_once]() { resume_once(QDialog::Rejected); });
  }

  int await_resume() const noexcept {
    return state_ ? state_->result : QDialog::Rejected;
  }

 private:
  struct State {
    std::coroutine_handle<> continuation;
    int result = QDialog::Rejected;
    bool resumed = false;
  };

  QPointer<QDialog> dialog_;
  std::shared_ptr<State> state_;
};

// Wraps a callback handed to sender-owned connections (widget signals, drop
// filter) with a liveness flag. Each connection stores its own std::function
// copy inside the SENDER widget, which lives until the dialog's deferred
// deleteLater — i.e. past the coroutine frame the [&] captures reference. Once
// the flag is cleared (at dialog completion) a late signal — a queued event, a
// focus-out during teardown, or a signal emitted from a child's destructor —
// becomes a no-op instead of a call into a destroyed frame.
template <typename Fn>
auto guardedCallback(std::shared_ptr<const bool> alive, Fn fn) {
  return [alive = std::move(alive), fn = std::move(fn)](auto&&... args) {
    if (*alive) {
      fn(std::forward<decltype(args)>(args)...);
    }
  };
}

// QUiLoader creates plugin section bands with their class default. Match them
// to the live application chrome only when the caller supplied its metrics.
void applySectionBandMetrics(QWidget* root, const std::optional<ChromeMetrics>& metrics) {
  if (root == nullptr || !metrics.has_value()) {
    return;
  }
  for (auto* band : root->findChildren<SectionHeaderBand*>()) {
    band->onChromeMetricsChanged(*metrics);
  }
}

// Opt-in via the `pjButtonsFillWidth` .ui property on a QDialogButtonBox: make
// its buttons span the box width instead of hugging one edge. QDialogButtonBox
// lays its buttons out in a QBoxLayout padded with style-driven stretch spacers;
// a plugin .ui cannot reach those inner items, so the host neutralizes the
// spacers' stretch and gives each button an Expanding policy + stretch weight.
void applyButtonBoxFillWidth(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  for (auto* box : root->findChildren<QDialogButtonBox*>()) {
    if (!box->property("pjButtonsFillWidth").toBool()) {
      continue;
    }
    auto* box_layout = qobject_cast<QBoxLayout*>(box->layout());
    if (box_layout == nullptr) {
      continue;
    }
    box_layout->setContentsMargins(0, 0, 0, 0);
    // Framework-based padding so a filled button reads with snug breathing room
    // rather than hugging its text; the widget stylesheet cascades over the app
    // QSS, so border/background/hover still come from the theme.
    const QString button_padding =
        QStringLiteral("QPushButton { padding: %1px; }").arg(theme::space(theme::Space::Snug));
    for (int i = 0; i < box_layout->count(); ++i) {
      QLayoutItem* item = box_layout->itemAt(i);
      if (QWidget* button = item->widget()) {
        button->setSizePolicy(QSizePolicy::Expanding, button->sizePolicy().verticalPolicy());
        button->setStyleSheet(button_padding);
        box_layout->setStretch(i, 1);
      } else {
        // A style-inserted stretch spacer — stop it from eating the width.
        box_layout->setStretch(i, 0);
      }
    }
  }
}

// QUiLoader applies a root QDialog's authored <geometry> to QWidget::size(),
// but that size is not part of the widget's sizeHint once the root is embedded
// as declarative content. Preserve the authored content extent and add only the
// canonical host title bar. Plain QWidget roots have no authored window extent
// and continue to size from their layout hints.
QSize authoredDialogContentSize(QWidget* root) {
  return qobject_cast<QDialog*>(root) != nullptr ? root->size() : QSize{};
}

void applyAuthoredDialogContentSize(PJ::Dialog* host, const QSize& content_size) {
  if (host == nullptr || !content_size.isValid() || content_size.isEmpty()) {
    return;
  }
  const auto* title_bar = host->findChild<QWidget*>(QStringLiteral("dialogTitleBar"));
  const int title_height = title_bar != nullptr ? title_bar->height() : 0;
  host->resize(content_size + QSize(0, title_height));
}

}  // namespace

// ---------------------------------------------------------------------------
// JSON diff: compute which widget keys changed between old and new data
// ---------------------------------------------------------------------------

static nlohmann::json computeDiff(const nlohmann::json& old_data, const nlohmann::json& new_data) {
  nlohmann::json diff = nlohmann::json::object();
  for (const auto& [key, val] : new_data.items()) {
    if (!old_data.contains(key) || old_data[key] != val) {
      diff[key] = val;
    }
  }
  return diff;
}

// ---------------------------------------------------------------------------
// apply_and_diff: re-read widget data, apply (diffed or full), update prev
// ---------------------------------------------------------------------------

/// Holds the result of applying widget data: whether accept was requested and
/// whether a sub-dialog was requested (with its UI XML).
struct ApplyResult {
  bool wants_accept = false;
  std::optional<std::string> sub_dialog_ui;
};

/// The engine's memory of the last payload delivered to a widget tree: the
/// parsed per-widget state (key-diffing) plus the raw bytes (byte-identical
/// skip guard). The two are always mutated together.
struct PayloadMemo {
  nlohmann::json data = nlohmann::json::object();
  std::string raw;
};

static ApplyResult applyAndDiff(
    QWidget* root, PJ::DialogHandle& handle, PayloadMemo& memo, const PJ::DialogEngineConfig& config,
    PJ::DialogEngine::Stats& stats) {
  const bool enable_diff = config.enable_diff;
  int& diff_apply_count = stats.diff_apply_count;
  std::string raw = handle.widget_data();
  // A plugin that reports "changed" but re-emits byte-identical widget data
  // pays nothing: skip the parse + diff + apply (same guard as PanelEngine).
  // One-shot requests (accept/sub-dialog) flip the bytes, so they still fire.
  if (raw == memo.raw) {
    ++stats.skipped_identical_count;
    return {};
  }
  memo.raw = raw;
  nlohmann::json new_data = nlohmann::json::parse(raw, nullptr, false);
  if (new_data.is_discarded()) {
    return {};
  }

  PJ::WidgetDataView full_view(raw);
  ApplyResult result;
  result.wants_accept = full_view.requestAccept();
  result.sub_dialog_ui = full_view.subDialogUi();

  // Strip commands before diffing (they're one-shot)
  new_data.erase("__request_accept");
  new_data.erase("__request_sub_dialog");

  if (enable_diff) {
    nlohmann::json diff = computeDiff(memo.data, new_data);
    if (!diff.empty()) {
      PJ::WidgetDataView view(diff.dump());
      applyWidgetData(root, view);
      ++diff_apply_count;
    }
  } else {
    applyWidgetData(root, full_view);
  }
  memo.data = std::move(new_data);
  return result;
}

// ---------------------------------------------------------------------------
// show_dialog
// ---------------------------------------------------------------------------

struct DialogEngine::AsyncRunner {
  static DetachedDialogTask run(DialogEngine& self, QWidget* parent, Completion completion) {
    auto& handle_ = self.handle_;
    auto& config_ = self.config_;
    auto& stats_ = self.stats_;
    auto& parser_config_ = self.parser_config_;
    stats_ = {};

    // 1. Load .ui
    std::string ui = handle_.ui_content();
    QByteArray data(ui.data(), static_cast<int>(ui.size()));
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);

    PjUiLoader loader;
    QWidget* loaded = loader.load(&buffer, parent);
    if (!loaded) {
      self.dialog_open_ = false;
      completion(DialogResult::kRejected);
      co_return;
    }
    adaptStyledWidgets(loaded);
    const QSize authored_content_size = authoredDialogContentSize(loaded);

    // 2. Plugin UIs are declarative content. The host owns their window chrome,
    // so embed every root (including QDialog roots) in the canonical app dialog.
    auto* dialog = new PJ::Dialog(parent);
    if (config_.section_band_metrics) {
      dialog->setChromeMetrics(*config_.section_band_metrics);
    }
    dialog->setDialogTitle(loaded->windowTitle());
    dialog->contentLayout()->addWidget(loaded);
    forwardEmbeddedDialogClose(loaded, dialog);

    // Dialogs with a parser slot embed a parser-options widget whose height varies
    // with the chosen message protocol (a single msgpack checkbox vs a tall
    // Protobuf table). A fixed .ui size can't fit both without dead space or
    // clipping, so for these — and only these — the host sizes the dialog to its
    // actual content at runtime. Plain dialogs (topic-table loaders, toolboxes…)
    // have no parser slot and keep their authored .ui size untouched.
    const bool content_fit_dialog = loaded->findChild<QWidget*>("pj_parser_slot") != nullptr;
    if (!content_fit_dialog) {
      applyAuthoredDialogContentSize(dialog, authored_content_size);
    }

    auto clamp_to_browser_screen = [dialog]() {
#ifdef Q_OS_WASM
      // A browser canvas is the whole available desktop. Authored plugin
      // dialogs can be taller than it (MCAP is 732 px before its title bar),
      // which otherwise leaves the button box outside the clickable canvas.
      if (const QScreen* screen = dialog->screen()) {
        constexpr int kWindowFrameAllowance = 48;
        const QSize available =
            screen->availableGeometry().size() - QSize(kWindowFrameAllowance, kWindowFrameAllowance);
        const QSize maximum = available.expandedTo(QSize(320, 240));
        // Lower any authored floor before the cap. Qt otherwise keeps a minimum
        // larger than the browser canvas and the footer remains unreachable.
        dialog->setMinimumSize(dialog->minimumSize().boundedTo(maximum));
        dialog->setMaximumSize(maximum);
        dialog->resize(dialog->size().boundedTo(maximum));
      }
#endif
    };

    // Snap the dialog to its real content: drop the authored fixed minimum heights
    // (the dialog's own and the parser slot's reservation) and collapse expanding
    // vertical spacers so there's no dead space, but give tables/lists/editors a
    // readable minimum so they don't shrink to a tiny default. Idempotent — safe
    // to call on every protocol change.
    auto fit_to_content = [loaded, dialog, clamp_to_browser_screen]() {
      QWidget* slot = loaded->findChild<QWidget*>("pj_parser_slot");
      if (slot == nullptr) {
        return;
      }
      // Reset both dimensions: the floor installed after the prior parser page
      // must not prevent a narrower/shorter page from being measured afresh.
      loaded->setMinimumSize(0, 0);
      dialog->setMinimumSize(0, 0);
      slot->setMinimumSize(0, 0);
      std::function<void(QLayout*)> collapse_vspacers = [&](QLayout* layout) {
        if (layout == nullptr) {
          return;
        }
        for (int i = 0; i < layout->count(); ++i) {
          QLayoutItem* item = layout->itemAt(i);
          if (QSpacerItem* spacer = item->spacerItem()) {
            if (spacer->expandingDirections() & Qt::Vertical) {
              // Remove the size hint while retaining vertical expansion so a
              // short page stays top-aligned when its splitter pane is taller.
              spacer->changeSize(
                  theme::space(theme::Space::None), theme::space(theme::Space::None), QSizePolicy::Minimum,
                  QSizePolicy::Expanding);
            }
          } else if (QLayout* child = item->layout()) {
            collapse_vspacers(child);
          }
        }
        layout->invalidate();
      };
      collapse_vspacers(loaded->layout());
      // Splitters own panes as widgets, so their nested layouts are not reached
      // by the recursion above.
      for (QWidget* descendant : loaded->findChildren<QWidget*>()) {
        collapse_vspacers(descendant->layout());
      }
      // Parser options that carry a table (Protobuf, ROS1/ROS2) need room to be
      // usable; give such tables/editors a generous minimum height. Protocols
      // without a table (msgpack/cbor/json) have no item view here and stay
      // compact.
      const auto ensure_readable = [](QWidget* w) {
        if (w->minimumHeight() < 300) {
          w->setMinimumHeight(300);
        }
      };
      for (QAbstractItemView* view : loaded->findChildren<QAbstractItemView*>()) {
        ensure_readable(view);
      }
      for (QPlainTextEdit* editor : loaded->findChildren<QPlainTextEdit*>()) {
        ensure_readable(editor);
      }
      if (dialog->layout() != nullptr) {
        dialog->layout()->activate();
      }
      dialog->adjustSize();
      clamp_to_browser_screen();
      // Reinstall the real content floor, capped to the browser maximum. This
      // prevents resize-crush on desktop without recreating an unreachable WASM
      // dialog when content is taller than the canvas.
      dialog->setMinimumSize(dialog->minimumSizeHint().boundedTo(dialog->maximumSize()));
    };

    // Owner side of guardedCallback(): flipped to false the moment the dialog
    // completes, severing every widget/drop callback that captures this frame
    // by reference before the frame dies at co_return.
    auto callbacks_alive = std::make_shared<bool>(true);

    // Wire buttonBox signals — works whether the loaded widget was a QDialog
    // or a plain QWidget. Needed so Close/OK/Cancel buttons function correctly.
    {
      auto* button_box = loaded->findChild<QDialogButtonBox*>("buttonBox");
      if (button_box) {
        QObject::connect(button_box, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
        QObject::connect(button_box, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
      } else {
        qWarning(
            "DialogEngine: no QDialogButtonBox named 'buttonBox' found in UI. "
            "OK/Cancel buttons will not work. Ensure your .ui XML has: "
            "<widget class=\"QDialogButtonBox\" name=\"buttonBox\">");
      }
    }

    // Restore saved dialog geometry (keyed by plugin manifest name)
    auto manifest_json = nlohmann::json::parse(handle_.manifest(), nullptr, false);
    std::string plugin_name = manifest_json.is_object() ? manifest_json.value("name", "") : "";
    QString geometry_key = QString("DialogGeometry/%1").arg(QString::fromStdString(plugin_name));
    // Content-fit dialogs never restore a saved size — it would override the fit
    // (a stale large geometry re-applied on show is exactly what left dead space).
    if (!plugin_name.empty() && !content_fit_dialog) {
      QSettings settings;
      auto saved = settings.value(geometry_key).toByteArray();
      if (!saved.isEmpty()) {
        dialog->restoreGeometry(saved);
      }
    }

    // Task 7: detect parser slot widget
    stats_.has_parser_slot = loaded->findChild<QWidget*>("pj_parser_slot") != nullptr;

    // --- Parser slot injection state ---
    QWidget* parser_slot = nullptr;
    QWidget* parser_slot_container = nullptr;  // Parent GroupBox to show/hide
    QVBoxLayout* parser_slot_layout = nullptr;
    QWidget* parser_dialog_widget = nullptr;
    std::unique_ptr<PJ::DialogHandle> parser_dialog_handle;
    PayloadMemo parser_memo;
    // Staged-file leases retained per target widget. A lease keeps the picker's
    // MEMFS copy alive because the plugin may re-read backing_path any time until
    // the dialog completes (e.g. on accept, when it opens the file for real). We
    // key by a scope-qualified name (top-level dialog vs. parser sub-dialog +
    // generation — see the completion below) and REPLACE on re-selection so a
    // superseded pick frees its staged copy immediately instead of pinning every
    // intermediate choice until dialog close. A pick the plugin rejects retains
    // nothing.
    std::vector<std::pair<std::string, std::shared_ptr<void>>> selected_file_leases;
    bool file_picker_active = false;
    std::uint64_t parser_dialog_generation = 0;

    // Parameterized file/folder picker handlers (work for any dialog handle)
    auto show_file_picker_for = [&](const std::string& widget_name, PJ::DialogHandle* handle, QWidget* target_widget,
                                    PayloadMemo& target_memo) {
      if (!config_.enable_file_picker || !handle) {
        return;
      }
      PJ::WidgetDataView view(handle->widget_data());
      if (!view.isFilePicker(widget_name)) {
        return;
      }
      // Returns true iff the plugin accepted the path (onFileSelected returned
      // true); the async completion retains the staging lease only on acceptance.
      auto apply_selected_path = [&, widget_name, handle, target_widget](const std::string& path) -> bool {
        if (!handle->sendEvent(widget_name, PJ::WidgetEventBuilder::fileSelected(path))) {
          return false;
        }
        std::string raw = handle->widget_data();
        nlohmann::json new_data = nlohmann::json::parse(raw, nullptr, false);
        if (!new_data.is_discarded()) {
          new_data.erase("__request_accept");
          new_data.erase("__request_sub_dialog");
          PJ::WidgetDataView updated_view(raw);
          applyWidgetData(target_widget, updated_view);
          target_memo.data = std::move(new_data);
          // The direct picker path applies state without consuming one-shot
          // commands, so the next poll must inspect this payload once.
          target_memo.raw.clear();
        }
        return true;
      };
      auto filter = view.filePickerFilter(widget_name).value_or("");
      auto title = view.filePickerTitle(widget_name).value_or("Select File");
      if (config_.file_selector) {
        if (file_picker_active) {
          return;
        }
        file_picker_active = true;
        const bool targets_parser = handle == parser_dialog_handle.get();
        const std::uint64_t target_generation = parser_dialog_generation;
        config_.file_selector(
            dialog, filter, title,
            [callbacks_alive, &file_picker_active, &parser_dialog_generation, targets_parser, target_generation,
             widget_name, apply_selected_path = std::move(apply_selected_path),
             &selected_file_leases](std::optional<DialogSelectedFile> selected) mutable {
              if (!*callbacks_alive) {
                return;
              }
              file_picker_active = false;
              if (!selected.has_value() || (targets_parser && target_generation != parser_dialog_generation)) {
                return;
              }
              // Only retain the lease if the plugin accepts the path; a rejected
              // pick frees its staged copy at once. Replace any prior lease for
              // this widget so a re-selection supersedes — not accumulates.
              if (apply_selected_path(selected->backing_path)) {
                // Scope-qualify by target (top-level dialog vs. injected parser
                // sub-dialog) plus, for the parser scope, its generation: widget
                // names are chosen by independent plugins and can collide (e.g. a
                // top-level "pick_file" and a parser sub-dialog's own "pick_file").
                // A bare-name key would let one scope's completion replace — and
                // thus free — the other scope's still-needed MEMFS lease.
                const std::string lease_key = targets_parser
                                                  ? "parser:" + std::to_string(target_generation) + ":" + widget_name
                                                  : "top:" + widget_name;
                auto existing = std::find_if(
                    selected_file_leases.begin(), selected_file_leases.end(),
                    [&](const auto& entry) { return entry.first == lease_key; });
                if (existing != selected_file_leases.end()) {
                  existing->second = std::move(selected->lease);
                } else {
                  selected_file_leases.emplace_back(lease_key, std::move(selected->lease));
                }
              }
            });
        return;
      }
#ifdef Q_OS_WASM
      Q_UNUSED(target_widget)
      Q_UNUSED(target_memo)
      qWarning("DialogEngine: no browser content picker was provided for this plugin file request");
#else
      QString path = PJ::FileDialog::getOpenFileName(
          dialog, QString::fromStdString(title), QString(), QString::fromStdString(filter));
      if (!path.isEmpty()) {
        apply_selected_path(path.toStdString());
      }
#endif
    };

    auto show_folder_picker_for = [&](const std::string& widget_name, PJ::DialogHandle* handle, QWidget* target_widget,
                                      PayloadMemo& target_memo) {
      if (!config_.enable_file_picker || !handle) {
        return;
      }
      PJ::WidgetDataView view(handle->widget_data());
      if (!view.isFolderPicker(widget_name)) {
        return;
      }
#ifdef Q_OS_WASM
      Q_UNUSED(target_widget)
      Q_UNUSED(target_memo)
      qWarning("DialogEngine: plugin folder pickers are unavailable on WebAssembly");
#else
      auto title = view.folderPickerTitle(widget_name).value_or("Select Folder");
      QString path = PJ::FileDialog::getExistingDirectory(dialog, QString::fromStdString(title));
      if (!path.isEmpty()) {
        if (handle->sendEvent(widget_name, PJ::WidgetEventBuilder::folderSelected(path.toStdString()))) {
          std::string raw = handle->widget_data();
          nlohmann::json new_data = nlohmann::json::parse(raw, nullptr, false);
          if (!new_data.is_discarded()) {
            new_data.erase("__request_accept");
            new_data.erase("__request_sub_dialog");
            PJ::WidgetDataView v(raw);
            applyWidgetData(target_widget, v);
            target_memo.data = std::move(new_data);
            // The direct picker path applies state without consuming one-shot
            // commands, so the next poll must inspect this payload once.
            target_memo.raw.clear();
          }
        }
      }
#endif
    };

    auto show_save_file_picker_for = [&](const std::string& widget_name, PJ::DialogHandle* handle,
                                         QWidget* target_widget, PayloadMemo& target_memo) {
      if (!config_.enable_file_picker || !handle) {
        return;
      }
      PJ::WidgetDataView view(handle->widget_data());
      if (!view.isSaveFilePicker(widget_name)) {
        return;
      }
#ifdef Q_OS_WASM
      Q_UNUSED(target_widget)
      Q_UNUSED(target_memo)
      qWarning("DialogEngine: path-returning plugin save pickers are unavailable on WebAssembly");
#else
      auto filter = view.filePickerFilter(widget_name).value_or("");
      auto title = view.filePickerTitle(widget_name).value_or("Save File");
      auto suffix = view.saveFilePickerDefaultSuffix(widget_name).value_or("");
      QString path = PJ::FileDialog::getSaveFileName(
          dialog, QString::fromStdString(title), QString(), QString::fromStdString(filter),
          QString::fromStdString(suffix));
      if (!path.isEmpty()) {
        if (handle->sendEvent(widget_name, PJ::WidgetEventBuilder::fileSelected(path.toStdString()))) {
          std::string raw = handle->widget_data();
          nlohmann::json new_data = nlohmann::json::parse(raw, nullptr, false);
          if (!new_data.is_discarded()) {
            new_data.erase("__request_accept");
            new_data.erase("__request_sub_dialog");
            PJ::WidgetDataView v(raw);
            applyWidgetData(target_widget, v);
            target_memo.data = std::move(new_data);
            // The direct picker path applies state without consuming one-shot
            // commands, so the next poll must inspect this payload once.
            target_memo.raw.clear();
          }
        }
      }
#endif
    };

    // Lambda: inject parser dialog for the given encoding
    auto inject_parser_dialog = [&](const QString& encoding) {
      ++parser_dialog_generation;
      // 1. Clear previous parser dialog
      if (parser_dialog_widget) {
        parser_slot_layout->removeWidget(parser_dialog_widget);
        delete parser_dialog_widget;
        parser_dialog_widget = nullptr;
      }
      parser_dialog_handle.reset();
      parser_memo = {};

      // 2. Query parser dialog vtable via provider
      if (!config_.parser_dialog_provider) {
        if (parser_slot_container) {
          parser_slot_container->setVisible(false);
        }
        return;
      }

      const PJ_dialog_vtable_t* vtable = config_.parser_dialog_provider(encoding.toStdString());
      if (vtable == nullptr) {
        // Parser has no dialog - hide the container
        if (parser_slot_container) {
          parser_slot_container->setVisible(false);
        }
        return;
      }

      // 3. Create parser dialog handle
      parser_dialog_handle = std::make_unique<PJ::DialogHandle>(vtable);

      // 3b. Load initial parser config if provided (restores previous state)
      if (!config_.initial_parser_config.empty()) {
        (void)parser_dialog_handle->load_config(config_.initial_parser_config);
      }

      // 4. Load parser dialog UI
      std::string parser_ui = parser_dialog_handle->ui_content();
      QByteArray parser_data(parser_ui.data(), static_cast<int>(parser_ui.size()));
      QBuffer parser_buffer(&parser_data);
      parser_buffer.open(QIODevice::ReadOnly);

      PjUiLoader parser_loader;
      parser_dialog_widget = parser_loader.load(&parser_buffer, parser_slot);
      if (!parser_dialog_widget) {
        parser_dialog_handle.reset();
        if (parser_slot_container) {
          parser_slot_container->setVisible(false);
        }
        return;
      }
      adaptStyledWidgets(parser_dialog_widget);
      applySectionBandMetrics(parser_dialog_widget, config_.section_band_metrics);
      applyButtonBoxFillWidth(parser_dialog_widget);

      // 5. Insert into slot and show container
      parser_slot_layout->addWidget(parser_dialog_widget);
      if (parser_slot_container) {
        parser_slot_container->setVisible(true);
      }
      stats_.parser_dialog_injected = true;

      // 6. Apply initial parser widget data
      std::string parser_initial_raw = parser_dialog_handle->widget_data();
      parser_memo.raw = parser_initial_raw;
      parser_memo.data = nlohmann::json::parse(parser_initial_raw, nullptr, false);
      if (parser_memo.data.is_discarded()) {
        parser_memo.data = nlohmann::json::object();
      }
      {
        PJ::WidgetDataView view(parser_initial_raw);
        applyWidgetData(parser_dialog_widget, view);
      }

      // 7. Wire parser dialog signals (events go to parser handle) + the
      //    declarative "pj_enable_when"/"pj_visible_when" rules (this widget is
      //    injected after the one-time install on binding_root, so it needs its
      //    own pass).
      installDeclarativeRules(parser_dialog_widget);
      connectWidgetSignals(
          parser_dialog_widget,
          guardedCallback(callbacks_alive, [&](const std::string& name, const std::string& event_json) {
            stats_.event_count++;
            if (parser_dialog_handle && parser_dialog_handle->sendEvent(name, event_json)) {
              // Re-apply parser widget data with the same byte-identical guard
              // as the top-level dialog.
              std::string raw = parser_dialog_handle->widget_data();
              if (raw == parser_memo.raw) {
                ++stats_.skipped_identical_count;
              } else if (
                  nlohmann::json new_data = nlohmann::json::parse(raw, nullptr, false); !new_data.is_discarded()) {
                parser_memo.raw = raw;
                new_data.erase("__request_accept");
                new_data.erase("__request_sub_dialog");
                if (config_.enable_diff) {
                  nlohmann::json diff = computeDiff(parser_memo.data, new_data);
                  if (!diff.empty()) {
                    PJ::WidgetDataView view(diff.dump());
                    applyWidgetData(parser_dialog_widget, view);
                  }
                } else {
                  PJ::WidgetDataView view(raw);
                  applyWidgetData(parser_dialog_widget, view);
                }
                parser_memo.data = std::move(new_data);
              }
            }

            // Handle file/folder pickers in parser dialog
            show_file_picker_for(name, parser_dialog_handle.get(), parser_dialog_widget, parser_memo);
            show_folder_picker_for(name, parser_dialog_handle.get(), parser_dialog_widget, parser_memo);
            show_save_file_picker_for(name, parser_dialog_handle.get(), parser_dialog_widget, parser_memo);
          }));
    };

    // Setup parser slot if detected and provider is available
    if (stats_.has_parser_slot && config_.parser_dialog_provider) {
      parser_slot = loaded->findChild<QWidget*>("pj_parser_slot");
      if (parser_slot) {
        parser_slot_container = parser_slot->parentWidget();  // Usually a QGroupBox
        parser_slot_layout = new QVBoxLayout(parser_slot);
        parser_slot_layout->setContentsMargins(
            theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
            theme::space(theme::Space::None));

        // Connect encoding combo to trigger parser dialog injection, then re-fit.
        // The fit is deferred with singleShot(0) so it runs AFTER the layout has
        // measured the freshly-injected widget — calling it synchronously here
        // would size the dialog to the PREVIOUS protocol's options (a one-step lag,
        // where each selection shows the prior protocol's size).
        if (auto* combo = loaded->findChild<QComboBox*>("comboBoxProtocol")) {
          QObject::connect(
              combo, &QComboBox::currentTextChanged, dialog,
              guardedCallback(callbacks_alive, [&, fit_to_content](const QString& encoding) {
                inject_parser_dialog(encoding);
                QTimer::singleShot(0, dialog, [fit_to_content]() { fit_to_content(); });
              }));
          // Note: initial injection happens AFTER widget_data is applied (see below)
        }
      }
    }

    QWidget* binding_root = loaded;

    // 3. Apply initial widget data
    std::string initial_raw = handle_.widget_data();
    PayloadMemo memo;
    memo.data = nlohmann::json::parse(initial_raw, nullptr, false);
    if (memo.data.is_discarded()) {
      memo.data = nlohmann::json::object();
    }
    // Keep raw empty until the first event/tick delivery. The asynchronous
    // engine historically consumes one-shot commands already present in the
    // initial payload on that first delivery (for example a requested
    // sub-dialog); pre-seeding raw here would suppress that command forever.
    {
      PJ::WidgetDataView view(initial_raw);
      applyWidgetData(binding_root, view);
    }

    // 3b. Trigger initial parser dialog injection now that combo is populated,
    // then size the dialog to its content.
    if (parser_slot != nullptr) {
      if (auto* combo = loaded->findChild<QComboBox*>("comboBoxProtocol")) {
        inject_parser_dialog(combo->currentText());
      }
    }
    fit_to_content();
    applySectionBandMetrics(loaded, config_.section_band_metrics);
    applyButtonBoxFillWidth(loaded);

    // The one sub-dialog currently open (pre-branch exec() allowed only one at a
    // time). While it is set, maybe_open_sub_dialog is a no-op — a plugin that
    // leaves __request_sub_dialog set across ticks otherwise spawns a fresh
    // sub-dialog every tick. Cleared in the sub-dialog's finished handler and
    // severed at frame teardown (below) so a shutdown-time reject cannot reach a
    // dead frame.
    QPointer<QDialog> active_sub_dialog;

    // Helper: open a sub-dialog from UI XML (app-modal, matching the pre-branch
    // exec()). `on_finished`, when set, runs after the sub-dialog is dismissed —
    // used to preserve the plugin-visible order of "sub-dialog then file picker"
    // that the old blocking exec() gave for free. When a sub-dialog is already
    // open (or none is requested), on_finished runs immediately so the caller's
    // continuation is never dropped.
    auto maybe_open_sub_dialog = [&, callbacks_alive](const ApplyResult& ar, std::function<void()> on_finished = {}) {
      if (!ar.sub_dialog_ui || !active_sub_dialog.isNull()) {
        if (on_finished) {
          on_finished();
        }
        return;
      }

      QByteArray sub_data(ar.sub_dialog_ui->data(), static_cast<int>(ar.sub_dialog_ui->size()));
      QBuffer sub_buffer(&sub_data);
      sub_buffer.open(QIODevice::ReadOnly);

      PjUiLoader sub_loader;
      QWidget* sub_loaded = sub_loader.load(&sub_buffer, dialog);
      if (!sub_loaded) {
        if (on_finished) {
          on_finished();
        }
        return;
      }
      adaptStyledWidgets(sub_loaded);
      applySectionBandMetrics(sub_loaded, config_.section_band_metrics);
      applyButtonBoxFillWidth(sub_loaded);
      const QSize authored_sub_content_size = authoredDialogContentSize(sub_loaded);

      auto* sub_dialog = new PJ::Dialog(dialog);
      if (config_.section_band_metrics) {
        sub_dialog->setChromeMetrics(*config_.section_band_metrics);
      }
      sub_dialog->setDialogTitle(sub_loaded->windowTitle());
      sub_dialog->contentLayout()->addWidget(sub_loaded);
      applyAuthoredDialogContentSize(sub_dialog, authored_sub_content_size);
      forwardEmbeddedDialogClose(sub_loaded, sub_dialog);
      if (auto* sub_bb = sub_loaded->findChild<QDialogButtonBox*>("buttonBox")) {
        QObject::connect(sub_bb, &QDialogButtonBox::accepted, sub_dialog, &QDialog::accept);
        QObject::connect(sub_bb, &QDialogButtonBox::rejected, sub_dialog, &QDialog::reject);
      }

      // The modal loop below blocks the plugin from driving field states — honor
      // the .ui's declarative "pj_enable_when"/"pj_visible_when" rules here, like
      // PanelEngine does.
      installDeclarativeRules(sub_dialog);

      active_sub_dialog = sub_dialog;
      // Chain any continuation to the dismissal (guarded: at frame teardown the
      // callbacks flag is cleared, so a late finished cannot re-enter a dead
      // frame). Clearing active_sub_dialog re-arms the stacking guard for a
      // subsequent request.
      QObject::connect(
          sub_dialog, &QDialog::finished, dialog,
          guardedCallback(callbacks_alive, [&active_sub_dialog, on_finished = std::move(on_finished)](int) {
            active_sub_dialog.clear();
            if (on_finished) {
              on_finished();
            }
          }));

      // The result is intentionally ignored (this is an informational/help
      // sub-dialog). ApplicationModal blocks input app-wide while the browser
      // event loop keeps running; WA_DeleteOnClose hands ownership to Qt so the
      // dialog self-destructs once dismissed. show() (not open()) preserves
      // ApplicationModal — open() would force it down to WindowModal.
      sub_dialog->setWindowModality(Qt::ApplicationModal);
      sub_dialog->setAttribute(Qt::WA_DeleteOnClose);
      sub_dialog->show();
    };

    // 5. Wire signals + the declarative "pj_enable_when"/"pj_visible_when"
    //    combo->field rules.
    installDeclarativeRules(binding_root);
    connectWidgetSignals(
        binding_root, guardedCallback(callbacks_alive, [&](const std::string& name, const std::string& event_json) {
          stats_.event_count++;
          // Pre-branch exec() ran the file/folder/save pickers only after any
          // sub-dialog was dismissed. Chain them through maybe_open_sub_dialog's
          // continuation so plugin-visible ordering is unchanged; when no sub-dialog
          // opens the continuation fires immediately.
          auto run_pickers = [&, name]() {
            show_file_picker_for(name, &handle_, binding_root, memo);
            show_folder_picker_for(name, &handle_, binding_root, memo);
            show_save_file_picker_for(name, &handle_, binding_root, memo);
          };
          if (handle_.sendEvent(name, event_json)) {
            auto ar = applyAndDiff(binding_root, handle_, memo, config_, stats_);
            if (ar.wants_accept) {
              dialog->accept();
              return;
            }
            maybe_open_sub_dialog(ar, run_pickers);
            return;
          }
          run_pickers();
        }));

    // 5b. Install button keyboard shortcuts declared in widget data
    {
      PJ::WidgetDataView shortcut_view(handle_.widget_data());
      installButtonShortcuts(dialog, shortcut_view);
    }

    // 5c. Install drop event filter for declared drop targets
    {
      PJ::WidgetDataView drop_view(initial_raw);
      auto targets = drop_view.dropTargets();
      if (!targets.empty()) {
        auto* drop_filter = new DropEventFilter(
            dialog, guardedCallback(callbacks_alive, [&](const std::string& name, const std::string& event_json) {
              stats_.event_count++;
              if (handle_.sendEvent(name, event_json)) {
                auto ar = applyAndDiff(binding_root, handle_, memo, config_, stats_);
                if (ar.wants_accept) {
                  dialog->accept();
                  return;
                }
                maybe_open_sub_dialog(ar);
              }
            }));
        for (const auto& t : targets) {
          drop_filter->addTarget(t);
        }
      }
    }

    // 6. Start tick timer
    QTimer tick_timer;
    tick_timer.setInterval(config_.tick_interval_ms);
    QObject::connect(&tick_timer, &QTimer::timeout, [&]() {
      stats_.tick_count++;
      if (handle_.tick()) {
        auto ar = applyAndDiff(binding_root, handle_, memo, config_, stats_);
        if (ar.wants_accept) {
          dialog->accept();
          return;
        }
        maybe_open_sub_dialog(ar);
      }
    });
    tick_timer.start();

    // Re-fit once the dialog is actually shown and laid out: the pre-show sizeHint
    // is stale (measured before the window exists), so a deferred singleShot(0)
    // fit gives the right size from the start. We only RE-FIT here — the parser
    // options were already injected and populated above, so we must NOT re-inject
    // (that would replace the populated widget with a fresh, empty one).
    if (content_fit_dialog) {
      QTimer::singleShot(0, dialog, [fit_to_content]() { fit_to_content(); });
    }

    // Plain authored dialogs do not pass through fit_to_content(). Clamp every
    // dialog once before it is mapped; the helper is a no-op on native builds.
    clamp_to_browser_screen();

    // 7. Open dialog and suspend this coroutine until QDialog::finished. Every
    // local above lives in the coroutine frame, so its signal callbacks, parser
    // handle, diff state, and tick timer remain valid without a nested loop.
    // Registering the dialog lets cancelActiveDialog() reject it synchronously.
    QPointer<QDialog> dialog_guard(dialog);
    self.active_dialog_ = dialog;
    if (config_.non_modal) {
      dialog->setWindowModality(Qt::NonModal);
      dialog->show();
      dialog->activateWindow();
    } else {
      // App-modal, not window-modal: window-modality only blocks the parent's
      // window, leaving floating ADS dock tool-windows (separate top-levels)
      // interactive. This restores the pre-branch exec() semantics (implicitly
      // app-modal) without a nested event loop. Use show(), NOT QDialog::open():
      // open() force-downgrades any non-WindowModal modality back to WindowModal,
      // which is the very regression being fixed. show() honors the set modality
      // and still fires QDialog::finished for the awaiter below.
      dialog->setWindowModality(Qt::ApplicationModal);
      dialog->show();
    }
    // Named local, not a co_await temporary: GCC 11.4 (linux-ci / AppImage)
    // mishandles awaiter temporaries inside co_await expressions.
    DialogFinishedAwaiter finished_awaiter(dialog);
    const int result = co_await std::move(finished_awaiter);
    tick_timer.stop();
    // The frame dies at co_return but the dialog (and every sender-held copy of
    // the callbacks above) survives until its deferred delete: sever them now,
    // before notifying the plugin, so nothing can re-enter this frame.
    *callbacks_alive = false;
    self.active_dialog_.clear();
    // Close a sub-dialog still open when the main dialog completes (e.g. a
    // cancelActiveDialog while a help sub-dialog is up). Its finished handler is
    // already severed above, so this just tears it down without re-entering the
    // dead frame; WA_DeleteOnClose frees it.
    if (!active_sub_dialog.isNull()) {
      active_sub_dialog->reject();
    }

    // A parent can disappear while a dialog is pending. QDialog then vanishes
    // without a usable widget for geometry/cleanup, but the plugin still receives
    // one rejection and the caller still receives one completion.
    if (dialog_guard.isNull()) {
      handle_.reject();
      parser_config_.clear();
      self.dialog_open_ = false;
      completion(DialogResult::kRejected);
      co_return;
    }

    // 8. Notify plugin and clean up
    DialogResult dr;
    if (result == QDialog::Accepted) {
      handle_.accept(handle_.save_config());
      // Save parser config if a parser dialog was shown
      if (parser_dialog_handle) {
        parser_config_ = parser_dialog_handle->save_config();
      } else {
        parser_config_.clear();
      }
      dr = DialogResult::kAccepted;
    } else {
      handle_.reject();
      parser_config_.clear();
      dr = DialogResult::kRejected;
    }
    // Save dialog geometry for next time (not for content-fit dialogs — they
    // always size to content, so a remembered manual size must not stick).
    if (!plugin_name.empty() && !content_fit_dialog) {
      QSettings settings;
      settings.setValue(geometry_key, dialog->saveGeometry());
    }

    dialog->deleteLater();
    self.dialog_open_ = false;
    completion(dr);
    co_return;
  }
};

void DialogEngine::openDialog(QWidget* parent, Completion completion) {
  if (!completion) {
    completion = [](DialogResult) {};
  }
  if (dialog_open_) {
    completion(DialogResult::kRejected);
    return;
  }
  dialog_open_ = true;
  AsyncRunner::run(*this, parent, std::move(completion));
}

void DialogEngine::cancelActiveDialog() {
  if (!dialog_open_ || active_dialog_.isNull()) {
    return;  // never opened, already completed, or destroyed with its parent
  }
  // reject() emits QDialog::finished, whose same-thread hook resumes the
  // suspended AsyncRunner immediately: the runner severs its widget callbacks,
  // delivers exactly one on_rejected to the plugin, and runs the completion
  // with kRejected — all before reject() returns. Taking the pointer first
  // keeps a re-entrant cancel (e.g. from inside that completion) a no-op.
  QPointer<QDialog> dialog = active_dialog_;
  active_dialog_.clear();
  dialog->reject();
}

DialogResult DialogEngine::showDialog(QWidget* parent) {
#ifdef Q_OS_WASM
  Q_UNUSED(parent)
  qWarning("DialogEngine::showDialog uses a nested event loop and is unavailable on WebAssembly; use openDialog");
  return DialogResult::kRejected;
#else
  // Heap-shared completion state: QCoreApplication::exit()/quit() can unwind
  // the nested loop below WITHOUT the completion having run. The suspended
  // dialog coroutine then outlives this frame, and its eventual completion must
  // write into live storage, not this dead stack (same hazard/mitigation as the
  // plugin message-box gate in FileLoader).
  struct SyncState {
    DialogResult result = DialogResult::kRejected;
    bool finished = false;
    QEventLoop loop;
  };
  auto state = std::make_shared<SyncState>();
  openDialog(parent, [state](DialogResult completed) {
    state->result = completed;
    state->finished = true;
    state->loop.quit();
  });
  if (!state->finished) {
    state->loop.exec();
  }
  if (!state->finished) {
    // Early unwind: complete the coroutine NOW, while this engine is still
    // alive — a caller destroying the stack engine with the frame suspended
    // would otherwise leave the pending dialog to resume against freed state.
    cancelActiveDialog();
  }
  return state->result;
#endif
}

// ---------------------------------------------------------------------------
// run_headless
// ---------------------------------------------------------------------------

std::string DialogEngine::savedConfig() const {
  return handle_.save_config();
}

std::string DialogEngine::parserConfig() const {
  return parser_config_;
}

std::string DialogEngine::runHeadless(int max_ticks) {
  for (int i = 0; i < max_ticks; ++i) {
    (void)handle_.tick();
  }
  return handle_.widget_data();
}

}  // namespace PJ
