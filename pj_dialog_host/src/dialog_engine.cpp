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
#include <iterator>
#include <memory>
#include <pj_plugins/host/widget_data_view.hpp>
#include <pj_plugins/host_qt/dialog_engine.hpp>
#include <pj_plugins/host_qt/drop_event_filter.hpp>
#include <pj_plugins/host_qt/pj_ui_loader.hpp>
#include <pj_plugins/host_qt/widget_adapters.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>
#include <utility>
#include <vector>

#include "host_info.hpp"
#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

DialogEngine::DialogEngine(PJ::DialogHandle handle, DialogEngineConfig config)
    : handle_(std::move(handle)), config_(config) {
  // Only panels embed scene views: no capability to announce.
  detail::announceHostInfo(handle_, /*capabilities=*/0);
}

namespace {

// Fire-and-forget coroutine that keeps the dialog setup stack alive until QDialog::finished; the frame is
// destroyed as soon as the completion callback returns.
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

// Awaits QDialog completion without a nested event loop. The destroyed connection covers a parent window
// disappearing while the dialog is pending; the QPointer keeps the continuation off the deleted widget.
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

// Wraps a callback handed to sender-owned connections (widget signals, drop filter) with a liveness flag. Each
// connection keeps its own copy inside the sender widget, which lives until the dialog's deleteLater, past the
// coroutine frame the [&] captures reference. Once the flag is cleared (at dialog completion) a late signal (a
// queued event, a focus-out during teardown, a signal from a child's destructor) is a no-op instead of a call into
// a destroyed frame.
template <typename Fn>
auto guardedCallback(std::shared_ptr<const bool> alive, Fn fn) {
  return [alive = std::move(alive), fn = std::move(fn)](auto&&... args) {
    if (*alive) {
      fn(std::forward<decltype(args)>(args)...);
    }
  };
}

// QUiLoader creates plugin section bands at their class default; match the app chrome when metrics were supplied.
void applySectionBandMetrics(QWidget* root, const std::optional<ChromeMetrics>& metrics) {
  if (root == nullptr || !metrics.has_value()) {
    return;
  }
  for (auto* band : root->findChildren<SectionHeaderBand*>()) {
    band->onChromeMetricsChanged(*metrics);
  }
}

// The `pjButtonsFillWidth` .ui property on a QDialogButtonBox: its buttons span the box instead of hugging one
// edge. The box lays them out in a QBoxLayout padded with style-inserted stretch spacers a .ui cannot reach, so the
// host zeroes the spacers' stretch and gives each button an Expanding policy and a stretch weight.
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
    // The widget stylesheet cascades over the app QSS: border, background and hover still come from the theme.
    const QString button_padding =
        QStringLiteral("QPushButton { padding: %1px; }").arg(theme::space(theme::Space::Snug));
    for (int i = 0; i < box_layout->count(); ++i) {
      QLayoutItem* item = box_layout->itemAt(i);
      if (QWidget* button = item->widget()) {
        button->setSizePolicy(QSizePolicy::Expanding, button->sizePolicy().verticalPolicy());
        button->setStyleSheet(button_padding);
        box_layout->setStretch(i, 1);
      } else {
        // A style-inserted stretch spacer.
        box_layout->setStretch(i, 0);
      }
    }
  }
}

// QUiLoader applies a root QDialog's authored <geometry> to size(), which is not part of its sizeHint once the
// root is embedded in the host dialog. The authored extent plus the host title bar is the dialog size. A plain
// QWidget root has no authored window extent and sizes from its layout.
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

// The keys of `new_data` that are new or changed against `old_data`.
static nlohmann::json computeDiff(const nlohmann::json& old_data, const nlohmann::json& new_data) {
  nlohmann::json diff = nlohmann::json::object();
  for (const auto& [key, val] : new_data.items()) {
    if (!old_data.contains(key) || old_data[key] != val) {
      diff[key] = val;
    }
  }
  return diff;
}

