// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QColor>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNodeList>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "FanoutConfig.h"
#include "LayoutXml.h"
#include "pj_runtime/XmlCdata.h"
using namespace Qt::StringLiterals;

namespace {

using PJ::layout_xml::DataSourceRef;

// ---------- appendJsonAsCdata ----------------------------------------------

QString roundTripJson(const QString& input) {
  QDomDocument doc;
  QDomElement plugin = doc.createElement(u"plugin"_s);
  PJ::appendJsonAsCdata(doc, plugin, input);
  doc.appendChild(plugin);

  // Round-trip through serialize -> reparse so the reader is exercised, not just the in-memory DOM.
  const QByteArray serialized = doc.toByteArray(2);
  QDomDocument reparsed;
  if (!reparsed.setContent(serialized)) {
    return u"__PARSE_FAILED__"_s;
  }
  return reparsed.documentElement().text();
}

TEST(AppendJsonAsCdata, RoundTripsSimpleJson) {
  const QString in = uR"({"topics":["/imu/accel"],"time":"publish"})"_s;
  EXPECT_EQ(roundTripJson(in), in);
}

TEST(AppendJsonAsCdata, RoundTripsEmptyString) {
  EXPECT_EQ(roundTripJson(QString()), QString());
}

TEST(AppendJsonAsCdata, RoundTripsJsonContainingClosingCdata) {
  // The literal "]]>" inside a CDATA section would close it. The helper
  // splits at every "]]>" boundary; text() on read concatenates them
  // back into the original string.
  const QString in = uR"({"pattern":"end ]]> middle ]]> tail"})"_s;
  EXPECT_EQ(roundTripJson(in), in);
}

TEST(AppendJsonAsCdata, RoundTripsJsonStartingWithClosingCdata) {
  const QString in = u"]]>{\"x\":1}"_s;
  EXPECT_EQ(roundTripJson(in), in);
}

TEST(AppendJsonAsCdata, RoundTripsJsonEndingWithClosingCdata) {
  const QString in = u"{\"x\":1}]]>"_s;
  EXPECT_EQ(roundTripJson(in), in);
}

TEST(AppendJsonAsCdata, RoundTripsConsecutiveClosingCdataSequences) {
  const QString in = u"a]]>]]>b]]>c"_s;
  EXPECT_EQ(roundTripJson(in), in);
}

TEST(AppendJsonAsCdata, RoundTripsUnicodeAndAngleBrackets) {
  const QString in = uR"({"label":"<x & y> ünïcødé"})"_s;
  EXPECT_EQ(roundTripJson(in), in);
}

// ---------- directCdataText (params vs <source_fallback> separation) --------
// A <processor> element carries its params as a direct CDATA child plus an optional <source_fallback> child
// element holding the filter's Luau source. QDomElement::text() recurses the subtree and would slurp the source into
// the params; directCdataText reads only the element's own CDATA.

QDomElement buildProcessorWithSource(QDomDocument& doc, const QString& params, const QString& source) {
  QDomElement processor = doc.createElement(u"processor"_s);
  PJ::appendJsonAsCdata(doc, processor, params);  // params: a direct CDATA child
  QDomElement src = doc.createElement(u"source_fallback"_s);
  PJ::appendJsonAsCdata(doc, src, source);
  processor.appendChild(src);  // source: nested inside a child element
  doc.appendChild(processor);
  return processor;
}

TEST(DirectCdataText, ReadsOwnPayloadIgnoringChildElement) {
  QDomDocument doc;
  const QString params = uR"({"value_scale":2.5})"_s;
  const QString source = u"return { id='scale', create=function(p) end }"_s;
  QDomElement processor = buildProcessorWithSource(doc, params, source);

  EXPECT_EQ(PJ::directCdataText(processor), params);
  EXPECT_EQ(processor.firstChildElement(u"source_fallback"_s).text(), source);
  // Document the gotcha being guarded against: text() recurses and merges both.
  EXPECT_EQ(processor.text(), params + source);
}

TEST(DirectCdataText, SurvivesSerializeReparseWithClosingCdata) {
  QDomDocument doc;
  const QString params = uR"({"pat":"a ]]> b"})"_s;
  const QString source = u"-- ]]> in source\nreturn {}"_s;
  buildProcessorWithSource(doc, params, source);

  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QDomElement processor = reparsed.documentElement();
  EXPECT_EQ(PJ::directCdataText(processor), params);
  EXPECT_EQ(processor.firstChildElement(u"source_fallback"_s).text(), source);
}

TEST(DirectCdataText, EmptyWhenNoDirectCdata) {
  QDomDocument doc;
  QDomElement processor = doc.createElement(u"processor"_s);
  QDomElement src = doc.createElement(u"source_fallback"_s);
  PJ::appendJsonAsCdata(doc, src, u"source-only"_s);
  processor.appendChild(src);
  doc.appendChild(processor);
  EXPECT_TRUE(PJ::directCdataText(processor).isEmpty());
}

// ---------- extractDataSource ----------------------------------------------

QDomDocument buildDataSourceDoc(
    const QString& filename, const QString& prefix = QString(), const QString& plugin_id = QString(),
    const QString& plugin_json = QString()) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  QDomElement wrapper = doc.createElement(u"previouslyLoaded_Datafiles"_s);
  root.appendChild(wrapper);
  QDomElement file_info = doc.createElement(u"fileInfo"_s);
  if (!filename.isNull()) {
    file_info.setAttribute(u"filename"_s, filename);
  }
  file_info.setAttribute(u"prefix"_s, prefix);
  if (!plugin_id.isEmpty()) {
    QDomElement plugin = doc.createElement(u"plugin"_s);
    plugin.setAttribute(u"ID"_s, plugin_id);
    PJ::appendJsonAsCdata(doc, plugin, plugin_json);
    file_info.appendChild(plugin);
  }
  wrapper.appendChild(file_info);
  return doc;
}

// Appends an extra <fileInfo> to an existing data-source doc, so a single doc
// can carry multiple loaded files (mirrors a multi-file session save).
void appendFileInfo(
    QDomDocument& doc, const QString& filename, const QString& prefix = QString(), const QString& plugin_id = QString(),
    const QString& plugin_json = QString()) {
  QDomElement wrapper = doc.documentElement().firstChildElement(u"previouslyLoaded_Datafiles"_s);
  QDomElement file_info = doc.createElement(u"fileInfo"_s);
  file_info.setAttribute(u"filename"_s, filename);
  file_info.setAttribute(u"prefix"_s, prefix);
  if (!plugin_id.isEmpty()) {
    QDomElement plugin = doc.createElement(u"plugin"_s);
    plugin.setAttribute(u"ID"_s, plugin_id);
    PJ::appendJsonAsCdata(doc, plugin, plugin_json);
    file_info.appendChild(plugin);
  }
  wrapper.appendChild(file_info);
}

TEST(ExtractDataSource, EmptyDocReturnsEmptyList) {
  QDomDocument doc;
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  EXPECT_TRUE(refs.isEmpty());
}

TEST(ExtractDataSource, MissingWrapperReturnsEmptyList) {
  QDomDocument doc;
  doc.appendChild(doc.createElement(u"root"_s));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  EXPECT_TRUE(refs.isEmpty());
}

TEST(ExtractDataSource, EmptyFilenameAttributeIsSkipped) {
  const QDomDocument doc = buildDataSourceDoc(u""_s);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  EXPECT_TRUE(refs.isEmpty());
}

TEST(ExtractDataSource, AbsolutePathPassesThrough) {
  // Build a genuinely-absolute path for the host platform. A hardcoded POSIX
  // path like "/tmp/x" is drive-relative on Windows, so QFileInfo would anchor
  // it at the current drive and the equality check would fail.
  const QString abs = QDir::tempPath() + u"/some_data.mcap"_s;
  const QDomDocument doc = buildDataSourceDoc(abs);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir(QDir::rootPath()));
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().resolved_path, QFileInfo(abs).absoluteFilePath());
}

TEST(ExtractDataSource, PreservesSerializedPathAndOpaqueUploadIdentity) {
  const QString identity = u"pj-upload://session/7/folder%2Flog.csv"_s;
  const QDomDocument doc = buildDataSourceDoc(identity);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir(u"/tmp/layouts"_s));
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().serialized_path, identity);
  EXPECT_EQ(refs.front().resolved_path, identity)
      << "an opaque browser identity must not be anchored under the layout directory";
}

TEST(ExtractDataSource, RelativePathIsAnchoredAtLayoutDir) {
  const QDomDocument doc = buildDataSourceDoc(u"data/run.csv"_s);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir(u"/tmp/layouts"_s));
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().serialized_path, u"data/run.csv"_s);
  EXPECT_EQ(refs.front().resolved_path, u"/tmp/layouts/data/run.csv"_s);
}

TEST(ExtractDataSource, CanonicalBrowserContentFingerprintIsOptionalAndValidated) {
  constexpr auto kSha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  QDomDocument doc = buildDataSourceDoc(u"duplicate.csv"_s);
  QDomElement file_info =
      doc.documentElement().firstChildElement(u"previouslyLoaded_Datafiles"_s).firstChildElement(u"fileInfo"_s);
  file_info.setAttribute(u"content_sha256"_s, QLatin1StringView(kSha256));
  QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().content_sha256, QLatin1StringView(kSha256));

  EXPECT_FALSE(refs.front().content_sha256_malformed);

  file_info.setAttribute(u"content_sha256"_s, u"ABCDEF"_s);
  refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().content_sha256.isEmpty());
  EXPECT_TRUE(refs.front().content_sha256_malformed);

  file_info.removeAttribute(u"content_sha256"_s);
  refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().content_sha256.isEmpty());
  EXPECT_FALSE(refs.front().content_sha256_malformed);
}

TEST(ExtractDataSource, SideFilesAreOptionalAndTheirFingerprintsValidated) {
  constexpr auto kSha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  QDomDocument doc = buildDataSourceDoc(u"run.log"_s);
  EXPECT_TRUE(PJ::layout_xml::extractDataSource(doc, QDir::current()).front().side_files.isEmpty())
      << "a layout without <sideFile> reads as before";
  QDomElement file_info =
      doc.documentElement().firstChildElement(u"previouslyLoaded_Datafiles"_s).firstChildElement(u"fileInfo"_s);
  for (const auto& [filename, sha256] : std::array<std::pair<QString, QString>, 3>{{
           {u"a.dbc"_s, QString::fromLatin1(kSha256)},
           {u"pj4-side/1/a.dbc"_s, u"ABCDEF"_s},
           {QString(), QString::fromLatin1(kSha256)},
       }}) {
    QDomElement side = doc.createElement(u"sideFile"_s);
    side.setAttribute(u"filename"_s, filename);
    side.setAttribute(u"content_sha256"_s, sha256);
    file_info.appendChild(side);
  }
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  ASSERT_EQ(refs.front().side_files.size(), 2) << "a side file with no filename is skipped";
  EXPECT_EQ(refs.front().side_files[0].filename, u"a.dbc"_s);
  EXPECT_EQ(refs.front().side_files[0].content_sha256, QLatin1StringView(kSha256));
  EXPECT_FALSE(refs.front().side_files[0].content_sha256_malformed);
  EXPECT_EQ(refs.front().side_files[1].filename, u"pj4-side/1/a.dbc"_s);
  EXPECT_TRUE(refs.front().side_files[1].content_sha256.isEmpty());
  EXPECT_TRUE(refs.front().side_files[1].content_sha256_malformed);
}

// ---------- logicalBrowserSideFiles ----------------------------------------

constexpr auto kSideSha = "1111111111111111111111111111111111111111111111111111111111111111";

std::optional<PJ::layout_xml::StagedFileInfo> stagedSideFile(const QString& path) {
  if (path == u"/pj_uploads/t/2/engine.dbc"_s) {
    return PJ::layout_xml::StagedFileInfo{.name = u"engine.dbc"_s, .content_sha256 = QString::fromLatin1(kSideSha)};
  }
  if (path == u"/pj_uploads/t/3/engine.dbc"_s) {
    return PJ::layout_xml::StagedFileInfo{.name = u"engine.dbc"_s, .content_sha256 = u"2"_s.repeated(64)};
  }
  if (path == u"/pj_uploads/t/1/run.log"_s) {
    return PJ::layout_xml::StagedFileInfo{.name = u"run.log"_s, .content_sha256 = u"3"_s.repeated(64)};
  }
  return std::nullopt;
}

TEST(LogicalBrowserSideFiles, NamesEachStagedSideFileAndEmptiesTheDataFolder) {
  // Two dictionaries of one basename, one in a fan-out child, plus a label that already reads like the name.
  const std::string config =
      R"({"filepath":"/pj_uploads/t/1/run.log","aux_base_dir":"/pj_uploads/t/1","label":"engine.dbc",)"
      R"("dictionaries":{"can0":["/pj_uploads/t/2/engine.dbc"]},"delimiter":",",)"
      R"("__pj_fanout":["{\"filepath\":\"/pj_uploads/t/1/run.log\",\"dbc\":\"/pj_uploads/t/3/engine.dbc\"}"]})";
  QString error;
  const auto result = PJ::layout_xml::logicalBrowserSideFiles(
      config, u"/pj_uploads/t/1/run.log"_s, u"run.log"_s, stagedSideFile, &error);
  ASSERT_TRUE(result.has_value()) << error.toStdString();
  ASSERT_EQ(result->side_files.size(), 2);
  EXPECT_EQ(result->side_files[0].filename, u"pj4-side/0/engine.dbc"_s) << "the plain name is taken by the label";
  EXPECT_EQ(result->side_files[0].content_sha256, QLatin1StringView(kSideSha));
  EXPECT_EQ(result->side_files[1].filename, u"pj4-side/1/engine.dbc"_s);
  EXPECT_EQ(result->config.find("/pj_uploads/t/2"), std::string::npos) << result->config;
  EXPECT_EQ(result->config.find("/pj_uploads/t/3"), std::string::npos) << result->config;

  const QJsonObject root = QJsonDocument::fromJson(QByteArray::fromStdString(result->config)).object();
  EXPECT_EQ(root.value(u"filepath"_s).toString(), u"/pj_uploads/t/1/run.log"_s)
      << "the data file is left for rewriteReplayFilepaths";
  EXPECT_EQ(root.value(u"aux_base_dir"_s).toString(), QString());
  EXPECT_EQ(root.value(u"label"_s).toString(), u"engine.dbc"_s);
  EXPECT_EQ(root.value(u"delimiter"_s).toString(), u","_s);
  EXPECT_EQ(
      root.value(u"dictionaries"_s).toObject().value(u"can0"_s).toArray().first().toString(),
      u"pj4-side/0/engine.dbc"_s);
  const QJsonObject child =
      QJsonDocument::fromJson(root.value(u"__pj_fanout"_s).toArray().first().toString().toUtf8()).object();
  EXPECT_EQ(child.value(u"dbc"_s).toString(), u"pj4-side/1/engine.dbc"_s);

  // Replay maps the saved names back to fresh copies; together with the filepath rewrite nothing ephemeral stays.
  const std::string replayed = PJ::detail::rewriteReplayFilepaths(
                                   PJ::detail::replaceConfigStringValues(
                                       result->config, {{u"pj4-side/0/engine.dbc"_s, u"/pj_uploads/u/7/engine.dbc"_s},
                                                        {u"pj4-side/1/engine.dbc"_s, u"/pj_uploads/u/8/engine.dbc"_s}})
                                       .value(),
                                   u"/pj_uploads/u/6/run.log"_s, PJ::detail::SavedFilepath::kOmit)
                                   .value();
  const QJsonObject replayed_root = QJsonDocument::fromJson(QByteArray::fromStdString(replayed)).object();
  EXPECT_EQ(
      replayed_root.value(u"dictionaries"_s).toObject().value(u"can0"_s).toArray().first().toString(),
      u"/pj_uploads/u/7/engine.dbc"_s);
  EXPECT_EQ(replayed_root.value(u"filepath"_s).toString(), u"/pj_uploads/u/6/run.log"_s);
}

TEST(LogicalBrowserSideFiles, UsesThePlainNameWhenFreeAndLeavesUnknownStringsAlone) {
  const std::string config =
      R"({"filepath":"/pj_uploads/t/1/run.log","dbc":"/pj_uploads/t/2/engine.dbc","other":"/pj_uploads/t/9/x"})";
  const auto result = PJ::layout_xml::logicalBrowserSideFiles(
      config, u"/pj_uploads/t/1/run.log"_s, u"run.log"_s, stagedSideFile, nullptr);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->side_files.size(), 1);
  EXPECT_EQ(result->side_files[0].filename, u"engine.dbc"_s);
  const QJsonObject root = QJsonDocument::fromJson(QByteArray::fromStdString(result->config)).object();
  EXPECT_EQ(root.value(u"dbc"_s).toString(), u"engine.dbc"_s);
  EXPECT_EQ(root.value(u"other"_s).toString(), u"/pj_uploads/t/9/x"_s)
      << "an unknown path stays, so the save's ephemeral-path check still refuses it";
}

// One file picked twice (candump can0 and can1 both given engine.dbc) is staged twice with equal name and bytes:
// it is one side file, so a desktop replay finds it beside the data file under one name.
TEST(LogicalBrowserSideFiles, OneFileStagedTwiceIsOneSideFile) {
  const auto staged = [](const QString& path) -> std::optional<PJ::layout_xml::StagedFileInfo> {
    if (path == u"/up/2/engine.dbc"_s || path == u"/up/3/engine.dbc"_s) {
      return PJ::layout_xml::StagedFileInfo{.name = u"engine.dbc"_s, .content_sha256 = QString::fromLatin1(kSideSha)};
    }
    return std::nullopt;
  };
  const auto result = PJ::layout_xml::logicalBrowserSideFiles(
      R"({"filepath":"/up/1/run.log","dictionaries":{"can0":["/up/2/engine.dbc"],"can1":["/up/3/engine.dbc"]}})",
      u"/up/1/run.log"_s, u"run.log"_s, staged, nullptr);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->side_files.size(), 1);
  EXPECT_EQ(result->side_files[0].filename, u"engine.dbc"_s);
  const QJsonObject dictionaries =
      QJsonDocument::fromJson(QByteArray::fromStdString(result->config)).object().value(u"dictionaries"_s).toObject();
  EXPECT_EQ(dictionaries.value(u"can0"_s).toArray().first().toString(), u"engine.dbc"_s);
  EXPECT_EQ(dictionaries.value(u"can1"_s).toArray().first().toString(), u"engine.dbc"_s);
}

