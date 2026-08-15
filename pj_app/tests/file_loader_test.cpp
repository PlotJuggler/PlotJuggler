// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// End-to-end tests for FileLoader. Single-instance loads run progressively on a
// worker thread (loadFile enqueues and returns; completion is async via
// fileLoaded/fileLoadFailed), and a same-file reload REPLACES the dataset's data
// in place (stable DatasetId/TopicIds, no duplicate topics, no re-append) via
// beginRefill's detach + the write-host's by-name topic reuse — NOT a staging
// swap. Drives the real plugin pipeline headlessly: the SDK's
// mock_file_source_plugin (claims ".mock", writes topic "mock/file_data" with
// 3 rows at t=100/200/300) loaded through ExtensionCatalogService, with
// skip-dialog LoadHints standing in for the data-source dialog.

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QMutex>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "FileLoader.h"
#include "pj_base/sdk/data_source_plugin_base.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_plugins/sdk/object_ingest_policy.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/MessageBox.h"
#include "support/loader_test_support.h"
using namespace Qt::StringLiterals;

#ifndef PJ_MOCK_FILE_SOURCE_PLUGIN_PATH
#error "PJ_MOCK_FILE_SOURCE_PLUGIN_PATH must be defined"
#endif

#ifndef PJ_MSGBOX_MOCK_SOURCE_PLUGIN_PATH
#error "PJ_MSGBOX_MOCK_SOURCE_PLUGIN_PATH must be defined"
#endif

#ifndef PJ_DIALOG_PROBE_SOURCE_PLUGIN_PATH
#error "PJ_DIALOG_PROBE_SOURCE_PLUGIN_PATH must be defined"
#endif

namespace {

// Read a probe file's newline-separated markers (see dialog_probe_source.cpp /
// msgbox_mock_source.cpp), skipping blank lines. Missing file → empty vector.
std::vector<std::string> readProbeLines(const QString& path) {
  std::vector<std::string> out;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return out;
  }
  while (!file.atEnd()) {
    const QByteArray line = file.readLine().trimmed();
    if (!line.isEmpty()) {
      out.emplace_back(line.constData(), static_cast<size_t>(line.size()));
    }
  }
  return out;
}

// Reuse the real mock source implementation behind a second catalog identity.
// The copied vtable has static storage because registerStaticDataSource keeps a
// non-owning pointer to it for the catalog lifetime.
const PJ_data_source_vtable_t* alternateMockSourceVtable(const PJ_data_source_vtable_t* source) {
  static PJ_data_source_vtable_t alternate;
  alternate = *source;
  alternate.manifest_json =
      R"({"id":"alternate-mock-file-source","name":"Alternate Mock File Source","version":"1.0.0","file_extensions":[".mock"]})";
  return &alternate;
}

// Decoy whose MANIFEST ID equals the real mock source's DISPLAY NAME. With the
// tiered matcher (manifest id across ALL candidates first, display name only
// as a fallback), an expected id of "Mock File Source" must select THIS plugin
// by its manifest id even though the real mock — an earlier extension match —
// carries that exact string as its display name.
const PJ_data_source_vtable_t* decoyWithIdEqualToMockDisplayName(const PJ_data_source_vtable_t* source) {
  static PJ_data_source_vtable_t decoy;
  decoy = *source;
  decoy.manifest_json =
      R"({"id":"Mock File Source","name":"Manifest Tier Decoy","version":"1.0.0","file_extensions":[".mock"]})";
  return &decoy;
}

// In-process DataSource used to put fanout cancellation at an exact point:
// entry 1 has completed, entry 2 has appended one row, and entry 3 has not
// started. Keeping the probe in this executable makes the GUI/worker handoff a
// condition-variable rendezvous instead of a timing-dependent plugin delay.
struct FanoutProbeCall {
  std::string slot;
  std::string suffix;
  std::thread::id thread;
};

struct FanoutProbeState {
  std::mutex mutex;
  std::condition_variable release_cv;
  bool release_cancel_entry = false;
  int released_progress_step = 0;
  std::thread::id main_thread;
  std::vector<FanoutProbeCall> calls;
};

FanoutProbeState& fanoutProbeState() {
  static FanoutProbeState state;
  return state;
}

void resetFanoutProbe() {
  FanoutProbeState& state = fanoutProbeState();
  const std::lock_guard lock(state.mutex);
  state.release_cancel_entry = false;
  state.released_progress_step = 0;
  state.main_thread = std::this_thread::get_id();
  state.calls.clear();
}

void recordFanoutProbeCall(std::string slot, std::string suffix = {}) {
  FanoutProbeState& state = fanoutProbeState();
  const std::lock_guard lock(state.mutex);
  state.calls.push_back(
      FanoutProbeCall{.slot = std::move(slot), .suffix = std::move(suffix), .thread = std::this_thread::get_id()});
}

void releaseFanoutProbeCancelEntry() {
  FanoutProbeState& state = fanoutProbeState();
  {
    const std::lock_guard lock(state.mutex);
    state.release_cancel_entry = true;
  }
  state.release_cv.notify_all();
}

// CONSUME-ONCE: the release is taken by the entry that observes it, so a fan-out
// with several "cancel" entries parks at each one instead of running the rest
// past a latch a previous entry already opened.
bool waitForFanoutProbeCancelRelease() {
  FanoutProbeState& state = fanoutProbeState();
  std::unique_lock lock(state.mutex);
  if (!state.release_cv.wait_for(lock, std::chrono::seconds(5), [&state]() { return state.release_cancel_entry; })) {
    return false;
  }
  state.release_cancel_entry = false;
  return true;
}

void releaseFanoutProbeProgressStep(int step) {
  FanoutProbeState& state = fanoutProbeState();
  {
    const std::lock_guard lock(state.mutex);
    state.released_progress_step = std::max(state.released_progress_step, step);
  }
  state.release_cv.notify_all();
}

bool waitForFanoutProbeProgressStep(int step) {
  FanoutProbeState& state = fanoutProbeState();
  std::unique_lock lock(state.mutex);
  return state.release_cv.wait_for(
      lock, std::chrono::seconds(5), [&state, step]() { return state.released_progress_step >= step; });
}

std::vector<std::string> fanoutProbeStartedSuffixes() {
  FanoutProbeState& state = fanoutProbeState();
  const std::lock_guard lock(state.mutex);
  std::vector<std::string> out;
  for (const FanoutProbeCall& call : state.calls) {
    if (call.slot == "start") {
      out.push_back(call.suffix);
    }
  }
  return out;
}

std::vector<std::string> fanoutProbeControlCallsOffMain() {
  FanoutProbeState& state = fanoutProbeState();
  const std::lock_guard lock(state.mutex);
  std::vector<std::string> out;
  for (const FanoutProbeCall& call : state.calls) {
    // Finite-import start() is a separate ABI decision: its current blocking
    // shape cannot simply move to the GUI thread. This assertion covers the
    // newly shifted control slots that the published SDK unambiguously marks
    // main-thread; start's contract must be reconciled independently.
    if (call.slot == "start" || call.thread == state.main_thread) {
      continue;
    }
    out.push_back(call.suffix.empty() ? call.slot : call.slot + ':' + call.suffix);
  }
  return out;
}

std::string joinFanoutProbeCalls(const std::vector<std::string>& calls) {
  std::string out;
  for (const std::string& call : calls) {
    if (!out.empty()) {
      out += ", ";
    }
    out += call;
  }
  return out;
}

std::string fanoutProbeSuffix(std::string_view config) {
  const QJsonDocument document =
      QJsonDocument::fromJson(QByteArray(config.data(), static_cast<qsizetype>(config.size())));
  return document.isObject() ? document.object().value(u"display_suffix"_s).toString().toStdString() : std::string{};
}

class FanoutProbeSource final : public PJ::DataSourcePluginBase {
 public:
  FanoutProbeSource() {
    recordFanoutProbeCall("create");
  }

  ~FanoutProbeSource() override {
    recordFanoutProbeCall("destroy", suffix_);
  }

  uint64_t capabilities() const override {
    recordFanoutProbeCall("capabilities", suffix_);
    return PJ::kCapabilityFiniteImport | PJ::kCapabilityDirectIngest;
  }

  PJ::Status bind(PJ::sdk::ServiceRegistry services) override {
    recordFanoutProbeCall("bind", suffix_);
    return PJ::DataSourcePluginBase::bind(services);
  }

  std::string saveConfig() const override {
    recordFanoutProbeCall("save_config", suffix_);
    return config_;
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    config_.assign(config_json);
    suffix_ = fanoutProbeSuffix(config_json);
    recordFanoutProbeCall("load_config", suffix_);
    return PJ::okStatus();
  }

  PJ::Status start() override {
    recordFanoutProbeCall("start", suffix_);
    state_ = PJ::DataSourceState::kStarting;
    runtimeHost().notifyState(state_);

    const auto fail = [this](std::string reason) -> PJ::Status {
      runtimeHost().progressFinish();
      state_ = PJ::DataSourceState::kFailed;
      runtimeHost().notifyState(state_);
      return PJ::unexpected(std::move(reason));
    };

    // "nostart" never announces a start: the plugin ABI's progress_start and
    // progress_update are independent slots and the host synthesizes no start,
    // so this is a real producer shape the host must survive.
    const std::string title = "fanout-probe:" + suffix_;
    if (suffix_ != "nostart") {
      if (auto status = runtimeHost().progressStart(title, 3, true); !status) {
        return fail(status.error());
      }
    }

    // Deterministic terminal-path seam: unlike the mock source's fail_start
    // option, this failure happens after progressStart, so FileLoader has
    // already minted an ingest token that must be ended with kFailed.
    if (suffix_ == "fail") {
      return fail("configured failure after progress start");
    }

    auto topic = writeHost().ensureTopic("fanout_probe/value");
    if (!topic) {
      return fail(topic.error());
    }
    const auto append = [this, &topic](uint64_t index) -> PJ::Status {
      return writeHost().appendRecord(
          *topic, PJ::Timestamp{static_cast<int64_t>(index * 100)},
          {{.name = "value", .value = static_cast<double>(index)}});
    };

    if (suffix_ == "cancel") {
      if (auto status = append(1); !status) {
        return fail(status.error());
      }
      if (!waitForFanoutProbeCancelRelease()) {
        return fail("timed out waiting for the GUI cancellation rendezvous");
      }
      // Match real finite-import plugins: cancellation is reported as a failed
      // start after the host's progress callback returns false. FileLoader must
      // inspect its cancellation mode before classifying that status as a
      // normal plugin failure.
      if (!runtimeHost().progressUpdate(1)) {
        return fail("cancelled via progress");
      }
      return fail("the host did not deliver the requested cancellation");
    }

    for (uint64_t index = 1; index <= 3; ++index) {
      if ((suffix_ == "progressive" || suffix_ == "nostart") &&
          !waitForFanoutProbeProgressStep(static_cast<int>(index))) {
        return fail("timed out waiting for the progressive flush rendezvous");
      }
      if (auto status = append(index); !status) {
        return fail(status.error());
      }
      if (!runtimeHost().progressUpdate(index)) {
        return fail("cancelled via progress");
      }
    }

    runtimeHost().progressFinish();
    state_ = PJ::DataSourceState::kStopped;
    runtimeHost().notifyState(state_);
    runtimeHost().requestStop(PJ::DataSourceState::kStopped, "import complete");
    return PJ::okStatus();
  }

  void stop() override {
    recordFanoutProbeCall("stop", suffix_);
    state_ = PJ::DataSourceState::kStopped;
  }

  PJ::DataSourceState currentState() const override {
    return state_;
  }

 private:
  std::string config_ = "{}";
  std::string suffix_;
  PJ::DataSourceState state_ = PJ::DataSourceState::kIdle;
};

// One static vtable per in-process test source: registerStaticDataSource keeps
// a non-owning pointer for the catalog lifetime, and the create hook must be a
// capture-less noexcept factory. One manifest per Source type — the first
// call's manifest wins (each wrapper below passes a single literal).
template <typename Source>
const PJ_data_source_vtable_t* staticSourceVtable(const char* manifest_json) {
  static const PJ_data_source_vtable_t* vtable = PJ::DataSourcePluginBase::vtableWithCreate(
      []() noexcept -> void* {
        try {
          return new Source();
        } catch (...) {
          return nullptr;
        }
      },
      manifest_json);
  return vtable;
}

const PJ_data_source_vtable_t* fanoutProbeVtable() {
  return staticSourceVtable<FanoutProbeSource>(
      R"({"id":"fanout-probe-source","name":"Fanout Probe Source","version":"1.0.0",)"
      R"("file_extensions":[".fanoutprobe"]})");
}

// Review finding 4: a source whose loadConfig REJECTS one specific preset
// (marker "poison") but accepts everything else. Distinguishes the strict
// DialogPolicy::kNever contract (rejected preset -> the load FAILS) from the
// interactive fallback (rejected preset -> silently retry with the QSettings
// pre-fill, which this plugin accepts, and the load would SUCCEED).
class RejectingPresetSource final : public PJ::DataSourcePluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kCapabilityFiniteImport | PJ::kCapabilityDirectIngest;
  }

  std::string saveConfig() const override {
    return config_;
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (config_json.find("poison") != std::string_view::npos) {
      return PJ::unexpected("poisoned preset rejected");
    }
    config_.assign(config_json);
    return PJ::okStatus();
  }

  PJ::Status start() override {
    state_ = PJ::DataSourceState::kStarting;
    auto topic = writeHost().ensureTopic("cfg/value");
    if (!topic) {
      return PJ::unexpected(topic.error());
    }
    if (auto status = writeHost().appendRecord(*topic, PJ::Timestamp{100}, {{.name = "value", .value = 1.0}});
        !status) {
      return PJ::unexpected(status.error());
    }
    state_ = PJ::DataSourceState::kStopped;
    runtimeHost().requestStop(PJ::DataSourceState::kStopped, "import complete");
    return PJ::okStatus();
  }

  void stop() override {
    state_ = PJ::DataSourceState::kStopped;
  }

  PJ::DataSourceState currentState() const override {
    return state_;
  }

 private:
  std::string config_ = "{}";
  PJ::DataSourceState state_ = PJ::DataSourceState::kIdle;
};

const PJ_data_source_vtable_t* rejectingPresetVtable() {
  return staticSourceVtable<RejectingPresetSource>(
      R"({"id":"rejecting-preset-source","name":"Rejecting Preset Source","version":"1.0.0",)"
      R"("file_extensions":[".cfgreject"]})");
}

// In-process DataSource whose CONTENT comes from its config, and which can park
// the worker once it has written (and flushed) that content. Telling a
// transactional rollback from a commit needs both: a replacement that differs
// from what it replaced — identical rows read the same either way — and a
// shutdown that lands while the replacement is genuinely mid-flight.
struct ReloadProbeState {
  std::mutex mutex;
  bool parked = false;
};

ReloadProbeState& reloadProbeState() {
  static ReloadProbeState state;
  return state;
}

void resetReloadProbe() {
  ReloadProbeState& state = reloadProbeState();
  const std::lock_guard lock(state.mutex);
  state.parked = false;
}

// True once the probe has written its rows and is waiting for a stop request.
bool reloadProbeParked() {
  ReloadProbeState& state = reloadProbeState();
  const std::lock_guard lock(state.mutex);
  return state.parked;
}

class ReloadProbeSource final : public PJ::DataSourcePluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kCapabilityFiniteImport | PJ::kCapabilityDirectIngest;
  }

  std::string saveConfig() const override {
    return config_;
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    config_.assign(config_json);
    const QJsonObject object =
        QJsonDocument::fromJson(QByteArray(config_json.data(), static_cast<qsizetype>(config_json.size()))).object();
    first_value_ = object.value(u"first_value"_s).toDouble(10.0);
    rows_ = object.value(u"rows"_s).toInt(3);
    park_ = object.value(u"park"_s).toBool(false);
    return PJ::okStatus();
  }

  PJ::Status start() override {
    state_ = PJ::DataSourceState::kStarting;
    runtimeHost().notifyState(state_);

    const auto fail = [this](std::string reason) -> PJ::Status {
      runtimeHost().progressFinish();
      state_ = PJ::DataSourceState::kFailed;
      runtimeHost().notifyState(state_);
      return PJ::unexpected(std::move(reason));
    };

    if (auto status = runtimeHost().progressStart("reload-probe", static_cast<uint64_t>(rows_), true); !status) {
      return fail(status.error());
    }
    auto topic = writeHost().ensureTopic("reload_probe/value");
    if (!topic) {
      return fail(topic.error());
    }

    for (int index = 1; index <= rows_; ++index) {
      auto status = writeHost().appendRecord(
          *topic, PJ::Timestamp{static_cast<int64_t>(index) * 100},
          {{.name = "value", .value = first_value_ + static_cast<double>(index)}});
      if (!status) {
        return fail(status.error());
      }
      if (park_ && index == rows_) {
        // FileLoader throttles worker-side flushes (50 ms). Sleep past it so the
        // progressUpdate below really commits these rows into the live engine —
        // the partial replacement the shutdown then has to undo.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
      }
      if (!runtimeHost().progressUpdate(static_cast<uint64_t>(index))) {
        return fail("cancelled via progress");
      }
    }

    if (park_) {
      {
        ReloadProbeState& state = reloadProbeState();
        const std::lock_guard lock(state.mutex);
        state.parked = true;
      }
      // Hold the worker on the real running-worker path: joinForShutdown's
      // requestStop() is what releases it.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!runtimeHost().isStopRequested() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!runtimeHost().isStopRequested()) {
        return fail("timed out waiting for the shutdown stop request");
      }
      return fail("stopped while parked");
    }

    runtimeHost().progressFinish();
    state_ = PJ::DataSourceState::kStopped;
    runtimeHost().notifyState(state_);
    runtimeHost().requestStop(PJ::DataSourceState::kStopped, "import complete");
    return PJ::okStatus();
  }

  void stop() override {
    state_ = PJ::DataSourceState::kStopped;
  }

  PJ::DataSourceState currentState() const override {
    return state_;
  }

 private:
  std::string config_ = "{}";
  double first_value_ = 10.0;
  int rows_ = 3;
  bool park_ = false;
  PJ::DataSourceState state_ = PJ::DataSourceState::kIdle;
};

const PJ_data_source_vtable_t* reloadProbeVtable() {
  return staticSourceVtable<ReloadProbeSource>(
      R"({"id":"reload-probe-source","name":"Reload Probe Source","version":"1.0.0",)"
      R"("file_extensions":[".reloadprobe"]})");
}

// Records every loadFinished emission so tests can assert exactly-once and the
// terminal payload (outcome / effective path / dataset ids) per ticket.
struct LoadFinishedRecorder {
  struct Event {
    quint64 ticket;
    PJ::LoadOutcome outcome;
    QString path;
    PJ::DatasetId dataset_id;
    QVector<PJ::DatasetId> produced;
  };

  explicit LoadFinishedRecorder(PJ::FileLoader& loader) {
    connection = QObject::connect(
        &loader, &PJ::FileLoader::loadFinished, &loader,
        [this](
            quint64 ticket, PJ::LoadOutcome outcome, const QString& path, PJ::DatasetId dataset_id,
            const QVector<PJ::DatasetId>& produced) {
          events.push_back(
              Event{
                  .ticket = ticket, .outcome = outcome, .path = path, .dataset_id = dataset_id, .produced = produced});
        });
  }
  ~LoadFinishedRecorder() {
    QObject::disconnect(connection);
  }
  LoadFinishedRecorder(const LoadFinishedRecorder&) = delete;
  LoadFinishedRecorder& operator=(const LoadFinishedRecorder&) = delete;

  [[nodiscard]] std::size_t countForTicket(quint64 ticket) const {
    return static_cast<std::size_t>(
        std::count_if(events.begin(), events.end(), [ticket](const Event& e) { return e.ticket == ticket; }));
  }

  std::vector<Event> events;
  QMetaObject::Connection connection;
};

// Records SessionManager's token-scoped ingest lifecycle. FileLoader's
// progress callbacks cross from a worker to the GUI thread, so the session
// signals are the authoritative surface for pairing begin/update/end.
struct IngestLifecycleRecorder {
  struct Began {
    PJ::IngestToken token;
    QString label;
    quint64 total;
    bool cancellable;
  };

  struct Progressed {
    PJ::IngestToken token;
    quint64 current;
    quint64 total;
  };

  struct Ended {
    PJ::IngestToken token;
    PJ::IngestOutcome outcome;
  };

  explicit IngestLifecycleRecorder(PJ::SessionManager& session) : session_(session) {
    connections_.push_back(
        QObject::connect(
            &session_, &PJ::SessionManager::ingestBegan, &session_,
            [this](PJ::IngestToken token, const QString& label, quint64 total) {
              bool cancellable = false;
              const auto active = session_.activeIngests();
              if (const auto it = active.find(token.dataset_id);
                  it != active.end() && it->second.token.id == token.id) {
                cancellable = it->second.cancellable;
              }
              began.push_back(Began{token, label, total, cancellable});
            }));
    connections_.push_back(
        QObject::connect(
            &session_, &PJ::SessionManager::ingestProgressed, &session_,
            [this](PJ::IngestToken token, quint64 current, quint64 total) {
              progressed.push_back(Progressed{token, current, total});
            }));
    connections_.push_back(
        QObject::connect(
            &session_, &PJ::SessionManager::ingestEnded, &session_,
            [this](PJ::IngestToken token, PJ::IngestOutcome outcome) { ended.push_back(Ended{token, outcome}); }));
  }

  ~IngestLifecycleRecorder() {
    for (const QMetaObject::Connection& connection : connections_) {
      QObject::disconnect(connection);
    }
  }

  IngestLifecycleRecorder(const IngestLifecycleRecorder&) = delete;
  IngestLifecycleRecorder& operator=(const IngestLifecycleRecorder&) = delete;

  [[nodiscard]] const Ended* endFor(PJ::IngestToken token) const {
    const auto it = std::find_if(ended.begin(), ended.end(), [token](const Ended& event) {
      return event.token.id == token.id && event.token.dataset_id == token.dataset_id;
    });
    return it == ended.end() ? nullptr : &*it;
  }

  [[nodiscard]] const Began* beginForLabel(const QString& label) const {
    const auto it =
        std::find_if(began.begin(), began.end(), [&label](const Began& event) { return event.label == label; });
    return it == began.end() ? nullptr : &*it;
  }

  [[nodiscard]] std::size_t beginCountForDataset(PJ::DatasetId dataset_id) const {
    return static_cast<std::size_t>(std::count_if(
        began.begin(), began.end(), [dataset_id](const Began& event) { return event.token.dataset_id == dataset_id; }));
  }

  [[nodiscard]] std::size_t endCountForDataset(PJ::DatasetId dataset_id) const {
    return static_cast<std::size_t>(std::count_if(
        ended.begin(), ended.end(), [dataset_id](const Ended& event) { return event.token.dataset_id == dataset_id; }));
  }

  PJ::SessionManager& session_;
  std::vector<Began> began;
  std::vector<Progressed> progressed;
  std::vector<Ended> ended;
  std::vector<QMetaObject::Connection> connections_;
};

class FileLoaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(data_dir_.isValid());

    // Stage a copy of the plugin instead of pointing at the build dir: the SDK
    // builds deliberately-broken sibling test plugins next to this one, and
    // they would pollute the catalog with load-error diagnostics.
    const QString plugin_src = QString::fromUtf8(PJ_MOCK_FILE_SOURCE_PLUGIN_PATH);
    const QString plugin_dst = extensions_dir_.filePath(QFileInfo(plugin_src).fileName());
    ASSERT_TRUE(QFile::copy(plugin_src, plugin_dst)) << "could not stage " << plugin_src.toStdString();

    // AppSession bundles SessionManager/CatalogModel/ExtensionCatalogService
    // with the destruction order the plugin handles require (session-held
    // parser handles die before the plugin libraries unload).
    app_session_ = std::make_unique<PJ::AppSession>(extensions_dir_.path());
    ASSERT_FALSE(app_session_->extensionCatalog().findSourcesForExtension(u".mock"_s).empty())
        << "mock_file_source_plugin did not load from the staged extensions dir";

    loader_ = std::make_unique<PJ::FileLoader>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), app_session_->catalogModel());

    // The mock source never reads the file; only the path/extension matter.
    mock_path_ = makeMockFile(u"sensors.mock"_s);
  }

  [[nodiscard]] PJ::SessionManager& session() {
    return app_session_->sessionManager();
  }
  [[nodiscard]] PJ::CatalogModel& catalog() {
    return app_session_->catalogModel();
  }

  [[nodiscard]] bool installFanoutProbe() {
    resetFanoutProbe();
    return app_session_->extensionCatalog().pluginCatalog().registerStaticDataSource(fanoutProbeVtable());
  }

  [[nodiscard]] PJ::LoadHints fanoutProbeHints(const QString& config) {
    PJ::LoadHints hints;
    hints.expected_plugin_id = u"Fanout Probe Source"_s;
    hints.preset_config_json = config;
    hints.dialog_policy = PJ::DialogPolicy::kPreferPreset;
    hints.require_expected_plugin = true;
    return hints;
  }

  [[nodiscard]] bool installReloadProbe() {
    resetReloadProbe();
    return app_session_->extensionCatalog().pluginCatalog().registerStaticDataSource(reloadProbeVtable());
  }

  [[nodiscard]] PJ::LoadHints reloadProbeHints(const QString& config) {
    PJ::LoadHints hints;
    hints.expected_plugin_id = u"Reload Probe Source"_s;
    hints.preset_config_json = config;
    hints.dialog_policy = PJ::DialogPolicy::kPreferPreset;
    hints.require_expected_plugin = true;
    return hints;
  }

  [[nodiscard]] QString makeMockFile(const QString& name) {
    return pj_app_test::makeMockFile(data_dir_, name);
  }

  [[nodiscard]] PJ::LoadHints loadHints(const QString& config = u"{}"_s, bool prefer_reuse = false) {
    return pj_app_test::mockLoadHints(config, prefer_reuse);
  }

  // Back-compat alias used by the progressive-load tests.
  PJ::LoadHints skipDialogHints(bool prefer_reuse = false) {
    return loadHints(u"{}"_s, prefer_reuse);
  }

  [[nodiscard]] bool loadAndWait(const QString& path, const PJ::LoadHints& hints) {
    return pj_app_test::loadAndWait(*loader_, path, hints);
  }

  [[nodiscard]] bool load(const QString& path) {
    return loadAndWait(path, loadHints());
  }

  [[nodiscard]] bool loadWithConfig(const QString& path, const QString& config) {
    return loadAndWait(path, loadHints(config));
  }

  [[nodiscard]] bool load() {
    return load(mock_path_);
  }

  // The dataset whose engine-side source_name matches `basename`, or 0.
  [[nodiscard]] PJ::DatasetId datasetNamed(const std::string& basename) {
    for (const PJ::DatasetId id : session().createReader().listDatasets()) {
      const PJ::DatasetInfo* info = session().dataEngine().getDataset(id);
      if (info != nullptr && info->source_name == basename) {
        return id;
      }
    }
    return 0;
  }

  // The catalog's user-facing name for `dataset_id` (display override applied),
  // or empty when the catalog does not list it.
  [[nodiscard]] QString datasetDisplayName(PJ::DatasetId dataset_id) {
    for (const auto& [id, name] : catalog().datasets()) {
      if (id == dataset_id) {
        return name;
      }
    }
    return {};
  }

  // The values of the dataset's single topic, in time order. Empty when the
  // dataset does not have exactly one topic. Row COUNT alone cannot tell one
  // load's data from another's; the values can.
  [[nodiscard]] std::vector<double> singleTopicValues(PJ::DatasetId dataset_id) {
    const PJ::DataReader reader = session().createReader();
    const auto topics = reader.listTopics(dataset_id);
    if (topics.size() != 1u) {
      return {};
    }
    const auto series = reader.series(topics.front(), 0);
    if (!series.has_value()) {
      return {};
    }
    std::vector<double> values;
    values.reserve(series->size());
    for (std::size_t index = 0; index < series->size(); ++index) {
      if (const auto sample = series->sampleAt(index); sample.has_value()) {
        values.push_back(sample->value);
      }
    }
    return values;
  }

  // Row count of the dataset's single topic, or -1 when the topic set is not
  // exactly {mock/file_data} (wiped or duplicated).
  [[nodiscard]] int64_t singleTopicRowCount(PJ::DatasetId dataset_id) {
    const PJ::DataReader reader = session().createReader();
    const auto topics = reader.listTopics(dataset_id);
    if (topics.size() != 1u) {
      return -1;
    }
    const auto metadata = reader.getMetadata(topics.front());
    return metadata.has_value() ? static_cast<int64_t>(metadata->total_row_count) : -1;
  }

  [[nodiscard]] bool engineHasDataset(PJ::DatasetId dataset_id) {
    const auto ids = session().createReader().listDatasets();
    return std::find(ids.begin(), ids.end(), dataset_id) != ids.end();
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir data_dir_;
  std::unique_ptr<PJ::AppSession> app_session_;
  std::unique_ptr<PJ::FileLoader> loader_;
  QString mock_path_;
};

TEST_F(FileLoaderTest, ReloadingSameFileReplacesDatasetInPlace) {
  ASSERT_TRUE(load());

  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  ASSERT_EQ(singleTopicRowCount(dataset_id), 3);
  EXPECT_EQ(catalog().items().size(), 1u);

  // Second load of the same file: replace in place — same DatasetId, same
  // topic, same row count. The pre-fix wiring ingested into the LIVE engine
  // (appending duplicate rows) and then swapped from the EMPTY staged engine
  // (retiring every topic); the row-count check catches both regressions.
  ASSERT_TRUE(load());

  EXPECT_EQ(session().createReader().listDatasets().size(), 1u) << "reload must not mint a second dataset";
  EXPECT_EQ(datasetNamed("sensors.mock"), dataset_id) << "reload must keep the DatasetId stable";
  EXPECT_EQ(singleTopicRowCount(dataset_id), 3) << "reload wiped, duplicated, or re-appended the dataset's topics";
  EXPECT_EQ(catalog().items().size(), 1u) << "curve tree must survive a same-file reload";
}

// "Replace dataset": the replace hint targets a dataset BY ID, so a different
// file refills it in place — same DatasetId (curve keys survive for matching
// topic names), display name and source path repointed to the new file.
TEST_F(FileLoaderTest, ReplaceHintRefillsTargetedDatasetFromDifferentFile) {
  ASSERT_TRUE(load());  // sensors.mock, 3 rows
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  ASSERT_EQ(singleTopicRowCount(dataset_id), 3);

  const QString other_path = makeMockFile(u"other.mock"_s);
  PJ::LoadHints hints = loadHints();
  hints.replace_dataset_id = dataset_id;
  ASSERT_TRUE(loadAndWait(other_path, hints));

  EXPECT_EQ(session().createReader().listDatasets().size(), 1u) << "replace must not mint a second dataset";
  EXPECT_EQ(singleTopicRowCount(dataset_id), 3) << "the replaced dataset must hold the new file's rows";
  EXPECT_EQ(datasetDisplayName(dataset_id), u"other.mock"_s) << "commit must rename the dataset to the new file";
  EXPECT_EQ(loader_->sourcePathForDataset(dataset_id), other_path) << "source path must repoint to the new file";
  EXPECT_EQ(catalog().items().size(), 1u);
}

// A failed replace must be invisible: prior data, name, and source path all
// survive (the rename is deferred to commit precisely for this rollback).
TEST_F(FileLoaderTest, FailedReplaceRollsBackDataNameAndSourcePath) {
  ASSERT_TRUE(load());  // sensors.mock, 3 rows
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);

  const QString other_path = makeMockFile(u"other.mock"_s);
  PJ::LoadHints hints = loadHints(uR"({"fail_start":true})"_s);
  hints.replace_dataset_id = dataset_id;
  EXPECT_FALSE(loadAndWait(other_path, hints));

  EXPECT_EQ(singleTopicRowCount(dataset_id), 3) << "prior data restored, NOT left empty";
  EXPECT_EQ(datasetDisplayName(dataset_id), u"sensors.mock"_s) << "a rolled-back replace must keep the prior name";
  EXPECT_EQ(loader_->sourcePathForDataset(dataset_id), mock_path_) << "a rolled-back replace must keep the prior path";
  EXPECT_EQ(catalog().items().size(), 1u) << "curve tree restored after the failed replace";
}

// After a replace, the dataset's engine source_name still carries the ORIGINAL
// basename (there is no engine-level rename) — only the tracked source path
// identifies it. A later load of the new path must therefore match by path and
// refill in place, not mint a duplicate dataset.
TEST_F(FileLoaderTest, ReloadOfNewPathAfterReplaceRefillsInsteadOfDuplicating) {
  ASSERT_TRUE(load());  // sensors.mock
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);

  const QString other_path = makeMockFile(u"other.mock"_s);
  PJ::LoadHints replace_hints = loadHints();
  replace_hints.replace_dataset_id = dataset_id;
  ASSERT_TRUE(loadAndWait(other_path, replace_hints));

  ASSERT_TRUE(load(other_path));  // plain reload of the dataset's NEW source

  EXPECT_EQ(session().createReader().listDatasets().size(), 1u)
      << "reloading a replaced dataset's new path must refill it, not duplicate it";
  EXPECT_EQ(singleTopicRowCount(dataset_id), 3);
  EXPECT_EQ(datasetDisplayName(dataset_id), u"other.mock"_s);
}

// A replace whose target vanished while the picker/config dialog was open must
// degrade to a plain fresh load instead of failing or refilling a stranger.
TEST_F(FileLoaderTest, ReplaceHintWithVanishedTargetLoadsFresh) {
  PJ::LoadHints hints = loadHints();
  hints.replace_dataset_id = 424242;  // never existed
  ASSERT_TRUE(loadAndWait(mock_path_, hints));

  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  EXPECT_EQ(singleTopicRowCount(dataset_id), 3);
  EXPECT_EQ(catalog().items().size(), 1u);
}

TEST_F(FileLoaderTest, RequiredLayoutPluginFailsInsteadOfFallingBackToFirstExtensionMatch) {
  PJ::LoadHints hints = loadHints();
  hints.expected_plugin_id = u"Missing Layout Plugin"_s;
  hints.require_expected_plugin = true;

  EXPECT_FALSE(loadAndWait(mock_path_, hints));
  EXPECT_TRUE(session().createReader().listDatasets().empty())
      << "a missing exact layout plugin must fail before creating a dataset shell";
}

TEST_F(FileLoaderTest, ExactPluginOptInSelectsRequestedNonFirstExtensionMatch) {
  auto& extensions = app_session_->extensionCatalog();
  const auto original_matches = extensions.findSourcesForExtension(u".mock"_s);
  ASSERT_EQ(original_matches.size(), 1u);
  ASSERT_TRUE(extensions.pluginCatalog().registerStaticDataSource(
      alternateMockSourceVtable(original_matches.front()->library.vtable())));

  const auto matches = extensions.findSourcesForExtension(u".mock"_s);
  ASSERT_EQ(matches.size(), 2u);
  ASSERT_EQ(QString::fromStdString(matches.front()->name), u"Mock File Source"_s);
  ASSERT_EQ(QString::fromStdString(matches.back()->name), u"Alternate Mock File Source"_s)
      << "the requested plugin must be a genuine non-first extension match";

  const auto load_and_capture_plugin = [this](const QString& path, const PJ::LoadHints& hints) {
    QString selected_plugin;
    const auto loaded = QObject::connect(
        loader_.get(), &PJ::FileLoader::fileLoaded,
        [&selected_plugin](const QString&, const QString&, const QString& plugin_id, const QString&) {
          selected_plugin = plugin_id;
        });
    const bool succeeded = loadAndWait(path, hints);
    QObject::disconnect(loaded);
    return std::pair{succeeded, selected_plugin};
  };

  PJ::LoadHints desktop_hints;
  ASSERT_FALSE(desktop_hints.require_expected_plugin);
  const auto [desktop_succeeded, desktop_plugin] =
      load_and_capture_plugin(makeMockFile(u"default-selection.mock"_s), desktop_hints);
  ASSERT_TRUE(desktop_succeeded);
  EXPECT_EQ(desktop_plugin, u"Mock File Source"_s) << "the desktop/default path must preserve first-match selection";

  PJ::LoadHints exact_hints = loadHints();
  exact_hints.expected_plugin_id = u"Alternate Mock File Source"_s;
  exact_hints.require_expected_plugin = true;
  const auto [exact_succeeded, exact_plugin] =
      load_and_capture_plugin(makeMockFile(u"exact-selection.mock"_s), exact_hints);
  ASSERT_TRUE(exact_succeeded);
  EXPECT_EQ(exact_plugin, u"Alternate Mock File Source"_s)
      << "exact layout replay must select the requested non-first match (display-name fallback tier)";

  // Same selection through the plugin's stable MANIFEST id (what new layouts
  // persist as manifest_id, carried in its OWN hint field): the id tier must
  // resolve the same plugin without any display-name hint.
  PJ::LoadHints manifest_hints = loadHints();
  manifest_hints.expected_plugin_id.clear();
  manifest_hints.expected_manifest_id = u"alternate-mock-file-source"_s;
  manifest_hints.require_expected_plugin = true;
  const auto [manifest_succeeded, manifest_plugin] =
      load_and_capture_plugin(makeMockFile(u"manifest-selection.mock"_s), manifest_hints);
  ASSERT_TRUE(manifest_succeeded);
  EXPECT_EQ(manifest_plugin, u"Alternate Mock File Source"_s)
      << "a manifest-id hint must select the requested non-first match";
}

// Review finding 5: a manifest-id mismatch must FALL BACK to the saved
// display name — the two identities ride separate fields, so an uninstalled
// manifest id does not doom a layout whose display name still matches.
TEST_F(FileLoaderTest, ManifestIdMismatchFallsBackToDisplayName) {
  auto& extensions = app_session_->extensionCatalog();
  const auto original_matches = extensions.findSourcesForExtension(u".mock"_s);
  ASSERT_EQ(original_matches.size(), 1u);
  ASSERT_TRUE(extensions.pluginCatalog().registerStaticDataSource(
      alternateMockSourceVtable(original_matches.front()->library.vtable())));

  PJ::LoadHints hints = loadHints();
  hints.expected_manifest_id = u"uninstalled-provider-id"_s;  // no candidate carries this
  hints.expected_plugin_id = u"Alternate Mock File Source"_s;
  hints.require_expected_plugin = true;

  QString selected_plugin;
  const auto loaded = QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded,
      [&](const QString&, const QString&, const QString& plugin_id, const QString&, const QString&) {
        selected_plugin = plugin_id;
      });
  ASSERT_TRUE(loadAndWait(mock_path_, hints));
  QObject::disconnect(loaded);
  EXPECT_EQ(selected_plugin, u"Alternate Mock File Source"_s)
      << "a manifest mismatch must fall back to the saved display name, not fail or first-match";
}

// Review finding 5: a legacy ID-only layout (display name only, no
// manifest_id) starts at the NAME tier — it must never select a plugin whose
// MANIFEST id merely collides with the saved display name (that would be a
// silent semantic change of which plugin parses the bytes).
TEST_F(FileLoaderTest, IdOnlyLayoutNeverMatchesByManifestId) {
  auto& extensions = app_session_->extensionCatalog();
  const auto original_matches = extensions.findSourcesForExtension(u".mock"_s);
  ASSERT_EQ(original_matches.size(), 1u);
  ASSERT_TRUE(extensions.pluginCatalog().registerStaticDataSource(
      decoyWithIdEqualToMockDisplayName(original_matches.front()->library.vtable())));

  PJ::LoadHints hints = loadHints();  // legacy layout: display name only
  hints.expected_plugin_id = u"Mock File Source"_s;
  ASSERT_TRUE(hints.expected_manifest_id.isEmpty());
  hints.require_expected_plugin = true;

  QString selected_name;
  QString selected_manifest_id;
  const auto loaded = QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded,
      [&](const QString&, const QString&, const QString& plugin_id, const QString&, const QString& manifest_id) {
        selected_name = plugin_id;
        selected_manifest_id = manifest_id;
      });
  ASSERT_TRUE(loadAndWait(mock_path_, hints));
  QObject::disconnect(loaded);
  EXPECT_EQ(selected_name, u"Mock File Source"_s)
      << "the ID-only layout must resolve by display name, not by the decoy's colliding manifest id";
  EXPECT_EQ(selected_manifest_id, u"mock-file-source"_s);
}

// The consult-locked tier order: manifest id exact — scanned across ALL
// extension matches — outranks a display-name match on an EARLIER candidate.
// The decoy's manifest id equals the real mock's display name, so a matcher
// that compares (id OR name) per candidate in order, or name before id, would
// select the wrong plugin.
TEST_F(FileLoaderTest, ManifestIdTierOutranksDisplayNameTierAcrossAllCandidates) {
  auto& extensions = app_session_->extensionCatalog();
  const auto original_matches = extensions.findSourcesForExtension(u".mock"_s);
  ASSERT_EQ(original_matches.size(), 1u);
  ASSERT_TRUE(extensions.pluginCatalog().registerStaticDataSource(
      decoyWithIdEqualToMockDisplayName(original_matches.front()->library.vtable())));
  const auto matches = extensions.findSourcesForExtension(u".mock"_s);
  ASSERT_EQ(matches.size(), 2u);
  ASSERT_EQ(QString::fromStdString(matches.front()->name), u"Mock File Source"_s)
      << "the display-name collision must sit BEFORE the manifest-id owner";
  ASSERT_EQ(QString::fromStdString(matches.back()->id), u"Mock File Source"_s);

  PJ::LoadHints hints = loadHints();
  // As a NEW layout would save the decoy: both identities, in their own
  // fields. The manifest tier must win even though an EARLIER candidate's
  // display name equals the manifest id.
  hints.expected_manifest_id = u"Mock File Source"_s;  // the DECOY's manifest id
  hints.expected_plugin_id = u"Manifest Tier Decoy"_s;
  hints.require_expected_plugin = true;

  QString selected_name;
  QString selected_manifest_id;
  const auto loaded = QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded,
      [&](const QString&, const QString&, const QString& plugin_id, const QString&, const QString& manifest_id) {
        selected_name = plugin_id;
        selected_manifest_id = manifest_id;
      });
  ASSERT_TRUE(loadAndWait(mock_path_, hints));
  QObject::disconnect(loaded);
  EXPECT_EQ(selected_name, u"Manifest Tier Decoy"_s)
      << "manifest-id tier must win over an earlier candidate's display name";
  EXPECT_EQ(selected_manifest_id, u"Mock File Source"_s) << "fileLoaded must report the selected plugin's manifest id";
}

// hint_eligible must accept an expected id that is the plugin's MANIFEST id
// (not only its display name): the dialog stays skipped and the layout preset
// applies byte-exact. A regression re-opens the dialog (headless here — the
// load would fall to the QSettings/dialog path and the preset marker is lost).
TEST_F(FileLoaderTest, ManifestIdHintStillSkipsDialogAndAppliesPreset) {
  const QString preset = uR"({"filepath":"desktop-layout-value","marker":"manifest-id-hint"})"_s;
  PJ::LoadHints hints = loadHints(preset);
  hints.expected_plugin_id.clear();
  hints.expected_manifest_id = u"mock-file-source"_s;  // manifest id only, no display name
  hints.require_expected_plugin = true;

  QString emitted_config;
  const auto loaded = QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded,
      [&](const QString&, const QString&, const QString&, const QString& config) { emitted_config = config; });
  ASSERT_TRUE(loadAndWait(mock_path_, hints));
  QObject::disconnect(loaded);
  EXPECT_EQ(emitted_config, preset);
  EXPECT_NE(datasetNamed("sensors.mock"), 0u);
}

TEST_F(FileLoaderTest, BrowserLayoutPresetRewritesOnlyFilepathToFreshBackingPath) {
  const QString stale_path = u"/pj_uploads/expired/1/sensors.mock"_s;
  PJ::LoadHints hints =
      loadHints(uR"({"filepath":"/pj_uploads/expired/1/sensors.mock","delimiter":";","nested":{"keep":7}})"_s);
  hints.dialog_policy = PJ::DialogPolicy::kNever;  // the rewrite rides automated replay only
  hints.require_expected_plugin = true;
  hints.rewrite_preset_filepath = true;

  QString emitted_config;
  bool done = false;
  bool succeeded = false;
  QEventLoop loop;
  const auto loaded = QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded, &loop,
      [&](const QString&, const QString&, const QString&, const QString& config) {
        emitted_config = config;
        succeeded = true;
        done = true;
        loop.quit();
      });
  const auto failed =
      QObject::connect(loader_.get(), &PJ::FileLoader::fileLoadFailed, &loop, [&](const QString&, const QString&) {
        done = true;
        loop.quit();
      });
  PJ::LoadInput browser_input{
      .display_name = u"sensors.mock"_s,
      .backing_path = mock_path_,
      .source_identity = u"pj-upload://fresh/2/sensors.mock"_s,
      .content_sha256 = {},
      .lease = {},
  };
  ASSERT_TRUE(loader_->loadFile(std::move(browser_input), nullptr, hints));
  if (!done) {
    QTimer::singleShot(10000, &loop, [&loop]() { loop.quit(); });
    loop.exec();
  }
  QObject::disconnect(loaded);
  QObject::disconnect(failed);
  ASSERT_TRUE(done);
  ASSERT_TRUE(succeeded);

  const QJsonDocument parsed = QJsonDocument::fromJson(emitted_config.toUtf8());
  ASSERT_TRUE(parsed.isObject());
  const QJsonObject config = parsed.object();
  EXPECT_EQ(config.value(u"filepath"_s).toString(), mock_path_);
  EXPECT_NE(config.value(u"filepath"_s).toString(), stale_path);
  EXPECT_EQ(config.value(u"delimiter"_s).toString(), u";"_s);
  EXPECT_EQ(config.value(u"nested"_s).toObject().value(u"keep"_s).toInt(), 7);
}

