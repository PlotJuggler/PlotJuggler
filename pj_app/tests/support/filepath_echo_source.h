#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// In-process file source for the layout-replay path tests. It reads the
// `filepath` of its config, records every config it receives and every path it
// opens, and echoes the config back verbatim from saveConfig so the host's
// handling of reserved keys is observable. The file's lines become the rows of
// topic `echo/value` (t = row * 100, value = the line as a number); a missing
// file fails the import.

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/sdk/data_source_patterns.hpp"
#include "pj_base/sdk/data_source_plugin_base.hpp"
#include "pj_runtime/PluginRuntimeCatalog.h"

namespace pj_app_test {

struct FilepathEchoProbe {
  std::vector<std::string> configs;  // every loadConfig() argument, in order
  std::vector<std::string> opened;   // every path importData() tried to open
};

namespace detail {
inline std::mutex& echoMutex() {
  static std::mutex mutex;
  return mutex;
}
inline FilepathEchoProbe& echoState() {
  static FilepathEchoProbe probe;
  return probe;
}
}  // namespace detail

inline void resetFilepathEcho() {
  const std::lock_guard<std::mutex> lock(detail::echoMutex());
  detail::echoState() = {};
}

[[nodiscard]] inline FilepathEchoProbe filepathEchoProbe() {
  const std::lock_guard<std::mutex> lock(detail::echoMutex());
  return detail::echoState();
}

// Writes `lines` to `path` (creating parent directories) and returns the
// canonical path: the macOS temp dir sits behind a symlink.
[[nodiscard]] inline QString writeEchoFile(const QString& path, const QStringList& lines) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    file.write(lines.join(u'\n').toUtf8());
    file.close();
  }
  const QString canonical = QFileInfo(path).canonicalFilePath();
  return canonical.isEmpty() ? path : canonical;
}

class FilepathEchoSource final : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
  }

  std::string saveConfig() const override {
    return config_;
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    config_.assign(config_json);
    const std::lock_guard<std::mutex> lock(detail::echoMutex());
    detail::echoState().configs.emplace_back(config_json);
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(config_));
    const QString path = doc.object().value(QStringLiteral("filepath")).toString();
    {
      const std::lock_guard<std::mutex> lock(detail::echoMutex());
      detail::echoState().opened.push_back(path.toStdString());
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
      return fail("cannot open file " + path.toStdString());
    }
    QStringList lines;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
      lines.push_back(stream.readLine());
    }
    auto topic = writeHost().ensureTopic("echo/value");
    if (!topic) {
      return PJ::unexpected(topic.error());
    }
    for (qsizetype row = 0; row < lines.size(); ++row) {
      if (auto status = writeHost().appendRecord(
              *topic, PJ::Timestamp{static_cast<int64_t>(row) * 100},
              {{.name = "value", .value = lines[row].toDouble()}});
          !status) {
        return status;
      }
    }
    return PJ::okStatus();
  }

 private:
  PJ::Status fail(const std::string& message) const {
    runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kError, message);
    return PJ::unexpected(message);
  }

  std::string config_ = "{}";
};

[[nodiscard]] inline const PJ_data_source_vtable_t* filepathEchoVtable() {
  static const PJ_data_source_vtable_t* vtable = PJ::DataSourcePluginBase::vtableWithCreate(
      []() noexcept -> void* {
        try {
          return new FilepathEchoSource();
        } catch (...) {
          return nullptr;
        }
      },
      R"({"id":"filepath-echo-source","name":"Filepath Echo Source","version":"1.0.0",)"
      R"("file_extensions":[".echo"]})");
  return vtable;
}

// Clears the probe and registers the echo source, the common start of a test.
[[nodiscard]] inline bool registerFilepathEcho(PJ::PluginRuntimeCatalog& catalog) {
  resetFilepathEcho();
  return catalog.registerStaticDataSource(filepathEchoVtable());
}

}  // namespace pj_app_test
