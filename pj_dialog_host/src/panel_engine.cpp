// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include <pj_widgets/Dialog.h>
#include <pj_widgets/FileDialog.h>
#include <pj_widgets/FrameworkTokens.h>
#include <pj_widgets/SvgUtil.h>  // currentTheme()

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
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
// A hidden panel root delivers one tick per this many timer fires (a pinned
// toolbox tab that is not current still advances its plugin, just slower).
constexpr int kHiddenTickDivisor = 10;
}  // namespace

struct PanelEngine::Impl {
  // DialogHandle has no default ctor; construct Impl with the handle.
  Impl(DialogHandle h, PanelEngineConfig c) : handle(std::move(h)), config(c) {}

  DialogHandle handle;
  PanelEngineConfig config;
  QPointer<QWidget> root;
  QPointer<QDialog> sub_panel;  // interactive sub-panel (requestSubPanel); null when closed
  QTimer* tick_timer = nullptr;
  nlohmann::json prev_data = nlohmann::json::object();
  std::string prev_raw;
  // Parsed view of prev_raw, assigned together with it: per-event consumers
  // (the file-picker check) read THIS instead of paying a fresh widget_data()
  // build + parse per forwarded event — at drag rates that build is the cost.
  WidgetDataView prev_view{std::string_view{}};
  std::function<bool(std::string)> close_cb;
  std::function<void()> request_owner_close;  // bound to PanelEngine::close in openPanel
  Stats stats;
  bool closed = false;
  // Theme the panel's icons were last applied for; a change triggers a full
  // re-apply (see PanelEngine::eventFilter).
  QString applied_theme;

  // Single wiring point for the session/catalog pair: every panel-family apply
  // must route here so a session-backed chart never silently degrades to the
  // preview widget because one call site forgot the arguments.
  void applyPanelData(QWidget* target, const WidgetDataView& view) {
    applyWidgetData(target, view, config.session, config.catalog);
  }

  void restartTickTimerAfterEvent() {
    // isActive() keeps this a pure deadline restart: applyAndDiff deliberately
    // stops the timer around a modal sub-dialog exec() to block re-entrant
    // ticks, and an event forwarded during that window must not re-arm it.
    if (config.restart_tick_timer_on_event && tick_timer != nullptr && tick_timer->isActive() && !closed) {
      tick_timer->start();
    }
  }

  // Route one widget event to the plugin, then apply any resulting widget-data
  // update. Shared by the main panel, its drop filter, and the interactive
  // sub-panel so all three reach the plugin through the same path.
  void forwardEvent(const std::string& name, const std::string& event_json) {
    if (closed) {
      return;
    }
    ++stats.event_count;
    if (handle.sendEvent(name, event_json)) {
      if (auto reason = applyAndDiff(); reason.has_value()) {
        // The owner decides: a host that keeps this panel alive past its own
        // batch completion (a pinned tab ignoring "import_complete") declines,
        // and the session must keep running rather than become an inert shell.
        const bool proceed = !close_cb || close_cb(*reason);
        if (proceed && request_owner_close) {
          request_owner_close();
        }
      }
      restartTickTimerAfterEvent();
    }
  }