/// The one-shot requests carried by a widget-data payload: accept, and a sub-dialog (with its UI XML).
struct ApplyResult {
  bool wants_accept = false;
  std::optional<std::string> sub_dialog_ui;
};

/// The last payload delivered to a widget tree: the parsed state (for key diffing) and the raw bytes (the
/// byte-identical skip guard). Mutated together.
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
  // A byte-identical payload skips parse, diff and apply. One-shot requests (accept, sub-dialog) change the
  // bytes, so they still fire.
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

  // One-shot commands are not widget state.
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

struct DialogEngine::AsyncRunner {
  static DetachedDialogTask run(DialogEngine& self, QWidget* parent, Completion completion) {
    auto& handle_ = self.handle_;
    auto& config_ = self.config_;
    auto& stats_ = self.stats_;
    auto& parser_config_ = self.parser_config_;
    stats_ = {};

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

    // The host owns window chrome: every root, QDialog roots included, is embedded in the app's Dialog.
    auto* dialog = new PJ::Dialog(parent);
    if (config_.section_band_metrics) {
      dialog->setChromeMetrics(*config_.section_band_metrics);
    }
    dialog->setDialogTitle(config_.title_prefix + loaded->windowTitle());
    dialog->contentLayout()->addWidget(loaded);
    forwardEmbeddedDialogClose(loaded, dialog);

    // A parser slot embeds a parser-options widget whose height varies with the protocol (one msgpack checkbox vs a
    // tall Protobuf table), so no fixed .ui size fits: these dialogs, and only these, are sized to their content
    // at run time. Others keep the authored .ui size.
    const bool content_fit_dialog = loaded->findChild<QWidget*>("pj_parser_slot") != nullptr;
    if (!content_fit_dialog) {
      applyAuthoredDialogContentSize(dialog, authored_content_size);
    }

    auto clamp_to_browser_screen = [dialog]() {
      Q_UNUSED(dialog);  // captured for the browser branch only
#ifdef Q_OS_WASM
      // The browser canvas is the whole desktop. An authored dialog can be taller than it (the MCAP one is 732 px
      // before its title bar), which leaves the button box outside the clickable canvas.
      if (const QScreen* screen = dialog->screen()) {
        constexpr int kWindowFrameAllowance = 48;
        const QSize available =
            screen->availableGeometry().size() - QSize(kWindowFrameAllowance, kWindowFrameAllowance);
        const QSize maximum = available.expandedTo(QSize(320, 240));
        // Lower the authored minimum first: Qt keeps a minimum larger than the canvas, and the footer stays
        // unreachable.
        dialog->setMinimumSize(dialog->minimumSize().boundedTo(maximum));
        dialog->setMaximumSize(maximum);
        dialog->resize(dialog->size().boundedTo(maximum));
      }
#else
      Q_UNUSED(dialog);
#endif
    };

    // Sizes the dialog to its content: drops the authored fixed minimum heights (the dialog's and the parser
    // slot's) and collapses expanding vertical spacers, but keeps tables, lists and editors readable. Idempotent;
    // runs on every protocol change.
    auto fit_to_content = [loaded, dialog, clamp_to_browser_screen]() {
      QWidget* slot = loaded->findChild<QWidget*>("pj_parser_slot");
      if (slot == nullptr) {
        return;
      }
      // The floor left by the previous parser page must not stop a narrower or shorter page from being measured.
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
              // No size hint, but still expanding: a short page stays top-aligned in a taller splitter pane.
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
      // Splitter panes are widgets, so the recursion above does not reach their layouts.
      for (QWidget* descendant : loaded->findChildren<QWidget*>()) {
        collapse_vspacers(descendant->layout());
      }
      // Parser options with a table (Protobuf, ROS1/ROS2) need room; protocols without one (msgpack, cbor, json)
      // have no item view and stay compact.
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
      // The content floor, capped to the browser maximum: no resize-crush on desktop, no unreachable dialog in the
      // browser when the content is taller than the canvas.
      dialog->setMinimumSize(dialog->minimumSizeHint().boundedTo(dialog->maximumSize()));
    };

    // Owner side of guardedCallback(): cleared when the dialog completes, before the frame dies at co_return.
    auto callbacks_alive = std::make_shared<bool>(true);

    // Connected here because the root may be a plain QWidget, whose button box nothing else wires.
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

    // Saved dialog geometry is keyed by the plugin's manifest name.
    auto manifest_json = nlohmann::json::parse(handle_.manifest(), nullptr, false);
    std::string plugin_name = manifest_json.is_object() ? manifest_json.value("name", "") : "";
    QString geometry_key = QString("DialogGeometry/%1").arg(QString::fromStdString(plugin_name));
    // A content-fit dialog never restores a saved size: it would override the fit and leave dead space.
    if (!plugin_name.empty() && !content_fit_dialog) {
      QSettings settings;
      auto saved = settings.value(geometry_key).toByteArray();
      if (!saved.isEmpty()) {
        dialog->restoreGeometry(saved);
      }
    }

    stats_.has_parser_slot = loaded->findChild<QWidget*>("pj_parser_slot") != nullptr;

    QWidget* parser_slot = nullptr;
    QWidget* parser_slot_container = nullptr;  // the slot's parent, shown only while a parser dialog is injected
    QVBoxLayout* parser_slot_layout = nullptr;
    QWidget* parser_dialog_widget = nullptr;
    std::unique_ptr<PJ::DialogHandle> parser_dialog_handle;
    PayloadMemo parser_memo;
    // Leases of every pick delivered to the plugin: the picked file stays readable at its backing_path until the
    // dialog completes for the plugin, and until the import ends for the host. A re-pick does not release the
    // earlier one (one picker can fill several config entries, such as a CAN loader's DBC per interface; a browser
    // lease only references the picked File). Handed over on accept; reject releases them.
    std::vector<std::shared_ptr<void>> selected_file_leases;
    bool file_picker_active = false;
    std::uint64_t parser_dialog_generation = 0;

    // The picker handlers serve the main dialog's handle and the parser dialog's.
    auto show_file_picker_for = [&](const std::string& widget_name, PJ::DialogHandle* handle, QWidget* target_widget,
                                    PayloadMemo& target_memo) {
      if (!config_.enable_file_picker || !handle) {
        return;
      }
      PJ::WidgetDataView view(handle->widget_data());
      if (!view.isFilePicker(widget_name)) {
        return;
      }
      // Delivers the pick and, when onFileSelected returns true, re-reads the plugin's widget data.
      auto apply_selected_path = [&, widget_name, handle, target_widget](const std::string& path) {
        if (!handle->sendEvent(widget_name, PJ::WidgetEventBuilder::fileSelected(path))) {
          return;
        }
        std::string raw = handle->widget_data();
        nlohmann::json new_data = nlohmann::json::parse(raw, nullptr, false);
        if (!new_data.is_discarded()) {
          new_data.erase("__request_accept");
          new_data.erase("__request_sub_dialog");
          PJ::WidgetDataView updated_view(raw);
          applyWidgetData(target_widget, updated_view);
          target_memo.data = std::move(new_data);
          // One-shot commands in this payload were not consumed: the next poll must inspect it once.
          target_memo.raw.clear();
        }
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
             apply_selected_path = std::move(apply_selected_path),
             &selected_file_leases](std::optional<DialogSelectedFile> selected) mutable {
              if (!*callbacks_alive) {
                return;
              }
              file_picker_active = false;
              if (!selected.has_value() || (targets_parser && target_generation != parser_dialog_generation)) {
                return;
              }
              apply_selected_path(selected->backing_path);
              // Kept whatever onFileSelected returned: it means "re-read my widget data", not "path accepted".
              selected_file_leases.push_back(std::move(selected->lease));
            });
        return;
      }
#ifdef Q_OS_WASM
      Q_UNUSED(target_widget)
      Q_UNUSED(target_memo)
      qWarning("DialogEngine: no browser content picker was provided for this plugin file request");
#else
      const FilePickerStart start = filePickerStart(view, widget_name);
      QString path = PJ::FileDialog::getOpenFileName(
          dialog, QString::fromStdString(title), start.dir, QString::fromStdString(filter), start.name);
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
      // A dialog's folder picker selects a folder to read (protobuf's include folders), which a browser cannot
      // provide; only toolbox (PanelEngine) folder pickers work there, as downloads.
      Q_UNUSED(target_widget)
      Q_UNUSED(target_memo)
      qWarning("DialogEngine: plugin folder pickers are unavailable on WebAssembly");
#else
      auto title = view.folderPickerTitle(widget_name).value_or("Select Folder");
      QString path = PJ::FileDialog::getExistingDirectory(
          dialog, QString::fromStdString(title), filePickerStart(view, widget_name).dir);
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
            // One-shot commands in this payload were not consumed: the next poll must inspect it once.
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
      const FilePickerStart start = filePickerStart(view, widget_name);
      QString path = PJ::FileDialog::getSaveFileName(
          dialog, QString::fromStdString(title), start.dir, QString::fromStdString(filter),
          QString::fromStdString(suffix), start.name);
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
            // One-shot commands in this payload were not consumed: the next poll must inspect it once.
            target_memo.raw.clear();
          }
        }
      }
