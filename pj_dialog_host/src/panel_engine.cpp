// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include <pj_widgets/Dialog.h>
#include <pj_widgets/FileDialog.h>
#include <pj_widgets/FrameworkTokens.h>
#include <pj_widgets/SvgUtil.h>

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGuiApplication>
#include <QLineEdit>
#include <QPointer>
#include <QScreen>
#include <QString>
#include <QTimer>
#include <QUiLoader>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_plugins/host/widget_data_view.hpp>
#include <pj_plugins/host/widget_event_builder.hpp>
#include <pj_plugins/host_qt/drop_event_filter.hpp>
#include <pj_plugins/host_qt/panel_engine.hpp>
#include <pj_plugins/host_qt/pj_ui_loader.hpp>
#include <pj_plugins/host_qt/widget_adapters.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>
#include <utility>

#include "browser_save_target.hpp"
#include "host_info.hpp"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
// A hidden panel root delivers one tick per this many timer fires.
constexpr int kHiddenTickDivisor = 10;
}  // namespace

struct PanelEngine::Impl {
  Impl(DialogHandle h, PanelEngineConfig c) : handle(std::move(h)), config(c) {}

  DialogHandle handle;
  PanelEngineConfig config;
  QPointer<QWidget> root;
  QPointer<QDialog> sub_panel;  // requestSubPanel; null when closed
  QTimer* tick_timer = nullptr;
  nlohmann::json prev_data = nlohmann::json::object();
  std::string prev_raw;
  // Parsed prev_raw, assigned with it. Per-event consumers (the file-picker check) read it instead of building and
  // parsing widget_data() per event, which is the cost at drag rates.
  WidgetDataView prev_view{std::string_view{}};
  std::function<bool(std::string)> close_cb;
  std::function<void()> request_owner_close;  // PanelEngine::close, set in openPanel
  Stats stats;
  bool closed = false;
#ifdef PJ_TARGET_WASM
  // Context of a non-blocking sub-dialog's completion: destroyed with this Impl, so a sub-dialog that outlives the
  // engine never calls back into it.
  std::unique_ptr<QObject> sub_dialog_context = std::make_unique<QObject>();
#endif
  // The theme the panel's icons were last applied for (see PanelEngine::eventFilter).
  QString applied_theme;

  // The one place that passes the session and catalog: every panel apply routes here, or a session-backed chart
  // silently degrades to the preview widget. A `scene_view` request the host refused reaches the plugin once the
  // whole update is applied; the widget-data change it makes in answer is picked up by the next tick.
  void applyPanelData(QWidget* target, const WidgetDataView& view) {
    for (const EmbeddedViewFailure& failure :
         applyWidgetData(target, view, config.session, config.catalog, &config.embedded_views)) {
      (void)handle.sendEvent(failure.frame, WidgetEventBuilder::sceneViewError(failure.error));
    }
  }
  // A modal sub-dialog embeds no scene view: the SDK grants that to panels only.
  void applyModalData(QWidget* target, const WidgetDataView& view) {
    (void)applyWidgetData(target, view, config.session, config.catalog);
  }

  void restartTickTimerAfterEvent() {
    // isActive(): applyAndDiff stops the timer around a modal sub-dialog to block re-entrant ticks, and an event
    // forwarded then must not re-arm it.
    if (config.restart_tick_timer_on_event && tick_timer != nullptr && tick_timer->isActive() && !closed) {
      tick_timer->start();
    }
  }

  // Sends one widget event to the plugin and applies the resulting widget data; the interactive sub-panel uses it.
  void forwardEvent(const std::string& name, const std::string& event_json) {
    if (closed) {
      return;
    }
    ++stats.event_count;
    if (handle.sendEvent(name, event_json)) {
      if (auto reason = applyAndDiff(); reason.has_value()) {
        // The owner decides: a host keeping a pinned tab alive past "import_complete" declines, and the session
        // keeps running instead of becoming an inert shell.
        const bool proceed = !close_cb || close_cb(*reason);
        if (proceed && request_owner_close) {
          request_owner_close();
        }
      }
      restartTickTimerAfterEvent();
    }
  }

