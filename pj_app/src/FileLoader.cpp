// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "FileLoader.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLoggingCategory>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#if defined(PJ_WASM_ENABLE_INGRESS_PROBE) && defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif
#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BrowserFileStore.h"
#include "DialogPresenter.h"
#include "FanoutConfig.h"
#include "FileSelectionService.h"
#ifdef PJ_TARGET_WASM
#include "BrowserPickedFile.h"
#endif
#include "LayoutXml.h"
#include "pj_base/data_source_protocol.h"
#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_plugins/host/data_source_handle.hpp"
#include "pj_plugins/host/data_source_library.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#if defined(PJ_WASM_ENABLE_INGRESS_PROBE) && (defined(PJ_WASM_WITH_ROS_PLUGIN) || defined(PJ_WASM_WITH_PROTOBUF_PLUGIN))
#include "pj_base/builtin/camera_info.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#endif
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/DiagnosticEmit.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TsanAnnotations.h"
#include "pj_runtime/parse_locked.h"
#ifdef PJ_WITH_SCENE3D
#include "pj_runtime/TransformService.h"
#endif
#include "pj_widgets/Dialog.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/SvgUtil.h"

#ifdef PJ_WASM_ENABLE_MCAP_PROBE_PARSER
#include "../tests/wasm_mcap_probe_topics.h"
#endif

using namespace Qt::StringLiterals;

namespace PJ {

// Rendezvous between an import worker parked in the synchronous message-box ABI and the GUI thread that answers it.
// Shared, not FileLoader state: the queued GUI lambda and the blocked worker can outlive joinForShutdown and the
// FileLoader itself. A shut gate answers every request -1 and never opens UI.
struct PluginMessageGate {
  // The first answer (a button, dialog destruction, or shutdown) wins.
  struct Request {
    QSemaphore ready;

    void finish(int answer) {
      bool expected = false;
      if (!finished_.compare_exchange_strong(expected, true)) {
        return;
      }
      value_.store(answer);
      ready.release();
    }

    // Valid once ready.acquire() succeeded (release/acquire orders value_).
    [[nodiscard]] int result() const {
      return value_.load();
    }

   private:
    std::atomic<int> value_{-1};
    std::atomic<bool> finished_{false};
  };

  // Worker. False when the gate is shut: the caller answers -1 itself and posts no GUI work.
  [[nodiscard]] bool beginRequest(const std::shared_ptr<Request>& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) {
      return false;
    }
    pending_.push_back(request);
    return true;
  }

  void endRequest(const std::shared_ptr<Request>& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase(pending_, request);
  }

  [[nodiscard]] bool isShutDown() {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutting_down_;
  }

  // GUI: releases every parked worker with -1, so joinForShutdown can join the worker without pumping the event
  // queue its dialog request may still sit in.
  void shutdown() {
    std::vector<std::shared_ptr<Request>> pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      shutting_down_ = true;
      pending.swap(pending_);
    }
    for (const auto& request : pending) {
      request->finish(-1);
    }
  }

 private:
  std::mutex mutex_;
  bool shutting_down_ = false;
  std::vector<std::shared_ptr<Request>> pending_;
};

namespace {

Q_LOGGING_CATEGORY(lcFileLoader, "pj.app.fileloader")

// Awaits the callback-style dialog presenter, which may also complete synchronously (no dialog, contract error).
// The shared state keeps a late QDialog callback off a coroutine frame destroyed during shutdown.
class DataSourceDialogAwaiter {
 public:
  explicit DataSourceDialogAwaiter(dialog_presenter::DataSourceRequest request)
      : request_(std::move(request)), state_(std::make_shared<State>()) {}

  DataSourceDialogAwaiter(const DataSourceDialogAwaiter&) = delete;
  DataSourceDialogAwaiter& operator=(const DataSourceDialogAwaiter&) = delete;

  ~DataSourceDialogAwaiter() {
    // Also runs when a suspended frame is destroyed at shutdown. Neuter the completion first (resuming a frame
    // mid-destruction is UB), then close a still-open dialog so the plugin gets its on_rejected while the frame's
    // DataSourceHandle, declared before this awaiter and so destroyed after it, still owns a live context.
    state_->alive = false;
    if (cancel_dialog_) {
      cancel_dialog_();
    }
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  bool await_suspend(std::coroutine_handle<> continuation) {
    state_->continuation = continuation;
    state_->inside_await_suspend = true;
    const std::shared_ptr<State> state = state_;
    cancel_dialog_ = dialog_presenter::showDataSourceDialogAsync(
        request_, [state](dialog_presenter::DataSourceResult result) mutable {
          if (!state->alive) {
            return;
          }
          state->result = std::move(result);
          state->completed = true;
          if (!state->inside_await_suspend) {
            state->continuation.resume();
          }
        });
    state_->inside_await_suspend = false;
    return !state_->completed;
  }

  dialog_presenter::DataSourceResult await_resume() {
    return std::move(*state_->result);
  }

 private:
  struct State {
    std::optional<dialog_presenter::DataSourceResult> result;
    std::coroutine_handle<> continuation;
    bool inside_await_suspend = false;
    bool completed = false;
    bool alive = true;
  };

  dialog_presenter::DataSourceRequest request_;
  std::shared_ptr<State> state_;
  // Empty when the presenter completed without opening a dialog.
  dialog_presenter::DialogCancelFn cancel_dialog_;
};

// Runs a blocking importer body on a QThread and resumes the coroutine on the GUI thread after QThread::finished.
// The FileLoader owns both the QThread and the frame, so shutdown joins first, then drops the continuation.
class GuiResumingWorkerAwaiter {
 public:
  GuiResumingWorkerAwaiter(QObject* context, std::unique_ptr<QThread>& worker, std::function<void()> job)
      : context_(context), worker_(worker), state_(std::make_shared<State>()) {
    state_->job = std::move(job);
  }

  GuiResumingWorkerAwaiter(const GuiResumingWorkerAwaiter&) = delete;
  GuiResumingWorkerAwaiter& operator=(const GuiResumingWorkerAwaiter&) = delete;

  ~GuiResumingWorkerAwaiter() {
    state_->alive.store(false);
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<> continuation) {
    state_->continuation = continuation;
    const std::shared_ptr<State> state = state_;
    worker_ = std::unique_ptr<QThread>(QThread::create([state]() { state->job(); }));
    QObject::connect(
        worker_.get(), &QThread::finished, context_,
        [state]() {
          if (state->alive.load()) {
            state->continuation.resume();
          }
        },
        Qt::QueuedConnection);
    worker_->start();
  }

  void await_resume() {
    // QThread::finished has fired, so wait() returns at once; it keeps reset() from destroying the QThread while it
    // is still emitting that signal.
    worker_->wait();
    worker_.reset();
  }

 private:
  struct State {
    std::function<void()> job;
    std::coroutine_handle<> continuation;
    std::atomic_bool alive = true;
  };

  QPointer<QObject> context_;
  std::unique_ptr<QThread>& worker_;
  std::shared_ptr<State> state_;
};

#ifndef PJ_TARGET_WASM
constexpr const char* kLastDirKey = "FileLoader/lastDir";
#endif
constexpr const char* kPluginConfigKeyPrefix = "PluginConfig/";

struct LoadKeepalive {
  std::shared_ptr<void> plugin_library;
  StorageLease storage;
};

std::shared_ptr<void> combineKeepalive(std::shared_ptr<void> plugin_library, StorageLease storage) {
  if (!plugin_library) {
    return storage;
  }
  if (!storage) {
    return plugin_library;
  }
  return std::make_shared<LoadKeepalive>(
      LoadKeepalive{.plugin_library = std::move(plugin_library), .storage = std::move(storage)});
}

QString normalizeExtension(const QString& path) {
  const QString suffix = QFileInfo(path).suffix();
  return suffix.isEmpty() ? QString() : u"."_s + suffix.toLower();
}

// In the browser probe build, also appends to globalThis.pjWasmLoadOutcomes: Firefox can drop a console line before
// the test runner sees it, so the browser tests read outcomes from the page.
void recordProbeOutcome(const QString& line) {
  qCInfo(lcFileLoader).noquote() << line;
#if defined(PJ_WASM_ENABLE_INGRESS_PROBE) && defined(__EMSCRIPTEN__)
  const QByteArray utf8 = line.toUtf8();
  EM_ASM(
      { (globalThis.pjWasmLoadOutcomes = globalThis.pjWasmLoadOutcomes || []).push(UTF8ToString($0)); },
      utf8.constData());
#endif
}

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
void logSuccessfulLoad(
    const DataEngine& engine, const CatalogModel& catalog, const QString& source_identity, const QString& plugin_name,
    const std::vector<DatasetId>& dataset_ids) {
  // Diagnostic cost on the GUI thread: skip it when the category is off.
  if (!lcFileLoader().isInfoEnabled()) {
    return;
  }
  if (dataset_ids.empty()) {
    // The filter below would match the whole catalog.
    return;
  }

  constexpr qsizetype kMaxLoggedSeries = 32;
  QStringList scalar_series;
  QStringList series_samples;
  const DataReader reader(engine);
  std::size_t catalog_items = 0;
  std::size_t scalar_count = 0;
  std::vector<DatasetId> sorted_dataset_ids(dataset_ids);
  std::sort(sorted_dataset_ids.begin(), sorted_dataset_ids.end());
  for (const CatalogItem& item : catalog.items()) {
    if (!sorted_dataset_ids.empty() &&
        !std::binary_search(sorted_dataset_ids.begin(), sorted_dataset_ids.end(), item.dataset_id)) {
      continue;
    }
    ++catalog_items;
    if (const ScalarFieldPayload* field = asScalarField(item); field != nullptr) {
      ++scalar_count;
      if (scalar_series.size() >= kMaxLoggedSeries) {
        continue;
      }
      const QString series_name = item.topic_name + u"/"_s + field->field_path;
      scalar_series.push_back(series_name);
      if (isPlottablePrimitive(field->logical_type)) {
        auto series_or = reader.series(field->topic_id, field->column_index);
        if (series_or.has_value() && !series_or->empty()) {
          const auto first = series_or->sampleAt(0);
          const auto last = series_or->sampleAt(series_or->size() - 1);
          if (first.has_value() && last.has_value()) {
            series_samples.push_back(u"%1:%2@%3..%4=%5..%6"_s.arg(series_name)
                                         .arg(series_or->size())
                                         .arg(first->timestamp)
                                         .arg(last->timestamp)
                                         .arg(first->value, 0, 'g', 17)
                                         .arg(last->value, 0, 'g', 17));
          }
        }
      }
    }
  }
  scalar_series.sort();
  series_samples.sort();
  recordProbeOutcome(
      u"PJ_FILE_LOAD_OK plugin="_s + plugin_name + u" identity="_s + source_identity + u" catalog_items="_s +
      QString::number(catalog_items) + u" scalar_count="_s + QString::number(scalar_count) + u" scalar_series="_s +
      scalar_series.join(u","_s) + u" series_samples="_s + series_samples.join(u","_s));
}
#endif
#if defined(PJ_WASM_ENABLE_INGRESS_PROBE) && (defined(PJ_WASM_WITH_ROS_PLUGIN) || defined(PJ_WASM_WITH_PROTOBUF_PLUGIN))
void probeObjectDecoding(SessionManager& session, ObjectStore& object_store, DatasetId dataset_id) {
  for (const ObjectTopicId topic_id : object_store.listTopics(dataset_id)) {
    const size_t entry_count = object_store.entryCount(topic_id);
    if (entry_count == 0) {
      continue;
    }
    const auto descriptor = object_store.descriptor(topic_id);
    const auto resolved = object_store.at(topic_id, 0);
    const auto binding = session.parserBindingForObjectTopic(topic_id);
    const QString topic = QString::fromStdString(descriptor.topic_name);
    const QString marker = topic.startsWith(u"/foxglove/"_s) ? u"PJ_WASM_PROTOBUF_OBJECT"_s : u"PJ_WASM_ROS_OBJECT"_s;
    if (!resolved.has_value() || resolved->payload.bytes.empty() || !binding) {
      qCWarning(lcFileLoader).noquote() << marker + u"_FAILED"_s << u"topic=%1"_s.arg(topic)
                                        << u"entries=%1"_s.arg(entry_count)
                                        << u"reason=%1"_s.arg(
                                               !binding ? u"missing parser binding"_s : u"cold fetch failed"_s);
      continue;
    }

    auto record = PJ::parseLocked(binding, resolved->timestamp, resolved->payload);
    if (!record) {
      qCWarning(lcFileLoader).noquote() << marker + u"_FAILED"_s << u"topic=%1"_s.arg(topic)
                                        << u"entries=%1"_s.arg(entry_count)
                                        << u"reason=%1"_s.arg(QString::fromStdString(record.error()));
      continue;
    }

    const PJ::sdk::BuiltinObjectType type = PJ::sdk::typeOf(record->object);
    QString details;
    if (const auto* tf = record->object.get<PJ::sdk::FrameTransforms>(); tf != nullptr) {
      details = u"transforms=%1"_s.arg(tf->transforms.size());
      if (!tf->transforms.empty()) {
        details += u" parent=%1 child=%2"_s.arg(
            QString::fromStdString(tf->transforms.front().parent_frame_id),
            QString::fromStdString(tf->transforms.front().child_frame_id));
      }
    } else if (const auto* grid = record->object.get<PJ::sdk::OccupancyGrid>(); grid != nullptr) {
      details = u"width=%1 height=%2 cells=%3 frame=%4 resolution=%5"_s.arg(grid->width)
                    .arg(grid->height)
                    .arg(grid->data.size())
                    .arg(QString::fromStdString(grid->frame_id))
                    .arg(grid->resolution, 0, 'g', 17);
    } else if (const auto* cloud = record->object.get<PJ::sdk::PointCloud>(); cloud != nullptr) {
      details = u"width=%1 height=%2 fields=%3 bytes=%4 frame=%5"_s.arg(cloud->width)
                    .arg(cloud->height)
                    .arg(cloud->fields.size())
                    .arg(cloud->data.size())
                    .arg(QString::fromStdString(cloud->frame_id));
    } else if (const auto* poses = record->object.get<PJ::sdk::PosesInFrame>(); poses != nullptr) {
      details = u"poses=%1 frame=%2"_s.arg(poses->poses.size()).arg(QString::fromStdString(poses->frame_id));
    } else if (const auto* image = record->object.get<PJ::sdk::Image>(); image != nullptr) {
      details = u"width=%1 height=%2 encoding=%3 bytes=%4 frame=%5"_s.arg(image->width)
                    .arg(image->height)
                    .arg(QString::fromStdString(image->encoding))
                    .arg(image->data.size())
                    .arg(QString::fromStdString(image->frame_id));
    } else if (const auto* camera = record->object.get<PJ::sdk::CameraInfo>(); camera != nullptr) {
      details = u"width=%1 height=%2 frame=%3 fx=%4 fy=%5"_s.arg(camera->width)
                    .arg(camera->height)
                    .arg(QString::fromStdString(camera->frame_id))
                    .arg(camera->K[0], 0, 'g', 17)
                    .arg(camera->K[4], 0, 'g', 17);
    }

    qCInfo(lcFileLoader).noquote() << marker + u"_OK"_s << u"topic=%1"_s.arg(topic) << u"entries=%1"_s.arg(entry_count)
                                   << u"type=%1"_s.arg(QString::fromStdString(std::string(PJ::sdk::name(type))))
                                   << u"raw_bytes=%1"_s.arg(resolved->payload.bytes.size()) << details;
  }
}
#endif
#ifdef PJ_WASM_ENABLE_MCAP_PROBE_PARSER
void probeColdObjectFetch(ObjectStore& object_store, DatasetId dataset_id) {
  for (const ObjectTopicId topic_id : object_store.listTopics(dataset_id)) {
    if (object_store.entryCount(topic_id) == 0) {
      continue;
    }
    const auto resolved = object_store.at(topic_id, 0);
    const auto descriptor = object_store.descriptor(topic_id);
    if (!resolved.has_value() || resolved->payload.bytes.empty()) {
      qCWarning(lcFileLoader).noquote() << "PJ_WASM_MCAP_COLD_FETCH_FAILED"
                                        << u"topic=%1"_s.arg(QString::fromStdString(descriptor.topic_name));
      return;
    }
    qCInfo(lcFileLoader).noquote() << "PJ_WASM_MCAP_COLD_FETCH_OK"
                                   << u"topic=%1"_s.arg(QString::fromStdString(descriptor.topic_name))
                                   << u"bytes=%1"_s.arg(resolved->payload.bytes.size());
    return;
  }
}
#endif

// The saved config (the previous load's dialog state) with `filepath` set; an unparsable one counts as empty.
std::string buildLoadConfig(std::string_view saved_config, const QString& path) {
  QJsonObject obj;
  if (!saved_config.empty()) {
    const QByteArray bytes(saved_config.data(), static_cast<qsizetype>(saved_config.size()));
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (doc.isObject()) {
      obj = doc.object();
    }
  }
  obj.insert(u"filepath"_s, path);
  const QByteArray out = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  return std::string(out.constData(), static_cast<std::size_t>(out.size()));
}

QString pluginConfigKey(const std::string& plugin_id) {
  return QString::fromLatin1(kPluginConfigKeyPrefix) + QString::fromStdString(plugin_id);
}

// Called with the clicked PJ_MSG_BTN_* mask, or -1 (do not proceed) when the window is closed.
using PluginMessageCompletion = std::function<void(int)>;

struct PluginMessageState {
  PluginMessageCompletion completion;
  bool completed = false;

