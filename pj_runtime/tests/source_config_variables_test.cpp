// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <string>

#include "pj_runtime/SourceConfigVariables.h"

using namespace Qt::StringLiterals;

namespace {

QJsonObject expandToObject(const QString& json, const QUrl& page_url) {
  QString error = u"stale"_s;
  const QString expanded = PJ::expandSourceConfigVariables(json, page_url, &error);
  EXPECT_TRUE(error.isEmpty()) << page_url.toString().toStdString() << ": " << error.toStdString();
  return QJsonDocument::fromJson(expanded.toUtf8()).object();
}

}  // namespace

TEST(SourceConfigVariablesTest, WebSocketOriginAndMissingContext) {
  const QString bridge = uR"({"url":"${deployment.ws_origin}/bridge"})"_s;
  const auto url_for_page = [&bridge](const char* page) {
    return expandToObject(bridge, QUrl(QString::fromLatin1(page))).value(u"url"_s).toString().toStdString();
  };
  EXPECT_EQ(url_for_page("http://robot.local:8080/app/index.html?preset=arm#top"), "ws://robot.local:8080/bridge");
  EXPECT_EQ(url_for_page("https://robot.example/app/"), "wss://robot.example/bridge");
  EXPECT_EQ(url_for_page("https://[2001:db8::7]:8443/app/"), "wss://[2001:db8::7]:8443/bridge");
  EXPECT_EQ(url_for_page("http://operator:secret@10.0.0.2/"), "ws://10.0.0.2/bridge");

  // Only string values expand, at any depth; keys and non-string values stay as written.
  const QJsonObject nested = expandToObject(
      uR"({"${deployment.ws_origin}":"key","port":9871,"secure":true,)"
      uR"("urls":["${deployment.ws_origin}","${deployment.ws_origin}/a"],)"
      uR"("inner":{"url":"x${deployment.ws_origin}"}})"_s,
      QUrl(u"https://robot.example:4443/"_s));
  EXPECT_EQ(nested.value(u"${deployment.ws_origin}"_s).toString(), u"key"_s);
  EXPECT_EQ(nested.value(u"port"_s).toInteger(), 9871);
  EXPECT_TRUE(nested.value(u"secure"_s).toBool());
  EXPECT_EQ(
      nested.value(u"urls"_s).toArray(), QJsonArray({u"wss://robot.example:4443"_s, u"wss://robot.example:4443/a"_s}));
  EXPECT_EQ(nested.value(u"inner"_s).toObject().value(u"url"_s).toString(), u"xwss://robot.example:4443"_s);

  // Raw text outside a string is not JSON: refused, never substituted textually.
  QString error;
  EXPECT_TRUE(
      PJ::expandSourceConfigVariables(uR"({"url": ${deployment.ws_origin}})"_s, QUrl(u"https://h/"_s), &error)
          .isEmpty());
  EXPECT_FALSE(error.isEmpty());

  // The desktop has no page URL; other schemes give no WebSocket origin either.
  for (const QUrl& page : {QUrl(), QUrl(u"file:///opt/pj/index.html"_s), QUrl(u"qrc:/index.html"_s)}) {
    error.clear();
    EXPECT_TRUE(PJ::expandSourceConfigVariables(bridge, page, &error).isEmpty()) << page.toString().toStdString();
    EXPECT_TRUE(error.contains(u"missing deployment context"_s)) << error.toStdString();
  }

  // An explicit URL needs no deployment context and comes back byte for byte.
  const QString explicit_url = uR"({ "url" : "ws://10.0.0.2:9871/bridge" })"_s;
  for (const QUrl& page : {QUrl(), QUrl(u"https://robot.example/"_s)}) {
    error = u"stale"_s;
    EXPECT_EQ(PJ::expandSourceConfigVariables(explicit_url, page, &error), explicit_url);
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
  }
}

// `${deployment.` is reserved: a name this build does not know, or a token without its `}`, is an error that names
// the token, reported before any missing deployment context and in text that is not JSON too. Any other `${...}` is
// the plugin's own template and passes through byte for byte.
TEST(SourceConfigVariablesTest, ReservedDeploymentNamespace) {
  const auto rejection = [](const QString& json, const QUrl& page) {
    QString error;
    EXPECT_TRUE(PJ::expandSourceConfigVariables(json, page, &error).isEmpty()) << json.toStdString();
    EXPECT_FALSE(error.contains(u"missing deployment context"_s)) << error.toStdString();
    return error.toStdString();
  };
  for (const QUrl& page : {QUrl(u"https://robot.example/"_s), QUrl()}) {
    const std::string unknown = rejection(uR"({"url":"${deployment.wsorigin}/bridge"})"_s, page);
    EXPECT_NE(unknown.find("${deployment.wsorigin}"), std::string::npos) << unknown;
    const std::string unterminated = rejection(uR"({"url":"${deployment.ws_origin/bridge"})"_s, page);
    EXPECT_NE(unterminated.find("${deployment.ws_origin/bridge"), std::string::npos) << unterminated;
    const std::string not_json = rejection(u"url=${deployment.host}/bridge"_s, page);
    EXPECT_NE(not_json.find("${deployment.host}"), std::string::npos) << not_json;
    const std::string not_json_unterminated = rejection(u"url=${deployment.ws_origin"_s, page);
    EXPECT_NE(not_json_unterminated.find("${deployment.ws_origin"), std::string::npos) << not_json_unterminated;
  }

  for (const QString& own : {uR"({ "path" : "${HOME}/x", "note": "${deployment}" })"_s, u"path=${HOME}/x"_s}) {
    for (const QUrl& page : {QUrl(), QUrl(u"https://robot.example/"_s)}) {
      QString error = u"stale"_s;
      EXPECT_EQ(PJ::expandSourceConfigVariables(own, page, &error), own);
      EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    }
  }
}