TEST_F(FileLoaderTest, DesktopPresetBytesRemainUntouchedWithoutRewriteOptIn) {
  const QString preset = uR"({"filepath":"desktop-layout-value","marker":"byte-exact"})"_s;
  PJ::LoadHints hints = loadHints(preset);

  QString emitted_config;
  const auto loaded = QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded,
      [&](const QString&, const QString&, const QString&, const QString& config) { emitted_config = config; });
  ASSERT_TRUE(loadAndWait(mock_path_, hints));
  QObject::disconnect(loaded);
  EXPECT_EQ(emitted_config, preset);
}

TEST_F(FileLoaderTest, BrowserLayoutEmptyPresetCanStillInjectFreshBackingPathAndSkipDialog) {
  PJ::LoadHints hints = loadHints(QString());
  hints.dialog_policy = PJ::DialogPolicy::kNever;  // the rewrite rides automated replay only
  hints.require_expected_plugin = true;
  hints.rewrite_preset_filepath = true;

  EXPECT_TRUE(loadAndWait(mock_path_, hints));
  EXPECT_NE(datasetNamed("sensors.mock"), 0u);
}

// With several files loaded, reloading ONE of them must replace only that
// dataset. The staged ingest always allocates dataset id 1 in its throwaway
// engine, so any id confusion between staged and primary leaks the reload into
// whichever primary dataset shares the staged id (here: the first file).
TEST_F(FileLoaderTest, ReloadWithMultipleDatasetsLeavesOthersIntact) {
  const QString path_a = makeMockFile(u"a.mock"_s);
  const QString path_b = makeMockFile(u"b.mock"_s);
  ASSERT_TRUE(load(path_a));
  ASSERT_TRUE(load(path_b));

  const PJ::DatasetId dataset_a = datasetNamed("a.mock");
  const PJ::DatasetId dataset_b = datasetNamed("b.mock");
  ASSERT_NE(dataset_a, 0u);
  ASSERT_NE(dataset_b, 0u);
  ASSERT_EQ(singleTopicRowCount(dataset_a), 3);
  ASSERT_EQ(singleTopicRowCount(dataset_b), 3);

  ASSERT_TRUE(load(path_b));

  EXPECT_EQ(session().createReader().listDatasets().size(), 2u) << "reload must not mint a new dataset";
  EXPECT_EQ(singleTopicRowCount(dataset_a), 3) << "reloading b must not touch dataset a";
  EXPECT_EQ(singleTopicRowCount(dataset_b), 3) << "dataset b must be replaced in place";
  EXPECT_EQ(catalog().items().size(), 2u) << "curve tree must keep both datasets' curves";
}

// Two files that share a basename but live in different directories (e.g.
// run1/log.mcap and run2/log.mcap — common for per-run robotics logs) must load
// as TWO distinct datasets. The same-source match keys on full-path identity,
// not basename, so the second file is not mistaken for a reload of the first
// (which would silently alias its data and lose the second file entirely).
TEST_F(FileLoaderTest, SameBasenameDifferentDirsLoadAsDistinctDatasets) {
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(u"runA"_s));
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(u"runB"_s));
  const QString path_a = makeMockFile(u"runA/log.mock"_s);
  const QString path_b = makeMockFile(u"runB/log.mock"_s);
  ASSERT_TRUE(load(path_a));
  ASSERT_TRUE(load(path_b));

  EXPECT_EQ(session().createReader().listDatasets().size(), 2u)
      << "same-basename files in different dirs must be distinct datasets, not aliased";
  EXPECT_EQ(catalog().items().size(), 2u) << "both files' curves must appear in the tree";
}

// The layout-reload path (prefer_reuse) of two same-basename files in different
// dirs must likewise restore both — the multi-file layout feature this depends
// on. Pre-fix the second prefer_reuse load reused the first dataset and emitted
// no new one, collapsing the session to a single file.
TEST_F(FileLoaderTest, LayoutReloadOfSameBasenameDifferentDirsRestoresBoth) {
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(u"runA"_s));
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(u"runB"_s));
  const QString path_a = makeMockFile(u"runA/log.mock"_s);
  const QString path_b = makeMockFile(u"runB/log.mock"_s);
  ASSERT_TRUE(loadAndWait(path_a, skipDialogHints(/*prefer_reuse=*/true)));  // mimic a layout replay
  ASSERT_TRUE(loadAndWait(path_b, skipDialogHints(/*prefer_reuse=*/true)));

  EXPECT_EQ(session().createReader().listDatasets().size(), 2u)
      << "layout reload of two same-basename files must restore two datasets";
}

// FileLoader maps each loaded dataset back to the file it came from (so the
// shell can drop the right loaded-source entry when a dataset is removed).
// untrackDataset clears that association.
TEST_F(FileLoaderTest, SourcePathForDatasetTracksLoadedFileAndUntracks) {
  ASSERT_TRUE(load());  // loads mock_path_ (sensors.mock)
  const PJ::DatasetId id = datasetNamed("sensors.mock");
  ASSERT_NE(id, 0u);
  EXPECT_EQ(loader_->sourcePathForDataset(id), mock_path_);
  // An unknown id has no recorded path.
  EXPECT_TRUE(loader_->sourcePathForDataset(id + 1000).isEmpty());

  loader_->untrackDataset(id);
  EXPECT_TRUE(loader_->sourcePathForDataset(id).isEmpty()) << "untrackDataset must drop the association";
}

// Source-path identity has ONE owner: SessionManager's registry. FileLoader is
// a pass-through, so plots/scenes/processors resolving through the session see
// exactly what the loader recorded — and untrackDataset clears the shared entry.
TEST_F(FileLoaderTest, SourcePathRegistryLivesInSessionManager) {
  ASSERT_TRUE(load());
  const PJ::DatasetId id = datasetNamed("sensors.mock");
  ASSERT_NE(id, 0u);
  EXPECT_EQ(session().datasetSourcePath(id), mock_path_);
  EXPECT_EQ(loader_->sourcePathForDataset(id), session().datasetSourcePath(id));

  loader_->untrackDataset(id);
  EXPECT_TRUE(session().datasetSourcePath(id).isEmpty());
}

// Core of the resurrection fix (MainWindow::appendDataSourceElement's liveness
// filter): once a dataset is removed, the file it came from must drop out of the
// set of source paths still backing a live dataset — otherwise a saved layout
// would re-list and resurrect it on reload. Exercised with the real
// CatalogModel + FileLoader (the XML emission itself lives in MainWindow).
TEST_F(FileLoaderTest, RemovedDatasetDropsFromLiveSourcePaths) {
  const QString path_a = makeMockFile(u"a.mock"_s);
  const QString path_b = makeMockFile(u"b.mock"_s);
  ASSERT_TRUE(load(path_a));
  ASSERT_TRUE(load(path_b));
  const PJ::DatasetId id_a = datasetNamed("a.mock");
  ASSERT_NE(id_a, 0u);

  // The set of source paths still backing a live catalog dataset — exactly how
  // appendDataSourceElement decides which <fileInfo> entries to write.
  const auto live_paths = [&]() {
    QSet<QString> paths;
    for (const auto& [id, name] : catalog().datasets()) {
      (void)name;
      if (const QString p = loader_->sourcePathForDataset(id); !p.isEmpty()) {
        paths.insert(p);
      }
    }
    return paths;
  };

  EXPECT_TRUE(live_paths().contains(path_a));
  EXPECT_TRUE(live_paths().contains(path_b));

  // Remove dataset a the way MainWindow::removeDatasetData does.
  session().evictDatasetObjects(id_a);
  catalog().removeDataset(id_a);
  loader_->untrackDataset(id_a);

  const QSet<QString> live = live_paths();
  EXPECT_FALSE(live.contains(path_a)) << "removed dataset's file must not be a live source (no resurrection)";
  EXPECT_TRUE(live.contains(path_b)) << "surviving dataset's file stays a live source";
}

// A real "Remove Dataset" (no tombstone) truly erases the dataset from the engine, so a
// later load of the SAME source — even via the layout-replay prefer_reuse path — mints a
// FRESH dataset and re-ingests the data, instead of reattaching to an emptied/tombstoned
// shell (the bug: the 2nd load showed no progress bar and no data).
TEST_F(FileLoaderTest, RealDeleteThenPreferReuseReloadReIngestsFreshDataset) {
  ASSERT_TRUE(load());
  const PJ::DatasetId id = datasetNamed("sensors.mock");
  ASSERT_NE(id, 0u);
  ASSERT_EQ(singleTopicRowCount(id), 3);
  ASSERT_EQ(catalog().items().size(), 1u);

  // Real delete the way MainWindow::removeDatasetData now does it: erase objects,
  // drop catalog items WITHOUT a tombstone, then erase the engine's scalar storage.
  session().evictDatasetObjects(id);
  catalog().removeDataset(id, /*tombstone=*/false);
  session().dataEngine().removeDataset(id);

  EXPECT_TRUE(session().createReader().listDatasets().empty()) << "dataset truly erased from the engine";
  EXPECT_TRUE(catalog().items().empty()) << "no catalog items survive a real delete";
  EXPECT_EQ(datasetNamed("sensors.mock"), 0u) << "no tombstoned shell left behind";

  // Reload the same source as a layout replay would (prefer_reuse). Pre-fix this reused
  // the emptied dataset (restoreDataset + return-false: no worker, no re-ingest); now the
  // erased dataset is gone from listDatasets, so the loader mints a fresh one and ingests.
  PJ::LoadHints hints;
  hints.expected_plugin_id = u"Mock File Source"_s;
  hints.preset_config_json = u"{}"_s;
  hints.dialog_policy = PJ::DialogPolicy::kPreferPreset;
  hints.prefer_reuse = true;
  // The dataset was erased, so prefer_reuse finds nothing to reuse and falls through
  // to a FRESH single-instance load — which is async (worker). Wait for it.
  ASSERT_TRUE(loadAndWait(mock_path_, hints));

  const PJ::DatasetId reloaded = datasetNamed("sensors.mock");
  EXPECT_NE(reloaded, 0u) << "reload must re-create the dataset";
  EXPECT_NE(reloaded, id) << "a real delete + reload mints a FRESH id, not the erased one";
  EXPECT_EQ(singleTopicRowCount(reloaded), 3) << "the file must be re-ingested, not reattached empty";
  EXPECT_EQ(catalog().items().size(), 1u) << "curves come back after reload";
}

TEST_F(FileLoaderTest, FanoutReloadErasesOldDatasetBeforePreferReuseReload) {
  const QString path = makeMockFile(u"fanout.mock"_s);
  ASSERT_TRUE(load(path));

  const PJ::DatasetId old_id = datasetNamed("fanout.mock");
  ASSERT_NE(old_id, 0u);
  ASSERT_EQ(singleTopicRowCount(old_id), 3);

  PJ::LoadHints fanout_hints =
      loadHints(uR"({"__pj_fanout":["{\"display_suffix\":\"left\"}","{\"display_suffix\":\"right\"}"]})"_s);
  ASSERT_TRUE(loadAndWait(path, fanout_hints));

  EXPECT_FALSE(engineHasDataset(old_id)) << "fanout reload must erase the tombstoned old dataset from the engine";
  EXPECT_EQ(datasetNamed("fanout.mock"), 0u) << "prefer_reuse must not find the old basename after fanout reload";

  PJ::LoadHints reuse_hints = loadHints(u"{}"_s);
  reuse_hints.prefer_reuse = true;
  // Old dataset erased → prefer_reuse falls through to a fresh async load; wait for it.
  ASSERT_TRUE(loadAndWait(path, reuse_hints));

  const PJ::DatasetId reloaded = datasetNamed("fanout.mock");
  EXPECT_NE(reloaded, 0u) << "prefer_reuse after fanout reload must ingest a fresh dataset";
  EXPECT_NE(reloaded, old_id) << "the old fanout-replaced DatasetId must not be reused";
  EXPECT_EQ(singleTopicRowCount(reloaded), 3) << "fresh prefer_reuse load must ingest rows";
}

// A replacing fan-out tombstones the dataset it is replacing and real-deletes
// it only once the replacement has committed. Discarding the load means nothing
// replaced it, so the tombstone must be rolled back — the user keeps the data
// they already had. (The delete used to run above these exits, which erased it
// permanently.)
TEST_F(FileLoaderTest, DiscardedReplacingFanoutRestoresTheDatasetItWasReplacing) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"replaced.fanoutprobe"_s);
  ASSERT_TRUE(loadAndWait(path, fanoutProbeHints(u"{}"_s)));
  const PJ::DatasetId original = datasetNamed("replaced.fanoutprobe");
  ASSERT_NE(original, 0u);
  ASSERT_EQ(singleTopicRowCount(original), 3);
  const std::size_t catalog_items_before = catalog().items().size();
  ASSERT_GT(catalog_items_before, 0u);

  // Reload the same path as a fan-out (which replaces in place) and discard the
  // whole load at its second entry.
  const PJ::LoadHints hints =
      fanoutProbeHints(uR"({"__pj_fanout":["{\"display_suffix\":\"complete\"}","{\"display_suffix\":\"cancel\"}"]})"_s);
  bool cancel_sent = false;
  const auto cancel_connection = QObject::connect(
      loader_.get(), &PJ::FileLoader::ingestStarted, loader_.get(),
      [this, &cancel_sent](const QString& title, int, int, bool) {
        if (title != u"fanout-probe:cancel"_s || cancel_sent) {
          return;
        }
        cancel_sent = true;
        loader_->cancelCurrent(/*keep_partial=*/false);
        releaseFanoutProbeCancelEntry();
      });
  (void)loadAndWait(path, hints);
  QObject::disconnect(cancel_connection);
  ASSERT_TRUE(cancel_sent);

  EXPECT_TRUE(engineHasDataset(original)) << "a discarded replacement must not erase what it was replacing";
  EXPECT_EQ(datasetNamed("replaced.fanoutprobe"), original) << "the original dataset keeps its id";
  EXPECT_EQ(singleTopicRowCount(original), 3) << "and its rows";
  EXPECT_EQ(catalog().items().size(), catalog_items_before) << "its curves come back with it";
  EXPECT_EQ(datasetNamed("replaced/complete"), 0u) << "the discarded fan-out leaves nothing of its own";
  EXPECT_EQ(datasetNamed("replaced/cancel"), 0u);
}

// Same contract on the other dataset-less exit: every fan-out entry failing is
// not a licence to delete the dataset the load was replacing.
TEST_F(FileLoaderTest, AllFailedReplacingFanoutRestoresTheDatasetItWasReplacing) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"allfail.fanoutprobe"_s);
  ASSERT_TRUE(loadAndWait(path, fanoutProbeHints(u"{}"_s)));
  const PJ::DatasetId original = datasetNamed("allfail.fanoutprobe");
  ASSERT_NE(original, 0u);
  ASSERT_EQ(singleTopicRowCount(original), 3);
  const std::size_t catalog_items_before = catalog().items().size();

  const PJ::LoadHints hints =
      fanoutProbeHints(uR"({"__pj_fanout":["{\"display_suffix\":\"fail\"}","{\"display_suffix\":\"fail\"}"]})"_s);
  EXPECT_FALSE(loadAndWait(path, hints)) << "every entry failed, so the load failed";

  EXPECT_TRUE(engineHasDataset(original)) << "an all-failed replacement must not erase what it was replacing";
  EXPECT_EQ(datasetNamed("allfail.fanoutprobe"), original);
  EXPECT_EQ(singleTopicRowCount(original), 3);
  EXPECT_EQ(catalog().items().size(), catalog_items_before);
}

// An observer of a fan-out entry's terminal is allowed to shut the loader down —
// closing the window while an import runs is exactly that. joinForShutdown
// resets begin_load_task_, which DESTROYS the coroutine frame the fan-out loop
// is running in, so the terminal must not be emitted from inside that frame: the
// loop would return into freed storage. Delivering it through the event loop is
// what makes this survivable, and the loader has to stay usable afterwards.
TEST_F(FileLoaderTest, ShutdownFromInsideAFanoutTerminalLeavesTheLoaderUsable) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"terminalshutdown.fanoutprobe"_s);
  const PJ::LoadHints hints = fanoutProbeHints(
      uR"({"__pj_fanout":["{\"display_suffix\":\"first\"}","{\"display_suffix\":\"second\"}",)"
      uR"("{\"display_suffix\":\"third\"}"]})"_s);

  int shutdowns = 0;
  const auto shutdown_connection = QObject::connect(
      &session(), &PJ::SessionManager::ingestEnded, loader_.get(),
      [this, &shutdowns](PJ::IngestToken, PJ::IngestOutcome) {
        if (shutdowns == 0) {
          ++shutdowns;
          loader_->joinForShutdown();
        }
      });
  ASSERT_TRUE(loader_->loadFile(path, nullptr, hints));
  ASSERT_TRUE(pj_app_test::pumpUntil([this, &shutdowns]() { return shutdowns > 0 && !loader_->isBusy(); }))
      << "no fan-out entry ever reported a terminal";
  QObject::disconnect(shutdown_connection);

  EXPECT_EQ(shutdowns, 1);
  EXPECT_FALSE(loader_->isBusy());
  EXPECT_FALSE(session().hasActiveIngests()) << "shutdown must close every token the fan-out began";

  // A fresh load after that shutdown still completes.
  EXPECT_TRUE(loadAndWait(path, fanoutProbeHints(u"{}"_s)));
  EXPECT_NE(datasetNamed("terminalshutdown.fanoutprobe"), 0u);
}

// Discarding EVERY row of a fan-out leaves precisely what a "Remove All" leaves:
// nothing. A row discard is neither a failure nor a load-wide stop, so this case
// used to reach the success tail — real-deleting the dataset the load was
// replacing (the tombstone exists to protect exactly that) and then announcing a
// successful load that produced no dataset at all.
TEST_F(FileLoaderTest, FanoutWithEveryRowDiscardedIsCancelledAndKeepsTheReplacedDataset) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"rowdiscard.fanoutprobe"_s);
  ASSERT_TRUE(loadAndWait(path, fanoutProbeHints(u"{}"_s)));
  const PJ::DatasetId original = datasetNamed("rowdiscard.fanoutprobe");
  ASSERT_NE(original, 0u);
  ASSERT_EQ(singleTopicRowCount(original), 3);
  const std::size_t catalog_items_before = catalog().items().size();

  // Reload the same path as a fan-out (which replaces in place) and discard both
  // entries through their own rows.
  const PJ::LoadHints hints =
      fanoutProbeHints(uR"({"__pj_fanout":["{\"display_suffix\":\"cancel\"}","{\"display_suffix\":\"cancel\"}"]})"_s);
  LoadFinishedRecorder recorder(*loader_);
  // The replacement announcement carries the arming load's ticket: the shell
  // disarms its workspace capture by ticket equality, so a stale same-path
  // terminal from another load can never kill a live load's capture.
  quint64 arm_ticket = 0;
  const auto arm_connection = QObject::connect(
      loader_.get(), &PJ::FileLoader::sourceReplacementAboutToCommit, loader_.get(),
      [&arm_ticket](quint64 ticket, const QString&, PJ::DatasetId) { arm_ticket = ticket; });
  int cancels_sent = 0;
  const auto cancel_connection = QObject::connect(
      &session(), &PJ::SessionManager::ingestBegan, loader_.get(),
      [this, &cancels_sent](PJ::IngestToken token, const QString& label, quint64) {
        if (label != u"fanout-probe:cancel"_s) {
          return;
        }
        ++cancels_sent;
        EXPECT_TRUE(session().requestCancel(token, /*keep_partial=*/false));
        releaseFanoutProbeCancelEntry();
      });

  EXPECT_FALSE(loadAndWait(path, hints)) << "a fan-out that kept nothing is not a successful load";
  QObject::disconnect(cancel_connection);
  QObject::disconnect(arm_connection);
  pj_app_test::flushQueuedEvents(2);

  ASSERT_EQ(cancels_sent, 2) << "both fanout entries must reach their cancellation rendezvous";
  EXPECT_TRUE(engineHasDataset(original)) << "discarding every row must not erase what the load was replacing";
  EXPECT_EQ(datasetNamed("rowdiscard.fanoutprobe"), original) << "the original dataset keeps its id";
  EXPECT_EQ(singleTopicRowCount(original), 3) << "and its rows";
  EXPECT_EQ(catalog().items().size(), catalog_items_before) << "its curves come back with it";
  EXPECT_EQ(datasetNamed("rowdiscard/cancel"), 0u) << "no discarded entry survives";

  ASSERT_FALSE(recorder.events.empty());
  EXPECT_EQ(recorder.events.back().outcome, PJ::LoadOutcome::kCancelled)
      << "a load that produced no dataset must not report success";
  EXPECT_TRUE(recorder.events.back().produced.isEmpty());
  EXPECT_NE(arm_ticket, 0u) << "the replacing fan-out must announce its capture";
  EXPECT_EQ(recorder.events.back().ticket, arm_ticket)
      << "the announcement carries the SAME ticket the load's terminal reports";
}

// A fan-out row's bin discards THAT ENTRY and nothing else: the sibling that
// already loaded keeps its data, and the entries after it still run. The row is
// the unit the user aimed at, and it names one dataset.
TEST_F(FileLoaderTest, FanoutRowDiscardDropsOnlyThatEntryAndKeepsSiblings) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"rowstop.fanoutprobe"_s);
  const PJ::LoadHints hints = fanoutProbeHints(
      uR"({"__pj_fanout":["{\"display_suffix\":\"first\"}","{\"display_suffix\":\"cancel\"}","{\"display_suffix\":\"third\"}"]})"_s);
  IngestLifecycleRecorder ingest_lifecycle(session());

  // The production route for a row click: the view resolves its row key to this
  // token and asks the session, exactly as wireIngestProgress does.
  bool cancel_sent = false;
  const auto cancel_connection = QObject::connect(
      &session(), &PJ::SessionManager::ingestBegan, loader_.get(),
      [this, &cancel_sent](PJ::IngestToken token, const QString& label, quint64) {
        if (label != u"fanout-probe:cancel"_s || cancel_sent) {
          return;
        }
        cancel_sent = true;
        EXPECT_TRUE(session().requestCancel(token, /*keep_partial=*/false));
        releaseFanoutProbeCancelEntry();
      });

  (void)loadAndWait(path, hints);
  QObject::disconnect(cancel_connection);
  // The fan-out delivers each entry's ingest terminal through the event loop
  // (it runs inside the load coroutine, whose frame a re-entrant observer may
  // destroy), so drain it before asserting on ingest lifecycle state.
  pj_app_test::flushQueuedEvents(2);

  ASSERT_TRUE(cancel_sent) << "the second fanout entry never reached its cancellation rendezvous";
  EXPECT_FALSE(loader_->isBusy());
  const std::vector<std::string> expected_starts{"first", "cancel", "third"};
  EXPECT_EQ(fanoutProbeStartedSuffixes(), expected_starts)
      << "a row stop must not end the fan-out: the entry after it still runs";

  const PJ::DatasetId first = datasetNamed("rowstop/first");
  const PJ::DatasetId third = datasetNamed("rowstop/third");
  ASSERT_NE(first, 0u) << "the sibling that completed before the stop must survive";
  ASSERT_NE(third, 0u) << "the entry after the stopped one must still load";
  EXPECT_EQ(singleTopicRowCount(first), 3);
  EXPECT_EQ(singleTopicRowCount(third), 3);
  EXPECT_EQ(datasetNamed("rowstop/cancel"), 0u) << "only the discarded row's dataset is dropped";
  EXPECT_EQ(session().createReader().listDatasets().size(), 2u);

  ASSERT_EQ(ingest_lifecycle.began.size(), 3u);
  ASSERT_EQ(ingest_lifecycle.ended.size(), 3u) << "every begun fanout entry must end exactly once";
  const auto* cancelled_begin = ingest_lifecycle.beginForLabel(u"fanout-probe:cancel"_s);
  const auto* third_begin = ingest_lifecycle.beginForLabel(u"fanout-probe:third"_s);
  ASSERT_NE(cancelled_begin, nullptr);
  ASSERT_NE(third_begin, nullptr);
  const auto* cancelled_end = ingest_lifecycle.endFor(cancelled_begin->token);
  const auto* third_end = ingest_lifecycle.endFor(third_begin->token);
  ASSERT_NE(cancelled_end, nullptr);
  ASSERT_NE(third_end, nullptr);
  EXPECT_EQ(cancelled_end->outcome, PJ::IngestOutcome::kCancelled);
  EXPECT_EQ(third_end->outcome, PJ::IngestOutcome::kCompleted)
      << "a sibling must not inherit the stopped row's outcome";
  EXPECT_FALSE(session().hasActiveIngests());
}

