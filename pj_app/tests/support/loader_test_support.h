#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Shared scaffolding for the standalone FileLoader-pipeline test binaries
// (file_loader_test, source_promotion_host_test,
// headless_descriptor_provider_session_test): the queued-event pump helpers
// and the common main() (offscreen QApplication + QSettings redirect). The
// one-binary-per-suite pattern stays — only the verbatim scaffolding lives
// here. The matching compiled scaffolding is the pj_app_loader_testlib
// OBJECT library in pj_app/CMakeLists.txt.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <string>
#include <vector>

#include "FileLoader.h"
#include "pj_datastore/reader.hpp"

namespace pj_app_test {

// Deliver already-queued metacalls/terminals: bounded zero-timer loop
// passes. More than one pass because a terminal may itself be scheduled from
// another queued event (a worker's completion metacall, a cross-thread
// cancel marshal) — callers pick the depth they need.
inline void flushQueuedEvents(int passes = 3) {
  for (int i = 0; i < passes; ++i) {
    QEventLoop loop;
    QTimer::singleShot(0, &loop, &QEventLoop::quit);
    loop.exec();
  }
}

// Pump the GUI loop until `done` returns true. Bounded so a regression fails
// instead of hanging CI.
[[nodiscard]] inline bool pumpUntil(const std::function<bool()>& done, int timeout_ms = 15000) {
  QElapsedTimer timer;
  timer.start();
  while (!done()) {
    if (timer.hasExpired(timeout_ms)) {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return true;
}

// Creates an empty file for the SDK's mock_file_source_plugin to ingest.
[[nodiscard]] inline QString makeMockFile(const QTemporaryDir& dir, const QString& name) {
  const QString path = dir.filePath(name);
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::WriteOnly));
  file.close();
  // Canonical, like the stored source path: the macOS temp dir sits behind a
  // symlink (/var -> /private/var).
  const QString canonical = QFileInfo(path).canonicalFilePath();
  return canonical.isEmpty() ? path : canonical;
}

// Unified hints builder: skip the dialog, target the mock source, with an
// optional preset config (e.g. {"fail_start":true} or a __pj_fanout list) and
// the prefer_reuse flag a layout replay sets.
[[nodiscard]] inline PJ::LoadHints mockLoadHints(
    const QString& config = QStringLiteral("{}"), bool prefer_reuse = false) {
  PJ::LoadHints hints;
  hints.expected_plugin_id = QStringLiteral("Mock File Source");
  hints.preset_config_json = config;
  hints.dialog_policy = PJ::DialogPolicy::kPreferPreset;
  hints.prefer_reuse = prefer_reuse;
  return hints;
}

// Enqueue a load and pump the event loop until it completes. Single-instance
// loads and fanout run on a worker thread, so completion is asynchronous
// (fileLoaded/fileLoadFailed). Layout reuse and early failures may still
// complete synchronously (the signal fires before exec(), so done is already
// set and we skip the loop). On success `emitted_config` receives the config
// fileLoaded carried; on failure `failure_reason` receives the reason.
[[nodiscard]] inline bool loadAndWait(
    PJ::FileLoader& loader, PJ::LoadInput input, const PJ::LoadHints& hints, QString* emitted_config = nullptr,
    QString* failure_reason = nullptr) {
  QEventLoop loop;
  bool ok = false;
  bool done = false;
  const auto on_loaded = QObject::connect(
      &loader, &PJ::FileLoader::fileLoaded, &loop,
      [&](const QString&, const QString&, const QString&, const QString& config) {
        if (emitted_config != nullptr) {
          *emitted_config = config;
        }
        ok = true;
        done = true;
        loop.quit();
      });
  const auto on_failed =
      QObject::connect(&loader, &PJ::FileLoader::fileLoadFailed, &loop, [&](const QString&, const QString& reason) {
        if (failure_reason != nullptr) {
          *failure_reason = reason;
        }
        ok = false;
        done = true;
        loop.quit();
      });
  loader.loadFile(std::move(input), nullptr, hints);
  if (!done) {
    QTimer::singleShot(10000, &loop, [&loop]() { loop.quit(); });  // safety: fail, don't hang CI
    loop.exec();
  }
  QObject::disconnect(on_loaded);
  QObject::disconnect(on_failed);
  return ok;
}

[[nodiscard]] inline bool loadAndWait(
    PJ::FileLoader& loader, const QString& path, const PJ::LoadHints& hints, QString* emitted_config = nullptr,
    QString* failure_reason = nullptr) {
  return loadAndWait(loader, PJ::LoadInput::fromNativePath(path), hints, emitted_config, failure_reason);
}

// The values of the dataset's single topic, in time order. Empty when the
// dataset does not have exactly one topic. Row COUNT alone cannot tell one
// load's data from another's; the values can.
[[nodiscard]] inline std::vector<double> singleTopicValues(const PJ::DataReader& reader, PJ::DatasetId dataset_id) {
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

[[nodiscard]] inline QString compactJson(const QJsonObject& object) {
  return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

[[nodiscard]] inline QJsonObject parseJsonObject(const QString& json) {
  return QJsonDocument::fromJson(json.toUtf8()).object();
}

[[nodiscard]] inline QJsonObject parseJsonObject(const std::string& json) {
  return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
}

}  // namespace pj_app_test

// The common main(): offscreen platform (the loader's dialog/progress
// widgets need a QApplication) and a QSettings redirect keeping test
// settings out of the user's real PlotJuggler4.conf. `app_name` is the
// per-binary QSettings application name (a string literal).
#define PJ_APP_TEST_MAIN(app_name)                                                       \
  int main(int argc, char** argv) {                                                      \
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));                          \
    ::testing::InitGoogleTest(&argc, argv);                                              \
    QApplication app(argc, argv);                                                        \
    QCoreApplication::setOrganizationName(QStringLiteral("PJ4Tests"));                   \
    QCoreApplication::setApplicationName(QStringLiteral(app_name));                      \
    static QTemporaryDir settings_dir;                                                   \
    QSettings::setDefaultFormat(QSettings::IniFormat);                                   \
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path()); \
    return RUN_ALL_TESTS();                                                              \
  }