// A side file never takes the data file's logical name, nor a fallback name the configuration already uses.
TEST(LogicalBrowserSideFiles, FallbackNamesSkipTheDataFileAndEveryTakenName) {
  const auto staged = [](const QString& path) -> std::optional<PJ::layout_xml::StagedFileInfo> {
    if (path == u"/up/2/run.log"_s) {
      return PJ::layout_xml::StagedFileInfo{.name = u"run.log"_s, .content_sha256 = QString::fromLatin1(kSideSha)};
    }
    return std::nullopt;
  };
  const auto result = PJ::layout_xml::logicalBrowserSideFiles(
      R"({"filepath":"/up/1/run.log","side":"/up/2/run.log","note":"pj4-side/0/run.log"})", u"/up/1/run.log"_s,
      u"run.log"_s, staged, nullptr);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->side_files.size(), 1);
  EXPECT_EQ(result->side_files[0].filename, u"pj4-side/1/run.log"_s);
  EXPECT_EQ(
      QJsonDocument::fromJson(QByteArray::fromStdString(result->config)).object().value(u"note"_s).toString(),
      u"pj4-side/0/run.log"_s);
}

TEST(LogicalBrowserSideFiles, RefusesASideFileWithoutFingerprintAndPassesNonObjectsThrough) {
  QString error;
  const auto unfingerprinted = PJ::layout_xml::logicalBrowserSideFiles(
      R"({"filepath":"/d/run.log","dbc":"/d/2/x.dbc"})", u"/d/run.log"_s, u"run.log"_s,
      [](const QString&) { return std::optional(PJ::layout_xml::StagedFileInfo{.name = u"x.dbc"_s}); }, &error);
  EXPECT_FALSE(unfingerprinted.has_value());
  EXPECT_TRUE(error.contains(u"x.dbc"_s)) << error.toStdString();

  const auto verbatim = PJ::layout_xml::logicalBrowserSideFiles("not json", {}, {}, stagedSideFile, nullptr);
  ASSERT_TRUE(verbatim.has_value());
  EXPECT_EQ(verbatim->config, "not json");
  EXPECT_TRUE(verbatim->side_files.isEmpty());
}

// A downloaded layout's sources live beside it; relative names are paths, so
// filesystem characters never turn into URL syntax.
TEST(ResolveFileSourceUrl, RelativeAndAbsoluteNetworkReferencesResolve) {
  const QUrl layout(u"https://host.test/presets/fr3/layout.pj4.xml?v=2"_s);
  const auto resolve = [&layout](const QString& filename) {
    QString error;
    const QUrl url = PJ::layout_xml::resolveFileSourceUrl(filename, layout, &error);
    EXPECT_TRUE(error.isEmpty()) << filename.toStdString() << ": " << error.toStdString();
    return url.toString(QUrl::FullyEncoded).toStdString();
  };
  EXPECT_EQ(resolve(u"data.mcap"_s), "https://host.test/presets/fr3/data.mcap");
  EXPECT_EQ(resolve(u"pj4-sources/1/duplicate.csv"_s), "https://host.test/presets/fr3/pj4-sources/1/duplicate.csv");
  EXPECT_EQ(resolve(u"../shared/run 1#2?.csv"_s), "https://host.test/presets/shared/run%201%232%3F.csv");
  EXPECT_EQ(resolve(u"https://cdn.test/data.mcap"_s), "https://cdn.test/data.mcap");
}

// A web-hosted layout never points the app at local files or other schemes.
TEST(ResolveFileSourceUrl, RejectsLocalPathsOtherSchemesAndUnanchoredLayouts) {
  const QUrl layout(u"https://host.test/presets/fr3/layout.pj4.xml"_s);
  for (const QString& filename :
       {u""_s, u"/home/user/data.mcap"_s, u"//server/share/data.mcap"_s, u"C:/data/run.mcap"_s, u"C:\\data\\run.mcap"_s,
        u"data\\run.mcap"_s, u"file:///home/user/data.mcap"_s, u"pj-upload://3f2b/1/data.csv"_s,
        u"javascript:alert(1)"_s, u"data/"_s}) {
    QString error;
    EXPECT_TRUE(PJ::layout_xml::resolveFileSourceUrl(filename, layout, &error).isEmpty()) << filename.toStdString();
    EXPECT_FALSE(error.isEmpty()) << filename.toStdString();
  }
  for (const QUrl& location : {QUrl(u"file:///data/layouts/robot.pj4.xml"_s), QUrl()}) {
    QString error;
    EXPECT_TRUE(PJ::layout_xml::resolveFileSourceUrl(u"data.mcap"_s, location, &error).isEmpty())
        << location.toString().toStdString();
    EXPECT_FALSE(error.isEmpty());
  }
}

TEST(ExtractDataSource, RelativeFilesystemPathContainingSchemeTextStillAnchors) {
  const QString relative = u"logs/http://capture.mcap"_s;
  const QDomDocument doc = buildDataSourceDoc(relative);
  const QDir layout_dir(u"/tmp/layouts"_s);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, layout_dir);
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().serialized_path, relative);
  EXPECT_EQ(refs.front().resolved_path, layout_dir.absoluteFilePath(relative));
}

TEST(ExtractDataSource, PluginIdAndCdataJsonRoundTrip) {
  const QString json = uR"({"topics":["a","b"]})"_s;
  const QDomDocument doc = buildDataSourceDoc(u"/tmp/x.mcap"_s, u"robot"_s, u"DataLoad MCAP"_s, json);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().prefix, u"robot"_s);
  EXPECT_EQ(refs.front().plugin_id, u"DataLoad MCAP"_s);
  EXPECT_EQ(refs.front().plugin_config_json, json);
}

TEST(ExtractDataSource, SourceFilepathModeResolvesAgainstLayoutDir) {
  const QString json = uR"({"filepath":"run.csv"})"_s;
  QDomDocument doc = buildDataSourceDoc(u"run.csv"_s, QString(), u"CSV Loader"_s, json);
  QDomElement plugin = doc.documentElement()
                           .firstChildElement(u"previouslyLoaded_Datafiles"_s)
                           .firstChildElement(u"fileInfo"_s)
                           .firstChildElement(u"plugin"_s);
  plugin.setAttribute(u"filepath_mode"_s, u"source"_s);

  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QDir layout_dir(QDir::tempPath() + u"/layouts"_s);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, layout_dir);
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().resolved_path, layout_dir.absoluteFilePath(u"run.csv"_s));
  EXPECT_EQ(refs.front().plugin_config_json, json);
}

TEST(ExtractDataSource, PluginCdataWithClosingSequenceRoundTrips) {
  const QString json = uR"({"pat":"weird ]]> in middle"})"_s;
  const QDomDocument doc = buildDataSourceDoc(u"/tmp/x.mcap"_s, QString(), u"CSV"_s, json);
  // Round-trip the whole doc through serialize+reparse so the CDATA splitting goes through the file pipeline.
  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().plugin_config_json, json);
}

TEST(ExtractDataSource, PluginManifestIdParsedWhenPresentAndEmptyWhenAbsent) {
  // Layouts write both the display name (ID, what old readers keep using) and the stable manifest id (manifest_id)
  // on <plugin>; an old layout carries only ID and the manifest id must then stay empty.
  QDomDocument doc = buildDataSourceDoc(u"/tmp/x.mcap"_s, QString(), u"MCAP Loader"_s, u"{}"_s);
  {
    const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
    ASSERT_EQ(refs.size(), 1);
    EXPECT_EQ(refs.front().plugin_id, u"MCAP Loader"_s);
    EXPECT_TRUE(refs.front().plugin_manifest_id.isEmpty());
  }
  QDomElement plugin = doc.documentElement()
                           .firstChildElement(u"previouslyLoaded_Datafiles"_s)
                           .firstChildElement(u"fileInfo"_s)
                           .firstChildElement(u"plugin"_s);
  plugin.setAttribute(u"manifest_id"_s, u"mcap-loader"_s);
  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().plugin_id, u"MCAP Loader"_s);
  EXPECT_EQ(refs.front().plugin_manifest_id, u"mcap-loader"_s);
}

// ---------- <materialize> provider source records ---------------------------

// Appends a <materialize> child to the doc's only <fileInfo>, mirroring MainWindow::appendDataSourceElement's save
// shape: provider/identity attributes, the descriptor JSON as CDATA through appendJsonAsCdata (so a "]]>"-bearing
// descriptor splits across sections).
void appendMaterialize(QDomDocument& doc, const QString& provider, const QString& identity, const QString& descriptor) {
  QDomElement file_info =
      doc.documentElement().firstChildElement(u"previouslyLoaded_Datafiles"_s).firstChildElement(u"fileInfo"_s);
  QDomElement materialize = doc.createElement(u"materialize"_s);
  materialize.setAttribute(u"provider"_s, provider);
  materialize.setAttribute(u"identity"_s, identity);
  PJ::appendJsonAsCdata(doc, materialize, descriptor);
  file_info.appendChild(materialize);
}

TEST(ExtractDataSource, MaterializeRecordSurvivesSerializeReparseByteExact) {
  // The descriptor bytes are a cross-repo identity contract: they must come back verbatim through the XML file
  // pipeline, including newlines and the CDATA-hostile "]]>" sequence.
  const QString descriptor = u"{\n  \"provider\": \"mcap-cloud\",\n  \"pattern\": \"a ]]> b\"\n}"_s;
  const QString plugin_json = uR"({"topics":[]})"_s;
  QDomDocument doc = buildDataSourceDoc(u"/tmp/cache.mcap"_s, QString(), u"MCAP Loader"_s, plugin_json);
  appendMaterialize(doc, u"mcap-cloud"_s, u"mcap-cloud:v1:sha256/128:ab12"_s, descriptor);

  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().materialize_provider, u"mcap-cloud"_s);
  EXPECT_EQ(refs.front().materialize_identity, u"mcap-cloud:v1:sha256/128:ab12"_s);
  EXPECT_EQ(refs.front().materialize_descriptor_json, descriptor);
  // <materialize> is a sibling of <plugin>: the plugin parses exactly as without it, no descriptor bytes in its config.
  EXPECT_EQ(refs.front().plugin_id, u"MCAP Loader"_s);
  EXPECT_EQ(refs.front().plugin_config_json, plugin_json);
}

TEST(ExtractDataSource, MaterializeAbsentLeavesFieldsEmpty) {
  // Old layouts (no <materialize> child) parse identically: all three fields stay empty, every other field is
  // untouched.
  const QString json = uR"({"topics":["a"]})"_s;
  const QDomDocument doc = buildDataSourceDoc(u"/tmp/x.mcap"_s, u"robot"_s, u"CSV"_s, json);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().materialize_provider.isEmpty());
  EXPECT_TRUE(refs.front().materialize_identity.isEmpty());
  EXPECT_TRUE(refs.front().materialize_descriptor_json.isEmpty());
  EXPECT_EQ(refs.front().plugin_id, u"CSV"_s);
  EXPECT_EQ(refs.front().plugin_config_json, json);
}

TEST(ExtractDataSource, UnknownFileInfoChildrenStayIgnoredAndParsingIsReadOnly) {
  // An unknown <fileInfo> child changes nothing (the old-reader guarantee <materialize> relies on), and extracting
  // never mutates the document (byte-stable).
  QDomDocument doc = buildDataSourceDoc(u"/tmp/x.mcap"_s, QString(), u"MCAP Loader"_s, uR"({"a":1})"_s);
  appendMaterialize(doc, u"mcap-cloud"_s, u"id-1"_s, uR"({"key":"cloud/a.mcap"})"_s);
  QDomElement file_info =
      doc.documentElement().firstChildElement(u"previouslyLoaded_Datafiles"_s).firstChildElement(u"fileInfo"_s);
  QDomElement unknown = doc.createElement(u"future_extension"_s);
  unknown.setAttribute(u"x"_s, u"1"_s);
  file_info.appendChild(unknown);

  const QByteArray before = doc.toByteArray(2);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  EXPECT_EQ(doc.toByteArray(2), before);
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().plugin_config_json, uR"({"a":1})"_s);
  EXPECT_EQ(refs.front().materialize_provider, u"mcap-cloud"_s);
  EXPECT_EQ(refs.front().materialize_identity, u"id-1"_s);
  EXPECT_EQ(refs.front().materialize_descriptor_json, uR"({"key":"cloud/a.mcap"})"_s);
}

TEST(ExtractDataSource, MultipleFileInfosParsedInOrder) {
  // Host-absolute paths (QDir::tempPath()): a hardcoded POSIX "/tmp/x" is drive-relative on Windows, so
  // extractDataSource would re-anchor it and the equality check would fail.
  const QString abs_a = QDir::tempPath() + u"/a.mcap"_s;
  const QString abs_b = QDir::tempPath() + u"/b.mcap"_s;
  const QString json_a = uR"({"topics":["/a"]})"_s;
  const QString json_b = uR"({"topics":["/b"]})"_s;
  QDomDocument doc = buildDataSourceDoc(abs_a, QString(), u"DataLoad MCAP"_s, json_a);
  appendFileInfo(doc, abs_b, u"robot"_s, u"DataLoad MCAP"_s, json_b);

  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 2);
  EXPECT_EQ(refs[0].resolved_path, QFileInfo(abs_a).absoluteFilePath());
  EXPECT_EQ(refs[0].plugin_config_json, json_a);
  EXPECT_EQ(refs[1].resolved_path, QFileInfo(abs_b).absoluteFilePath());
  EXPECT_EQ(refs[1].prefix, u"robot"_s);
  EXPECT_EQ(refs[1].plugin_config_json, json_b);
}

TEST(ExtractDataSource, MultiFileSurvivesSerializeReparse) {
  // Two fileInfos serialized and reparsed come back in order. Host-absolute paths, as above.
  const QString abs_a = QDir::tempPath() + u"/a.mcap"_s;
  const QString abs_b = QDir::tempPath() + u"/b.mcap"_s;
  QDomDocument doc = buildDataSourceDoc(abs_a);
  appendFileInfo(doc, abs_b);
  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, QDir::current());
  ASSERT_EQ(refs.size(), 2);
  EXPECT_EQ(refs[0].resolved_path, QFileInfo(abs_a).absoluteFilePath());
  EXPECT_EQ(refs[1].resolved_path, QFileInfo(abs_b).absoluteFilePath());
}

// ---------- Source Timeline state --------------------------------------

// Stamps the per-source timeline attributes onto a doc's only <fileInfo>, as
// MainWindow::appendDataSourceElement does at save time.
void setTimelineState(QDomDocument& doc, qint64 offset_ns, int order) {
  QDomElement file_info =
      doc.documentElement().firstChildElement(u"previouslyLoaded_Datafiles"_s).firstChildElement(u"fileInfo"_s);
  file_info.setAttribute(u"display_offset_ns"_s, QString::number(offset_ns));
  file_info.setAttribute(u"timeline_order"_s, QString::number(order));
}