// The ✕ arm of the same scope: the stopped row keeps what it received, and the
// fan-out carries on.
TEST_F(FileLoaderTest, FanoutRowKeepRetainsThatEntryAndContinues) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"rowkeep.fanoutprobe"_s);
  const PJ::LoadHints hints = fanoutProbeHints(
      uR"({"__pj_fanout":["{\"display_suffix\":\"first\"}","{\"display_suffix\":\"cancel\"}","{\"display_suffix\":\"third\"}"]})"_s);
  IngestLifecycleRecorder ingest_lifecycle(session());

  bool cancel_sent = false;
  const auto cancel_connection = QObject::connect(
      &session(), &PJ::SessionManager::ingestBegan, loader_.get(),
      [this, &cancel_sent](PJ::IngestToken token, const QString& label, quint64) {
        if (label != u"fanout-probe:cancel"_s || cancel_sent) {
          return;
        }
        cancel_sent = true;
        EXPECT_TRUE(session().requestCancel(token, /*keep_partial=*/true));
        releaseFanoutProbeCancelEntry();
      });

  (void)loadAndWait(path, hints);
  QObject::disconnect(cancel_connection);
  pj_app_test::flushQueuedEvents(2);

  ASSERT_TRUE(cancel_sent) << "the second fanout entry never reached its cancellation rendezvous";
  const std::vector<std::string> expected_starts{"first", "cancel", "third"};
  EXPECT_EQ(fanoutProbeStartedSuffixes(), expected_starts);

  const PJ::DatasetId partial = datasetNamed("rowkeep/cancel");
  ASSERT_NE(partial, 0u);
  EXPECT_EQ(singleTopicRowCount(partial), 1) << "the stopped row keeps exactly what arrived";
  EXPECT_EQ(singleTopicRowCount(datasetNamed("rowkeep/first")), 3);
  EXPECT_EQ(singleTopicRowCount(datasetNamed("rowkeep/third")), 3);
  EXPECT_EQ(session().createReader().listDatasets().size(), 3u);
  EXPECT_FALSE(session().hasActiveIngests());
}

// WHOLE-LOAD scope (the title-bar strip's stop, cancelCurrent): it ends the
// fan-out at the entry in flight and drops everything the load produced. The
// per-row affordance is scoped differently — see the FanoutRow* tests below.
TEST_F(FileLoaderTest, FanoutRemoveAllStopsAtCancelledEntryAndDropsCompletedEntries) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"cancel.fanoutprobe"_s);
  const PJ::LoadHints hints = fanoutProbeHints(
      uR"({"__pj_fanout":["{\"display_suffix\":\"complete\"}","{\"display_suffix\":\"cancel\"}","{\"display_suffix\":\"must-not-start\"}"]})"_s);
  IngestLifecycleRecorder ingest_lifecycle(session());

  bool cancel_sent = false;
  const auto cancel_connection = QObject::connect(
      loader_.get(), &PJ::FileLoader::ingestStarted, loader_.get(),
      [this, &cancel_sent](const QString& title, int, int, bool) {
        if (title != u"fanout-probe:cancel"_s || cancel_sent) {
          return;
        }
        cancel_sent = true;
        loader_->cancelCurrent(/*keep_partial=*/false);
        releaseFanoutProbeCancelEntry();
      });

  // Discard may reasonably terminate through either fileLoaded or
  // fileLoadFailed; this test pins the dataset/cancellation contract instead of
  // coupling it to that separate signal-policy decision.
  (void)loadAndWait(path, hints);
  QObject::disconnect(cancel_connection);
  pj_app_test::flushQueuedEvents(2);

  ASSERT_TRUE(cancel_sent) << "the second fanout entry never reached its cancellation rendezvous";
  EXPECT_FALSE(loader_->isBusy());
  const std::vector<std::string> expected_starts{"complete", "cancel"};
  EXPECT_EQ(fanoutProbeStartedSuffixes(), expected_starts)
      << "a progress cancellation must stop fanout instead of being classified as a recoverable plugin failure";
  EXPECT_TRUE(session().createReader().listDatasets().empty())
      << "Remove All must also delete fanout entries that completed before cancellation";
  EXPECT_EQ(datasetNamed("cancel/complete"), 0u);
  EXPECT_EQ(datasetNamed("cancel/cancel"), 0u);
  EXPECT_EQ(datasetNamed("cancel/must-not-start"), 0u);

  ASSERT_EQ(ingest_lifecycle.began.size(), 2u);
  ASSERT_EQ(ingest_lifecycle.ended.size(), 2u) << "every begun fanout entry must end exactly once";
  const auto* completed_begin = ingest_lifecycle.beginForLabel(u"fanout-probe:complete"_s);
  const auto* cancelled_begin = ingest_lifecycle.beginForLabel(u"fanout-probe:cancel"_s);
  ASSERT_NE(completed_begin, nullptr);
  ASSERT_NE(cancelled_begin, nullptr);
  const auto* completed_end = ingest_lifecycle.endFor(completed_begin->token);
  const auto* cancelled_end = ingest_lifecycle.endFor(cancelled_begin->token);
  ASSERT_NE(completed_end, nullptr);
  ASSERT_NE(cancelled_end, nullptr);
  EXPECT_EQ(completed_end->outcome, PJ::IngestOutcome::kCompleted);
  EXPECT_EQ(cancelled_end->outcome, PJ::IngestOutcome::kCancelled);
  EXPECT_EQ(ingest_lifecycle.beginCountForDataset(completed_begin->token.dataset_id), 1u);
  EXPECT_EQ(ingest_lifecycle.endCountForDataset(completed_begin->token.dataset_id), 1u);
  EXPECT_EQ(ingest_lifecycle.beginCountForDataset(cancelled_begin->token.dataset_id), 1u);
  EXPECT_EQ(ingest_lifecycle.endCountForDataset(cancelled_begin->token.dataset_id), 1u);
  EXPECT_FALSE(session().hasActiveIngests());
}

// WHOLE-LOAD scope, keep arm: the fan-out still ends early, but the entries it
// already produced (and the partial one) are kept.
TEST_F(FileLoaderTest, FanoutStopAndKeepStopsAtCancelledEntryAndKeepsPartialEntry) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"keep.fanoutprobe"_s);
  const PJ::LoadHints hints = fanoutProbeHints(
      uR"({"__pj_fanout":["{\"display_suffix\":\"complete\"}","{\"display_suffix\":\"cancel\"}","{\"display_suffix\":\"must-not-start\"}"]})"_s);
  IngestLifecycleRecorder ingest_lifecycle(session());

  bool cancel_sent = false;
  const auto cancel_connection = QObject::connect(
      loader_.get(), &PJ::FileLoader::ingestStarted, loader_.get(),
      [this, &cancel_sent](const QString& title, int, int, bool) {
        if (title != u"fanout-probe:cancel"_s || cancel_sent) {
          return;
        }
        cancel_sent = true;
        loader_->cancelCurrent(/*keep_partial=*/true);
        releaseFanoutProbeCancelEntry();
      });

  (void)loadAndWait(path, hints);
  QObject::disconnect(cancel_connection);
  pj_app_test::flushQueuedEvents(2);

  ASSERT_TRUE(cancel_sent) << "the second fanout entry never reached its cancellation rendezvous";
  EXPECT_FALSE(loader_->isBusy());
  const std::vector<std::string> expected_starts{"complete", "cancel"};
  EXPECT_EQ(fanoutProbeStartedSuffixes(), expected_starts)
      << "Stop and Keep must not continue into the next fanout entry";
  EXPECT_EQ(session().createReader().listDatasets().size(), 2u)
      << "Stop and Keep must retain completed entries plus the in-flight partial entry";

  const PJ::DatasetId complete = datasetNamed("keep/complete");
  const PJ::DatasetId partial = datasetNamed("keep/cancel");
  ASSERT_NE(complete, 0u);
  ASSERT_NE(partial, 0u);
  EXPECT_EQ(singleTopicRowCount(complete), 3);
  EXPECT_EQ(singleTopicRowCount(partial), 1);
  EXPECT_EQ(datasetNamed("keep/must-not-start"), 0u);

  ASSERT_EQ(ingest_lifecycle.began.size(), 2u);
  ASSERT_EQ(ingest_lifecycle.ended.size(), 2u) << "every begun fanout entry must end exactly once";
  const auto* completed_begin = ingest_lifecycle.beginForLabel(u"fanout-probe:complete"_s);
  const auto* cancelled_begin = ingest_lifecycle.beginForLabel(u"fanout-probe:cancel"_s);
  ASSERT_NE(completed_begin, nullptr);
  ASSERT_NE(cancelled_begin, nullptr);
  const auto* completed_end = ingest_lifecycle.endFor(completed_begin->token);
  const auto* cancelled_end = ingest_lifecycle.endFor(cancelled_begin->token);
  ASSERT_NE(completed_end, nullptr);
  ASSERT_NE(cancelled_end, nullptr);
  EXPECT_EQ(completed_end->outcome, PJ::IngestOutcome::kCompleted);
  EXPECT_EQ(cancelled_end->outcome, PJ::IngestOutcome::kCancelled);
  EXPECT_EQ(ingest_lifecycle.beginCountForDataset(complete), 1u);
  EXPECT_EQ(ingest_lifecycle.endCountForDataset(complete), 1u);
  EXPECT_EQ(ingest_lifecycle.beginCountForDataset(partial), 1u);
  EXPECT_EQ(ingest_lifecycle.endCountForDataset(partial), 1u);
  EXPECT_FALSE(session().hasActiveIngests());
}

TEST_F(FileLoaderTest, FanoutFailureEndsOnlyTheFailedEntryWithFailedOutcome) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"failure.fanoutprobe"_s);
  const PJ::LoadHints hints = fanoutProbeHints(
      uR"({"__pj_fanout":["{\"display_suffix\":\"before\"}","{\"display_suffix\":\"fail\"}","{\"display_suffix\":\"after\"}"]})"_s);
  IngestLifecycleRecorder ingest_lifecycle(session());

  ASSERT_TRUE(loadAndWait(path, hints)) << "successful siblings keep a partially successful fanout load";
  pj_app_test::flushQueuedEvents(2);

  ASSERT_EQ(ingest_lifecycle.began.size(), 3u);
  ASSERT_EQ(ingest_lifecycle.ended.size(), 3u) << "every begun fanout entry must end exactly once";
  for (const auto& [label, outcome] : std::vector<std::pair<QString, PJ::IngestOutcome>>{
           {u"fanout-probe:before"_s, PJ::IngestOutcome::kCompleted},
           {u"fanout-probe:fail"_s, PJ::IngestOutcome::kFailed},
           {u"fanout-probe:after"_s, PJ::IngestOutcome::kCompleted}}) {
    const auto* began = ingest_lifecycle.beginForLabel(label);
    ASSERT_NE(began, nullptr) << label.toStdString();
    const auto* ended = ingest_lifecycle.endFor(began->token);
    ASSERT_NE(ended, nullptr) << label.toStdString();
    EXPECT_EQ(ended->outcome, outcome) << label.toStdString();
    EXPECT_EQ(ingest_lifecycle.beginCountForDataset(began->token.dataset_id), 1u);
    EXPECT_EQ(ingest_lifecycle.endCountForDataset(began->token.dataset_id), 1u);
  }
  EXPECT_FALSE(session().hasActiveIngests());
}

TEST_F(FileLoaderTest, FanoutControlLifecycleStaysOnSdkMainThread) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"threads.fanoutprobe"_s);
  const PJ::LoadHints hints =
      fanoutProbeHints(uR"({"__pj_fanout":["{\"display_suffix\":\"left\"}","{\"display_suffix\":\"right\"}"]})"_s);

  ASSERT_TRUE(loadAndWait(path, hints));
  const std::vector<std::string> expected_starts{"left", "right"};
  ASSERT_EQ(fanoutProbeStartedSuffixes(), expected_starts) << "the lifecycle assertion must not pass vacuously";

  const std::vector<std::string> off_main = fanoutProbeControlCallsOffMain();
  EXPECT_TRUE(off_main.empty()) << "SDK [main-thread] DataSource control slots ran on the fanout worker: "
                                << joinFanoutProbeCalls(off_main);
}

// A producer may tick without ever announcing a start (the plugin ABI's
// progress_start and progress_update are independent slots). There is then no
// ingest lifecycle to report, but the rows it committed are already visible to
// readers, so the publish must happen anyway: dropping data because a lifecycle
// call never arrived is the one thing this path must not do.
TEST_F(FileLoaderTest, ProgressTickWithoutABeginStillPublishesTheDatasetRows) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"nostart.fanoutprobe"_s);
  const PJ::LoadHints hints = fanoutProbeHints(uR"({"display_suffix":"nostart"})"_s);
  IngestLifecycleRecorder ingest_lifecycle(session());

  int notifications = 0;
  const auto ingest_connection = QObject::connect(
      &session(), &PJ::SessionManager::samplesIngested, loader_.get(),
      [&notifications](const QVector<PJ::TopicId>& ids, bool live) {
        if (!live && !ids.isEmpty()) {
          ++notifications;
        }
      });
  // Space the records past the 50 ms flush throttle so the ticks really reach
  // the GUI, the same way the progressive test paces its flushes.
  QTimer::singleShot(100, []() { releaseFanoutProbeProgressStep(1); });
  QTimer::singleShot(200, []() { releaseFanoutProbeProgressStep(2); });
  QTimer::singleShot(300, []() { releaseFanoutProbeProgressStep(3); });

  const bool loaded = loadAndWait(path, hints);
  QObject::disconnect(ingest_connection);

  ASSERT_TRUE(loaded);
  EXPECT_TRUE(ingest_lifecycle.began.empty()) << "the producer never started an ingest";
  EXPECT_GT(notifications, 0) << "a tick without a begin must still publish the dataset's committed rows";
  const PJ::DatasetId dataset_id = datasetNamed("nostart.fanoutprobe");
  ASSERT_NE(dataset_id, 0u);
  EXPECT_EQ(singleTopicRowCount(dataset_id), 3);
}

TEST_F(FileLoaderTest, ProgressiveFlushNotifiesStableTopicAndRefreshesRangeEachTime) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"progressive.fanoutprobe"_s);
  const PJ::LoadHints hints = fanoutProbeHints(uR"({"display_suffix":"progressive"})"_s);
  IngestLifecycleRecorder ingest_lifecycle(session());

  int notification_count = 0;
  std::vector<int> progress_steps;
  std::vector<int64_t> committed_rows;
  std::vector<int> notifications_at_step;
  std::vector<double> playback_max_at_step;

  const auto ingest_connection = QObject::connect(
      &session(), &PJ::SessionManager::samplesIngested, loader_.get(),
      [this, &notification_count](const QVector<PJ::TopicId>&, bool live) {
        if (live) {
          return;
        }
        ++notification_count;
        // Mirrors MainWindow's non-live samplesIngested handler: progressive
        // notifications are what make the playback/timeline range grow.
        app_session_->recomputeRange();
      });
  const auto start_connection = QObject::connect(
      loader_.get(), &PJ::FileLoader::ingestStarted, loader_.get(), [](const QString& title, int, int, bool) {
        if (title == u"fanout-probe:progressive"_s) {
          QTimer::singleShot(100, []() { releaseFanoutProbeProgressStep(1); });
        }
      });
  const auto progress_connection = QObject::connect(
      loader_.get(), &PJ::FileLoader::ingestProgress, loader_.get(),
      [this, &notification_count, &progress_steps, &committed_rows, &notifications_at_step, &playback_max_at_step](
          int current, int) {
        const PJ::DatasetId dataset_id = loader_->activeLoadDatasetId();
        progress_steps.push_back(current);
        committed_rows.push_back(singleTopicRowCount(dataset_id));
        notifications_at_step.push_back(notification_count);
        playback_max_at_step.push_back(app_session_->playbackEngine().rangeMax().value);
        if (current < 3) {
          QTimer::singleShot(100, [current]() { releaseFanoutProbeProgressStep(current + 1); });
        }
      });

  const bool loaded = loadAndWait(path, hints);
  QObject::disconnect(ingest_connection);
  QObject::disconnect(start_connection);
  QObject::disconnect(progress_connection);

  ASSERT_TRUE(loaded);
  const std::vector<int> expected_steps{1, 2, 3};
  const std::vector<int64_t> expected_rows{1, 2, 3};
  EXPECT_EQ(progress_steps, expected_steps);
  EXPECT_EQ(committed_rows, expected_rows)
      << "the fixture must prove that all three append-only flushes committed data";
  EXPECT_EQ(notifications_at_step, expected_steps)
      << "samplesIngested must fire for every flush even after the topic count stabilizes";
  ASSERT_EQ(playback_max_at_step.size(), 3U);
  EXPECT_DOUBLE_EQ(playback_max_at_step[0], 100.0e-9);
  EXPECT_DOUBLE_EQ(playback_max_at_step[1], 200.0e-9);
  EXPECT_DOUBLE_EQ(playback_max_at_step[2], 300.0e-9);

  ASSERT_EQ(ingest_lifecycle.began.size(), 1u);
  const IngestLifecycleRecorder::Began& began = ingest_lifecycle.began.front();
  EXPECT_NE(began.token.id, 0u);
  EXPECT_NE(began.token.dataset_id, 0u);
  EXPECT_EQ(began.label, u"fanout-probe:progressive"_s);
  EXPECT_EQ(began.total, 3u);
  EXPECT_TRUE(began.cancellable) << "FileLoader must preserve the plugin's cancellable flag";
  ASSERT_EQ(ingest_lifecycle.progressed.size(), 3u);
  for (std::size_t index = 0; index < ingest_lifecycle.progressed.size(); ++index) {
    const IngestLifecycleRecorder::Progressed& progress = ingest_lifecycle.progressed[index];
    EXPECT_EQ(progress.token.id, began.token.id);
    EXPECT_EQ(progress.token.dataset_id, began.token.dataset_id);
    EXPECT_EQ(progress.current, index + 1);
    EXPECT_EQ(progress.total, 3u);
  }
  ASSERT_EQ(ingest_lifecycle.ended.size(), 1u);
  const auto* ended = ingest_lifecycle.endFor(began.token);
  ASSERT_NE(ended, nullptr);
  EXPECT_EQ(ended->outcome, PJ::IngestOutcome::kCompleted);
  EXPECT_EQ(ingest_lifecycle.beginCountForDataset(began.token.dataset_id), 1u);
  EXPECT_EQ(ingest_lifecycle.endCountForDataset(began.token.dataset_id), 1u);
  EXPECT_FALSE(session().hasActiveIngests());
}

TEST_F(FileLoaderTest, FailedFirstLoadErasesAbandonedLiveDatasetBeforePreferReuseReload) {
  const QString path = makeMockFile(u"failed.mock"_s);

  ASSERT_FALSE(loadWithConfig(path, uR"({"fail_start":true})"_s));

  EXPECT_TRUE(session().createReader().listDatasets().empty())
      << "failed first load must erase the live-engine dataset shell";
  EXPECT_EQ(datasetNamed("failed.mock"), 0u) << "no failed-load shell may remain matchable by basename";

  PJ::LoadHints reuse_hints = loadHints(u"{}"_s);
  reuse_hints.prefer_reuse = true;
  // Old dataset erased → prefer_reuse falls through to a fresh async load; wait for it.
  ASSERT_TRUE(loadAndWait(path, reuse_hints));

  const PJ::DatasetId reloaded = datasetNamed("failed.mock");
  EXPECT_NE(reloaded, 0u) << "prefer_reuse after a failed first load must create a new dataset";
  EXPECT_EQ(singleTopicRowCount(reloaded), 3) << "prefer_reuse must ingest data, not reattach to a shell";
  EXPECT_EQ(catalog().items().size(), 1u) << "curves come back after the successful reload";
}

// Several files enqueued without waiting between them run sequentially on the
// worker; queueDrained fires once when the last completes, and all datasets land.
TEST_F(FileLoaderTest, QueueProcessesEnqueuedLoadsSequentially) {
  const QString a = makeMockFile(u"qa.mock"_s);
  const QString b = makeMockFile(u"qb.mock"_s);
  const QString c = makeMockFile(u"qc.mock"_s);

  int drained = 0;
  QObject::connect(loader_.get(), &PJ::FileLoader::queueDrained, loader_.get(), [&drained]() { ++drained; });

  EXPECT_TRUE(loader_->loadFile(a, nullptr, skipDialogHints()));
  EXPECT_TRUE(loader_->loadFile(b, nullptr, skipDialogHints()));
  EXPECT_TRUE(loader_->loadFile(c, nullptr, skipDialogHints()));

  QEventLoop loop;
  QObject::connect(loader_.get(), &PJ::FileLoader::queueDrained, &loop, &QEventLoop::quit);
  if (loader_->isBusy()) {
    QTimer::singleShot(15000, &loop, [&loop]() { loop.quit(); });
    loop.exec();
  }

  EXPECT_FALSE(loader_->isBusy());
  EXPECT_EQ(session().createReader().listDatasets().size(), 3u) << "all three queued files must load";
  EXPECT_EQ(drained, 1) << "queueDrained fires once when the queue empties";
}

