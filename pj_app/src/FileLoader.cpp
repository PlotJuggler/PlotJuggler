// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "FileLoader.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
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
#include <QVBoxLayout>
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
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
#ifdef PJ_WITH_SCENE3D
#include "pj_scene3d_widgets/transform_service.h"
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

// Rendezvous between an import worker parked in the synchronous message-box ABI
// and the GUI thread that answers it. Deliberately NOT FileLoader state: the
// queued GUI lambda that would open the dialog and the worker blocked on the
// answer can both outlive joinForShutdown — and even the FileLoader — so all
// parties hold this by shared_ptr. A shut gate answers every request -1 and
// never opens UI.
struct PluginMessageGate {
  // One worker question. finish() is release-once: the first answer (a button,
  // dialog destruction, or shutdown) wins; later answers are ignored.
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

  // Worker: register a request about to block. False when the gate is already
  // shut — the caller must answer -1 itself and must not post any GUI work.
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

  // GUI: refuse future requests and release every parked worker with -1, so
  // joinForShutdown can join the worker without pumping the event queue the
  // worker's dialog request may still be sitting in.
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

// Await the callback-style dialog presenter while also handling its synchronous
// no-dialog/contract-error completion paths. The shared state prevents a late
// QDialog callback from touching a coroutine frame destroyed during shutdown,
// and the destructor cancels a still-open dialog so the plugin is rejected
// while its context is alive.
class DataSourceDialogAwaiter {
 public:
  explicit DataSourceDialogAwaiter(dialog_presenter::DataSourceRequest request)
      : request_(std::move(request)), state_(std::make_shared<State>()) {}

  DataSourceDialogAwaiter(const DataSourceDialogAwaiter&) = delete;
  DataSourceDialogAwaiter& operator=(const DataSourceDialogAwaiter&) = delete;

