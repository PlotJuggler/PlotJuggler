// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ActiveWidgetsMarker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtGlobal>
#include <algorithm>
#include <limits>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>

#include <cerrno>
#include <csignal>
#endif

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

constexpr int kFormatVersion = 1;

void appendUnique(std::vector<ActiveWidgetsMarker::Entry>& entries, ActiveWidgetsMarker::Entry entry) {
  if (std::ranges::find(entries, entry) == entries.end()) {
    entries.push_back(std::move(entry));
  }
}

}  // namespace

ActiveWidgetsMarker::ActiveWidgetsMarker(QString directory, qint64 pid)
    : directory_(std::move(directory)), path_(QDir(directory_).filePath(fileNameFor(pid))) {}

QString ActiveWidgetsMarker::defaultDirectory() {
  return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QString ActiveWidgetsMarker::fileNameFor(qint64 pid) {
  return u"active_widgets.%1.json"_s.arg(pid);
}

qint64 ActiveWidgetsMarker::currentPid() {
  return QCoreApplication::applicationPid();
}

bool ActiveWidgetsMarker::isProcessAlive(qint64 pid) {
  if (pid <= 0) {
    return false;
  }
#if defined(Q_OS_WIN)
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) {
    // A process that exists but denies even this query is alive; one that does not exist is not.
    return GetLastError() == ERROR_ACCESS_DENIED;
  }
  DWORD exit_code = 0;
  const bool alive = GetExitCodeProcess(process, &exit_code) != 0 && exit_code == STILL_ACTIVE;
  CloseHandle(process);
  return alive;
#else
  if (pid > std::numeric_limits<pid_t>::max()) {
    return false;
  }
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

void ActiveWidgetsMarker::add(const void* owner, const QString& plugin_id, const QString& type) {
  std::erase_if(live_, [owner](const auto& item) { return item.first == owner; });
  live_.emplace_back(owner, Entry{plugin_id, type});
  write();
}

void ActiveWidgetsMarker::remove(const void* owner) {
  if (std::erase_if(live_, [owner](const auto& item) { return item.first == owner; }) > 0) {
    write();
  }
}

void ActiveWidgetsMarker::clear() {
  live_.clear();
  QFile::remove(path_);
}

void ActiveWidgetsMarker::write() const {
  if (live_.empty()) {
    QFile::remove(path_);
    return;
  }
  std::vector<Entry> entries;
  for (const auto& [owner, entry] : live_) {
    appendUnique(entries, entry);
  }
  QJsonArray widgets;
  for (const Entry& entry : entries) {
    widgets.append(QJsonObject{{u"plugin"_s, entry.plugin_id}, {u"type"_s, entry.type}});
  }
  const QJsonObject root{{u"version"_s, kFormatVersion}, {u"widgets"_s, widgets}};

  QDir().mkpath(QFileInfo(path_).absolutePath());
  QSaveFile file(path_);
  if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0 ||
      !file.commit()) {
    qWarning("Could not write the active widgets record %s: %s", qPrintable(path_), qPrintable(file.errorString()));
  }
}

ActiveWidgetsMarker::ReadResult ActiveWidgetsMarker::read(const QString& path) {
  ReadResult result;
  QFile file(path);
  if (!file.exists()) {
    return result;
  }
  if (!file.open(QIODevice::ReadOnly)) {
    result.error = u"cannot open %1: %2"_s.arg(path, file.errorString());
    return result;
  }
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError) {
    result.error = u"%1 is not valid JSON: %2"_s.arg(path, parse_error.errorString());
    return result;
  }
  const QJsonValue widgets = document.object().value(u"widgets"_s);
  if (!document.isObject() || !widgets.isArray()) {
    result.error = u"%1 has no \"widgets\" array"_s.arg(path);
    return result;
  }
  for (const QJsonValue& item : widgets.toArray()) {
    const QString plugin_id = item.toObject().value(u"plugin"_s).toString();
    if (!plugin_id.isEmpty()) {
      appendUnique(result.entries, Entry{plugin_id, item.toObject().value(u"type"_s).toString()});
    }
  }
  return result;
}

ActiveWidgetsMarker::ReadResult ActiveWidgetsMarker::take(const QString& directory, const ProcessProbe& alive) {
  ReadResult merged;
  static const QRegularExpression kRecordName(u"^active_widgets\\.(\\d+)\\.json$"_s);
  const QDir dir(directory);
  for (const QString& name : dir.entryList(QDir::Files, QDir::Name)) {
    const QRegularExpressionMatch match = kRecordName.match(name);
    bool parsed = false;
    const qint64 pid = match.hasMatch() ? match.captured(1).toLongLong(&parsed) : 0;
    if (!parsed || alive(pid)) {
      continue;
    }
    const QString path = dir.filePath(name);
    ReadResult one = read(path);
    QFile::remove(path);
    for (Entry& entry : one.entries) {
      appendUnique(merged.entries, std::move(entry));
    }
    if (!one.error.isEmpty()) {
      merged.error += (merged.error.isEmpty() ? QString() : u"; "_s) + one.error;
    }
  }
  return merged;
}

}  // namespace PJ