  // One tick: advance the plugin, poll widget_data, apply the diff, honor a requestClose. Run by the tick timer and
  // the Show-event catch-up. The modal guard matters for the catch-up only (the timer is paused during a modal
  // sub-dialog): a re-entrant tick could observe half-applied state. An open interactive sub-panel does not stop
  // ticks; they update it.
  void runTick() {
    if (closed || modal_sub_dialog_open) {
      return;
    }
    ++stats.tick_count;
    (void)handle.tick();
    if (auto reason = applyAndDiff(); reason.has_value()) {
      const bool proceed = !close_cb || close_cb(*reason);
      if (proceed && request_owner_close) {
        request_owner_close();
      }
    }
  }
  // Timer fires skipped since the last delivered tick while the panel root was hidden.
  int hidden_tick_skips = 0;
  // A Show-event catch-up tick is queued (a burst of Show events yields one).
  bool show_catchup_queued = false;
  // A modal sub-dialog's nested exec() is running (desktop only).
  bool modal_sub_dialog_open = false;

  // Opens the interactive sub-panel from its .ui XML. Unlike the requestSubDialog modal it is non-blocking and on
  // the normal event path: its widgets forward events to the plugin and receive every tick's widget-data update.
  // `full_view` populates it once at open.
  void openSubPanel(const std::string& ui_xml, const WidgetDataView& full_view) {
    if (sub_panel != nullptr || root == nullptr) {
      return;
    }
    QByteArray sub_data(ui_xml.data(), static_cast<int>(ui_xml.size()));
    QBuffer sub_buffer(&sub_data);
    sub_buffer.open(QIODevice::ReadOnly);
    PjUiLoader loader;
    QWidget* loaded = loader.load(&sub_buffer, root);
    if (loaded == nullptr) {
      return;
    }
    adaptStyledWidgets(loaded);
    // The app's frameless chrome; the .ui's windowTitle shows on its title bar. Bindings target `loaded`, not the
    // whole dialog, so the chrome's own buttons are never wired to the plugin.
    auto* dlg = new PJ::Dialog(root);
    dlg->setDialogTitle(loaded->windowTitle());
    dlg->contentLayout()->addWidget(loaded);
    // Embedding drops the root's authored window geometry unless the chrome adopts it; clamped to the screen.
    const QSize want = loaded->size().expandedTo(loaded->sizeHint());
    const QSize avail = (dlg->screen() ? dlg->screen() : QGuiApplication::primaryScreen())->availableSize();
    dlg->resize(want.boundedTo(avail));
    forwardEmbeddedDialogClose(loaded, dlg);
    // Dismissed by any path, it is deleted with the scene views it embeds.
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::ApplicationModal);
    applyPanelData(loaded, full_view);
    installDeclarativeRules(loaded);
    connectWidgetSignals(loaded, [this](const std::string& n, const std::string& j) { forwardEvent(n, j); });
    if (auto* button_box = loaded->findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"))) {
      QObject::connect(button_box, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
      QObject::connect(button_box, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    }
    // The plugin is notified once however it is dismissed: sub_panel is nulled first, so closeSubPanelNow() and
    // close() become no-ops and this does not re-enter.
    QObject::connect(dlg, &QDialog::finished, dlg, [this](int) {
      if (sub_panel != nullptr) {
        sub_panel = nullptr;
        forwardEvent("subPanelClosed", R"({"clicked":true})");
      }
    });
    sub_panel = dlg;
    dlg->show();
  }

  void closeSubPanelNow() {
    if (sub_panel != nullptr) {
      QDialog* dlg = sub_panel;
      sub_panel = nullptr;  // disarms the finished handler
      dlg->close();
      dlg->deleteLater();
    }
  }

  // DialogEngine's diff-and-apply cycle without the accept hook. Returns the close reason if the plugin requested
  // close.
  std::optional<std::string> applyAndDiff() {
    std::string raw = handle.widget_data();
    if (raw.empty()) {
      return std::nullopt;
    }
    // An idle plugin re-emits byte-identical widget data every tick: skip parse, diff and apply. One-shot requests
    // (close, sub-dialog) change the bytes, so they still fire.
    if (raw == prev_raw) {
      ++stats.skipped_identical_count;
      return std::nullopt;
    }
    prev_raw = raw;
    nlohmann::json new_data = nlohmann::json::parse(raw, nullptr, false);
    if (new_data.is_discarded()) {
      return std::nullopt;
    }
    prev_view = WidgetDataView(raw);
    // An alias: a nested applyAndDiff (during the modal sub-dialog below) reassigns prev_view, so `view` is not
    // read after that exec returns.
    const WidgetDataView& view = prev_view;
    auto close_reason = view.requestClose();
    auto sub_dialog_ui = view.subDialogUi();
    auto sub_panel_ui = view.subPanelUi();
    const bool sub_panel_close = view.subPanelClose();

    // One-shot commands are consumed here, so the identical-payload skip must not compare the next payload against
    // them: a plugin that answers "" while idle (the documented "no update") leaves prev_raw on this request, and
    // an identical request later (Settings, Cancel, Settings) would be dropped as a repeat.
    const bool consumed_one_shot = close_reason.has_value() || sub_dialog_ui.has_value() || sub_panel_ui.has_value() ||
                                   sub_panel_close || new_data.contains("__request_accept");

    new_data.erase("__request_close");
    new_data.erase("__request_sub_dialog");
    new_data.erase("__request_sub_panel");
    new_data.erase("__request_sub_panel_close");
    new_data.erase("__request_accept");

    if (config.enable_diff) {
      nlohmann::json diff = nlohmann::json::object();
      for (const auto& [key, val] : new_data.items()) {
        if (!prev_data.contains(key) || prev_data[key] != val) {
          diff[key] = val;
        }
      }
      if (!diff.empty()) {
        WidgetDataView diff_view(diff.dump());
        if (root != nullptr) {
          applyPanelData(root, diff_view);
          ++stats.diff_apply_count;
        }
        // The sub-panel tracks the plugin's state too; names it does not own are skipped.
        if (sub_panel != nullptr) {
          applyPanelData(sub_panel, diff_view);
        }
      }
    } else {
      if (root != nullptr) {
        applyPanelData(root, view);
        ++stats.diff_apply_count;
      }
      if (sub_panel != nullptr) {
        applyPanelData(sub_panel, view);
      }
    }
    prev_data = std::move(new_data);

    // Close before open, so a same-tick close and reopen works; an open request is ignored while one is up.
    if (sub_panel_close) {
      closeSubPanelNow();
    }
    if (sub_panel_ui.has_value()) {
      openSubPanel(*sub_panel_ui, view);
    }

    // The modal sub-dialog has no event plumbing: on OK its values are sent to the plugin (see `finish`).
    if (sub_dialog_ui.has_value() && root != nullptr) {
      QByteArray sub_data(sub_dialog_ui->data(), static_cast<int>(sub_dialog_ui->size()));
      QBuffer sub_buffer(&sub_data);
      sub_buffer.open(QIODevice::ReadOnly);
      PjUiLoader sub_loader;
      QWidget* sub_loaded = sub_loader.load(&sub_buffer, root);
      if (sub_loaded != nullptr) {
        adaptStyledWidgets(sub_loaded);
        // The plugin content, QDialog-rooted or not, is the content of a PJ::Dialog (themed title bar, close
        // button, drag-to-move), as in DialogEngine.
        auto* sub_dialog = new PJ::Dialog(root);
        sub_dialog->setDialogTitle(sub_loaded->windowTitle());
        sub_dialog->contentLayout()->addWidget(sub_loaded);
        // PJ::Dialog opens at its minimal chrome size and ignores the plugin's dimensions: use the root's .ui
        // geometry or sizeHint, clamped to the screen.
        {
          const QSize want = sub_loaded->size().expandedTo(sub_loaded->sizeHint());
          const QSize avail =
              (sub_dialog->screen() != nullptr ? sub_dialog->screen() : QGuiApplication::primaryScreen())
                  ->availableSize();
          sub_dialog->resize(want.boundedTo(avail));
        }
        forwardEmbeddedDialogClose(sub_loaded, sub_dialog);
        // The "buttonBox" is wired here: embedded in a PJ::Dialog, its buttons reach no QDialog::accept/reject.
        if (auto* button_box = sub_loaded->findChild<QDialogButtonBox*>(u"buttonBox"_s)) {
          QObject::connect(button_box, &QDialogButtonBox::accepted, sub_dialog, &QDialog::accept);
          QObject::connect(button_box, &QDialogButtonBox::rejected, sub_dialog, &QDialog::reject);
        }
        // Pre-fills the sub-dialog from the current widget data; names it does not have are skipped.
        applyModalData(sub_dialog, view);
        // The nested modal loop below blocks the plugin from driving field states: the .ui's "pj_enable_when" /
        // "pj_visible_when" rules do it.
        installDeclarativeRules(sub_dialog);
        // A visibility rule collapses form rows, so the natural size depends on the current combo indices: size it
        // now that the rules ran, and again (queued, after the rule handlers) whenever a combo changes, or hidden
        // rows leave a blank band and revealed ones squeeze the rest.
        sub_dialog->adjustSize();
        // The chrome takes its geometry on show, after the call above: a queued pass sizes the shown window.
        QTimer::singleShot(0, sub_dialog, [sub_dialog]() { sub_dialog->adjustSize(); });
        for (auto* combo : sub_dialog->findChildren<QComboBox*>()) {
          QObject::connect(
              combo, &QComboBox::currentIndexChanged, sub_dialog, [sub_dialog](int) { sub_dialog->adjustSize(); },
              Qt::QueuedConnection);
        }
        // The timer is paused while the sub-dialog is up: a re-entrant tick could observe a __request_close and
        // tear the engine down, deleting sub_dialog under the suspended outer call. It resumes only if the engine
        // was not closed meanwhile.
        const bool timer_was_active = (tick_timer != nullptr) && tick_timer->isActive();
        if (timer_was_active) {
          tick_timer->stop();
        }
        const auto finish = [this, sub_dialog, timer_was_active](int dlg_result) {
          if (timer_was_active && !closed && tick_timer != nullptr) {
            tick_timer->start();
          }
          // On OK the sub-dialog's inputs reach the plugin as events (onTextChanged, onClicked, ... for each
          // widget that has an objectName in the .ui), followed by a synthetic `subDialogAccepted` click meaning
          // "commit". Without it the typed values would be dropped.
          if (dlg_result == QDialog::Accepted) {
            for (auto* line_edit : sub_dialog->findChildren<QLineEdit*>()) {
              const QString name = line_edit->objectName();
              if (name.isEmpty() || name.startsWith("qt_"_L1)) {
                continue;
              }
              nlohmann::json ev = {{"text", line_edit->text().toStdString()}};
              (void)handle.sendEvent(name.toStdString(), ev.dump());
            }
            for (auto* check_box : sub_dialog->findChildren<QCheckBox*>()) {
              const QString name = check_box->objectName();
              if (name.isEmpty() || name.startsWith("qt_"_L1)) {
                continue;
              }
              nlohmann::json ev = {{"checked", check_box->isChecked()}};
              (void)handle.sendEvent(name.toStdString(), ev.dump());
            }
            for (auto* combo_box : sub_dialog->findChildren<QComboBox*>()) {
              const QString name = combo_box->objectName();
              if (name.isEmpty() || name.startsWith("qt_"_L1)) {
                continue;
              }
              nlohmann::json ev = {
                  {"current_text", combo_box->currentText().toStdString()},
                  {"current_index", combo_box->currentIndex()}};
              (void)handle.sendEvent(name.toStdString(), ev.dump());
            }
            (void)handle.sendEvent("subDialogAccepted", R"({"clicked":true})");
          }
        };
#ifdef PJ_TARGET_WASM
        // The browser build (no Asyncify) cannot nest exec()'s loop: open non-blocking and harvest on finish,
        // unless the engine closed meanwhile.
        QObject::connect(
            sub_dialog, &QDialog::finished, sub_dialog_context.get(), [this, sub_dialog, finish](int dlg_result) {
              if (!closed) {
                finish(dlg_result);
              }
              sub_dialog->deleteLater();
            });
        sub_dialog->open();
#else
        modal_sub_dialog_open = true;
        const int dlg_result = sub_dialog->exec();
        modal_sub_dialog_open = false;
        finish(dlg_result);
        delete sub_dialog;
#endif
      }
    }

    if (consumed_one_shot) {
      prev_raw.clear();
    }
    return close_reason;
  }
};

PanelEngine::PanelEngine(DialogHandle handle, PanelEngineConfig config, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(std::move(handle), config)) {
  // A plugin gates its scene UI on this bit.
  detail::announceHostInfo(
      impl_->handle, impl_->config.embedded_views ? static_cast<uint64_t>(PJ_DIALOG_HOST_EMBEDS_SCENE_VIEWS) : 0);
}

PanelEngine::~PanelEngine() {
  close();
}

QWidget* PanelEngine::openPanel() {
  impl_->stats = {};
  impl_->request_owner_close = [this]() { this->close(); };

  std::string ui = impl_->handle.ui_content();
  if (ui.empty()) {
    return nullptr;
  }
  QByteArray data(ui.data(), static_cast<int>(ui.size()));
  QBuffer buffer(&data);
  buffer.open(QIODevice::ReadOnly);

  PjUiLoader loader;
  QWidget* loaded = loader.load(&buffer);
  if (loaded == nullptr) {
    return nullptr;
  }
  adaptStyledWidgets(loaded);
  impl_->root = loaded;

  std::string initial_raw = impl_->handle.widget_data();
  if (!initial_raw.empty()) {
    nlohmann::json initial_data = nlohmann::json::parse(initial_raw, nullptr, false);
    if (!initial_data.is_discarded()) {
      // One-shot commands are not widget state.
      initial_data.erase("__request_close");
      initial_data.erase("__request_sub_dialog");
      initial_data.erase("__request_accept");
      impl_->prev_view = WidgetDataView(initial_raw);
      impl_->applyPanelData(loaded, impl_->prev_view);
      impl_->prev_data = std::move(initial_data);
      impl_->prev_raw = initial_raw;
    }
  }

  // See eventFilter: a live theme switch needs a re-apply; the baseline is the theme just used.
  impl_->applied_theme = currentTheme();
  loaded->installEventFilter(this);

  installDeclarativeRules(loaded);
  connectWidgetSignals(loaded, [this](const std::string& name, const std::string& event_json) {
    if (impl_->closed) {
      return;
    }
    ++impl_->stats.event_count;
    auto forward = [this](const std::string& widget, const std::string& ev) {
      if (impl_->handle.sendEvent(widget, ev)) {
        if (auto reason = impl_->applyAndDiff(); reason.has_value()) {
          const bool proceed = !impl_->close_cb || impl_->close_cb(*reason);
          if (proceed) {
            this->close();
          }
        }
        impl_->restartTickTimerAfterEvent();
      }
    };
    forward(name, event_json);
    if (impl_->closed) {
      return;
    }
    // A click on a widget the plugin marked as a file, save-file or folder picker opens the chooser and sends the
    // path back as a fileSelected / folderSelected event (DialogEngine does the same for dialogs). Reads the
    // cached view: a handled event just refreshed it, and re-polling widget_data() doubled the cost of every
    // high-frequency event (a slider drag) for metadata that changes at most once per tick.
    const PJ::WidgetDataView& picker_view = impl_->prev_view;
    if (picker_view.isFilePicker(name)) {
      const FilePickerStart start = filePickerStart(picker_view, name);
      const QString path = PJ::FileDialog::getOpenFileName(
          impl_->root, QString::fromStdString(picker_view.filePickerTitle(name).value_or("Select File")), start.dir,
          QString::fromStdString(picker_view.filePickerFilter(name).value_or("")), start.name);
      if (!path.isEmpty()) {
        forward(name, PJ::WidgetEventBuilder::fileSelected(path.toStdString()));
      }
    } else if (picker_view.isSaveFilePicker(name)) {
#ifdef Q_OS_WASM
      // A browser has no path to pick: the plugin writes into a scratch folder whose files become downloads
      // (BrowserSaveTarget).
      const BrowserSaveTarget target(browserSaveName(
          filePickerStart(picker_view, name), picker_view.saveFilePickerDefaultSuffix(name).value_or("")));
      if (target.isValid()) {
        forward(name, PJ::WidgetEventBuilder::fileSelected(target.path().toStdString()));
        target.offerDownloads();
      }
#else
      const FilePickerStart start = filePickerStart(picker_view, name);
      const QString path = PJ::FileDialog::getSaveFileName(
          impl_->root, QString::fromStdString(picker_view.filePickerTitle(name).value_or("Save File")), start.dir,
          QString::fromStdString(picker_view.filePickerFilter(name).value_or("")),
          QString::fromStdString(picker_view.saveFilePickerDefaultSuffix(name).value_or("")), start.name);
      if (!path.isEmpty()) {
        forward(name, PJ::WidgetEventBuilder::fileSelected(path.toStdString()));
      }
#endif
    } else if (picker_view.isFolderPicker(name)) {
#ifdef Q_OS_WASM
      const BrowserSaveTarget target;
      if (target.isValid()) {
        forward(name, PJ::WidgetEventBuilder::folderSelected(target.path().toStdString()));
        target.offerDownloads();
      }
#else
      const QString path = PJ::FileDialog::getExistingDirectory(
          impl_->root, QString::fromStdString(picker_view.folderPickerTitle(name).value_or("Select Folder")),
          filePickerStart(picker_view, name).dir);
      if (!path.isEmpty()) {
        forward(name, PJ::WidgetEventBuilder::folderSelected(path.toStdString()));
      }
#endif
    }
  });

  // connectWidgetSignals skips buttons owned by a button box, and a panel has no QDialog::accept/reject to fall
  // back on, so Close and OK would be inert. They take the path of a plugin-requested __request_close.
  if (auto* button_box = loaded->findChild<QDialogButtonBox*>(u"buttonBox"_s)) {
    auto on_close = [this]() {
      if (impl_->closed) {
        return;
      }
      const bool proceed = !impl_->close_cb || impl_->close_cb("closed by user");
      if (proceed) {
        this->close();
      }
    };
    QObject::connect(button_box, &QDialogButtonBox::rejected, this, on_close);
    QObject::connect(button_box, &QDialogButtonBox::accepted, this, on_close);
  }

  {
    WidgetDataView drop_view(initial_raw);
    auto targets = drop_view.dropTargets();
    if (!targets.empty()) {
      auto* drop_filter = new DropEventFilter(loaded, [this](const std::string& name, const std::string& event_json) {
        if (impl_->closed) {
          return;
        }
        ++impl_->stats.event_count;
        if (impl_->handle.sendEvent(name, event_json)) {
          if (auto reason = impl_->applyAndDiff(); reason.has_value()) {
            const bool proceed = !impl_->close_cb || impl_->close_cb(*reason);
            if (proceed) {
              this->close();
            }
          }
          impl_->restartTickTimerAfterEvent();
        }
      });
      for (const auto& t : targets) {
        drop_filter->addTarget(t);
      }
      if (impl_->config.catalog_key_resolver) {
        drop_filter->setKeyResolver(impl_->config.catalog_key_resolver);
      }
      if (impl_->config.catalog_key_data_source) {
        drop_filter->setKeyDataSourceResolver(impl_->config.catalog_key_data_source);
      }
    }
  }

  // Read from the initial document: polling again would consume the plugin's answer to an error the initial
  // apply reported, which belongs to the next tick.
  {
    WidgetDataView shortcut_view(initial_raw);
    installButtonShortcuts(loaded, shortcut_view);
  }

  // Unlike DialogEngine, every tick polls widget_data() whether or not on_tick reports a change: a long-lived
  // panel's state is often updated by worker threads or async fetches that on_tick cannot observe.
  impl_->tick_timer = new QTimer(this);
  impl_->tick_timer->setInterval(impl_->config.tick_interval_ms);
  QObject::connect(impl_->tick_timer, &QTimer::timeout, this, [this]() {
    // A hidden root (a toolbox pinned into a non-current tab) ticks at 1/kHiddenTickDivisor: the plugin's periodic
    // logic and async fetches keep progressing while the invisible UI skips most poll and diff work. A Show event
    // delivers a catch-up tick (see eventFilter).
    if (impl_->root != nullptr && !impl_->root->isVisible()) {
      if (++impl_->hidden_tick_skips < kHiddenTickDivisor) {
        return;
      }
    }
    impl_->hidden_tick_skips = 0;
    impl_->runTick();
  });
  impl_->tick_timer->start();

  return loaded;
}

void PanelEngine::close() {
  if (impl_->closed) {
    return;
  }
  impl_->closed = true;
  if (impl_->tick_timer != nullptr) {
    impl_->tick_timer->stop();
  }
  impl_->closeSubPanelNow();
  impl_->handle.reject();
}

void PanelEngine::onCloseRequested(std::function<bool(std::string)> cb) {
  impl_->close_cb = std::move(cb);
}

PanelEngine::Stats PanelEngine::stats() const {
  return impl_->stats;
}

bool PanelEngine::eventFilter(QObject* watched, QEvent* event) {
  // Host-themed icons (setButtonIconNamed) are applied once and then diffed away, so a theme change re-applies
  // the last full widget data. Gated on the theme actually changing: other StyleChange events (polish, the
  // initial show) would trigger a redundant re-apply. The re-apply is the full data, not a diff, so an in-flight
  // edit the plugin has not echoed into widget_data is reset to the plugin's last value; plugins echo user input
  // back (controlled components), so normally it is a no-op, and theme toggles are rare.
  if (event->type() == QEvent::StyleChange || event->type() == QEvent::ApplicationPaletteChange) {
    const QString theme = currentTheme();
    if (theme != impl_->applied_theme && impl_->root != nullptr && !impl_->prev_raw.empty()) {
      impl_->applied_theme = theme;
      WidgetDataView view(impl_->prev_raw);
      impl_->applyPanelData(impl_->root, view);
      // The sub-panel is themed by the same path at open and on every tick; without this mirror its icons and
      // chart colors stay stale after a theme switch.
      if (impl_->sub_panel != nullptr) {
        impl_->applyPanelData(impl_->sub_panel, view);
      }
    }
  }
  // A revealed panel may have skipped up to kHiddenTickDivisor-1 timer fires: catch up so no stale state shows.
  // Queued, not synchronous: a Show fired mid-presentation (splitter replaceWidget, stack setCurrentWidget) must
  // not reach the host's presentation bookkeeping through a plugin requestClose before it is committed.
  if (event->type() == QEvent::Show && watched == impl_->root && !impl_->show_catchup_queued) {
    impl_->show_catchup_queued = true;
    impl_->hidden_tick_skips = 0;
    QMetaObject::invokeMethod(
        this,
        [this]() {
          impl_->show_catchup_queued = false;
          impl_->runTick();
        },
        Qt::QueuedConnection);
  }
  return QObject::eventFilter(watched, event);
}

}  // namespace PJ