  ~DataSourceDialogAwaiter() {
    // Runs either after normal resumption (the cancel below is then a no-op) or
    // while the suspended frame is being destroyed at shutdown. Neuter the
    // completion FIRST — resuming a frame mid-destruction is UB — then close a
    // dialog still open so the plugin receives its one on_rejected while the
    // frame's DataSourceHandle (declared before this awaiter, hence destroyed
    // after it) still owns a live plugin context.
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
  // Synchronous teardown hook for the pending dialog; empty when the presenter
  // completed without opening one.
  dialog_presenter::DialogCancelFn cancel_dialog_;
};

// Runs a blocking importer body on QThread and resumes its owning coroutine on
// the GUI thread after QThread::finished. The FileLoader owns both the QThread
// and coroutine frame, so shutdown can join first and then cancel continuation.
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
    // The continuation resumes only after QThread::finished, so wait() returns
    // near-instantly here; it still closes the window where reset() could
    // destroy the QThread object while it is mid-teardown from emitting that
    // very signal, matching every other worker teardown site in this file.
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

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
void logSuccessfulLoad(
    const DataEngine& engine, const CatalogModel& catalog, const QString& source_identity, const QString& plugin_name,
    const std::vector<DatasetId>& dataset_ids) {
  // The catalog walk + up to kMaxLoggedSeries DataReader::series() reads below
  // are pure diagnostic cost on the GUI thread; skip all of it when the
  // category won't actually emit.
  if (!lcFileLoader().isInfoEnabled()) {
    return;
  }
  if (dataset_ids.empty()) {
    // No datasets — the filter below would degenerate to match-all and log a
    // "success" enumerating the whole unrelated catalog.
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
  qCInfo(lcFileLoader).noquote() << "PJ_FILE_LOAD_OK" << u"plugin=%1"_s.arg(plugin_name)
                                 << u"identity=%1"_s.arg(source_identity) << u"catalog_items=%1"_s.arg(catalog_items)
                                 << u"scalar_count=%1"_s.arg(scalar_count)
                                 << u"scalar_series=%1"_s.arg(scalar_series.join(u","_s))
                                 << u"series_samples=%1"_s.arg(series_samples.join(u","_s));
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

    PJ::Expected<PJ::sdk::ObjectRecord> record = PJ::unexpected(std::string("parser not invoked"));
    if (binding.mutex) {
      std::lock_guard<std::mutex> lock(*binding.mutex);
      record = binding.parser->parseObject(resolved->timestamp, resolved->payload);
    } else {
      record = binding.parser->parseObject(resolved->timestamp, resolved->payload);
    }
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

// Merge the file path into the (possibly empty) saved JSON config. Saved
// config carries the dialog state from the previous load (delimiter, time
// column, etc.) so the dialog opens pre-populated. If saved_config doesn't
// parse, treat it as empty rather than failing the import.
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

// A long plugin message (e.g. a per-row list of thousands of skipped CSV
// lines) would stretch a plain QMessageBox label past the screen and push
// the action buttons out of reach. Render it in an app-styled PJ::Dialog
// (same chrome as DiagnosticsDetailDialog): themed icon + summary on top, the
// error text in a read-only monospace scroll view that manages its own space,
// and role-styled buttons at the bottom. The scroll view bounds the dialog,
// so it never grows past the screen no matter how many lines the plugin sends.
//
// A namespace-scope function rather than a branch inside the message-box
// handler's nested lambdas: MSVC's front end rejects by-ref captures through
// that lambda chain (bogus C3493 "'this' cannot be implicitly captured" /
// C2065 '__this'), so the dialog construction lives here.
//
// Completes with the PJ_MSG_BTN_* mask of the clicked button; a window-close
// (✕ / Esc) completes with -1, i.e. "do not proceed".
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

// `active_dialog` is heap-shared FileLoader state (see FileLoader.h): the
// WA_DeleteOnClose dialog can outlive the loader, so its finished/destroyed
// hooks capture the shared_ptr, never a pointer into FileLoader.
void openScrollableMessageDialog(
    QWidget* dialog_parent, const QString& q_title, const QString& q_text, int type, int buttons,
    const std::shared_ptr<QPointer<QDialog>>& active_dialog, PluginMessageCompletion completion) {
  // Keep the first line as the summary (the non-scrolling label) and
  // route everything else into the bounded scroll view. Producers put
  // a one-line summary first; anything longer must scroll, never
  // inflate the label -- otherwise we reproduce the very overflow this
  // dialog exists to prevent. (A leading blank line, when a producer
  // separates summary from detail with "\n\n", is dropped by trimmed().)
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
  // App-modal (not window-modal) restores the pre-branch exec() semantics:
  // window-modality would leave floating ADS dock tool-windows interactive.
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

  // Read-only, monospace scroll view (the CurveTreeView FixedFont
  // idiom): it scrolls its own content, so the dialog stays bounded.
  // Skipped for a short (single-line) message — no detail to scroll, so the
  // dialog stays compact instead of showing an empty scroll area.
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

  // App button idiom (see PJ::MessageBox): objectName + msgbox_role
  // dynamic property drive the themed look (primary = brand gradient,
  // cancel = subtler). No QDialogButtonBox — the app reserves that for
  // plugin-hosted dialogs.
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
  // Fall back to a lone OK when the plugin passed no button we render
  // (mirrors the QMessageBox path in the caller); specs[0] is that OK entry.
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
  // show(), not open(): QDialog::open() force-downgrades ApplicationModal back to
  // WindowModal. show() honors the app-modality set above and still fires finished.
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
  // App-modal (not window-modal) restores the pre-branch exec() semantics; see
  // openScrollableMessageDialog.
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
  // Chromium can occasionally leave this short message box behind the
  // main Qt canvas after many fresh threaded-WASM contexts. Keep the acceptance
  // probe deterministic without changing production UI: the missing-parser
  // scenario still proves presentation via PJ_WASM_PLUGIN_MESSAGE, then its
  // sole acknowledgement is activated through the real button.
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

    // The ABI is synchronous for the plugin, but browser UI is not. For a
    // worker-originated question, sleep only that worker while the GUI keeps
    // dispatching events; the WASM path below never nests a Qt event loop.
    if (QThread::currentThread() == qApp->thread()) {
      if (gate->isShutDown()) {
        return -1;  // shutdown in progress: never open UI or spin a nested loop
      }
#ifdef PJ_TARGET_WASM
      qCWarning(lcFileLoader) << "plugin message box requested from the GUI thread; rejecting synchronous answer";
      openPluginMessageBox(message_parent.data(), q_title, q_text, type, buttons, active_dialog, [](int) {});
      return -1;
#else
      // Native Qt can satisfy the plugin's synchronous ABI even when a
      // main-thread lifecycle callback asks the question. Keep the nested loop
      // out of WASM, where browser event-loop re-entry is unsupported.
      //
      // Heap-shared completion state: exec() can unwind WITHOUT an answer
      // (QCoreApplication::exit() terminates nested loops), and the dialog's
      // finished/destroyed hooks may deliver the completion afterwards — it
      // must then write into live storage, not this frame's dead stack.
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
          // Can run after joinForShutdown — and after the FileLoader died —
          // since the GUI event queue outlives both. Everything captured here
          // is shared state, and a shut gate answers -1 without opening UI.
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

}  // namespace

// Default ingest policies the app applies to every DataSourceRuntimeHost it
// builds: scalars eager, objects lazy (decoded on pull). Under the default
// kLazyObjectsEagerScalars the payload bytes are fetched ONCE at ingest so the
// parser's scalar route can emit its slim timeseries (header stamp, frame_id,
// counts), then dropped from RAM; object pulls re-fetch from the source, so
// large blobs (clouds, images, grids) stay NON-resident either way and peak
// RSS is unaffected. The price is one extra read of those messages at load
// time — accepted so every header-bearing object topic (point clouds, images,
// depth images, occupancy/voxel grids) exposes its header/timestamp series in
// the curve tree, not only a 3D/2D object.
// PURE-LAZY (bytes never fetched at ingest, no scalar series at all) is kept
// only where even that one ingest-time read buys nothing worth plotting:
//   * Video frames: reading every file-backed bitstream at load is a
//     full-file pass for a couple of metadata columns; the video dock pulls
//     frames on demand instead.
//   * SceneEntities (markers) / ImageAnnotations: no meaningful scalar
//     fields; their parsers return empty/slim rows purely so ingest survives
//     under non-pure-lazy policies.
// TF (kFrameTransforms) intentionally stays eager: its payload is tiny and its
// scalar fields are useful. Static so the pre-dialog scratch session and the
// per-fanout loop iterations stay in lockstep, and so it is unit-testable
// against a bare resolver.
void FileLoader::applyDefaultIngestPolicies(PJ::sdk::ObjectIngestPolicyResolver& resolver) {
  using PJ::sdk::BuiltinObjectType;
  using PJ::sdk::ObjectIngestPolicy;
  resolver.setDefault(ObjectIngestPolicy::kLazyObjectsEagerScalars);
  resolver.setForType(BuiltinObjectType::kSceneEntities, ObjectIngestPolicy::kPureLazy);
  resolver.setForType(BuiltinObjectType::kImageAnnotations, ObjectIngestPolicy::kPureLazy);
  resolver.setForType(BuiltinObjectType::kVideoFrame, ObjectIngestPolicy::kPureLazy);
#ifdef PJ_TARGET_WASM
  // The browser RobotModel consumes std_msgs/String through the object parser.
  // That parser intentionally has no scalar handler, so keep its payload lazy
  // until the layer requests the selected description. Fence this to WASM so
  // the desktop application's ingest policy remains exactly unchanged.
  resolver.setForType(BuiltinObjectType::kRobotDescription, ObjectIngestPolicy::kPureLazy);
#endif
#ifdef PJ_WASM_ENABLE_MCAP_PROBE_PARSER
  // The official 50k-message cancellation fixture uses this topic. Force its
  // acceptance-only parser through the eager scalar route so the browser test
  // has a deterministic window in which to exercise cooperative cancellation;
  // normal builds retain the production policy above.
  resolver.setForTopic(kWasmProbeBulkTopic, ObjectIngestPolicy::kEager);
#endif
}

// Per-load state for a single-instance worker load. Holds the bound plugin
// handle + ingest host so they outlive the GUI prologue across the worker run.
struct FileLoader::LoadContext {
  // DataSourceHandle has no default ctor (it wraps a created plugin instance),
  // so the context is built by moving the bound handle + host in.
  LoadContext(DataSourceHandle bound_handle, std::unique_ptr<DataSourceRuntimeHost> bound_ingest)
      : handle(std::move(bound_handle)), ingest(std::move(bound_ingest)) {}

  QString path;
  QPointer<QWidget> dialog_parent;
  QString source_name;
  // The selected plugin's stable manifest id, carried alongside its display
  // name (source_name) so the commit epilogue's fileLoaded reports both.
  QString source_manifest_id;
  std::string config;
  DatasetId dataset_id = 0;
  quint64 ticket = 0;  // the request's LoadRequestId, for the worker-path terminal
  bool replacing = false;
  // REPLACING loads only: display name applied at commit (plugin-supplied name,
  // else the incoming file's basename). Deferred so a rollback keeps the prior
  // dataset's name — a replace may point the dataset at a different file.
  QString commit_display_name;
  // Engaged only on a REPLACING reload: the RAII transaction that detached the
  // prior data up front. Committed on success/keep (onWorkerFinished); otherwise
  // its destructor — fired by ctx_.reset()/destruction — rolls the dataset back.
  std::optional<RefillGuard> refill_guard;
  int file_index = 1;
  int file_total = 1;
  DataSourceHandle handle;
  std::unique_ptr<DataSourceRuntimeHost> ingest;
  IngestToken ingest_token{};
  // Worker-owned: started on the GUI before the thread runs, then only the
  // worker touches it (elapsed/restart) to pace flush+notify.
  QElapsedTimer flush_clock;
  // Set once by on_progress_start (worker); read by the queued progress lambda.
  uint64_t progress_total = 0;
  // Worker -> GUI handoff (read in onWorkerFinished after the worker joins).
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
  // Move ctor only: BeginLoadTask lives exclusively in a std::unique_ptr
  // (make_unique + reset), which never move-assigns the pointee itself.
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
      message_gate_(std::make_shared<PluginMessageGate>()) {}

FileLoader::~FileLoader() {
  // Stricter admission than a reusable shutdown: the final synchronous
  // terminal flush inside joinForShutdown runs loadFinished slots, and one of
  // them starting fresh work here would leave an accepted request nothing can
  // ever resolve (and a worker outliving this object).
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

  // PJ::FileDialog wraps a non-native QFileDialog in our frameless
  // chrome — see pj_widgets/FileDialog.h. The native GTK dialog also
  // crashes on this app's libpng ABI skew (see the --exclude-libs,ALL
  // note in pj_app/CMakeLists.txt), so we avoid it both for look and
  // for stability. The shell-injected picker threads MainWindow's chrome
  // metrics into the dialog (toolbar icon size, kept in step via
  // chromeMetricsChanged) — see setFilePicker().
  const QStringList paths = file_picker_ != nullptr
                                ? file_picker_(dialog_parent, tr("Load Data"), last_dir, filter, /*multi=*/true)
                                : FileDialog::getOpenFileNames(dialog_parent, tr("Load Data"), last_dir, filter);
  if (paths.isEmpty()) {
    return;
  }
  // Remember the directory of the last pick for next time.
  settings.setValue(kLastDirKey, QFileInfo(paths.last()).absolutePath());
  // Load each selected file in order so several datasets populate in one go.
  // Each loadFile may pop its own data-source config dialog and reports its own
  // failures, so a single bad file doesn't abort the rest.
  for (const QString& path : paths) {
    loadFile(path, dialog_parent);
  }
#endif
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
  // Single-select: the pick replaces exactly one dataset.
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
  // Qt's getOpenFileContent is the browser-native, non-blocking API.
  // It must be invoked directly from this user-activation turn; no nested
  // QEventLoop/exec() is involved.
  QPointer<FileLoader> self(this);
  QPointer<QWidget> guarded_parent(dialog_parent);
  selectBrowserInput(
      dialog_parent, [self, guarded_parent, hints = std::move(hints)](BrowserSelectionResult staged) mutable {
        if (self.isNull() || (!staged.input.has_value() && staged.error.isEmpty())) {
          return;  // loader destroyed, or picker cancelled.
        }
        if (!staged.input.has_value()) {
          const QString reason = staged.error.isEmpty() ? self->tr("Could not stage the selected file.") : staged.error;
          qCWarning(lcFileLoader).noquote() << reason;
          self->reportLoadWarning(guarded_parent.data(), reason);
          emit self->fileLoadFailed(staged.browser_name, reason);
          return;
        }
        self->loadFile(std::move(*staged.input), guarded_parent, hints);
      });
}

void FileLoader::selectBrowserInput(QWidget* dialog_parent, BrowserSelectionCallback callback) {
  const QString filter = extensions_.buildFileFilter();
  qCInfo(lcFileLoader).noquote() << "Opening browser file picker with filter:" << filter;
  FileSelectionService::selectAndStageFile(
      dialog_parent, filter, browser_file_store_,
      [callback = std::move(callback)](FileSelectionService::StagedSelection staged) mutable {
        callback({
            .browser_name = std::move(staged.browser_name),
            .input = std::move(staged.input),
            .error = std::move(staged.error),
        });
      });
}

QString FileLoader::browserContentSha256(const QString& source_identity) const {
  return browser_content_sha256_.value(source_identity);
}
#endif

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
    // Non-replacing loads create directly in the live engine. If they fail
    // before commit, drop any emitted catalog items (and TF state) before
    // erasing the engine dataset so no reader/adapter can keep a dangling
    // TopicStorage pointer.
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

  // One unified failure path — log, optionally pop a dialog, emit signals.
  // Every fail() call is followed by co_return, so it is a terminal exit and
  // resolves the request's ticket — terminalized BEFORE the re-entrant legacy
  // surfaces (warning dialog, fileLoadFailed) run.
  const auto fail = [&](const QString& reason) -> bool {
    erase_created_live_dataset();
    rollback_tombstones();
    emitLoadFinished(ticket, LoadOutcome::kFailed, source_identity, 0);
    qCWarning(lcFileLoader).noquote() << reason;
    reportLoadWarning(dialog_parent.data(), reason);
    emit fileLoadFailed(source_identity, reason);
    return false;
  };

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

  // Interactive/native loads keep the established first-match behavior. A
  // layout that names a plugin (browser source-bound replay, desktop layout
  // replay) opts into the exact plugin it saved: silently choosing another
  // extension match could parse the same bytes with different semantics.
  // Matching is tiered on SEPARATE identities — the stable MANIFEST id
  // (expected_manifest_id, when the layout carries one) exact across ALL
  // matches first, then the display name (expected_plugin_id) exact as the
  // fallback. Each hint field only ever compares against its own candidate
  // field, so a manifest mismatch still falls back to the saved display name,
  // and an ID-only legacy layout starts at the name tier — it can never
  // select a plugin whose manifest id merely collides with the display name.
  // Both tiers sweep the whole candidate list so an id match on a later
  // candidate beats a name collision on an earlier one.
  // GUI-thread catalog pointer. The async presenter resolves the dialog vtable
  // synchronously before suspension; the created source handle pins its DSO for
  // the rest of this coroutine/worker load.
  const LoadedDataSource* source = matches.front();
  if (hints.require_expected_plugin) {
    const auto match_by = [&matches](auto&& field_of) {
      return std::find_if(matches.begin(), matches.end(), [&field_of](const LoadedDataSource* candidate) {
        return candidate != nullptr && field_of(*candidate);
      });
    };
    auto expected = matches.end();
    if (!hints.expected_manifest_id.isEmpty()) {
      expected = match_by([&hints](const LoadedDataSource& candidate) {
        return QString::fromStdString(candidate.id) == hints.expected_manifest_id;
      });
    }
    if (expected == matches.end() && !hints.expected_plugin_id.isEmpty()) {
      expected = match_by([&hints](const LoadedDataSource& candidate) {
        return QString::fromStdString(candidate.name) == hints.expected_plugin_id;
      });
    }
    if (expected == matches.end()) {
      const QString expected_label = !hints.expected_plugin_id.isEmpty()     ? hints.expected_plugin_id
                                     : !hints.expected_manifest_id.isEmpty() ? hints.expected_manifest_id
                                                                             : tr("(unspecified)");
      (void)fail(tr("The layout requires DataSource plugin '%1', but it is not installed for %2 files.")
                     .arg(expected_label, ext));
      co_return;
    }
    source = *expected;
  }
  const QString source_name = QString::fromStdString(source->name);
  const QString source_manifest_id = QString::fromStdString(source->id);

  DataSourceHandle handle = source->library.createHandle();
  if (!handle.valid()) {
    (void)fail(tr("Plugin '%1': createHandle failed.").arg(source_name));
    co_return;
  }
  // Snapshot everything needed after an asynchronous dialog. The handle owner
  // pins the DSO even if a later desktop marketplace reload reallocates the
  // GUI-owned catalog vector.
  const PJ_data_source_vtable_t* const source_vtable = source->library.vtable();
  const std::shared_ptr<void> source_library_owner = handle.libraryOwner();
  const std::string source_id = source->id;
  const std::string source_plugin_name = source->name;

  // The v4 DataSource protocol resolves host services during bind(), so the
  // target dataset must exist before loadConfig() and start().
  DataEngine& engine = session_.dataEngine();

  const QString display_name = input.display_name;
  const std::string display_name_utf8 = display_name.toStdString();

  // One TimeDomain per loaded source so each is independently time-shiftable
  // (the Source Timeline drives the per-domain display_offset). The staged
  // replace path below mints its own; the live first-load uses this one.
  auto td_or = engine.createTimeDomain(display_name_utf8);
  if (!td_or.has_value()) {
    (void)fail(tr("Could not create the time domain for %1.").arg(display_name));
    co_return;
  }
  const TimeDomainId td_id = *td_or;

  // Same-source handling: layout replay reuses the existing DatasetId; an interactive load/reload replaces the
  // dataset's data in place, keeping its DatasetId/TopicIds (and so all curve keys) stable. The engine names
  // datasets by basename, so the basename match is only a pre-filter — reuse is gated on full-path identity,
  // so two different files that share a basename (e.g. log.mcap in separate run dirs) stay distinct datasets
  // instead of the second silently aliasing the first.
  DatasetId existing_primary_id = 0;
  // The dataset "Replace" action targets a dataset chosen by the user, not by
  // source identity — honor it directly. A target that vanished while the
  // picker/config dialog was open degrades to a plain fresh load.
  if (hints.replace_dataset_id != 0 && engine.getDataset(hints.replace_dataset_id) != nullptr) {
    existing_primary_id = hints.replace_dataset_id;
  } else {
    if (strict_replace) {
      // The caller pinned this load to one dataset, and silently loading the
      // file fresh would break that association.
      (void)fail(tr("Replacement target dataset %1 no longer exists.").arg(hints.replace_dataset_id));
      co_return;
    }
    // One pass over the datasets, ranking two match kinds (sameSourceIdentity
    // canonicalizes paths — filesystem syscalls — so compute it once per dataset):
    // 1. Path identity, honored only when the incoming path backs exactly ONE
    //    dataset. This finds a replaced dataset — its engine source_name keeps
    //    the ORIGINAL basename (there is no engine-level rename), so only its
    //    tracked path identifies it. Fan-out children are one-of-several from
    //    the same file, so a fresh load / layout replay of that file must not
    //    silently refill one of them.
    // 2. Engine basename, gated on path identity when a path is tracked; a
    //    dataset with no tracked path (created outside FileLoader, e.g.
    //    streaming/test data) keeps the legacy basename-only match.
    DatasetId sole_path_match = 0;
    bool path_match_ambiguous = false;
    DatasetId basename_match = 0;
    for (const auto existing_id : engine.listDatasets()) {
      const DatasetInfo* info = engine.getDataset(existing_id);
      if (info == nullptr) {
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
    // Reuse the id referenced by the layout; keep the recorded config
    // when legacy XML has no preset. Match the recorded source by path
    // (not just the most-recent one) so a multi-file session recovers the
    // right file's config when reloading any of its sources.
    QString emit_config = hints.preset_config_json;
    if (emit_config.isEmpty()) {
      // loadedSources() paths are stored normalized; compare canonically so a
      // symlink/relative alias of a tracked file still recovers its config.
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
    co_return;  // layout-replay reuse: done synchronously, no worker
  }
  // Tombstone of a matched dataset is deferred to the post-ingest swap (single-instance) or the fanout fallback
  // below: don't disturb the live dataset until the staged ingest has succeeded.

  // Ingest target — always the LIVE engine/store (no staging engine). A first
  // load creates a fresh dataset. A same-source single-instance reload binds to
  // the EXISTING dataset and refills it in place under the transactional
  // RefillGuard from beginRefill() (below), so its DatasetId/TopicIds — and every
  // curve key — stay stable: the write host's ensureTopic reuses each topic by
  // name, writing back into the original ids. (A fanout reload can't refill one
  // dataset into N, so it falls back to remove-then-fresh-load; see the fanout branch.)
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

  // A non-replacing first load just created its dataset in the LIVE engine
  // (above). Track it so a SYNCHRONOUS prologue failure (bind / loadConfig below,
  // before the worker starts) erases the abandoned shell via fail()'s
  // erase_created_live_dataset, instead of leaving an empty dataset a prefer_reuse
  // layout replay could reattach to. Worker-thread failure / discard is handled
  // symmetrically in onWorkerFinished (which knows the same dataset via !replacing).
  if (!replacing) {
    created_live_dataset_id = dataset_id;
  }

  // Parsers register directly under the live ids (no staged remap needed).
  DataSourceRuntimeHost::ObjectTopicParserRegistrar object_parser_registrar =
      [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
        session_.registerObjectTopicParser(id, std::move(parser));
      };
  // Heap-owned so a single-instance load can MOVE it into ctx_ and let the
  // worker thread run handle.start() against it after this prologue returns.
  // The reference stays valid across that move (unique_ptr move transfers
  // ownership; the object's address does not change).
  auto ingest_session_ptr = std::make_unique<DataSourceRuntimeHost>(
      engine, extensions_, dataset_id, source_handle, session_.objectStore(), source_id,
      std::move(object_parser_registrar), nullptr, nullptr, combineKeepalive(handle.libraryOwner(), input.lease),
      session_.ingestTapsShared());
  DataSourceRuntimeHost& ingest_session = *ingest_session_ptr;
  DataSourceRuntimeHost::MessageBoxHandler message_box_handler;
  if (dialog_parent != nullptr) {
    message_box_handler = makePluginMessageBoxHandler(dialog_parent, active_plugin_message_dialog_, message_gate_);
    ingest_session.setMessageBoxHandler(message_box_handler);
  }
  applyDefaultIngestPolicies(ingest_session.policyResolver());

  ServiceRegistryBuilder registry;
  if (auto status = ingest_session.registerServices(registry); !status) {
    (void)fail(
        tr("Plugin '%1': service registration failed: %2").arg(source_name, QString::fromStdString(status.error())));
    co_return;
  }

  if (auto status = handle.bind(registry.view()); !status) {
    (void)fail(tr("Plugin '%1': bind failed: %2").arg(source_name, QString::fromStdString(status.error())));
    co_return;
  }

  // Pre-populate the dialog with last-used settings so users don't re-pick
  // delimiter/time column on every load. The layout-driven path (hints
  // with a matching plugin id + a usable preset config) skips the dialog
  // entirely; we fall back to the dialog with the QSettings pre-fill if
  // either the id mismatches or loadConfig rejects the preset.
  QSettings persisted_settings;
  const QString config_key = pluginConfigKey(source_plugin_name);
  const std::string saved_config = persisted_settings.value(config_key, QString()).toString().toStdString();

  std::string config;
  bool skip_dialog = false;
  // The layout preset's saved bytes — the one materialization both
  // preset-driven policies start from.
  const std::string preset_bytes = hints.preset_config_json.toStdString();

  if (hints.dialog_policy == DialogPolicy::kNever) {
    // Automated (layout-driven) replay: the dialog is prohibited and the
    // saved preset is AUTHORITATIVE — the browser replay's filepath rewrite
    // when requested, the minimal fresh-filepath config for an intentionally
    // empty preset (a plugin whose saveConfig returned nothing is
    // legitimate), the saved bytes verbatim otherwise. A rejected preset
    // FAILS the source: silently retrying with the QSettings pre-fill would
    // replay the layout with a configuration the user never saved.
    const std::string preset = hints.rewrite_preset_filepath
                                   ? detail::rewriteReplayFilepaths(preset_bytes, input.backing_path)
                               : preset_bytes.empty() ? buildLoadConfig(std::string_view{}, input.backing_path)
                                                      : preset_bytes;
    if (auto status = handle.loadConfig(preset); !status) {
      (void)fail(tr("Plugin '%1' rejected the layout's saved configuration: %2")
                     .arg(source_name, QString::fromStdString(status.error())));
      co_return;
    }
    config = preset;
    skip_dialog = true;
  } else {
    // kPreferPreset: skip the dialog when the preset names the SELECTED
    // plugin (each hint identity against its own candidate field) and applies
    // cleanly. kInteractive, a missing/mismatched hint, or a rejected preset
    // all drive the dialog with the QSettings pre-fill.
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
    if (!skip_dialog) {
      config = buildLoadConfig(saved_config, input.backing_path);
      if (auto status = handle.loadConfig(config); !status) {
        (void)fail(tr("Plugin '%1': loadConfig failed: %2").arg(source_name, QString::fromStdString(status.error())));
        co_return;
      }
    }
  }

  if (!skip_dialog) {
    // Extract parser config saved from a previous session so DialogEngine can
    // restore the embedded parser dialog to its last state.
    std::string initial_parser_config;
    {
      auto saved_cfg = nlohmann::json::parse(saved_config, nullptr, false);
      if (!saved_cfg.is_discarded() && saved_cfg.contains("_parser_config")) {
        initial_parser_config = saved_cfg["_parser_config"].get<std::string>();
      }
    }

    // Named frame local, deliberately NOT a co_await temporary: GCC 11.4 (the
    // linux-ci / AppImage compiler) mishandles awaiter temporaries inside
    // co_await expressions — as a temporary this segfaulted the
    // DialogShutdownTest pair on CI while newer GCC/MSVC were fine. A named
    // local keeps the same declaration order relative to `handle` (so frame
    // destruction still rejects the dialog before the plugin context dies).
    DataSourceDialogAwaiter dialog_awaiter({
        .source = *source,
        .handle = handle,
        .catalog = extensions_,
        .parent = dialog_parent,
        .initial_parser_config = initial_parser_config,
        .browser_file_store = browser_file_store_,
    });
    const auto dlg = co_await std::move(dialog_awaiter);
    if (dlg.outcome == dialog_presenter::Outcome::kPluginContractViolation) {
      (void)fail(tr("Plugin contract violation: %1. Reinstall the plugin from the Marketplace.")
                     .arg(QString::fromStdString(dlg.error)));
      co_return;
    }
    if (dlg.outcome == dialog_presenter::Outcome::kRejected) {
      // Diagnostic marker: the reject path completed with no dataset committed
      // (automation greps for it on every platform).
      qCInfo(lcFileLoader).noquote() << "PJ_FILE_LOAD_REJECTED" << u"plugin=%1"_s.arg(source_name)
                                     << u"identity=%1"_s.arg(source_identity);
      // The user declined the load: a terminal cancel for the ticket, while
      // the legacy signals stay silent as they always have on this exit.
      // Clean up before emitting so a slot observing the terminal sees the
      // rolled-back state.
      erase_created_live_dataset();
      rollback_tombstones();
      emitLoadFinished(ticket, LoadOutcome::kCancelled, source_identity, 0);
      co_return;
    }
    if (dlg.payload.has_value()) {
      config = dlg.payload->saved_config;
      // If the dialog had an embedded parser slot, embed the parser config in the
      // saved config so it survives across sessions and reaches the source via
      // loadConfig(). The source extracts it under the "_parser_config" key.
      if (!dlg.payload->parser_config.empty()) {
        auto cfg = nlohmann::json::parse(config, nullptr, false);
        if (!cfg.is_discarded()) {
          cfg["_parser_config"] = dlg.payload->parser_config;
          config = cfg.dump();
        }
      }
      // DialogEngine already wrote the dialog's choices back via the dialog vtable,
      // but for plugins that split dialog state from source state the explicit
      // reload keeps the contract uniform.
      if (auto status = handle.loadConfig(config); !status) {
        (void)fail(tr("Plugin '%1': loadConfig (post-dialog) failed: %2")
                       .arg(source_name, QString::fromStdString(status.error())));
        co_return;
      }
    }
  }

  // A cancel ("Remove All" / "Stop") issued while this prologue sat suspended at
  // its config dialog set cancel_mode_ but had no worker to observe it. Honor it
  // here rather than letting the cancel_mode_.store(0) below silently wipe it:
  // roll back any partial state and abandon the load. (shutting_down_ is handled
  // separately by joinForShutdown, which destroys the frame outright.)
  if (cancel_mode_.load() != 0) {
    qCInfo(lcFileLoader) << "[FileLoader] load cancelled while suspended at its config dialog";
    cancel_mode_.store(0);
    erase_created_live_dataset();
    rollback_tombstones();
    emitLoadFinished(ticket, LoadOutcome::kCancelled, source_identity, 0);
    emit fileLoadFailed(source_identity, tr("Load cancelled"));
    co_return;
  }

  // Persist before start() so dialog choices stick even if ingest fails.
  // Skip on the hint path: layout-driven reloads should NOT overwrite the
  // user's last interactive choice in QSettings (spec §11). Otherwise
  // opening a layout would silently mutate the global per-plugin pre-fill.
  if (!skip_dialog) {
    persisted_settings.setValue(config_key, QString::fromStdString(config));
  }

  // Detect multi-instance fanout. A DataSource plugin emits a `__pj_fanout`
  // array on accept when one selection should expand into several independent
  // imports — each entry becomes its own DatasetId. For single-instance
  // importers the helper returns `{ config }` and the legacy flow runs unchanged.
  const auto fanouts = detail::extractFanout(config);

  // Strict replacement cannot survive a fan-out expansion: one pinned target
  // cannot absorb N results, and the fan-out fallback (tombstone the target,
  // mint fresh ids) would silently break the caller's dataset association.
  // Rejected HERE, before the fan-out arm below mutates the target.
  if (strict_replace && fanouts.size() != 1) {
    (void)fail(tr("Cannot replace dataset %1: the selected configuration expands into %2 datasets.")
                   .arg(hints.replace_dataset_id)
                   .arg(fanouts.size()));
    co_return;
  }

  // Live DatasetIds that fanout entries actually loaded data into (Completed or
  // Cancel-kept); the post-load TF ingest below runs on these. The pre-branch
  // scratch dataset never qualifies — it stays empty in fanout mode, and on a
  // replacing fanout its id is staged-engine-scoped (it may alias an unrelated
  // live dataset).
  std::vector<DatasetId> fanout_loaded_ids;
  std::vector<DatasetId> fanout_created_ids;
  bool fanout_committed = false;
  const auto rollback_fanout_on_cancel = qScopeGuard([&]() {
    if (fanout_committed) {
      return;
    }
    for (const DatasetId created_id : fanout_created_ids) {
      removeCreatedDataset(created_id);
    }
  });

  // Fan-out results, declared here so the shared epilogue below the branch can
  // route the three dataset-less shapes — every row discarded, every entry
  // failed, the whole load stopped — away from the success tail. The
  // single-instance arm never touches them. What survived IS fanout_loaded_ids
  // and what went wrong IS failed_labels; no parallel counters restate them.
  QStringList failed_labels;
  // The whole-load stop that ended the fan-out early, in cancel_mode_'s encoding
  // (0 none, 1 keep, 2 discard). Only the title-bar strip and shutdown write
  // that scope, and it is set exactly where the loop breaks on it.
  int whole_load_stop = 0;

  if (fanouts.size() == 1) {
    // --- Single-instance load: run the read loop on a worker thread, filling
    // the datastore progressively while the GUI stays interactive. The modal
    // ProgressDialog above is unused here (destroyed when this prologue returns);
    // progress + cancellation flow through ingestStarted/ingestProgress +
    // cancelCurrent (the title-bar IngestProgressWidget), wired by MainWindow. ---
    // Unreachable by construction: a live strict-replace target binds
    // dataset_id to the pinned id, and the vanished-target and fan-out guards
    // above fail every divergence — so a kLoaded strict replacement always
    // reports produced == {replace_dataset_id}.
    Q_ASSERT(!strict_replace || dataset_id == hints.replace_dataset_id);

    // issue #98: apply the plugin's dataset name before start() so the
    // commit-driven catalog rebuild surfaces curves under the right tree-root.
    // On a REPLACING load the rename waits for commit (finishLoadOnGui): the
    // tree keeps the prior name while the refill runs, and a rollback must not
    // leave the restored data mislabeled with the new source's name.
    const QString plugin_name = detail::parseDisplayName(config);
    if (!replacing && !plugin_name.isEmpty()) {
      catalog_.setDatasetDisplayName(dataset_id, plugin_name);
    }

    // Move the bound handle + host into the per-load context so they outlive
    // this prologue into the worker. The `ingest_session` reference stays valid
    // (the move transfers ownership without relocating the heap object).
    ctx_ = std::make_unique<LoadContext>(std::move(handle), std::move(ingest_session_ptr));
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
      // Transactional in-place refill: DETACH (move aside, not free) the existing
      // dataset's prior data, keeping its topics registered so the refill's
      // ensureTopic rebinds each by name into the same ids. Constructed as the LAST
      // prologue step — after every synchronous fail()-return above and after ctx_
      // exists — so a synchronous prologue failure never touches the live data, and
      // a worker failure / discard / shutdown rolls back via the guard (the prior
      // data lives in the snapshot until commit()). Replaces the old up-front
      // in-place clear, which destroyed the prior data with no rollback.
      ctx_->refill_guard = session_.beginRefill(existing_primary_id);
    }

    // Worker-driven progress. on_progress_* run on the WORKER; they touch only
    // ctx_ (set before the thread starts, not mutated by the GUI until join) and
    // the atomic cancel flag, and marshal every GUI access via invokeMethod.
    const std::uint64_t generation = load_generation_;
    DataSourceRuntimeHost& host = *ctx_->ingest;
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
        return false;  // user asked to stop; the read loop exits cooperatively
      }
      if (ctx_->flush_clock.elapsed() < flush_throttle_ms_) {
        return true;
      }
      ctx_->ingest->flushPending();  // worker-side: seal+commit -> rows visible
      ctx_->flush_clock.restart();
      const DatasetId notify_dataset = ctx_->dataset_id;
      const uint64_t max = ctx_->progress_total;
      QMetaObject::invokeMethod(
          this,
          [this, generation, notify_dataset, current, max]() {
            // A queued tick can survive shutdown and the loader can then be
            // reused. Reject it unless it still belongs to this exact context.
            if (generation != load_generation_ || ctx_ == nullptr || ctx_->dataset_id != notify_dataset) {
              return;
            }
            publishIngestProgress(notify_dataset, ctx_->ingest_token, current, max);
          },
          Qt::QueuedConnection);
      return true;
    };
    host.on_progress_finish = [] {};  // terminal flush is done in onWorkerFinished

    cancel_mode_.store(0);
    ctx_->flush_clock.start();
    worker_ = std::unique_ptr<QThread>(QThread::create([this, generation]() { runIngestOnWorker(generation); }));
    worker_->start();
    rollback_armed = false;   // LoadContext/onWorkerFinished owns rollback now.
    fanout_committed = true;  // No fanout datasets exist on this path.
    worker_handoff = true;
    co_return;  // worker running; onWorkerFinished resumes the queue
  } else {
    // A same-source reload that fans out cannot refill in place (one source becomes N datasets). Fall back to
    // remove-then-fresh: tombstone the existing dataset now (objects evicted past the rollback point below) and let
    // the fanout create fresh datasets on the live engine. The handle bound to existing_primary_id above is never
    // start()ed here (fanout mints its own per-entry handles), so the dataset takes no data before its removal.
    if (replacing) {
      emit sourceReplacementAboutToCommit(ticket, source_identity, existing_primary_id);
    }
    if (replacing && catalog_.removeDataset(existing_primary_id)) {
      tombstoned_for_replace.push_back(existing_primary_id);
    }
    // Multi-instance fanout. On a FRESH load the dataset created above for the bind is now an empty orphan;
    // pj_datastore has no removeDataset, but an empty dataset has no committed topics so
    // CatalogModel::rebuildFromDatastore skips it (no phantom entry). Each fanout entry mints its own handle +
    // dataset + ingest_session. Continue-on-error per the user-confirmed policy: a bad entry does not lose the others.
    // Outcome of ONE fanout entry. Kept keeps the entry's partial flush
    // ("Cancel" — stop here but keep what was already parsed); Discarded throws
    // it away. Whether the loop continues is a separate question, answered by
    // the SCOPE of the stop (whole_load_stop), not by these.
    enum class EntryOutcome { kCompleted, kFailed, kKept, kDiscarded };

    const QString basename = QFileInfo(display_name).completeBaseName();
    // issue #98: let the plugin name the dataset root. `display_name` (if the
    // plugin emitted it in the accepted config) replaces the file basename as
    // the shared prefix; the per-episode `display_suffix` still forms the leaf.
    const QString fanout_name = detail::parseDisplayName(config);
    const QString base = fanout_name.isEmpty() ? basename : fanout_name;

    // Keep SDK control slots on the GUI thread. Only the finite import's
    // blocking start() call runs on a worker; the coroutine frame keeps the
    // handle and host alive until that worker has joined, then destroys them
    // back here on the GUI thread.
    cancel_mode_.store(0);
    const std::uint64_t fanout_generation = load_generation_;
    for (std::size_t idx = 0; idx < fanouts.size(); ++idx) {
      const std::string& cfg_i = fanouts[idx];
      const QString suffix = detail::parseDisplaySuffix(cfg_i, QString::number(idx + 1));
      const QString iter_display = base + QChar('/') + suffix;

      // Each fanned dataset gets its own TimeDomain so it is independently
      // draggable on the Source Timeline (rather than sharing the primary's).
      auto iter_td = engine.createTimeDomain(iter_display.toStdString());
      if (!iter_td.has_value()) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: createTimeDomain failed:" << QString::fromStdString(iter_td.error());
        failed_labels << iter_display;
        continue;
      }
      auto iter_dataset_or = engine.createDataset(
          DatasetDescriptor{.source_name = iter_display.toStdString(), .time_domain_id = *iter_td});
      if (!iter_dataset_or.has_value()) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: createDataset failed:" << QString::fromStdString(iter_dataset_or.error());
        failed_labels << iter_display;
        continue;
      }
      const auto iter_dataset_id = static_cast<DatasetId>(*iter_dataset_or);
      fanout_created_ids.push_back(iter_dataset_id);
      const PJ_data_source_handle_t iter_source_handle{static_cast<uint32_t>(iter_dataset_id)};

      // Construct from the pinned vtable/owner snapshot rather than reading the
      // GUI-owned catalog while start() is on the worker. A marketplace reload
      // may reallocate the catalog, while source_library_owner keeps the DSO
      // mapped.
      DataSourceHandle iter_handle(source_vtable, source_library_owner);
      if (!iter_handle.valid()) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx << "]: createHandle failed";
        failed_labels << iter_display;
        continue;
      }

      DataSourceRuntimeHost iter_ingest(
          engine, extensions_, iter_dataset_id, iter_source_handle, session_.objectStore(), source_id,
          [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
            session_.registerObjectTopicParser(id, std::move(parser));
          },
          nullptr, nullptr, combineKeepalive(iter_handle.libraryOwner(), input.lease), session_.ingestTapsShared());
      if (message_box_handler) {
        iter_ingest.setMessageBoxHandler(message_box_handler);
      }
      applyDefaultIngestPolicies(iter_ingest.policyResolver());

      ServiceRegistryBuilder iter_registry;
      if (auto status = iter_ingest.registerServices(iter_registry); !status) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: service registration failed:" << QString::fromStdString(status.error());
        failed_labels << iter_display;
        continue;
      }

      if (auto status = iter_handle.bind(iter_registry.view()); !status) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: bind failed:" << QString::fromStdString(status.error());
        failed_labels << iter_display;
        continue;
      }
      if (auto status = iter_handle.loadConfig(cfg_i); !status) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: loadConfig failed:" << QString::fromStdString(status.error());
        failed_labels << iter_display;
        continue;
      }