TEST(ExtractDataSource, TimelineStateAttributesParsed) {
  QDomDocument doc = buildDataSourceDoc(u"/tmp/run.mcap"_s);
  setTimelineState(doc, /*offset_ns=*/-1'500'000'000LL, /*order=*/2);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().has_display_offset);
  EXPECT_EQ(refs.front().display_offset_ns, -1'500'000'000LL);
  // No pj4_version attribute -> legacy (<4) read: the offset still has the global
  // reference baked in, so the apply path must subtract it.
  EXPECT_TRUE(refs.front().display_offset_includes_global_reference);
  EXPECT_EQ(refs.front().timeline_order, 2);
}

TEST(ExtractDataSource, SchemaV4TimelineOffsetIsSourceOnly) {
  QDomDocument doc = buildDataSourceDoc(u"/tmp/run.mcap"_s);
  doc.documentElement().setAttribute(u"pj4_version"_s, u"4"_s);
  setTimelineState(doc, /*offset_ns=*/42'000LL, /*order=*/0);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().has_display_offset);
  EXPECT_EQ(refs.front().display_offset_ns, 42'000LL);
  // v4 persists sourceDisplayOffset() only -> no read-side subtraction.
  EXPECT_FALSE(refs.front().display_offset_includes_global_reference);
}

TEST(ExtractDataSource, TimelineStateAbsentLeavesDefaults) {
  // A layout without timeline attributes: the dataset keeps its natural zero offset (has_display_offset=false, so the
  // caller skips the write) and falls back to load order (timeline_order=-1).
  const QDomDocument doc = buildDataSourceDoc(u"/tmp/run.mcap"_s);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_FALSE(refs.front().has_display_offset);
  EXPECT_FALSE(refs.front().display_offset_includes_global_reference);
  EXPECT_EQ(refs.front().display_offset_ns, 0);
  EXPECT_EQ(refs.front().timeline_order, -1);
  EXPECT_TRUE(refs.front().datasets.isEmpty());
}

TEST(ExtractDataSource, TimelineStateSurvivesSerializeReparse) {
  QDomDocument doc = buildDataSourceDoc(u"/tmp/run.mcap"_s);
  setTimelineState(doc, /*offset_ns=*/42'000LL, /*order=*/0);
  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().has_display_offset);
  EXPECT_EQ(refs.front().display_offset_ns, 42'000LL);
  EXPECT_EQ(refs.front().timeline_order, 0);
}

// A zero offset is meaningful (a dataset deliberately at its natural position),
// so it must be written-and-parsed as present, not conflated with "absent".
TEST(ExtractDataSource, TimelineStateZeroOffsetIsStillPresent) {
  QDomDocument doc = buildDataSourceDoc(u"/tmp/run.mcap"_s);
  setTimelineState(doc, /*offset_ns=*/0, /*order=*/1);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().has_display_offset);
  EXPECT_EQ(refs.front().display_offset_ns, 0);
  EXPECT_EQ(refs.front().timeline_order, 1);
}

// Schema v4: one file that fans out into several datasets writes one <dataset>
// child per member. Each carries its own (source_name, source_index) key plus
// per-source offset/order, so identical display names can't swap tracks.
TEST(ExtractDataSource, FanoutDatasetTimelineStatesRoundTripUnderOneFileReplay) {
  QDomDocument doc = buildDataSourceDoc(u"/tmp/fanout.mcap"_s, QString(), u"MCAP"_s, uR"({"fanout":true})"_s);
  doc.documentElement().setAttribute(u"pj4_version"_s, u"4"_s);
  QDomElement file_info =
      doc.documentElement().firstChildElement(u"previouslyLoaded_Datafiles"_s).firstChildElement(u"fileInfo"_s);
  for (int index = 0; index < 2; ++index) {
    QDomElement dataset = doc.createElement(u"dataset"_s);
    dataset.setAttribute(u"source_name"_s, index == 0 ? u"run/left"_s : u"run/right"_s);
    dataset.setAttribute(u"source_index"_s, QString::number(index));
    dataset.setAttribute(u"display_offset_ns"_s, QString::number((index + 1) * 1'000));
    dataset.setAttribute(u"timeline_order"_s, QString::number(1 - index));
    file_info.appendChild(dataset);
  }

  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, QDir::current());
  ASSERT_EQ(refs.size(), 1) << "one file is replayed once even when it produces two datasets";
  ASSERT_EQ(refs.front().datasets.size(), 2);
  EXPECT_EQ(refs.front().datasets[0].source_name, u"run/left"_s);
  EXPECT_EQ(refs.front().datasets[0].source_index, 0);
  EXPECT_EQ(refs.front().datasets[0].display_offset_ns, 1'000);
  EXPECT_EQ(refs.front().datasets[0].timeline_order, 1);
  EXPECT_FALSE(refs.front().datasets[0].display_offset_includes_global_reference);
  EXPECT_EQ(refs.front().datasets[1].source_name, u"run/right"_s);
  EXPECT_EQ(refs.front().datasets[1].source_index, 1);
  EXPECT_EQ(refs.front().datasets[1].display_offset_ns, 2'000);
  EXPECT_EQ(refs.front().datasets[1].timeline_order, 0);
  EXPECT_EQ(refs.front().plugin_id, u"MCAP"_s);
  EXPECT_EQ(refs.front().plugin_config_json, uR"({"fanout":true})"_s);
}

// ---------- isSamePath ------------------------------------------------------
// isSamePath() decides whether the source a layout references is the one already loaded.

TEST(IsSamePath, EmptyInputsAreNotSame) {
  EXPECT_FALSE(PJ::layout_xml::isSamePath(QString(), QString()));
  EXPECT_FALSE(PJ::layout_xml::isSamePath(u"/tmp/x"_s, QString()));
  EXPECT_FALSE(PJ::layout_xml::isSamePath(QString(), u"/tmp/x"_s));
}

TEST(IsSamePath, DifferentExistingFilesAreNotSame) {
  // Two real files with different basenames.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString a = dir.filePath(u"sagod.mcap"_s);
  const QString b = dir.filePath(u"zeg.mcap"_s);
  ASSERT_TRUE(QFile(a).open(QIODevice::WriteOnly));
  ASSERT_TRUE(QFile(b).open(QIODevice::WriteOnly));
  EXPECT_FALSE(PJ::layout_xml::isSamePath(a, b));
}

TEST(IsSamePath, IdenticalAbsolutePathsAreSame) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString p = dir.filePath(u"log.mcap"_s);
  ASSERT_TRUE(QFile(p).open(QIODevice::WriteOnly));
  EXPECT_TRUE(PJ::layout_xml::isSamePath(p, p));
}

TEST(IsSamePath, RelativeAndAbsoluteFormsOfSameFileAreSame) {
  // The data-source XML may store a relative path; the SessionManager
  // may carry the absolute form. Canonicalization has to collapse them.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString abs = dir.filePath(u"log.mcap"_s);
  ASSERT_TRUE(QFile(abs).open(QIODevice::WriteOnly));

  const QString cwd_before = QDir::currentPath();
  ASSERT_TRUE(QDir::setCurrent(dir.path()));
  EXPECT_TRUE(PJ::layout_xml::isSamePath(abs, u"log.mcap"_s));
  EXPECT_TRUE(QDir::setCurrent(cwd_before));
}

TEST(IsSamePath, NonexistentPathsAreNotSame) {
  // canonicalFilePath() is empty for missing files; those must not compare as the same.
  EXPECT_FALSE(PJ::layout_xml::isSamePath(u"/nonexistent/a.mcap"_s, u"/nonexistent/b.mcap"_s));
  EXPECT_FALSE(PJ::layout_xml::isSamePath(u"/nonexistent/a.mcap"_s, u"/nonexistent/a.mcap"_s));
}

TEST(IsSamePath, OpaqueUploadIdentitiesCompareLiterally) {
  const QString identity = u"pj-upload://session/1/log.csv"_s;
  EXPECT_TRUE(PJ::layout_xml::isSamePath(identity, identity));
  EXPECT_FALSE(PJ::layout_xml::isSamePath(identity, u"pj-upload://session/2/log.csv"_s));
  EXPECT_FALSE(PJ::layout_xml::isSamePath(identity, u"/tmp/log.csv"_s));
}

TEST(IsSamePath, FilesystemPathContainingSchemeTextStillCanonicalizes) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
#ifdef Q_OS_WIN
  // ':' cannot occur in a Windows path component. A doubled separator after the drive prefix is nevertheless a legal
  // spelling of the same file and still contains `://`, which a broad URI check would misclassify.
  const QString absolute = QDir::fromNativeSeparators(dir.filePath(u"capture.mcap"_s));
  ASSERT_TRUE(QFile(absolute).open(QIODevice::WriteOnly));
  ASSERT_GE(absolute.size(), 3);
  ASSERT_EQ(absolute.at(1), u':');
  ASSERT_EQ(absolute.at(2), u'/');
  QString scheme_like = absolute;
  scheme_like.insert(2, u'/');
  ASSERT_TRUE(scheme_like.contains(u"://"_s));
  EXPECT_TRUE(PJ::layout_xml::isSamePath(absolute, scheme_like));
#else
  ASSERT_TRUE(QDir().mkpath(dir.filePath(u"logs/http:"_s)));
  const QString absolute = dir.filePath(u"logs/http:/capture.mcap"_s);
  ASSERT_TRUE(QFile(absolute).open(QIODevice::WriteOnly));

  const QString previous = QDir::currentPath();
  ASSERT_TRUE(QDir::setCurrent(dir.path()));
  EXPECT_TRUE(PJ::layout_xml::isSamePath(absolute, u"logs/http://capture.mcap"_s));
  EXPECT_TRUE(QDir::setCurrent(previous));
#endif
}

TEST(EnsureLayoutExtension, AppendsWhenNoExtension) {
  EXPECT_EQ(PJ::layout_xml::ensureLayoutExtension(u"my_layout"_s), u"my_layout.pj4.xml"_s);
  EXPECT_EQ(PJ::layout_xml::ensureLayoutExtension(u"/home/user/setup"_s), u"/home/user/setup.pj4.xml"_s);
}

TEST(EnsureLayoutExtension, LeavesCorrectExtensionUntouched) {
  EXPECT_EQ(PJ::layout_xml::ensureLayoutExtension(u"my_layout.pj4.xml"_s), u"my_layout.pj4.xml"_s);
}

TEST(EnsureLayoutExtension, RespectsAnyUserSpecifiedExtension) {
  // Append only when no extension is given: a user-chosen suffix is left alone, not turned into notes.txt.pj4.xml.
  EXPECT_EQ(PJ::layout_xml::ensureLayoutExtension(u"notes.txt"_s), u"notes.txt"_s);
  EXPECT_EQ(PJ::layout_xml::ensureLayoutExtension(u"my.layout"_s), u"my.layout"_s);
}

TEST(EnsureLayoutExtension, EmptyInEmptyOut) {
  EXPECT_EQ(PJ::layout_xml::ensureLayoutExtension(QString()), QString());
}

// ---------- SeriesPath / extractSeriesPaths / rebindCurveKeys ---------------

using PJ::layout_xml::SeriesPath;

// A doc with one <root><plot>; callers append <curve> elements to the plot.
struct PlotDoc {
  QDomDocument doc;
  QDomElement plot;
};
PlotDoc makePlotDoc() {
  PlotDoc pd;
  QDomElement root = pd.doc.createElement(u"root"_s);
  pd.doc.appendChild(root);
  pd.plot = pd.doc.createElement(u"plot"_s);
  root.appendChild(pd.plot);
  return pd;
}
QDomElement addTsCurve(PlotDoc& pd, const QString& topic, const QString& field) {
  QDomElement c = pd.doc.createElement(u"curve"_s);
  c.setAttribute(u"topic"_s, topic);
  c.setAttribute(u"field"_s, field);
  pd.plot.appendChild(c);
  return c;
}
QDomElement addXyCurve(PlotDoc& pd, const SeriesPath& x, const SeriesPath& y) {
  QDomElement c = pd.doc.createElement(u"curve"_s);
  c.setAttribute(u"x_topic"_s, x.topic);
  c.setAttribute(u"x_field"_s, x.field);
  c.setAttribute(u"y_topic"_s, y.topic);
  c.setAttribute(u"y_field"_s, y.field);
  pd.plot.appendChild(c);
  return c;
}

TEST(SeriesPathDisplay, JoinsTopicAndField) {
  EXPECT_EQ((SeriesPath{u"/imu"_s, u"accel.x"_s}).display(), u"/imu/accel.x"_s);
  EXPECT_EQ((SeriesPath{QString(), u"lonely"_s}).display(), u"lonely"_s);
}

TEST(ExtractSeriesPaths, CollectsTimeSeriesTopicField) {
  PlotDoc pd = makePlotDoc();
  addTsCurve(pd, u"/imu"_s, u"accel.x"_s);
  addTsCurve(pd, u"/imu"_s, u"accel.y"_s);
  const QList<SeriesPath> paths = PJ::layout_xml::extractSeriesPaths(pd.doc);
  ASSERT_EQ(paths.size(), 2);
  EXPECT_EQ(paths[0], (SeriesPath{u"/imu"_s, u"accel.x"_s}));
  EXPECT_EQ(paths[1], (SeriesPath{u"/imu"_s, u"accel.y"_s}));
}

TEST(ExtractSeriesPaths, CollectsXyAxesAndDeduplicates) {
  PlotDoc pd = makePlotDoc();
  const SeriesPath x{u"/t"_s, u"a"_s};
  const SeriesPath y{u"/t"_s, u"b"_s};
  addXyCurve(pd, x, y);
  addTsCurve(pd, u"/t"_s, u"a"_s);  // duplicate of x
  const QList<SeriesPath> paths = PJ::layout_xml::extractSeriesPaths(pd.doc);
  ASSERT_EQ(paths.size(), 2);
  EXPECT_EQ(paths[0], x);
  EXPECT_EQ(paths[1], y);
}

TEST(ExtractSeriesPaths, SkipsCurvesWithoutStableIdentity) {
  PlotDoc pd = makePlotDoc();
  QDomElement legacy = pd.doc.createElement(u"curve"_s);
  legacy.setAttribute(u"name"_s, u"dataset:1/topic:2/column:0"_s);
  pd.plot.appendChild(legacy);
  EXPECT_TRUE(PJ::layout_xml::extractSeriesPaths(pd.doc).isEmpty());
}

TEST(ExtractSeriesPaths, PreservesDatasetQualifiers) {
  PlotDoc pd = makePlotDoc();
  QDomElement curve = addTsCurve(pd, u"/imu"_s, u"x"_s);
  curve.setAttribute(u"dataset_id"_s, u"4"_s);
  curve.setAttribute(u"dataset_source"_s, u"run.mcap"_s);
  curve.setAttribute(u"dataset_path"_s, u"data/run.mcap"_s);
  const QList<SeriesPath> paths = PJ::layout_xml::extractSeriesPaths(pd.doc);
  ASSERT_EQ(paths.size(), 1);
  EXPECT_EQ(paths.front().dataset_id, 4U);
  EXPECT_EQ(paths.front().dataset_source, u"run.mcap"_s);
  EXPECT_EQ(paths.front().dataset_path, u"data/run.mcap"_s);
}

TEST(ExtractSeriesPaths, DedupKeepsSameTopicFieldFromDifferentDatasets) {
  PlotDoc pd = makePlotDoc();
  QDomElement a = addTsCurve(pd, u"/speed"_s, u"value"_s);
  a.setAttribute(u"dataset_id"_s, u"1"_s);
  QDomElement b = addTsCurve(pd, u"/speed"_s, u"value"_s);
  b.setAttribute(u"dataset_id"_s, u"2"_s);
  // Same (topic, field) with distinct dataset qualifiers must stay two entries.
  EXPECT_EQ(PJ::layout_xml::extractSeriesPaths(pd.doc).size(), 2);
}

// ---------- stampDatasetSourcePaths / removeUnvalidatedDatasetIds /
//            resolveDatasetSourcePaths -----------------------------------------

TEST(DatasetSourcePath, StampResolveAndPortableIdGuardCoverEveryIdentityShape) {
  QTemporaryDir layout_dir;
  ASSERT_TRUE(layout_dir.isValid());
  // Curves live inside a <plot>; processor inputs carry the same portable dataset
  // identity, while scene elements remain owned by their widget families.
  PlotDoc pd = makePlotDoc();
  QDomElement& doc_root = pd.plot;
  QDomElement ts = addTsCurve(pd, u"/imu"_s, u"x"_s);
  ts.setAttribute(u"dataset_id"_s, u"7"_s);
  QDomElement xy = addXyCurve(pd, SeriesPath{u"/xy"_s, u"x"_s}, SeriesPath{u"/xy"_s, u"y"_s});
  xy.setAttribute(u"x_dataset_id"_s, u"7"_s);
  xy.setAttribute(u"y_dataset_id"_s, u"8"_s);

  QDomElement processors = pd.doc.createElement(u"data_processors"_s);
  pd.doc.documentElement().appendChild(processors);
  QDomElement processor = pd.doc.createElement(u"processor"_s);
  processor.setAttribute(u"input_dataset_id"_s, u"7"_s);
  processors.appendChild(processor);
  QDomElement transform = pd.doc.createElement(u"transform"_s);
  QDomElement transform_input = pd.doc.createElement(u"input"_s);
  transform_input.setAttribute(u"dataset_id"_s, u"7"_s);
  transform.appendChild(transform_input);
  processors.appendChild(transform);
  // A dataset-bound marker generator saved with an absolute path (as DerivedObjects::save
  // writes it), an unplaceable one, and an all_datasets one that names no dataset.
  QDomElement generator = pd.doc.createElement(u"generator"_s);
  generator.setAttribute(u"dataset_id"_s, u"7"_s);
  generator.setAttribute(u"dataset_path"_s, u"/saved/elsewhere/run.mcap"_s);
  processors.appendChild(generator);
  QDomElement unplaced_generator = pd.doc.createElement(u"generator"_s);
  unplaced_generator.setAttribute(u"dataset_id"_s, u"8"_s);
  unplaced_generator.setAttribute(u"dataset_source"_s, u"stream"_s);
  unplaced_generator.setAttribute(u"dataset_path"_s, QString());
  processors.appendChild(unplaced_generator);
  QDomElement global_generator = pd.doc.createElement(u"generator"_s);
  global_generator.setAttribute(u"all_datasets"_s, u"1"_s);
  processors.appendChild(global_generator);

  // Scene elements must be invisible to all three passes.
  QDomElement scene = pd.doc.createElement(u"scene3d"_s);
  pd.doc.documentElement().appendChild(scene);
  QDomElement layer = pd.doc.createElement(u"layer"_s);
  layer.setAttribute(u"dataset_id"_s, u"7"_s);
  layer.setAttribute(u"dataset_source"_s, u"run.mcap"_s);
  scene.appendChild(layer);
  QDomElement robot = pd.doc.createElement(u"robot_model"_s);
  robot.setAttribute(u"source_dataset_id"_s, u"7"_s);
  scene.appendChild(robot);
  QDomElement config = pd.doc.createElement(u"config_topic"_s);
  config.setAttribute(u"dataset_id"_s, u"9"_s);
  config.setAttribute(u"dataset_source"_s, u"duplicate.mcap"_s);
  scene.appendChild(config);
  QDomElement local = pd.doc.createElement(u"layer"_s);
  local.setAttribute(u"dataset_id"_s, u"0"_s);
  scene.appendChild(local);
  (void)doc_root;

  PJ::layout_xml::stampDatasetSourcePaths(
      pd.doc, [](std::uint32_t id) { return id == 7 ? u"data/run.mcap"_s : QString{}; });
  EXPECT_EQ(ts.attribute(u"dataset_path"_s), u"data/run.mcap"_s);
  EXPECT_EQ(xy.attribute(u"x_dataset_path"_s), u"data/run.mcap"_s);
  EXPECT_FALSE(xy.hasAttribute(u"y_dataset_path"_s));  // id 8 unknown to the lookup
  EXPECT_EQ(processor.attribute(u"input_dataset_path"_s), u"data/run.mcap"_s);
  EXPECT_EQ(transform_input.attribute(u"dataset_path"_s), u"data/run.mcap"_s);
  EXPECT_EQ(generator.attribute(u"dataset_path"_s), u"data/run.mcap"_s) << "the absolute save-time path is replaced";
  EXPECT_TRUE(unplaced_generator.attribute(u"dataset_path"_s).isEmpty());
  EXPECT_FALSE(global_generator.hasAttribute(u"dataset_path"_s));
  EXPECT_FALSE(layer.hasAttribute(u"dataset_path"_s)) << "scene layers are not stamped";
  EXPECT_FALSE(robot.hasAttribute(u"source_dataset_path"_s)) << "robot models are not stamped";

  PJ::layout_xml::removeUnvalidatedDatasetIds(pd.doc);
  EXPECT_TRUE(ts.hasAttribute(u"dataset_id"_s));  // path-qualified -> kept
  EXPECT_TRUE(xy.hasAttribute(u"x_dataset_id"_s));
  EXPECT_FALSE(xy.hasAttribute(u"y_dataset_id"_s))  // id-only, no path -> stripped
      << "an unvalidated volatile id must not survive a file save";
  EXPECT_TRUE(processor.hasAttribute(u"input_dataset_id"_s));
  EXPECT_TRUE(transform_input.hasAttribute(u"dataset_id"_s));
  EXPECT_TRUE(generator.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(unplaced_generator.hasAttribute(u"dataset_id"_s)) << "a path-less generator id is a volatile hint";
  EXPECT_EQ(unplaced_generator.attribute(u"dataset_source"_s), u"stream"_s);
  // Scene ids survive untouched — restore requires them and the scene family owns
  // its own qualifier round-trip.
  EXPECT_TRUE(layer.hasAttribute(u"dataset_id"_s)) << "scene layer id must not be stripped";
  EXPECT_TRUE(robot.hasAttribute(u"source_dataset_id"_s));
  EXPECT_TRUE(config.hasAttribute(u"dataset_id"_s)) << "scene config-topic id must survive the file-save strip";
  EXPECT_EQ(config.attribute(u"dataset_source"_s), u"duplicate.mcap"_s);
  EXPECT_EQ(local.attribute(u"dataset_id"_s), u"0"_s)
      << "zero is a local-scene sentinel, not a remintable dataset identity";

  PJ::layout_xml::resolveDatasetSourcePaths(pd.doc, QDir(layout_dir.path()));
  const QString expected = QDir::cleanPath(layout_dir.filePath(u"data/run.mcap"_s));
  EXPECT_EQ(ts.attribute(u"dataset_path"_s), expected);
  EXPECT_EQ(xy.attribute(u"x_dataset_path"_s), expected);
  EXPECT_EQ(processor.attribute(u"input_dataset_path"_s), expected);
  EXPECT_EQ(transform_input.attribute(u"dataset_path"_s), expected);
  EXPECT_EQ(generator.attribute(u"dataset_path"_s), expected);
}

// An <on_demand> recipe's inputs carry their dataset like a transform's: a known id gets its path at file save, an
// unvalidated volatile id is stripped, and a generic layout keeps no qualifier.
TEST(DatasetSourcePath, OnDemandInputsAreStampedAndStripped) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  QDomElement processors = doc.createElement(u"data_processors"_s);
  root.appendChild(processors);
  QDomElement on_demand = doc.createElement(u"on_demand"_s);
  processors.appendChild(on_demand);
  QDomElement known = doc.createElement(u"input"_s);
  known.setAttribute(u"name"_s, u"cloud"_s);
  known.setAttribute(u"dataset_id"_s, u"7"_s);
  known.setAttribute(u"dataset_source"_s, u"run.mcap"_s);
  on_demand.appendChild(known);
  QDomElement unknown = doc.createElement(u"input"_s);
  unknown.setAttribute(u"name"_s, u"threshold"_s);
  unknown.setAttribute(u"dataset_id"_s, u"8"_s);
  on_demand.appendChild(unknown);

  PJ::layout_xml::stampDatasetSourcePaths(
      doc, [](std::uint32_t id) { return id == 7 ? u"data/run.mcap"_s : QString{}; });
  EXPECT_EQ(known.attribute(u"dataset_path"_s), u"data/run.mcap"_s);
  EXPECT_FALSE(unknown.hasAttribute(u"dataset_path"_s)) << "id 8 unknown to the lookup";

  PJ::layout_xml::removeUnvalidatedDatasetIds(doc);
  EXPECT_TRUE(known.hasAttribute(u"dataset_id"_s)) << "path-qualified: kept";
  EXPECT_FALSE(unknown.hasAttribute(u"dataset_id"_s)) << "an unvalidated volatile id must not survive a file save";

  PJ::layout_xml::removeDatasetQualifiersForGenericLayout(doc);
  for (const QDomElement& input : {known, unknown}) {
    EXPECT_FALSE(input.hasAttribute(u"dataset_id"_s));
    EXPECT_FALSE(input.hasAttribute(u"dataset_source"_s));
    EXPECT_FALSE(input.hasAttribute(u"dataset_path"_s));
    EXPECT_FALSE(input.attribute(u"name"_s).isEmpty()) << "the name stays";
  }
}

// A Custom Widget wire carries the same dataset triple and goes through the same three passes: the
// absolute path the panel saves is replaced by the layout-relative one at file save, an unvalidated
// volatile id is stripped, and a generic layout keeps no qualifier. A marker rule's <input> outside a
// <CustomWidget> is not a wire: the stamp and the id pass leave it alone.
TEST(DatasetSourcePath, CustomWidgetInputsAreStampedAndStripped) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  QDomElement custom = doc.createElement(u"CustomWidget"_s);
  root.appendChild(custom);
  QDomElement known = doc.createElement(u"input"_s);
  known.setAttribute(u"name"_s, u"value"_s);
  known.setAttribute(u"dataset_id"_s, u"7"_s);
  known.setAttribute(u"dataset_source"_s, u"run.mcap"_s);
  known.setAttribute(u"dataset_path"_s, u"/saved/elsewhere/run.mcap"_s);
  custom.appendChild(known);
  QDomElement unknown = doc.createElement(u"input"_s);
  unknown.setAttribute(u"name"_s, u"state"_s);
  unknown.setAttribute(u"dataset_id"_s, u"8"_s);
  unknown.setAttribute(u"dataset_source"_s, u"stream"_s);
  custom.appendChild(unknown);
  QDomElement marker_input = doc.createElement(u"input"_s);
  marker_input.setAttribute(u"dataset_id"_s, u"7"_s);
  root.appendChild(marker_input);

  PJ::layout_xml::stampDatasetSourcePaths(
      doc, [](std::uint32_t id) { return id == 7 ? u"data/run.mcap"_s : QString{}; });
  EXPECT_EQ(known.attribute(u"dataset_path"_s), u"data/run.mcap"_s) << "the absolute save-time path is replaced";
  EXPECT_FALSE(unknown.hasAttribute(u"dataset_path"_s)) << "id 8 unknown to the lookup";
  EXPECT_FALSE(marker_input.hasAttribute(u"dataset_path"_s)) << "not a Custom Widget wire";

  PJ::layout_xml::removeUnvalidatedDatasetIds(doc);
  EXPECT_TRUE(known.hasAttribute(u"dataset_id"_s)) << "path-qualified: kept";
  EXPECT_FALSE(unknown.hasAttribute(u"dataset_id"_s)) << "an unvalidated volatile id must not survive a file save";
  EXPECT_EQ(unknown.attribute(u"dataset_source"_s), u"stream"_s);
  EXPECT_TRUE(marker_input.hasAttribute(u"dataset_id"_s));

  PJ::layout_xml::removeDatasetQualifiersForGenericLayout(doc);
  for (const QDomElement& input : {known, unknown}) {
    EXPECT_FALSE(input.hasAttribute(u"dataset_id"_s));
    EXPECT_FALSE(input.hasAttribute(u"dataset_source"_s));
    EXPECT_FALSE(input.hasAttribute(u"dataset_path"_s));
    EXPECT_FALSE(input.attribute(u"name"_s).isEmpty()) << "the name stays";
  }
}

// A hidden <marker_scope> carries the same dataset identity as the curves beside it
// and must survive the same stamp / portable-id / generic-strip passes, or a reload
// with exchanged dataset ids binds the hidden scope to the wrong dataset.
TEST(DatasetSourcePath, MarkerScopeChildIsStampedAndStrippedLikeItsCurves) {
  PlotDoc pd = makePlotDoc();
  QDomElement ts = addTsCurve(pd, u"/imu"_s, u"x"_s);
  ts.setAttribute(u"dataset_id"_s, u"7"_s);
  QDomElement hidden = pd.doc.createElement(u"marker_scope"_s);
  hidden.setAttribute(u"dataset_id"_s, u"7"_s);
  hidden.setAttribute(u"dataset_source"_s, u"run.mcap"_s);
  hidden.setAttribute(u"scope"_s, u"global"_s);
  hidden.setAttribute(u"visible"_s, u"false"_s);
  pd.plot.appendChild(hidden);
  QDomElement unknown = pd.doc.createElement(u"marker_scope"_s);
  unknown.setAttribute(u"dataset_id"_s, u"8"_s);
  unknown.setAttribute(u"scope"_s, u"dataset"_s);
  unknown.setAttribute(u"visible"_s, u"false"_s);
  pd.plot.appendChild(unknown);

  PJ::layout_xml::stampDatasetSourcePaths(
      pd.doc, [](std::uint32_t id) { return id == 7 ? u"data/run.mcap"_s : QString{}; });
  EXPECT_EQ(hidden.attribute(u"dataset_path"_s), u"data/run.mcap"_s) << "stamped like its curve";
  EXPECT_FALSE(unknown.hasAttribute(u"dataset_path"_s));

  PJ::layout_xml::removeUnvalidatedDatasetIds(pd.doc);
  EXPECT_TRUE(hidden.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(unknown.hasAttribute(u"dataset_id"_s)) << "an unvalidated volatile id must not survive a file save";

  PJ::layout_xml::removeDatasetQualifiersForGenericLayout(pd.doc);
  EXPECT_FALSE(hidden.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(hidden.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(hidden.hasAttribute(u"dataset_path"_s));
  EXPECT_EQ(hidden.attribute(u"scope"_s), u"global"_s);
  EXPECT_EQ(hidden.attribute(u"visible"_s), u"false"_s);
}

TEST(DatasetSourcePath, RemapCoversAllFiveQualifierShapesWithoutTouchingOtherAttributes) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  QDomElement element = doc.createElement(u"identity_carrier"_s);
  root.appendChild(element);
  const QStringList attributes{
      u"dataset_path"_s, u"x_dataset_path"_s, u"y_dataset_path"_s, u"input_dataset_path"_s, u"source_dataset_path"_s};
  for (const QString& attribute : attributes) {
    element.setAttribute(attribute, u"saved/log.csv"_s);
  }
  element.setAttribute(u"unrelated_path"_s, u"saved/log.csv"_s);
  QDomElement empty = doc.createElement(u"empty"_s);
  empty.setAttribute(u"dataset_path"_s, QString());
  root.appendChild(empty);

  int calls = 0;
  PJ::layout_xml::remapDatasetSourcePaths(doc, [&calls](const QString& path) {
    ++calls;
    EXPECT_EQ(path, u"saved/log.csv"_s);
    return u"pj-upload://fresh/9/log.csv"_s;
  });

  EXPECT_EQ(calls, 5);
  for (const QString& attribute : attributes) {
    EXPECT_EQ(element.attribute(attribute), u"pj-upload://fresh/9/log.csv"_s);
  }
  EXPECT_EQ(element.attribute(u"unrelated_path"_s), u"saved/log.csv"_s);
  EXPECT_TRUE(empty.attribute(u"dataset_path"_s).isEmpty());
}

TEST(DatasetSourcePath, DesktopResolverLeavesOpaqueUriQualifiersLiteral) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  QDomElement curve = doc.createElement(u"curve"_s);
  curve.setAttribute(u"dataset_path"_s, u"pj-upload://session/7/log.csv"_s);
  root.appendChild(curve);

  PJ::layout_xml::resolveDatasetSourcePaths(doc, QDir(u"/tmp/layouts"_s));
  EXPECT_EQ(curve.attribute(u"dataset_path"_s), u"pj-upload://session/7/log.csv"_s);
}

TEST(DatasetSourcePath, RelativizeRelocatesOnlyAbsoluteFilesystemQualifiers) {
  const QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  const QString inside = QDir(root.path()).filePath(u"in/data/run.mcap"_s);
  const QString outside = QDir(root.path()).filePath(u"out/far.mcap"_s);

  QDomDocument doc;
  const QString xml =
      u"<root>"
      u"<scene3d><layer dataset_path=\"%1\"/><config_topic dataset_path=\"%1\"/></scene3d>"
      u"<layer local=\"true\" dataset_id=\"0\"><robot_model source_dataset_path=\"%1\"/>"
      u"<trail source_dataset_path=\"%1\"/></layer>"
      u"<scene2d><layer dataset_path=\"%1\"/></scene2d>"
      u"<state_transitions>"
      u"<series dataset_path=\"%1\"/><series dataset_path=\"%2\"/><series dataset_path=\"\"/>"
      u"</state_transitions>"
      u"<curve dataset_path=\"data/run.mcap\" x_dataset_path=\"logs/http://capture.mcap\"/>"
      u"<upload><layer dataset_path=\"pj-upload://session/7/log.csv\"/></upload>"
      u"</root>"_s.arg(inside, outside);
  ASSERT_TRUE(doc.setContent(xml));

  QStringList calls;
  PJ::layout_xml::relativizeDatasetSourcePaths(doc, [&](const QString& path) {
    calls.push_back(path);
    return path == inside ? u"data/run.mcap"_s : path;
  });

  QStringList expected(6, inside);
  expected.push_back(outside);
  calls.sort();
  expected.sort();
  EXPECT_EQ(calls, expected);

  const QDomElement top = doc.documentElement();
  const QDomElement scene3d = top.firstChildElement(u"scene3d"_s);
  EXPECT_EQ(scene3d.firstChildElement(u"layer"_s).attribute(u"dataset_path"_s), u"data/run.mcap"_s);
  EXPECT_EQ(scene3d.firstChildElement(u"config_topic"_s).attribute(u"dataset_path"_s), u"data/run.mcap"_s);
  const QDomElement robot_layer = top.firstChildElement(u"layer"_s);
  EXPECT_EQ(robot_layer.attribute(u"dataset_id"_s), u"0"_s);
  EXPECT_EQ(robot_layer.firstChildElement(u"robot_model"_s).attribute(u"source_dataset_path"_s), u"data/run.mcap"_s);
  EXPECT_EQ(robot_layer.firstChildElement(u"trail"_s).attribute(u"source_dataset_path"_s), u"data/run.mcap"_s);
  EXPECT_EQ(
      top.firstChildElement(u"scene2d"_s).firstChildElement(u"layer"_s).attribute(u"dataset_path"_s),
      u"data/run.mcap"_s);
  const QDomElement states = top.firstChildElement(u"state_transitions"_s);
  EXPECT_EQ(states.elementsByTagName(u"series"_s).at(0).toElement().attribute(u"dataset_path"_s), u"data/run.mcap"_s);
  EXPECT_EQ(states.elementsByTagName(u"series"_s).at(1).toElement().attribute(u"dataset_path"_s), outside);
  EXPECT_EQ(states.elementsByTagName(u"series"_s).at(2).toElement().attribute(u"dataset_path"_s), QString());
  const QDomElement curve = top.firstChildElement(u"curve"_s);
  EXPECT_EQ(curve.attribute(u"dataset_path"_s), u"data/run.mcap"_s);
  EXPECT_EQ(curve.attribute(u"x_dataset_path"_s), u"logs/http://capture.mcap"_s);
  EXPECT_EQ(
      top.firstChildElement(u"upload"_s).firstChildElement(u"layer"_s).attribute(u"dataset_path"_s),
      u"pj-upload://session/7/log.csv"_s);
}

// A colon in a path component is legal on POSIX: only a real source identity is opaque, on
// save as on load, so such a file beneath the layout moves with it like any other.
TEST(DatasetSourcePath, RelativizeAndResolveAgreeOnWhatIsOpaque) {
  const QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  const QString layout = QDir::cleanPath(root.path());
  const QString with_colon = layout + u"/logs/http://capture.mcap"_s;
  const QString upload = u"pj-upload://session/7/log.csv"_s;
  const QString stream = u"pj-stream://robot"_s;

  QDomDocument doc;
  QDomElement element = doc.createElement(u"curve"_s);
  element.setAttribute(u"dataset_path"_s, with_colon);
  element.setAttribute(u"x_dataset_path"_s, upload);
  element.setAttribute(u"y_dataset_path"_s, stream);
  doc.appendChild(element);

  PJ::layout_xml::relativizeDatasetSourcePaths(doc, [&layout](const QString& path) {
    return path.startsWith(layout + u'/') ? path.mid(layout.size() + 1) : path;
  });
  EXPECT_EQ(element.attribute(u"dataset_path"_s), u"logs/http://capture.mcap"_s);
  EXPECT_EQ(element.attribute(u"x_dataset_path"_s), upload);
  EXPECT_EQ(element.attribute(u"y_dataset_path"_s), stream);

  PJ::layout_xml::resolveDatasetSourcePaths(doc, QDir(layout));
  EXPECT_EQ(element.attribute(u"dataset_path"_s), QDir::cleanPath(with_colon));
}

TEST(GenericLayout, RemovesDatasetQualifiersButKeepsStablePaths) {
  PlotDoc pd = makePlotDoc();
  QDomElement ts = addTsCurve(pd, u"/imu"_s, u"x"_s);
  ts.setAttribute(u"dataset_id"_s, u"7"_s);
  ts.setAttribute(u"dataset_source"_s, u"old.mcap"_s);
  ts.setAttribute(u"dataset_path"_s, u"data/old.mcap"_s);
  QDomElement xy = addXyCurve(pd, SeriesPath{u"/xy"_s, u"x"_s}, SeriesPath{u"/xy"_s, u"y"_s});
  xy.setAttribute(u"x_dataset_id"_s, u"7"_s);
  xy.setAttribute(u"x_dataset_source"_s, u"old.mcap"_s);
  xy.setAttribute(u"x_dataset_path"_s, u"data/old.mcap"_s);
  xy.setAttribute(u"y_dataset_id"_s, u"8"_s);
  xy.setAttribute(u"y_dataset_source"_s, u"other.mcap"_s);
  xy.setAttribute(u"y_dataset_path"_s, u"data/other.mcap"_s);

  QDomElement processors = pd.doc.createElement(u"data_processors"_s);
  QDomElement processor = pd.doc.createElement(u"processor"_s);
  processor.setAttribute(u"input_topic"_s, u"/imu"_s);
  processor.setAttribute(u"input_field"_s, u"x"_s);
  processor.setAttribute(u"input_dataset_id"_s, u"7"_s);
  processor.setAttribute(u"input_dataset_source"_s, u"old.mcap"_s);
  processor.setAttribute(u"input_dataset_path"_s, u"data/old.mcap"_s);
  processors.appendChild(processor);
  QDomElement transform = pd.doc.createElement(u"transform"_s);
  QDomElement input = pd.doc.createElement(u"input"_s);
  input.setAttribute(u"name"_s, u"/imu/x"_s);
  input.setAttribute(u"dataset_id"_s, u"7"_s);
  input.setAttribute(u"dataset_source"_s, u"old.mcap"_s);
  input.setAttribute(u"dataset_path"_s, u"data/old.mcap"_s);
  input.setAttribute(u"topic"_s, u"/imu"_s);
  input.setAttribute(u"field"_s, u"x"_s);
  input.setAttribute(u"column"_s, u"1"_s);
  transform.appendChild(input);
  processors.appendChild(transform);
  QDomElement generator = pd.doc.createElement(u"generator"_s);
  generator.setAttribute(u"id"_s, u"rule"_s);
  generator.setAttribute(u"all_datasets"_s, u"0"_s);
  generator.setAttribute(u"dataset_id"_s, u"7"_s);
  generator.setAttribute(u"dataset_source"_s, u"old.mcap"_s);
  generator.setAttribute(u"dataset_path"_s, u"data/old.mcap"_s);
  QDomElement generator_input = pd.doc.createElement(u"input"_s);
  generator_input.setAttribute(u"name"_s, u"/imu/x"_s);
  generator.appendChild(generator_input);
  processors.appendChild(generator);
  pd.doc.documentElement().appendChild(processors);

  QDomElement scene = pd.doc.createElement(u"scene3d"_s);
  QDomElement layer = pd.doc.createElement(u"layer"_s);
  layer.setAttribute(u"dataset_id"_s, u"7"_s);
  layer.setAttribute(u"dataset_source"_s, u"old.mcap"_s);
  layer.setAttribute(u"dataset_path"_s, u"data/old.mcap"_s);
  layer.setAttribute(u"topic_name"_s, u"/cloud"_s);
  QDomElement robot = pd.doc.createElement(u"robot_model"_s);
  robot.setAttribute(u"source_dataset_id"_s, u"7"_s);
  robot.setAttribute(u"source_dataset_source"_s, u"old.mcap"_s);
  robot.setAttribute(u"source_dataset_path"_s, u"data/old.mcap"_s);
  robot.setAttribute(u"source_topic"_s, u"/robot_description"_s);
  layer.appendChild(robot);
  scene.appendChild(layer);
  QDomElement config = pd.doc.createElement(u"config_topic"_s);
  config.setAttribute(u"dataset_id"_s, u"7"_s);
  config.setAttribute(u"dataset_source"_s, u"old.mcap"_s);
  config.setAttribute(u"dataset_path"_s, u"data/old.mcap"_s);
  config.setAttribute(u"topic_name"_s, u"/tf"_s);
  scene.appendChild(config);
  // A pose-topic trail payload (TrailLayer::xmlSaveState shape): its source_*
  // qualifiers must strip like <robot_model>'s, its identity and style survive.
  QDomElement trail = pd.doc.createElement(u"trail"_s);
  trail.setAttribute(u"source_kind"_s, u"pose_topic"_s);
  trail.setAttribute(u"source_topic_name"_s, u"/poses"_s);
  trail.setAttribute(u"source_dataset_id"_s, u"7"_s);
  trail.setAttribute(u"source_dataset_source"_s, u"old.mcap"_s);
  trail.setAttribute(u"source_dataset_path"_s, u"data/old.mcap"_s);
  trail.setAttribute(u"past_color"_s, u"#ff00ff"_s);
  scene.appendChild(trail);
  pd.doc.documentElement().appendChild(scene);

  // A Custom Widget wire carries the same qualifiers as a curve and must shed them too.
  QDomElement custom = pd.doc.createElement(u"CustomWidget"_s);
  custom.setAttribute(u"plugin"_s, u"basic-widgets"_s);
  QDomElement wire = pd.doc.createElement(u"input"_s);
  wire.setAttribute(u"name"_s, u"value"_s);
  wire.setAttribute(u"topic"_s, u"/battery"_s);
  wire.setAttribute(u"field"_s, u"voltage"_s);
  wire.setAttribute(u"dataset_id"_s, u"3"_s);
  wire.setAttribute(u"dataset_source"_s, u"old.mcap"_s);
  wire.setAttribute(u"dataset_path"_s, u"data/old.mcap"_s);
  custom.appendChild(wire);
  pd.doc.documentElement().appendChild(custom);

  PJ::layout_xml::removeDatasetQualifiersForGenericLayout(pd.doc);

  EXPECT_EQ(wire.attribute(u"topic"_s), u"/battery"_s);
  EXPECT_EQ(wire.attribute(u"field"_s), u"voltage"_s);
  EXPECT_FALSE(wire.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(wire.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(wire.hasAttribute(u"dataset_path"_s));

  // Plot curves and <processor> inputs: structural identity (topic/field) survives;
  // the exact dataset qualifiers are gone so binding falls back to unique-only.
  EXPECT_EQ(ts.attribute(u"topic"_s), u"/imu"_s);
  EXPECT_EQ(ts.attribute(u"field"_s), u"x"_s);
  EXPECT_FALSE(ts.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(ts.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(ts.hasAttribute(u"dataset_path"_s));
  EXPECT_FALSE(xy.hasAttribute(u"x_dataset_id"_s));
  EXPECT_FALSE(xy.hasAttribute(u"x_dataset_source"_s));
  EXPECT_FALSE(xy.hasAttribute(u"x_dataset_path"_s));
  EXPECT_FALSE(xy.hasAttribute(u"y_dataset_id"_s));
  EXPECT_FALSE(xy.hasAttribute(u"y_dataset_source"_s));
  EXPECT_FALSE(xy.hasAttribute(u"y_dataset_path"_s));
  EXPECT_FALSE(processor.hasAttribute(u"input_dataset_id"_s));
  EXPECT_FALSE(processor.hasAttribute(u"input_dataset_source"_s));
  EXPECT_FALSE(processor.hasAttribute(u"input_dataset_path"_s));

  // Transform input qualifiers are portable in source-bound files but stripped
  // from a generic export, leaving its topic/field identity for unique resolution.
  EXPECT_FALSE(input.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(input.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(input.hasAttribute(u"dataset_path"_s));
  EXPECT_EQ(input.attribute(u"topic"_s), u"/imu"_s);
  EXPECT_EQ(input.attribute(u"field"_s), u"x"_s);
  EXPECT_EQ(input.attribute(u"column"_s), u"1"_s);

  // A marker generator keeps its input series, which binds it to the one dataset
  // that carries them.
  EXPECT_FALSE(generator.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(generator.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(generator.hasAttribute(u"dataset_path"_s));
  EXPECT_EQ(generator.attribute(u"id"_s), u"rule"_s);
  EXPECT_EQ(generator_input.attribute(u"name"_s), u"/imu/x"_s);

  // Scene layers use the same unique-only structural rebind in generic layouts.
  // Removing all source qualifiers also prevents browser upload identities from
  // leaking into a downloaded generic layout.
  EXPECT_FALSE(layer.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(layer.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(layer.hasAttribute(u"dataset_path"_s));
  EXPECT_EQ(layer.attribute(u"topic_name"_s), u"/cloud"_s);
  EXPECT_FALSE(config.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(config.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(config.hasAttribute(u"dataset_path"_s));
  EXPECT_EQ(config.attribute(u"topic_name"_s), u"/tf"_s);
  EXPECT_FALSE(robot.hasAttribute(u"source_dataset_id"_s));
  EXPECT_FALSE(robot.hasAttribute(u"source_dataset_source"_s));
  EXPECT_FALSE(robot.hasAttribute(u"source_dataset_path"_s));
  EXPECT_EQ(robot.attribute(u"source_topic"_s), u"/robot_description"_s);
  EXPECT_FALSE(trail.hasAttribute(u"source_dataset_id"_s));
  EXPECT_FALSE(trail.hasAttribute(u"source_dataset_source"_s));
  EXPECT_FALSE(trail.hasAttribute(u"source_dataset_path"_s));
  EXPECT_EQ(trail.attribute(u"source_topic_name"_s), u"/poses"_s);
  EXPECT_EQ(trail.attribute(u"past_color"_s), u"#ff00ff"_s);
}

TEST(GenericLayout, StripsStateTransitionsSeriesQualifiers) {
  QDomDocument doc;
  ASSERT_TRUE(doc.setContent(
      u"<root><state_transitions zoom_px_per_ns=\"1\"><series dataset_id=\"7\" dataset_source=\"run.mcap\" "
      u"dataset_path=\"data/run.mcap\" topic=\"/mode\" field=\"value\" visible=\"0\"/></state_transitions>"
      u"<other><series dataset_id=\"7\" dataset_source=\"run.mcap\" dataset_path=\"data/run.mcap\"/></other>"
      u"</root>"_s));

  PJ::layout_xml::removeDatasetQualifiersForGenericLayout(doc);

  const QDomElement series =
      doc.documentElement().firstChildElement(u"state_transitions"_s).firstChildElement(u"series"_s);
  EXPECT_FALSE(series.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(series.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(series.hasAttribute(u"dataset_path"_s));
  EXPECT_EQ(series.attribute(u"topic"_s), u"/mode"_s);
  EXPECT_EQ(series.attribute(u"field"_s), u"value"_s);
  EXPECT_EQ(series.attribute(u"visible"_s), u"0"_s);
  // The attribute names mean a dataset qualifier wherever they appear.
  const QDomElement elsewhere = doc.documentElement().firstChildElement(u"other"_s).firstChildElement(u"series"_s);
  EXPECT_FALSE(elsewhere.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(elsewhere.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(elsewhere.hasAttribute(u"dataset_path"_s));
}

// Stamping/removing unvalidated ids are source-bound passes and leave scene-owned
// identities alone. Generic export then deliberately strips them so the scene
// rebinds by its unique topic/type structural identity.
TEST(DatasetIdentityPasses, SceneLayerIdentityIsOnlyStrippedForGenericExport) {
  QTemporaryDir layout_dir;
  ASSERT_TRUE(layout_dir.isValid());
  QDomDocument doc;
  QDomElement scene = doc.createElement(u"scene3d"_s);
  scene.setAttribute(u"version"_s, u"1"_s);
  doc.appendChild(scene);
  // Mirrors SceneDockWidget::xmlSaveState's <layer> shape for a dataset with no source path (id + source, no path).
  QDomElement layer = doc.createElement(u"layer"_s);
  layer.setAttribute(u"dataset_id"_s, u"3"_s);
  layer.setAttribute(u"dataset_source"_s, u"run.mcap"_s);
  layer.setAttribute(u"topic_name"_s, u"/cloud"_s);
  layer.setAttribute(u"object_type"_s, u"PointCloud"_s);
  layer.setAttribute(u"display_name"_s, u"/cloud"_s);
  layer.setAttribute(u"visible"_s, u"true"_s);
  QDomElement payload = doc.createElement(u"pointcloud"_s);
  payload.setAttribute(u"point_size"_s, u"2"_s);
  layer.appendChild(payload);
  scene.appendChild(layer);
  // A config-topic (TF) child, also id-qualified.
  QDomElement config = doc.createElement(u"config_topic"_s);
  config.setAttribute(u"dataset_id"_s, u"3"_s);
  config.setAttribute(u"dataset_source"_s, u"run.mcap"_s);
  config.setAttribute(u"topic_name"_s, u"/tf"_s);
  scene.appendChild(config);

  const QByteArray before_source_passes = doc.toByteArray(2);

  PJ::layout_xml::stampDatasetSourcePaths(doc, [](std::uint32_t) { return u"data/run.mcap"_s; });
  PJ::layout_xml::removeUnvalidatedDatasetIds(doc);
  EXPECT_EQ(doc.toByteArray(2), before_source_passes);

  PJ::layout_xml::removeDatasetQualifiersForGenericLayout(doc);

  EXPECT_FALSE(layer.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(layer.hasAttribute(u"dataset_source"_s));
  EXPECT_FALSE(config.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(config.hasAttribute(u"dataset_source"_s));
  EXPECT_EQ(layer.attribute(u"topic_name"_s), u"/cloud"_s);
  EXPECT_EQ(config.attribute(u"topic_name"_s), u"/tf"_s);
}

TEST(RebindCurveKeys, SetsNameForResolvedTimeSeries) {
  PlotDoc pd = makePlotDoc();
  QDomElement c = addTsCurve(pd, u"/imu"_s, u"accel.x"_s);
  const auto resolve = [](const SeriesPath& p) -> std::optional<QString> {
    if (p.topic == "/imu"_L1 && p.field == "accel.x"_L1) {
      return u"dataset:7/topic:3/column:0"_s;
    }
    return std::nullopt;
  };
  const QList<SeriesPath> unresolved = PJ::layout_xml::rebindCurveKeys(pd.doc, resolve);
  EXPECT_TRUE(unresolved.isEmpty());
  EXPECT_EQ(c.attribute(u"name"_s), u"dataset:7/topic:3/column:0"_s);
}

TEST(RebindCurveKeys, ClearsNameAndReportsUnresolvedTimeSeries) {
  PlotDoc pd = makePlotDoc();
  QDomElement c = addTsCurve(pd, u"/missing"_s, u"f"_s);
  c.setAttribute(u"name"_s, u"stale_key"_s);  // stale from prior session
  const auto resolve = [](const SeriesPath&) -> std::optional<QString> { return std::nullopt; };
  const QList<SeriesPath> unresolved = PJ::layout_xml::rebindCurveKeys(pd.doc, resolve);
  ASSERT_EQ(unresolved.size(), 1);
  EXPECT_EQ(unresolved[0], (SeriesPath{u"/missing"_s, u"f"_s}));
  EXPECT_FALSE(c.hasAttribute(u"name"_s));  // stale key cleared, won't mis-resolve
  EXPECT_EQ(c.attribute(u"topic"_s), u"/missing"_s);
  EXPECT_EQ(c.attribute(u"field"_s), u"f"_s);
}

TEST(RebindCurveKeys, ResolvesXyOnlyWhenBothAxesMatch) {
  PlotDoc pd = makePlotDoc();
  const SeriesPath x{u"/t"_s, u"a"_s};
  const SeriesPath y{u"/t"_s, u"b"_s};
  QDomElement both = addXyCurve(pd, x, y);
  QDomElement half = addXyCurve(pd, x, SeriesPath{u"/t"_s, u"absent"_s});
  const auto resolve = [&](const SeriesPath& p) -> std::optional<QString> {
    if (p == x) {
      return u"kx"_s;
    }
    if (p == y) {
      return u"ky"_s;
    }
    return std::nullopt;
  };
  const QList<SeriesPath> unresolved = PJ::layout_xml::rebindCurveKeys(pd.doc, resolve);
  EXPECT_EQ(both.attribute(u"curve_x"_s), u"kx"_s);
  EXPECT_EQ(both.attribute(u"curve_y"_s), u"ky"_s);
  EXPECT_FALSE(half.hasAttribute(u"curve_x"_s));  // partial → both cleared
  EXPECT_FALSE(half.hasAttribute(u"curve_y"_s));
  EXPECT_EQ(half.attribute(u"x_topic"_s), x.topic);
  EXPECT_EQ(half.attribute(u"x_field"_s), x.field);
  EXPECT_EQ(half.attribute(u"y_topic"_s), u"/t"_s);
  EXPECT_EQ(half.attribute(u"y_field"_s), u"absent"_s);
  // Only the genuinely-missing half is reported — x resolved, so listing it as
  // "missing" would mislead the prompt.
  ASSERT_EQ(unresolved.size(), 1);
  EXPECT_EQ(unresolved[0], (SeriesPath{u"/t"_s, u"absent"_s}));
}

TEST(RebindCurveKeys, ClearsKeysAndPreservesStableAttrsForUnresolvedXy) {
  PlotDoc pd = makePlotDoc();
  const SeriesPath x{u"/pose"_s, u"x"_s};
  const SeriesPath y{u"/pose"_s, u"y"_s};
  QDomElement c = addXyCurve(pd, x, y);
  c.setAttribute(u"curve_x"_s, u"stale_x"_s);
  c.setAttribute(u"curve_y"_s, u"stale_y"_s);

  const auto resolve = [](const SeriesPath&) -> std::optional<QString> { return std::nullopt; };
  const QList<SeriesPath> unresolved = PJ::layout_xml::rebindCurveKeys(pd.doc, resolve);

  EXPECT_FALSE(c.hasAttribute(u"curve_x"_s));
  EXPECT_FALSE(c.hasAttribute(u"curve_y"_s));
  EXPECT_EQ(c.attribute(u"x_topic"_s), x.topic);
  EXPECT_EQ(c.attribute(u"x_field"_s), x.field);
  EXPECT_EQ(c.attribute(u"y_topic"_s), y.topic);
  EXPECT_EQ(c.attribute(u"y_field"_s), y.field);
  ASSERT_EQ(unresolved.size(), 2);
  EXPECT_EQ(unresolved[0], x);
  EXPECT_EQ(unresolved[1], y);
}

TEST(StripUnresolvedCurves, RemovesOnlyKeylessCurves) {
  PlotDoc pd = makePlotDoc();
  QDomElement keep = addTsCurve(pd, u"/t"_s, u"a"_s);
  keep.setAttribute(u"name"_s, u"resolved_key"_s);
  addTsCurve(pd, u"/t"_s, u"b"_s);  // no name → unresolved
  PJ::layout_xml::stripUnresolvedCurves(pd.doc);
  const QDomNodeList curves = pd.doc.elementsByTagName(u"curve"_s);
  ASSERT_EQ(curves.size(), 1);
  EXPECT_EQ(curves.at(0).toElement().attribute(u"name"_s), u"resolved_key"_s);
}

// ---------- SourceTimelineViewState ----------------------------------------

using PJ::layout_xml::readSourceTimelineViewState;
using PJ::layout_xml::SourceTimelineViewState;
using PJ::layout_xml::writeSourceTimelineViewState;

// write -> serialize -> reparse -> read, so the full XML pipeline (not just the
// in-memory DOM) is exercised.
SourceTimelineViewState roundTripViewState(const SourceTimelineViewState& in) {
  QDomDocument doc;
  doc.appendChild(writeSourceTimelineViewState(doc, in));
  QDomDocument reparsed;
  EXPECT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  return readSourceTimelineViewState(reparsed.documentElement());
}

TEST(SourceTimelineViewState, AllFieldsRoundTrip) {
  SourceTimelineViewState in;
  in.zoom = 1.234567890123456e-7;  // tiny pixels-per-ns: 17 sig-figs must survive
  in.scroll_left_ns = -5'000'000'000LL;
  in.scroll_top_px = 84;
  in.name_column_width = 173;
  in.snap = false;

  const SourceTimelineViewState out = roundTripViewState(in);
  ASSERT_TRUE(out.zoom.has_value());
  EXPECT_DOUBLE_EQ(*out.zoom, *in.zoom);  // exact: 'g',17 preserves the double
  ASSERT_TRUE(out.scroll_left_ns.has_value());
  EXPECT_EQ(*out.scroll_left_ns, *in.scroll_left_ns);
  ASSERT_TRUE(out.scroll_top_px.has_value());
  EXPECT_EQ(*out.scroll_top_px, 84);
  ASSERT_TRUE(out.name_column_width.has_value());
  EXPECT_EQ(*out.name_column_width, 173);
  ASSERT_TRUE(out.snap.has_value());
  EXPECT_FALSE(*out.snap);
}

TEST(SourceTimelineViewState, AbsentElementYieldsAllNullopt) {
  // A pre-Source-Timeline layout has no <source_timeline> element.
  const SourceTimelineViewState out = readSourceTimelineViewState(QDomElement{});
  EXPECT_FALSE(out.zoom.has_value());
  EXPECT_FALSE(out.scroll_left_ns.has_value());
  EXPECT_FALSE(out.scroll_top_px.has_value());
  EXPECT_FALSE(out.name_column_width.has_value());
  EXPECT_FALSE(out.snap.has_value());
}

TEST(SourceTimelineViewState, MissingAndMalformedAttributesStayNullopt) {
  QDomDocument doc;
  QDomElement el = doc.createElement(u"source_timeline"_s);
  el.setAttribute(u"zoom"_s, u"0"_s);                // non-positive → rejected
  el.setAttribute(u"name_column_width"_s, u"-4"_s);  // non-positive → rejected
  el.setAttribute(u"scroll_left_ns"_s, u"not-a-number"_s);
  el.setAttribute(u"scroll_top_px"_s, u"-1"_s);
  // snap omitted entirely.
  const SourceTimelineViewState out = readSourceTimelineViewState(el);
  EXPECT_FALSE(out.zoom.has_value());
  EXPECT_FALSE(out.name_column_width.has_value());
  EXPECT_FALSE(out.scroll_left_ns.has_value());
  EXPECT_FALSE(out.scroll_top_px.has_value());
  EXPECT_FALSE(out.snap.has_value());
}

TEST(SourceTimelineViewState, UnsetFieldsAreNotWritten) {
  // Only the snap field is set; the element must carry no other attributes, so a
  // partially-populated state never injects bogus zeros on reload.
  SourceTimelineViewState in;
  in.snap = true;
  QDomDocument doc;
  const QDomElement el = writeSourceTimelineViewState(doc, in);
  EXPECT_TRUE(el.hasAttribute(u"snap"_s));
  EXPECT_FALSE(el.hasAttribute(u"zoom"_s));
  EXPECT_FALSE(el.hasAttribute(u"scroll_left_ns"_s));
  EXPECT_FALSE(el.hasAttribute(u"scroll_top_px"_s));
  EXPECT_FALSE(el.hasAttribute(u"name_column_width"_s));
}

namespace {
// Builds a <plot mode=...> carrying a <range> with the given attributes but no
// x_basis marker, so normalizePlotRangeBasis has something historical to annotate.
QDomElement addRange(PlotDoc& pd) {
  QDomElement range = pd.doc.createElement(u"range"_s);
  range.setAttribute(u"left"_s, u"1600000000.000000"_s);
  range.setAttribute(u"right"_s, u"1600000001.000000"_s);
  range.setAttribute(u"top"_s, u"5.0"_s);
  range.setAttribute(u"bottom"_s, u"-5.0"_s);
  pd.plot.appendChild(range);
  return range;
}
}  // namespace

TEST(NormalizePlotRangeBasis, TimeSeriesRangeGetsAbsoluteBasis) {
  PlotDoc pd = makePlotDoc();
  pd.plot.setAttribute(u"mode"_s, u"TimeSeries"_s);
  QDomElement range = addRange(pd);
  PJ::layout_xml::normalizePlotRangeBasis(pd.doc);
  EXPECT_EQ(range.attribute(u"x_basis"_s), u"absolute"_s);
  EXPECT_EQ(range.attribute(u"left"_s), u"1600000000.000000"_s);
  EXPECT_EQ(range.attribute(u"right"_s), u"1600000001.000000"_s);
}

TEST(NormalizePlotRangeBasis, XyRangeGetsValueBasis) {
  PlotDoc pd = makePlotDoc();
  pd.plot.setAttribute(u"mode"_s, u"XYPlot"_s);
  QDomElement range = addRange(pd);
  PJ::layout_xml::normalizePlotRangeBasis(pd.doc);
  EXPECT_EQ(range.attribute(u"x_basis"_s), u"value"_s);
}

TEST(NormalizePlotRangeBasis, ExistingBasisIsLeftUntouched) {
  PlotDoc pd = makePlotDoc();
  pd.plot.setAttribute(u"mode"_s, u"XYPlot"_s);
  QDomElement range = addRange(pd);
  range.setAttribute(u"x_basis"_s, u"absolute"_s);  // deliberately mismatched vs mode
  PJ::layout_xml::normalizePlotRangeBasis(pd.doc);
  // Idempotent: a range that already carries x_basis is never re-derived.
  EXPECT_EQ(range.attribute(u"x_basis"_s), u"absolute"_s);
}

TEST(NormalizePlotRangeBasis, PlotWithoutRangeIsSkipped) {
  PlotDoc pd = makePlotDoc();
  pd.plot.setAttribute(u"mode"_s, u"TimeSeries"_s);
  PJ::layout_xml::normalizePlotRangeBasis(pd.doc);
  EXPECT_TRUE(pd.plot.firstChildElement(u"range"_s).isNull());
}

// --- matchFanoutDatasets: the shape-guarded fan-out matcher -------------------

namespace fanout {

PJ::layout_xml::DataSourceDatasetRef savedChild(const QString& name, int index) {
  PJ::layout_xml::DataSourceDatasetRef child;
  child.source_name = name;
  child.source_index = index;
  return child;
}

// Candidate ids double as name keys: names[i] names candidates[i].
std::function<QString(std::uint32_t)> namesOf(const std::vector<std::uint32_t>& candidates, QStringList names) {
  return [candidates, names = std::move(names)](std::uint32_t id) {
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      if (candidates[index] == id) {
        return names[static_cast<qsizetype>(index)];
      }
    }
    return QString();
  };
}

}  // namespace fanout

TEST(MatchFanoutDatasets, UniqueNamesBindByNameRegardlessOfIndex) {
  const std::vector<std::uint32_t> candidates{11, 22};
  const auto matches = PJ::layout_xml::matchFanoutDatasets(
      {fanout::savedChild(u"right"_s, 0), fanout::savedChild(u"left"_s, 1)}, candidates,
      fanout::namesOf(candidates, {u"left"_s, u"right"_s}));
  EXPECT_EQ(matches, (std::vector<std::uint32_t>{22, 11}));
}

TEST(MatchFanoutDatasets, DuplicateNamesBindIndexOnlyWhileShapeIsPreserved) {
  const std::vector<std::uint32_t> candidates{11, 22, 33};
  const auto matches = PJ::layout_xml::matchFanoutDatasets(
      {fanout::savedChild(u"cam"_s, 0), fanout::savedChild(u"cam"_s, 2), fanout::savedChild(u"imu"_s, 1)}, candidates,
      fanout::namesOf(candidates, {u"cam"_s, u"imu"_s, u"cam"_s}));
  EXPECT_EQ(matches, (std::vector<std::uint32_t>{11, 33, 22}));
}

TEST(MatchFanoutDatasets, ChangedShapeBindsNoDuplicateNameChild) {
  // Two saved "cam" tracks but only one live "cam": a surviving sibling must
  // never inherit an offset that cannot be proven its own.
  const std::vector<std::uint32_t> candidates{11, 22};
  const auto matches = PJ::layout_xml::matchFanoutDatasets(
      {fanout::savedChild(u"cam"_s, 0), fanout::savedChild(u"cam"_s, 1)}, candidates,
      fanout::namesOf(candidates, {u"cam"_s, u"imu"_s}));
  EXPECT_EQ(matches, (std::vector<std::uint32_t>{0, 0}));
}

TEST(MatchFanoutDatasets, IndexBindRequiresNameAgreementAndUnconsumedCandidate) {
  const std::vector<std::uint32_t> candidates{11, 22};
  // Child 0's unique name is not loaded and its index points at a
  // differently-named candidate: the index fallback must not bind it. Child 1
  // then consumes candidate 22 by unique name; child 2's index points at that
  // consumed candidate and stays unbound.
  const auto matches = PJ::layout_xml::matchFanoutDatasets(
      {fanout::savedChild(u"gps"_s, 0), fanout::savedChild(u"right"_s, 0), fanout::savedChild(u"right2"_s, 1)},
      candidates, fanout::namesOf(candidates, {u"left"_s, u"right"_s}));
  EXPECT_EQ(matches[0], 0U) << "index bind must agree on the saved name";
  EXPECT_EQ(matches[1], 22U) << "unique name binds by name, not its stale index";
  EXPECT_EQ(matches[2], 0U) << "a consumed candidate cannot bind again";
}

TEST(MatchFanoutDatasets, EmptySavedNameMatchesByIndexAcrossAnyNames) {
  const std::vector<std::uint32_t> candidates{11, 22};
  const auto matches = PJ::layout_xml::matchFanoutDatasets(
      {fanout::savedChild(QString(), 1)}, candidates, fanout::namesOf(candidates, {u"a"_s, u"b"_s}));
  EXPECT_EQ(matches, (std::vector<std::uint32_t>{22}));
}

TEST(BrowserLayoutSerialization, DetectsEveryEphemeralPathSentinel) {
  EXPECT_FALSE(
      PJ::layout_xml::containsEphemeralBrowserPath(
          QByteArrayLiteral("<root binding=\"generic\"><curve source=\"capture.mcap\"/></root>")));
  EXPECT_TRUE(
      PJ::layout_xml::containsEphemeralBrowserPath(
          QByteArrayLiteral("<root><curve dataset_path=\"pj-upload://session/7/capture.mcap\"/></root>")));
  EXPECT_TRUE(
      PJ::layout_xml::containsEphemeralBrowserPath(QByteArrayLiteral(
          "<root><plugin><![CDATA[{\"filepath\":\"/pj_uploads/7/capture.mcap\"}]]></plugin></root>")));
}

// ---------- streaming sources -----------------------------------------------

QDomDocument parseLayout(const QByteArray& xml) {
  QDomDocument doc;
  EXPECT_TRUE(static_cast<bool>(doc.setContent(xml))) << xml.toStdString();
  return doc;
}

TEST(LayoutXmlTest, StreamingRoundTripAndDuplicateIds) {
  using PJ::layout_xml::StreamingSourceRef;
  // Two streams of one plugin stay two records. The configuration is the saved template:
  // it round-trips unexpanded and byte-exact, CDATA terminator included.
  StreamingSourceRef robot;
  robot.id = u"robot"_s;
  robot.source_name = u"Robot"_s;
  robot.plugin_id = u"PlotJuggler Bridge"_s;
  robot.plugin_manifest_id = u"plotjuggler-bridge"_s;
  robot.plugin_config_json = uR"({"url":"${deployment.ws_origin}/bridge","note":"]]>"})"_s;
  robot.dataset.source_name = u"[stream] Robot"_s;
  robot.dataset.source_index = 0;
  robot.dataset.display_offset_ns = -250;
  robot.dataset.has_display_offset = true;
  robot.dataset.timeline_order = 1;
  StreamingSourceRef arm = robot;
  arm.id = u"arm"_s;
  arm.source_name = u"Arm"_s;
  arm.plugin_config_json = uR"({"url":"ws://10.0.0.2:9871/bridge"})"_s;
  arm.dataset = {};

  QDomDocument doc = parseLayout(R"(<root pj4_version="4" binding="source"/>)");
  QString error = u"stale"_s;
  ASSERT_TRUE(PJ::layout_xml::writeStreamingSources(doc, {robot, arm}, &error)) << error.toStdString();
  EXPECT_TRUE(error.isEmpty());
  EXPECT_EQ(PJ::layout_xml::kStreamingLayoutSchemaVersion, 5);
  EXPECT_EQ(doc.documentElement().attribute(u"pj4_version"_s), u"5"_s)
      << "only a stream-bearing layout is written with the newer schema";

  error = u"stale"_s;
  const QList<StreamingSourceRef> refs =
      PJ::layout_xml::extractStreamingSources(parseLayout(doc.toByteArray(2)), &error);
  EXPECT_TRUE(error.isEmpty()) << error.toStdString();
  ASSERT_EQ(refs.size(), 2);
  const std::array<const StreamingSourceRef*, 2> expected{&robot, &arm};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const StreamingSourceRef& want = *expected[index];
    const StreamingSourceRef& got = refs[static_cast<qsizetype>(index)];
    EXPECT_EQ(got.id, want.id);
    EXPECT_EQ(got.source_name, want.source_name);
    EXPECT_EQ(got.plugin_id, want.plugin_id);
    EXPECT_EQ(got.plugin_manifest_id, want.plugin_manifest_id);
    EXPECT_EQ(got.plugin_config_json, want.plugin_config_json);
    EXPECT_EQ(got.dataset.source_name, want.dataset.source_name);
    EXPECT_EQ(got.dataset.source_index, want.dataset.source_index);
    EXPECT_EQ(got.dataset.display_offset_ns, want.dataset.display_offset_ns);
    EXPECT_EQ(got.dataset.has_display_offset, want.dataset.has_display_offset);
    EXPECT_FALSE(got.dataset.display_offset_includes_global_reference);
    EXPECT_EQ(got.dataset.timeline_order, want.dataset.timeline_order);
  }

  // A layout without streams keeps the plain schema and gains no empty block.
  QDomDocument plain = parseLayout(R"(<root pj4_version="4" binding="source"/>)");
  EXPECT_TRUE(PJ::layout_xml::writeStreamingSources(plain, {}, &error));
  EXPECT_EQ(plain.documentElement().attribute(u"pj4_version"_s), u"4"_s);
  EXPECT_FALSE(plain.documentElement().hasChildNodes());
  error = u"stale"_s;
  EXPECT_TRUE(PJ::layout_xml::extractStreamingSources(plain, &error).isEmpty());
  EXPECT_TRUE(error.isEmpty());

  // The hand-authored preset form: the CDATA payload is kept as written.
  const QList<StreamingSourceRef> authored = PJ::layout_xml::extractStreamingSources(
      parseLayout(R"(<root pj4_version="5" binding="source"><previouslyLoaded_StreamingSources>
  <streamInfo id="robot" source_name="Robot">
    <plugin ID="PlotJuggler Bridge" manifest_id="plotjuggler-bridge"><![CDATA[
      {"url":"${deployment.ws_origin}/bridge"}
    ]]></plugin>
  </streamInfo>
</previouslyLoaded_StreamingSources></root>)"),
      &error);
  EXPECT_TRUE(error.isEmpty()) << error.toStdString();
  ASSERT_EQ(authored.size(), 1);
  EXPECT_EQ(authored.front().plugin_config_json, u"\n      {\"url\":\"${deployment.ws_origin}/bridge\"}\n    "_s);
  EXPECT_EQ(authored.front().dataset.source_index, -1);
  EXPECT_FALSE(authored.front().dataset.has_display_offset);
  EXPECT_EQ(authored.front().dataset.timeline_order, -1);

  // Any invalid record rejects the whole block, so a layout never replays a partial stream set.
  const auto rejection = [](const char* records) {
    QString reason;
    const QList<StreamingSourceRef> parsed = PJ::layout_xml::extractStreamingSources(
        parseLayout(
            QByteArray("<root pj4_version=\"5\"><previouslyLoaded_StreamingSources>") + records +
            "</previouslyLoaded_StreamingSources></root>"),
        &reason);
    EXPECT_TRUE(parsed.isEmpty()) << records;
    EXPECT_FALSE(reason.isEmpty()) << records;
    return reason.toStdString();
  };
  constexpr auto kBridge = R"(<plugin ID="PlotJuggler Bridge"><![CDATA[{"url":"ws://a"}]]></plugin>)";
  const std::string duplicate =
      rejection((u"<streamInfo id=\"robot\">%1</streamInfo><streamInfo id=\"robot\">%1</streamInfo>"_s.arg(
                     QLatin1StringView(kBridge)))
                    .toUtf8()
                    .constData());
  EXPECT_NE(duplicate.find("robot"), std::string::npos) << duplicate;
  rejection((u"<streamInfo>%1</streamInfo>"_s.arg(QLatin1StringView(kBridge))).toUtf8().constData());
  rejection(R"(<streamInfo id="robot"/>)");
  rejection(R"(<streamInfo id="robot"><plugin><![CDATA[{"url":"ws://a"}]]></plugin></streamInfo>)");
  rejection(R"(<streamInfo id="robot"><plugin manifest_id="plotjuggler-bridge">  </plugin></streamInfo>)");

  // Everything bound to a stream is qualified by its opaque record identity, never a path.
  const QString identity = PJ::layout_xml::streamSourceIdentity(u"robot"_s);
  EXPECT_EQ(identity, u"pj-stream://robot"_s);
  QDomDocument bound = parseLayout(R"(<root><plot><curve topic="/imu" field="x"/></plot></root>)");
  QDomElement curve = bound.documentElement().firstChildElement(u"plot"_s).firstChildElement(u"curve"_s);
  curve.setAttribute(u"dataset_path"_s, identity);
  PJ::layout_xml::resolveDatasetSourcePaths(bound, QDir(u"/tmp/layouts"_s));
  EXPECT_EQ(curve.attribute(u"dataset_path"_s), identity);
  EXPECT_TRUE(PJ::layout_xml::isSamePath(identity, identity));
  EXPECT_FALSE(PJ::layout_xml::isSamePath(identity, PJ::layout_xml::streamSourceIdentity(u"arm"_s)));
}

// A robot or trail layer names its stream only on its nested payload. Binding marks the owning
// <layer> pending (it waits for the topic instead of failing the restore) and gives it the
// stream's dataset, so its topic is demanded from that stream alone.
TEST(LayoutXmlTest, BindingANestedStreamReferenceMarksItsLayerPending) {
  QDomDocument doc = parseLayout(R"(<root><scene3d>
    <layer local="true" object_type="robot_description" order="0">
      <robot_model source_type="topic" source_topic_name="robot_description" source_dataset_path="pj-stream://robot"/>
    </layer>
    <layer role="trail" order="1">
      <trail source_kind="pose_topic" source_topic_name="pose" source_dataset_path="pj-stream://robot"/>
    </layer>
    <layer local="true" object_type="robot_description" order="2">
      <robot_model source_type="topic" source_topic_name="robot_description" source_dataset_path="data.mcap"/>
    </layer>
  </scene3d></root>)");

  PJ::layout_xml::bindStreamQualifiers(doc, {{u"pj-stream://robot"_s, 7U}});

  const QDomNodeList layers = doc.elementsByTagName(u"layer"_s);
  ASSERT_EQ(layers.size(), 3);
  for (int index = 0; index < 2; ++index) {
    const QDomElement layer = layers.at(index).toElement();
    EXPECT_EQ(layer.attribute(u"pending_intent"_s), u"true"_s) << index;
    EXPECT_EQ(layer.attribute(u"dataset_id"_s), u"7"_s) << index;
    const QDomElement payload = layer.firstChildElement();
    EXPECT_EQ(payload.attribute(u"source_dataset_id"_s), u"7"_s) << index;
    EXPECT_FALSE(payload.hasAttribute(u"source_dataset_path"_s)) << index;
  }
  const QDomElement file_bound = layers.at(2).toElement();
  EXPECT_FALSE(file_bound.hasAttribute(u"pending_intent"_s));
  EXPECT_FALSE(file_bound.hasAttribute(u"dataset_id"_s));
}

// An on-demand input's name is the alias its script reads, and what it binds stays recorded beside it.
TEST(DatasetIdentityPasses, GenericLayoutKeepsAnOnDemandInputNameAndWhatItReads) {
  QDomDocument doc;
  ASSERT_TRUE(doc.setContent(
      u"<root><on_demand id=\"crop\"><input name=\"run.mcap:cloud\" dataset_id=\"7\" dataset_source=\"run.mcap\" "
      u"topic=\"cloud\"/></on_demand></root>"_s));

  PJ::layout_xml::removeDatasetQualifiersForGenericLayout(doc);

  const QDomElement input = doc.elementsByTagName(u"input"_s).at(0).toElement();
  EXPECT_EQ(input.attribute(u"name"_s), u"run.mcap:cloud"_s);
  EXPECT_EQ(input.attribute(u"topic"_s), u"cloud"_s);
  EXPECT_FALSE(input.hasAttribute(u"dataset_id"_s));
  EXPECT_FALSE(input.hasAttribute(u"dataset_source"_s));
}

// Startup's explicit files spare the qualifiers that name them. Ids are always stripped.
class KeptFileQualifiers : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.isValid());
    const QDir root(dir_.path());
    ASSERT_TRUE(root.mkpath(u"a"_s) && root.mkpath(u"b"_s) && root.mkpath(u"d"_s) && root.mkpath(u"e"_s));
    for (const QString& name :
         {u"a/run.csv"_s, u"b/run.csv"_s, u"a/other.csv"_s, u"d/left.csv"_s, u"d/right.csv"_s, u"e/left.csv"_s,
          u"e/right_new.csv"_s, u"e/gps_new.csv"_s, u"e/robot_new.mcap"_s}) {
      QFile file(root.filePath(name));
      ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }
    ASSERT_TRUE(QFile::link(root.filePath(u"b/run.csv"_s), root.filePath(u"link.csv"_s)));
    a_run_ = QFileInfo(root.filePath(u"a/run.csv"_s)).canonicalFilePath();
    b_run_ = QFileInfo(root.filePath(u"b/run.csv"_s)).canonicalFilePath();
    other_ = QFileInfo(root.filePath(u"a/other.csv"_s)).canonicalFilePath();
  }

  // The <root>'s children after the rewrite, by their `id`.
  QHash<QString, QDomElement> rewrite(const QString& children, const QStringList& keep, const QStringList& order = {}) {
    doc_ = QDomDocument();
    EXPECT_TRUE(doc_.setContent(u"<root>"_s + children + u"</root>"_s));
    matches_ = PJ::layout_xml::matchDatasetsToFiles(doc_, keep, order);
    PJ::layout_xml::removeDatasetQualifiersForGenericLayout(doc_, matches_);
    QHash<QString, QDomElement> by_id;
    for (QDomElement e = doc_.documentElement().firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
      by_id.insert(e.attribute(u"id"_s), e);
    }
    return by_id;
  }

  QString path(const QString& relative) const {
    return QDir(dir_.path()).filePath(relative);
  }

  QTemporaryDir dir_;
  QDomDocument doc_;
  QList<PJ::layout_xml::DatasetFileMatch> matches_;
  QString a_run_;
  QString b_run_;
  QString other_;
};

// The canonical file wins over a namesake, and the kept label becomes the explicit file's name: the saved one (a
// symlink's name, a fan-out entry under the old base name) would fail the resolver's exact label check.
TEST_F(KeptFileQualifiers, CanonicalMatchTakesTheExplicitFilesName) {
  auto by_id = rewrite(
      u"<input id=\"link\" dataset_id=\"3\" dataset_source=\"link.csv\" dataset_path=\"%1\"/>"
      u"<input id=\"fanout\" dataset_source=\"link/imu\" dataset_path=\"%1\"/>"
      u"<input id=\"plugin_label\" dataset_source=\"Robot A\" dataset_path=\"%1\"/>"_s.arg(path(u"link.csv"_s)),
      {a_run_, b_run_});
  EXPECT_EQ(by_id[u"link"_s].attribute(u"dataset_path"_s), b_run_);
  EXPECT_EQ(by_id[u"link"_s].attribute(u"dataset_source"_s), u"run.csv"_s);
  EXPECT_FALSE(by_id[u"link"_s].hasAttribute(u"dataset_id"_s));
  EXPECT_EQ(by_id[u"fanout"_s].attribute(u"dataset_source"_s), u"run/imu"_s);
  EXPECT_EQ(by_id[u"plugin_label"_s].attribute(u"dataset_source"_s), u"Robot A"_s) << "not derived from the file";
}

// The file-name fallback: only to an explicit file no saved path names canonically, and only when one file has the
// name. A qualifier without a path matches by its label.
TEST_F(KeptFileQualifiers, FileNameFallbackTakesOnlyAnUnclaimedFile) {
  auto by_id = rewrite(
      u"<curve id=\"gone\" dataset_source=\"other.csv\" dataset_path=\"/elsewhere/other.csv\"/>"
      u"<curve id=\"two_namesakes\" dataset_source=\"run.csv\" dataset_path=\"/elsewhere/run.csv\"/>"
      u"<input id=\"by_label\" dataset_source=\"other.csv\"/>"
      u"<layer id=\"opaque\" dataset_source=\"other.csv\" dataset_path=\"pj-upload://1/other.csv\"/>"_s,
      {a_run_, b_run_, other_});
  EXPECT_EQ(by_id[u"gone"_s].attribute(u"dataset_path"_s), other_);
  EXPECT_FALSE(by_id[u"two_namesakes"_s].hasAttribute(u"dataset_path"_s));
  EXPECT_FALSE(by_id[u"two_namesakes"_s].hasAttribute(u"dataset_source"_s));
  EXPECT_EQ(by_id[u"by_label"_s].attribute(u"dataset_path"_s), other_);
  EXPECT_FALSE(by_id[u"opaque"_s].hasAttribute(u"dataset_path"_s));
  EXPECT_FALSE(by_id[u"opaque"_s].hasAttribute(u"dataset_source"_s));

  // d/left.csv and d/right.csv are still on disk; e/left.csv takes left by name whatever the order, and right pairs
  // with the file left over.
  by_id = rewrite(
      u"<curve id=\"left\" dataset_source=\"left.csv\" dataset_path=\"%1\"/>"
      u"<curve id=\"right\" dataset_source=\"right.csv\" dataset_path=\"%2\"/>"_s.arg(
          path(u"d/left.csv"_s), path(u"d/right.csv"_s)),
      {path(u"e/right_new.csv"_s), path(u"e/left.csv"_s)});
  EXPECT_EQ(by_id[u"left"_s].attribute(u"dataset_path"_s), path(u"e/left.csv"_s));
  EXPECT_EQ(by_id[u"right"_s].attribute(u"dataset_path"_s), path(u"e/right_new.csv"_s));

  // a/run.csv is claimed by the qualifier that names it; a missing namesake does not take it too.
  by_id = rewrite(
      u"<curve id=\"run_a\" dataset_source=\"run.csv\" dataset_path=\"%1\"/>"
      u"<curve id=\"gone\" dataset_source=\"run.csv\" dataset_path=\"/elsewhere/run.csv\"/>"_s.arg(a_run_),
      {a_run_});
  EXPECT_EQ(by_id[u"run_a"_s].attribute(u"dataset_path"_s), a_run_);
  EXPECT_FALSE(by_id[u"gone"_s].hasAttribute(u"dataset_path"_s));
}

// What no path or name places pairs with the files left, in order, when the counts agree: the layout's <fileInfo>
// order first, then first appearance. A path-less label that a saved path carries follows that path.
TEST_F(KeptFileQualifiers, LeftoverDatasetsPairWithLeftoverFilesInOrder) {
  const QString gone_a = path(u"gone/a.csv"_s);
  const QString gone_b = path(u"gone/b.csv"_s);
  const QString elements =
      u"<curve id=\"a\" dataset_source=\"a.csv\" dataset_path=\"%1\"/>"
      u"<curve id=\"b\" dataset_source=\"b.csv\" dataset_path=\"%2\"/>"
      u"<generator id=\"a_label\" dataset_source=\"a.csv\"/>"_s.arg(gone_a, gone_b);

  auto by_id = rewrite(elements, {other_, a_run_});
  EXPECT_EQ(by_id[u"a"_s].attribute(u"dataset_path"_s), other_);
  EXPECT_EQ(by_id[u"a"_s].attribute(u"dataset_source"_s), u"other.csv"_s);
  EXPECT_EQ(by_id[u"b"_s].attribute(u"dataset_path"_s), a_run_);
  EXPECT_EQ(by_id[u"a_label"_s].attribute(u"dataset_path"_s), other_);
  EXPECT_EQ(by_id[u"a_label"_s].attribute(u"dataset_source"_s), u"other.csv"_s);
  ASSERT_EQ(matches_.size(), 3);
  EXPECT_EQ(matches_[0].label, u"a.csv"_s);
  EXPECT_EQ(matches_[0].rule, PJ::layout_xml::DatasetFileMatch::Rule::kPosition);
  EXPECT_EQ(matches_[2].rule, PJ::layout_xml::DatasetFileMatch::Rule::kAlias);

  by_id = rewrite(elements, {other_, a_run_}, {gone_b, gone_a});
  EXPECT_EQ(by_id[u"b"_s].attribute(u"dataset_path"_s), other_) << "<fileInfo> order wins";
  EXPECT_EQ(by_id[u"a"_s].attribute(u"dataset_path"_s), a_run_);

  by_id = rewrite(elements, {other_, a_run_, b_run_});
  for (const QString& id : {u"a"_s, u"b"_s, u"a_label"_s}) {
    EXPECT_FALSE(by_id[id].hasAttribute(u"dataset_path"_s)) << "three files for two datasets: " << id.toStdString();
    EXPECT_FALSE(by_id[id].hasAttribute(u"dataset_source"_s));
  }
}

// Positional pairing stays within one format: a recording and a CSV never trade places, and a format whose counts
// differ is stripped while another still pairs.
TEST_F(KeptFileQualifiers, PositionalPairingStaysWithinOneFormat) {
  const QString saved =
      u"<curve id=\"robot\" dataset_source=\"robot.mcap\" dataset_path=\"%1\"/>"
      u"<curve id=\"gps\" dataset_source=\"gps.csv\" dataset_path=\"%2\"/>"_s.arg(
          path(u"gone/robot.mcap"_s), path(u"gone/gps.csv"_s));
  auto by_id = rewrite(saved, {path(u"e/gps_new.csv"_s), path(u"e/robot_new.mcap"_s)});
  EXPECT_EQ(by_id[u"robot"_s].attribute(u"dataset_path"_s), path(u"e/robot_new.mcap"_s));
  EXPECT_EQ(by_id[u"robot"_s].attribute(u"dataset_source"_s), u"robot_new.mcap"_s);
  EXPECT_EQ(by_id[u"gps"_s].attribute(u"dataset_path"_s), path(u"e/gps_new.csv"_s));

  by_id = rewrite(
      saved + u"<curve id=\"imu\" dataset_source=\"imu.CSV\" dataset_path=\"%1\"/>"_s.arg(path(u"gone/imu.CSV"_s)),
      {path(u"e/gps_new.csv"_s), path(u"e/robot_new.mcap"_s)});
  EXPECT_EQ(by_id[u"robot"_s].attribute(u"dataset_path"_s), path(u"e/robot_new.mcap"_s));
  EXPECT_FALSE(by_id[u"gps"_s].hasAttribute(u"dataset_path"_s)) << "two CSV datasets (any case), one CSV file";
  EXPECT_FALSE(by_id[u"imu"_s].hasAttribute(u"dataset_path"_s));
}

// A label-only identity (a toolbox's own source) is matched by name only: it neither pairs nor counts.
TEST_F(KeptFileQualifiers, LabelOnlyDatasetNeverPairsByPosition) {
  const auto by_id = rewrite(
      u"<curve id=\"a\" dataset_source=\"a.csv\" dataset_path=\"%1\"/>"
      u"<curve id=\"b\" dataset_source=\"b.csv\" dataset_path=\"%2\"/>"
      u"<generator id=\"toolbox\" dataset_source=\"Toolbox X\"/>"_s.arg(path(u"gone/a.csv"_s), path(u"gone/b.csv"_s)),
      {other_, a_run_});
  EXPECT_EQ(by_id[u"a"_s].attribute(u"dataset_path"_s), other_);
  EXPECT_EQ(by_id[u"b"_s].attribute(u"dataset_path"_s), a_run_);
  EXPECT_FALSE(by_id[u"toolbox"_s].hasAttribute(u"dataset_path"_s));
  EXPECT_FALSE(by_id[u"toolbox"_s].hasAttribute(u"dataset_source"_s));
  EXPECT_EQ(matches_.size(), 2);
}

// A file name with several dots: the plain label is the file name, the fan-out prefix its complete base name.
TEST_F(KeptFileQualifiers, RelabelKeepsEveryDotOfTheFileName) {
  const auto by_id = rewrite(
      u"<input id=\"plain\" dataset_source=\"run.v2.csv\" dataset_path=\"%1\"/>"
      u"<input id=\"fanout\" dataset_source=\"run.v2/imu\" dataset_path=\"%1\"/>"_s.arg(path(u"gone/run.v2.csv"_s)),
      {other_});
  EXPECT_EQ(by_id[u"plain"_s].attribute(u"dataset_source"_s), u"other.csv"_s);
  EXPECT_EQ(by_id[u"fanout"_s].attribute(u"dataset_source"_s), u"other/imu"_s);
}

TEST(LayoutXmlTest, StreamingBlockShapeAndWriterValidation) {
  using PJ::layout_xml::StreamingSourceRef;
  // Hand-authored presets often omit pj4_version. Stream records only exist from v5, so their
  // timeline offset never takes the pre-v4 migration that an unversioned <fileInfo> does.
  QString error;
  const QList<StreamingSourceRef> unversioned = PJ::layout_xml::extractStreamingSources(
      parseLayout(
          R"(<root binding="source"><previouslyLoaded_StreamingSources><streamInfo id="robot">)"
          R"(<plugin ID="Stream Plugin"><![CDATA[{"url":"ws://a"}]]></plugin>)"
          R"(<dataset display_offset_ns="-5"/></streamInfo></previouslyLoaded_StreamingSources></root>)"),
      &error);
  ASSERT_EQ(unversioned.size(), 1) << error.toStdString();
  EXPECT_TRUE(unversioned.front().dataset.has_display_offset);
  EXPECT_EQ(unversioned.front().dataset.display_offset_ns, -5);
  EXPECT_FALSE(unversioned.front().dataset.display_offset_includes_global_reference);

  // A second block, or a second plugin in one record, rejects everything like any invalid record.
  const auto record = [](const char* id) {
    return QString::fromLatin1(R"(<streamInfo id="%1"><plugin ID="Stream Plugin"><![CDATA[{}]]></plugin></streamInfo>)")
        .arg(QLatin1StringView(id));
  };
  const QString two_blocks =
      u"<root pj4_version=\"5\"><previouslyLoaded_StreamingSources>%1</previouslyLoaded_StreamingSources>"
      u"<previouslyLoaded_StreamingSources>%2</previouslyLoaded_StreamingSources></root>"_s.arg(
          record("robot"), record("arm"));
  const QString two_plugins =
      uR"(<root pj4_version="5"><previouslyLoaded_StreamingSources><streamInfo id="robot">)"
      uR"(<plugin ID="Stream Plugin"><![CDATA[{}]]></plugin><plugin ID="Other Plugin"><![CDATA[{}]]></plugin>)"
      uR"(</streamInfo></previouslyLoaded_StreamingSources></root>)"_s;
  for (const QString& xml : {two_blocks, two_plugins}) {
    error.clear();
    EXPECT_TRUE(PJ::layout_xml::extractStreamingSources(parseLayout(xml.toUtf8()), &error).isEmpty())
        << xml.toStdString();
    EXPECT_FALSE(error.isEmpty()) << xml.toStdString();
  }

  // The writer refuses what its reader would reject, and then writes nothing.
  StreamingSourceRef valid;
  valid.id = u"robot"_s;
  valid.plugin_id = u"Stream Plugin"_s;
  valid.plugin_config_json = uR"({"url":"ws://a"})"_s;
  StreamingSourceRef no_id = valid;
  no_id.id.clear();
  StreamingSourceRef no_plugin = valid;
  no_plugin.plugin_id.clear();
  StreamingSourceRef no_config = valid;
  no_config.plugin_config_json = u"  "_s;
  const QList<QList<StreamingSourceRef>> refused{{no_id}, {valid, valid}, {no_plugin}, {no_config}};
  for (const QList<StreamingSourceRef>& sources : refused) {
    QDomDocument doc = parseLayout(R"(<root pj4_version="4" binding="source"/>)");
    error.clear();
    EXPECT_FALSE(PJ::layout_xml::writeStreamingSources(doc, sources, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(doc.documentElement().hasChildNodes());
    EXPECT_EQ(doc.documentElement().attribute(u"pj4_version"_s), u"4"_s);
  }
  // A second write would leave two blocks, which the reader rejects.
  QDomDocument doc = parseLayout(R"(<root pj4_version="4" binding="source"/>)");
  ASSERT_TRUE(PJ::layout_xml::writeStreamingSources(doc, {valid}, &error)) << error.toStdString();
  StreamingSourceRef arm = valid;
  arm.id = u"arm"_s;
  error.clear();
  EXPECT_FALSE(PJ::layout_xml::writeStreamingSources(doc, {arm}, &error));
  EXPECT_FALSE(error.isEmpty());
  EXPECT_EQ(doc.elementsByTagName(u"previouslyLoaded_StreamingSources"_s).size(), 1);
  EXPECT_EQ(PJ::layout_xml::extractStreamingSources(doc, &error).size(), 1) << error.toStdString();
}

// ---------- One layout over several datasets ----------------------------
// Intended behaviour (pj_app ARCHITECTURE "One layout over several datasets"): a layout aimed at one dataset is pinned
// to the dataset(s) the user chose, and several choices duplicate its tabs, one copy per dataset, each copy pinned to
// its own. A test failing here is a contract break, not a stale expectation.

// A generic layout (no qualifier anywhere): a plot with a coloured and an uncoloured curve, an XY curve, a scene layer
// and a local one, a Custom Widget wire, a filter over a transform output, a transform chain, a dataset-bound and an
// all-datasets marker rule. Without transforms, "speed" is a raw topic.
QDomDocument genericDerivedLayout(bool with_transforms = true) {
  QDomDocument doc;
  EXPECT_TRUE(
      static_cast<bool>(doc.setContent(QByteArrayLiteral(
          R"(<root pj4_version="4" binding="generic">
<tabbed_widget id="" name="main" parent="main_window">
 <Tab id="t1" tab_name="Plots" owner_plugin="tb" tab_id="x"><Container><DockSplitter count="1" orientation="-" sizes="1">
  <DockArea id="a1" name="p"><plot id="p1" mode="TimeSeries">
   <curve topic="sensor" field="value" color="#d62728"/>
   <curve topic="speed" field="value"/>
   <curve x_topic="sensor" x_field="value" y_topic="speed" y_field="value"/>
   <marker_scope scope="global" visible="false"/>
  </plot></DockArea></DockSplitter></Container></Tab>
 <Tab id="t2" tab_name="Scene"><Container><DockSplitter count="2" orientation="-" sizes="0.5;0.5">
  <DockArea id="a2" name="s"><scene3d><layer topic_name="/points" object_type="kPointCloud"/>
   <layer local="true" dataset_id="0" topic_name=""/></scene3d></DockArea>
  <DockArea id="a3" name="w"><CustomWidget plugin="gauge"><input name="value" slot="0" topic="speed" field="value"/>
  </CustomWidget></DockArea></DockSplitter></Container></Tab>
 <currentTabIndex index="1"/>
</tabbed_widget>
<data_processors>
 <transform owner_plugin="tb" id="kmh"><input name="sensor" topic="sensor" field="value"/><output name="speed"/></transform>
 <transform owner_plugin="tb" id="twice"><input name="speed" topic="speed" field="value"/><output name="speed2"/></transform>
 <processor input_topic="speed" input_field="value" output_name="speed_abs" processor_id="absolute"/>
 <generator id="tb/rule/sensor" all_datasets="0"><input name="sensor/value"/><output name="m"/></generator>
 <generator id="tb/rule/all" all_datasets="1"><input name="sensor/value"/><output name="__all__"/></generator>
</data_processors>
</root>)"))));
  for (QDomElement transform =
           doc.documentElement().firstChildElement(u"data_processors"_s).firstChildElement(u"transform"_s);
       !with_transforms && !transform.isNull();
       transform = doc.documentElement().firstChildElement(u"data_processors"_s).firstChildElement(u"transform"_s)) {
    transform.parentNode().removeChild(transform);
  }
  return doc;
}

const PJ::layout_xml::DatasetPin kRunA{
    .id = 3, .source = u"run.csv"_s, .path = u"/d/a/run.csv"_s, .label = u"run.csv"_s};
const PJ::layout_xml::DatasetPin kRunB{
    .id = 4, .source = u"run.csv"_s, .path = u"/d/b/run.csv"_s, .label = u"run.csv (2)"_s};

QList<QDomElement> elementsNamed(const QDomDocument& doc, const QString& tag) {
  QList<QDomElement> found;
  const QDomNodeList nodes = doc.elementsByTagName(tag);
  for (int index = 0; index < nodes.size(); ++index) {
    found.push_back(nodes.at(index).toElement());
  }
  return found;
}

using PJ::layout_xml::DatasetIdentity;

TEST(LayoutDatasetChoice, IdentitiesCountAPathlessAliasOnce) {
  const QDomDocument doc = parseLayout(
      R"(<root><curve topic="a" dataset_source="run.csv" dataset_path="/d/run.csv"/>
<layer topic_name="b" dataset_source="run.csv"/><curve topic="c" dataset_source="other.csv"/>
<curve topic="d" dataset_path="/d/run.csv"/></root>)");
  EXPECT_EQ(
      PJ::layout_xml::datasetIdentities(doc),
      (QList<DatasetIdentity>{{u"/d/run.csv"_s, u"run.csv"_s}, {QString(), u"other.csv"_s}}));
  EXPECT_TRUE(PJ::layout_xml::datasetIdentities(genericDerivedLayout()).isEmpty());
}

// The entries of one fan-out file share its path: each label is a dataset of its own.
TEST(LayoutDatasetChoice, IdentitiesKeepEachFanOutEntry) {
  const QDomDocument doc = parseLayout(
      R"(<root><curve topic="a" dataset_source="log/left" dataset_path="/d/log.mcap"/>
<curve topic="a" dataset_source="log/right" dataset_path="/d/log.mcap"/></root>)");
  EXPECT_EQ(
      PJ::layout_xml::datasetIdentities(doc),
      (QList<DatasetIdentity>{{u"/d/log.mcap"_s, u"log/left"_s}, {u"/d/log.mcap"_s, u"log/right"_s}}));
}

// The layout's own outputs are not topics it needs from the data: a dataset holding them from an earlier load fits no
// better.
TEST(LayoutDatasetChoice, ReferencedTopicsLeaveOutTheLayoutsOwnOutputs) {
  EXPECT_EQ(PJ::layout_xml::referencedTopics(genericDerivedLayout()), (QSet<QString>{u"sensor"_s, u"/points"_s}));
  EXPECT_EQ(
      PJ::layout_xml::referencedTopics(genericDerivedLayout(/*with_transforms=*/false)),
      (QSet<QString>{u"sensor"_s, u"speed"_s, u"/points"_s}));
}

// One dataset chosen: every element that reads data names it, the generic ones included; an all-datasets rule and a
// local scene layer stay as they were.
TEST(LayoutDatasetChoice, OnePinQualifiesEveryReader) {
  QDomDocument doc = genericDerivedLayout();
  PJ::layout_xml::pinLayoutToDatasets(doc, {kRunB});
  int pinned = 0;
  for (const QString& tag : {u"curve"_s, u"layer"_s, u"input"_s, u"processor"_s, u"generator"_s, u"marker_scope"_s}) {
    for (const QDomElement& element : elementsNamed(doc, tag)) {
      const bool local = element.attribute(u"local"_s) == u"true"_s;
      const bool all = element.attribute(u"all_datasets"_s) == u"1"_s;
      const bool rule_input = element.parentNode().toElement().tagName() == u"generator"_s;
      const QString prefix = tag == u"processor"_s                ? u"input_"_s
                             : element.hasAttribute(u"x_topic"_s) ? u"x_"_s
                                                                  : QString();
      if (local || all || rule_input) {
        EXPECT_FALSE(element.hasAttribute(prefix + u"dataset_path"_s)) << tag.toStdString();
        continue;
      }
      EXPECT_EQ(element.attribute(prefix + u"dataset_path"_s), kRunB.path) << tag.toStdString();
      EXPECT_EQ(element.attribute(prefix + u"dataset_source"_s), kRunB.source) << tag.toStdString();
      EXPECT_EQ(element.attribute(prefix + u"dataset_id"_s), u"4"_s) << tag.toStdString();
      ++pinned;
    }
  }
  EXPECT_EQ(pinned, 10);
  EXPECT_EQ(elementsNamed(doc, u"curve"_s)[2].attribute(u"y_dataset_path"_s), kRunB.path);
  EXPECT_EQ(elementsNamed(doc, u"Tab"_s).size(), 2) << "one dataset: no copy";
}

// A layout already naming another dataset is moved to the chosen one, not kept.
TEST(LayoutDatasetChoice, OnePinReplacesTheSavedDataset) {
  QDomDocument doc;
  ASSERT_TRUE(
      static_cast<bool>(doc.setContent(QByteArrayLiteral(
          R"(<root><plot><curve topic="a" dataset_id="9" dataset_source="old.csv" dataset_path="/old.csv"/></plot></root>)"))));
  PJ::layout_xml::pinLayoutToDatasets(doc, {kRunA});
  const QDomElement curve = elementsNamed(doc, u"curve"_s).front();
  EXPECT_EQ(curve.attribute(u"dataset_path"_s), kRunA.path);
  EXPECT_EQ(curve.attribute(u"dataset_id"_s), u"3"_s);
}

// Two datasets chosen: each tab is followed by its copy for the second dataset; names say which dataset each shows,
// ids stay unique, the plugin's ownership stays with the first, and the current tab is the first copy of the saved one.
TEST(LayoutDatasetChoice, TwoPinsDuplicateEachTabSideBySide) {
  QDomDocument doc = genericDerivedLayout(/*with_transforms=*/false);
  PJ::layout_xml::pinLayoutToDatasets(doc, {kRunA, kRunB});
  const QList<QDomElement> tabs = elementsNamed(doc, u"Tab"_s);
  ASSERT_EQ(tabs.size(), 4);
  QStringList names;
  QStringList ids;
  for (const QDomElement& tab : tabs) {
    names.push_back(tab.attribute(u"tab_name"_s));
    ids.push_back(tab.attribute(u"id"_s));
  }
  EXPECT_EQ(
      names,
      (QStringList{u"Plots [run.csv]"_s, u"Plots [run.csv (2)]"_s, u"Scene [run.csv]"_s, u"Scene [run.csv (2)]"_s}));
  EXPECT_EQ(ids, (QStringList{u"t1"_s, u"t1~2"_s, u"t2"_s, u"t2~2"_s}));
  EXPECT_EQ(tabs[0].attribute(u"owner_plugin"_s), u"tb"_s);
  EXPECT_FALSE(tabs[1].hasAttribute(u"owner_plugin"_s));
  EXPECT_FALSE(tabs[1].hasAttribute(u"tab_id"_s));
  EXPECT_EQ(tabs[1].elementsByTagName(u"plot"_s).at(0).toElement().attribute(u"id"_s), u"p1~2"_s);
  EXPECT_EQ(tabs[3].elementsByTagName(u"DockArea"_s).at(1).toElement().attribute(u"id"_s), u"a3~2"_s);
  EXPECT_EQ(elementsNamed(doc, u"currentTabIndex"_s).front().attribute(u"index"_s), u"2"_s);

  for (int copy = 0; copy < 4; ++copy) {
    const QString path = copy % 2 == 0 ? kRunA.path : kRunB.path;
    QDomDocument tab_doc;
    tab_doc.appendChild(tab_doc.importNode(tabs[copy], true));
    for (const QString& tag : {u"layer"_s, u"input"_s}) {
      for (const QDomElement& element : elementsNamed(tab_doc, tag)) {
        if (element.attribute(u"local"_s) != u"true"_s) {
          EXPECT_EQ(element.attribute(u"dataset_path"_s), path) << copy << tag.toStdString();
        }
      }
    }
  }
}

// Each filter and dataset-bound marker rule gets a copy per dataset, pinned to it. Every copy keeps the saved id or
// output name, whatever the dataset label holds ('#' separates a marker owner): a marker rule binding and a filter
// output are both unique per dataset. Topics keep their names in every copy.
TEST(LayoutDatasetChoice, TwoPinsCopyEachDatasetBoundRecipe) {
  QDomDocument doc = genericDerivedLayout(/*with_transforms=*/false);
  PJ::layout_xml::DatasetPin hashed = kRunB;
  hashed.label = u"run#2.csv"_s;
  PJ::layout_xml::pinLayoutToDatasets(doc, {kRunA, hashed});
  QStringList recipes;
  for (QDomElement recipe = doc.documentElement().firstChildElement(u"data_processors"_s).firstChildElement();
       !recipe.isNull(); recipe = recipe.nextSiblingElement()) {
    const QString name =
        recipe.tagName() == u"processor"_s ? recipe.attribute(u"output_name"_s) : recipe.attribute(u"id"_s);
    recipes.push_back(name + u'@' + recipe.attribute(u"dataset_path"_s) + recipe.attribute(u"input_dataset_path"_s));
  }
  EXPECT_EQ(
      recipes, (QStringList{
                   u"speed_abs@/d/a/run.csv"_s, u"speed_abs@/d/b/run.csv"_s, u"tb/rule/sensor@/d/a/run.csv"_s,
                   u"tb/rule/sensor@/d/b/run.csv"_s, u"tb/rule/all@"_s}));
  const QList<QDomElement> tabs = elementsNamed(doc, u"Tab"_s);
  QDomDocument tab_doc;
  tab_doc.appendChild(tab_doc.importNode(tabs[1], true));
  EXPECT_EQ(elementsNamed(tab_doc, u"curve"_s)[1].attribute(u"topic"_s), u"speed"_s);
  QDomDocument wire_doc;
  wire_doc.appendChild(wire_doc.importNode(tabs[3], true));
  EXPECT_EQ(elementsNamed(wire_doc, u"input"_s).front().attribute(u"name"_s), u"value"_s) << "a wire's slot name";
}

// A duplicated layout saved generic and loaded again over two datasets duplicates again: every tab, area and plot id
// stays unique in the document, or a restore would merge two copies into one.
TEST(LayoutDatasetChoice, RepinningACopiedLayoutKeepsEveryIdUnique) {
  QDomDocument doc = genericDerivedLayout(/*with_transforms=*/false);
  PJ::layout_xml::pinLayoutToDatasets(doc, {kRunA, kRunB});
  PJ::layout_xml::removeDatasetQualifiersForGenericLayout(doc);
  PJ::layout_xml::pinLayoutToDatasets(doc, {kRunA, kRunB});
  ASSERT_EQ(elementsNamed(doc, u"Tab"_s).size(), 8);
  for (const QString& tag : {u"Tab"_s, u"DockArea"_s, u"plot"_s}) {
    QStringList ids;
    for (const QDomElement& element : elementsNamed(doc, tag)) {
      ids.push_back(element.attribute(u"id"_s));
    }
    EXPECT_EQ(QSet<QString>(ids.begin(), ids.end()).size(), ids.size()) << ids.join(u' ').toStdString();
    EXPECT_FALSE(ids.contains(QString())) << tag.toStdString();
  }
}

// The copies exist to compare runs: a curve shows the same colour in every copy, whether the layout saved one, the
// session remembers one for it, or a palette colour was picked for it (from its position, the first its plot does not
// already wear).
TEST(LayoutDatasetChoice, EveryCopyOfACurveHasTheSameColour) {
  QDomDocument doc = genericDerivedLayout(/*with_transforms=*/false);
  PJ::layout_xml::pinLayoutToDatasets(doc, {kRunA, kRunB}, [](const QDomElement& curve) -> std::optional<QColor> {
    if (curve.attribute(u"topic"_s) == u"speed"_s) {
      return QColor(u"#9467bd"_s);
    }
    return std::nullopt;
  });
  const QList<QDomElement> plots = elementsNamed(doc, u"plot"_s);
  ASSERT_EQ(plots.size(), 2);
  QStringList colours[2];
  for (int copy = 0; copy < 2; ++copy) {
    for (QDomElement curve = plots[copy].firstChildElement(u"curve"_s); !curve.isNull();
         curve = curve.nextSiblingElement(u"curve"_s)) {
      colours[copy].push_back(curve.attribute(u"color"_s));
    }
  }
  EXPECT_EQ(colours[0], (QStringList{u"#d62728"_s, u"#9467bd"_s, u"#1ac938"_s}));
  EXPECT_EQ(colours[0], colours[1]);
}

// The best-fitting dataset takes the references to the topics it carries, and the nameless readers; a reference to a
// topic it lacks loses its qualifier and binds wherever that topic is unique.
TEST(LayoutDatasetChoice, OnePinOverSomeTopicsLeavesTheOthersUnqualified) {
  QDomDocument doc = parseLayout(
      R"(<root><plot><curve topic="sensor" field="value" dataset_source="old.csv"/>
<curve topic="speed" field="value" dataset_source="old.csv"/></plot></root>)");
  PJ::layout_xml::pinLayoutToDataset(doc, kRunB, {u"sensor"_s});
  const QList<QDomElement> curves = elementsNamed(doc, u"curve"_s);
  EXPECT_EQ(curves[0].attribute(u"dataset_path"_s), kRunB.path);
  EXPECT_FALSE(curves[1].hasAttribute(u"dataset_source"_s));
}

// A marker rule follows its inputs: pinned to the best-fitting dataset when it reads one of the topics that dataset
// takes, unqualified otherwise, so it binds by its inputs wherever they are.
TEST(LayoutDatasetChoice, OnePinTakesOnlyTheMarkerRulesReadingItsTopics) {
  QDomDocument doc = parseLayout(
      R"(<root><plot><curve topic="sensor" field="value"/></plot><data_processors>
<generator id="sensor_rule"><input name="sensor/value"/></generator>
<generator id="battery_rule" dataset_source="old.csv"><input name="battery/voltage"/></generator>
<generator id="sensor_prefix_rule"><input name="sensor_raw/value"/></generator>
</data_processors></root>)");
  PJ::layout_xml::pinLayoutToDataset(doc, kRunB, {u"sensor"_s});
  const QList<QDomElement> rules = elementsNamed(doc, u"generator"_s);
  ASSERT_EQ(rules.size(), 3);
  EXPECT_EQ(rules[0].attribute(u"dataset_path"_s), kRunB.path);
  for (const QDomElement& rule : {rules[1], rules[2]}) {
    EXPECT_FALSE(rule.hasAttribute(u"dataset_source"_s)) << rule.attribute(u"id"_s).toStdString();
    EXPECT_FALSE(rule.hasAttribute(u"dataset_path"_s)) << rule.attribute(u"id"_s).toStdString();
  }
}

// ---------- planDatasetChoice ----------

// Loaded datasets: each pin with the topics it carries.
PJ::layout_xml::LoadedDatasets loadedWith(
    const QList<std::pair<PJ::layout_xml::DatasetPin, QStringList>>& datasets,
    std::function<bool(const DatasetIdentity&)> names_one = [](const DatasetIdentity&) { return false; }) {
  PJ::layout_xml::LoadedDatasets loaded;
  for (const auto& [pin, topics] : datasets) {
    loaded.datasets.push_back(pin);
  }
  loaded.for_each_topic = [datasets](const std::function<void(std::uint32_t, const QString&)>& visit) {
    for (const auto& [pin, topics] : datasets) {
      for (const QString& topic : topics) {
        visit(pin.id, topic);
      }
    }
  };
  loaded.names_one = std::move(names_one);
  return loaded;
}

using Kind = PJ::layout_xml::DatasetChoice::Kind;

TEST(PlanDatasetChoice, DatasetsFittingEquallyAreAskedAbout) {
  const auto choice = PJ::layout_xml::planDatasetChoice(
      genericDerivedLayout(), std::nullopt,
      loadedWith({{kRunA, {u"sensor"_s, u"/points"_s}}, {kRunB, {u"sensor"_s, u"/points"_s, u"other"_s}}}));
  EXPECT_EQ(choice.kind, Kind::kAsk);
  ASSERT_EQ(choice.datasets.size(), 2);
  EXPECT_EQ(choice.datasets[1].path, kRunB.path);
  EXPECT_EQ(choice.blockers, (QStringList{u"tb/kmh"_s, u"tb/twice"_s}));
}

TEST(PlanDatasetChoice, TheBestFittingDatasetIsPinnedOverTheTopicsItCarries) {
  const auto choice = PJ::layout_xml::planDatasetChoice(
      genericDerivedLayout(), std::nullopt, loadedWith({{kRunA, {u"sensor"_s}}, {kRunB, {u"sensor"_s, u"/points"_s}}}));
  EXPECT_EQ(choice.kind, Kind::kPin);
  ASSERT_EQ(choice.datasets.size(), 1);
  EXPECT_EQ(choice.datasets.front().path, kRunB.path);
  EXPECT_EQ(choice.topics, (QSet<QString>{u"sensor"_s, u"/points"_s}));
}

TEST(PlanDatasetChoice, ADatasetHoldingTheLayoutsOutputsFitsNoBetter) {
  const auto choice = PJ::layout_xml::planDatasetChoice(
      genericDerivedLayout(), std::nullopt,
      loadedWith(
          {{kRunA, {u"sensor"_s, u"/points"_s, u"speed"_s, u"speed2"_s, u"speed_abs"_s}},
           {kRunB, {u"sensor"_s, u"/points"_s}}}));
  EXPECT_EQ(choice.kind, Kind::kAsk);
}

TEST(PlanDatasetChoice, KeepsWhatNeedsNoChoice) {
  const auto plan = [](const QByteArray& xml, const std::optional<QList<DatasetIdentity>>& saved = std::nullopt,
                       bool loaded_one = false) {
    return PJ::layout_xml::planDatasetChoice(
               parseLayout(xml), saved,
               loadedWith(
                   {{kRunA, {u"a"_s}}, {kRunB, {u"a"_s}}}, [loaded_one](const DatasetIdentity&) { return loaded_one; }))
        .kind;
  };
  EXPECT_EQ(plan(R"(<root><curve topic="a"/></root>)"), Kind::kAsk);
  EXPECT_EQ(plan(R"(<root><curve topic="z"/></root>)"), Kind::kKeep) << "no loaded dataset carries it";
  EXPECT_EQ(plan(R"(<root><plot/></root>)"), Kind::kKeep) << "no topic";
  EXPECT_EQ(plan(R"(<root><curve topic="a" dataset_path="/d/x.csv"/></root>)", std::nullopt, true), Kind::kKeep)
      << "its dataset is loaded";
  EXPECT_EQ(
      plan(R"(<root><curve topic="a" dataset_source="log/left" dataset_path="/d/log.mcap"/>
<curve topic="a" dataset_source="log/right" dataset_path="/d/log.mcap"/></root>)"),
      Kind::kKeep)
      << "two fan-out entries";
  EXPECT_EQ(plan(R"(<root><curve topic="a" dataset_path="pj-stream://abc"/></root>)"), Kind::kKeep) << "a stream";
  EXPECT_EQ(
      plan(R"(<root><curve topic="a"/></root>)", QList<DatasetIdentity>{{u"/x"_s, {}}, {u"/y"_s, {}}}), Kind::kKeep)
      << "the file named two datasets";
}

// Duplication is refused, never guessed, where a copy cannot work: a transform's id is its script's class id, so a
// second copy under the same owner cannot exist, and an on-demand recipe reads its inputs by name.
TEST(LayoutDatasetChoice, DuplicationBlockersNameWhatCannotBeCopied) {
  EXPECT_EQ(PJ::layout_xml::duplicationBlockers(genericDerivedLayout()), (QStringList{u"tb/kmh"_s, u"tb/twice"_s}));
  QDomDocument doc = genericDerivedLayout(/*with_transforms=*/false);
  EXPECT_TRUE(PJ::layout_xml::duplicationBlockers(doc).isEmpty());
  QDomElement on_demand = doc.createElement(u"on_demand"_s);
  on_demand.setAttribute(u"owner_plugin"_s, u"tb"_s);
  on_demand.setAttribute(u"id"_s, u"crop"_s);
  doc.documentElement().firstChildElement(u"data_processors"_s).appendChild(on_demand);
  EXPECT_EQ(PJ::layout_xml::duplicationBlockers(doc), (QStringList{u"tb/crop"_s}));
}

}  // namespace
