// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// LIVE cross-repo E2E for the canonical layout import (stage-5 §1 E1(a), E4,
// E6): a real MainWindow offscreen, the REAL plugin DSOs (mcap-cloud +
// mcap-loader + ros-parser) staged into its extensions dir, against a LIVE
// pj-cloud server. Five scenarios, declaration order, one window per binary:
//   1. cold import -> promoted, with the live progressive witness
//      (>=1 real ingestProgressed; curve bound while the batch is active);
//   2. the literal GUI flow (E6): real save/load QActions with automated
//      modals, warm reload after unload, zero-network Prometheus counters;
//   3. EAGER_ONLY via the regular-file cache root (MCAP_CLOUD_CACHE_DIR
//      pointing at a FILE makes <file>/<digest>.lock fail non-contended =>
//      tee dropped => spec §9.6), `layout-import-eager-only` asserted
//      in-process, no artifact, re-save carries no <materialize>;
//   4. trust gate: fresh-miss refusal with the ledger deleted (zero network),
//      then ledger-seeded success;
//   5. three-way catalog-equality signature (E4d): complete EAGER dataset at
//      the pre-replace boundary (datasetAboutToBeReplaced) == promoted
//      dataset == later warm stock load, dataset-ID-normalized, with
//      DatasetId/TopicId stability and bound-curve survival.
//
// LIVE-GATED: every scenario GTEST_SKIPs cleanly unless ALL of
//   MCAP_CLOUD_E2E_URL         (e.g. ws://localhost:8082)
//   MCAP_CLOUD_E2E_EXTENSIONS  (dir with the three staged .so + manifests)
//   MCAP_CLOUD_E2E_VECTORS     (path to source-descriptor-vectors.json)
// are set — the env contract the e2e-layout-import.sh harness provides.
//
// Scenario identities are the FROZEN e2e-8082-* vector cases; their
// descriptor bytes + identities are consumed VERBATIM from the vectors file
// (the `canonical` string goes into the <materialize> CDATA byte-for-byte;
// no re-canonicalization — the vector is an independent witness of the
// cross-repo canonicalizer).
//
// XDG sandboxing (stage-5 §2, binding): main() below builds a PRIVATE
// XDG_CONFIG_HOME/XDG_CACHE_HOME/XDG_DATA_HOME (+ HOME + test org/app names)
// BEFORE QApplication/MainWindow construction — the plugin reads
// XDG_CONFIG_HOME directly at ImportRuntime construction. The sandbox is
// never switched mid-process; per-scenario state mutations (trust ledger
// file, MCAP_CLOUD_CACHE_DIR) are legal because every load's
// HeadlessDescriptorProviderSession constructs a FRESH plugin instance that
// re-reads them — and each scenario mutates only after the prior batch
// settled and was retired. Each scenario documents the sandbox state it
// assumes and restores.

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_runtime/CatalogModel.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

namespace {

using PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::flushQueuedEvents;
using pj_app_test::pumpUntil;
using Peer = MainWindowLayoutImportTestPeer;

// The cloud connector's stable manifest id — the <materialize provider>
// routing key (HeadlessDescriptorProviderSession::create resolves it in the
// plugin catalog) and the provider id a promotion-attached SourceRecord must
// carry.
constexpr const char* kProviderId = "mcap-cloud";

// The three frozen stage-5 vector case names (docs/source-descriptor-
// vectors.json). Bytes are read from $MCAP_CLOUD_E2E_VECTORS at runtime.
constexpr const char* kCaseMain = "e2e-8082-main-cold-warm";
constexpr const char* kCaseEager = "e2e-8082-eager-leg";
constexpr const char* kCaseTrust = "e2e-8082-trust-leg";

// Curve references into the deterministic gen-ci-fixtures corpus, decoded by
// the REAL ros-parser (specialized sensor_msgs/Imu and nav_msgs/Odometry
// handlers): field paths as parser_ros emits them (no leading slash).
constexpr const char* kImuTopic = "/imu";
constexpr const char* kImuField = "linear_acceleration/x";
constexpr const char* kOdomTopic = "/odom";
constexpr const char* kOdomField = "pose/pose/position/x";

// One frozen vector case: name -> {canonical bytes, identity}.
struct VectorCase {
  QString canonical;  // VERBATIM canonical descriptor bytes (the CDATA payload)
  QString identity;   // "mcap-cloud:v1:sha256/128:<32 hex>"
};

// Records every layoutRestoreSettled delivery for one load cycle. The window
// is suite-static, so the receiver context must be scenario-scoped (the
// settle-test pattern): an ASSERT abort tears the connection down with it.
struct SettleProbe {
  QObject scope;
  std::vector<bool> values;
  explicit SettleProbe(PJ::MainWindow& window) {
    QObject::connect(&window, &PJ::MainWindow::layoutRestoreSettled, &scope, [this](bool ok) { values.push_back(ok); });
  }
  [[nodiscard]] bool waitSettled(int timeout_ms = 60000) {
    return pumpUntil([this]() { return !values.empty(); }, timeout_ms);
  }
};

class MainWindowLayoutImportE2ETest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    const QString url = qEnvironmentVariable("MCAP_CLOUD_E2E_URL");
    const QString extensions = qEnvironmentVariable("MCAP_CLOUD_E2E_EXTENSIONS");
    const QString vectors = qEnvironmentVariable("MCAP_CLOUD_E2E_VECTORS");
    if (url.isEmpty() || extensions.isEmpty() || vectors.isEmpty()) {
      // Not live: leave window_ null; every scenario SKIPs from SetUp().
      return;
    }
    live_ = true;
    server_url_ = url;

    const QUrl parsed(url);
    ASSERT_TRUE(
        parsed.isValid() && (parsed.scheme() == QStringLiteral("ws") || parsed.scheme() == QStringLiteral("wss")))
        << "MCAP_CLOUD_E2E_URL must be ws://host:port, got: " << url.toStdString();
    ASSERT_NE(parsed.port(), -1) << "MCAP_CLOUD_E2E_URL must carry an explicit port (the trust "
                                    "ledger records scheme://host:port with the port explicit)";
    metrics_host_ = parsed.host();
    metrics_port_ = parsed.port();

    ASSERT_TRUE(loadVectors(vectors));