      uint64_t progress_total = 0;
      QElapsedTimer flush_clock;
      flush_clock.start();
      // THIS entry's row stop: 0 none, 1 keep, 2 discard. One slot per entry, so
      // a click that lands after its entry retired — the terminal is delivered
      // through the event loop, and the session still accepts a cancel queued
      // ahead of it — writes into a slot nobody reads again. Shared ownership
      // because the row's cancel handler outlives this iteration (it lives in
      // the session's ingest entry until the terminal).
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
              // A fan-out row stops ITS OWN entry: the siblings that already
              // loaded keep their data and the entries after it still run.
              // Whole-load stops stay with the title-bar strip and shutdown,
              // which latch cancel_mode_ instead.
              auto cancel_handler = [this, fanout_generation, iter_dataset_id, entry_stop](bool keep_partial) {
                if (fanout_generation != load_generation_) {
                  return;  // a stale row from a load that already moved on
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
        // Either scope stops THIS entry; they differ only in what happens after
        // it (see the epilogue's cancel_action).
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
              // A fanout entry that never announced a start has no token, but it
              // does have a dataset, and its committed rows must still publish.
              const auto token_it = fanout_ingest_tokens_.find(iter_dataset_id);
              const IngestToken token = token_it != fanout_ingest_tokens_.end() ? token_it->second : IngestToken{};
              publishIngestProgress(iter_dataset_id, token, current, max);
            },
            Qt::QueuedConnection);
        return true;
      };
      iter_ingest.on_progress_finish = [] {};