// The stop-confirmation dialog binds to loadGeneration() and must auto-dismiss
// when its load stops being the current one. fileLoaded/fileLoadFailed cannot
// signal that hand-over — they fire while the finished load is still the
// current generation — and queueDrained stays silent while a next load is
// queued. Pin the covering signal: every queued takeover emits
// loadGenerationAdvanced with a fresh generation, before queueDrained.
TEST_F(FileLoaderTest, QueuedNextLoadEmitsLoadGenerationAdvanced) {
  const QString a = makeMockFile(u"gen_a.mock"_s);
  const QString b = makeMockFile(u"gen_b.mock"_s);

  std::vector<std::uint64_t> advances;
  int drained = 0;
  int drained_at_second_advance = -1;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::loadGenerationAdvanced, loader_.get(),
      [&advances, &drained, &drained_at_second_advance](std::uint64_t generation) {
        advances.push_back(generation);
        if (advances.size() == 2) {
          drained_at_second_advance = drained;
        }
      });
  QObject::connect(loader_.get(), &PJ::FileLoader::queueDrained, loader_.get(), [&drained]() { ++drained; });

  EXPECT_TRUE(loader_->loadFile(a, nullptr, skipDialogHints()));
  const std::uint64_t first_generation = loader_->loadGeneration();
  ASSERT_NE(first_generation, 0u);
  EXPECT_TRUE(loader_->loadFile(b, nullptr, skipDialogHints()));  // queued behind a

  ASSERT_EQ(advances.size(), 1u) << "the first load advances the generation as it starts";
  EXPECT_EQ(advances[0], first_generation);

  QEventLoop loop;
  QObject::connect(loader_.get(), &PJ::FileLoader::queueDrained, &loop, &QEventLoop::quit);
  if (loader_->isBusy()) {
    QTimer::singleShot(15000, &loop, [&loop]() { loop.quit(); });
    loop.exec();
  }

  ASSERT_EQ(advances.size(), 2u) << "the queued load's takeover must advance the generation";
  EXPECT_GT(advances[1], first_generation);
  EXPECT_EQ(drained_at_second_advance, 0)
      << "the takeover advance must arrive while the queue is still draining — it is the only "
         "dismissal signal a stale stop dialog gets between back-to-back loads";
}

// A fan-out load finishes its coroutine frame (active_load_ false) one event-loop
// hop BEFORE the queued startNext advances the generation. A generation-gated
// cancel landing in that hop still matches the finished load's generation — it
// must be rejected, or the latched cancel_mode_ silently kills the queued NEXT
// load at its prologue. The Qt::QueuedConnection below runs exactly in that hop
// (posted during the fan-out's fileLoaded emit, ahead of the queued startNext).
TEST_F(FileLoaderTest, StaleGenerationCancelInFanoutEpilogueHopIsNoOp) {
  const QString a = makeMockFile(u"hop_a.mock"_s);
  const QString b = makeMockFile(u"hop_b.mock"_s);

  std::uint64_t stale_generation = 0;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded, loader_.get(),
      [this, &stale_generation](const QString&, const QString&, const QString&, const QString&) {
        if (stale_generation == 0) {
          stale_generation = loader_->loadGeneration();
          loader_->cancelCurrent(stale_generation, /*keep_partial=*/false);  // must be a no-op
        }
      },
      Qt::QueuedConnection);
  int failed_count = 0;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoadFailed, loader_.get(),
      [&failed_count](const QString&, const QString&) { ++failed_count; });

  PJ::LoadHints fanout_hints =
      loadHints(uR"({"__pj_fanout":["{\"display_suffix\":\"left\"}","{\"display_suffix\":\"right\"}"]})"_s);
  EXPECT_TRUE(loader_->loadFile(a, nullptr, fanout_hints));
  EXPECT_TRUE(loader_->loadFile(b, nullptr, skipDialogHints()));

  QEventLoop loop;
  QObject::connect(loader_.get(), &PJ::FileLoader::queueDrained, &loop, &QEventLoop::quit);
  if (loader_->isBusy()) {
    QTimer::singleShot(15000, &loop, [&loop]() { loop.quit(); });
    loop.exec();
  }

  ASSERT_NE(stale_generation, 0u) << "the hop probe never ran";
  EXPECT_EQ(failed_count, 0) << "the stale cancel leaked into a load";
  EXPECT_NE(datasetNamed("hop_b.mock"), 0u) << "the stale-generation cancel must not kill the queued next load";
}

// Two browser uploads sharing a display name and a backing-file basename carry
// DISTINCT opaque pj-upload:// identities: the reload-match loop must key on
// the identity (literal compare), never collapse them by filesystem-path shape
// — and reloading one identity must replace exactly that dataset in place.
TEST_F(FileLoaderTest, BrowserUploadsSharingBasenameStayDistinctAndReloadInPlace) {
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(u"u1"_s));
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(u"u2"_s));
  const QString backing_a = makeMockFile(u"u1/run.mock"_s);
  const QString backing_b = makeMockFile(u"u2/run.mock"_s);
  const QString identity_a = u"pj-upload://session/1/run.mock"_s;
  const QString identity_b = u"pj-upload://session/2/run.mock"_s;

  const auto upload_input = [](const QString& backing, const QString& identity) {
    return PJ::LoadInput{
        .display_name = u"run.mock"_s,
        .backing_path = backing,
        .source_identity = identity,
        .content_sha256 = {},
        .lease = {},
    };
  };
  const auto load_upload = [this, &upload_input](const QString& backing, const QString& identity) {
    QEventLoop loop;
    bool ok = false;
    bool done = false;
    const auto loaded = QObject::connect(
        loader_.get(), &PJ::FileLoader::fileLoaded, &loop,
        [&](const QString&, const QString&, const QString&, const QString&) {
          ok = true;
          done = true;
          loop.quit();
        });
    const auto failed =
        QObject::connect(loader_.get(), &PJ::FileLoader::fileLoadFailed, &loop, [&](const QString&, const QString&) {
          done = true;
          loop.quit();
        });
    EXPECT_TRUE(loader_->loadFile(upload_input(backing, identity), nullptr, skipDialogHints()));
    if (!done) {
      QTimer::singleShot(10000, &loop, [&loop]() { loop.quit(); });
      loop.exec();
    }
    QObject::disconnect(loaded);
    QObject::disconnect(failed);
    return ok;
  };
  const auto datasetWithIdentity = [this](const QString& identity) -> PJ::DatasetId {
    for (const PJ::DatasetId id : session().createReader().listDatasets()) {
      if (loader_->sourcePathForDataset(id) == identity) {
        return id;
      }
    }
    return 0;
  };

  ASSERT_TRUE(load_upload(backing_a, identity_a));
  ASSERT_TRUE(load_upload(backing_b, identity_b));
  EXPECT_EQ(session().createReader().listDatasets().size(), 2u)
      << "same-basename uploads with distinct identities must stay distinct datasets";
  const PJ::DatasetId id_a = datasetWithIdentity(identity_a);
  const PJ::DatasetId id_b = datasetWithIdentity(identity_b);
  ASSERT_NE(id_a, 0u);
  ASSERT_NE(id_b, 0u);
  ASSERT_NE(id_a, id_b);

  // Reload the FIRST identity: an in-place replace of that dataset only.
  ASSERT_TRUE(load_upload(backing_a, identity_a));
  EXPECT_EQ(session().createReader().listDatasets().size(), 2u) << "reloading one identity must not add a dataset";
  EXPECT_EQ(datasetWithIdentity(identity_a), id_a) << "reload must replace the matching dataset in place";
  EXPECT_EQ(datasetWithIdentity(identity_b), id_b) << "the sibling upload must be untouched";
}

// V10: an unusable LoadInput (a trailing-slash directory path → empty fileName,
// so display_name is empty) must NOT fail silently. loadFile returns false AND
// emits fileLoadFailed exactly once with a user-visible reason — MainWindow's
// layout replay relies on that contract ("FileLoader shows its own error on
// failure").
TEST_F(FileLoaderTest, InvalidLoadInputEmitsFileLoadFailedOnce) {
  int failed_count = 0;
  QString failed_path;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoadFailed, loader_.get(), [&](const QString& path, const QString&) {
        ++failed_count;
        failed_path = path;
      });

  // A directory path with a trailing slash: QFileInfo::fileName() is empty, so
  // LoadInput::fromNativePath produces an empty display_name.
  const QString dir_path = data_dir_.path() + u"/"_s;
  const bool accepted = loader_->loadFile(dir_path, nullptr);

  EXPECT_FALSE(accepted) << "an unusable input must be rejected (return false)";
  EXPECT_EQ(failed_count, 1) << "the failure must surface via exactly one fileLoadFailed";
  EXPECT_FALSE(failed_path.isEmpty()) << "the failure must name the offending path";
  EXPECT_FALSE(loader_->isBusy()) << "nothing was enqueued";
}

// V9: two failing loads (unmatched extension → synchronous prologue fail) that
// share a parent must aggregate into ONE warning dialog whose body mentions both
// failures and whose title reads "2 loads failed" — not two overlapping boxes.
TEST_F(FileLoaderTest, ConsecutiveFailuresAggregateIntoOneWarningDialog) {
  QWidget parent;  // owns the aggregated warning dialog (a MessageBox child)

  const QString bad_a = makeMockFile(u"broken_a.unhandledext"_s);
  const QString bad_b = makeMockFile(u"broken_b.unhandledext"_s);

  int failed_count = 0;
  QObject::connect(loader_.get(), &PJ::FileLoader::fileLoadFailed, loader_.get(), [&](const QString&, const QString&) {
    ++failed_count;
  });

  // Both extensions have no matching plugin → the prologue's fail() runs
  // synchronously (before any dialog await) and routes through reportLoadWarning.
  EXPECT_TRUE(loader_->loadFile(bad_a, &parent));
  EXPECT_TRUE(loader_->loadFile(bad_b, &parent));

  // The second load is dequeued via a queued startNext; pump until both fail.
  QEventLoop loop;
  QTimer::singleShot(5000, &loop, &QEventLoop::quit);
  QObject::connect(loader_.get(), &PJ::FileLoader::queueDrained, &loop, &QEventLoop::quit);
  if (loader_->isBusy()) {
    loop.exec();
  }
  EXPECT_EQ(failed_count, 2) << "both loads must fail";

  // Exactly one warning dialog (a MessageBox), parented to `parent`, showing both.
  QList<PJ::MessageBox*> warnings;
  for (PJ::MessageBox* box : parent.findChildren<PJ::MessageBox*>()) {
    if (box->isVisible()) {
      warnings << box;
    }
  }
  ASSERT_EQ(warnings.size(), 1) << "N failures must yield ONE aggregated dialog, not N stacked boxes";

  PJ::MessageBox* warning = warnings.front();
  // setTitle() mirrors onto windowTitle(); "2 loads failed" reflects the count.
  EXPECT_TRUE(warning->windowTitle().contains(u"2"_s))
      << "aggregated title must reflect two failures, got: " << warning->windowTitle().toStdString();

  // The aggregated body (a QLabel) must mention BOTH failing extensions.
  QString body;
  for (QLabel* label : warning->findChildren<QLabel*>()) {
    body += label->text();
  }
  EXPECT_TRUE(body.contains(u".unhandledext"_s)) << "body must name the failed loads";

  warning->reject();  // dismiss so the fixture tears down cleanly
}

// Tearing the loader down mid-load (the closeEvent path) must join the worker
// and drain the queue without crashing or hanging; the loader stays usable after.
TEST_F(FileLoaderTest, JoinForShutdownDuringLoadDoesNotCrash) {
  EXPECT_TRUE(loader_->loadFile(mock_path_, nullptr, skipDialogHints()));
  loader_->joinForShutdown();  // worker may still be running
  EXPECT_FALSE(loader_->isBusy());

  // The loader recovers: a fresh load after shutdown still completes.
  EXPECT_TRUE(load());
  EXPECT_NE(datasetNamed("sensors.mock"), 0u);
}

// A REPLACING reload is transactional: if the reload fails after the prior data was
// detached, the RefillGuard rolls back to that prior data instead of leaving the
// dataset empty. Before this guard, the up-front in-place clear destroyed the prior
// data with no recovery — the regression this fixes (codex #1 on PR #246).
// The success/commit path is covered by ReloadingSameFileReplacesDatasetInPlace
// above (it now routes through beginRefill + commit); commit-vs-rollback SEMANTICS
// are pinned deterministically at the SessionManager layer (the mock writes the same
// 3 rows every load, so the two are indistinguishable by row count here).
TEST_F(FileLoaderTest, ReplacingReloadStartFailureRestoresPriorData) {
  ASSERT_TRUE(load());  // loads sensors.mock (3 rows)
  const PJ::DatasetId id = datasetNamed("sensors.mock");
  ASSERT_NE(id, 0u);
  ASSERT_EQ(singleTopicRowCount(id), 3);

  // Reload the SAME file with a configured start() failure. beginRefill detaches the
  // prior data up front; start() then fails on the worker, so onWorkerFinished's
  // replacing start-fail arm must ROLL BACK to the prior data, not leave it empty.
  EXPECT_FALSE(loadWithConfig(mock_path_, uR"({"fail_start":true})"_s));

  EXPECT_EQ(datasetNamed("sensors.mock"), id) << "DatasetId stays stable across a failed reload";
  EXPECT_EQ(singleTopicRowCount(id), 3) << "prior data restored, NOT left empty";
  EXPECT_EQ(catalog().items().size(), 1u) << "curve tree restored after the failed reload";
}

// Tearing the loader down mid REPLACING-reload discards the in-flight reload and
// rolls back to the prior data (joinForShutdown captures was_replacing before
// ctx_.reset(), whose guard dtor performs the rollback).
//
// The reload deliberately writes DIFFERENT rows than the load it replaces, and
// parks after flushing them: the shutdown therefore lands with partial
// replacement data committed in the engine, and a defect that COMMITTED that
// data instead of rolling it back changes the values the dataset ends up with.
// With identical content on both sides the two outcomes are indistinguishable.
TEST_F(FileLoaderTest, JoinForShutdownDuringReplacingReloadRestoresThePriorContentNotTheReplacement) {
  ASSERT_TRUE(installReloadProbe());
  const QString path = makeMockFile(u"reloaded.reloadprobe"_s);
  ASSERT_TRUE(loadAndWait(path, reloadProbeHints(uR"({"first_value":10,"rows":3})"_s)));
  const PJ::DatasetId id = datasetNamed("reloaded.reloadprobe");
  ASSERT_NE(id, 0u);
  const std::vector<double> original_values{11.0, 12.0, 13.0};
  ASSERT_EQ(singleTopicValues(id), original_values);
  const std::size_t catalog_items_before = catalog().items().size();

  // Replacing reload of the same path, parked once its own (different) rows are
  // in the engine.
  ASSERT_TRUE(loader_->loadFile(path, nullptr, reloadProbeHints(uR"({"first_value":700,"rows":2,"park":true})"_s)));
  ASSERT_TRUE(pj_app_test::pumpUntil([]() { return reloadProbeParked(); }))
      << "the replacing reload never reached its rendezvous";

  loader_->joinForShutdown();  // shut down before it finalizes

  EXPECT_FALSE(loader_->isBusy());
  EXPECT_EQ(datasetNamed("reloaded.reloadprobe"), id) << "DatasetId stable across shutdown-mid-reload";
  EXPECT_EQ(singleTopicValues(id), original_values)
      << "the shutdown must roll the reload back to the prior content, not commit the partial replacement";
  EXPECT_EQ(catalog().items().size(), catalog_items_before) << "the curve tree comes back with the data";

  // The loader recovers: a fresh load after shutdown still completes, and it is
  // the one that finally replaces the content.
  EXPECT_TRUE(loadAndWait(path, reloadProbeHints(uR"({"first_value":50,"rows":1})"_s)));
  EXPECT_EQ(singleTopicValues(datasetNamed("reloaded.reloadprobe")), (std::vector<double>{51.0}));
}

// ---------------------------------------------------------------------------
// LoadTicket: request-scoped load tracking. loadFileTicketed mints a ticket at
// enqueue; every ACCEPTED request must resolve with EXACTLY ONE loadFinished —
// on the success, failure, cancel(queued), cancel(active), dialog-reject, and
// shutdown paths alike. cancelLoad targets one request by ticket.
// ---------------------------------------------------------------------------

namespace {

// Deliver queued loadFinished terminals: two zero-timer loop passes, because a
// terminal may itself be scheduled from another queued event (a worker's
// completion metacall, a cross-thread cancel marshal).
void flushQueuedTerminals() {
  pj_app_test::flushQueuedEvents(2);
}

// Pump the event loop until the loader's queue drains (bounded, so a regression
// fails instead of hanging CI), then deliver any queued terminals.
void pumpUntilIdle(PJ::FileLoader& loader) {
  QEventLoop loop;
  QObject::connect(&loader, &PJ::FileLoader::queueDrained, &loop, &QEventLoop::quit);
  if (loader.isBusy()) {
    QTimer::singleShot(15000, &loop, [&loop]() { loop.quit(); });
    loop.exec();
  }
  flushQueuedTerminals();
}

}  // namespace

TEST_F(FileLoaderTest, TicketedLoadReportsLoadedExactlyOnce) {
  LoadFinishedRecorder recorder(*loader_);
  IngestLifecycleRecorder ingest_lifecycle(session());
  int legacy_loaded = 0;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded, loader_.get(),
      [&](const QString&, const QString&, const QString&, const QString&) { ++legacy_loaded; });

  const quint64 ticket = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(mock_path_), nullptr, loadHints());
  ASSERT_NE(ticket, 0u);
  EXPECT_EQ(loader_->currentLoadTicket(), ticket) << "the accepted request is the current load";
  pumpUntilIdle(*loader_);

  ASSERT_EQ(recorder.events.size(), 1u) << "exactly one terminal per accepted request";
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kLoaded);
  EXPECT_EQ(recorder.events[0].path, mock_path_);
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  EXPECT_EQ(recorder.events[0].dataset_id, dataset_id) << "the terminal must carry the loaded primary dataset";
  EXPECT_EQ(recorder.events[0].produced, QVector<PJ::DatasetId>{dataset_id});
  EXPECT_EQ(legacy_loaded, 1) << "legacy fileLoaded stays untouched";
  EXPECT_EQ(loader_->currentLoadTicket(), 0u) << "idle again after the drain";

  ASSERT_EQ(ingest_lifecycle.began.size(), 1u);
  const IngestLifecycleRecorder::Began& began = ingest_lifecycle.began.front();
  EXPECT_NE(began.token.id, 0u);
  EXPECT_EQ(began.token.dataset_id, dataset_id);
  EXPECT_EQ(began.label, u"Importing"_s);
  EXPECT_EQ(began.total, 3u);
  EXPECT_TRUE(began.cancellable);
  ASSERT_EQ(ingest_lifecycle.ended.size(), 1u);
  const auto* ended = ingest_lifecycle.endFor(began.token);
  ASSERT_NE(ended, nullptr);
  EXPECT_EQ(ended->outcome, PJ::IngestOutcome::kCompleted);
  EXPECT_EQ(ingest_lifecycle.beginCountForDataset(dataset_id), 1u);
  EXPECT_EQ(ingest_lifecycle.endCountForDataset(dataset_id), 1u);
  EXPECT_FALSE(session().hasActiveIngests());
}

TEST_F(FileLoaderTest, TicketedLoadRejectionReturnsZeroWithoutLoadFinished) {
  LoadFinishedRecorder recorder(*loader_);
  int legacy_failed = 0;
  QObject::connect(loader_.get(), &PJ::FileLoader::fileLoadFailed, loader_.get(), [&](const QString&, const QString&) {
    ++legacy_failed;
  });

  // Trailing-slash directory → empty display_name → rejected at enqueue.
  const QString dir_path = data_dir_.path() + u"/"_s;
  EXPECT_EQ(loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(dir_path)), 0u);

  flushQueuedTerminals();
  EXPECT_TRUE(recorder.events.empty()) << "a rejected request was never accepted, so no loadFinished";
  EXPECT_EQ(legacy_failed, 1) << "the legacy rejection surface stays intact";
  EXPECT_FALSE(loader_->isBusy());
}

TEST_F(FileLoaderTest, TicketedSynchronousPrologueFailureReportsFailedExactlyOnce) {
  LoadFinishedRecorder recorder(*loader_);
  const QString bad = makeMockFile(u"ticket.unhandledext"_s);

  const quint64 ticket = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(bad));
  ASSERT_NE(ticket, 0u) << "an enqueued request is accepted even if its prologue fails";
  EXPECT_TRUE(recorder.events.empty())
      << "terminals are queued: even a synchronous prologue failure must not resolve before enqueue returns";
  pumpUntilIdle(*loader_);

  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kFailed);
  EXPECT_EQ(recorder.events[0].path, bad);
  EXPECT_EQ(recorder.events[0].dataset_id, 0u);
  EXPECT_TRUE(recorder.events[0].produced.isEmpty());
}

TEST_F(FileLoaderTest, TicketedWorkerStartFailureReportsFailedExactlyOnce) {
  LoadFinishedRecorder recorder(*loader_);

  const quint64 ticket = loader_->loadFileTicketed(
      PJ::LoadInput::fromNativePath(mock_path_), nullptr, loadHints(uR"({"fail_start":true})"_s));
  ASSERT_NE(ticket, 0u);
  pumpUntilIdle(*loader_);

  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kFailed);
  EXPECT_EQ(recorder.events[0].dataset_id, 0u) << "a failed load leaves no dataset behind";
}

TEST_F(FileLoaderTest, SingleInstanceFailureAfterBeginEndsIngestAsFailed) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"single_fail.fanoutprobe"_s);
  LoadFinishedRecorder load_recorder(*loader_);
  IngestLifecycleRecorder ingest_lifecycle(session());

  const quint64 ticket = loader_->loadFileTicketed(
      PJ::LoadInput::fromNativePath(path), nullptr, fanoutProbeHints(uR"({"display_suffix":"fail"})"_s));
  ASSERT_NE(ticket, 0u);
  pumpUntilIdle(*loader_);

  ASSERT_EQ(load_recorder.events.size(), 1u);
  EXPECT_EQ(load_recorder.events.front().outcome, PJ::LoadOutcome::kFailed);
  ASSERT_EQ(ingest_lifecycle.began.size(), 1u);
  ASSERT_EQ(ingest_lifecycle.ended.size(), 1u);
  const IngestLifecycleRecorder::Began& began = ingest_lifecycle.began.front();
  EXPECT_EQ(began.label, u"fanout-probe:fail"_s);
  EXPECT_TRUE(began.cancellable);
  const auto* ended = ingest_lifecycle.endFor(began.token);
  ASSERT_NE(ended, nullptr);
  EXPECT_EQ(ended->outcome, PJ::IngestOutcome::kFailed);
  EXPECT_EQ(ingest_lifecycle.beginCountForDataset(began.token.dataset_id), 1u);
  EXPECT_EQ(ingest_lifecycle.endCountForDataset(began.token.dataset_id), 1u);
  EXPECT_FALSE(session().hasActiveIngests());
}