    project_dir_ = std::make_unique<QTemporaryDir>();
    ASSERT_TRUE(project_dir_->isValid());

    window_ = std::make_unique<PJ::MainWindow>(extensions);
    // The staged mcap-loader must be discoverable, or the promotion/warm legs
    // would silently degrade into provider-only observations.
    ASSERT_FALSE(appSession().extensionCatalog().findSourcesForExtension(QStringLiteral(".mcap")).empty())
        << "mcap-loader did not load from " << extensions.toStdString();
    pj_layout_import_gui::recordDiagnosticIds(*window_, diagnostic_ids_);
  }

  static void TearDownTestSuite() {
    window_.reset();
    project_dir_.reset();
  }

  void SetUp() override {
    if (!live_) {
      GTEST_SKIP() << "live E2E env not set (need MCAP_CLOUD_E2E_URL, "
                      "MCAP_CLOUD_E2E_EXTENSIONS, MCAP_CLOUD_E2E_VECTORS)";
    }
    // Diagnostic delivery is QUEUED: drain the previous scenario's stragglers
    // BEFORE clearing, or they land mid-scenario and poison the id counts.
    flushQueuedEvents();
    diagnostic_ids_.clear();
  }

  void TearDown() override {
    // MCAP_CLOUD_CACHE_DIR is scenario-3's lever; it must never leak into a
    // later scenario's fresh plugin instance (unconditional: an ASSERT abort
    // inside scenario 3 lands here too).
    qunsetenv("MCAP_CLOUD_CACHE_DIR");
    // Same reasoning for scenario 4's lever. It deletes the trust ledger and
    // re-seeds it only after two ASSERTs that can abort in between, which
    // would leave every later scenario refused as untrusted — a misleading
    // cascade on an already-failing run. The ledger content is fixed, so
    // restoring it here is idempotent; scenario 4 only ever deletes.
    if (live_) {
      writeTrustLedger();
    }
  }

  // ---- suite plumbing -----------------------------------------------------

  [[nodiscard]] static PJ::MainWindow& window() {
    return *window_;
  }
  [[nodiscard]] static PJ::AppSession& appSession() {
    return Peer::session(*window_);
  }
  [[nodiscard]] static PJ::SessionManager& sessionManager() {
    return appSession().sessionManager();
  }

  [[nodiscard]] static bool loadVectors(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
      ADD_FAILURE() << "cannot open vectors file " << path.toStdString();
      return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    for (const auto& value : doc.object().value(QStringLiteral("cases")).toArray()) {
      const QJsonObject obj = value.toObject();
      const QString name = obj.value(QStringLiteral("name")).toString();
      if (name == QLatin1String(kCaseMain) || name == QLatin1String(kCaseEager) || name == QLatin1String(kCaseTrust)) {
        vectors_.emplace_back(
            name, VectorCase{
                      obj.value(QStringLiteral("canonical")).toString(),
                      obj.value(QStringLiteral("identity")).toString(),
                  });
      }
    }
    if (vectors_.size() != 3u) {
      ADD_FAILURE() << "vectors file " << path.toStdString() << " is missing e2e-8082-* cases (found "
                    << vectors_.size() << " of 3)";
      return false;
    }
    return true;
  }

  [[nodiscard]] static VectorCase vectorCase(const char* name) {
    for (const auto& [case_name, data] : vectors_) {
      if (case_name == QLatin1String(name)) {
        return data;
      }
    }
    ADD_FAILURE() << "unknown vector case " << name;
    return {};
  }

  // ---- sandbox state helpers ---------------------------------------------

  [[nodiscard]] static QString trustLedgerPath() {
    return qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/mcap_cloud/trusted_origins.json");
  }

  // Write the ledger trusting the harness origin (port explicit — the
  // serialized-origin shape the plugin's TrustedOrigins uses).
  static void writeTrustLedger() {
    const QUrl parsed(server_url_);
    const QString origin = QStringLiteral("%1://%2:%3").arg(parsed.scheme(), parsed.host()).arg(parsed.port());
    QDir().mkpath(QFileInfo(trustLedgerPath()).absolutePath());
    QFile file(trustLedgerPath());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QStringLiteral("{\"v\":1,\"origins\":[\"%1\"]}").arg(origin).toUtf8());
  }

  static void removeTrustLedger() {
    QFile::remove(trustLedgerPath());
  }

  // The cache artifact a promoted import of `identity` materializes:
  // $XDG_CACHE_HOME/mcap_cloud/sessions/<identity-hex>.mcap.
  [[nodiscard]] static QString artifactPath(const QString& identity) {
    const QString hex = identity.section(QLatin1Char(':'), -1);
    return qEnvironmentVariable("XDG_CACHE_HOME") + QStringLiteral("/mcap_cloud/sessions/") + hex +
           QStringLiteral(".mcap");
  }

  // Unload every dataset through the shell's removal route (never the engine
  // directly). The warm legs REQUIRE this first: findAlreadyLoaded() resolves
  // by provenance and would short-circuit the reload into
  // kResolvedAlreadyLoaded, proving nothing.
  static void removeAllLoadedDatasets() {
    for (const auto& [dataset_id, name] : appSession().catalogModel().datasets()) {
      static_cast<void>(name);
      Peer::removeDatasetData(window(), dataset_id);
    }
    flushQueuedEvents();
  }

  // ---- layout authoring ---------------------------------------------------

  // One plot with a (topic, field) curve plus one <materialize>-bearing
  // <fileInfo> carrying the FROZEN vector bytes: provider/identity as
  // attributes, the canonical descriptor VERBATIM as CDATA. The saved
  // filename is the identity's artifact path (what a production save records).
  [[nodiscard]] static QDomDocument buildE2eLayoutDoc(
      const QString& curve_topic, const QString& curve_field, const VectorCase& vec) {
    QDomDocument doc;
    QDomElement root = doc.createElement(QStringLiteral("root"));
    root.setAttribute(QStringLiteral("pj4_version"), QStringLiteral("4"));
    root.setAttribute(QStringLiteral("binding"), QStringLiteral("source"));
    doc.appendChild(root);

    QDomElement tabbed = doc.createElement(QStringLiteral("tabbed_widget"));
    tabbed.setAttribute(QStringLiteral("parent"), QStringLiteral("main_window"));
    QDomElement tab = doc.createElement(QStringLiteral("Tab"));
    tab.setAttribute(QStringLiteral("id"), QStringLiteral("t1"));
    tab.setAttribute(QStringLiteral("containers"), QStringLiteral("1"));
    QDomElement container = doc.createElement(QStringLiteral("Container"));
    QDomElement dock_area = doc.createElement(QStringLiteral("DockArea"));
    dock_area.setAttribute(QStringLiteral("id"), QStringLiteral("a1"));
    dock_area.setAttribute(QStringLiteral("name"), QStringLiteral("View"));
    QDomElement plot = doc.createElement(QStringLiteral("plot"));
    plot.setAttribute(QStringLiteral("id"), QStringLiteral("plot1"));
    plot.setAttribute(QStringLiteral("mode"), QStringLiteral("TimeSeries"));
    QDomElement curve = doc.createElement(QStringLiteral("curve"));
    curve.setAttribute(QStringLiteral("topic"), curve_topic);
    curve.setAttribute(QStringLiteral("field"), curve_field);
    plot.appendChild(curve);
    dock_area.appendChild(plot);
    container.appendChild(dock_area);
    tab.appendChild(container);
    tabbed.appendChild(tab);
    root.appendChild(tabbed);

    QDomElement wrapper = doc.createElement(QStringLiteral("previouslyLoaded_Datafiles"));
    QDomElement file_info = doc.createElement(QStringLiteral("fileInfo"));
    file_info.setAttribute(QStringLiteral("filename"), artifactPath(vec.identity));
    QDomElement materialize = doc.createElement(QStringLiteral("materialize"));
    materialize.setAttribute(QStringLiteral("provider"), QLatin1String(kProviderId));
    materialize.setAttribute(QStringLiteral("identity"), vec.identity);
    PJ::layout_xml::appendJsonAsCdata(doc, materialize, vec.canonical);
    file_info.appendChild(materialize);
    wrapper.appendChild(file_info);
    root.appendChild(wrapper);
    return doc;
  }

  [[nodiscard]] static QString writeE2eLayout(
      const QString& stem, const QString& curve_topic, const QString& curve_field, const VectorCase& vec) {
    const QString path = project_dir_->filePath(stem + QStringLiteral(".pj4.xml"));
    EXPECT_TRUE(pj_layout_import_gui::writeLayoutFile(path, buildE2eLayoutDoc(curve_topic, curve_field, vec)));
    return path;
  }

  // ---- observation helpers ------------------------------------------------

  [[nodiscard]] static QStringList curveNames() {
    QStringList names;
    if (PJ::PlotWidget* plot = Peer::firstPlot(window())) {
      for (const auto& info : plot->curveList()) {
        names.push_back(info.source_name);
      }
    }
    names.sort();
    return names;
  }

  // The bound curves as dataset-ID-normalized "topic|field_path" lines: curve
  // keys embed the DatasetId (renumbered by a remove+reload cycle), so
  // cross-load curve identity compares the resolved catalog coordinates.
  [[nodiscard]] static QStringList curveTopicFieldPairs() {
    QStringList pairs;
    for (const QString& key : curveNames()) {
      const auto item = appSession().catalogModel().itemDescriptor(key);
      if (!item.has_value()) {
        pairs.push_back(QStringLiteral("unresolved-key:") + key);
        continue;
      }
      const PJ::ScalarFieldPayload* scalar = PJ::asScalarField(*item);
      pairs.push_back(item->topic_name + QStringLiteral("|") + (scalar != nullptr ? scalar->field_path : QString()));
    }
    pairs.sort();
    return pairs;
  }

  // Scrape one Prometheus counter off the live server's /metrics (plain
  // blocking HTTP over QTcpSocket — test-only). nullopt = scrape failure.
  [[nodiscard]] static std::optional<double> scrapeCounter(const QString& name) {
    QTcpSocket socket;
    socket.connectToHost(metrics_host_, static_cast<quint16>(metrics_port_));
    if (!socket.waitForConnected(5000)) {
      return std::nullopt;
    }
    const QByteArray request =
        "GET /metrics HTTP/1.1\r\nHost: " + metrics_host_.toUtf8() + "\r\nConnection: close\r\n\r\n";
    socket.write(request);
    if (!socket.waitForBytesWritten(5000)) {
      return std::nullopt;
    }
    QByteArray body;
    while (socket.state() == QAbstractSocket::ConnectedState && socket.waitForReadyRead(5000)) {
      body += socket.readAll();
    }
    body += socket.readAll();
    for (const QByteArray& line : body.split('\n')) {
      const QList<QByteArray> parts = line.trimmed().split(' ');
      if (parts.size() == 2 && parts[0] == name.toUtf8()) {
        bool ok = false;
        const double value = parts[1].toDouble(&ok);
        if (ok) {
          return value;
        }
      }
    }
    return std::nullopt;
  }

  // The saved-layout <materialize> elements: (provider, identity, CDATA text)
  // per fileInfo, in document order.
  struct SavedMaterialize {
    QString provider;
    QString identity;
    QString descriptor;
  };
  [[nodiscard]] static std::vector<SavedMaterialize> savedMaterializeRecords(const QString& path) {
    std::vector<SavedMaterialize> records;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
      ADD_FAILURE() << "cannot open saved layout " << path.toStdString();
      return records;
    }
    QDomDocument doc;
    if (!doc.setContent(&file)) {
      ADD_FAILURE() << "cannot parse saved layout " << path.toStdString();
      return records;
    }
    const QDomNodeList list = doc.elementsByTagName(QStringLiteral("materialize"));
    for (int i = 0; i < list.size(); ++i) {
      const QDomElement element = list.at(i).toElement();
      records.push_back(
          SavedMaterialize{
              element.attribute(QStringLiteral("provider")),
              element.attribute(QStringLiteral("identity")),
              element.text(),
          });
    }
    return records;
  }

  // Dataset-ID-normalized catalog signature (E4d): one sorted line per scalar
  // field — (topic, field_path, logical_type, row_count, time range) — and per
  // object topic — (topic, object_type, normalized metadata, count, range).
  // Counts/ranges come from the DATASTORE (DataReader / ObjectStore), the
  // authoritative source at the pre-replace boundary; structure comes from the
  // CatalogModel items. No DatasetId/TopicId ever enters a line.
  [[nodiscard]] static QStringList captureSignature(PJ::DatasetId dataset_id) {
    QStringList lines;
    const PJ::DataReader reader = sessionManager().createReader();
    auto& object_store = sessionManager().objectStore();
    for (const PJ::CatalogItem& item : appSession().catalogModel().items()) {
      if (item.dataset_id != dataset_id) {
        continue;
      }
      if (const PJ::ScalarFieldPayload* scalar = PJ::asScalarField(item)) {
        const auto meta = reader.getMetadata(scalar->topic_id);
        lines.push_back(QStringLiteral("scalar|%1|%2|type=%3|rows=%4|range=%5..%6")
                            .arg(item.topic_name, scalar->field_path)
                            .arg(static_cast<int>(scalar->logical_type))
                            .arg(meta ? static_cast<qulonglong>(meta->total_row_count) : 0)
                            .arg(meta ? static_cast<qlonglong>(meta->time_range_min) : 0)
                            .arg(meta ? static_cast<qlonglong>(meta->time_range_max) : 0));
      } else if (const PJ::ObjectTopicPayload* object = PJ::asObjectTopic(item)) {
        const auto range = object_store.timeRange(object->object_topic_id);
        QString normalized_meta = object->metadata_json;
        const QJsonDocument parsed = QJsonDocument::fromJson(object->metadata_json.toUtf8());
        if (parsed.isObject()) {
          normalized_meta = QString::fromUtf8(QJsonDocument(parsed.object()).toJson(QJsonDocument::Compact));
        }
        lines.push_back(QStringLiteral("object|%1|type=%2|meta=%3|count=%4|range=%5..%6")
                            .arg(item.topic_name)
                            .arg(static_cast<int>(object->object_type))
                            .arg(normalized_meta)
                            .arg(static_cast<qulonglong>(object_store.entryCount(object->object_topic_id)))
                            .arg(static_cast<qlonglong>(range.first))
                            .arg(static_cast<qlonglong>(range.second)));
      }
    }
    lines.sort();
    return lines;
  }

  // ---- modal automation (E6, the file_dialog_test technique) --------------

  // Drives the SAVE modal: reads the "bind to this data source" checkbox
  // default (first sighting), selects `path`, accepts.
  static void automateSaveDialog(const QString& path, bool* checkbox_seen_checked) {
    auto seen = std::make_shared<bool>(false);
    driveWhenModal([path, checkbox_seen_checked, seen](QWidget* modal) {
      auto* checkbox = modal->findChild<QCheckBox*>();
      auto* file_dialog = modal->findChild<QFileDialog*>();
      if (checkbox == nullptr || file_dialog == nullptr) {
        return;  // not the save dialog (yet)
      }
      if (!*seen) {
        *seen = true;
        *checkbox_seen_checked = checkbox->isChecked();  // the untouched default
      }
      selectPathInFileDialog(file_dialog, path);
      QMetaObject::invokeMethod(file_dialog, "accept", Qt::QueuedConnection);
    });
  }

  // Drives the OPEN modal: selects `path`, accepts.
  static void automateOpenDialog(const QString& path) {
    driveWhenModal([path](QWidget* modal) {
      auto* file_dialog = modal->findChild<QFileDialog*>();
      if (file_dialog == nullptr) {
        return;  // some other modal (e.g. a message box) — not ours to drive
      }
      selectPathInFileDialog(file_dialog, path);
      QMetaObject::invokeMethod(file_dialog, "accept", Qt::QueuedConnection);
    });
  }

  // selectFile() with a cross-directory ABSOLUTE path never lands offscreen:
  // the directory switch kicks an async QFileSystemModel load that clears the
  // filename edit again, so accept() keeps silently no-op'ing on an empty
  // name. Pin the directory, select the RELATIVE name, and backstop the
  // filename edit directly — idempotent, converges under re-driving.
  static void selectPathInFileDialog(QFileDialog* file_dialog, const QString& path) {
    const QFileInfo info(path);
    file_dialog->setDirectory(info.absolutePath());
    file_dialog->selectFile(info.fileName());
    if (auto* edit = file_dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
        edit != nullptr && edit->text().isEmpty()) {
      edit->setText(info.fileName());
    }
  }

  // Polls (50ms cadence, bounded) for an active modal and hands it to `drive`
  // on EVERY tick while one is up — QFileDialog::accept() silently no-ops
  // until its async directory model has populated the filename edit, so a
  // single selectFile+accept can be swallowed; idempotent re-driving until
  // the modal actually CLOSES is the robust shape. Stops once a modal was
  // seen and is gone (or the ~15s budget runs out — the blocked trigger then
  // times the test out visibly). Must be armed BEFORE triggering the QAction:
  // the modal blocks the caller until the driver closes it.
  static void driveWhenModal(std::function<void(QWidget*)> drive) {
    auto* timer = new QTimer(&window());
    timer->setInterval(50);
    auto tries = std::make_shared<int>(0);
    auto driven = std::make_shared<bool>(false);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, tries, driven, drive = std::move(drive)]() {
      QWidget* modal = QApplication::activeModalWidget();
      if (modal == nullptr) {
        if (*driven || ++*tries > 300) {
          timer->stop();
          timer->deleteLater();
        }
        return;
      }
      *driven = true;
      drive(modal);
    });
    timer->start();
  }

  // Auto-answers any message box that appears while armed (E6: "if a
  // trust/reload message box appears on this path, automate the choice"),
  // recording one entry per box. Handles BOTH the themed PJ::MessageBox
  // (a QDialog named "pjMessageBox"; its buttons are "pjMessageBoxButton"
  // QPushButtons in addButton order — the FIRST is the primary/recommended
  // action, e.g. "Reload source file" on the layout-load prompt) and a stock
  // QMessageBox. Scoped: disarms on destruction.
  class MessageBoxAutomator {
   public:
    MessageBoxAutomator() {
      timer_.setInterval(100);
      QObject::connect(&timer_, &QTimer::timeout, &timer_, [this]() {
        QWidget* modal = QApplication::activeModalWidget();
        if (modal == nullptr) {
          return;
        }
        if (auto* box = qobject_cast<QMessageBox*>(modal)) {
          seen.push_back(box->text());
          QMetaObject::invokeMethod(box, "accept", Qt::QueuedConnection);
          return;
        }
        if (modal->objectName() == QStringLiteral("pjMessageBox")) {
          const auto buttons = modal->findChildren<QPushButton*>(QStringLiteral("pjMessageBoxButton"));
          if (!buttons.isEmpty()) {
            seen.push_back(modal->windowTitle() + QStringLiteral(" -> ") + buttons.first()->text());
            QMetaObject::invokeMethod(buttons.first(), "click", Qt::QueuedConnection);
          }
        }
      });
      timer_.start();
    }
    QStringList seen;

   private:
    QTimer timer_;
  };

  inline static bool live_ = false;
  inline static QString server_url_;
  inline static QString metrics_host_;
  inline static int metrics_port_ = 0;
  inline static std::vector<std::pair<QString, VectorCase>> vectors_;
  inline static std::unique_ptr<QTemporaryDir> project_dir_;
  inline static std::unique_ptr<PJ::MainWindow> window_;
  inline static QStringList diagnostic_ids_;
};