  // One full tick: advance the plugin, poll widget_data, apply the diff, and
  // honor a requestClose. Shared by the tick timer and the Show-event
  // catch-up (see PanelEngine::eventFilter). The sub_panel guard only matters
  // for the catch-up path — during a modal sub-dialog exec() the timer is
  // already paused, and a re-entrant tick could observe half-applied state.
  void runTick() {
    if (closed || sub_panel != nullptr) {
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
  // Timer fires skipped since the last delivered tick while the panel root
  // was hidden (a pinned toolbox tab that is not the current tab).
  int hidden_tick_skips = 0;
  // A Show-event catch-up tick is already queued (coalesces bursts of Show
  // events into one deferred tick).
  bool show_catchup_queued = false;

  // Open the interactive sub-panel from its .ui XML. Unlike the requestSubDialog
  // modal, this is a live, non-blocking child wired into the normal event path:
  // its widgets forward events to the plugin and receive widget-data updates on
  // every tick. `full_view` populates it once at open time.
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
    // Wrap in the app's canonical frameless chrome — the .ui's windowTitle shows
    // on the custom title bar. Bindings/signals target `loaded` (the plugin
    // content), not the whole dialog, so the chrome's own buttons never get wired.
    auto* dlg = new PJ::Dialog(root);
    dlg->setDialogTitle(loaded->windowTitle());
    dlg->contentLayout()->addWidget(loaded);
    // Embedding drops the root's authored window geometry unless the chrome
    // adopts it explicitly; keep the requested size usable on the active screen.
    const QSize want = loaded->size().expandedTo(loaded->sizeHint());
    const QSize avail = (dlg->screen() ? dlg->screen() : QGuiApplication::primaryScreen())->availableSize();
    dlg->resize(want.boundedTo(avail));
    forwardEmbeddedDialogClose(loaded, dlg);
    dlg->setWindowModality(Qt::ApplicationModal);
    applyPanelData(loaded, full_view);
    installDeclarativeRules(loaded);
    connectWidgetSignals(loaded, [this](const std::string& n, const std::string& j) { forwardEvent(n, j); });
    if (auto* button_box = loaded->findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"))) {
      QObject::connect(button_box, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
      QObject::connect(button_box, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    }
    // When dismissed (Close button, programmatic close, or otherwise), notify the
    // plugin exactly once. Null the pointer FIRST so closeSubPanel()/close() are
    // no-ops and we never re-enter this teardown.
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
      sub_panel = nullptr;  // disarm the finished handler's re-entry guard
      dlg->close();
      dlg->deleteLater();
    }
  }

  // Same diff-and-apply cycle as DialogEngine, minus the QDialog::accept hook.
  // Returns the close-reason if the plugin requested close on this tick.
  std::optional<std::string> applyAndDiff() {
    std::string raw = handle.widget_data();
    if (raw.empty()) {
      return std::nullopt;
    }
    // Panels tick at 20Hz; an idle plugin re-emits byte-identical widget data
    // each tick. Skip the parse + per-key diff + apply when nothing changed —
    // one-shot requests (close/sub-dialog) flip the bytes, so they still fire.
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
    // Alias, not a copy. A nested applyAndDiff (modal sub-dialog exec below)
    // reassigns prev_view, so `view` must not be read after that exec returns —
    // today every use precedes it.
    const WidgetDataView& view = prev_view;
    auto close_reason = view.requestClose();
    auto sub_dialog_ui = view.subDialogUi();
    auto sub_panel_ui = view.subPanelUi();
    const bool sub_panel_close = view.subPanelClose();

    // One-shot commands are consumed by this call, so the skip-identical
    // shortcut above must not be allowed to compare the NEXT payload against
    // them: a plugin that answers "" while idle (the documented "no update")
    // leaves prev_raw parked on this very request, and an identical request
    // later (Settings, Cancel, Settings) would be thrown away as a repeat.
    const bool consumed_one_shot = close_reason.has_value() || sub_dialog_ui.has_value() || sub_panel_ui.has_value() ||
                                   sub_panel_close || new_data.contains("__request_accept");

    // Strip one-shot commands before diffing.
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
        // Mirror the same update into the live sub-panel so its preview/list/etc.
        // track the plugin's state; names it doesn't own are simply skipped.
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

    // Interactive sub-panel lifecycle (requestSubPanel / closeSubPanel). Close
    // before open so a same-tick close+reopen works; open is ignored if one is up.
    if (sub_panel_close) {
      closeSubPanelNow();
    }
    if (sub_panel_ui.has_value()) {
      openSubPanel(*sub_panel_ui, view);
    }

    // Open sub-dialog if requested (read-only modal popup, no event plumbing).
    if (sub_dialog_ui.has_value() && root != nullptr) {
      QByteArray sub_data(sub_dialog_ui->data(), static_cast<int>(sub_dialog_ui->size()));
      QBuffer sub_buffer(&sub_data);
      sub_buffer.open(QIODevice::ReadOnly);
      PjUiLoader sub_loader;
      QWidget* sub_loaded = sub_loader.load(&sub_buffer, root);
      if (sub_loaded != nullptr) {
        adaptStyledWidgets(sub_loaded);
        // Canonical chrome, same as dialog_engine's sub-dialogs: the plugin
        // content (QDialog-rooted or not) is embedded as the CONTENT of a
        // PJ::Dialog, which brings the themed title bar with a working close
        // button, drag-to-move, and the app dialog surface — the hand-rolled
        // frameless QDialog this replaces had a border but no affordances.
        auto* sub_dialog = new PJ::Dialog(root);
        sub_dialog->setDialogTitle(sub_loaded->windowTitle());
        sub_dialog->contentLayout()->addWidget(sub_loaded);
        // Size the dialog to the loaded root's preferred size (its .ui geometry /
        // sizeHint), clamped to the screen — PJ::Dialog otherwise opens at its own
        // minimal chrome size and ignores the plugin's requested dimensions.
        {
          const QSize want = sub_loaded->size().expandedTo(sub_loaded->sizeHint());
          const QSize avail =
              (sub_dialog->screen() != nullptr ? sub_dialog->screen() : QGuiApplication::primaryScreen())
                  ->availableSize();
          sub_dialog->resize(want.boundedTo(avail));
        }
        forwardEmbeddedDialogClose(sub_loaded, sub_dialog);
        // Wire the standard QDialogButtonBox (objectName "buttonBox") to
        // QDialog::accept/reject. Without this the OK/Cancel buttons are
        // inert — the OK click would do nothing.
        if (auto* button_box = sub_loaded->findChild<QDialogButtonBox*>(u"buttonBox"_s)) {
          QObject::connect(button_box, &QDialogButtonBox::accepted, sub_dialog, &QDialog::accept);
          QObject::connect(button_box, &QDialogButtonBox::rejected, sub_dialog, &QDialog::reject);
        }
        // Pre-fill the sub-dialog from the current widget data so it opens
        // populated. Names that don't exist in the sub-dialog are simply
        // skipped, so the panel's own widget values don't leak in.
        applyPanelData(sub_dialog, view);
        // Declarative per-combo field enabling/visibility ("pj_enable_when" /
        // "pj_visible_when" in the .ui) — the nested modal loop below blocks the
        // plugin from doing this itself.
        installDeclarativeRules(sub_dialog);
        // A visibility rule collapses form rows, so the dialog's natural size
        // depends on which combo indices are current: size it now that the
        // rules have run, and again (queued, after the rule handlers) whenever
        // a combo inside it changes — otherwise the rows a rule hid leave a
        // blank band behind, and the ones it revealed squeeze the rest.
        sub_dialog->adjustSize();
        // The chrome wrapping the dialog takes its geometry on show, after the
        // line above ran; a queued pass, dispatched by exec()'s own loop once
        // the dialog is up, sizes the shown window to the rows that survived.
        QTimer::singleShot(0, sub_dialog, [sub_dialog]() { sub_dialog->adjustSize(); });
        for (auto* combo : sub_dialog->findChildren<QComboBox*>()) {
          QObject::connect(
              combo, &QComboBox::currentIndexChanged, sub_dialog, [sub_dialog](int) { sub_dialog->adjustSize(); },
              Qt::QueuedConnection);
        }
        // exec() spins a nested modal event loop. Pause our tick timer for its
        // duration so a timer-driven applyAndDiff() can't re-enter on the same
        // Impl while the sub-dialog is open — a re-entrant tick could observe a
        // __request_close and tear us down (and delete sub_dialog) underneath
        // the suspended outer call. Resume only if we weren't closed meanwhile.
        const bool timer_was_active = (tick_timer != nullptr) && tick_timer->isActive();
        if (timer_was_active) {
          tick_timer->stop();
        }
        const int dlg_result = sub_dialog->exec();
        if (timer_was_active && !closed && tick_timer != nullptr) {
          tick_timer->start();
        }
        // When the user clicks OK, harvest the values the user typed
        // into the sub-dialog's inputs and surface them to the plugin
        // through the existing event channels. Plugins that want
        // user input from a sub-dialog override `onTextChanged` /
        // `onClicked` for the dialog's widget names (each must have
        // an objectName set in the .ui), then handle the synthetic
        // `subDialogAccepted` click as the "OK was pressed, commit
        // changes" signal. Without this loop, the values typed into
        // the sub-dialog are dropped on the floor.
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
                {"current_text", combo_box->currentText().toStdString()}, {"current_index", combo_box->currentIndex()}};
            (void)handle.sendEvent(name.toStdString(), ev.dump());
          }
          (void)handle.sendEvent("subDialogAccepted", R"({"clicked":true})");
        }
        delete sub_dialog;
      }
    }