  void finish(int result) {
    if (completed) {
      return;
    }
    completed = true;
    completion(result);
  }
};

// For long plugin messages (thousands of skipped CSV rows): a plain message-box label would grow past the screen and
// push the buttons out of reach, so the text goes into a bounded monospace scroll view. A free function, not a branch
// in the message-box handler's nested lambdas, because MSVC rejects by-ref captures through that chain (C3493 /
// C2065). The WA_DeleteOnClose dialog can outlive the loader: its hooks capture `active_dialog`, never FileLoader.
void openScrollableMessageDialog(
    QWidget* dialog_parent, const QString& q_title, const QString& q_text, int type, int buttons,
    const std::shared_ptr<QPointer<QDialog>>& active_dialog, PluginMessageCompletion completion) {
  // The first line is the summary label; the rest always scrolls, or the label overflows again.
  const qsizetype nl = q_text.indexOf(QLatin1Char('\n'));
  const QString head = nl < 0 ? q_text : q_text.left(nl);
  const QString body = nl < 0 ? QString() : q_text.mid(nl + 1).trimmed();

  QString icon_path = u":/resources/svg/diag_info.svg"_s;
  if (type == PJ_MESSAGE_BOX_ERROR) {
    icon_path = u":/resources/svg/diag_error.svg"_s;
  } else if (type == PJ_MESSAGE_BOX_WARNING || type == PJ_MESSAGE_BOX_QUESTION) {
    icon_path = u":/resources/svg/diag_warning.svg"_s;
  }

  auto* dlg = new Dialog(dialog_parent);
  if (active_dialog != nullptr) {
    *active_dialog = dlg;
  }
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  // Not window-modal: that would leave floating ADS dock windows interactive.
  dlg->setWindowModality(Qt::ApplicationModal);
  dlg->setDialogTitle(q_title);
  dlg->setMinimumSize(520, 360);
  dlg->resize(560, 480);

  auto* body_widget = new QWidget;
  auto* vbox = new QVBoxLayout(body_widget);
  vbox->setContentsMargins(
      PJ::theme::space(theme::Space::Section), PJ::theme::space(theme::Space::Section),
      PJ::theme::space(theme::Space::Section), PJ::theme::space(theme::Space::Section));
  vbox->setSpacing(PJ::theme::space(theme::Space::Comfortable));

  auto* header = new QHBoxLayout();
  header->setSpacing(PJ::theme::space(theme::Space::Section));
  auto* icon_label = new QLabel(body_widget);
  // Use the (already-shown) parent's DPR; the dialog has no screen yet.
  const qreal dpr = dialog_parent != nullptr ? dialog_parent->devicePixelRatioF() : dlg->devicePixelRatioF();
  QPixmap icon_pm = renderSvgPixmap(icon_path, currentTheme(), QSize(32, 32), dpr);
  if (icon_pm.isNull()) {
    // A missing bundled resource shouldn't drop the severity cue.
    QStyle::StandardPixmap sp = QStyle::SP_MessageBoxInformation;
    if (type == PJ_MESSAGE_BOX_ERROR) {
      sp = QStyle::SP_MessageBoxCritical;
    } else if (type == PJ_MESSAGE_BOX_WARNING || type == PJ_MESSAGE_BOX_QUESTION) {
      sp = QStyle::SP_MessageBoxWarning;
    }
    icon_pm = dlg->style()->standardIcon(sp).pixmap(32, 32);
  }
  icon_label->setPixmap(icon_pm);
  header->addWidget(icon_label, 0, Qt::AlignTop);
  auto* head_label = new QLabel(head, body_widget);
  head_label->setWordWrap(true);
  header->addWidget(head_label, 1);
  vbox->addLayout(header);

  if (!body.isEmpty()) {
    auto* body_view = new QPlainTextEdit(body, body_widget);
    body_view->setReadOnly(true);
    body_view->setFrameShape(QFrame::NoFrame);
    body_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = body_view->font();
    mono.setFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    mono.setStyleHint(QFont::Monospace);
    body_view->setFont(mono);
    vbox->addWidget(body_view, 1);
  }

  // Themed through objectName + the msgbox_role property, as PJ::MessageBox does.
  struct BtnSpec {
    int mask;
    const char* label;
    const char* role;
  };
  const BtnSpec specs[] = {
      {PJ_MSG_BTN_OK, QT_TR_NOOP("OK"), "primary"},      {PJ_MSG_BTN_YES, QT_TR_NOOP("Yes"), "primary"},
      {PJ_MSG_BTN_NO, QT_TR_NOOP("No"), "neutral"},      {PJ_MSG_BTN_CONTINUE, QT_TR_NOOP("Continue"), "primary"},
      {PJ_MSG_BTN_ABORT, QT_TR_NOOP("Abort"), "cancel"}, {PJ_MSG_BTN_CANCEL, QT_TR_NOOP("Cancel"), "cancel"},
  };
  // No button we render: a lone OK, as openPluginMessageBox does.
  int wanted = buttons;
  int known = 0;
  for (const auto& s : specs) {
    known |= s.mask;
  }
  if ((wanted & known) == 0) {
    wanted = PJ_MSG_BTN_OK;
  }

  auto* footer = new QHBoxLayout();
  footer->addStretch(1);
  auto state = std::make_shared<PluginMessageState>(PluginMessageState{.completion = std::move(completion)});
  auto chosen = std::make_shared<int>(-1);
  for (const auto& s : specs) {
    if ((wanted & s.mask) == 0) {
      continue;
    }
    auto* btn = new QPushButton(QObject::tr(s.label), body_widget);
    btn->setObjectName(u"pjMessageBoxButton"_s);
    btn->setProperty("msgbox_role", QLatin1String(s.role));
    btn->setAutoDefault(false);
    btn->setDefault(std::strcmp(s.role, "primary") == 0);
    const int code = s.mask;
    QObject::connect(btn, &QPushButton::clicked, dlg, [chosen, code, dlg]() {
      *chosen = code;
      dlg->accept();
    });
    footer->addWidget(btn);
  }
  vbox->addLayout(footer);

  dlg->contentLayout()->addWidget(body_widget);
  QObject::connect(dlg, &QDialog::finished, qApp, [state, chosen, active_dialog, dlg](int) {
    if (active_dialog != nullptr && active_dialog->data() == dlg) {
      active_dialog->clear();
    }
    state->finish(*chosen);
  });
  QObject::connect(dlg, &QObject::destroyed, qApp, [state, active_dialog, dlg]() {
    if (active_dialog != nullptr && active_dialog->data() == dlg) {
      active_dialog->clear();
    }
    state->finish(-1);
  });
  // show(), not open(): open() force-downgrades ApplicationModal to WindowModal.
  dlg->show();
}

void openPluginMessageBox(
    QWidget* dialog_parent, const QString& q_title, const QString& q_text, int type, int buttons,
    const std::shared_ptr<QPointer<QDialog>>& active_dialog, PluginMessageCompletion completion) {
  constexpr int kInlineLineLimit = 12;
  if (q_text.count(QLatin1Char('\n')) >= kInlineLineLimit) {
    openScrollableMessageDialog(dialog_parent, q_title, q_text, type, buttons, active_dialog, std::move(completion));
    return;
  }

  auto* msg_box = new PJ::MessageBox(dialog_parent);
  if (active_dialog != nullptr) {
    *active_dialog = msg_box;
  }
  msg_box->setAttribute(Qt::WA_DeleteOnClose);
  // Not window-modal: see openScrollableMessageDialog.
  msg_box->setWindowModality(Qt::ApplicationModal);
  msg_box->setTitle(q_title);
  msg_box->setText(q_text);
  Q_UNUSED(type);

  std::vector<int> results;
  QPointer<QPushButton> acknowledgment;
  const auto add_button = [&](int mask, const QString& label, PJ::MessageBox::ButtonRole role) {
    if ((buttons & mask) != 0) {
      auto* button = msg_box->addButton(label, role);
      results.push_back(mask);
      if (mask == PJ_MSG_BTN_OK) {
        acknowledgment = button;
      }
    }
  };
  add_button(PJ_MSG_BTN_OK, QObject::tr("OK"), PJ::MessageBox::kPrimaryRole);
  add_button(PJ_MSG_BTN_CANCEL, QObject::tr("Cancel"), PJ::MessageBox::kCancelRole);
  add_button(PJ_MSG_BTN_YES, QObject::tr("Yes"), PJ::MessageBox::kPrimaryRole);
  add_button(PJ_MSG_BTN_NO, QObject::tr("No"), PJ::MessageBox::kNeutralRole);
  add_button(PJ_MSG_BTN_CONTINUE, QObject::tr("Continue"), PJ::MessageBox::kPrimaryRole);
  add_button(PJ_MSG_BTN_ABORT, QObject::tr("Abort"), PJ::MessageBox::kDestructiveRole);
  if (results.empty()) {
    acknowledgment = msg_box->addButton(QObject::tr("OK"), PJ::MessageBox::kPrimaryRole);
    results.push_back(PJ_MSG_BTN_OK);
  }

  auto state = std::make_shared<PluginMessageState>(PluginMessageState{.completion = std::move(completion)});
  QObject::connect(msg_box, &QDialog::finished, qApp, [state, msg_box, results, active_dialog](int) {
    if (active_dialog != nullptr && active_dialog->data() == msg_box) {
      active_dialog->clear();
    }
    const int clicked = msg_box->clickedIndex();
    state->finish(
        clicked >= 0 && clicked < static_cast<int>(results.size()) ? results[static_cast<std::size_t>(clicked)] : -1);
  });
  QObject::connect(msg_box, &QObject::destroyed, qApp, [state, active_dialog, msg_box]() {
    if (active_dialog != nullptr && active_dialog->data() == msg_box) {
      active_dialog->clear();
    }
    state->finish(-1);
  });
  // show(), not open(): open() force-downgrades ApplicationModal to WindowModal.
  msg_box->show();
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  // Probe build only: Chromium can leave this message box behind the Qt canvas after many fresh threaded-WASM
  // contexts. The missing-parser scenario proves presentation through PJ_WASM_PLUGIN_MESSAGE; clicking its only
  // button keeps the probe deterministic.
  if (q_title == u"Parser Error"_s) {
    QTimer::singleShot(1000, msg_box, [acknowledgment]() {
      if (acknowledgment != nullptr) {
        acknowledgment->click();
      }
    });
  }
#endif
}

DataSourceRuntimeHost::MessageBoxHandler makePluginMessageBoxHandler(
    QPointer<QWidget> message_parent, std::shared_ptr<QPointer<QDialog>> active_dialog,
    std::shared_ptr<PluginMessageGate> gate) {
  return [message_parent, active_dialog = std::move(active_dialog), gate = std::move(gate)](
             int type, std::string_view title, std::string_view message, int buttons) -> int {
    const QString q_title = QString::fromUtf8(title.data(), static_cast<int>(title.size()));
    const QString q_text = QString::fromUtf8(message.data(), static_cast<int>(message.size()));
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
    qCInfo(lcFileLoader).noquote() << "PJ_WASM_PLUGIN_MESSAGE" << u"title=%1"_s.arg(q_title)
                                   << u"text=%1"_s.arg(q_text);
#endif

    // The ABI is synchronous for the plugin, browser UI is not: a worker's question sleeps only that worker while
    // the GUI keeps dispatching; WASM never nests a Qt event loop.
    if (QThread::currentThread() == qApp->thread()) {
      if (gate->isShutDown()) {
        return -1;  // shutdown in progress: never open UI or spin a nested loop
      }
#ifdef PJ_TARGET_WASM
      qCWarning(lcFileLoader) << "plugin message box requested from the GUI thread; rejecting synchronous answer";
      openPluginMessageBox(message_parent.data(), q_title, q_text, type, buttons, active_dialog, [](int) {});
      return -1;
#else
      // Heap-shared: exec() can return without an answer (QCoreApplication::exit() ends nested loops) and the
      // dialog's hooks may complete afterwards, into live storage rather than this dead frame.
      struct SyncAnswer {
        QEventLoop loop;
        int answer = -1;
        bool answered = false;
      };
      auto state = std::make_shared<SyncAnswer>();
      openPluginMessageBox(message_parent.data(), q_title, q_text, type, buttons, active_dialog, [state](int value) {
        state->answer = value;
        state->answered = true;
        if (state->loop.isRunning()) {
          state->loop.quit();
        }
      });
      if (!state->answered) {
        state->loop.exec();
      }
      return state->answer;
#endif
    }

    auto request = std::make_shared<PluginMessageGate::Request>();
    if (!gate->beginRequest(request)) {
      return -1;  // shutdown already in progress: never park on the semaphore
    }
    const bool posted = QMetaObject::invokeMethod(
        qApp,
        [gate, request, message_parent, q_title, q_text, type, buttons, active_dialog]() {
          // May run after joinForShutdown and after the FileLoader died: capture shared state only.
          if (gate->isShutDown() || message_parent.isNull()) {
            request->finish(-1);
            return;
          }
          openPluginMessageBox(
              message_parent.data(), q_title, q_text, type, buttons, active_dialog,
              [request](int value) { request->finish(value); });
        },
        Qt::QueuedConnection);
    if (!posted) {
      gate->endRequest(request);
      return -1;
    }
    request->ready.acquire();
    gate->endRequest(request);
    return request->result();
  };
}

/// For coroutine frames: a frame destroyed while suspended skips the code after its co_await but still runs the
/// destructors of its locals.
template <typename T>
class ScopedPointerReset {
 public:
  explicit ScopedPointerReset(T*& slot) : slot_(slot) {}
  ~ScopedPointerReset() {
    slot_ = nullptr;
  }
  ScopedPointerReset(const ScopedPointerReset&) = delete;
  ScopedPointerReset& operator=(const ScopedPointerReset&) = delete;