TEST_F(FileLoaderTest, CancelQueuedTicketResolvesCancelledWithoutStarting) {
  const QString a = makeMockFile(u"ticket_qa.mock"_s);
  const QString b = makeMockFile(u"ticket_qb.mock"_s);
  LoadFinishedRecorder recorder(*loader_);

  const quint64 ticket_a = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(a), nullptr, loadHints());
  const quint64 ticket_b = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(b), nullptr, loadHints());
  ASSERT_NE(ticket_a, 0u);
  ASSERT_NE(ticket_b, 0u);
  EXPECT_GT(ticket_b, ticket_a) << "tickets are minted monotonically at enqueue";
  EXPECT_EQ(loader_->currentLoadTicket(), ticket_a) << "b is queued behind the active a";

  EXPECT_TRUE(loader_->cancelLoad(ticket_b));
  EXPECT_TRUE(recorder.events.empty()) << "the queued-cancel terminal is delivered via the event loop, never inline";
  EXPECT_FALSE(loader_->cancelLoad(ticket_b)) << "cancelling an already-terminal ticket is a no-op";

  pumpUntilIdle(*loader_);

  EXPECT_EQ(recorder.countForTicket(ticket_a), 1u);
  EXPECT_EQ(recorder.countForTicket(ticket_b), 1u);
  ASSERT_EQ(recorder.events.size(), 2u);
  // b's terminal was scheduled at cancel time, before a's completion could
  // schedule its own — delivery keeps that order.
  EXPECT_EQ(recorder.events[0].ticket, ticket_b);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kCancelled);
  EXPECT_EQ(recorder.events[0].path, b);
  EXPECT_EQ(recorder.events[0].dataset_id, 0u);
  EXPECT_TRUE(recorder.events[0].produced.isEmpty());
  EXPECT_EQ(recorder.events[1].ticket, ticket_a);
  EXPECT_EQ(recorder.events[1].outcome, PJ::LoadOutcome::kLoaded);
  EXPECT_EQ(datasetNamed("ticket_qb.mock"), 0u) << "the cancelled queued request must never load";
}

// Discard means "as if this load had never run", so what it leaves behind is
// whatever was there BEFORE it: on a REPLACING reload that is the pre-reload
// data, restored in place. (A first-load discard has nothing prior and deletes
// the dataset — CancelActiveTicketDiscardReportsCancelledExactlyOnce.)
TEST_F(FileLoaderTest, ReplacingReloadDiscardRestoresPriorData) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"reload_discard.fanoutprobe"_s);
  LoadFinishedRecorder recorder(*loader_);

  // First load: a plain entry that runs to completion, so the dataset exists.
  // The "cancel" entry cannot serve here — it reports a failed start whether or
  // not the rendezvous cancels, so it never commits a dataset to reload.
  ASSERT_TRUE(loadAndWait(path, fanoutProbeHints(uR"({"display_suffix":"initial"})"_s)));
  const PJ::DatasetId dataset_id = datasetNamed("reload_discard.fanoutprobe");
  ASSERT_NE(dataset_id, 0u) << "the first load must have committed a dataset";
  ASSERT_EQ(session().createReader().listDatasets().size(), 1u);
  ASSERT_EQ(singleTopicRowCount(dataset_id), 3) << "the first load must have committed its rows";
  const std::size_t catalog_items_before = catalog().items().size();
  ASSERT_GT(catalog_items_before, 0u);

  // Reload the same file (replaces in place) and discard mid-flight. The probe's
  // cancel entry appends one row before parking, so a failure to roll back shows
  // up as a row count of 1 rather than the restored 3.
  bool cancel_sent = false;
  const auto cancel_connection = QObject::connect(
      &session(), &PJ::SessionManager::ingestBegan, loader_.get(),
      [this, &cancel_sent](PJ::IngestToken token, const QString& label, quint64) {
        if (label != u"fanout-probe:cancel"_s || cancel_sent) {
          return;
        }
        cancel_sent = true;
        EXPECT_TRUE(session().requestCancel(token, /*keep_partial=*/false));
        releaseFanoutProbeCancelEntry();
      });
  const quint64 reload_ticket = loader_->loadFileTicketed(
      PJ::LoadInput::fromNativePath(path), nullptr, fanoutProbeHints(uR"({"display_suffix":"cancel"})"_s));
  ASSERT_NE(reload_ticket, 0u);
  pumpUntilIdle(*loader_);
  QObject::disconnect(cancel_connection);
  ASSERT_TRUE(cancel_sent) << "the reload never reached its cancellation rendezvous";

  // The dataset survives with exactly its pre-reload contents, and none of the
  // abandoned reload's partial rows.
  EXPECT_EQ(datasetNamed("reload_discard.fanoutprobe"), dataset_id) << "the dataset must survive with its id";
  EXPECT_TRUE(engineHasDataset(dataset_id));
  EXPECT_EQ(session().createReader().listDatasets().size(), 1u) << "the discard must not mint a second dataset";
  EXPECT_EQ(singleTopicRowCount(dataset_id), 3) << "the pre-reload rows must be restored, not the partial refill";
  EXPECT_EQ(catalog().items().size(), catalog_items_before) << "the catalog row must survive the discard";
  EXPECT_FALSE(session().hasActiveIngests());
  EXPECT_FALSE(loader_->isBusy());

  // Exactly one terminal for the reload, reported as cancelled.
  ASSERT_EQ(recorder.countForTicket(reload_ticket), 1u);
  const auto reload_event = std::find_if(
      recorder.events.begin(), recorder.events.end(),
      [reload_ticket](const auto& event) { return event.ticket == reload_ticket; });
  ASSERT_NE(reload_event, recorder.events.end());
  EXPECT_EQ(reload_event->outcome, PJ::LoadOutcome::kCancelled);
}

TEST_F(FileLoaderTest, CancelActiveTicketDiscardReportsCancelledExactlyOnce) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"ticket_cancel.fanoutprobe"_s);
  LoadFinishedRecorder recorder(*loader_);
  IngestLifecycleRecorder ingest_lifecycle(session());

  quint64 ticket = 0;
  bool cancel_sent = false;
  const auto cancel_connection = QObject::connect(
      &session(), &PJ::SessionManager::ingestBegan, loader_.get(),
      [this, &cancel_sent](PJ::IngestToken token, const QString& label, quint64) {
        if (label != u"fanout-probe:cancel"_s || cancel_sent) {
          return;
        }
        cancel_sent = true;
        EXPECT_TRUE(session().requestCancel(token, /*keep_partial=*/false))
            << "the FileLoader beginIngest cancel handler must route to cancelCurrent";
        releaseFanoutProbeCancelEntry();
      });

  // Single-instance probe entry that parks on the worker until the cancel
  // rendezvous releases it — the cancel deterministically hits an ACTIVE load.
  ticket = loader_->loadFileTicketed(
      PJ::LoadInput::fromNativePath(path), nullptr, fanoutProbeHints(uR"({"display_suffix":"cancel"})"_s));
  ASSERT_NE(ticket, 0u);
  pumpUntilIdle(*loader_);
  QObject::disconnect(cancel_connection);

  ASSERT_TRUE(cancel_sent) << "the probe never reached its cancellation rendezvous";
  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kCancelled);
  EXPECT_EQ(recorder.events[0].path, path);
  EXPECT_EQ(recorder.events[0].dataset_id, 0u) << "a discarded load reports no dataset";
  EXPECT_TRUE(recorder.events[0].produced.isEmpty());
  EXPECT_EQ(datasetNamed("ticket_cancel.fanoutprobe"), 0u) << "discard must drop the partial dataset";
  EXPECT_FALSE(loader_->cancelLoad(ticket)) << "cancelling an already-terminal ticket is a no-op";

  ASSERT_EQ(ingest_lifecycle.began.size(), 1u);
  const IngestLifecycleRecorder::Began& began = ingest_lifecycle.began.front();
  EXPECT_TRUE(began.cancellable);
  ASSERT_EQ(ingest_lifecycle.ended.size(), 1u);
  const auto* ended = ingest_lifecycle.endFor(began.token);
  ASSERT_NE(ended, nullptr);
  EXPECT_EQ(ended->outcome, PJ::IngestOutcome::kCancelled);
  EXPECT_EQ(ingest_lifecycle.beginCountForDataset(began.token.dataset_id), 1u);
  EXPECT_EQ(ingest_lifecycle.endCountForDataset(began.token.dataset_id), 1u);
  EXPECT_FALSE(session().hasActiveIngests());
}

TEST_F(FileLoaderTest, CancelActiveTicketKeepPartialReportsCancelledWithKeptDataset) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path = makeMockFile(u"ticket_keep.fanoutprobe"_s);
  LoadFinishedRecorder recorder(*loader_);
  IngestLifecycleRecorder ingest_lifecycle(session());
  int legacy_loaded = 0;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded, loader_.get(),
      [&](const QString&, const QString&, const QString&, const QString&) { ++legacy_loaded; });
  int committing = 0;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::loadCommitting, loader_.get(),
      [&](quint64, const QVector<PJ::DatasetId>&, const QString&, const QString&) { ++committing; });

  quint64 ticket = 0;
  bool cancel_sent = false;
  const auto cancel_connection = QObject::connect(
      &session(), &PJ::SessionManager::ingestBegan, loader_.get(),
      [this, &cancel_sent](PJ::IngestToken token, const QString& label, quint64) {
        if (label != u"fanout-probe:cancel"_s || cancel_sent) {
          return;
        }
        cancel_sent = true;
        EXPECT_TRUE(session().requestCancel(token, /*keep_partial=*/true))
            << "the FileLoader beginIngest cancel handler must preserve keep_partial";
        releaseFanoutProbeCancelEntry();
      });

  ticket = loader_->loadFileTicketed(
      PJ::LoadInput::fromNativePath(path), nullptr, fanoutProbeHints(uR"({"display_suffix":"cancel"})"_s));
  ASSERT_NE(ticket, 0u);
  pumpUntilIdle(*loader_);
  QObject::disconnect(cancel_connection);

  ASSERT_TRUE(cancel_sent) << "the probe never reached its cancellation rendezvous";
  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kCancelled) << "a user stop is Cancelled even when kept";
  const PJ::DatasetId kept = recorder.events[0].dataset_id;
  ASSERT_NE(kept, 0u) << "keep-partial reports the dataset holding the kept rows";
  EXPECT_EQ(recorder.events[0].produced, QVector<PJ::DatasetId>{kept});
  EXPECT_EQ(singleTopicRowCount(kept), 1) << "the partial row parsed before the stop is kept";
  EXPECT_EQ(legacy_loaded, 1) << "keep-partial still finalizes through the legacy fileLoaded";
  EXPECT_EQ(committing, 0) << "the pre-catalog commit seam fires only for fully-loaded requests";

  ASSERT_EQ(ingest_lifecycle.began.size(), 1u);
  const IngestLifecycleRecorder::Began& began = ingest_lifecycle.began.front();
  EXPECT_TRUE(began.cancellable);
  EXPECT_EQ(began.token.dataset_id, kept);
  ASSERT_EQ(ingest_lifecycle.ended.size(), 1u);
  const auto* ended = ingest_lifecycle.endFor(began.token);
  ASSERT_NE(ended, nullptr);
  EXPECT_EQ(ended->outcome, PJ::IngestOutcome::kCancelled);
  EXPECT_EQ(ingest_lifecycle.beginCountForDataset(kept), 1u);
  EXPECT_EQ(ingest_lifecycle.endCountForDataset(kept), 1u);
  EXPECT_FALSE(session().hasActiveIngests());
}

TEST_F(FileLoaderTest, CancelUnknownTicketIsANoOp) {
  LoadFinishedRecorder recorder(*loader_);
  EXPECT_FALSE(loader_->cancelLoad(0));
  EXPECT_FALSE(loader_->cancelLoad(424242));
  flushQueuedTerminals();
  EXPECT_TRUE(recorder.events.empty());
}

TEST_F(FileLoaderTest, JoinForShutdownReportsCancelledForActiveAndQueuedTickets) {
  const QString a = makeMockFile(u"ticket_sa.mock"_s);
  const QString b = makeMockFile(u"ticket_sb.mock"_s);
  LoadFinishedRecorder recorder(*loader_);

  const quint64 ticket_a = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(a), nullptr, loadHints());
  const quint64 ticket_b = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(b), nullptr, loadHints());
  ASSERT_NE(ticket_a, 0u);
  ASSERT_NE(ticket_b, 0u);

  loader_->joinForShutdown();  // discards the active load and drains the queue

  // Shutdown is the deliberate exception to queued terminal delivery: teardown
  // must not lose terminals, so they are flushed synchronously here.
  ASSERT_EQ(recorder.events.size(), 2u) << "shutdown must resolve every accepted request before returning";
  EXPECT_EQ(recorder.events[0].ticket, ticket_a);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kCancelled);
  EXPECT_EQ(recorder.events[0].path, a);
  EXPECT_EQ(recorder.events[1].ticket, ticket_b);
  EXPECT_EQ(recorder.events[1].outcome, PJ::LoadOutcome::kCancelled);
  EXPECT_EQ(recorder.events[1].path, b);
  EXPECT_FALSE(loader_->isBusy());

  // The loader stays usable after shutdown, and the fresh request gets a fresh
  // terminal of its own.
  const quint64 ticket_c = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(mock_path_), nullptr, loadHints());
  ASSERT_NE(ticket_c, 0u);
  pumpUntilIdle(*loader_);
  EXPECT_EQ(recorder.countForTicket(ticket_c), 1u);
  ASSERT_EQ(recorder.events.size(), 3u);
  EXPECT_EQ(recorder.events[2].outcome, PJ::LoadOutcome::kLoaded);
}

// Review finding 1: ~FileLoader used to purge queued terminal emissions, so
// accepted loads got NO terminal when the loader died without another event
// loop pass. Destruction must deliver every outstanding terminal.
TEST_F(FileLoaderTest, DestroyWithoutPumpingDeliversAllTerminals) {
  const QString a = makeMockFile(u"ticket_da.mock"_s);
  const QString b = makeMockFile(u"ticket_db.mock"_s);
  LoadFinishedRecorder recorder(*loader_);

  const quint64 ticket_a = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(a), nullptr, loadHints());
  const quint64 ticket_b = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(b), nullptr, loadHints());
  ASSERT_NE(ticket_a, 0u);
  ASSERT_NE(ticket_b, 0u);

  loader_.reset();  // destructor: active worker + queued request, NO event-loop pass

  ASSERT_EQ(recorder.events.size(), 2u) << "destruction must deliver a terminal for every accepted request";
  EXPECT_EQ(recorder.events[0].ticket, ticket_a);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kCancelled);
  EXPECT_EQ(recorder.events[1].ticket, ticket_b);
  EXPECT_EQ(recorder.events[1].outcome, PJ::LoadOutcome::kCancelled);
}

// Review finding 1, resolved-but-undelivered flavor: a terminal that was
// already scheduled (the load resolved) but not yet delivered must be flushed
// by destruction with its TRUE outcome — not dropped, not rewritten to
// kCancelled.
TEST_F(FileLoaderTest, DestroyDeliversResolvedButUndeliveredTerminal) {
  const QString bad = makeMockFile(u"ticket_dd.unhandledext"_s);
  LoadFinishedRecorder recorder(*loader_);

  const quint64 ticket = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(bad));
  ASSERT_NE(ticket, 0u);
  EXPECT_TRUE(recorder.events.empty()) << "the sync failure resolves after enqueue returns";

  loader_.reset();  // destructor before any event-loop pass

  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kFailed) << "the resolved outcome survives teardown";
}

// Codex residual R1: the destructor's synchronous terminal flush delivers
// loadFinished while the loader is mid-destruction, and joinForShutdown has
// already restored reusability (shutting_down_ false) — so a slot could
// legally start NEW work whose terminal nothing would ever resolve and whose
// worker would outlive the loader. Admission during destruction must be
// rejected outright: ticket 0, nothing enqueued, destruction stays clean.
TEST_F(FileLoaderTest, LoadRequestedDuringDestructorFlushIsRejected) {
  const QString a = makeMockFile(u"ticket_destroy.mock"_s);
  LoadFinishedRecorder recorder(*loader_);
  PJ::FileLoader* raw = loader_.get();
  quint64 reentrant_ticket = 42;  // sentinel: must become 0
  QObject::connect(
      raw, &PJ::FileLoader::loadFinished, raw,
      [&, raw](quint64, PJ::LoadOutcome, const QString&, PJ::DatasetId, const QVector<PJ::DatasetId>&) {
        reentrant_ticket = raw->loadFileTicketed(PJ::LoadInput::fromNativePath(mock_path_), nullptr, loadHints());
      });

  const quint64 ticket_a = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(a), nullptr, loadHints());
  ASSERT_NE(ticket_a, 0u);

  loader_.reset();  // destructor flush delivers a's terminal; the slot tries to start new work

  EXPECT_EQ(reentrant_ticket, 0u) << "admission during destruction must be rejected";
  EXPECT_EQ(recorder.countForTicket(ticket_a), 1u);
  EXPECT_EQ(recorder.events.size(), 1u) << "no terminal may be lost, and none minted for the rejected request";
}

// Review finding 2: legacy fileLoadFailed/fileLoaded fire before the ticket
// used to be marked terminal, so a direct slot re-entering joinForShutdown
// recorded a shutdown kCancelled AND the original path then scheduled its own
// kFailed — two terminals. The ticket must be terminalized before any
// externally re-entrant legacy emission.
TEST_F(FileLoaderTest, ReentrantShutdownFromLegacyFailureSlotYieldsOneTerminal) {
  const QString bad = makeMockFile(u"ticket_reentrant.unhandledext"_s);
  LoadFinishedRecorder recorder(*loader_);
  int shutdowns = 0;
  QObject::connect(loader_.get(), &PJ::FileLoader::fileLoadFailed, loader_.get(), [&](const QString&, const QString&) {
    ++shutdowns;
    loader_->joinForShutdown();
  });

  const quint64 ticket = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(bad));
  ASSERT_NE(ticket, 0u);
  ASSERT_EQ(shutdowns, 1) << "the failure slot must have re-entered shutdown";
  pumpUntilIdle(*loader_);

  ASSERT_EQ(recorder.countForTicket(ticket), recorder.events.size());
  ASSERT_EQ(recorder.events.size(), 1u) << "a re-entrant shutdown must not add a second terminal";
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kFailed)
      << "the original failure outcome wins over the re-entrant shutdown's cancel";
}

// The stop-dialog binds to loadGeneration(); currentLoadTicket() must expose
// the ticket that generation belongs to at every loadGenerationAdvanced, so a
// ticket can never be attributed to a later load's generation.
TEST_F(FileLoaderTest, CurrentLoadTicketTracksEachGenerationAdvance) {
  const QString a = makeMockFile(u"ticket_ga.mock"_s);
  const QString b = makeMockFile(u"ticket_gb.mock"_s);

  std::vector<std::pair<std::uint64_t, quint64>> advances;  // (generation, ticket)
  QObject::connect(
      loader_.get(), &PJ::FileLoader::loadGenerationAdvanced, loader_.get(),
      [&](std::uint64_t generation) { advances.emplace_back(generation, loader_->currentLoadTicket()); });

  const quint64 ticket_a = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(a), nullptr, loadHints());
  const quint64 ticket_b = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(b), nullptr, loadHints());
  ASSERT_NE(ticket_a, 0u);
  ASSERT_NE(ticket_b, 0u);
  pumpUntilIdle(*loader_);

  ASSERT_EQ(advances.size(), 2u);
  EXPECT_EQ(advances[0].second, ticket_a);
  EXPECT_EQ(advances[1].second, ticket_b);
  EXPECT_NE(advances[0].first, advances[1].first) << "each ticket maps to its own generation";
}

// The prefer_reuse fast path resolves without a worker; its terminal must
// still be delivered through the event loop, never inside loadFileTicketed.
TEST_F(FileLoaderTest, PreferReuseTerminalArrivesAsynchronously) {
  ASSERT_TRUE(load());     // sensors.mock, tracked path
  flushQueuedTerminals();  // deliver the setup load's terminal before recording
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  LoadFinishedRecorder recorder(*loader_);

  const quint64 ticket = loader_->loadFileTicketed(
      PJ::LoadInput::fromNativePath(mock_path_), nullptr, loadHints(u"{}"_s, /*prefer_reuse=*/true));
  ASSERT_NE(ticket, 0u);
  EXPECT_TRUE(recorder.events.empty()) << "the synchronous reuse path must not resolve before enqueue returns";
  pumpUntilIdle(*loader_);

  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kLoaded);
  EXPECT_EQ(recorder.events[0].dataset_id, dataset_id);
  EXPECT_EQ(recorder.events[0].produced, QVector<PJ::DatasetId>{dataset_id});
}

// requestStarted announces the (ticket, generation) association the moment a
// request becomes current, immediately before that generation's
// loadGenerationAdvanced.
TEST_F(FileLoaderTest, RequestStartedPairsTicketWithItsGeneration) {
  const QString a = makeMockFile(u"ticket_ra.mock"_s);
  const QString b = makeMockFile(u"ticket_rb.mock"_s);

  enum class Kind { kStarted, kAdvanced };
  std::vector<Kind> order;
  std::vector<std::pair<quint64, std::uint64_t>> starts;  // (ticket, generation)
  std::vector<std::uint64_t> advances;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::requestStarted, loader_.get(), [&](quint64 ticket, std::uint64_t generation) {
        order.push_back(Kind::kStarted);
        starts.emplace_back(ticket, generation);
      });
  QObject::connect(
      loader_.get(), &PJ::FileLoader::loadGenerationAdvanced, loader_.get(), [&](std::uint64_t generation) {
        order.push_back(Kind::kAdvanced);
        advances.push_back(generation);
      });

  const quint64 ticket_a = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(a), nullptr, loadHints());
  const quint64 ticket_b = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(b), nullptr, loadHints());
  ASSERT_NE(ticket_a, 0u);
  ASSERT_NE(ticket_b, 0u);
  pumpUntilIdle(*loader_);

  const std::vector<Kind> expected_order{Kind::kStarted, Kind::kAdvanced, Kind::kStarted, Kind::kAdvanced};
  EXPECT_EQ(order, expected_order) << "requestStarted precedes its generation's loadGenerationAdvanced";
  ASSERT_EQ(starts.size(), 2u);
  ASSERT_EQ(advances.size(), 2u);
  EXPECT_EQ(starts[0].first, ticket_a);
  EXPECT_EQ(starts[1].first, ticket_b);
  EXPECT_EQ(starts[0].second, advances[0]);
  EXPECT_EQ(starts[1].second, advances[1]);
  EXPECT_GT(starts[1].second, starts[0].second);
}