// (1) Cold import -> promoted, with the live progressive witness.
//
// Sandbox: assumes a fresh cache (no artifact for the main identity) and
// writes the trust ledger (ws origin, port explicit). Leaves: the promoted
// dataset loaded, the artifact materialized, the ledger present — scenario 2
// builds on exactly this state.
TEST_F(MainWindowLayoutImportE2ETest, ColdImportPromotesWithLiveProgressiveWitness) {
  const VectorCase vec = vectorCase(kCaseMain);
  ASSERT_FALSE(vec.identity.isEmpty());
  ASSERT_TRUE(vec.canonical.contains(server_url_))
      << "vector server_uri and MCAP_CLOUD_E2E_URL disagree — harness/vector drift";
  ASSERT_FALSE(QFileInfo::exists(artifactPath(vec.identity)))
      << "cold leg requires a fresh cache; stale artifact at " << artifactPath(vec.identity).toStdString();
  writeTrustLedger();

  const QString layout = writeE2eLayout(QStringLiteral("cold-main"), kImuTopic, kImuField, vec);

  // Live progressive witness (E4e): >=1 REAL SessionManager::ingestProgressed
  // for the batch-announced dataset, and the curve observed bound WHILE the
  // batch was still active. Sampled inside the deliveries themselves (both
  // the progress ticks and the plots' curveListChanged), not after the fact.
  QObject scope;
  int progress_events = 0;
  bool curve_bound_mid_flight = false;
  QStringList curve_names_at_bind;
  std::optional<PJ::DatasetId> announced_dataset;
  const auto sample_mid_flight = [&]() {
    if (!curve_bound_mid_flight && Peer::totalCurveCount(window()) >= 1 && Peer::progressiveInFlight(window()) &&
        Peer::batchActive(window())) {
      curve_bound_mid_flight = true;
      curve_names_at_bind = curveNames();
    }
  };
  QObject::connect(
      &sessionManager(), &PJ::SessionManager::ingestProgressed, &scope,
      [&](PJ::IngestToken token, quint64 /*current*/, quint64 /*total*/) {
        const PJ::DatasetId dataset_id = token.dataset_id;
        auto* batch = Peer::batch(window());
        if (batch == nullptr || !batch->activeImportDataset().has_value() ||
            *batch->activeImportDataset() != dataset_id) {
          return;
        }
        announced_dataset = dataset_id;
        ++progress_events;
        sample_mid_flight();
      });

  SettleProbe settle(window());
  Peer::loadLayout(window(), layout, /*interactive=*/false);
  // The plots exist as soon as the layout applied; hook their bind signal so
  // the mid-flight sample runs at the exact moment the curve appears.
  if (PJ::PlotWidget* plot = Peer::firstPlot(window())) {
    QObject::connect(plot, &PJ::PlotWidgetBase::curveListChanged, &scope, sample_mid_flight);
  }

  ASSERT_TRUE(settle.waitSettled()) << "cold import never settled";
  ASSERT_EQ(settle.values.size(), 1u);
  EXPECT_TRUE(settle.values.front()) << "cold import restore must settle true";

  EXPECT_GE(progress_events, 1) << "must observe >=1 real ingestProgressed for the announced dataset";
  EXPECT_TRUE(curve_bound_mid_flight) << "the curve must be observed bound while the batch is still active";

  // Promoted, not eager-only: artifact + source record + no degradation diag.
  EXPECT_TRUE(QFileInfo::exists(artifactPath(vec.identity)))
      << "promoted import must materialize " << artifactPath(vec.identity).toStdString();
  EXPECT_EQ(diagnostic_ids_.count(QStringLiteral("layout-import-eager-only")), 0)
      << "ids seen: " << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();
  EXPECT_EQ(diagnostic_ids_.count(QStringLiteral("layout-import-unresolved-curves")), 0)
      << "ids seen: " << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();

  const auto datasets = appSession().catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 1u) << "exactly one dataset from the cold import";
  ASSERT_TRUE(announced_dataset.has_value());
  EXPECT_EQ(datasets.front().first, *announced_dataset)
      << "strict in-place promotion (I-11): the surviving dataset must BE the announced eager one";
  const PJ::SourceRecord* record = sessionManager().sourceRecord(datasets.front().first);
  ASSERT_NE(record, nullptr) << "promotion must attach the SourceRecord";
  EXPECT_EQ(record->provider_id, QLatin1String(kProviderId));
  EXPECT_EQ(record->source_identity, vec.identity) << "the identity is the frozen cross-repo witness";
  // The as-built promotion record carries the plugin's DISPLAY-form
  // serialization (toSourceDescriptorJson: the canonical fields with
  // "display_name" inserted first). Our cold layout embedded the canonical
  // bytes (which carry no display_name), so the expected record is a pure
  // byte-splice of the FROZEN vector bytes — no re-canonicalization here.
  const QString expected_record = QStringLiteral("{\"display_name\":\"\",") + vec.canonical.mid(1);
  EXPECT_EQ(record->descriptor_json, expected_record)
      << "the promotion-attached descriptor must be the display-form projection of the frozen bytes; got: "
      << record->descriptor_json.toStdString();

  // The SAME curve is still bound after promotion.
  EXPECT_EQ(Peer::totalCurveCount(window()), 1);
  EXPECT_EQ(curveNames(), curve_names_at_bind) << "the mid-flight-bound curve must survive promotion";
}