      // start() runs off-GUI although the SDK doc-tags it [main-thread]: a
      // finite importer's start() blocks for the whole read loop, which would
      // freeze the UI and deadlock the message-box marshal (it posts to the GUI
      // thread and blocks the caller). Same accepted host-side deviation as the
      // single-instance path's runIngestOnWorker; every other lifecycle slot
      // (create/bind/loadConfig/destroy) stays on the GUI thread.
      Status start_status = okStatus();
      // Published so cancelCurrent/joinForShutdown can requestStop() a
      // progress-silent plugin (one that polls is_stop_requested but rarely
      // reports progress) — the coroutine-frame-local host is otherwise
      // unreachable from the GUI-side cancel paths. GUI-thread writes only;
      // the worker never reads this pointer.
      active_fanout_ingest_ = &iter_ingest;
      {
        // Named local, not a co_await temporary — see dialog_awaiter above
        // (GCC 11.4 awaiter-temporary miscompile).
        GuiResumingWorkerAwaiter start_awaiter(
            this, worker_, [&iter_handle, &start_status]() { start_status = iter_handle.start(); });
        co_await std::move(start_awaiter);
      }
      active_fanout_ingest_ = nullptr;

      // Cancellation takes precedence over the plugin's status: finite
      // importers commonly report a rejected progress update as start failure.
      // Treating that as a recoverable plugin error would incorrectly continue
      // into the next fanout entry.
      // Whole-load scope wins: a strip/shutdown stop ends the fan-out, while a
      // row's stop is consumed here and the loop moves to the next entry.
      const int whole_load_action = cancel_mode_.load();
      const int entry_action = entry_stop->exchange(0);
      const int cancel_action = whole_load_action != 0 ? whole_load_action : entry_action;
      const bool stops_whole_load = whole_load_action != 0;
      EntryOutcome outcome = EntryOutcome::kCompleted;
      if (cancel_action == 2) {
        outcome = EntryOutcome::kDiscarded;
      } else if (cancel_action == 1) {
        iter_ingest.flushAll();
        fanout_loaded_ids.push_back(iter_dataset_id);
        outcome = EntryOutcome::kKept;
      } else if (!start_status) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: start failed:" << QString::fromStdString(start_status.error());
        outcome = EntryOutcome::kFailed;
      } else {
        iter_ingest.flushAll();
        fanout_loaded_ids.push_back(iter_dataset_id);
      }

      const IngestOutcome ingest_outcome =
          outcome == EntryOutcome::kCompleted
              ? IngestOutcome::kCompleted
              : (outcome == EntryOutcome::kFailed ? IngestOutcome::kFailed : IngestOutcome::kCancelled);
      if (const auto token_it = fanout_ingest_tokens_.find(iter_dataset_id); token_it != fanout_ingest_tokens_.end()) {
        const IngestToken token = token_it->second;
        // Erase first so an ingestEnded observer that re-enters shutdown cannot
        // terminalize this token a second time.
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
        // A row discard drops only its own entry — which the cleanup loop below
        // already does, since it never joined fanout_loaded_ids. A WHOLE-load
        // stop is the one that ends the fan-out and, when it discards, drops
        // even the entries that already completed.
        whole_load_stop = whole_load_action;
        break;
      }
    }
    qCInfo(lcFileLoader) << "[FileLoader] fanout complete:" << fanout_loaded_ids.size() << "ok," << failed_labels.size()
                         << "failed" << failed_labels;
    cancel_mode_.store(0);

    // "Remove All" (discard) drops the WHOLE fanout load, not just the in-flight
    // entry: entries that already Completed live in fanout_loaded_ids and would
    // otherwise survive the cancel. Clear the loaded list so (a) the unified
    // cleanup loop below real-deletes every created dataset (completed ones no
    // longer match the keep-list), and (b) the downstream TF-ingest /
    // source-path / time-reference loops skip them. "Stop and Keep" (mode 1)
    // keeps them, so this arm is discard-only. TF is invalidated in the same
    // loop, so nothing extra is needed here.
    if (whole_load_stop == 2) {
      fanout_loaded_ids.clear();
    }

    // GUI-owned cleanup after the worker has stopped touching the datastore.
    // Every created dataset NOT in the keep-list is real-deleted. A
    // completed-then-discarded entry may have ingested FrameTransforms, which
    // removeCreatedDataset invalidates before eviction empties its topic list.
    for (const DatasetId created_id : fanout_created_ids) {
      if (std::find(fanout_loaded_ids.begin(), fanout_loaded_ids.end(), created_id) != fanout_loaded_ids.end()) {
        continue;
      }
      removeCreatedDataset(created_id);
    }
    if (!replacing && created_live_dataset_id != 0) {
      // The initial handle binds against a scratch dataset before the dialog
      // reveals fanout. It never receives rows; remove the shell now that all
      // real fanout datasets have been created — no objects to evict.
      removeCreatedDataset(created_live_dataset_id, /*evict_objects=*/false);
      created_live_dataset_id = 0;
    }
  }

  // The fan-out produced nothing to keep. Leave the tombstones ALONE and take
  // the rollback exits below: the scope guard restores the dataset this load
  // was replacing, which is the whole point of tombstoning it instead of
  // deleting it up front. Real-deleting before these checks would erase the
  // user's existing dataset permanently on a discard or an all-failed fan-out —
  // the two cases where nothing replaced it.
  // What survived is the whole question: the load-wide discard, every entry
  // failing and every row being thrown away one at a time all leave this list
  // empty, and none of them may commit. (extractFanout never yields an empty
  // list — every rejection falls back to a single-instance config — so an empty
  // list here always means entries ran and none of them kept anything.)
  const bool nothing_to_commit = fanout_loaded_ids.empty();
  if (!nothing_to_commit) {
    // Past the last rollback point: the load committed in place (no staging swap).
    // Free the ObjectStore topics, derived TF state, AND scalar engine storage of any
    // datasets the fanout-reload fallback tombstoned. A fanout reload can't refill one
    // dataset into N, so the old dataset is replaced by fresh ones and must be ERASED
    // from the engine — not left as a shell a prefer_reuse layout replay could reattach
    // to (#249's real-delete, fanout face). Deferred to here (not the tombstone site)
    // because a mid-load failure rolls the tombstones back.
    for (const DatasetId tombstoned_id : tombstoned_for_replace) {
      // Already tombstoned above (catalog_.removeDataset(existing_primary_id));
      // skip the redundant catalog call here.
      removeCreatedDataset(tombstoned_id, /*evict_objects=*/true, /*remove_from_catalog=*/false);
    }
    tombstoned_for_replace.clear();
    rollback_armed = false;
    fanout_committed = true;
  }

  catalog_.rebuildFromDatastore();  // reload: same keys ⇒ no spurious itemsRemoved

  // Publish tap-fed fanout buffers after their worker imports finish. When no
  // tap is active, ingestNewTransforms drains the corresponding store entries.