// The one FileLoader entry a batch worker may hit off-thread: cancelLoadAsync
// marshals to the loader's thread and the cancel resolves through the normal
// queued terminal.
TEST_F(FileLoaderTest, CancelLoadAsyncFromWorkerThreadCancelsQueuedTicket) {
  ASSERT_TRUE(installFanoutProbe());
  const QString path_a = makeMockFile(u"ticket_async.fanoutprobe"_s);
  const QString path_b = makeMockFile(u"ticket_async_b.mock"_s);
  LoadFinishedRecorder recorder(*loader_);

  // A parks on the worker at the cancel rendezvous, so B stays QUEUED while
  // the off-thread cancel runs.
  const quint64 ticket_a = loader_->loadFileTicketed(
      PJ::LoadInput::fromNativePath(path_a), nullptr, fanoutProbeHints(uR"({"display_suffix":"cancel"})"_s));
  const quint64 ticket_b = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(path_b), nullptr, loadHints());
  ASSERT_NE(ticket_a, 0u);
  ASSERT_NE(ticket_b, 0u);
  EXPECT_EQ(loader_->currentLoadTicket(), ticket_a);

  std::thread off_thread([this, ticket_b]() { loader_->cancelLoadAsync(ticket_b, /*keep_partial=*/false); });
  off_thread.join();
  EXPECT_TRUE(recorder.events.empty()) << "the marshal and the terminal both ride the event loop";
  flushQueuedTerminals();

  EXPECT_EQ(recorder.countForTicket(ticket_b), 1u) << "the off-thread cancel must resolve the queued ticket";
  ASSERT_FALSE(recorder.events.empty());
  EXPECT_EQ(recorder.events[0].ticket, ticket_b);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kCancelled);

  // Wind down A through the ticket API and drain.
  EXPECT_TRUE(loader_->cancelLoad(ticket_a, /*keep_partial=*/false));
  releaseFanoutProbeCancelEntry();
  pumpUntilIdle(*loader_);
  EXPECT_EQ(recorder.countForTicket(ticket_a), 1u);
  EXPECT_EQ(recorder.events.size(), 2u);
  EXPECT_EQ(datasetNamed("ticket_async_b.mock"), 0u) << "the cancelled queued request must never load";
}

// Strict replacement: require_replacement forbids the silent degrade to a
// fresh load when the replace target vanished before the request was dequeued.
TEST_F(FileLoaderTest, RequireReplacementFailsWhenTargetVanished) {
  LoadFinishedRecorder recorder(*loader_);
  QString failure_reason;
  int legacy_failed = 0;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoadFailed, loader_.get(), [&](const QString&, const QString& reason) {
        ++legacy_failed;
        failure_reason = reason;
      });

  PJ::LoadHints hints = loadHints();
  hints.replace_dataset_id = 424242;  // never existed
  hints.require_replacement = true;
  const quint64 ticket = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(mock_path_), nullptr, hints);
  ASSERT_NE(ticket, 0u);
  pumpUntilIdle(*loader_);

  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kFailed);
  EXPECT_EQ(recorder.events[0].dataset_id, 0u);
  EXPECT_EQ(legacy_failed, 1);
  EXPECT_TRUE(failure_reason.contains(u"424242"_s))
      << "the failure must name the vanished target, got: " << failure_reason.toStdString();
  EXPECT_TRUE(session().createReader().listDatasets().empty()) << "strict replacement must not create a fresh dataset";
}

TEST_F(FileLoaderTest, RequireReplacementReplacesExistingTarget) {
  ASSERT_TRUE(load());     // sensors.mock
  flushQueuedTerminals();  // deliver the setup load's terminal before recording
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  LoadFinishedRecorder recorder(*loader_);

  const QString other_path = makeMockFile(u"other.mock"_s);
  PJ::LoadHints hints = loadHints();
  hints.replace_dataset_id = dataset_id;
  hints.require_replacement = true;
  const quint64 ticket = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(other_path), nullptr, hints);
  ASSERT_NE(ticket, 0u);
  pumpUntilIdle(*loader_);

  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kLoaded)
      << "a live target replaces exactly as without the flag";
  EXPECT_EQ(recorder.events[0].dataset_id, dataset_id);
  EXPECT_EQ(recorder.events[0].produced, QVector<PJ::DatasetId>{dataset_id});
  EXPECT_EQ(session().createReader().listDatasets().size(), 1u);
}

// Review finding 6: the require_replacement target-exists check ran at
// dequeue, but a fan-out expansion then tombstoned the target, minted fresh
// ids, and reported kLoaded — strict replacement defeated. A config that
// expands to fan-out must be rejected BEFORE any target mutation.
TEST_F(FileLoaderTest, RequireReplacementRejectsFanoutExpansion) {
  ASSERT_TRUE(load());     // sensors.mock, 3 rows
  flushQueuedTerminals();  // deliver the setup load's terminal before recording
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  ASSERT_EQ(singleTopicRowCount(dataset_id), 3);
  LoadFinishedRecorder recorder(*loader_);
  QString failure_reason;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoadFailed, loader_.get(),
      [&](const QString&, const QString& reason) { failure_reason = reason; });

  const QString fan_path = makeMockFile(u"strict_fan.mock"_s);
  PJ::LoadHints hints =
      loadHints(uR"({"__pj_fanout":["{\"display_suffix\":\"left\"}","{\"display_suffix\":\"right\"}"]})"_s);
  hints.replace_dataset_id = dataset_id;
  hints.require_replacement = true;
  const quint64 ticket = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(fan_path), nullptr, hints);
  ASSERT_NE(ticket, 0u);
  pumpUntilIdle(*loader_);

  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kFailed) << "fan-out must not defeat strict replacement";
  EXPECT_TRUE(recorder.events[0].produced.isEmpty());
  EXPECT_TRUE(failure_reason.contains(QString::number(dataset_id)))
      << "the diagnostic must name the pinned target, got: " << failure_reason.toStdString();
  EXPECT_TRUE(engineHasDataset(dataset_id)) << "the pinned target must be untouched";
  EXPECT_EQ(singleTopicRowCount(dataset_id), 3) << "the pinned target's data must be untouched";
  EXPECT_EQ(datasetNamed("strict_fan/left"), 0u) << "no fan-out dataset may be created";
  EXPECT_EQ(datasetNamed("strict_fan/right"), 0u);
  EXPECT_EQ(catalog().items().size(), 1u) << "the target's curves survive (no tombstone)";
}

// Review finding 3: a successful replacement rewrites the dataset's content
// from a NEW source, so the target's old provider SourceRecord is stale
// provenance — it must be detached BEFORE the loadCommitting seam, where a
// promotion listener attaches the record for the new content.
TEST_F(FileLoaderTest, SuccessfulReplaceDetachesStaleSourceRecordBeforeCommitSeam) {
  ASSERT_TRUE(load());  // sensors.mock
  flushQueuedTerminals();
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  session().attachSourceRecord(
      dataset_id, PJ::SourceRecord{
                      .provider_id = u"stale-cloud-provider"_s,
                      .source_identity = u"stale-digest"_s,
                      .descriptor_json = uR"({"stale":true})"_s,
                  });
  ASSERT_NE(session().sourceRecord(dataset_id), nullptr);

  bool stale_gone_at_seam = false;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::loadCommitting, loader_.get(),
      [&](quint64, const QVector<PJ::DatasetId>& produced, const QString&, const QString&) {
        if (produced.contains(dataset_id)) {
          stale_gone_at_seam = session().sourceRecord(dataset_id) == nullptr;
          // What a promotion listener does with the seam: attach the NEW
          // content's record atomically with catalog publication.
          session().attachSourceRecord(
              dataset_id, PJ::SourceRecord{
                              .provider_id = u"new-provider"_s,
                              .source_identity = u"new-digest"_s,
                              .descriptor_json = uR"({"new":true})"_s,
                          });
        }
      });

  const QString other_path = makeMockFile(u"other.mock"_s);
  PJ::LoadHints hints = loadHints();
  hints.replace_dataset_id = dataset_id;
  ASSERT_TRUE(loadAndWait(other_path, hints));
  flushQueuedTerminals();

  EXPECT_TRUE(stale_gone_at_seam) << "the stale record must be gone by the time the commit seam fires";
  const PJ::SourceRecord* record = session().sourceRecord(dataset_id);
  ASSERT_NE(record, nullptr) << "the seam-attached record must survive the commit";
  EXPECT_EQ(record->provider_id, u"new-provider"_s);
}

TEST_F(FileLoaderTest, FailedReplaceKeepsSourceRecord) {
  ASSERT_TRUE(load());  // sensors.mock
  flushQueuedTerminals();
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  session().attachSourceRecord(
      dataset_id, PJ::SourceRecord{
                      .provider_id = u"cloud-provider"_s,
                      .source_identity = u"digest"_s,
                      .descriptor_json = uR"({"d":1})"_s,
                  });

  const QString other_path = makeMockFile(u"other.mock"_s);
  PJ::LoadHints hints = loadHints(uR"({"fail_start":true})"_s);
  hints.replace_dataset_id = dataset_id;
  EXPECT_FALSE(loadAndWait(other_path, hints));
  flushQueuedTerminals();

  const PJ::SourceRecord* record = session().sourceRecord(dataset_id);
  ASSERT_NE(record, nullptr) << "a rolled-back replace must keep the target's provenance";
  EXPECT_EQ(record->provider_id, u"cloud-provider"_s);
}

// A fan-out load reports EVERY dataset it produced; the primary argument stays
// the first loaded entry as the single-id convenience.
TEST_F(FileLoaderTest, FanoutLoadReportsAllProducedDatasets) {
  const QString path = makeMockFile(u"ticket_fan.mock"_s);
  LoadFinishedRecorder recorder(*loader_);
  IngestLifecycleRecorder ingest_lifecycle(session());
  const PJ::LoadHints hints =
      loadHints(uR"({"__pj_fanout":["{\"display_suffix\":\"left\"}","{\"display_suffix\":\"right\"}"]})"_s);

  const quint64 ticket = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(path), nullptr, hints);
  ASSERT_NE(ticket, 0u);
  pumpUntilIdle(*loader_);

  const PJ::DatasetId left = datasetNamed("ticket_fan/left");
  const PJ::DatasetId right = datasetNamed("ticket_fan/right");
  ASSERT_NE(left, 0u);
  ASSERT_NE(right, 0u);
  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kLoaded);
  ASSERT_EQ(recorder.events[0].produced.size(), 2);
  EXPECT_EQ(recorder.events[0].dataset_id, recorder.events[0].produced.front());
  EXPECT_TRUE(recorder.events[0].produced.contains(left));
  EXPECT_TRUE(recorder.events[0].produced.contains(right));

  ASSERT_EQ(ingest_lifecycle.began.size(), 2u);
  ASSERT_EQ(ingest_lifecycle.ended.size(), 2u) << "one begin/end pair is required per fanout dataset";
  for (const PJ::DatasetId dataset_id : {left, right}) {
    EXPECT_EQ(ingest_lifecycle.beginCountForDataset(dataset_id), 1u);
    EXPECT_EQ(ingest_lifecycle.endCountForDataset(dataset_id), 1u);
    const auto began = std::find_if(
        ingest_lifecycle.began.begin(), ingest_lifecycle.began.end(),
        [dataset_id](const auto& event) { return event.token.dataset_id == dataset_id; });
    ASSERT_NE(began, ingest_lifecycle.began.end());
    EXPECT_NE(began->token.id, 0u);
    EXPECT_TRUE(began->cancellable);
    const auto* ended = ingest_lifecycle.endFor(began->token);
    ASSERT_NE(ended, nullptr);
    EXPECT_EQ(ended->outcome, PJ::IngestOutcome::kCompleted);
  }
  EXPECT_FALSE(session().hasActiveIngests());
}

// The pre-catalog commit seam: loadCommitting fires synchronously while a
// fully-loaded single-instance request commits — dataset + source path +
// captured config installed, catalog rebuild not yet published — and strictly
// before fileLoaded/loadFinished.
TEST_F(FileLoaderTest, LoadCommittingFiresWithSourceInstalledBeforeCatalogAndTerminal) {
  LoadFinishedRecorder recorder(*loader_);
  struct CommitObservation {
    quint64 ticket = 0;
    QVector<PJ::DatasetId> produced;
    QString plugin_id;
    QString config;
    QString source_path_at_commit;
    std::size_t catalog_items_at_commit = 999;
    bool dataset_in_engine = false;
  };
  std::vector<CommitObservation> commits;
  QStringList sequence;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::loadCommitting, loader_.get(),
      [&](quint64 ticket, const QVector<PJ::DatasetId>& produced, const QString& plugin_id, const QString& config) {
        CommitObservation obs;
        obs.ticket = ticket;
        obs.produced = produced;
        obs.plugin_id = plugin_id;
        obs.config = config;
        if (!produced.isEmpty()) {
          obs.source_path_at_commit = loader_->sourcePathForDataset(produced.front());
          obs.dataset_in_engine = engineHasDataset(produced.front());
        }
        obs.catalog_items_at_commit = catalog().items().size();
        commits.push_back(obs);
        sequence << u"committing"_s;
      });
  QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded, loader_.get(),
      [&](const QString&, const QString&, const QString&, const QString&) { sequence << u"fileLoaded"_s; });
  QObject::connect(
      loader_.get(), &PJ::FileLoader::loadFinished, loader_.get(),
      [&](quint64, PJ::LoadOutcome, const QString&, PJ::DatasetId, const QVector<PJ::DatasetId>&) {
        sequence << u"loadFinished"_s;
      });

  const quint64 ticket = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(mock_path_), nullptr, loadHints());
  ASSERT_NE(ticket, 0u);
  pumpUntilIdle(*loader_);

  ASSERT_EQ(commits.size(), 1u) << "one commit seam per fully-loaded request";
  EXPECT_EQ(commits[0].ticket, ticket);
  ASSERT_EQ(commits[0].produced.size(), 1);
  EXPECT_EQ(commits[0].plugin_id, u"Mock File Source"_s);
  EXPECT_TRUE(commits[0].dataset_in_engine) << "the produced dataset must exist by commit time";
  EXPECT_EQ(commits[0].source_path_at_commit, mock_path_) << "the source path must be installed by commit time";
  // The fast mock never flushes mid-load (50ms worker throttle), so no
  // samplesIngested-gate rebuild precedes the commit: the catalog must still
  // be unpublished at the seam and published right after.
  EXPECT_EQ(commits[0].catalog_items_at_commit, 0u)
      << "loadCommitting must fire BEFORE the catalog rebuild publishes the load";
  EXPECT_EQ(catalog().items().size(), 1u);
  const QStringList expected_sequence{u"committing"_s, u"fileLoaded"_s, u"loadFinished"_s};
  EXPECT_EQ(sequence, expected_sequence);
}

TEST_F(FileLoaderTest, LoadCommittingSkippedOnFailedLoad) {
  int committing = 0;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::loadCommitting, loader_.get(),
      [&](quint64, const QVector<PJ::DatasetId>&, const QString&, const QString&) { ++committing; });

  const quint64 ticket = loader_->loadFileTicketed(
      PJ::LoadInput::fromNativePath(mock_path_), nullptr, loadHints(uR"({"fail_start":true})"_s));
  ASSERT_NE(ticket, 0u);
  pumpUntilIdle(*loader_);

  EXPECT_EQ(committing, 0) << "the commit seam fires only for loads that finish kLoaded";
}

// Review finding 4: under DialogPolicy::kNever the layout's preset is
// AUTHORITATIVE — a preset the plugin rejects must FAIL the source (kFailed)
// instead of silently retrying with the QSettings pre-fill (which this
// plugin would accept, turning a broken replay into a wrong-config success).
TEST_F(FileLoaderTest, DialogPolicyNeverRejectedPresetFailsInsteadOfFallback) {
  ASSERT_TRUE(app_session_->extensionCatalog().pluginCatalog().registerStaticDataSource(rejectingPresetVtable()));
  const QString path = makeMockFile(u"strict.cfgreject"_s);
  LoadFinishedRecorder recorder(*loader_);
  QString failure_reason;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoadFailed, loader_.get(),
      [&](const QString&, const QString& reason) { failure_reason = reason; });

  PJ::LoadHints hints;
  hints.expected_plugin_id = u"Rejecting Preset Source"_s;
  hints.preset_config_json = uR"({"poison":true})"_s;
  hints.dialog_policy = PJ::DialogPolicy::kNever;
  hints.require_expected_plugin = true;
  const quint64 ticket = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(path), nullptr, hints);
  ASSERT_NE(ticket, 0u);
  pumpUntilIdle(*loader_);

  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kFailed)
      << "a rejected authoritative preset must fail the source, never fall back";
  EXPECT_FALSE(failure_reason.isEmpty());
  EXPECT_TRUE(failure_reason.contains(u"poisoned preset rejected"_s))
      << "the diagnostic must surface the plugin's rejection, got: " << failure_reason.toStdString();
  EXPECT_TRUE(session().createReader().listDatasets().empty()) << "no dataset may survive the strict rejection";
}

// Fixture for the shutdown-while-suspended-at-the-plugin-config-dialog path.
// Stages the test-only dialog_probe_source plugin (kCapabilityHasDialog, embedded
// dialog that records onRejected/destroy order to PJ_DIALOG_PROBE_FILE) and drives
// a real FileLoader against it with the INTERACTIVE dialog policy, so the
// prologue coroutine suspends on DataSourceDialogAwaiter with the dialog open.
class DialogShutdownTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(data_dir_.isValid());
    ASSERT_TRUE(probe_dir_.isValid());

    const QString src = QString::fromUtf8(PJ_DIALOG_PROBE_SOURCE_PLUGIN_PATH);
    const QString dst = extensions_dir_.filePath(QFileInfo(src).fileName());
    ASSERT_TRUE(QFile::copy(src, dst)) << "could not stage " << src.toStdString();

    probe_path_ = probe_dir_.filePath(u"dialog_probe.log"_s);
    qputenv("PJ_DIALOG_PROBE_FILE", probe_path_.toUtf8());

    app_session_ = std::make_unique<PJ::AppSession>(extensions_dir_.path());
    ASSERT_FALSE(app_session_->extensionCatalog().findSourcesForExtension(u".dlgprobe"_s).empty())
        << "dialog_probe_source_plugin did not load from the staged extensions dir";

    loader_ = std::make_unique<PJ::FileLoader>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), app_session_->catalogModel());
  }

  void TearDown() override {
    qunsetenv("PJ_DIALOG_PROBE_FILE");
  }

  // A visible plugin-config QDialog owned by `parent`, or nullptr.
  [[nodiscard]] static QDialog* visibleDialog(QWidget* parent) {
    for (QDialog* dialog : parent->findChildren<QDialog*>()) {
      if (dialog->isVisible()) {
        return dialog;
      }
    }
    return nullptr;
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir data_dir_;
  QTemporaryDir probe_dir_;
  QString probe_path_;
  std::unique_ptr<PJ::AppSession> app_session_;
  std::unique_ptr<PJ::FileLoader> loader_;
};

// Tearing the loader down while a load is suspended on its plugin config dialog
// must reject the plugin EXACTLY ONCE and BEFORE the embedded dialog (and its
// plugin ctx) is destroyed — a UAF/ordering bug shows up as a missing/duplicate
// "rejected" or a "destroyed" that precedes it (recorded across the DSO boundary
// via the probe file). joinForShutdown must also return without hanging.
TEST_F(DialogShutdownTest, ShutdownWhileSuspendedAtDialogRejectsBeforeDestroy) {
  QWidget parent;  // real GUI-thread parent so the dialog actually opens (interactive policy)

  const QString path = data_dir_.filePath(u"probe.dlgprobe"_s);
  {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
  }

  LoadFinishedRecorder recorder(*loader_);

  // No hints → the interactive dialog policy → the prologue awaits the plugin dialog.
  ASSERT_TRUE(loader_->loadFile(path, &parent, PJ::LoadHints{}));

  // Pump the event loop until the plugin config dialog is actually shown.
  QEventLoop wait_for_dialog;
  QTimer poll;
  poll.setInterval(5);
  QObject::connect(&poll, &QTimer::timeout, &wait_for_dialog, [&]() {
    if (visibleDialog(&parent) != nullptr) {
      wait_for_dialog.quit();
    }
  });
  QTimer::singleShot(3000, &wait_for_dialog, &QEventLoop::quit);
  poll.start();
  wait_for_dialog.exec();
  poll.stop();
  ASSERT_NE(visibleDialog(&parent), nullptr) << "the plugin config dialog never opened";

  // Nothing recorded yet: the load's dialog is open, not rejected, not destroyed.
  ASSERT_TRUE(readProbeLines(probe_path_).empty());

  // Destroy the suspended prologue frame. The DataSourceDialogAwaiter dtor cancels
  // the dialog (reject → onRejected), THEN the frame's DataSourceHandle destroys
  // the embedded dialog.
  loader_->joinForShutdown();
  EXPECT_FALSE(loader_->isBusy());
  EXPECT_EQ(visibleDialog(&parent), nullptr) << "shutdown must close the plugin dialog";

  // A shutdown that destroys the suspended prologue still resolves its ticket
  // (terminal delivery is queued).
  flushQueuedTerminals();
  ASSERT_EQ(recorder.events.size(), 1u) << "shutdown must resolve the dialog-suspended request's ticket";
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kCancelled);
  EXPECT_EQ(recorder.events[0].path, path);

  const std::vector<std::string> markers = readProbeLines(probe_path_);
  const auto rejected_count = std::count(markers.begin(), markers.end(), std::string("rejected"));
  const auto destroyed_count = std::count(markers.begin(), markers.end(), std::string("destroyed"));
  EXPECT_EQ(rejected_count, 1) << "plugin must be rejected exactly once at shutdown";
  EXPECT_EQ(destroyed_count, 1) << "the embedded dialog must be destroyed exactly once";

  const auto first_reject = std::find(markers.begin(), markers.end(), std::string("rejected"));
  const auto first_destroy = std::find(markers.begin(), markers.end(), std::string("destroyed"));
  ASSERT_NE(first_reject, markers.end());
  ASSERT_NE(first_destroy, markers.end());
  EXPECT_LT(first_reject, first_destroy) << "on_rejected must reach the plugin BEFORE its ctx is destroyed (UAF guard)";

  // The loader recovers after shutdown.
  EXPECT_FALSE(loader_->isBusy());
}