// (2) The literal GUI flow (E6) — automated modals, warm reload, zero network.
//
// Sandbox: assumes scenario 1's state (promoted dataset loaded, artifact
// present, ledger present). Leaves: the warm-reloaded dataset loaded, the
// saved layout file in the project dir, artifact + ledger untouched.
TEST_F(MainWindowLayoutImportE2ETest, GuiSaveUnloadReloadIsWarmAndZeroNetwork) {
  const VectorCase vec = vectorCase(kCaseMain);
  ASSERT_FALSE(appSession().catalogModel().datasets().empty()) << "scenario 1 must have left its dataset loaded";
  const QString artifact = artifactPath(vec.identity);
  ASSERT_TRUE(QFileInfo::exists(artifact));
  const QDateTime artifact_mtime = QFileInfo(artifact).lastModified();

  // --- the real Save Layout QAction, modal automated ---
  const QString saved_path = project_dir_->filePath(QStringLiteral("gui-saved.pj4.xml"));
  QAction* save_action = Peer::saveLayoutAction(window());
  ASSERT_NE(save_action, nullptr);
  ASSERT_TRUE(save_action->isEnabled());
  bool checkbox_default_checked = false;
  automateSaveDialog(saved_path, &checkbox_default_checked);
  save_action->trigger();  // blocks in the modal until the driver accepts
  flushQueuedEvents();

  EXPECT_TRUE(checkbox_default_checked) << "the save-data-source binding checkbox must default to CHECKED";
  ASSERT_TRUE(QFileInfo::exists(saved_path)) << "the GUI save must have written " << saved_path.toStdString();

  const auto records = savedMaterializeRecords(saved_path);
  ASSERT_EQ(records.size(), 1u) << "the saved layout must carry exactly one <materialize>";
  EXPECT_EQ(records.front().provider, QLatin1String(kProviderId));
  EXPECT_EQ(records.front().identity, vec.identity) << "the identity attribute is the frozen cross-repo witness";
  // The saved CDATA is the promotion-attached record: the plugin's display
  // form of the FROZEN canonical bytes (scenario 1 pinned that projection) —
  // asserted byte-exactly via the same pure splice, no re-serialization.
  EXPECT_EQ(records.front().descriptor, QStringLiteral("{\"display_name\":\"\",") + vec.canonical.mid(1))
      << "the saved CDATA must be the display-form projection of the frozen canonical bytes; got: "
      << records.front().descriptor.toStdString();

  // --- unload (consult-flagged): findAlreadyLoaded() would otherwise
  // short-circuit by provenance and the warm leg would prove nothing ---
  removeAllLoadedDatasets();
  ASSERT_TRUE(appSession().catalogModel().datasets().empty());

  const auto sessions_before = scrapeCounter(QStringLiteral("pj_cloud_sessions_total"));
  const auto connections_before = scrapeCounter(QStringLiteral("pj_cloud_ws_connections_total"));
  ASSERT_TRUE(sessions_before.has_value() && connections_before.has_value())
      << "metrics scrape failed against " << metrics_host_.toStdString() << ":" << metrics_port_;

  // --- the real Load Layout QAction, modal automated; any message box on the
  // path is auto-accepted and recorded ---
  SettleProbe settle(window());
  MessageBoxAutomator boxes;
  QAction* load_action = Peer::loadLayoutAction(window());
  ASSERT_NE(load_action, nullptr);
  ASSERT_TRUE(load_action->isEnabled());
  automateOpenDialog(saved_path);
  load_action->trigger();

  ASSERT_TRUE(settle.waitSettled()) << "warm reload never settled";
  EXPECT_TRUE(settle.values.front()) << "warm reload must settle true";
  EXPECT_EQ(Peer::totalCurveCount(window()), 1) << "the curve must rebind on the warm reload";
  EXPECT_EQ(diagnostic_ids_.count(QStringLiteral("layout-import-unresolved-curves")), 0)
      << "ids seen: " << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();

  // WARM HIT witnesses: zero network (both counters unchanged) + artifact
  // untouched (same mtime — nothing re-materialized it).
  const auto sessions_after = scrapeCounter(QStringLiteral("pj_cloud_sessions_total"));
  const auto connections_after = scrapeCounter(QStringLiteral("pj_cloud_ws_connections_total"));
  ASSERT_TRUE(sessions_after.has_value() && connections_after.has_value());
  EXPECT_EQ(*sessions_after, *sessions_before) << "warm reload must open ZERO sessions";
  EXPECT_EQ(*connections_after, *connections_before) << "warm reload must open ZERO ws connections";
  EXPECT_EQ(QFileInfo(artifact).lastModified(), artifact_mtime) << "warm reload must not touch the artifact";

  if (!boxes.seen.isEmpty()) {
    // Not an error by itself — but visible in the log for the runbook.
    std::cerr << "[e2e] auto-accepted message box(es): " << boxes.seen.join(QStringLiteral(" | ")).toStdString()
              << "\n";
  }
}