 private:
  T*& slot_;
};

// The config layouts persist, stripped of host-private replay keys (a plugin may echo `__pj_saved_filepath`). A
// saveConfig failure is logged and yields an empty config: the layout then saves no plugin config.
std::string captureSavedConfig(DataSourceHandle& handle, const QString& source_name) {
  std::string captured_config;
  auto status = handle.saveConfig(captured_config);
  if (status) {
    return detail::stripHostReplayKeys(captured_config);
  }
  qCWarning(lcFileLoader).noquote() << FileLoader::tr(
                                           "Plugin '%1': saveConfig failed: %2 — layout save will skip plugin config")
                                           .arg(source_name, QString::fromStdString(status.error()));
  return {};
}

// Why a load that ran to completion is still a failure: it produced no row or entry. The plugin's own words, when it
// gave any: the last warning or error it reported during start(), else the reason of a FAILED request_stop.
QString noDataReason(const QString& source_name, const DataSourceRuntimeHost& host) {
  std::string problem = host.lastReportedProblem();
  if (problem.empty()) {
    problem = host.stopFailure();
  }
  const QString reason = FileLoader::tr("Plugin '%1' loaded no data").arg(source_name);
  return problem.empty() ? reason : reason + u": "_s + QString::fromStdString(problem);
}

// The preset with each layout side-file name (hints.side_file_paths) replaced by
// its path; an empty path resolves the name beside the loaded data file. A config
// that is not a JSON object is left for the filepath rewrite to judge.
std::string withSideFilePaths(const std::string& config, const LoadHints& hints, const QString& data_path) {
  if (hints.side_file_paths.isEmpty()) {
    return config;
  }
  const QDir data_dir = QFileInfo(data_path).dir();
  QHash<QString, QString> replacements;
  for (auto it = hints.side_file_paths.cbegin(); it != hints.side_file_paths.cend(); ++it) {
    replacements.insert(it.key(), it.value().isEmpty() ? data_dir.filePath(it.key()) : it.value());
  }
  return detail::replaceConfigStringValues(config, replacements).value_or(config);
}

// A browser upload's identity (pj-upload://<session>/<id>/<name>) is internal; messages name the picked file.
QString shownSourceName(const QString& identity) {
  const QString name = BrowserFileStore::displayNameForIdentity(identity);
  return name.isEmpty() ? identity : name;
}

}  // namespace

// Under kLazyObjectsEagerScalars each object payload is read once at ingest for its scalar series (header stamp,
// frame_id, counts) and then dropped; object pulls re-read the source. Pure-lazy (no ingest read, no scalars) only
// where that read buys nothing: video frames (a full-file pass for a few columns) and scene entities / image
// annotations (no useful scalars). TF stays eager: tiny payloads, useful scalars. Shared by the single-instance
// ingest session and every fan-out iteration.
void FileLoader::applyDefaultIngestPolicies(PJ::sdk::ObjectIngestPolicyResolver& resolver) {
  using PJ::sdk::BuiltinObjectType;
  using PJ::sdk::ObjectIngestPolicy;
  resolver.setDefault(ObjectIngestPolicy::kLazyObjectsEagerScalars);
  resolver.setForType(BuiltinObjectType::kSceneEntities, ObjectIngestPolicy::kPureLazy);
  resolver.setForType(BuiltinObjectType::kImageAnnotations, ObjectIngestPolicy::kPureLazy);
  resolver.setForType(BuiltinObjectType::kVideoFrame, ObjectIngestPolicy::kPureLazy);
#ifdef PJ_TARGET_WASM
  // The browser RobotModel reads std_msgs/String through the object parser, which has no scalar handler: keep the
  // payload lazy until the layer asks for the selected description.
  resolver.setForType(BuiltinObjectType::kRobotDescription, ObjectIngestPolicy::kPureLazy);
#endif
#ifdef PJ_WASM_ENABLE_MCAP_PROBE_PARSER
  // Probe build only: the 50k-message cancellation fixture's topic goes eager, so the browser test has a
  // deterministic window in which to cancel.
  resolver.setForTopic(kWasmProbeBulkTopic, ObjectIngestPolicy::kEager);
#endif
}

namespace {

// A loader plugin instance and the host services it is bound to. After bind() the SDK's DataSourcePluginBase keeps the
// registry (its services() lookups) and views into the ingest host's services, up to its destructor: `handle` is
// declared last, so the plugin is destroyed before the registry and the host it reaches through them.
struct BoundSource {
  explicit BoundSource(DataSourceHandle plugin) : handle(std::move(plugin)) {}

  // Registers the ingest host's services in a registry owned here and binds the plugin to it. Requires `ingest`.
  [[nodiscard]] Status bind() {
    registry = std::make_unique<ServiceRegistryBuilder>();
    if (auto status = ingest->registerServices(*registry); !status) {
      return unexpected("service registration failed: " + status.error());
    }
    if (auto status = handle.bind(registry->view()); !status) {
      return unexpected("bind failed: " + status.error());
    }
    return okStatus();
  }

  std::unique_ptr<DataSourceRuntimeHost> ingest;
  std::unique_ptr<ServiceRegistryBuilder> registry;  // heap: the plugin holds its address
  DataSourceHandle handle;
};

}  // namespace

// A single-instance load's bound plugin, kept alive from the GUI prologue through the worker run.
struct FileLoader::LoadContext {
  explicit LoadContext(BoundSource bound_source) : bound(std::move(bound_source)) {}