// V12a: a cancel issued while the prologue sits suspended at its config dialog
// must be HONORED when the prologue resumes — not silently wiped by the
// cancel_mode_.store(0) that precedes the worker. The user accepts the dialog
// AFTER cancelling; the load must still roll back (fileLoadFailed, no dataset
// created, no worker started).
TEST_F(DialogShutdownTest, CancelWhileSuspendedAtDialogIsHonoredOnResume) {
  QWidget parent;

  const QString path = data_dir_.filePath(u"cancel_probe.dlgprobe"_s);
  {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
  }

  int failed_count = 0;
  int loaded_count = 0;
  QObject::connect(loader_.get(), &PJ::FileLoader::fileLoadFailed, loader_.get(), [&](const QString&, const QString&) {
    ++failed_count;
  });
  QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded, loader_.get(),
      [&](const QString&, const QString&, const QString&, const QString&) { ++loaded_count; });
  LoadFinishedRecorder recorder(*loader_);

  ASSERT_TRUE(loader_->loadFile(path, &parent, PJ::LoadHints{}));

  // Pump until the plugin config dialog is shown (prologue suspended).
  QEventLoop wait_for_dialog;
  QTimer poll;
  poll.setInterval(5);
  QObject::connect(&poll, &QTimer::timeout, &wait_for_dialog, [&]() {
    if (visibleDialog(&parent) != nullptr) {
      wait_for_dialog.quit();
    }
  });
  QTimer::singleShot(3000, &wait_for_dialog, &QEventLoop::quit);
  poll.start();
  wait_for_dialog.exec();
  poll.stop();
  QDialog* dialog = visibleDialog(&parent);
  ASSERT_NE(dialog, nullptr) << "the plugin config dialog never opened";

  // The prologue created a dataset SHELL before opening the dialog (bind() needs
  // it), but no worker has run, so it holds no rows yet.
  const std::size_t datasets_while_suspended = app_session_->sessionManager().createReader().listDatasets().size();

  // Issue the cancel WHILE suspended (the title-bar Stop → Remove All path), then
  // let the user "accept" the dialog. The pending cancel must win.
  loader_->cancelCurrent(/*keep_partial=*/false);
  dialog->accept();

  // Pump until the prologue resumes and the load resolves.
  QEventLoop settle;
  QObject::connect(loader_.get(), &PJ::FileLoader::fileLoadFailed, &settle, &QEventLoop::quit);
  QObject::connect(loader_.get(), &PJ::FileLoader::fileLoaded, &settle, &QEventLoop::quit);
  QObject::connect(loader_.get(), &PJ::FileLoader::queueDrained, &settle, &QEventLoop::quit);
  QTimer::singleShot(5000, &settle, &QEventLoop::quit);
  if (loader_->isBusy()) {
    settle.exec();
  }

  EXPECT_FALSE(loader_->isBusy());
  EXPECT_EQ(loaded_count, 0) << "a load cancelled while suspended must NOT complete";
  EXPECT_EQ(failed_count, 1) << "the honored cancel must surface as one fileLoadFailed";
  // The ticket surface classifies this exit as a user cancel, exactly once.
  flushQueuedTerminals();
  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kCancelled);
  // The dataset shell created before the dialog must be rolled back: the count
  // returns to whatever it was BEFORE this load began (0 here).
  EXPECT_EQ(app_session_->sessionManager().createReader().listDatasets().size(), datasets_while_suspended - 1)
      << "the cancelled load's dataset shell must be erased on rollback";
  EXPECT_EQ(app_session_->sessionManager().createReader().listDatasets().size(), 0u)
      << "no dataset may survive a cancel honored at prologue resume";
}

// The user rejecting the plugin config dialog is a terminal exit with NO legacy
// signal at all (deliberately — see beginLoad's kRejected arm). The ticket
// surface must still resolve it: exactly one loadFinished(kCancelled), while
// fileLoaded/fileLoadFailed stay silent as before.
TEST_F(DialogShutdownTest, ConfigDialogRejectReportsCancelledTicketExactlyOnce) {
  QWidget parent;

  const QString path = data_dir_.filePath(u"reject_probe.dlgprobe"_s);
  {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
  }

  int failed_count = 0;
  int loaded_count = 0;
  QObject::connect(loader_.get(), &PJ::FileLoader::fileLoadFailed, loader_.get(), [&](const QString&, const QString&) {
    ++failed_count;
  });
  QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded, loader_.get(),
      [&](const QString&, const QString&, const QString&, const QString&) { ++loaded_count; });
  LoadFinishedRecorder recorder(*loader_);

  const quint64 ticket = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(path), &parent, PJ::LoadHints{});
  ASSERT_NE(ticket, 0u);

  // Pump until the plugin config dialog is shown (prologue suspended).
  QEventLoop wait_for_dialog;
  QTimer poll;
  poll.setInterval(5);
  QObject::connect(&poll, &QTimer::timeout, &wait_for_dialog, [&]() {
    if (visibleDialog(&parent) != nullptr) {
      wait_for_dialog.quit();
    }
  });
  QTimer::singleShot(3000, &wait_for_dialog, &QEventLoop::quit);
  poll.start();
  wait_for_dialog.exec();
  poll.stop();
  QDialog* dialog = visibleDialog(&parent);
  ASSERT_NE(dialog, nullptr) << "the plugin config dialog never opened";

  dialog->reject();

  QEventLoop settle;
  QObject::connect(loader_.get(), &PJ::FileLoader::queueDrained, &settle, &QEventLoop::quit);
  QTimer::singleShot(5000, &settle, &QEventLoop::quit);
  if (loader_->isBusy()) {
    settle.exec();
  }

  EXPECT_FALSE(loader_->isBusy());
  flushQueuedTerminals();
  ASSERT_EQ(recorder.events.size(), 1u) << "a rejected config dialog must resolve the ticket exactly once";
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kCancelled);
  EXPECT_EQ(recorder.events[0].path, path);
  EXPECT_EQ(recorder.events[0].dataset_id, 0u);
  EXPECT_EQ(loaded_count, 0) << "the reject path stays silent on the legacy signals";
  EXPECT_EQ(failed_count, 0) << "the reject path stays silent on the legacy signals";
}

// Review finding 4: an automated (layout-driven) replay of a source whose
// saved config is legitimately EMPTY must load without EVER prompting — the
// dialog is prohibited under DialogPolicy::kNever and the empty preset is
// authoritative (it becomes the minimal fresh-filepath config). This plugin
// HAS a dialog, so any fallback-to-dialog regression parks the load on an
// open dialog and times this test out.
TEST_F(DialogShutdownTest, DialogPolicyNeverEmptyPresetNeverOpensDialog) {
  QWidget parent;  // a real parent: the dialog COULD open if the strict flag failed

  const QString path = data_dir_.filePath(u"nodialog.dlgprobe"_s);
  {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
  }

  LoadFinishedRecorder recorder(*loader_);
  bool dialog_seen = false;
  QTimer dialog_watch;
  dialog_watch.setInterval(5);
  QObject::connect(&dialog_watch, &QTimer::timeout, &parent, [&]() {
    if (visibleDialog(&parent) != nullptr) {
      dialog_seen = true;
    }
  });
  dialog_watch.start();

  PJ::LoadHints hints;
  hints.expected_plugin_id = u"Dialog Probe Source"_s;
  hints.preset_config_json.clear();  // an empty saveConfig is legitimate
  hints.dialog_policy = PJ::DialogPolicy::kNever;
  hints.require_expected_plugin = true;
  const quint64 ticket = loader_->loadFileTicketed(PJ::LoadInput::fromNativePath(path), &parent, hints);
  ASSERT_NE(ticket, 0u);
  pumpUntilIdle(*loader_);
  dialog_watch.stop();

  EXPECT_FALSE(dialog_seen) << "an automated replay must never prompt";
  ASSERT_EQ(recorder.events.size(), 1u) << "the load must complete without waiting on any dialog";
  EXPECT_EQ(recorder.events[0].ticket, ticket);
  EXPECT_EQ(recorder.events[0].outcome, PJ::LoadOutcome::kLoaded)
      << "the empty preset is authoritative and loads with the minimal fresh-filepath config";
  EXPECT_TRUE(readProbeLines(probe_path_).empty()) << "the embedded dialog must never be exercised";
}

// Header-bearing object types (clouds, images, grids) ingest with EAGER
// scalars so their header/timestamp series appear in the curve tree; the
// bytes are fetched once for the scalar parse and dropped, so blobs stay
// non-resident (objects remain lazy). Pure-lazy is reserved for video frames
// and scalar-less scene/annotation payloads, where even the one ingest-time
// read buys nothing worth plotting. TF stays eager (tiny payload, useful
// scalars).
TEST(FileLoaderIngestPolicy, HeaderBearingTypesGetEagerScalarsOthersStayPureLazy) {
  using PJ::sdk::BuiltinObjectType;
  using PJ::sdk::ObjectIngestPolicy;

  PJ::sdk::ObjectIngestPolicyResolver resolver;
  PJ::FileLoader::applyDefaultIngestPolicies(resolver);

  EXPECT_EQ(
      resolver.resolve("src", "/cam/color", BuiltinObjectType::kImage), ObjectIngestPolicy::kLazyObjectsEagerScalars);
  EXPECT_EQ(
      resolver.resolve("src", "/cam/depth", BuiltinObjectType::kDepthImage),
      ObjectIngestPolicy::kLazyObjectsEagerScalars);
  EXPECT_EQ(
      resolver.resolve("src", "/lidar", BuiltinObjectType::kPointCloud), ObjectIngestPolicy::kLazyObjectsEagerScalars);
  EXPECT_EQ(
      resolver.resolve("src", "/lidar_c", BuiltinObjectType::kCompressedPointCloud),
      ObjectIngestPolicy::kLazyObjectsEagerScalars);
  EXPECT_EQ(
      resolver.resolve("src", "/map", BuiltinObjectType::kOccupancyGrid), ObjectIngestPolicy::kLazyObjectsEagerScalars);
  EXPECT_EQ(
      resolver.resolve("src", "/voxels", BuiltinObjectType::kVoxelGrid), ObjectIngestPolicy::kLazyObjectsEagerScalars);
  // Video and the scalar-less scene/annotation payloads stay pure-lazy.
  EXPECT_EQ(resolver.resolve("src", "/video", BuiltinObjectType::kVideoFrame), ObjectIngestPolicy::kPureLazy);
  EXPECT_EQ(resolver.resolve("src", "/markers", BuiltinObjectType::kSceneEntities), ObjectIngestPolicy::kPureLazy);
  EXPECT_EQ(
      resolver.resolve("src", "/annotations", BuiltinObjectType::kImageAnnotations), ObjectIngestPolicy::kPureLazy);
  // TF is deliberately NOT pure-lazy.
  EXPECT_NE(resolver.resolve("src", "/tf", BuiltinObjectType::kFrameTransforms), ObjectIngestPolicy::kPureLazy);
}

// Captures Qt log messages for the lifetime of the instance. The cross-thread
// QObject::setParent warning fires on the import worker thread, so the sink is
// mutex-guarded. One instance at a time (tests run sequentially).
class QtMessageCapture {
 public:
  QtMessageCapture() {
    QMutexLocker lock(&mutex());
    sink() = &messages_;
    previous_ = qInstallMessageHandler(&QtMessageCapture::handle);
  }
  ~QtMessageCapture() {
    qInstallMessageHandler(previous_);
    QMutexLocker lock(&mutex());
    sink() = nullptr;
  }
  QtMessageCapture(const QtMessageCapture&) = delete;
  QtMessageCapture& operator=(const QtMessageCapture&) = delete;

  [[nodiscard]] bool sawText(const QString& needle) {
    QMutexLocker lock(&mutex());
    return std::any_of(messages_.begin(), messages_.end(), [&](const QString& m) { return m.contains(needle); });
  }

 private:
  static QMutex& mutex() {
    static QMutex m;
    return m;
  }
  static std::vector<QString>*& sink() {
    static std::vector<QString>* s = nullptr;
    return s;
  }
  static void handle(QtMsgType /*type*/, const QMessageLogContext& /*ctx*/, const QString& msg) {
    QMutexLocker lock(&mutex());
    if (sink() != nullptr) {
      sink()->push_back(msg);
    }
  }

  std::vector<QString> messages_;
  QtMessageHandler previous_ = nullptr;
};

// Fixture for the cross-thread message-box regression. Stages the test-only
// msgbox_mock_source plugin (calls runtimeHost().askContinue() from the import
// worker thread when configured) and drives a real FileLoader against it.
class MessageBoxMarshalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(data_dir_.isValid());
    const QString src = QString::fromUtf8(PJ_MSGBOX_MOCK_SOURCE_PLUGIN_PATH);
    const QString dst = extensions_dir_.filePath(QFileInfo(src).fileName());
    ASSERT_TRUE(QFile::copy(src, dst)) << "could not stage " << src.toStdString();

    app_session_ = std::make_unique<PJ::AppSession>(extensions_dir_.path());
    ASSERT_FALSE(app_session_->extensionCatalog().findSourcesForExtension(u".msgboxmock"_s).empty())
        << "msgbox_mock_source_plugin did not load from the staged extensions dir";

    loader_ = std::make_unique<PJ::FileLoader>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), app_session_->catalogModel());
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir data_dir_;
  std::unique_ptr<PJ::AppSession> app_session_;
  std::unique_ptr<PJ::FileLoader> loader_;
};

// REGRESSION (CSV-load segfault, cross-thread message box): a DataSource plugin
// may call the runtime host's message box from the import worker thread (the
// SDK contract tags show_message_box [main-thread] and promises the host
// marshals it). Pre-fix, FileLoader's setMessageBoxHandler built the message box
// directly on the worker → "QObject::setParent: ... different thread" + a
// paint-engine segfault. Here we pass a real GUI-thread dialog_parent (the
// handler is only installed when non-null), trigger a worker-thread askContinue,
// auto-click Continue from the GUI thread, and assert no cross-thread warning
// was emitted and the load completed.
TEST_F(MessageBoxMarshalTest, WorkerThreadMessageBoxIsMarshaledToGuiThread) {
  QtMessageCapture capture;
  QWidget parent;  // a real GUI-thread-owned parent for the marshaled message box

  const QString path = data_dir_.filePath(u"trigger.msgboxmock"_s);
  {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
  }

  PJ::LoadHints hints;
  hints.expected_plugin_id = u"Msgbox Mock Source"_s;
  hints.preset_config_json = uR"({"ask_msgbox":true})"_s;
  hints.dialog_policy = PJ::DialogPolicy::kPreferPreset;

  // The GUI thread opens the message box asynchronously while only the ingest
  // worker waits. This timer clicks Continue so askContinue returns true and
  // the load proceeds without a nested GUI event loop.
  QTimer dismiss;
  dismiss.setInterval(20);
  QObject::connect(&dismiss, &QTimer::timeout, [&]() {
    QWidget* dlg = QApplication::activeModalWidget();
    if (dlg == nullptr) {
      for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->isVisible() && w->isModal()) {
          dlg = w;
          break;
        }
      }
    }
    if (dlg == nullptr) {
      return;
    }
    for (QAbstractButton* button : dlg->findChildren<QAbstractButton*>()) {
      if (button->text() == u"Continue"_s) {
        button->click();
        return;
      }
    }
  });
  dismiss.start();

  QEventLoop loop;
  bool ok = false;
  bool done = false;
  const auto on_loaded = QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded, &loop,
      [&](const QString&, const QString&, const QString&, const QString&) {
        ok = true;
        done = true;
        loop.quit();
      });
  const auto on_failed =
      QObject::connect(loader_.get(), &PJ::FileLoader::fileLoadFailed, &loop, [&](const QString&, const QString&) {
        ok = false;
        done = true;
        loop.quit();
      });
  loader_->loadFile(path, &parent, hints);
  if (!done) {
    QTimer::singleShot(10000, &loop, [&loop]() { loop.quit(); });  // safety: fail, don't hang CI
    loop.exec();
  }
  QObject::disconnect(on_loaded);
  QObject::disconnect(on_failed);
  dismiss.stop();

  EXPECT_FALSE(capture.sawText(u"Cannot set parent"_s))
      << "host built the message box off the GUI thread (QObject::setParent cross-thread warning)";
  EXPECT_FALSE(capture.sawText(u"different thread"_s))
      << "a cross-thread Qt warning was emitted during the worker-thread message box";
  EXPECT_TRUE(ok) << "the marshaled askContinue must return Continue and complete the load";
}

// REGRESSION (desktop plugin contract): FileLoader calls loadConfig() on the
// GUI thread after bind(). show_message_box is nevertheless a synchronous ABI:
// it must block until the user answers and return that exact button. The current
// GUI-thread branch opens the dialog asynchronously and immediately returns -1,
// so the plugin observes Abort even though this test clicks Continue.
TEST_F(MessageBoxMarshalTest, GuiThreadLoadConfigReceivesClickedMessageBoxAnswer) {
  QTemporaryDir probe_dir;
  ASSERT_TRUE(probe_dir.isValid());
  const QString probe_path = probe_dir.filePath(u"msgbox_probe.log"_s);
  qputenv("PJ_MSGBOX_PROBE_FILE", probe_path.toUtf8());
  const auto cleanup_env = qScopeGuard([]() { qunsetenv("PJ_MSGBOX_PROBE_FILE"); });

  QWidget parent;
  const QString path = data_dir_.filePath(u"gui_load_config.msgboxmock"_s);
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.close();

  PJ::LoadHints hints;
  hints.expected_plugin_id = u"Msgbox Mock Source"_s;
  hints.preset_config_json = uR"({"ask_msgbox_in_load_config":true})"_s;
  hints.dialog_policy = PJ::DialogPolicy::kPreferPreset;

  QEventLoop loop;
  bool ok = false;
  bool done = false;
  bool clicked_continue = false;
  const auto maybe_finish = [&]() {
    if (done && clicked_continue) {
      loop.quit();
    }
  };

  QTimer dismiss;
  dismiss.setInterval(5);
  QObject::connect(&dismiss, &QTimer::timeout, [&]() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
      auto* message_box = qobject_cast<PJ::MessageBox*>(widget);
      if (message_box == nullptr || !message_box->isVisible()) {
        continue;
      }
      for (QAbstractButton* button : message_box->findChildren<QAbstractButton*>()) {
        if (button->text() == u"Continue"_s) {
          clicked_continue = true;
          button->click();
          maybe_finish();
          return;
        }
      }
    }
  });
  dismiss.start();

  const auto on_loaded = QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded, &loop,
      [&](const QString&, const QString&, const QString&, const QString&) {
        ok = true;
        done = true;
        maybe_finish();
      });
  const auto on_failed =
      QObject::connect(loader_.get(), &PJ::FileLoader::fileLoadFailed, &loop, [&](const QString&, const QString&) {
        ok = false;
        done = true;
        maybe_finish();
      });

  loader_->loadFile(path, &parent, hints);
  if (!done || !clicked_continue) {
    QTimer::singleShot(10000, &loop, [&loop]() { loop.quit(); });
    loop.exec();
  }
  QObject::disconnect(on_loaded);
  QObject::disconnect(on_failed);
  dismiss.stop();

  ASSERT_TRUE(clicked_continue) << "the test never clicked the visible Continue button";
  ASSERT_TRUE(done) << "the load never completed after answering the plugin message box";
  EXPECT_TRUE(ok) << "loadConfig must receive Continue instead of the host's premature -1";

  const std::vector<std::string> markers = readProbeLines(probe_path);
  EXPECT_EQ(std::count(markers.begin(), markers.end(), std::string("load_config_continued")), 1)
      << "the plugin must synchronously observe the clicked Continue answer";
  EXPECT_EQ(std::count(markers.begin(), markers.end(), std::string("load_config_aborted")), 0)
      << "the plugin observed -1/Abort before the user's click was delivered";
}

TEST_F(MessageBoxMarshalTest, ShutdownRejectsWorkerMessageBeforeJoining) {
  QWidget parent;
  const QString path = data_dir_.filePath(u"shutdown.msgboxmock"_s);
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.close();

  PJ::LoadHints hints;
  hints.expected_plugin_id = u"Msgbox Mock Source"_s;
  hints.preset_config_json = uR"({"ask_msgbox":true})"_s;
  hints.dialog_policy = PJ::DialogPolicy::kPreferPreset;
  ASSERT_TRUE(loader_->loadFile(path, &parent, hints));

  QEventLoop wait_for_dialog;
  QTimer poll;
  poll.setInterval(5);
  QObject::connect(&poll, &QTimer::timeout, &wait_for_dialog, [&]() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
      if (qobject_cast<PJ::MessageBox*>(widget) != nullptr && widget->isVisible()) {
        wait_for_dialog.quit();
        return;
      }
    }
  });
  QTimer::singleShot(2000, &wait_for_dialog, &QEventLoop::quit);
  poll.start();
  wait_for_dialog.exec();
  poll.stop();

  bool found = false;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    found = found || (qobject_cast<PJ::MessageBox*>(widget) != nullptr && widget->isVisible());
  }
  ASSERT_TRUE(found) << "worker never opened its asynchronous message box";

  // Must return: joinForShutdown rejects the dialog first, releasing the worker
  // from the synchronous plugin ABI, and only then waits for QThread::finished.
  loader_->joinForShutdown();
  EXPECT_FALSE(loader_->isBusy());
}

// The harder shutdown race: the worker posts its message-box request but the GUI
// thread never dispatches it (we never pump the event loop before joining). The
// gate must still unblock the worker with -1 so joinForShutdown returns without
// hanging, and the queued GUI open — if it ever runs — is a no-op. We assert the
// join is bounded and the plugin's askContinue observed -1 (recorded "aborted",
// never "continued") via the probe file.
TEST_F(MessageBoxMarshalTest, ShutdownAnswersQueuedWorkerMessageWithMinusOne) {
  QTemporaryDir probe_dir;
  ASSERT_TRUE(probe_dir.isValid());
  const QString probe_path = probe_dir.filePath(u"msgbox_probe.log"_s);
  qputenv("PJ_MSGBOX_PROBE_FILE", probe_path.toUtf8());
  const auto cleanup_env = qScopeGuard([]() { qunsetenv("PJ_MSGBOX_PROBE_FILE"); });

  QWidget parent;
  const QString path = data_dir_.filePath(u"queued_shutdown.msgboxmock"_s);
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.close();

  PJ::LoadHints hints;
  hints.expected_plugin_id = u"Msgbox Mock Source"_s;
  hints.preset_config_json = uR"({"ask_msgbox":true})"_s;
  hints.dialog_policy = PJ::DialogPolicy::kPreferPreset;
  ASSERT_TRUE(loader_->loadFile(path, &parent, hints));

  // Deliberately do NOT pump the event loop: the worker's queued GUI open sits
  // unserved. joinForShutdown must shut the gate (answering the worker -1) BEFORE
  // waiting on the worker, or it would deadlock against that unserved queue entry.
  QElapsedTimer timer;
  timer.start();
  loader_->joinForShutdown();
  const qint64 elapsed_ms = timer.elapsed();

  EXPECT_FALSE(loader_->isBusy());
  EXPECT_LT(elapsed_ms, 5000) << "joinForShutdown must return promptly, not block on the unserved GUI open";

  // No visible message box was ever dispatched (we never ran the event loop).
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    EXPECT_FALSE(qobject_cast<PJ::MessageBox*>(widget) != nullptr && widget->isVisible())
        << "the queued message box must not have opened";
  }

  // The worker (joined by joinForShutdown) recorded its askContinue outcome: the
  // gate answered -1, so askContinue was false and the plugin aborted.
  const std::vector<std::string> markers = readProbeLines(probe_path);
  ASSERT_FALSE(markers.empty()) << "the worker never reached askContinue";
  EXPECT_EQ(std::count(markers.begin(), markers.end(), std::string("aborted")), 1)
      << "the shutdown gate must answer the worker -1 (askContinue == false)";
  EXPECT_EQ(std::count(markers.begin(), markers.end(), std::string("continued")), 0)
      << "the worker must not have been told to continue during shutdown";
}

}  // namespace

PJ_APP_TEST_MAIN("file_loader_test")