// (3) EAGER_ONLY — the pinned §10 requirement, via the E4c lever.
//
// Sandbox: assumes the ledger present (scenario 1). Starts by unloading every
// dataset. Sets MCAP_CLOUD_CACHE_DIR to a REGULAR FILE for exactly this load
// (a read-only DIRECTORY does NOT work — the cache chmods its root 0700
// first; the file makes <file>/<digest>.lock fail non-contended => the tee is
// dropped => spec §9.6 EAGER_ONLY). Restores: unsets the env (also in
// TearDown), removes nothing else. Scenario 1 is the writable-cache baseline
// that proves the parsers decode — keep the declaration order.
TEST_F(MainWindowLayoutImportE2ETest, BrokenCacheRootDegradesToEagerOnly) {
  const VectorCase vec = vectorCase(kCaseEager);
  ASSERT_FALSE(vec.identity.isEmpty());
  removeAllLoadedDatasets();
  ASSERT_FALSE(QFileInfo::exists(artifactPath(vec.identity))) << "eager leg requires a fresh identity (cache MISS)";

  // The lever: a REGULAR FILE as cache root.
  const QString cache_root_file = project_dir_->filePath(QStringLiteral("cache-root-as-file"));
  const QByteArray broken_root_marker("not a directory");
  {
    QFile file(cache_root_file);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(broken_root_marker);
  }
  qputenv("MCAP_CLOUD_CACHE_DIR", cache_root_file.toUtf8());

  const QString layout = writeE2eLayout(QStringLiteral("eager-leg"), kImuTopic, kImuField, vec);
  SettleProbe settle(window());
  Peer::loadLayout(window(), layout, /*interactive=*/false);
  ASSERT_TRUE(settle.waitSettled()) << "eager import never settled";
  EXPECT_TRUE(settle.values.front()) << "EAGER_ONLY is a usable outcome — the restore settles true";

  qunsetenv("MCAP_CLOUD_CACHE_DIR");  // restore immediately (TearDown backstops)

  // §10 observations: the degradation diagnostic fired, the eager data is
  // usable (curve bound), and NOTHING was materialized anywhere.
  EXPECT_GE(diagnostic_ids_.count(QStringLiteral("layout-import-eager-only")), 1)
      << "ids seen: " << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();
  EXPECT_EQ(Peer::totalCurveCount(window()), 1) << "the eager dataset must still bind the curve";
  EXPECT_EQ(diagnostic_ids_.count(QStringLiteral("layout-import-job-failed")), 0)
      << "EAGER_ONLY must not be a job failure; ids seen: " << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();
  EXPECT_FALSE(QFileInfo::exists(artifactPath(vec.identity))) << "no artifact may exist in the XDG cache";
  // The broken root must still BE the regular file the lever made it — i.e.
  // nothing replaced it with a directory and materialized underneath. (A path
  // check below it would be vacuous: every stat under a regular file is
  // ENOTDIR, so it could never fail.)
  const QFileInfo broken_root(cache_root_file);
  EXPECT_TRUE(broken_root.isFile()) << "the broken cache root must remain a regular file";
  EXPECT_EQ(broken_root.size(), broken_root_marker.size()) << "the broken cache root was written to";

  // A re-save carries NO <materialize> for the eager-only source (scenario 2
  // already proved the modal; the direct entry suffices here).
  const QString resaved = project_dir_->filePath(QStringLiteral("eager-resave.pj4.xml"));
  Peer::saveLayoutToPath(window(), resaved, /*include_data_source=*/true);
  ASSERT_TRUE(QFileInfo::exists(resaved));
  EXPECT_TRUE(savedMaterializeRecords(resaved).empty())
      << "an EAGER_ONLY dataset has no import record — the re-saved layout must carry no <materialize>";
}

