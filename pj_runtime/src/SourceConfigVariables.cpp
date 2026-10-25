// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/SourceConfigVariables.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QUrl>

namespace PJ {

namespace {

constexpr QLatin1StringView kDeploymentPrefix("${deployment.");
constexpr QLatin1StringView kWebSocketOrigin("${deployment.ws_origin}");

// The first reserved token in `text` this build cannot fill (an unknown name, or no closing brace),
// described for the user; empty when every `${deployment.` token is one it knows.
QString deploymentVariableProblem(const QString& text) {
  for (qsizetype start = text.indexOf(kDeploymentPrefix); start >= 0;
       start = text.indexOf(kDeploymentPrefix, start + 1)) {
    const qsizetype close = text.indexOf(u'}', start);
    if (close < 0) {
      return QCoreApplication::translate(
                 "SourceConfigVariables", "The source configuration has a deployment variable without its '}': %1")
          .arg(text.mid(start));
    }
    const QStringView token = QStringView(text).sliced(start, close + 1 - start);
    if (token != kWebSocketOrigin) {
      return QCoreApplication::translate(
                 "SourceConfigVariables",
                 "The source configuration uses %1, which is not a "
                 "deployment variable this PlotJuggler knows.")
          .arg(token);
    }
  }
  return {};
}

// "ws://host:port" for an http page, "wss://..." for https; empty for anything else.
QString webSocketOrigin(const QUrl& page_url) {
  if (!page_url.isValid() || page_url.host().isEmpty()) {
    return {};
  }
  QString scheme;
  if (page_url.scheme() == QLatin1StringView("http")) {
    scheme = QStringLiteral("ws");
  } else if (page_url.scheme() == QLatin1StringView("https")) {
    scheme = QStringLiteral("wss");
  } else {
    return {};
  }
  QUrl origin = page_url.adjusted(QUrl::RemoveUserInfo | QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
  origin.setScheme(scheme);
  return origin.toString(QUrl::FullyEncoded);
}

// Replaces the variable in every string value under `value` (never in object keys); true when any changed.
// Stops at the first string with an unusable reserved token, describing it in `problem`.
bool expandStrings(QJsonValue& value, const QString& origin, QString& problem) {
  if (value.isString()) {
    QString text = value.toString();
    if (!text.contains(kDeploymentPrefix)) {
      return false;
    }
    problem = deploymentVariableProblem(text);
    if (!problem.isEmpty()) {
      return false;
    }
    value = text.replace(kWebSocketOrigin, origin);
    return true;
  }
  bool changed = false;
  if (value.isArray()) {
    QJsonArray array = value.toArray();
    for (qsizetype index = 0; index < array.size() && problem.isEmpty(); ++index) {
      QJsonValue element = array.at(index);
      if (expandStrings(element, origin, problem)) {
        array.replace(index, element);
        changed = true;
      }
    }
    if (changed) {
      value = array;
    }
  } else if (value.isObject()) {
    QJsonObject object = value.toObject();
    for (auto member = object.begin(); member != object.end() && problem.isEmpty(); ++member) {
      QJsonValue element = member.value();
      if (expandStrings(element, origin, problem)) {
        member.value() = element;
        changed = true;
      }
    }
    if (changed) {
      value = object;
    }
  }
  return changed;
}

}  // namespace

QString expandSourceConfigVariables(const QString& json, const QUrl& page_url, QString* error) {
  const auto fail = [error](const QString& message) {
    if (error != nullptr) {
      *error = message;
    }
    return QString();
  };
  if (error != nullptr) {
    error->clear();
  }
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError) {
    // Not JSON, so nothing to expand, unless a reserved token sits outside any string.
    if (!json.contains(kDeploymentPrefix)) {
      return json;
    }
    if (const QString problem = deploymentVariableProblem(json); !problem.isEmpty()) {
      return fail(problem);
    }
    return fail(
        QCoreApplication::translate(
            "SourceConfigVariables", "The source configuration uses %1 but is not valid JSON: %2.")
            .arg(kWebSocketOrigin, parse_error.errorString()));
  }
  QJsonValue root = document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
  const QString origin = webSocketOrigin(page_url);
  QString problem;
  const bool changed = expandStrings(root, origin, problem);
  if (!problem.isEmpty()) {
    return fail(problem);
  }
  if (!changed) {
    return json;
  }
  if (origin.isEmpty()) {
    return fail(
        QCoreApplication::translate(
            "SourceConfigVariables",
            "The source configuration uses %1, which only an app opened from an http(s) page can fill in "
            "(missing deployment context).")
            .arg(kWebSocketOrigin));
  }
  const QJsonDocument expanded = root.isArray() ? QJsonDocument(root.toArray()) : QJsonDocument(root.toObject());
  return QString::fromUtf8(expanded.toJson(QJsonDocument::Compact));
}

}  // namespace PJ