#endif
    };

    // Replaces the parser dialog with the one for `encoding`; the container is hidden when there is none. The
    // generation lets a late browser file pick detect that its parser dialog was replaced.
    auto inject_parser_dialog = [&](const QString& encoding) {
      ++parser_dialog_generation;
      if (parser_dialog_widget) {
        parser_slot_layout->removeWidget(parser_dialog_widget);
        delete parser_dialog_widget;
        parser_dialog_widget = nullptr;
      }
      parser_dialog_handle.reset();
      parser_memo = {};

      if (!config_.parser_dialog_provider) {
        if (parser_slot_container) {
          parser_slot_container->setVisible(false);
        }
        return;
      }

      const PJ_dialog_vtable_t* vtable = config_.parser_dialog_provider(encoding.toStdString());
      if (vtable == nullptr) {
        if (parser_slot_container) {
          parser_slot_container->setVisible(false);
        }
        return;
      }

      parser_dialog_handle = std::make_unique<PJ::DialogHandle>(vtable);

      if (!config_.initial_parser_config.empty()) {
        (void)parser_dialog_handle->load_config(config_.initial_parser_config);
      }

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

      parser_slot_layout->addWidget(parser_dialog_widget);
      if (parser_slot_container) {
        parser_slot_container->setVisible(true);
      }
      stats_.parser_dialog_injected = true;

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

      // Injected after the one-time rule install on binding_root, so it needs its own pass.
      installDeclarativeRules(parser_dialog_widget);
      connectWidgetSignals(
          parser_dialog_widget,
          guardedCallback(callbacks_alive, [&](const std::string& name, const std::string& event_json) {
            stats_.event_count++;
            if (parser_dialog_handle && parser_dialog_handle->sendEvent(name, event_json)) {
              // Same byte-identical guard as the main dialog.
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

            show_file_picker_for(name, parser_dialog_handle.get(), parser_dialog_widget, parser_memo);
            show_folder_picker_for(name, parser_dialog_handle.get(), parser_dialog_widget, parser_memo);
            show_save_file_picker_for(name, parser_dialog_handle.get(), parser_dialog_widget, parser_memo);
          }));
    };

    if (stats_.has_parser_slot && config_.parser_dialog_provider) {
      parser_slot = loaded->findChild<QWidget*>("pj_parser_slot");
      if (parser_slot) {
        parser_slot_container = parser_slot->parentWidget();
        parser_slot_layout = new QVBoxLayout(parser_slot);
        parser_slot_layout->setContentsMargins(
            theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
            theme::space(theme::Space::None));

        // The re-fit is deferred with singleShot(0) so it runs after the layout has measured the injected widget;
        // run synchronously it would size the dialog to the previous protocol's options.
        if (auto* combo = loaded->findChild<QComboBox*>("comboBoxProtocol")) {
          QObject::connect(
              combo, &QComboBox::currentTextChanged, dialog,
              guardedCallback(callbacks_alive, [&, fit_to_content](const QString& encoding) {
                inject_parser_dialog(encoding);
                QTimer::singleShot(0, dialog, [fit_to_content]() { fit_to_content(); });
              }));
          // The initial injection follows the first widget-data apply, once the combo is populated.
        }
      }
    }

    QWidget* binding_root = loaded;

    std::string initial_raw = handle_.widget_data();
    PayloadMemo memo;
    memo.data = nlohmann::json::parse(initial_raw, nullptr, false);
    if (memo.data.is_discarded()) {
      memo.data = nlohmann::json::object();
    }
    // memo.raw stays empty until the first event or tick delivery: that delivery consumes the one-shot commands of
    // the initial payload (a requested sub-dialog); seeding it here would suppress them for good.
    {
      PJ::WidgetDataView view(initial_raw);
      applyWidgetData(binding_root, view);
    }

    if (parser_slot != nullptr) {
      if (auto* combo = loaded->findChild<QComboBox*>("comboBoxProtocol")) {
        inject_parser_dialog(combo->currentText());
      }
    }
    fit_to_content();
    applySectionBandMetrics(loaded, config_.section_band_metrics);
    applyButtonBoxFillWidth(loaded);

    // The one open sub-dialog. While it is set, maybe_open_sub_dialog does nothing: a plugin that leaves
    // __request_sub_dialog set across ticks would otherwise spawn one per tick. Cleared by the sub-dialog's
    // finished handler, which is severed at frame teardown so a shutdown-time reject cannot reach a dead frame.
    QPointer<QDialog> active_sub_dialog;

    // Opens an application-modal sub-dialog from the UI XML. `on_finished` runs after it is dismissed, which keeps
    // the plugin-visible order "sub-dialog, then file picker"; when one is already open or none is requested it
    // runs at once, so the continuation is never dropped.
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

      // Plugin widget data is applied to the main dialog's tree only, so the sub-dialog's field states come from
      // the .ui's declarative "pj_enable_when" / "pj_visible_when" rules.
      installDeclarativeRules(sub_dialog);

      active_sub_dialog = sub_dialog;
      // Guarded: at frame teardown the flag is cleared, so a late finished cannot re-enter a dead frame.
      QObject::connect(
          sub_dialog, &QDialog::finished, dialog,
          guardedCallback(callbacks_alive, [&active_sub_dialog, on_finished = std::move(on_finished)](int) {
            active_sub_dialog.clear();
            if (on_finished) {
              on_finished();
            }
          }));

      // The result is ignored: a sub-dialog is informational. show(), not open(), keeps ApplicationModal (open()
      // forces WindowModal); WA_DeleteOnClose frees it once dismissed.
      sub_dialog->setWindowModality(Qt::ApplicationModal);
      sub_dialog->setAttribute(Qt::WA_DeleteOnClose);
      sub_dialog->show();
    };

    installDeclarativeRules(binding_root);
    connectWidgetSignals(
        binding_root, guardedCallback(callbacks_alive, [&](const std::string& name, const std::string& event_json) {
          stats_.event_count++;
          // The pickers run after any sub-dialog is dismissed (the continuation fires at once when none opens).
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

    {
      PJ::WidgetDataView shortcut_view(handle_.widget_data());
      installButtonShortcuts(dialog, shortcut_view);
    }

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

    // The pre-show sizeHint is stale (measured before the window exists): fit again once the dialog is shown. Only
    // a re-fit: re-injecting would replace the populated parser widget with an empty one.
    if (content_fit_dialog) {
      QTimer::singleShot(0, dialog, [fit_to_content]() { fit_to_content(); });
    }

    // Plain dialogs skip fit_to_content(), so clamp every dialog once before it is mapped (a no-op natively).
    clamp_to_browser_screen();

    // Suspends this coroutine until QDialog::finished: every local above lives in its frame, so the callbacks,
    // parser handle, diff state and tick timer stay valid without a nested loop. Registering the dialog lets
    // cancelActiveDialog() reject it.
    QPointer<QDialog> dialog_guard(dialog);
    self.active_dialog_ = dialog;
    if (config_.non_modal) {
      dialog->setWindowModality(Qt::NonModal);
      dialog->show();
      dialog->activateWindow();
    } else {
      // Application-modal: window-modal only blocks the parent's window, leaving floating dock tool-windows
      // (separate top-levels) interactive. show(), not open(): open() forces any other modality down to
      // WindowModal. show() still fires QDialog::finished for the awaiter.
      dialog->setWindowModality(Qt::ApplicationModal);
      dialog->show();
    }
    // A named local: GCC 11.4 (linux-ci, AppImage) mishandles awaiter temporaries inside co_await.
    DialogFinishedAwaiter finished_awaiter(dialog);
    const int result = co_await std::move(finished_awaiter);
    tick_timer.stop();
    // The dialog and every sender-held copy of the callbacks outlive this frame (until deleteLater): sever them
    // before notifying the plugin, so nothing re-enters it.
    *callbacks_alive = false;
    self.active_dialog_.clear();
    // A sub-dialog still open (a cancelActiveDialog while it is up): its finished handler is already severed.
    if (!active_sub_dialog.isNull()) {
      active_sub_dialog->reject();
    }

    // The parent disappeared while the dialog was pending: no widget is left for geometry or cleanup, but the
    // plugin still gets one rejection and the caller one completion.
    if (dialog_guard.isNull()) {
      handle_.reject();
      parser_config_.clear();
      self.dialog_open_ = false;
      completion(DialogResult::kRejected);
      co_return;
    }

    DialogResult dr;
    if (result == QDialog::Accepted) {
      handle_.accept(handle_.save_config());
      if (parser_dialog_handle) {
        parser_config_ = parser_dialog_handle->save_config();
      } else {
        parser_config_.clear();
      }
      std::move(selected_file_leases.begin(), selected_file_leases.end(), std::back_inserter(self.selected_files_));
      dr = DialogResult::kAccepted;
    } else {
      handle_.reject();
      parser_config_.clear();
      dr = DialogResult::kRejected;
    }
    // Not for content-fit dialogs: they size to content, so a remembered manual size must not stick.
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
  selected_files_.clear();
  AsyncRunner::run(*this, parent, std::move(completion));
}

void DialogEngine::cancelActiveDialog() {
  if (!dialog_open_ || active_dialog_.isNull()) {
    return;  // never opened, already completed, or destroyed with its parent
  }
  // reject() emits QDialog::finished, which resumes the suspended coroutine in the same call: it severs the widget
  // callbacks, delivers one on_rejected and runs the completion before reject() returns. Clearing the pointer
  // first makes a re-entrant cancel (from inside that completion) a no-op.
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
  // Heap-shared: QCoreApplication::exit() can unwind the nested loop before the completion has run. The suspended
  // coroutine then outlives this frame, and its eventual completion must write into live storage, not this dead
  // stack.
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
    // Early unwind: complete the coroutine now, while the engine is alive; a caller destroying a stack engine would
    // otherwise leave the dialog to resume against freed state.
    cancelActiveDialog();
  }
  return state->result;
#endif
}

std::string DialogEngine::savedConfig() const {
  return handle_.save_config();
}

std::string DialogEngine::parserConfig() const {
  return parser_config_;
}

std::vector<std::shared_ptr<void>> DialogEngine::takeSelectedFiles() {
  return std::exchange(selected_files_, {});
}

std::string DialogEngine::runHeadless(int max_ticks) {
  for (int i = 0; i < max_ticks; ++i) {
    (void)handle_.tick();
  }
  return handle_.widget_data();
}

}  // namespace PJ