// (4) Trust gate — fresh-miss refusal, then ledger-seeded success.
//
// Sandbox: starts by unloading every dataset and DELETING the trust ledger
// (the fresh identity guarantees a cache MISS — cache hits deliberately
// bypass the gate). Restores: re-writes the ledger (the state every earlier
// scenario ran under) and leaves its import loaded.
TEST_F(MainWindowLayoutImportE2ETest, TrustGateRefusesFreshMissUntilLedgerSeeded) {
  const VectorCase vec = vectorCase(kCaseTrust);
  ASSERT_FALSE(vec.identity.isEmpty());
  removeAllLoadedDatasets();
  ASSERT_FALSE(QFileInfo::exists(artifactPath(vec.identity))) << "trust leg requires a fresh identity (cache MISS)";
  removeTrustLedger();

  const auto sessions_before = scrapeCounter(QStringLiteral("pj_cloud_sessions_total"));
  const auto connections_before = scrapeCounter(QStringLiteral("pj_cloud_ws_connections_total"));
  ASSERT_TRUE(sessions_before.has_value() && connections_before.has_value());

  const QString layout = writeE2eLayout(QStringLiteral("trust-leg"), kOdomTopic, kOdomField, vec);
  {
    SettleProbe settle(window());
    Peer::loadLayout(window(), layout, /*interactive=*/false);
    ASSERT_TRUE(settle.waitSettled()) << "the refused restore must still reach settlement";
    flushQueuedEvents();  // the refusal settles synchronously; its diagnostic delivery is queued
    EXPECT_GE(diagnostic_ids_.count(QStringLiteral("layout-import-untrusted")), 1)
        << "ids seen: " << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();
    EXPECT_EQ(Peer::totalCurveCount(window()), 0) << "the refused source must not have produced data";
    EXPECT_TRUE(appSession().catalogModel().datasets().empty());
  }

  // Zero-network witness: the refusal happened at the bounded query — no
  // session, no connection.
  const auto sessions_mid = scrapeCounter(QStringLiteral("pj_cloud_sessions_total"));
  const auto connections_mid = scrapeCounter(QStringLiteral("pj_cloud_ws_connections_total"));
  ASSERT_TRUE(sessions_mid.has_value() && connections_mid.has_value());
  EXPECT_EQ(*sessions_mid, *sessions_before) << "a trust refusal must open ZERO sessions";
  EXPECT_EQ(*connections_mid, *connections_before) << "a trust refusal must open ZERO ws connections";

  // Ledger seeded -> the SAME layout now imports for real.
  writeTrustLedger();
  flushQueuedEvents();  // drain block-1 stragglers before the clean count
  diagnostic_ids_.clear();
  {
    SettleProbe settle(window());
    Peer::loadLayout(window(), layout, /*interactive=*/false);
    ASSERT_TRUE(settle.waitSettled()) << "the trusted import never settled";
    EXPECT_TRUE(settle.values.front()) << "the trusted import must settle true";
  }
  flushQueuedEvents();
  EXPECT_EQ(diagnostic_ids_.count(QStringLiteral("layout-import-untrusted")), 0)
      << "ids seen: " << diagnostic_ids_.join(QStringLiteral(", ")).toStdString();
  EXPECT_EQ(Peer::totalCurveCount(window()), 1) << "the trusted import must bind the curve";
}