#ifdef PJ_WITH_SCENE3D
  if (transform_service_ != nullptr) {
    for (const DatasetId loaded_id : fanout_loaded_ids) {
      transform_service_->publishTransforms(loaded_id);
    }
  }
#endif

  // Capture the plugin's canonical post-load state AFTER start() + ingest so a
  // layout persists discovered fields / applied defaults / ingest-time policy
  // overrides. saveConfig failures here are non-fatal.
  std::string captured_config;
  if (auto status = handle.saveConfig(captured_config); !status) {
    qCWarning(lcFileLoader).noquote() << tr("Plugin '%1': saveConfig failed: %2 — layout save will skip plugin config")
                                             .arg(source_name, QString::fromStdString(status.error()));
    captured_config.clear();
  }

  // Remember which file each dataset came from so a later load of a DIFFERENT
  // file sharing this basename is not mistaken for a reload of it (the match
  // loop above gates reuse on this). Single-instance: `dataset_id` is the stable
  // id (existing on reload, else the fresh one). Fanout: each dataset that took
  // data.
  if (fanouts.size() == 1) {
    session_.setDatasetSourcePath(dataset_id, source_identity);
  } else {
    for (const DatasetId loaded_id : fanout_loaded_ids) {
      session_.setDatasetSourcePath(loaded_id, source_identity);
    }
  }

  // The synchronous load committed its rows straight to DataEngine via the plugin
  // write host, bypassing SessionManager::commitChunks — so the cross-dataset time
  // origin was never re-evaluated. Refresh it per loaded dataset: a file whose data
  // is EARLIER than any prior dataset must reframe every plot. No-op when "Use time
  // offset" is off.
  if (fanouts.size() == 1) {
    session_.refreshDatasetTimeReference(dataset_id);
  } else {
    for (const DatasetId loaded_id : fanout_loaded_ids) {
      session_.refreshDatasetTimeReference(loaded_id);
    }
  }

  if (nothing_to_commit) {
    // A replacing fan-out restores the dataset it was replacing on the way out
    // (rollback guard, still armed): a load that kept nothing must not destroy
    // the data the user already had.
    rollback_tombstones();
    if (whole_load_stop == 2 || failed_labels.isEmpty()) {
      // "Remove All", or every row discarded by hand: nothing went wrong, the
      // user threw this load away. Surface it as a discarded load, mirroring the
      // single-instance discard — recording it (recents, loadedSources, layout
      // data-source entries) or logging a success would resurrect a source the
      // user explicitly removed.
      emitLoadFinished(ticket, LoadOutcome::kCancelled, source_identity, 0);
      emit fileLoadFailed(source_identity, tr("Import discarded"));
      co_return;
    }
    // Entries failed and nothing survived them: report the aggregate as a
    // failure instead of announcing a dataset-less success.
    const QString reason = tr("All %1 entries of '%2' failed to load:\n%3")
                               .arg(failed_labels.size())
                               .arg(display_name, failed_labels.join(u"\n"_s));
    emitLoadFinished(ticket, LoadOutcome::kFailed, source_identity, 0);
    reportLoadWarning(dialog_parent, reason);
    emit fileLoadFailed(source_identity, reason);
    co_return;
  }

  // "Stop and Keep" ended the fan-out early: the kept data is real (and the
  // legacy fileLoaded below reports it as before), but the request ended by
  // user cancel and the ticket classifies it as such. The primary dataset is
  // the first entry that took data — the id the replace/reuse logic would
  // treat as this source's principal dataset.
  const DatasetId fanout_primary_id = fanout_loaded_ids.empty() ? 0 : fanout_loaded_ids.front();
  emitLoadFinished(
      ticket, whole_load_stop != 0 ? LoadOutcome::kCancelled : LoadOutcome::kLoaded, source_identity, fanout_primary_id,
      QVector<DatasetId>(fanout_loaded_ids.begin(), fanout_loaded_ids.end()));
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  logSuccessfulLoad(engine, catalog_, source_identity, source_name, fanout_loaded_ids);
#endif
  emit fileLoaded(source_identity, QString(), source_name, QString::fromStdString(captured_config), source_manifest_id);
  co_return;  // fanout completed; process the next queued request
}

