// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <cstdint>

#include "LayoutXml.h"
using namespace Qt::StringLiterals;

namespace {

using PJ::layout_xml::DataSourceRef;

// ---------- appendJsonAsCdata ----------------------------------------------

QString roundTripJson(const QString& input) {
  QDomDocument doc;
  QDomElement plugin = doc.createElement(u"plugin"_s);
  PJ::layout_xml::appendJsonAsCdata(doc, plugin, input);
  doc.appendChild(plugin);

  // Round-trip through serialize -> reparse to confirm the document
  // survives the actual XML pipeline (not just text() of the in-memory
  // DOM, which would only test our writer, not the reader).
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
  // Tests that QDomDocument doesn't choke on <, >, &, unicode within CDATA.
  const QString in = uR"({"label":"<x & y> ünïcødé"})"_s;
  EXPECT_EQ(roundTripJson(in), in);
}

// ---------- directCdataText (params vs <source_fallback> separation) --------
//
// A <processor> layout element (M7) carries its params as a DIRECT CDATA child
// AND an optional <source_fallback> CHILD ELEMENT holding the filter's Luau
// source. QDomElement::text() recurses the whole subtree, so reading params via
// processor.text() would slurp the embedded source in too. directCdataText reads
// only the element's own direct CDATA, keeping the two payloads independent.

QDomElement buildProcessorWithSource(QDomDocument& doc, const QString& params, const QString& source) {
  QDomElement processor = doc.createElement(u"processor"_s);
  PJ::layout_xml::appendJsonAsCdata(doc, processor, params);  // params: a direct CDATA child
  QDomElement src = doc.createElement(u"source_fallback"_s);
  PJ::layout_xml::appendJsonAsCdata(doc, src, source);
  processor.appendChild(src);  // source: nested inside a child element
  doc.appendChild(processor);
  return processor;
}

TEST(DirectCdataText, ReadsOwnPayloadIgnoringChildElement) {
  QDomDocument doc;
  const QString params = uR"({"value_scale":2.5})"_s;
  const QString source = u"return { id='scale', create=function(p) end }"_s;
  QDomElement processor = buildProcessorWithSource(doc, params, source);

  EXPECT_EQ(PJ::layout_xml::directCdataText(processor), params);
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
  EXPECT_EQ(PJ::layout_xml::directCdataText(processor), params);
  EXPECT_EQ(processor.firstChildElement(u"source_fallback"_s).text(), source);
}

TEST(DirectCdataText, EmptyWhenNoDirectCdata) {
  QDomDocument doc;
  QDomElement processor = doc.createElement(u"processor"_s);
  QDomElement src = doc.createElement(u"source_fallback"_s);
  PJ::layout_xml::appendJsonAsCdata(doc, src, u"source-only"_s);
  processor.appendChild(src);
  doc.appendChild(processor);
  EXPECT_TRUE(PJ::layout_xml::directCdataText(processor).isEmpty());
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
    PJ::layout_xml::appendJsonAsCdata(doc, plugin, plugin_json);
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
    PJ::layout_xml::appendJsonAsCdata(doc, plugin, plugin_json);
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

  file_info.setAttribute(u"content_sha256"_s, u"ABCDEF"_s);
  refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().content_sha256.isEmpty());

  file_info.removeAttribute(u"content_sha256"_s);
  refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().content_sha256.isEmpty());
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
  EXPECT_FALSE(refs.front().rewrite_plugin_filepath);
}

TEST(ExtractDataSource, LogicalPluginFilepathMarkerOptsIntoResolvedReplayPath) {
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
  EXPECT_TRUE(refs.front().rewrite_plugin_filepath);
}

TEST(ExtractDataSource, PluginCdataWithClosingSequenceRoundTrips) {
  const QString json = uR"({"pat":"weird ]]> in middle"})"_s;
  const QDomDocument doc = buildDataSourceDoc(u"/tmp/x.mcap"_s, QString(), u"CSV"_s, json);
  // Round-trip the WHOLE doc through serialize+reparse to confirm the
  // CDATA splitting survives the actual file pipeline.
  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().plugin_config_json, json);
}