// (5) Catalog equality (§12, E4d) — the three-way normalized signature on a
// dedicated cold cycle over the SAME main identity.
//
// Sandbox: assumes the ledger present (restored by scenario 4). Starts by
// unloading every dataset and DELETING the main artifact so the import runs
// cold again. Leaves: the warm-reloaded dataset, artifact re-materialized.
TEST_F(MainWindowLayoutImportE2ETest, CatalogSignatureIsEqualAcrossEagerPromotedAndWarm) {
  const VectorCase vec = vectorCase(kCaseMain);
  removeAllLoadedDatasets();
  ASSERT_TRUE(QFile::remove(artifactPath(vec.identity))) << "scenario 1/2 must have left the artifact in place";

  // (a) the complete EAGER dataset immediately before the promotion replace:
  // datasetAboutToBeReplaced is emitted synchronously at the head of the
  // replace transaction, before any detach — the honest pre-replace
  // observation point (direct connection, data still fully readable).
  QObject scope;
  QStringList signature_eager;
  std::vector<PJ::TopicId> topics_eager;
  std::optional<PJ::DatasetId> replaced_dataset;
  QObject::connect(
      &sessionManager(), &PJ::SessionManager::datasetAboutToBeReplaced, &scope, [&](PJ::DatasetId dataset_id) {
        if (replaced_dataset.has_value()) {
          return;  // first boundary only
        }
        replaced_dataset = dataset_id;
        signature_eager = captureSignature(dataset_id);
        topics_eager = sessionManager().createReader().listTopics(dataset_id);
        std::sort(topics_eager.begin(), topics_eager.end());
      });

  const QString layout = writeE2eLayout(QStringLiteral("equality-cold"), kImuTopic, kImuField, vec);
  {
    SettleProbe settle(window());
    Peer::loadLayout(window(), layout, /*interactive=*/false);
    ASSERT_TRUE(settle.waitSettled()) << "equality cold import never settled";
    ASSERT_TRUE(settle.values.front());
  }
  ASSERT_TRUE(replaced_dataset.has_value()) << "the promotion replace boundary was never observed";
  ASSERT_FALSE(signature_eager.isEmpty()) << "the eager dataset must have been observable pre-replace";

  // (b) the PROMOTED dataset: same DatasetId (strict in-place promotion),
  // identical TopicId set, identical normalized signature, curve survived.
  const auto datasets = appSession().catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 1u);
  ASSERT_EQ(datasets.front().first, *replaced_dataset) << "DatasetId must be stable across the promotion replace";
  std::vector<PJ::TopicId> topics_promoted = sessionManager().createReader().listTopics(*replaced_dataset);
  std::sort(topics_promoted.begin(), topics_promoted.end());
  EXPECT_EQ(topics_promoted, topics_eager) << "TopicIds must be stable across the promotion replace";
  const QStringList signature_promoted = captureSignature(*replaced_dataset);
  EXPECT_EQ(signature_promoted, signature_eager)
      << "EAGER vs PROMOTED catalog signatures must be identical\n--- eager ---\n"
      << signature_eager.join(QStringLiteral("\n")).toStdString() << "\n--- promoted ---\n"
      << signature_promoted.join(QStringLiteral("\n")).toStdString();
  EXPECT_EQ(Peer::totalCurveCount(window()), 1) << "the bound curve must survive the replace";
  const QStringList curves_after_promotion = curveTopicFieldPairs();

  // (c) a later stock WARM load after removing the prior dataset: save
  // source-bound, unload, reload -> the artifact loads through the stock
  // mcap-loader; the dataset-ID-normalized signature must still be identical.
  const QString saved = project_dir_->filePath(QStringLiteral("equality-resave.pj4.xml"));
  Peer::saveLayoutToPath(window(), saved, /*include_data_source=*/true);
  removeAllLoadedDatasets();
  {
    SettleProbe settle(window());
    Peer::loadLayout(window(), saved, /*interactive=*/false);
    ASSERT_TRUE(settle.waitSettled()) << "equality warm reload never settled";
    ASSERT_TRUE(settle.values.front());
  }
  const auto warm_datasets = appSession().catalogModel().datasets();
  ASSERT_EQ(warm_datasets.size(), 1u);
  const QStringList signature_warm = captureSignature(warm_datasets.front().first);
  EXPECT_EQ(signature_warm, signature_promoted)
      << "PROMOTED vs WARM catalog signatures must be identical\n--- promoted ---\n"
      << signature_promoted.join(QStringLiteral("\n")).toStdString() << "\n--- warm ---\n"
      << signature_warm.join(QStringLiteral("\n")).toStdString();
  // Dataset-ID-normalized curve identity: the warm dataset is a NEW id, so
  // the raw curve KEY differs by construction; the resolved (topic, field)
  // coordinates must not.
  EXPECT_EQ(curveTopicFieldPairs(), curves_after_promotion)
      << "the curve must rebind onto the same (topic, field) on the warm load";
  EXPECT_EQ(Peer::totalCurveCount(window()), 1);
}

}  // namespace