  QString path;
  QPointer<QWidget> dialog_parent;
  QString source_name;  // the plugin's display name
  QString source_manifest_id;
  std::string config;
  DatasetId dataset_id = 0;
  quint64 ticket = 0;
  bool replacing = false;
  // Replacing loads: the name applied at commit, so a rollback keeps the prior name.
  QString commit_display_name;
  // Replacing loads: committed on success or keep (onWorkerFinished); otherwise its destructor, run by ctx_.reset(),
  // rolls the dataset back.
  std::optional<RefillGuard> refill_guard;
  int file_index = 1;
  int file_total = 1;
  BoundSource bound;
  // Files picked in the plugin's dialog (a DBC, say): the worker's import reads
  // their browser-staged copies after this prologue's frame is gone.
  std::vector<std::shared_ptr<void>> dialog_selected_files;
  IngestToken ingest_token{};
  // Started on the GUI before the thread runs; afterwards only the worker touches it.
  QElapsedTimer flush_clock;
  // Written once by on_progress_start (worker); read by the queued progress lambda.
  uint64_t progress_total = 0;
  // Worker -> GUI, read in onWorkerFinished after the join.
  bool start_ok = false;
  QString start_error;
};

struct FileLoader::BeginLoadTask {
  struct promise_type {
    BeginLoadTask get_return_object() {
      return BeginLoadTask{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_never initial_suspend() const noexcept {
      return {};
    }
    std::suspend_always final_suspend() const noexcept {
      return {};
    }
    void return_void() const noexcept {}
    void unhandled_exception() const noexcept {
      std::terminate();
    }
  };

  explicit BeginLoadTask(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
  // No move assignment: it lives only in a std::unique_ptr.
  BeginLoadTask(BeginLoadTask&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  BeginLoadTask& operator=(BeginLoadTask&&) = delete;
  BeginLoadTask(const BeginLoadTask&) = delete;
  BeginLoadTask& operator=(const BeginLoadTask&) = delete;
  ~BeginLoadTask() {
    if (handle_) {
      handle_.destroy();
    }
  }

 private:
  std::coroutine_handle<promise_type> handle_;
};

FileLoader::FileLoader(
    SessionManager& session, ExtensionCatalogService& extensions, CatalogModel& catalog, QObject* parent)
    : QObject(parent),
      session_(session),
      extensions_(extensions),
      catalog_(catalog),
      browser_file_store_(std::make_shared<BrowserFileStore>()),
      active_plugin_message_dialog_(std::make_shared<QPointer<QDialog>>()),
      message_gate_(std::make_shared<PluginMessageGate>()) {
#ifdef PJ_TARGET_WASM
  // A picked file is read lazily, for the whole session: a change on disk after the pick turns up later as failed
  // reads, which a std::istream-based source takes for the end of the file. Say so, or the data looks complete.
  browser_picked_file::setReadFailureHandler([this](const QString& path) {
    // Its content is no longer what was fingerprinted: a layout must not pin the session to that fingerprint, nor
    // to one a hash still running stores later.
    browser_content_sha256_.remove(browser_identity_by_path_.take(path));
    emitDiagnosticTo(
        diagnostic_sink_, DiagnosticLevel::kWarning, "File Loader", "browser-file-read-failed",
        tr("The browser can no longer read '%1': it changed or was removed on disk since it was opened. Data loaded "
           "from it may be incomplete; open it again to reload it.")
            .arg(QFileInfo(path).fileName())
            .toStdString());
  });
#endif
}

FileLoader::~FileLoader() {
#ifdef PJ_TARGET_WASM
  browser_picked_file::setReadFailureHandler({});
#endif
  // joinForShutdown's synchronous terminal flush runs loadFinished slots; a load one of them starts now would never
  // resolve and its worker would outlive this object.
  destroying_ = true;
  joinForShutdown();
}

bool FileLoader::sameSourceIdentity(const QString& lhs, const QString& rhs) {
  if (isBrowserUploadIdentity(lhs) || isBrowserUploadIdentity(rhs)) {
    return lhs == rhs;
  }
  return layout_xml::isSamePath(lhs, rhs);
}

void FileLoader::openFromDialog(QWidget* dialog_parent) {
#ifdef PJ_TARGET_WASM
  loadFromBrowserPicker(dialog_parent, {});
  return;
#else
  const QString filter = extensions_.buildFileFilter();
  QSettings settings;
  const QString last_dir = settings.value(kLastDirKey, QString()).toString();

  const QStringList paths = file_picker_ != nullptr
                                ? file_picker_(dialog_parent, tr("Load Data"), last_dir, filter, /*multi=*/true)
                                : FileDialog::getOpenFileNames(dialog_parent, tr("Load Data"), last_dir, filter);
  if (paths.isEmpty()) {
    return;
  }
  settings.setValue(kLastDirKey, QFileInfo(paths.last()).absolutePath());
  loadFiles(paths, dialog_parent);
#endif
}

void FileLoader::loadFiles(const QStringList& paths, QWidget* dialog_parent) {
  if (paths.size() < 2 || dialog_parent == nullptr || !batch_asker_ || !batchRepeatsADialog(paths)) {
    for (const QString& path : paths) {
      loadFile(path, dialog_parent);
    }
    return;
  }
  QPointer<FileLoader> self(this);
  QPointer<QWidget> parent(dialog_parent);
  batch_asker_(dialog_parent, paths, [self, parent, paths](std::shared_ptr<LoadBatch> batch) {
    if (self.isNull() || parent.isNull() || batch == nullptr) {
      return;
    }
    LoadHints hints;
    hints.batch = std::move(batch);
    for (const QString& path : paths) {
      self->loadFile(path, parent, hints);
    }
  });
}

bool FileLoader::batchRepeatsADialog(const QStringList& paths) const {
  QHash<QString, int> files_per_loader;
  for (const QString& path : paths) {
    const auto matches = extensions_.findSourcesForExtension(normalizeExtension(path));
    if (!matches.empty() && (matches.front()->capabilities & PJ_DATA_SOURCE_CAPABILITY_HAS_DIALOG) != 0 &&
        ++files_per_loader[QString::fromStdString(matches.front()->id)] > 1) {
      return true;
    }
  }
  return false;
}

QStringList FileLoader::droppedFilePaths(const QMimeData& mime) {
  QStringList paths;
  for (const QUrl& url : mime.urls()) {
    if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
      paths.append(url.toLocalFile());
    }
  }
  return paths;
}

void FileLoader::replaceFromDialog(DatasetId dataset_id, QWidget* dialog_parent) {
  LoadHints hints;
  hints.replace_dataset_id = dataset_id;
#ifdef PJ_TARGET_WASM
  loadFromBrowserPicker(dialog_parent, hints);
#else
  const QString filter = extensions_.buildFileFilter();
  QSettings settings;
  const QString last_dir = settings.value(kLastDirKey, QString()).toString();
  QString path;
  if (file_picker_ != nullptr) {
    const QStringList picked = file_picker_(dialog_parent, tr("Replace Data"), last_dir, filter, /*multi=*/false);
    path = picked.isEmpty() ? QString() : picked.first();
  } else {
    path = FileDialog::getOpenFileName(dialog_parent, tr("Replace Data"), last_dir, filter);
  }
  if (path.isEmpty()) {
    return;
  }
  settings.setValue(kLastDirKey, QFileInfo(path).absolutePath());
  loadFile(path, dialog_parent, hints);
#endif
}

#ifdef PJ_TARGET_WASM
void FileLoader::loadFromBrowserPicker(QWidget* dialog_parent, LoadHints hints) {
  // The browser picker must open in this user-activation turn; no nested event loop.
  QPointer<FileLoader> self(this);
  QPointer<QWidget> guarded_parent(dialog_parent);
  // A plain open needs no fingerprint up front (only saving a source-bound layout does), so it is computed while the
  // file loads instead of before its dialog opens.
  selectBrowserInput(
      dialog_parent,
      [self, guarded_parent, hints = std::move(hints)](BrowserSelectionResult staged) mutable {
        if (self.isNull() || (!staged.input.has_value() && staged.error.isEmpty())) {
          return;  // loader destroyed, or picker cancelled.
        }
        if (!staged.input.has_value()) {
          const QString reason = staged.error.isEmpty() ? self->tr("Could not stage the selected file.") : staged.error;
          self->logLoadFailure(staged.browser_name, reason);
          self->reportLoadWarning(guarded_parent.data(), reason);
          emit self->fileLoadFailed(staged.browser_name, reason);
          return;
        }
        self->loadFile(std::move(*staged.input), guarded_parent, hints);
      },
      FileSelectionService::Fingerprint::kInBackground);
}

void FileLoader::selectBrowserInput(
    QWidget* dialog_parent, BrowserSelectionCallback callback, FileSelectionService::Fingerprint fingerprint,
    const QString& name_filter) {
  const QString filter = name_filter.isEmpty() ? extensions_.buildFileFilter() : name_filter;
  qCInfo(lcFileLoader).noquote() << "Opening browser file picker with filter:" << filter;
  FileSelectionService::selectAndStageFile(
      dialog_parent, filter, browser_file_store_,
      [callback = std::move(callback)](FileSelectionService::StagedSelection staged) mutable {
        callback({
            .browser_name = std::move(staged.browser_name),
            .input = std::move(staged.input),
            .error = std::move(staged.error),
        });
      },
      fingerprint);
}
#endif

QString FileLoader::browserContentSha256(const QString& source_identity) const {
  return browser_content_sha256_.value(source_identity);
}

void FileLoader::rememberBrowserUpload(const LoadInput& input) {
  if (!input.content_sha256.isEmpty()) {
    browser_content_sha256_.insert(input.source_identity, input.content_sha256);
  }
  if (isBrowserUploadIdentity(input.source_identity)) {
    browser_identity_by_path_.insert(input.backing_path, input.source_identity);
  }
}

QString FileLoader::browserIdentityForPath(const QString& backing_path) const {
  return browser_identity_by_path_.value(backing_path);
}

QString FileLoader::browserPathForIdentity(const QString& source_identity) const {
  return browser_identity_by_path_.key(source_identity);
}

FileLoader::BeginLoadTask FileLoader::beginLoad(LoadRequest request) {
  bool worker_handoff = false;
  const auto complete_prologue = qScopeGuard([this, &worker_handoff]() {
    if (!worker_handoff) {
      finishPrologue();
    }
  });

  const LoadInput& input = request.input;
  const QString& source_identity = input.source_identity;
  const QPointer<QWidget> dialog_parent = request.dialog_parent;
  const LoadHints& hints = request.hints;
  const quint64 ticket = request.ticket;
  // Strict replacement: the load is pinned to replace_dataset_id and every
  // divergence (vanished target, fan-out expansion) fails instead of degrading.
  const bool strict_replace = hints.require_replacement && hints.replace_dataset_id != 0;

  // Restore same-source datasets if a replacement load is cancelled or fails.
  std::vector<DatasetId> tombstoned_for_replace;
  DatasetId created_live_dataset_id = 0;
  const auto rollback_tombstones = [&]() {
    for (const DatasetId id : tombstoned_for_replace) {
      catalog_.restoreDataset(id);
    }
    tombstoned_for_replace.clear();
  };
  const auto erase_created_live_dataset = [&]() {
    if (created_live_dataset_id == 0) {
      return;
    }
    removeCreatedDataset(created_live_dataset_id);
    created_live_dataset_id = 0;
  };
  bool rollback_armed = true;
  const auto rollback_on_cancel = qScopeGuard([&]() {
    if (rollback_armed) {
      erase_created_live_dataset();
      rollback_tombstones();
    }
  });

  // Always followed by co_return. Terminalizes the ticket before the re-entrant warning dialog and fileLoadFailed.
  const auto fail = [&](const QString& reason) -> bool {
    erase_created_live_dataset();
    rollback_tombstones();
    emitLoadFinished(ticket, LoadOutcome::kFailed, source_identity, 0, {}, reason);
    logLoadFailure(source_identity, reason);
    reportLoadWarning(dialog_parent.data(), reason);
    emit fileLoadFailed(source_identity, reason);
    return false;
  };
  // The caller's right to load lapsed (LoadHints::still_allowed): a terminal cancel, silent like
  // a declined dialog.
  const auto disallowed = [&]() -> bool {
    if (!hints.still_allowed || hints.still_allowed()) {
      return false;
    }
    qCInfo(lcFileLoader).noquote() << "load no longer allowed by its caller:" << source_identity;
    cancel_mode_.store(0);  // a cancel latched for this load is honoured: it ends here
    erase_created_live_dataset();
    rollback_tombstones();
    emitLoadFinished(ticket, LoadOutcome::kCancelled, source_identity, 0);
    return true;
  };
  if (disallowed()) {
    co_return;
  }

  const QString ext = normalizeExtension(input.display_name);
  if (ext.isEmpty()) {
    (void)fail(tr("File has no extension; cannot pick a plugin."));
    co_return;
  }

  const auto matches = extensions_.findSourcesForExtension(ext);
  if (matches.empty()) {
    (void)fail(tr("No DataSource plugin handles %1 files. Install one from the Marketplace.").arg(ext));
    co_return;
  }

  // The first match, unless a layout names its plugin: another match could parse the same bytes differently.
  // `source` points into the GUI-owned catalog, which a marketplace reload can reallocate: use it only before the
  // dialog suspends; after that, the snapshots below (the handle pins the DSO).
  const LoadedDataSource* source = matches.front();
  if (hints.require_expected_plugin) {
    const LoadedDataSource* expected =
        findDataSourceByIdentity(matches, hints.expected_manifest_id, hints.expected_plugin_id);
    if (expected == nullptr) {
      const QString expected_label = !hints.expected_plugin_id.isEmpty()     ? hints.expected_plugin_id
                                     : !hints.expected_manifest_id.isEmpty() ? hints.expected_manifest_id
                                                                             : tr("(unspecified)");
      (void)fail(tr("The layout requires DataSource plugin '%1', but it is not installed for %2 files.")
                     .arg(expected_label, ext));
      co_return;
    }
    source = expected;
  }
  const QString source_name = QString::fromStdString(source->name);
  const QString source_manifest_id = QString::fromStdString(source->id);
  // A batch: a loader whose dialog was cancelled cancels its other files, before anything is created for them.
  if (hints.batch != nullptr && hints.batch->cancelled.contains(source_manifest_id)) {
    erase_created_live_dataset();
    rollback_tombstones();
    emitLoadFinished(ticket, LoadOutcome::kCancelled, source_identity, 0);
    co_return;
  }

  BoundSource bound(source->library.createHandle());
  DataSourceHandle& handle = bound.handle;
  if (!handle.valid()) {
    (void)fail(tr("Plugin '%1': createHandle failed.").arg(source_name));
    co_return;
  }
  const PJ_data_source_vtable_t* const source_vtable = source->library.vtable();
  const std::shared_ptr<void> source_library_owner = handle.libraryOwner();
  const std::string source_id = source->id;
  const std::string source_plugin_name = source->name;

  // The DataSource protocol resolves host services in bind(), so the dataset must exist before bind().
  DataEngine& engine = session_.dataEngine();

  const QString display_name = input.display_name;
  const std::string display_name_utf8 = display_name.toStdString();

  // One TimeDomain per loaded source, so the Source Timeline can shift each on its own.
  auto td_or = engine.createTimeDomain(display_name_utf8);
  if (!td_or.has_value()) {
    (void)fail(tr("Could not create the time domain for %1.").arg(display_name));
    co_return;
  }
  const TimeDomainId td_id = *td_or;

  // Same source: layout replay reuses the DatasetId; a load or reload refills it in place, keeping DatasetId and
  // TopicIds (so curve keys) stable. Matching is by full-path identity: two files sharing a basename stay distinct.
  DatasetId existing_primary_id = 0;
  if (hints.replace_dataset_id != 0 && engine.getDataset(hints.replace_dataset_id) != nullptr) {
    existing_primary_id = hints.replace_dataset_id;
  } else {
    if (strict_replace) {
      (void)fail(tr("Replacement target dataset %1 no longer exists.").arg(hints.replace_dataset_id));
      co_return;
    }
    // sameSourceIdentity canonicalizes paths (syscalls): once per dataset. Two ranked matches:
    // 1. Path identity, only when exactly one dataset has the path. A replaced dataset keeps its original engine
    //    basename, so only its path finds it; fan-out children share one file and must not be refilled singly.
    // 2. Engine basename, gated on path identity when a path is tracked; untracked datasets (streaming, tests)
    //    match by basename alone.
    // A merged-away dataset is never a match: its label names the anchor's data, and the plugin snapshot does not
    // list it, so a refill there would be invisible to plugins and agents.
    DatasetId sole_path_match = 0;
    bool path_match_ambiguous = false;
    DatasetId basename_match = 0;
    for (const auto existing_id : engine.listDatasets()) {
      const DatasetInfo* info = engine.getDataset(existing_id);
      if (info == nullptr || engine.isMergedAway(existing_id)) {
        continue;
      }
      const QString tracked_path = session_.datasetSourcePath(existing_id);
      const bool path_matches = !tracked_path.isEmpty() && sameSourceIdentity(tracked_path, source_identity);
      if (path_matches) {
        path_match_ambiguous = sole_path_match != 0;
        sole_path_match = existing_id;
      }
      if (basename_match == 0 && info->source_name == display_name_utf8 && (tracked_path.isEmpty() || path_matches)) {
        basename_match = existing_id;
      }
    }
    existing_primary_id = (sole_path_match != 0 && !path_match_ambiguous) ? sole_path_match : basename_match;
  }

  if (existing_primary_id != 0 && hints.prefer_reuse) {
    // The emitted config is what a later Reload replays verbatim, so a preset gets a fresh load's path rewrite (a
    // non-object preset is emitted as it is). Without a preset, the recorded config of this path is kept.
    QString emit_config = hints.preset_config_json;
    if (!emit_config.isEmpty()) {
      const std::optional<std::string> rewritten = detail::rewriteReplayFilepaths(
          withSideFilePaths(emit_config.toStdString(), hints, input.backing_path), input.backing_path,
          detail::SavedFilepath::kOmit);
      if (rewritten.has_value()) {
        emit_config = QString::fromStdString(*rewritten);
      }
    } else {
      // Canonical comparison: a symlink or relative alias still finds its config.
      const auto& prior = session_.loadedSources();
      const auto it = std::find_if(prior.begin(), prior.end(), [&source_identity](const auto& src) {
        return sameSourceIdentity(src.path, source_identity);
      });
      if (it != prior.end()) {
        emit_config = it->plugin_config_json;
      }
    }
    catalog_.restoreDataset(existing_primary_id);
    session_.setDatasetSourcePath(existing_primary_id, source_identity);
    emitLoadFinished(ticket, LoadOutcome::kLoaded, source_identity, existing_primary_id, {existing_primary_id});
    emit fileLoaded(source_identity, QString(), source_name, emit_config, source_manifest_id);
    co_return;
  }

  // Ingest always targets the live engine. A same-source single-instance reload refills the existing dataset under
  // a RefillGuard; ensureTopic reuses topics by name, so their ids stay. A fan-out reload cannot refill one dataset
  // into N: it removes and loads fresh (see the fan-out branch).
  const bool replacing = (existing_primary_id != 0);
  DatasetId dataset_id = existing_primary_id;
  if (!replacing) {
    auto dataset_or =
        engine.createDataset(DatasetDescriptor{.source_name = display_name_utf8, .time_domain_id = td_id});
    if (!dataset_or.has_value()) {
      (void)fail(tr("createDataset failed: %1").arg(QString::fromStdString(dataset_or.error())));
      co_return;
    }
    dataset_id = static_cast<DatasetId>(*dataset_or);
  }
  const PJ_data_source_handle_t source_handle{static_cast<uint32_t>(dataset_id)};

  // A prologue failure erases it, or a prefer_reuse replay could reattach to the empty shell. Worker failures and
  // discards are handled in onWorkerFinished.
  if (!replacing) {
    created_live_dataset_id = dataset_id;
  }

  DataSourceRuntimeHost::ObjectTopicParserRegistrar object_parser_registrar =
      [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
        session_.registerObjectTopicParser(id, std::move(parser));
      };
  // Heap-owned: a single-instance load moves it into ctx_ for the worker; the reference below stays valid.
  bound.ingest = std::make_unique<DataSourceRuntimeHost>(
      engine, extensions_, dataset_id, source_handle, session_.objectStore(), source_id,
      std::move(object_parser_registrar), nullptr, nullptr, combineKeepalive(handle.libraryOwner(), input.lease),
      session_.ingestTapsShared());
  DataSourceRuntimeHost& ingest_session = *bound.ingest;
  ingest_session.setDiagnosticSink(diagnostic_sink_, source_plugin_name, display_name_utf8);
  DataSourceRuntimeHost::MessageBoxHandler message_box_handler;
  if (dialog_parent != nullptr) {
    message_box_handler = makePluginMessageBoxHandler(dialog_parent, active_plugin_message_dialog_, message_gate_);
    ingest_session.setMessageBoxHandler(message_box_handler);
  }
  applyDefaultIngestPolicies(ingest_session.policyResolver());

  if (auto status = bound.bind(); !status) {
    (void)fail(tr("Plugin '%1': %2").arg(source_name, QString::fromStdString(status.error())));
    co_return;
  }

  // The last-used settings pre-fill the dialog.
  QSettings persisted_settings;
  const QString config_key = pluginConfigKey(source_plugin_name);
  const std::string saved_config = persisted_settings.value(config_key, QString()).toString().toStdString();

  std::string config;
  bool skip_dialog = false;
  // Side-file names already resolved to the files this replay reads.
  const std::string preset_bytes = withSideFilePaths(hints.preset_config_json.toStdString(), hints, input.backing_path);

  if (hints.dialog_policy == DialogPolicy::kNever) {
    // A rejected preset fails the source: the QSettings pre-fill would replay a configuration the user never saved.
    auto rewritten = detail::rewriteReplayFilepaths(
        preset_bytes, input.backing_path,
        hints.record_saved_filepath && !isBrowserUploadIdentity(source_identity) ? detail::SavedFilepath::kRecord
                                                                                 : detail::SavedFilepath::kOmit);
    if (!rewritten.has_value()) {
      (void)fail(
          tr("Plugin '%1': the saved configuration is not a JSON object and cannot be replayed.").arg(source_name));
      co_return;
    }
    std::string preset = std::move(*rewritten);
    if (auto status = handle.loadConfig(preset); !status) {
      (void)fail(tr("Plugin '%1' rejected the layout's saved configuration: %2")
                     .arg(source_name, QString::fromStdString(status.error())));
      co_return;
    }
    config = preset;
    skip_dialog = true;
  } else {
    // Each hint identity compares only against its own field of the selected plugin.
    const bool expected_matches_selected =
        (!hints.expected_manifest_id.isEmpty() && hints.expected_manifest_id == source_manifest_id) ||
        (!hints.expected_plugin_id.isEmpty() && hints.expected_plugin_id == source_name);
    if (hints.dialog_policy == DialogPolicy::kPreferPreset && !preset_bytes.empty() && expected_matches_selected) {
      if (auto status = handle.loadConfig(preset_bytes); status) {
        config = preset_bytes;
        skip_dialog = true;
      } else {
        qCInfo(lcFileLoader).noquote() << tr("Layout preset rejected by '%1': %2 — falling back to dialog")
                                              .arg(source_name, QString::fromStdString(status.error()));
      }
    }
    // A batch reuses the last-used settings, which an earlier dialog of this batch saved before its import started. A
    // loader with no saved settings, or whose settings are about that file's own contents (a fan-out into its
    // datasets, a display name for its tree root), still shows its dialog.
    bool batch_reuse = false;
    if (!skip_dialog && hints.batch != nullptr) {
      const std::vector<std::string> instances = detail::extractFanout(saved_config);
      const bool fans_out = instances.size() != 1 || instances.front() != saved_config ||
                            !detail::parseDisplayName(saved_config).isEmpty();
      batch_reuse =
          !saved_config.empty() && !fans_out &&
          (hints.batch->mode == BatchDialogMode::kSkip ||
           (hints.batch->mode == BatchDialogMode::kShowFirst && hints.batch->prompted.contains(source_manifest_id)));
    }
    if (!skip_dialog) {
      config = buildLoadConfig(saved_config, input.backing_path);
      if (auto status = handle.loadConfig(config); !status) {
        (void)fail(tr("Plugin '%1': loadConfig failed: %2").arg(source_name, QString::fromStdString(status.error())));
        co_return;
      }
      skip_dialog = batch_reuse;
    }
  }

  // Browser-staged copies of files picked in the dialog (a DBC, say) must outlive it while the import reads them:
  // the fan-out path imports in this frame, the single-instance path hands them to ctx_.
  std::vector<std::shared_ptr<void>> dialog_selected_files;
  if (!skip_dialog) {
    // Restores the embedded parser dialog to its last state.
    std::string initial_parser_config;
    {
      auto saved_cfg = nlohmann::json::parse(saved_config, nullptr, false);
      if (!saved_cfg.is_discarded() && saved_cfg.contains("_parser_config")) {
        initial_parser_config = saved_cfg["_parser_config"].get<std::string>();
      }
    }

    // A named local, not a co_await temporary: GCC 11.4 (linux-ci, AppImage) mishandles awaiter temporaries (the
    // DialogShutdownTest pair segfaulted on CI; newer GCC and MSVC were fine). Declared after `handle`, so frame
    // destruction rejects the dialog before the plugin context dies.
    DataSourceDialogAwaiter dialog_awaiter({
        .source = *source,
        .handle = handle,
        .catalog = extensions_,
        .parent = dialog_parent,
        .initial_parser_config = initial_parser_config,
        .browser_file_store = browser_file_store_,
        .title_prefix = hints.dialog_title_prefix,
        .on_dialog = hints.on_dialog,
        .on_browser_file_staged =
            [self = QPointer<FileLoader>(this)](const LoadInput& picked) {
              if (self) {
                self->rememberBrowserUpload(picked);
              }
            },
    });
    auto dlg = co_await std::move(dialog_awaiter);
    if (dlg.outcome == dialog_presenter::Outcome::kPluginContractViolation) {
      (void)fail(tr("Plugin contract violation: %1. Reinstall the plugin from the Marketplace.")
                     .arg(QString::fromStdString(dlg.error)));
      co_return;
    }
    if (dlg.outcome == dialog_presenter::Outcome::kRejected) {
      if (hints.batch != nullptr) {
        hints.batch->cancelled.insert(source_manifest_id);
      }
      // Test automation greps this marker on every platform.
      recordProbeOutcome(u"PJ_FILE_LOAD_REJECTED plugin="_s + source_name + u" identity="_s + source_identity);
      // A cancel terminal; fileLoaded/fileLoadFailed stay silent. Roll back first so the terminal sees it.
      erase_created_live_dataset();
      rollback_tombstones();
      emitLoadFinished(ticket, LoadOutcome::kCancelled, source_identity, 0);
      co_return;
    }
    if (disallowed()) {
      co_return;
    }
    if (dlg.payload.has_value()) {
      config = dlg.payload->saved_config;
      dialog_selected_files = std::move(dlg.payload->selected_files);
      // An embedded parser's config travels inside the source config under "_parser_config".
      if (!dlg.payload->parser_config.empty()) {
        auto cfg = nlohmann::json::parse(config, nullptr, false);
        if (!cfg.is_discarded()) {
          cfg["_parser_config"] = dlg.payload->parser_config;
          config = cfg.dump();
        }
      }
      // The dialog vtable already applied the choices; this reaches plugins that keep dialog and source state apart.
      if (auto status = handle.loadConfig(config); !status) {
        (void)fail(tr("Plugin '%1': loadConfig (post-dialog) failed: %2")
                       .arg(source_name, QString::fromStdString(status.error())));
        co_return;
      }
    }
  }

  // A cancel issued while the config dialog was open had no worker to observe it; the store(0) below would wipe it.
  if (cancel_mode_.load() != 0) {
    qCInfo(lcFileLoader) << "[FileLoader] load cancelled while suspended at its config dialog";
    cancel_mode_.store(0);
    erase_created_live_dataset();
    rollback_tombstones();
    emitLoadFinished(ticket, LoadOutcome::kCancelled, source_identity, 0);
    emit fileLoadFailed(source_identity, tr("Load cancelled"));
    co_return;
  }

  // Before start(), so dialog choices stick even if ingest fails. A preset load skips it: opening a layout must not
  // overwrite the user's last interactive choice.
  if (!skip_dialog) {
    persisted_settings.setValue(config_key, QString::fromStdString(config));
    // Only now are there settings this batch accepted: a dialog that ended any other way leaves "show first" asking.
    if (hints.batch != nullptr) {
      hints.batch->prompted.insert(source_manifest_id);
    }
    // The config may still name earlier picks (a DBC for another channel): add, never replace.
    // ponytail: every pick stays staged for the session; prune by what the
    // config still references if large dialog-picked files become common.
    auto& kept = last_dialog_files_[source_name];
    kept.insert(kept.end(), dialog_selected_files.begin(), dialog_selected_files.end());
  }

  // One entry per dataset; `{ config }` when the plugin emitted no `__pj_fanout`.
  const auto fanouts = detail::extractFanout(config);

  // One pinned target cannot absorb N results. Checked before the fan-out arm tombstones the target.
  if (strict_replace && fanouts.size() != 1) {
    (void)fail(tr("Cannot replace dataset %1: the selected configuration expands into %2 datasets.")
                   .arg(hints.replace_dataset_id)
                   .arg(fanouts.size()));
    co_return;
  }

  // Fan-out datasets that kept data (completed or keep-stopped). The prologue's own dataset is never one of them.
  std::vector<DatasetId> fanout_loaded_ids;
  std::vector<DatasetId> fanout_created_ids;
  // Captured before each entry's host dies; published with the source paths.
  std::unordered_map<DatasetId, QString> fanout_loader_metadata;
  bool fanout_committed = false;
  const auto rollback_fanout_on_cancel = qScopeGuard([&]() {
    if (fanout_committed) {
      return;
    }
    for (const DatasetId created_id : fanout_created_ids) {
      removeCreatedDataset(created_id);
    }
  });

  // Fan-out only, read by the shared epilogue.
  QStringList failed_labels;
  // The whole-load stop that ended the fan-out early, in cancel_mode_'s encoding.
  int whole_load_stop = 0;

  if (fanouts.size() == 1) {
    // Single instance: the read loop runs on a worker, filling the datastore progressively.
    Q_ASSERT(!strict_replace || dataset_id == hints.replace_dataset_id);

    // Named before start(), so the catalog rebuilds under the right root. A replacing load renames at commit, so a
    // rollback does not leave the restored data under the new source's name.
    const QString plugin_name = detail::parseDisplayName(config);
    if (!replacing && !plugin_name.isEmpty()) {
      catalog_.setDatasetDisplayName(dataset_id, plugin_name);
    }

    ctx_ = std::make_unique<LoadContext>(std::move(bound));
    ctx_->dialog_selected_files = std::move(dialog_selected_files);
    ctx_->path = source_identity;
    ctx_->dialog_parent = request.dialog_parent;
    ctx_->source_name = source_name;
    ctx_->source_manifest_id = source_manifest_id;
    ctx_->config = config;
    ctx_->dataset_id = dataset_id;
    ctx_->ticket = ticket;
    ctx_->replacing = replacing;
    if (replacing) {
      ctx_->commit_display_name = !plugin_name.isEmpty() ? plugin_name : display_name;
    }
    ctx_->file_index = request.file_index;
    ctx_->file_total = request.file_total;

    if (replacing) {
#ifdef PJ_WITH_SCENE3D
      if (transform_service_ != nullptr) {
        transform_service_->beginReplacingLoad(existing_primary_id);
      }
#endif
      // Detaches the prior data (topics stay registered, so the refill reuses their ids). The last prologue step,
      // so a prologue failure never touches live data; a worker failure, discard or shutdown rolls back through it.
      ctx_->refill_guard = session_.beginRefill(existing_primary_id);
    }

    // on_progress_* run on the worker: they touch only ctx_ (which the GUI leaves alone until the join) and
    // cancel_mode_, and queue every GUI access.
    const std::uint64_t generation = load_generation_;
    DataSourceRuntimeHost& host = *ctx_->bound.ingest;
    host.on_progress_start = [this, generation](std::string_view label, uint64_t total, bool cancellable) {
      ctx_->progress_total = total;
      const QString title = QString::fromUtf8(label.data(), static_cast<int>(label.size()));
      const bool determinate = total > 0;
      const DatasetId ingest_dataset = ctx_->dataset_id;
      const int file_index = ctx_->file_index;
      const int file_total = ctx_->file_total;
      QMetaObject::invokeMethod(
          this,
          [this, generation, title, total, cancellable, determinate, ingest_dataset, file_index, file_total]() {
            if (generation != load_generation_ || ctx_ == nullptr || ctx_->dataset_id != ingest_dataset) {
              return;
            }
            auto cancel_handler = [this, generation](bool keep_partial) {
              this->cancelCurrent(generation, keep_partial);
            };
            const IngestToken token =
                session_.beginIngest(ingest_dataset, title, total, cancellable, std::move(cancel_handler));
            qCInfo(lcFileLoader) << "[ingest] begin" << title << "total=" << total
                                 << (total == 0 ? "(INDETERMINATE - marquee, no percentage)" : "(determinate)")
                                 << "cancellable=" << cancellable;
            ctx_->ingest_token = token;
            emit ingestStarted(title, file_index, file_total, determinate);
          },
          Qt::QueuedConnection);
    };
    host.on_progress_update = [this, generation](uint64_t current) -> bool {
      if (cancel_mode_.load() != 0) {
        return false;
      }
      if (ctx_->flush_clock.elapsed() < flush_throttle_ms_) {
        return true;
      }
      ctx_->bound.ingest->flushPending();
      ctx_->flush_clock.restart();
      const DatasetId notify_dataset = ctx_->dataset_id;
      const uint64_t max = ctx_->progress_total;
      QMetaObject::invokeMethod(
          this,
          [this, generation, notify_dataset, current, max]() {
            // A tick can survive shutdown into a reused loader.
            if (generation != load_generation_ || ctx_ == nullptr || ctx_->dataset_id != notify_dataset) {
              return;
            }
            publishIngestProgress(notify_dataset, ctx_->ingest_token, current, max);
          },
          Qt::QueuedConnection);
      return true;
    };
    host.on_progress_finish = [] {};  // onWorkerFinished does the terminal flush

    cancel_mode_.store(0);
    ctx_->flush_clock.start();
    worker_ = std::unique_ptr<QThread>(QThread::create([this, generation]() { runIngestOnWorker(generation); }));
    worker_->start();
    rollback_armed = false;  // onWorkerFinished owns rollback now
    fanout_committed = true;
    worker_handoff = true;
    co_return;
  } else {
    // A same-source reload cannot refill one dataset into N: tombstone it now (erased past the rollback point
    // below). The handle bound to it is never started, so it takes no data.
    if (replacing) {
      emit sourceReplacementAboutToCommit(ticket, source_identity, existing_primary_id);
    }
    if (replacing && catalog_.removeDataset(existing_primary_id)) {
      tombstoned_for_replace.push_back(existing_primary_id);
    }
    // Each entry gets its own handle, dataset and host; a failed entry does not stop the others. Whether the loop
    // continues after a stop depends on its scope (whole_load_stop), not on the entry outcome.
    enum class EntryOutcome { kCompleted, kFailed, kKept, kDiscarded };

    const QString basename = QFileInfo(display_name).completeBaseName();
    // The plugin's `display_name` replaces the basename as the shared prefix; `display_suffix` names the leaf.
    const QString fanout_name = detail::parseDisplayName(config);
    const QString base = fanout_name.isEmpty() ? basename : fanout_name;

    // Only start() runs on a worker; the frame keeps handle and host alive until the join and destroys them here, on
    // the GUI thread.
    cancel_mode_.store(0);
    const std::uint64_t fanout_generation = load_generation_;
    // One failed fan-out entry: logged, and reported once under "<identity> (<entry>)".
    const auto report_entry_failed = [&](std::size_t idx, const QString& iter_display, const QString& reason) {
      logLoadFailure(
          u"%1 (%2)"_s.arg(shownSourceName(source_identity), iter_display), reason,
          u"[FileLoader] fanout[%1]: %2"_s.arg(idx).arg(reason));
    };
    // A failure before start(): the entry also joins failed_labels here. The start
    // failure joins it later, through outcome == kFailed.
    const auto entry_failed = [&](std::size_t idx, const QString& iter_display, const QString& reason) {
      report_entry_failed(idx, iter_display, reason);
      failed_labels << iter_display;
    };
    for (std::size_t idx = 0; idx < fanouts.size(); ++idx) {
      const std::string& cfg_i = fanouts[idx];
      const QString suffix = detail::parseDisplaySuffix(cfg_i, QString::number(idx + 1));
      const QString iter_display = base + QChar('/') + suffix;

      auto iter_td = engine.createTimeDomain(iter_display.toStdString());
      if (!iter_td.has_value()) {
        entry_failed(
            idx, iter_display,
            tr("Could not create the time domain for %1: %2")
                .arg(iter_display, QString::fromStdString(iter_td.error())));
        continue;
      }
      auto iter_dataset_or = engine.createDataset(
          DatasetDescriptor{.source_name = iter_display.toStdString(), .time_domain_id = *iter_td});
      if (!iter_dataset_or.has_value()) {
        entry_failed(
            idx, iter_display, tr("createDataset failed: %1").arg(QString::fromStdString(iter_dataset_or.error())));
        continue;
      }
      const auto iter_dataset_id = static_cast<DatasetId>(*iter_dataset_or);
      fanout_created_ids.push_back(iter_dataset_id);
      const PJ_data_source_handle_t iter_source_handle{static_cast<uint32_t>(iter_dataset_id)};

      // From the snapshot, never `source`: see the catalog note above.
      BoundSource iter_bound(DataSourceHandle(source_vtable, source_library_owner));
      DataSourceHandle& iter_handle = iter_bound.handle;
      if (!iter_handle.valid()) {
        entry_failed(idx, iter_display, tr("Plugin '%1': createHandle failed.").arg(source_name));
        continue;
      }

      iter_bound.ingest = std::make_unique<DataSourceRuntimeHost>(
          engine, extensions_, iter_dataset_id, iter_source_handle, session_.objectStore(), source_id,
          [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
            session_.registerObjectTopicParser(id, std::move(parser));
          },
          nullptr, nullptr, combineKeepalive(iter_handle.libraryOwner(), input.lease), session_.ingestTapsShared());
      DataSourceRuntimeHost& iter_ingest = *iter_bound.ingest;
      iter_ingest.setDiagnosticSink(diagnostic_sink_, source_plugin_name, iter_display.toStdString());
      if (message_box_handler) {
        iter_ingest.setMessageBoxHandler(message_box_handler);
      }
      applyDefaultIngestPolicies(iter_ingest.policyResolver());

      if (auto status = iter_bound.bind(); !status) {
        entry_failed(idx, iter_display, tr("Plugin '%1': %2").arg(source_name, QString::fromStdString(status.error())));
        continue;
      }
      if (auto status = iter_handle.loadConfig(cfg_i); !status) {
        entry_failed(
            idx, iter_display,
            tr("Plugin '%1': loadConfig failed: %2").arg(source_name, QString::fromStdString(status.error())));
        continue;
      }

      uint64_t progress_total = 0;
      QElapsedTimer flush_clock;
      flush_clock.start();
      // This entry's row stop, in cancel_mode_'s encoding. One slot per entry, so a click landing after the entry
      // retired writes into a slot nobody reads. Shared: the row's cancel handler outlives the iteration.
      const auto entry_stop = std::make_shared<std::atomic<int>>(0);
      iter_ingest.on_progress_start = [this, fanout_generation, &progress_total, iter_dataset_id, idx, entry_stop,
                                       fanout_count = fanouts.size()](
                                          std::string_view label, uint64_t total, bool cancellable) {
        progress_total = total;
        const QString title = QString::fromUtf8(label.data(), static_cast<int>(label.size()));
        QMetaObject::invokeMethod(
            this,
            [this, fanout_generation, iter_dataset_id, title, total, cancellable, idx, fanout_count, entry_stop,
             determinate = total > 0]() {
              if (fanout_generation != load_generation_) {
                return;
              }
              // A row stops its own entry only; the strip and shutdown latch cancel_mode_ for the whole load.
              auto cancel_handler = [this, fanout_generation, iter_dataset_id, entry_stop](bool keep_partial) {
                if (fanout_generation != load_generation_) {
                  return;
                }
                qCInfo(lcFileLoader) << "[FileLoader] fanout: row stop on dataset" << iter_dataset_id << "keep_partial"
                                     << keep_partial;
                entry_stop->store(keep_partial ? 1 : 2);
              };
              const IngestToken token =
                  session_.beginIngest(iter_dataset_id, title, total, cancellable, std::move(cancel_handler));
              fanout_ingest_tokens_[iter_dataset_id] = token;
              emit ingestStarted(title, static_cast<int>(idx + 1), static_cast<int>(fanout_count), determinate);
            },
            Qt::QueuedConnection);
      };
      iter_ingest.on_progress_update = [this, fanout_generation, &iter_ingest, &flush_clock, &progress_total,
                                        iter_dataset_id, entry_stop](uint64_t current) -> bool {
        if (cancel_mode_.load() != 0 || entry_stop->load() != 0) {
          iter_ingest.requestStop();
          return false;
        }
        if (flush_clock.elapsed() < flush_throttle_ms_) {
          return true;
        }
        iter_ingest.flushPending();
        flush_clock.restart();
        const uint64_t max = progress_total;
        QMetaObject::invokeMethod(
            this,
            [this, fanout_generation, iter_dataset_id, current, max]() {
              if (fanout_generation != load_generation_) {
                return;
              }
              // No token when the entry never announced a start; its rows still publish.
              const auto token_it = fanout_ingest_tokens_.find(iter_dataset_id);
              const IngestToken token = token_it != fanout_ingest_tokens_.end() ? token_it->second : IngestToken{};
              publishIngestProgress(iter_dataset_id, token, current, max);
            },
            Qt::QueuedConnection);
        return true;
      };
      iter_ingest.on_progress_finish = [] {};

      // start() runs off the GUI thread although the SDK tags it [main-thread]: a finite importer blocks in it for
      // the whole read loop, which would freeze the UI and deadlock the message-box marshal. Every other lifecycle
      // slot stays on the GUI thread (as in runIngestOnWorker).
      Status start_status = okStatus();
      iter_ingest.forgetReportedProblem();
      active_fanout_ingest_ = &iter_ingest;
      {
        // Also clears it when a shutdown destroys the frame while suspended below.
        const ScopedPointerReset<DataSourceRuntimeHost> clear_active_ingest(active_fanout_ingest_);
        // A named local: see dialog_awaiter.
        GuiResumingWorkerAwaiter start_awaiter(
            this, worker_, [&iter_handle, &start_status]() { start_status = iter_handle.start(); });
        co_await std::move(start_awaiter);
      }

      // A stop wins over the plugin's status: importers commonly report a refused progress update as a start
      // failure. A whole-load stop ends the fan-out; a row stop is consumed here and the loop goes on.
      const int whole_load_action = cancel_mode_.load();
      const int entry_action = entry_stop->exchange(0);
      const int cancel_action = whole_load_action != 0 ? whole_load_action : entry_action;
      const bool stops_whole_load = whole_load_action != 0;
      EntryOutcome outcome = EntryOutcome::kCompleted;
      const auto capture_loader_metadata = [&]() {
        if (auto staged = iter_ingest.stagedLoaderMetadata(); staged.has_value()) {
          fanout_loader_metadata.emplace(iter_dataset_id, QString::fromStdString(*staged));
        }
      };
      if (cancel_action == 2) {
        outcome = EntryOutcome::kDiscarded;
      } else if (cancel_action == 1) {
        iter_ingest.flushAll();
        if (session_.datasetHoldsData(iter_dataset_id)) {
          fanout_loaded_ids.push_back(iter_dataset_id);
          capture_loader_metadata();
          outcome = EntryOutcome::kKept;
        } else {
          outcome = EntryOutcome::kDiscarded;  // the stop kept nothing
        }
      } else if (!start_status) {
        report_entry_failed(
            idx, iter_display,
            tr("Plugin '%1': start failed: %2").arg(source_name, QString::fromStdString(start_status.error())));
        outcome = EntryOutcome::kFailed;
      } else {
        iter_ingest.flushAll();
        if (session_.datasetHoldsData(iter_dataset_id)) {
          fanout_loaded_ids.push_back(iter_dataset_id);
          capture_loader_metadata();
        } else {
          report_entry_failed(idx, iter_display, noDataReason(source_name, iter_ingest));
          outcome = EntryOutcome::kFailed;
        }
      }

      const IngestOutcome ingest_outcome =
          outcome == EntryOutcome::kCompleted
              ? IngestOutcome::kCompleted
              : (outcome == EntryOutcome::kFailed ? IngestOutcome::kFailed : IngestOutcome::kCancelled);
      if (const auto token_it = fanout_ingest_tokens_.find(iter_dataset_id); token_it != fanout_ingest_tokens_.end()) {
        const IngestToken token = token_it->second;
        // Erase first: an ingestEnded observer re-entering shutdown would end it a second time.
        fanout_ingest_tokens_.erase(token_it);
        postIngestTerminal(token, ingest_outcome);
      }

      if (outcome == EntryOutcome::kFailed) {
        failed_labels << iter_display;
      }
      if (cancel_action != 0) {
        qCInfo(lcFileLoader) << "[FileLoader] fanout: stop with cancellation mode" << cancel_action << "at entry"
                             << (idx + 1) << "of" << fanouts.size()
                             << (stops_whole_load ? "(whole load)" : "(this row only)");
      }
      if (stops_whole_load) {
        whole_load_stop = whole_load_action;
        break;
      }
    }
    qCInfo(lcFileLoader) << "[FileLoader] fanout complete:" << fanout_loaded_ids.size() << "ok," << failed_labels.size()
                         << "failed" << failed_labels;
    cancel_mode_.store(0);

    // A whole-load discard drops the entries that already completed too.
    if (whole_load_stop == 2) {
      fanout_loaded_ids.clear();
    }

    // The worker has stopped: erase every created dataset that kept nothing.
    for (const DatasetId created_id : fanout_created_ids) {
      if (std::find(fanout_loaded_ids.begin(), fanout_loaded_ids.end(), created_id) != fanout_loaded_ids.end()) {
        continue;
      }
      removeCreatedDataset(created_id);
    }
    if (!replacing && created_live_dataset_id != 0) {
      // The prologue's dataset, bound before the dialog revealed the fan-out; it never took rows.
      removeCreatedDataset(created_live_dataset_id, /*evict_objects=*/false);
      created_live_dataset_id = 0;
    }
  }

  // Nothing kept (whole-load discard, every entry failed or discarded): the tombstones stay for the rollback exits
  // below, which restore the dataset being replaced. extractFanout never yields an empty list.
  const bool nothing_to_commit = fanout_loaded_ids.empty();
  if (!nothing_to_commit) {
    // Past the last rollback point: erase the tombstoned dataset, or a prefer_reuse replay could reattach to it. Its
    // processors stay: the shell's workspace restore rebinds them onto the children.
    for (const DatasetId tombstoned_id : tombstoned_for_replace) {
      removeCreatedDataset(
          tombstoned_id, /*evict_objects=*/true, /*remove_from_catalog=*/false, /*remove_dependent_processors=*/false);
    }
    tombstoned_for_replace.clear();
    rollback_armed = false;
    fanout_committed = true;
  }

  catalog_.rebuildFromDatastore();

#ifdef PJ_WITH_SCENE3D
  if (transform_service_ != nullptr) {
    for (const DatasetId loaded_id : fanout_loaded_ids) {
      transform_service_->publishTransforms(loaded_id);
    }
  }
#endif

  // After start(), so the layout persists what the import discovered.
  std::string captured_config = captureSavedConfig(handle, source_name);

  // A rejected metadata document is logged and never affects the load.
  const auto publish_loader_metadata =
      [this, &source_manifest_id](DatasetId loaded_id, const QString& json) {
        if (auto status = session_.setDatasetMetadata(loaded_id, json, source_manifest_id); !status) {
          qCWarning(lcFileLoader).noquote()
              << tr("Loader metadata rejected: %1").arg(QString::fromStdString(status.error()));
        }
      };
  if (fanouts.size() == 1) {
    session_.setDatasetSourcePath(dataset_id, source_identity);
    if (ctx_ != nullptr && ctx_->bound.ingest != nullptr) {
      if (auto staged = ctx_->bound.ingest->stagedLoaderMetadata(); staged.has_value()) {
        publish_loader_metadata(dataset_id, QString::fromStdString(*staged));
      }
    }
  } else {
    for (const DatasetId loaded_id : fanout_loaded_ids) {
      session_.setDatasetSourcePath(loaded_id, source_identity);
      if (const auto staged_it = fanout_loader_metadata.find(loaded_id); staged_it != fanout_loader_metadata.end()) {
        publish_loader_metadata(loaded_id, staged_it->second);
      }
    }
  }

  // The plugin write host bypasses SessionManager::commitChunks, so the time origin was never re-evaluated: data
  // earlier than every prior dataset must reframe every plot.
  if (fanouts.size() == 1) {
    session_.refreshDatasetTimeReference(dataset_id);
  } else {
    for (const DatasetId loaded_id : fanout_loaded_ids) {
      session_.refreshDatasetTimeReference(loaded_id);
    }
  }

  if (nothing_to_commit) {
    rollback_tombstones();
    if (whole_load_stop != 0 || failed_labels.isEmpty()) {
      // A user stop, or every entry discarded: report a cancel, as the single-instance path does, never an
      // all-entries-failed load. Recording it would resurrect a source the user removed.
      emitLoadFinished(ticket, LoadOutcome::kCancelled, source_identity, 0);
      emit fileLoadFailed(source_identity, tr("Import discarded"));
      co_return;
    }
    // Each failed entry already reported its own data-source-load-failed.
    const QString reason = tr("All %1 entries of '%2' failed to load:\n%3")
                               .arg(failed_labels.size())
                               .arg(display_name, failed_labels.join(u"\n"_s));
    emitLoadFinished(ticket, LoadOutcome::kFailed, source_identity, 0, {}, reason);
    reportLoadWarning(dialog_parent, reason);
    emit fileLoadFailed(source_identity, reason);
    co_return;
  }

  // A keep-stop still emits fileLoaded, but the ticket reports kCancelled.
  const DatasetId fanout_primary_id = fanout_loaded_ids.empty() ? 0 : fanout_loaded_ids.front();
  emitLoadFinished(
      ticket, whole_load_stop != 0 ? LoadOutcome::kCancelled : LoadOutcome::kLoaded, source_identity, fanout_primary_id,
      QVector<DatasetId>(fanout_loaded_ids.begin(), fanout_loaded_ids.end()));
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  logSuccessfulLoad(engine, catalog_, source_identity, source_name, fanout_loaded_ids);
#endif
  emit fileLoaded(source_identity, QString(), source_name, QString::fromStdString(captured_config), source_manifest_id);
  co_return;
}

bool FileLoader::loadFile(const QString& path, QWidget* dialog_parent, const LoadHints& hints) {
  return loadFile(LoadInput::fromNativePath(path), dialog_parent, hints);
}

bool FileLoader::loadFile(LoadInput input, QWidget* dialog_parent, const LoadHints& hints) {
  return loadFileTicketed(std::move(input), dialog_parent, hints) != 0;
}

LoadRequestId FileLoader::loadFileTicketed(LoadInput input, QWidget* dialog_parent, const LoadHints& hints) {
  if (destroying_) {
    // See destroying_. No signals or dialogs from a dying loader.
    qCWarning(lcFileLoader) << "[FileLoader] load request rejected: the loader is being destroyed";
    return 0;
  }
  if (input.display_name.isEmpty() || input.backing_path.isEmpty() || input.source_identity.isEmpty()) {
    // E.g. a trailing-slash directory path. Reported like any load error (MainWindow's layout replay relies on
    // fileLoadFailed), but never accepted: no ticket, no loadFinished.
    const QString path = !input.source_identity.isEmpty() ? input.source_identity
                         : !input.backing_path.isEmpty()  ? input.backing_path
                                                          : input.display_name;
    const QString reason = tr("Cannot load '%1': not a readable file.").arg(path);
    logLoadFailure(path, tr("not a readable file."));
    reportLoadWarning(dialog_parent, reason);
    emit fileLoadFailed(path, reason);
    return 0;
  }
  rememberBrowserUpload(input);
#ifdef PJ_TARGET_WASM
  if (isBrowserUploadIdentity(input.source_identity)) {
    // Only a lazily read pick arrives unfingerprinted (a copied or downloaded file is hashed as it is written); an
    // empty result means its File changed since the pick, which its reads report.
    if (input.content_sha256.isEmpty()) {
      browser_picked_file::hash(
          input.backing_path, [self = QPointer<FileLoader>(this), path = input.backing_path,
                               identity = input.source_identity](QString sha256) {
            if (self && !sha256.isEmpty() && self->browser_identity_by_path_.contains(path)) {
              self->browser_content_sha256_.insert(identity, std::move(sha256));
            }
          });
    }
  }
#endif
  ++next_load_ticket_;
  if (next_load_ticket_ == 0) {
    ++next_load_ticket_;  // reserve zero for "invalid", even after wraparound
  }
  const quint64 ticket = next_load_ticket_;
  queue_.push_back(
      LoadRequest{.input = std::move(input), .dialog_parent = dialog_parent, .hints = hints, .ticket = ticket});
  startNext();
  return ticket;
}

void FileLoader::startNext() {
  if (active_load_) {
    return;  // a suspended prologue or worker resumes the queue when done
  }
  // Any prior task is at final_suspend. finishPrologue queues this call, so we are not inside that frame.
  begin_load_task_.reset();
  // A cancel belongs to its own load: exits that never read it (a rejected dialog, a later failure) leave it
  // latched, and the next prologue would take it as its own.
  cancel_mode_.store(0);
  if (queue_.empty()) {
    load_generation_ = 0;
    current_load_ticket_ = 0;
    current_load_identity_.clear();
    current_ticket_terminal_ = false;
    emit queueDrained();
    return;
  }
  LoadRequest request = std::move(queue_.front());
  queue_.pop_front();
  active_load_ = true;
  ++next_load_generation_;
  if (next_load_generation_ == 0) {
    ++next_load_generation_;  // reserve zero for idle, even after wraparound
  }
  load_generation_ = next_load_generation_;
  // Together, before loadGenerationAdvanced, so its slots read a consistent pair.
  current_load_ticket_ = request.ticket;
  current_load_identity_ = request.input.source_identity;
  current_ticket_terminal_ = false;
  emit requestStarted(current_load_ticket_, load_generation_);
  // Before the coroutine: stale UI must be gone before this load raises its application-modal dialogs.
  emit loadGenerationAdvanced(load_generation_);
  begin_load_task_ = std::make_unique<BeginLoadTask>(beginLoad(std::move(request)));
}

void FileLoader::finishPrologue() {
  if (shutting_down_) {
    return;
  }
  active_load_ = false;
  QMetaObject::invokeMethod(this, [this]() { startNext(); }, Qt::QueuedConnection);
}

void FileLoader::runIngestOnWorker(std::uint64_t generation) {
  // Worker thread. The GUI leaves ctx_ alone until onWorkerFinished.
  ctx_->bound.ingest->forgetReportedProblem();  // a loadConfig or dialog warning is not why the import found nothing
  auto status = ctx_->bound.handle.start();
  ctx_->start_ok = static_cast<bool>(status);
  if (!status) {
    ctx_->start_error = QString::fromStdString(status.error());
  }
  // ThreadSanitizer cannot see QThread::wait() (a futex inside prebuilt Qt). Released after the last worker access,
  // acquired after every wait() (onWorkerFinished, the shutdown join), so TSan sees the join's ordering.
  tsan::release(this);
  QMetaObject::invokeMethod(this, [this, generation]() { onWorkerFinished(generation); }, Qt::QueuedConnection);
}

void FileLoader::onWorkerFinished(std::uint64_t generation) {
  if (generation != load_generation_ || !ctx_) {
    return;  // shutdown or reuse already replaced this load
  }
  if (worker_) {
    worker_->wait();
    tsan::acquire(this);
    worker_.reset();
  }

  const int cancel = cancel_mode_.load();
  const DatasetId dataset_id = ctx_->dataset_id;
  const bool replacing = ctx_->replacing;
  const QString path = ctx_->path;
  const QString source_name = ctx_->source_name;
  const quint64 ticket = ctx_->ticket;
  const IngestToken ingest_token = ctx_->ingest_token;
  // Taken out of the context before any signal can re-enter joinForShutdown, which ends only a token still stored.
  ctx_->ingest_token = {};
  IngestOutcome ingest_outcome = IngestOutcome::kCompleted;
  // A user stop (either mode) reports kCancelled, even where fileLoaded or fileLoadFailed follows. Each arm
  // terminalizes the ticket before its fileLoaded/fileLoadFailed, so a re-entrant shutdown cannot resolve it again.

  // Rows are invisible until the terminal flush, so whether the import produced anything is known only after it. A
  // completed import that produced nothing fails; a stop-and-keep that kept nothing is a discard. Either way no empty
  // dataset stays behind holding the file's label.
  const bool ingest_ran = cancel != 2 && (ctx_->start_ok || cancel != 0);
  if (ingest_ran) {
    ctx_->bound.ingest->flushAll();
  }
  const bool yielded_nothing = ingest_ran && !session_.datasetHoldsData(dataset_id);

  if (cancel == 2 || (yielded_nothing && cancel == 1)) {  // Discard
    ingest_outcome = IngestOutcome::kCancelled;
    emitLoadFinished(ticket, LoadOutcome::kCancelled, path, 0);
    // As if this load never ran: nothing on a first load, the prior data on a reload.
    if (!replacing) {
      qCWarning(lcFileLoader) << "[FileLoader] import discarded by user; the new dataset is removed";
      // Erased, not tombstoned: a prefer_reuse replay must not reattach to an empty dataset.
      removeCreatedDataset(dataset_id);
      ctx_.reset();
      emit fileLoadFailed(path, tr("Import discarded"));
    } else {
      qCWarning(lcFileLoader) << "[FileLoader] import discarded by user; the dataset keeps its pre-reload data";
      failReplacingLoad(dataset_id, path, tr("Import discarded"));
    }
  } else if (yielded_nothing || (!ctx_->start_ok && cancel == 0)) {
    ingest_outcome = IngestOutcome::kFailed;
    const QString reason = yielded_nothing ? noDataReason(source_name, *ctx_->bound.ingest)
                                           : tr("Plugin '%1': start failed: %2").arg(source_name, ctx_->start_error);
    emitLoadFinished(ticket, LoadOutcome::kFailed, path, 0, {}, reason);
    logLoadFailure(path, reason);
    if (!replacing) {
      removeCreatedDataset(dataset_id);
      ctx_.reset();
      emit fileLoadFailed(path, reason);
    } else {
      failReplacingLoad(dataset_id, path, reason);
    }
  } else {  // completed, or keep-stopped
    if (cancel == 1) {
      qCInfo(lcFileLoader) << "[FileLoader] import cancelled by user; keeping the partial load";
    }
    bool refill_ok = true;
    if (ctx_->refill_guard) {
      // Derived outputs were detached with the dataset: replay them while a failure can still roll back.
      if (const Status replayed = ctx_->refill_guard->recomputeProcessors(); !replayed.has_value()) {
        ingest_outcome = cancel == 0 ? IngestOutcome::kFailed : IngestOutcome::kCancelled;
        const QString reason = tr("Plugin '%1': derived-series replay failed: %2")
                                   .arg(source_name, QString::fromStdString(replayed.error()));
        emitLoadFinished(
            ticket, cancel == 0 ? LoadOutcome::kFailed : LoadOutcome::kCancelled, path, 0, {},
            cancel == 0 ? reason : QString());
        if (cancel == 0) {
          logLoadFailure(path, reason);
        } else {
          qCWarning(lcFileLoader).noquote() << reason;
        }
        failReplacingLoad(dataset_id, path, reason);
        refill_ok = false;
      } else {
        // Complete reloads only: a topic a partial load never reached has not vanished. Before commit(), which
        // frees the snapshots it reads.
        if (cancel == 0) {
          ctx_->refill_guard->pruneVanishedTopics();
        }
        // The write host notifies nothing, and the commit recomputes the playback range from the catalog: show the
        // topics the reload added first, or the range misses them and clamps the playhead.
        catalog_.rebuildFromDatastore();
        // Before finishLoadOnGui(), which resets ctx_: an uncommitted guard would roll back over the new data.
        ctx_->refill_guard->commit();
      }
    }
    if (refill_ok) {
      ingest_outcome = cancel == 1 ? IngestOutcome::kCancelled : IngestOutcome::kCompleted;
      emitLoadFinished(
          ticket, cancel == 1 ? LoadOutcome::kCancelled : LoadOutcome::kLoaded, path, dataset_id, {dataset_id});
      finishLoadOnGui(/*fully_loaded=*/cancel == 0);
    }
  }

  if (ingest_token.id != 0) {
    session_.endIngest(ingest_token, ingest_outcome);
  }
  cancel_mode_.store(0);
  active_load_ = false;
  startNext();
}

void FileLoader::publishIngestProgress(DatasetId dataset_id, IngestToken token, uint64_t current, uint64_t total) {
  if (token.id != 0) {
    // Publishes the committed rows before the progress number.
    session_.updateIngest(token, current, total);
  } else {
    // A producer that ticks without a start has no token; its rows still publish.
    const auto ids = session_.dataEngine().listTopics(dataset_id);
    session_.notifyIngest(QVector<TopicId>(ids.begin(), ids.end()), /*live=*/false);
  }
#ifdef PJ_WITH_SCENE3D
  if (transform_service_ != nullptr) {
    transform_service_->publishTransforms(dataset_id);
  }
#endif
  emit ingestProgress(static_cast<int>(current), static_cast<int>(total));
  emit loadProgress(current_load_ticket_, current, total);
}

void FileLoader::removeCreatedDataset(
    DatasetId dataset_id, bool evict_objects, bool remove_from_catalog, bool remove_dependent_processors) {
#ifdef PJ_WITH_SCENE3D
  if (transform_service_ != nullptr) {
    transform_service_->invalidateDataset(dataset_id);
  }
#endif
  if (evict_objects) {
    session_.evictDatasetObjects(dataset_id);
  }
  if (remove_from_catalog) {
    catalog_.removeDataset(dataset_id, /*tombstone=*/false);
  }
  if (session_.removeDataset(dataset_id, remove_dependent_processors)) {
    catalog_.rebuildIfChanged();
  }
}

void FileLoader::failReplacingLoad(DatasetId dataset_id, const QString& path, const QString& reason) {
  ctx_->refill_guard.reset();
  refreshAfterReplacingRollback(dataset_id);
  ctx_.reset();
  emit fileLoadFailed(path, reason);
}

void FileLoader::refreshAfterReplacingRollback(DatasetId dataset_id) {
  // The prior TF generation stayed active throughout: only the private one is dropped.
  catalog_.rebuildFromDatastore();
#ifndef PJ_WITH_SCENE3D
  Q_UNUSED(dataset_id)
#endif
#ifdef PJ_WITH_SCENE3D
  if (transform_service_ != nullptr) {
    transform_service_->abortReplacingLoad(dataset_id);
  }
#endif
}

void FileLoader::finishLoadOnGui(bool fully_loaded) {
  // Locals, not ctx_, past the loadCommitting emission: a slot may re-enter joinForShutdown, which resets ctx_.
  const DatasetId dataset_id = ctx_->dataset_id;
  const quint64 ticket = ctx_->ticket;
  const bool replacing = ctx_->replacing;

  if (replacing && !ctx_->commit_display_name.isEmpty()) {
    catalog_.setDatasetDisplayName(dataset_id, ctx_->commit_display_name);
  }

  std::string captured_config = captureSavedConfig(ctx_->bound.handle, ctx_->source_name);
  session_.setDatasetSourcePath(dataset_id, ctx_->path);
  // RefillGuard::commit cleared a replaced dataset's document: no staged document leaves it with none.
  if (ctx_->bound.ingest != nullptr) {
    if (auto staged = ctx_->bound.ingest->stagedLoaderMetadata(); staged.has_value()) {
      if (auto status =
              session_.setDatasetMetadata(dataset_id, QString::fromStdString(*staged), ctx_->source_manifest_id);
          !status) {
        qCWarning(lcFileLoader).noquote()
            << tr("Loader metadata rejected: %1").arg(QString::fromStdString(status.error()));
      }
    }
  }
  const QString path = ctx_->path;
  const QString source_name = ctx_->source_name;
  const QString source_manifest_id = ctx_->source_manifest_id;

  // RefillGuard::commit already detached a replaced dataset's provider record, so a loadCommitting slot can attach
  // the new one.
  if (fully_loaded) {
    emit loadCommitting(ticket, QVector<DatasetId>{dataset_id}, source_name, QString::fromStdString(captured_config));
  }
  catalog_.rebuildFromDatastore();

  // A replacement's TF generation is published atomically; a fresh load filled its active buffer on the worker.
#ifdef PJ_WITH_SCENE3D
  if (transform_service_ != nullptr) {
    if (replacing) {
      transform_service_->commitReplacingLoad(dataset_id);
    } else {
      transform_service_->publishTransforms(dataset_id);
    }
  }
#endif

  // The plugin write host bypasses SessionManager::commitChunks: re-evaluate the time origin, and publish the rows,
  // or a curve bound to a refilled dataset keeps the empty window it re-indexed when the refill began.
  session_.refreshDatasetTimeReference(dataset_id);
  const auto topic_ids = session_.dataEngine().listTopics(dataset_id);
  session_.notifyIngest(QVector<TopicId>(topic_ids.begin(), topic_ids.end()), /*live=*/false);
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  logSuccessfulLoad(session_.dataEngine(), catalog_, path, source_name, {dataset_id});
#endif
  ctx_.reset();
#if defined(PJ_WASM_ENABLE_INGRESS_PROBE) && (defined(PJ_WASM_WITH_ROS_PLUGIN) || defined(PJ_WASM_WITH_PROTOBUF_PLUGIN))
  // The scene consumers' cold-fetch + parser path, after the source and runtime host are gone.
  probeObjectDecoding(session_, session_.objectStore(), dataset_id);
#endif
#ifdef PJ_WASM_ENABLE_MCAP_PROBE_PARSER
  // After the source and runtime host are gone: proves the ObjectStore owns the file lease and the cold reader.
  probeColdObjectFetch(session_.objectStore(), dataset_id);
#endif
  emit fileLoaded(path, QString(), source_name, QString::fromStdString(captured_config), source_manifest_id);
}

void FileLoader::logLoadFailure(const QString& identity, const QString& reason, const QString& log_text) const {
  qCWarning(lcFileLoader).noquote() << (log_text.isEmpty() ? reason : log_text);
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  // So a browser test fails at once with the reason instead of timing out.
  recordProbeOutcome(u"PJ_FILE_LOAD_FAILED identity="_s + identity + u" reason="_s + reason);
#endif
  emitDiagnosticTo(
      diagnostic_sink_, DiagnosticLevel::kError, "File Loader", "data-source-load-failed",
      tr("Could not load '%1': %2").arg(shownSourceName(identity), reason).toStdString());
}

void FileLoader::reportLoadWarning(QWidget* parent, const QString& reason) {
  if (parent == nullptr) {
    // Not accumulated: only a dialog clears the list, so the next parented failure would show it.
    return;
  }
  load_warning_lines_ << reason;

  const auto refresh_body = [this](MessageBox* box) {
    const int count = static_cast<int>(load_warning_lines_.size());
    box->setTitle(count > 1 ? tr("%1 loads failed").arg(count) : tr("Load failed"));
    box->setText(load_warning_lines_.join(u"\n\n"_s));
  };

  if (!active_load_warning_.isNull()) {
    refresh_body(qobject_cast<MessageBox*>(active_load_warning_.data()));
    return;
  }

  auto* dialog = new MessageBox(parent);
  active_load_warning_ = dialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  // Not window-modal: see openScrollableMessageDialog.
  dialog->setWindowModality(Qt::ApplicationModal);
  refresh_body(dialog);
  dialog->addButton(tr("OK"), MessageBox::kPrimaryRole);
  // On finished, not destroyed: a failure arriving before the deferred delete would append to a closing dialog.
  QObject::connect(dialog, &QDialog::finished, this, [this]() {
    active_load_warning_.clear();
    load_warning_lines_.clear();
  });
  // show(), not open(): open() force-downgrades ApplicationModal to WindowModal.
  dialog->show();
}

void FileLoader::cancelCurrent(bool keep_partial) {
  if (!active_load_) {
    // Latching now would cancel the next load at its prologue check.
    return;
  }
  cancel_mode_.store(keep_partial ? 1 : 2);
  if (ctx_ == nullptr) {
    // Fan-out: the entry's progress callback reads cancel_mode_; requestStop also wakes a progress-silent plugin.
    if (active_fanout_ingest_ != nullptr) {
      active_fanout_ingest_->requestStop();
    }
    return;
  }
  if (ctx_->bound.ingest) {
    // Flag only: writing a reason would race the worker's last_error_.
    ctx_->bound.ingest->requestStop();
  }
}

void FileLoader::cancelCurrent(std::uint64_t generation, bool keep_partial) {
  if (generation == 0 || generation != load_generation_ || !active_load_) {
    // The load already finished. !active_load_ covers the hop before startNext advances the generation: a click
    // there would latch a cancel against the next load.
    return;
  }
  cancelCurrent(keep_partial);
}

bool FileLoader::cancelLoad(quint64 ticket, bool keep_partial) {
  if (QThread::currentThread() != thread()) {
    cancelLoadAsync(ticket, keep_partial);
    return false;
  }
  if (ticket == 0) {
    return false;
  }
  // Still queued: nothing else will ever resolve this ticket.
  for (auto it = queue_.begin(); it != queue_.end(); ++it) {
    if (it->ticket == ticket) {
      const QString identity = it->input.source_identity;
      queue_.erase(it);
      emitLoadFinished(ticket, LoadOutcome::kCancelled, identity, 0);
      return true;
    }
  }
  // current_ticket_terminal_: the terminal is scheduled but startNext has not run; a cancel there would latch
  // against the next load.
  if (ticket != current_load_ticket_ || !active_load_ || current_ticket_terminal_) {
    return false;
  }
  cancelCurrent(load_generation_, keep_partial);
  return true;
}

void FileLoader::cancelLoadAsync(quint64 ticket, bool keep_partial) {
  QMetaObject::invokeMethod(
      this, [this, ticket, keep_partial]() { (void)cancelLoad(ticket, keep_partial); }, Qt::QueuedConnection);
}

void FileLoader::emitLoadFinished(
    quint64 ticket, LoadOutcome outcome, const QString& effective_path, DatasetId dataset_id,
    QVector<DatasetId> produced_dataset_ids, QString reason) {
  if (ticket == current_load_ticket_) {
    if (current_ticket_terminal_) {
      // First wins (e.g. a joinForShutdown re-entered from a fileLoaded slot). A different outcome means an exit
      // path emitted fileLoaded/fileLoadFailed before terminalizing.
      if (outcome != current_ticket_outcome_) {
        qCWarning(lcFileLoader) << "[FileLoader] conflicting second resolution for ticket" << ticket << ": recorded"
                                << static_cast<int>(current_ticket_outcome_) << ", ignoring"
                                << static_cast<int>(outcome);
      }
      return;
    }
    current_ticket_terminal_ = true;
    current_ticket_outcome_ = outcome;
  }
  pending_terminals_.push_back(
      PendingTerminal{ticket, outcome, effective_path, dataset_id, std::move(produced_dataset_ids), std::move(reason)});
  // Queued, so no terminal arrives before loadFileTicketed returns; joinForShutdown flushes synchronously.
  QMetaObject::invokeMethod(this, [this]() { deliverPendingTerminal(); }, Qt::QueuedConnection);
}

void FileLoader::deliverPendingTerminal() {
  if (pending_terminals_.empty()) {
    return;  // a shutdown flush got there first
  }
  const PendingTerminal terminal = std::move(pending_terminals_.front());
  pending_terminals_.pop_front();
  emit loadFinished(
      terminal.ticket, terminal.outcome, terminal.effective_path, terminal.dataset_id, terminal.produced,
      terminal.reason);
}

void FileLoader::postIngestTerminal(IngestToken token, IngestOutcome outcome) {
  QMetaObject::invokeMethod(
      this, [this, token, outcome]() { session_.endIngest(token, outcome); }, Qt::QueuedConnection);
}

void FileLoader::flushPendingTerminals() {
  while (!pending_terminals_.empty()) {
    deliverPendingTerminal();
  }
}

bool FileLoader::isBusy() const {
  return active_load_ || !queue_.empty();
}

DatasetId FileLoader::activeLoadDatasetId() const {
  return ctx_ != nullptr ? ctx_->dataset_id : 0;
}

void FileLoader::joinForShutdown() {
  // Resolved at the end, once the loader is reset, so a slot reacting to the cancel can start a fresh load.
  struct CancelledRequest {
    quint64 ticket;
    QString identity;
  };
  std::vector<CancelledRequest> cancelled_requests;
  if (active_load_ && !current_ticket_terminal_) {
    cancelled_requests.push_back({current_load_ticket_, current_load_identity_});
  }
  for (const LoadRequest& request : queue_) {
    cancelled_requests.push_back({request.ticket, request.input.source_identity});
  }

  shutting_down_ = true;
  // Invalidates every queued callback; next_load_generation_ is kept, so no later load reuses a value.
  load_generation_ = 0;
  cancel_mode_.store(2);
  if (active_fanout_ingest_ != nullptr) {
    // A plugin that polls is_stop_requested but never reports progress would otherwise run to completion.
    active_fanout_ingest_->requestStop();
  }
  // Before the wait: a worker parked in the message-box ABI waits on a GUI request queued behind this call, and
  // wait() would deadlock.
  message_gate_->shutdown();
  if (!active_plugin_message_dialog_->isNull()) {
    // The completion is release-once: no double answer after the gate release above.
    (*active_plugin_message_dialog_)->reject();
    active_plugin_message_dialog_->clear();
  }
  if (ctx_ != nullptr && ctx_->bound.ingest) {
    ctx_->bound.ingest->requestStop();
  }
  if (worker_) {
    worker_->wait();
    tsan::acquire(this);
    worker_.reset();
  }
  // onWorkerFinished and the fan-out continuation will not run: end their tokens, detached first so an ingestEnded
  // observer re-entering shutdown finds nothing to end twice.
  std::vector<IngestToken> shutdown_ingest_tokens;
  if (ctx_ != nullptr && ctx_->ingest_token.id != 0) {
    shutdown_ingest_tokens.push_back(ctx_->ingest_token);
    ctx_->ingest_token = {};
  }
  shutdown_ingest_tokens.reserve(shutdown_ingest_tokens.size() + fanout_ingest_tokens_.size());
  for (const auto& entry : fanout_ingest_tokens_) {
    shutdown_ingest_tokens.push_back(entry.second);
  }
  fanout_ingest_tokens_.clear();
  for (const IngestToken token : shutdown_ingest_tokens) {
    session_.endIngest(token, IngestOutcome::kCancelled);
  }
  // Only after the join: a fan-out worker captures frame locals. A prologue suspended at its dialog closes it and
  // rejects the plugin while the context is alive (~DataSourceDialogAwaiter).
  begin_load_task_.reset();
  // onWorkerFinished will no-op: discard here, as it would. A first load's dataset is erased; a replacing reload
  // rolls back through the guard when ctx_ is reset.
  const bool was_replacing = (ctx_ != nullptr && ctx_->replacing);
  const DatasetId reload_id = (ctx_ != nullptr) ? ctx_->dataset_id : 0;
  if (ctx_ != nullptr && !was_replacing) {
    removeCreatedDataset(reload_id);
  }
  ctx_.reset();
  if (was_replacing) {
    refreshAfterReplacingRollback(reload_id);
  }
  queue_.clear();
  active_load_ = false;
  current_load_ticket_ = 0;
  current_load_identity_.clear();
  current_ticket_terminal_ = false;
  // Or the next prologue takes it for a cancel issued at its dialog.
  cancel_mode_.store(0);
  // The loader stays reusable; the shut gate keeps neutering straggler message-box lambdas.
  message_gate_ = std::make_shared<PluginMessageGate>();
  shutting_down_ = false;
  // Every discarded request still resolves its ticket, in FIFO order.
  for (const CancelledRequest& cancelled : cancelled_requests) {
    emitLoadFinished(cancelled.ticket, LoadOutcome::kCancelled, cancelled.identity, 0);
  }
  // The one synchronous delivery: teardown must not lose a terminal. The loader is reset, so re-entrant slots are safe.
  flushPendingTerminals();
}

QString FileLoader::sourcePathForDataset(DatasetId dataset_id) const {
  return session_.datasetSourcePath(dataset_id);
}

void FileLoader::untrackDataset(DatasetId dataset_id) {
  session_.setDatasetSourcePath(dataset_id, {});
}

bool FileLoader::loadFile(const QString& path, QWidget* dialog_parent) {
  return loadFile(path, dialog_parent, LoadHints{});
}

}  // namespace PJ