TEST(ExtractDataSource, PluginManifestIdParsedWhenPresentAndEmptyWhenAbsent) {
  // New layouts write BOTH the display name (ID — what old readers keep using)
  // and the stable manifest id (manifest_id) on <plugin>. Old layouts carry
  // only ID; the manifest id field must then stay empty.
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

// Appends a <materialize> child to the doc's only <fileInfo>, mirroring
// MainWindow::appendDataSourceElement's save shape: provider/identity as
// attributes, the canonical descriptor JSON as a CDATA payload written through
// appendJsonAsCdata (so a "]]>"-bearing descriptor splits across sections).
void appendMaterialize(QDomDocument& doc, const QString& provider, const QString& identity, const QString& descriptor) {
  QDomElement file_info =
      doc.documentElement().firstChildElement(u"previouslyLoaded_Datafiles"_s).firstChildElement(u"fileInfo"_s);
  QDomElement materialize = doc.createElement(u"materialize"_s);
  materialize.setAttribute(u"provider"_s, provider);
  materialize.setAttribute(u"identity"_s, identity);
  PJ::layout_xml::appendJsonAsCdata(doc, materialize, descriptor);
  file_info.appendChild(materialize);
}

TEST(ExtractDataSource, MaterializeRecordSurvivesSerializeReparseByteExact) {
  // The descriptor bytes are a cross-repo identity contract: they must come
  // back VERBATIM through the real XML file pipeline, including newlines and
  // the CDATA-hostile "]]>" sequence.
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
  // <materialize> is a SIBLING of <plugin>: the plugin child must parse exactly
  // as before, with no descriptor bytes leaking into its config payload.
  EXPECT_EQ(refs.front().plugin_id, u"MCAP Loader"_s);
  EXPECT_EQ(refs.front().plugin_config_json, plugin_json);
}

TEST(ExtractDataSource, MaterializeAbsentLeavesFieldsEmpty) {
  // Old layouts (no <materialize> child) parse identically: all three fields
  // stay empty and every pre-existing field is untouched.
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
  // Forward/backward tolerance: a genuinely unknown <fileInfo> child changes
  // nothing (the old-reader guarantee <materialize> relies on), and extracting
  // never mutates the document (byte-stable before/after).
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
  // A multi-file session: two distinct files, each with its own plugin config.
  // Host-absolute paths (QDir::tempPath()) — a hardcoded POSIX "/tmp/x" is
  // drive-relative on Windows, so extractDataSource would re-anchor it and the
  // equality check would fail.
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
  // The full file pipeline: build two fileInfos, serialize, reparse, and
  // confirm both come back in order — the round-trip a saved/loaded layout takes.
  // Host-absolute paths so the assertion holds cross-platform (see above).
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

// ---------- Source Timeline state (v3) --------------------------------------

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
  // A pre-v3 layout (no timeline attributes): the reloaded dataset must keep its
  // natural zero offset (has_display_offset=false → caller skips the write) and
  // fall back to load order (timeline_order=-1).
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
  // The realistic path: stamp attrs, serialize to bytes, reparse — the exact
  // round-trip a saved/loaded layout file takes.
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
//
// Codifies the data-source-replay bug: loadLayoutFromPath used to skip the
// reload whenever ANY source was loaded, instead of checking whether the
// currently-loaded source matched the one the layout referenced. A layout
// for file A would then no-op when file B was open, leaving the catalog
// populated with B's keys and every A-key reported as "missing".
// isSamePath() is what gates that decision now.

TEST(IsSamePath, EmptyInputsAreNotSame) {
  EXPECT_FALSE(PJ::layout_xml::isSamePath(QString(), QString()));
  EXPECT_FALSE(PJ::layout_xml::isSamePath(u"/tmp/x"_s, QString()));
  EXPECT_FALSE(PJ::layout_xml::isSamePath(QString(), u"/tmp/x"_s));
}

TEST(IsSamePath, DifferentExistingFilesAreNotSame) {
  // The exact bug scenario: two real files with different basenames.
  // Before the fix, loadLayoutFromPath treated "anything loaded" as
  // "skip" — this test would have passed even when it shouldn't have.
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
  // canonicalFilePath() returns empty for missing files; treating those
  // as "same" would mean two layouts that reference deleted files would
  // skip the reload prompt — the opposite of helpful.
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
  // ':' cannot occur in a Windows path component. A doubled separator after
  // the drive prefix is nevertheless a legal spelling of the same file and
  // still contains the `://` text that the historical broad URI check
  // misclassified.
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
  // Contract is "append only when no extension is specified", so a name the
  // user deliberately gave another suffix is left alone rather than turned
  // into a double extension like notes.txt.pj4.xml.
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
  // Same (topic, field) but distinct dataset qualifiers must remain two entries;
  // collapsing them was the root of the multi-dataset undo bug.
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
  QDomElement& doc_root = pd.plot;  // curves attach to the plot
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
  EXPECT_FALSE(layer.hasAttribute(u"dataset_path"_s)) << "scene layers are not stamped";
  EXPECT_FALSE(robot.hasAttribute(u"source_dataset_path"_s)) << "robot models are not stamped";

  PJ::layout_xml::removeUnvalidatedDatasetIds(pd.doc);
  EXPECT_TRUE(ts.hasAttribute(u"dataset_id"_s));  // path-qualified -> kept
  EXPECT_TRUE(xy.hasAttribute(u"x_dataset_id"_s));
  EXPECT_FALSE(xy.hasAttribute(u"y_dataset_id"_s))  // id-only, no path -> stripped
      << "an unvalidated volatile id must not survive a file save";
  EXPECT_TRUE(processor.hasAttribute(u"input_dataset_id"_s));
  EXPECT_TRUE(transform_input.hasAttribute(u"dataset_id"_s));
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

  PJ::layout_xml::removeDatasetQualifiersForGenericLayout(pd.doc);

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
  // Mirrors SceneDockWidget::xmlSaveState's <layer> shape (id + source, NO path).
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
  // A config-topic (TF) child, also id-qualified, likewise must survive.
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
  // Numeric values are untouched.
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
  // No <range> child.
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

}  // namespace