// The live-E2E main(): the private XDG sandbox + test org/app identity are
// installed BEFORE QApplication/MainWindow construction (the plugin reads
// XDG_CONFIG_HOME directly at ImportRuntime construction; QSettings and the
// plugin cache land inside the sandbox via the same env). Never switched
// mid-process. PJ_MAIN_WINDOW_TEST_MAIN is NOT used: it cannot install env
// this early.
int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QTemporaryDir sandbox;  // auto-removed on exit
  if (!sandbox.isValid()) {
    fprintf(stderr, "cannot create the XDG sandbox\n");
    return 1;
  }
  const QString root = sandbox.path();
  for (const char* sub : {"config", "cache", "data", "home"}) {
    QDir(root).mkpath(QLatin1String(sub));
  }
  qputenv("HOME", (root + QStringLiteral("/home")).toUtf8());
  qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/config")).toUtf8());
  qputenv("XDG_CACHE_HOME", (root + QStringLiteral("/cache")).toUtf8());
  qputenv("XDG_DATA_HOME", (root + QStringLiteral("/data")).toUtf8());
  QCoreApplication::setOrganizationName(QStringLiteral("pj4-e2e-test"));
  QCoreApplication::setApplicationName(QStringLiteral("layout-import-e2e"));
  QStandardPaths::setTestModeEnabled(true);
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