    if (consumed_one_shot) {
      prev_raw.clear();
    }
    return close_reason;
  }
};

PanelEngine::PanelEngine(DialogHandle handle, PanelEngineConfig config, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(std::move(handle), config)) {}

PanelEngine::~PanelEngine() {
  close();
}

QWidget* PanelEngine::openPanel() {
  impl_->stats = {};
  impl_->request_owner_close = [this]() { this->close(); };

  // 1. Load the .ui blob into a QWidget.
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

  // 2. Apply initial widget data.
  std::string initial_raw = impl_->handle.widget_data();
  if (!initial_raw.empty()) {
    nlohmann::json initial_data = nlohmann::json::parse(initial_raw, nullptr, false);
    if (!initial_data.is_discarded()) {
      // Strip one-shot commands so they don't get re-applied on every tick.
      initial_data.erase("__request_close");
      initial_data.erase("__request_sub_dialog");
      initial_data.erase("__request_accept");
      impl_->prev_view = WidgetDataView(initial_raw);
      impl_->applyPanelData(loaded, impl_->prev_view);
      impl_->prev_data = std::move(initial_data);
      impl_->prev_raw = initial_raw;
    }
  }

  // Watch the root for app theme changes: host-themed icons are applied once and
  // then diffed away, so a live theme switch needs a re-apply to re-tint them
  // (see eventFilter). Seed the baseline with the theme just used above.
  impl_->applied_theme = currentTheme();
  loaded->installEventFilter(this);

  // 3. Wire widget signals to forward events into the plugin, and honor the
  //    declarative "pj_enable_when"/"pj_visible_when" combo->field rules from
  //    the .ui.
  installDeclarativeRules(loaded);
  connectWidgetSignals(loaded, [this](const std::string& name, const std::string& event_json) {
    if (impl_->closed) {
      return;
    }
    ++impl_->stats.event_count;
    // Forward an event to the plugin, apply the resulting widget data, and honour
    // a plugin-requested close.
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
    // Service file / save-file / folder pickers: a click on a widget the plugin
    // marked with the matching action opens the native chooser and feeds the
    // chosen path back as a fileSelected / folderSelected event. DialogEngine
    // does this too; toolbox dialogs are hosted here in PanelEngine, so without
    // this their Import/Export buttons would be inert. Reads the cached view:
    // a handled event just refreshed it via applyAndDiff, and re-polling
    // widget_data() here doubled the cost of every high-frequency event (a
    // slider drag) for metadata that changes at most once per tick.
    const PJ::WidgetDataView& picker_view = impl_->prev_view;
    if (picker_view.isFilePicker(name)) {
      const QString path = PJ::FileDialog::getOpenFileName(
          impl_->root, QString::fromStdString(picker_view.filePickerTitle(name).value_or("Select File")), QString(),
          QString::fromStdString(picker_view.filePickerFilter(name).value_or("")));
      if (!path.isEmpty()) {
        forward(name, PJ::WidgetEventBuilder::fileSelected(path.toStdString()));
      }
    } else if (picker_view.isSaveFilePicker(name)) {
      const QString path = PJ::FileDialog::getSaveFileName(
          impl_->root, QString::fromStdString(picker_view.filePickerTitle(name).value_or("Save File")), QString(),
          QString::fromStdString(picker_view.filePickerFilter(name).value_or("")),
          QString::fromStdString(picker_view.saveFilePickerDefaultSuffix(name).value_or("")));
      if (!path.isEmpty()) {
        forward(name, PJ::WidgetEventBuilder::fileSelected(path.toStdString()));
      }
    } else if (picker_view.isFolderPicker(name)) {
      const QString path = PJ::FileDialog::getExistingDirectory(
          impl_->root, QString::fromStdString(picker_view.folderPickerTitle(name).value_or("Select Folder")));
      if (!path.isEmpty()) {
        forward(name, PJ::WidgetEventBuilder::folderSelected(path.toStdString()));
      }
    }
  });

  // 3b. Wire the panel's standard QDialogButtonBox (objectName "buttonBox").
  // connectWidgetSignals deliberately skips buttons owned by a button box, and
  // unlike a modal dialog the non-modal panel has no QDialog::accept/reject to
  // fall back on — so without this the Close/OK buttons are inert (the reported
  // bug: Close does nothing in every toolbox). Route them through the same
  // close path as a plugin-requested __request_close.
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

  // 4. Install drop event filter for any declared drop targets.
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
    }
  }

  // 5. Install keyboard shortcuts declared in widget data.
  {
    WidgetDataView shortcut_view(impl_->handle.widget_data());
    installButtonShortcuts(loaded, shortcut_view);
  }

  // 6. Start tick timer.
  //
  // PanelEngine semantics differ from DialogEngine here: every tick polls
  // widget_data() unconditionally, regardless of whether the plugin's
  // on_tick reports state change. Long-lived panels frequently have their
  // state updated by external sources (worker threads, async fetch
  // callbacks) that the plugin's on_tick cannot observe. on_tick is still
  // called so the plugin can advance any internal periodic logic.
  impl_->tick_timer = new QTimer(this);
  impl_->tick_timer->setInterval(impl_->config.tick_interval_ms);
  QObject::connect(impl_->tick_timer, &QTimer::timeout, this, [this]() {
    // A hidden panel root (e.g. a toolbox pinned into a non-current central
    // tab) ticks at 1/kHiddenTickDivisor rate: the plugin's periodic logic
    // stays alive (async fetches keep progressing) while the invisible UI
    // skips most poll+diff work. A Show event delivers a catch-up tick
    // immediately (see eventFilter), so stale state never flashes on reveal.
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
  // A global stylesheet (theme) change re-polishes every widget with a
  // StyleChange. Host-themed icons (setButtonIconNamed → loadSvg) are applied
  // once and then diffed away, so they won't re-tint on their own. Re-apply the
  // panel's last full widget-data to re-theme them through the normal bind path.
  // Gated on the theme actually changing so unrelated StyleChange/polish events
  // (and the initial show) don't trigger a redundant full re-apply.
  //
  // This re-applies the FULL last widget-data (not a diff), so an in-flight user
  // edit the plugin hasn't echoed back into widget_data yet is reset to the
  // plugin's last-known value. That's a non-issue under the controlled-component
  // model (the plugin echoes user input back, so prev_raw matches the widget and
  // the re-apply is a no-op) and a theme toggle is rare and deliberate — cheap
  // enough that re-theming via the normal path beats tracking per-icon state.
  if (event->type() == QEvent::StyleChange || event->type() == QEvent::ApplicationPaletteChange) {
    const QString theme = currentTheme();
    if (theme != impl_->applied_theme && impl_->root != nullptr && !impl_->prev_raw.empty()) {
      impl_->applied_theme = theme;
      WidgetDataView view(impl_->prev_raw);
      impl_->applyPanelData(impl_->root, view);
      // The interactive sub-panel is themed through the same apply path at open
      // and on every tick — without this mirror its icons and chart colors
      // would be the one surface left stale after a theme switch.
      if (impl_->sub_panel != nullptr) {
        impl_->applyPanelData(impl_->sub_panel, view);
      }
    }
  }
  // A panel revealed after being hidden (tab switch back to a pinned toolbox)
  // may have skipped up to kHiddenTickDivisor-1 timer fires; catch up so the
  // user never sees stale widget state. Queued, not synchronous: a Show fired
  // mid-presentation (splitter replaceWidget / stack setCurrentWidget) must
  // not reenter the host's presentation bookkeeping through a plugin
  // requestClose before that bookkeeping is committed.
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