bool FileLoader::loadFile(const QString& path, QWidget* dialog_parent, const LoadHints& hints) {
  return loadFile(LoadInput::fromNativePath(path), dialog_parent, hints);
}

bool FileLoader::loadFile(LoadInput input, QWidget* dialog_parent, const LoadHints& hints) {
  return loadFileTicketed(std::move(input), dialog_parent, hints) != 0;
}

LoadRequestId FileLoader::loadFileTicketed(LoadInput input, QWidget* dialog_parent, const LoadHints& hints) {
  if (destroying_) {
    // The destructor's final terminal flush is delivering loadFinished right
    // now; an accepted request could never be resolved and its worker would
    // outlive the loader. Rejected per the 0-on-rejection contract — no
    // signals or dialogs from a dying loader.
    qCWarning(lcFileLoader) << "[FileLoader] load request rejected: the loader is being destroyed";
    return 0;
  }
  if (input.display_name.isEmpty() || input.backing_path.isEmpty() || input.source_identity.isEmpty()) {
    // An unusable input (e.g. a trailing-slash directory path whose fileName() is
    // empty) must surface the same user-visible failure as any other load error,
    // not fail silently — MainWindow's layout replay relies on that contract.
    // Rejected means never accepted: no ticket, no loadFinished.
    const QString path = !input.source_identity.isEmpty() ? input.source_identity
                         : !input.backing_path.isEmpty()  ? input.backing_path
                                                          : input.display_name;
    const QString reason = tr("Cannot load '%1': not a readable file.").arg(path);
    qCWarning(lcFileLoader).noquote() << reason;
    reportLoadWarning(dialog_parent, reason);
    emit fileLoadFailed(path, reason);
    return 0;
  }
#ifdef PJ_TARGET_WASM
  if (!input.content_sha256.isEmpty()) {
    browser_content_sha256_.insert(input.source_identity, input.content_sha256);
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
  return ticket;  // accepted/enqueued — completion is async (loadFinished + legacy signals)
}

void FileLoader::startNext() {
  if (active_load_) {
    return;  // a suspended prologue or worker resumes the queue when done
  }
  // Any prior task is now at final_suspend. Destroy it before installing the
  // next frame; finishPrologue always schedules this method rather than calling
  // it recursively from inside that frame.
  begin_load_task_.reset();
  if (queue_.empty()) {
    load_generation_ = 0;  // idle: no load to bind a stop-dialog to
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
  // Old queued callbacks can never match a reused loader.
  load_generation_ = next_load_generation_;
  // Ticket and generation advance TOGETHER, before loadGenerationAdvanced goes
  // out, so a slot reading currentLoadTicket() there sees a consistent pair.
  current_load_ticket_ = request.ticket;
  current_load_identity_ = request.input.source_identity;
  current_ticket_terminal_ = false;
  // Announce the ticket↔generation association first: a consumer tracking a
  // ticket learns its generation before any generation-keyed UI reacts.
  emit requestStarted(current_load_ticket_, load_generation_);
  // Before the coroutine runs: stale-generation UI must be gone before this
  // load can raise its own (application-modal) dialogs.
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
  // WORKER thread. ctx_ is set on the GUI thread before this thread starts and
  // is not mutated by the GUI until after we post onWorkerFinished, so reading
  // it here races nothing. start() blocks until the plugin finishes (or stops
  // cooperatively when on_progress_update returns false).
  auto status = ctx_->handle.start();
  ctx_->start_ok = static_cast<bool>(status);
  if (!status) {
    ctx_->start_error = QString::fromStdString(status.error());
  }
  QMetaObject::invokeMethod(this, [this, generation]() { onWorkerFinished(generation); }, Qt::QueuedConnection);
}

void FileLoader::onWorkerFinished(std::uint64_t generation) {
  if (generation != load_generation_ || !ctx_) {
    return;  // shutdown/reuse already tore down or replaced this load
  }
  if (worker_) {
    worker_->wait();  // the worker posted us as its last act, so this returns promptly
    worker_.reset();
  }

  const int cancel = cancel_mode_.load();
  const DatasetId dataset_id = ctx_->dataset_id;
  const bool replacing = ctx_->replacing;
  const QString path = ctx_->path;
  const QString source_name = ctx_->source_name;
  const quint64 ticket = ctx_->ticket;
  const IngestToken ingest_token = ctx_->ingest_token;
  // Transfer terminal ownership out of LoadContext before any legacy signal
  // can re-enter joinForShutdown. Exactly one path below reports the outcome;
  // shutdown owns the token only while it remains stored in the context.
  ctx_->ingest_token = {};
  IngestOutcome ingest_outcome = IngestOutcome::kCompleted;
  // Terminal classification for the ticket. A user stop (either mode) reports
  // kCancelled even where the wind-down otherwise ends in fileLoaded (keep) or
  // fileLoadFailed (discard, or a stop the plugin surfaced as a failed start).
  // Each arm terminalizes the ticket the moment its outcome is decided —
  // BEFORE its legacy-emitting wind-down runs — so a re-entrant shutdown from
  // a legacy slot cannot resolve the request a second time.
  // Ingest-token exits mirror every worker terminal: discard -> kCancelled;
  // start failure -> kFailed; derived replay failure -> kFailed (or
  // kCancelled after keep-stop); ordinary completion -> kCompleted; and both
  // keep/discard user stops -> kCancelled. joinForShutdown handles the sole
  // bypass of this method.

  if (cancel == 2) {  // Discard
    ingest_outcome = IngestOutcome::kCancelled;
    emitLoadFinished(ticket, LoadOutcome::kCancelled, path, 0);
    // Discard means "as if this load had never run". What that leaves behind
    // depends on what was there BEFORE it: nothing on a first load, the prior
    // data on a reload.
    if (!replacing) {
      qCWarning(lcFileLoader) << "[FileLoader] import discarded by user; the new dataset is removed";
      // Real-delete the abandoned shell: evict its objects, drop catalog items
      // WITHOUT a tombstone, invalidate any TF it ingested, and erase the
      // engine's scalar storage, so a later prefer_reuse layout replay mints a
      // fresh dataset instead of reattaching to an empty one.
      removeCreatedDataset(dataset_id);
      ctx_.reset();
      emit fileLoadFailed(path, tr("Import discarded"));
    } else {
      qCWarning(lcFileLoader) << "[FileLoader] import discarded by user; the dataset keeps its pre-reload data";
      // Roll back to the pre-reload data: the guard dtor reattaches the scalar +
      // object data and retires the topics the abandoned refill added, and
      // abortReplacingLoad drops the TF generation the worker was staging into.
      // Deleting the dataset instead would punish a reload for being a reload —
      // the user discarded THIS load, not the data they already had.
      failReplacingLoad(dataset_id, path, tr("Import discarded"));
    }
  } else if (!ctx_->start_ok && cancel == 0) {  // start() failed (and not a user stop)
    ingest_outcome = IngestOutcome::kFailed;
    emitLoadFinished(ticket, LoadOutcome::kFailed, path, 0);
    const QString reason = tr("Plugin '%1': start failed: %2").arg(source_name, ctx_->start_error);
    qCWarning(lcFileLoader).noquote() << reason;
    if (!replacing) {
      // Real-delete the abandoned first-load shell (no tombstone), invalidating
      // any TF it ingested before failing, then erase its engine storage, so a
      // later prefer_reuse layout replay re-ingests instead of reattaching to
      // the empty dataset a failed start() left behind.
      removeCreatedDataset(dataset_id);
      ctx_.reset();
      emit fileLoadFailed(path, reason);
    } else {
      // Same rollback as Discard: a failed start() on a reload must restore the prior
      // data rather than leave the dataset empty.
      failReplacingLoad(dataset_id, path, reason);
    }
  } else {  // Completed, or Cancel(keep): make the parsed rows visible and finalize
    if (cancel == 1) {
      qCInfo(lcFileLoader) << "[FileLoader] import cancelled by user; keeping the partial load";
    }
    ctx_->ingest->flushAll();
    bool refill_ok = true;
    if (ctx_->refill_guard) {
      // Derived outputs were detached with the raw dataset. Replay them before
      // pruning, while failure can still restore the complete prior snapshot.
      if (const Status replayed = ctx_->refill_guard->recomputeProcessors(); !replayed.has_value()) {
        ingest_outcome = cancel == 0 ? IngestOutcome::kFailed : IngestOutcome::kCancelled;
        // Error terminal — except under a user keep-stop, whose ticket keeps
        // reporting kCancelled (the user DID stop it; the replay error only
        // decided that nothing could be kept).
        emitLoadFinished(ticket, cancel == 0 ? LoadOutcome::kFailed : LoadOutcome::kCancelled, path, 0);
        const QString reason = tr("Plugin '%1': derived-series replay failed: %2")
                                   .arg(source_name, QString::fromStdString(replayed.error()));
        qCWarning(lcFileLoader).noquote() << reason;
        failReplacingLoad(dataset_id, path, reason);
        refill_ok = false;
      } else {
        // On a COMPLETE reload (cancel == 0), retire prior topics the new file no
        // longer has — they stayed empty through the refill (codex #2). Skipped on
        // Cancel-Keep: a topic the partial load never reached is not "vanished". Must
        // run BEFORE commit(), which frees the prior-topic snapshots it reads.
        if (cancel == 0) {
          ctx_->refill_guard->pruneVanishedTopics();
        }
        // COMMIT the refill: both Completed and Cancel-Keep keep the refilled data, so
        // free the prior-data snapshot. Must run BEFORE finishLoadOnGui() — it resets ctx_
        // (destroying the guard), and an uncommitted guard would then silently roll back
        // over the new data.
        ctx_->refill_guard->commit();
      }
    }
    if (refill_ok) {
      ingest_outcome = cancel == 1 ? IngestOutcome::kCancelled : IngestOutcome::kCompleted;
      emitLoadFinished(
          ticket, cancel == 1 ? LoadOutcome::kCancelled : LoadOutcome::kLoaded, path, dataset_id, {dataset_id});
      // emits loadCommitting (unless keep-stopped) + fileLoaded; resets ctx_
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
  // The DATASET comes from the load context, never from the token: the plugin
  // C ABI forwards progress_start and progress_update as independent slots and
  // the host synthesizes no start, so a producer can tick without ever
  // beginning an ingest. Its token is then 0/0 while the load still has a real
  // dataset whose rows are already reader-visible.
  if (token.id != 0) {
    // updateIngest publishes every committed batch before it announces the
    // progress number, so plots and the playback range grow progressively even
    // when a flush appended samples without adding a topic.
    session_.updateIngest(token, current, total);
  } else {
    // No lifecycle to report, but the data must never be held hostage to one:
    // publish the same way updateIngest would.
    const auto ids = session_.dataEngine().listTopics(dataset_id);
    session_.notifyIngest(QVector<TopicId>(ids.begin(), ids.end()), /*live=*/false);
  }
  // Publish the worker-fed TF revision so a progressive scene refreshes. When
  // no tap is active, the same call drains the new store entries first.
#ifdef PJ_WITH_SCENE3D
  if (transform_service_ != nullptr) {
    transform_service_->publishTransforms(dataset_id);
  }
#endif
  emit ingestProgress(static_cast<int>(current), static_cast<int>(total));
}

void FileLoader::removeCreatedDataset(DatasetId dataset_id, bool evict_objects, bool remove_from_catalog) {
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
  session_.removeDataset(dataset_id);
}

// Shared failure exit for a REPLACING reload: destroy the guard (its dtor
// reattaches the prior data snapshot), refresh the catalog/UI to the restored
// state, and report the failure.
void FileLoader::failReplacingLoad(DatasetId dataset_id, const QString& path, const QString& reason) {
  ctx_->refill_guard.reset();
  refreshAfterReplacingRollback(dataset_id);
  ctx_.reset();
  emit fileLoadFailed(path, reason);
}

void FileLoader::refreshAfterReplacingRollback(DatasetId dataset_id) {
  // The RefillGuard restored the scalar + object data. The prior TF generation
  // stayed active throughout, so rollback only discards the private generation.
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
  // Locals, not ctx_ reads, past the loadCommitting emission below: a seam
  // slot may re-enter joinForShutdown, which resets ctx_.
  const DatasetId dataset_id = ctx_->dataset_id;
  const quint64 ticket = ctx_->ticket;
  const bool replacing = ctx_->replacing;

  // A replacing load may have pointed the dataset at a different file; surface
  // the new name in the same catalog rebuild that surfaces the new data.
  if (replacing && !ctx_->commit_display_name.isEmpty()) {
    catalog_.setDatasetDisplayName(dataset_id, ctx_->commit_display_name);
  }

  // Capture the plugin's canonical post-load state for layout persistence.
  std::string captured_config;
  if (auto status = ctx_->handle.saveConfig(captured_config); !status) {
    qCWarning(lcFileLoader).noquote() << tr("Plugin '%1': saveConfig failed: %2 — layout save will skip plugin config")
                                             .arg(ctx_->source_name, QString::fromStdString(status.error()));
    captured_config.clear();
  }
  session_.setDatasetSourcePath(dataset_id, ctx_->path);
  const QString path = ctx_->path;
  const QString source_name = ctx_->source_name;
  const QString source_manifest_id = ctx_->source_manifest_id;

  // (A replacing load's stale provider record was already detached by
  // RefillGuard::commit — structurally, before this seam — so a promotion
  // listener can attach the new content's record below.)

  // Pre-catalog commit seam: dataset, source path, and captured config are all
  // installed; the catalog rebuild that publishes the load follows immediately
  // after, so a promotion service can attach source records atomically with
  // catalog publication. Skipped on a keep-stop — the seam is for loads that
  // finish kLoaded.
  if (fully_loaded) {
    emit loadCommitting(ticket, QVector<DatasetId>{dataset_id}, source_name, QString::fromStdString(captured_config));
  }
  catalog_.rebuildFromDatastore();

  // Publish a completed replacement generation atomically. A fresh load already
  // populated its active buffer on the worker and needs only the ready signal.
#ifdef PJ_WITH_SCENE3D
  if (transform_service_ != nullptr) {
    if (replacing) {
      transform_service_->commitReplacingLoad(dataset_id);
    } else {
      transform_service_->publishTransforms(dataset_id);
    }
  }
#endif

  // The terminal flush (onWorkerFinished) committed the file's rows straight to
  // DataEngine via the plugin write host, bypassing SessionManager::commitChunks —
  // so the cross-dataset time origin was never re-evaluated. Refresh it now: a short
  // file whose data is EARLIER than any prior dataset must reframe every plot to the
  // new origin. No-op when "Use time offset" is off.
  session_.refreshDatasetTimeReference(dataset_id);
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  logSuccessfulLoad(session_.dataEngine(), catalog_, path, source_name, {dataset_id});
#endif
  ctx_.reset();  // drop the handle/host before notifying — the load is complete
#if defined(PJ_WASM_ENABLE_INGRESS_PROBE) && (defined(PJ_WASM_WITH_ROS_PLUGIN) || defined(PJ_WASM_WITH_PROTOBUF_PLUGIN))
  // Exercise the same cold-fetch + parser path used later by the accelerated
  // scene consumers, after the MCAP source/runtime context has been destroyed.
  probeObjectDecoding(session_, session_.objectStore(), dataset_id);
#endif
#ifdef PJ_WASM_ENABLE_MCAP_PROBE_PARSER
  // Resolve only after the source instance and runtime host are gone. Success
  // proves the ObjectStore closure owns both the browser file lease and MCAP's
  // post-import cold-reader state rather than borrowing either load context.
  probeColdObjectFetch(session_.objectStore(), dataset_id);
#endif
  emit fileLoaded(path, QString(), source_name, QString::fromStdString(captured_config), source_manifest_id);
}

void FileLoader::reportLoadWarning(QWidget* parent, const QString& reason) {
  if (parent == nullptr) {
    // Headless / no-UI load: the warning already went to the log. Accumulating
    // it here would resurface it (with an inflated count) in the next PARENTED
    // failure's dialog, since only a dialog ever clears the list.
    return;
  }
  load_warning_lines_ << reason;

  // Reuse the still-open dialog: append this reason and retitle so N stacked
  // failures read as one aggregated report instead of N overlapping boxes.
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
  // App-modal (not window-modal) restores the pre-branch exec() semantics.
  dialog->setWindowModality(Qt::ApplicationModal);
  refresh_body(dialog);
  dialog->addButton(tr("OK"), MessageBox::kPrimaryRole);
  // Drop the aggregation state the moment the user dismisses it (finished
  // fires before WA_DeleteOnClose's deferred deletion), so a failure arriving
  // in that gap opens a fresh dialog instead of appending to the dying one —
  // where the new reason would never be seen.
  QObject::connect(dialog, &QDialog::finished, this, [this]() {
    active_load_warning_.clear();
    load_warning_lines_.clear();
  });
  // show(), not open(): open() force-downgrades ApplicationModal to WindowModal.
  dialog->show();
}

void FileLoader::cancelCurrent(bool keep_partial) {
  if (!active_load_) {
    // No load is processing. Latching cancel_mode_ now would silently kill the
    // NEXT load at its prologue check instead of cancelling anything current.
    return;
  }
  cancel_mode_.store(keep_partial ? 1 : 2);
  if (ctx_ == nullptr) {
    // Fanout has no single-instance LoadContext. The active entry's worker-side
    // progress callback observes cancel_mode_ directly; wake a progress-silent
    // plugin through the documented is_stop_requested channel too.
    if (active_fanout_ingest_ != nullptr) {
      active_fanout_ingest_->requestStop();
    }
    return;
  }
  if (ctx_->ingest) {
    // Flag-only stop: cancelCurrent runs on the GUI thread while the worker may be in a
    // host callback, so writing a reason here would race the worker's last_error_. The
    // cooperative stop only needs the atomic flag; the reason is unused on this path.
    ctx_->ingest->requestStop();
  }
}

void FileLoader::cancelCurrent(std::uint64_t generation, bool keep_partial) {
  if (generation == 0 || generation != load_generation_ || !active_load_) {
    // The load this cancel was raised for already finished (and possibly a next
    // one began): cancelling now would stop the wrong load. The !active_load_
    // arm covers the fan-out epilogue's one-event-loop hop, where the finished
    // load's generation is still current but startNext has not yet advanced it
    // — a click landing there must not poison the queued next load.
    return;
  }
  cancelCurrent(keep_partial);
}

bool FileLoader::cancelLoad(quint64 ticket, bool keep_partial) {
  if (QThread::currentThread() != thread()) {
    // Best-effort degrade for the one entry a batch worker may call off the
    // loader's thread: the real answer requires this object's thread, so the
    // cancel is marshalled and the caller learns the result via loadFinished.
    cancelLoadAsync(ticket, keep_partial);
    return false;
  }
  if (ticket == 0) {
    return false;
  }
  // Still queued: remove the request before it ever starts, and schedule its
  // terminal — nothing else will ever resolve this ticket.
  for (auto it = queue_.begin(); it != queue_.end(); ++it) {
    if (it->ticket == ticket) {
      const QString identity = it->input.source_identity;
      queue_.erase(it);
      emitLoadFinished(ticket, LoadOutcome::kCancelled, identity, 0);
      return true;
    }
  }
  // The active request. current_ticket_terminal_ closes the finished-but-not-
  // yet-advanced event-loop hop: the terminal is already scheduled, so a
  // cancel landing there must not latch cancel_mode_ against the NEXT load. A
  // ticket can never name a LATER load — tickets advance with
  // load_generation_ in startNext — so the generation-guarded delegate always
  // targets the load this ticket was minted for.
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
    QVector<DatasetId> produced_dataset_ids) {
  if (ticket == current_load_ticket_) {
    if (current_ticket_terminal_) {
      // Already resolved — e.g. a re-entrant joinForShutdown ran inside a
      // legacy signal slot after this path terminalized. Exactly-once: first
      // wins. A DIFFERENT outcome here means an exit path broke the
      // terminalize-before-legacy-emission convention — make that loud.
      if (outcome != current_ticket_outcome_) {
        qCWarning(lcFileLoader) << "[FileLoader] conflicting second resolution for ticket" << ticket << ": recorded"
                                << static_cast<int>(current_ticket_outcome_) << ", ignoring"
                                << static_cast<int>(outcome);
      }
      return;
    }
    // Mark terminal BEFORE anything can re-enter: every exit path calls this
    // ahead of its legacy fileLoaded/fileLoadFailed emission, so a slot
    // re-entering cancelLoad/joinForShutdown sees the request as resolved
    // instead of latching cancel_mode_ or resolving it a second time.
    current_ticket_terminal_ = true;
    current_ticket_outcome_ = outcome;
  }
  pending_terminals_.push_back(
      PendingTerminal{ticket, outcome, effective_path, dataset_id, std::move(produced_dataset_ids)});
  // Queued delivery keeps the no-terminal-before-enqueue-returns contract;
  // joinForShutdown flushes the deque synchronously instead, so teardown
  // cannot lose a scheduled terminal.
  QMetaObject::invokeMethod(this, [this]() { deliverPendingTerminal(); }, Qt::QueuedConnection);
}

void FileLoader::deliverPendingTerminal() {
  if (pending_terminals_.empty()) {
    return;  // a shutdown flush ran before this metacall was delivered
  }
  const PendingTerminal terminal = std::move(pending_terminals_.front());
  pending_terminals_.pop_front();
  emit loadFinished(terminal.ticket, terminal.outcome, terminal.effective_path, terminal.dataset_id, terminal.produced);
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
  // Terminal bookkeeping for the requests this shutdown discards: the active
  // load (unless it already resolved) and every queued request. Resolved at
  // the END, once the loader is fully reset and reusable, so a slot reacting
  // to a cancellation can immediately start a fresh load.
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
  // Invalidate every queued callback before waiting. next_load_generation_
  // remains untouched, so a subsequent load cannot reuse this token.
  load_generation_ = 0;
  cancel_mode_.store(2);  // discard whatever is mid-flight
  if (active_fanout_ingest_ != nullptr) {
    // A fan-out entry's host lives in the coroutine frame; without this nudge a
    // plugin that polls is_stop_requested but never reports progress would keep
    // the worker (and the wait below) running to natural completion.
    active_fanout_ingest_->requestStop();
  }
  // Unblock a worker parked in the synchronous message-box ABI BEFORE waiting
  // on it: a pending question answers -1, and a request still queued on the GUI
  // thread (i.e. behind this very call) becomes a no-op when it eventually runs
  // — without this, wait() below deadlocks against the unserved queue entry.
  message_gate_->shutdown();
  if (!active_plugin_message_dialog_->isNull()) {
    // finished is delivered synchronously on this GUI thread; its completion is
    // release-once, so the gate release above cannot double-answer the worker.
    (*active_plugin_message_dialog_)->reject();
    active_plugin_message_dialog_->clear();
  }
  if (ctx_ != nullptr && ctx_->ingest) {
    ctx_->ingest->requestStop();  // flag-only: worker may be mid-ingest, avoid racing last_error_
  }
  if (worker_) {
    worker_->wait();
    worker_.reset();
  }
  // onWorkerFinished / the fan-out continuation will not run after this
  // shutdown invalidated their generation and joined the worker. Close every
  // token they would otherwise have terminalized. Detach the tokens from
  // loader state first so an ingestEnded observer that re-enters shutdown sees
  // nothing left to end a second time.
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
  // A fanout worker captures locals in this coroutine frame, so cancel it only
  // after the worker has joined. Destroying a prologue suspended on its plugin
  // config dialog synchronously closes that dialog and rejects the plugin while
  // the frame's DataSourceHandle still owns a live plugin context (the
  // DataSourceDialogAwaiter destructor does both), so no tick or late resume
  // can ever touch a freed ctx.
  begin_load_task_.reset();
  // The worker has joined; the queued onWorkerFinished will no-op once ctx_ is reset, so
  // run the abandoned-load cleanup here. Capture the reload identity BEFORE ctx_.reset()
  // destroys the guard. A discarded mid-flight NON-replacing first load created a dataset
  // in the live engine — erase it (objects + TF state + catalog without a tombstone +
  // engine storage) so a later prefer_reuse layout replay re-ingests instead of
  // reattaching to the empty shell (matches onWorkerFinished's discard path). A REPLACING
  // reload instead rolls back via the guard's destructor when ctx_ is reset, restoring the
  // pre-reload data.
  const bool was_replacing = (ctx_ != nullptr && ctx_->replacing);
  const DatasetId reload_id = (ctx_ != nullptr) ? ctx_->dataset_id : 0;
  if (ctx_ != nullptr && !was_replacing) {
    removeCreatedDataset(reload_id);
  }
  ctx_.reset();  // guard dtor rolls back a replacing reload; any queued onWorkerFinished no-ops
  if (was_replacing) {
    refreshAfterReplacingRollback(reload_id);  // reflect the restored data in catalog + TF
  }
  queue_.clear();
  active_load_ = false;
  current_load_ticket_ = 0;
  current_load_identity_.clear();
  current_ticket_terminal_ = false;
  // Clear the shutdown's discard flag so the NEXT load's prologue does not
  // mistake it for a user cancel issued while suspended (the V12a check reads
  // cancel_mode_ at prologue resume).
  cancel_mode_.store(0);
  // Leave the loader reusable (a fresh load after shutdown must work): mint a
  // new gate — the shut one keeps neutering any straggler queued message-box
  // lambdas — and re-enable prologue completion.
  message_gate_ = std::make_shared<PluginMessageGate>();
  shutting_down_ = false;
  // Every discarded request still resolves its ticket, in FIFO order.
  for (const CancelledRequest& cancelled : cancelled_requests) {
    emitLoadFinished(cancelled.ticket, LoadOutcome::kCancelled, cancelled.identity, 0);
  }
  // The deliberate exception to queued terminal delivery: shutdown (and so
  // destruction) must not lose terminals, so everything still pending — the
  // cancellations above plus any terminal resolved earlier but not yet
  // delivered — goes out NOW, before this call returns. The loader is fully
  // reset, so re-entrancy from these slots is contained.
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
