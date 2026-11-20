// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "LayoutXml.h"

#include <QCoreApplication>
#include <QDomNodeList>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "FanoutConfig.h"
#include "LoadInput.h"
#include "pj_plotting/PlotXml.h"
#include "pj_runtime/HttpUrl.h"
#include "pj_runtime/XmlCdata.h"

using namespace Qt::StringLiterals;

namespace PJ::layout_xml {

namespace {

constexpr QLatin1StringView kStreamingSourcesTag("previouslyLoaded_StreamingSources");

// A browser upload or stream-record identity: a token, never a filesystem path.
bool isOpaqueSourceIdentity(const QString& value) {
  // Keep it narrow: `:` is legal in POSIX path components, so a relative path such as logs/http://capture.mcap
  // must stay a filesystem path.
  return isBrowserUploadIdentity(value) || isStreamIdentity(value);
}

// One <dataset> timeline child (of a <fileInfo> or a <streamInfo>). `schema_version`
// is the root's pj4_version: below 4, the saved offset includes the global reference.
DataSourceDatasetRef readDatasetRef(const QDomElement& dataset, int schema_version) {
  DataSourceDatasetRef dataset_ref;
  dataset_ref.source_name = dataset.attribute(u"source_name"_s);
  bool source_index_ok = false;
  dataset_ref.source_index = dataset.attribute(u"source_index"_s, u"-1"_s).toInt(&source_index_ok);
  if (!source_index_ok) {
    dataset_ref.source_index = -1;
  }
  if (dataset.hasAttribute(u"display_offset_ns"_s)) {
    bool ok = false;
    dataset_ref.display_offset_ns = dataset.attribute(u"display_offset_ns"_s).toLongLong(&ok);
    dataset_ref.has_display_offset = ok;
    dataset_ref.display_offset_includes_global_reference = ok && schema_version < 4;
  }
  bool dataset_order_ok = false;
  dataset_ref.timeline_order = dataset.attribute(u"timeline_order"_s, u"-1"_s).toInt(&dataset_order_ok);
  if (!dataset_order_ok) {
    dataset_ref.timeline_order = -1;
  }
  return dataset_ref;
}

bool isCanonicalSha256(const QString& value) {
  if (value.size() != 64) {
    return false;
  }
  return std::all_of(
      value.cbegin(), value.cend(), [](QChar ch) { return (ch >= u'0' && ch <= u'9') || (ch >= u'a' && ch <= u'f'); });
}

// An element's optional content_sha256: only canonical lower-case SHA-256 is kept; any other non-empty value
// leaves `sha256` empty and sets `malformed`.
void readContentSha256(const QDomElement& element, QString& sha256, bool& malformed) {
  const QString value = element.attribute(u"content_sha256"_s);
  if (isCanonicalSha256(value)) {
    sha256 = value;
  } else {
    malformed = !value.isEmpty();
  }
}

}  // namespace

QUrl resolveFileSourceUrl(const QString& filename, const QUrl& layout_url, QString* error) {
  const auto fail = [error](const QString& message) {
    if (error != nullptr) {
      *error = message;
    }
    return QUrl();
  };
  if (!isHttpUrl(layout_url)) {
    return fail(
        QCoreApplication::translate("LayoutXml", "Data source '%1' has no web location to load from.").arg(filename));
  }
  if (filename.isEmpty()) {
    return fail(QCoreApplication::translate("LayoutXml", "The layout names a data source without a filename."));
  }
  static const QRegularExpression kDriveLetter(u"^[A-Za-z]:"_s);
  static const QRegularExpression kScheme(u"^[A-Za-z][A-Za-z0-9+.-]*:"_s);
  if (filename.startsWith(u'/') || filename.contains(u'\\') || kDriveLetter.match(filename).hasMatch()) {
    return fail(
        QCoreApplication::translate(
            "LayoutXml", "Data source '%1' is a file on a local machine, which a web layout cannot open.")
            .arg(filename));
  }
  QUrl resolved;
  if (kScheme.match(filename).hasMatch()) {
    resolved = QUrl(filename, QUrl::StrictMode);
    if (!isHttpUrl(resolved)) {
      return fail(QCoreApplication::translate("LayoutXml", "Data source '%1' is not an http(s) URL.").arg(filename));
    }
  } else {
    // A path, as the desktop writes it: '#', '?' and '%' are part of the name.
    QUrl relative;
    relative.setPath(filename, QUrl::DecodedMode);
    resolved = layout_url.resolved(relative);
  }
  if (!resolved.isValid() || resolved.fileName().isEmpty()) {
    return fail(QCoreApplication::translate("LayoutXml", "Data source '%1' does not name a file.").arg(filename));
  }
  if (error != nullptr) {
    error->clear();
  }
  return resolved;
}

QString streamSourceIdentity(const QString& stream_id) {
  return QLatin1StringView(kStreamIdentityScheme) + stream_id;
}

namespace {

// Why `source` cannot be a stream record (empty when it can); records `source.id` in `seen_ids`.
// The one rule both the reader and the writer apply, so neither accepts what the other refuses.
QString streamRecordProblem(const StreamingSourceRef& source, QSet<QString>& seen_ids) {
  if (source.id.isEmpty()) {
    return QCoreApplication::translate("LayoutXml", "The layout names a streaming source without an id.");
  }
  if (seen_ids.contains(source.id)) {
    return QCoreApplication::translate("LayoutXml", "The layout names streaming source '%1' more than once.")
        .arg(source.id);
  }
  seen_ids.insert(source.id);
  if (source.plugin_id.isEmpty() && source.plugin_manifest_id.isEmpty()) {
    return QCoreApplication::translate("LayoutXml", "Streaming source '%1' does not name its plugin.").arg(source.id);
  }
  if (source.plugin_config_json.trimmed().isEmpty()) {
    return QCoreApplication::translate("LayoutXml", "Streaming source '%1' has no plugin configuration.")
        .arg(source.id);
  }
  return {};
}

}  // namespace

QList<StreamingSourceRef> extractStreamingSources(const QDomDocument& doc, QString* error) {
  const auto fail = [error](const QString& message) {
    if (error != nullptr) {
      *error = message;
    }
    return QList<StreamingSourceRef>();
  };
  if (error != nullptr) {
    error->clear();
  }
  const QDomElement wrapper = doc.documentElement().firstChildElement(kStreamingSourcesTag);
  if (!wrapper.nextSiblingElement(kStreamingSourcesTag).isNull()) {
    return fail(QCoreApplication::translate("LayoutXml", "The layout records its streaming sources more than once."));
  }
  QList<StreamingSourceRef> sources;
  QSet<QString> seen_ids;
  for (QDomElement stream_info = wrapper.firstChildElement(u"streamInfo"_s); !stream_info.isNull();
       stream_info = stream_info.nextSiblingElement(u"streamInfo"_s)) {
    StreamingSourceRef source;
    source.id = stream_info.attribute(u"id"_s);
    source.source_name = stream_info.attribute(u"source_name"_s);
    const QDomElement plugin = stream_info.firstChildElement(u"plugin"_s);
    source.plugin_id = plugin.attribute(u"ID"_s);
    source.plugin_manifest_id = plugin.attribute(u"manifest_id"_s);
    source.plugin_config_json = plugin.text();
    if (const QString problem = streamRecordProblem(source, seen_ids); !problem.isEmpty()) {
      return fail(problem);
    }
    if (!plugin.nextSiblingElement(u"plugin"_s).isNull()) {
      return fail(
          QCoreApplication::translate("LayoutXml", "Streaming source '%1' names more than one plugin.").arg(source.id));
    }
    // Stream records exist only from schema 5, so their offset never takes the pre-v4 basis, even in a
    // hand-written layout that omits pj4_version.
    if (const QDomElement dataset = stream_info.firstChildElement(u"dataset"_s); !dataset.isNull()) {
      source.dataset = readDatasetRef(dataset, kStreamingLayoutSchemaVersion);
    }
    sources.push_back(std::move(source));
  }
  return sources;
}

bool writeStreamingSources(QDomDocument& doc, const QList<StreamingSourceRef>& sources, QString* error) {
  const auto fail = [error](const QString& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (error != nullptr) {
    error->clear();
  }
  if (sources.isEmpty()) {
    return true;
  }
  QDomElement root = doc.documentElement();
  if (!root.firstChildElement(kStreamingSourcesTag).isNull()) {
    return fail(QCoreApplication::translate("LayoutXml", "The layout records its streaming sources more than once."));
  }
  QSet<QString> seen_ids;
  for (const StreamingSourceRef& source : sources) {
    if (const QString problem = streamRecordProblem(source, seen_ids); !problem.isEmpty()) {
      return fail(problem);
    }
  }
  QDomElement wrapper = doc.createElement(kStreamingSourcesTag);
  for (const StreamingSourceRef& source : sources) {
    QDomElement stream_info = doc.createElement(u"streamInfo"_s);
    stream_info.setAttribute(u"id"_s, source.id);
    if (!source.source_name.isEmpty()) {
      stream_info.setAttribute(u"source_name"_s, source.source_name);
    }
    QDomElement plugin = doc.createElement(u"plugin"_s);
    plugin.setAttribute(u"ID"_s, source.plugin_id);
    if (!source.plugin_manifest_id.isEmpty()) {
      plugin.setAttribute(u"manifest_id"_s, source.plugin_manifest_id);
    }
    appendJsonAsCdata(doc, plugin, source.plugin_config_json);
    stream_info.appendChild(plugin);

    const DataSourceDatasetRef& state = source.dataset;
    QDomElement dataset = doc.createElement(u"dataset"_s);
    if (!state.source_name.isEmpty()) {
      dataset.setAttribute(u"source_name"_s, state.source_name);
    }
    if (state.source_index >= 0) {
      dataset.setAttribute(u"source_index"_s, QString::number(state.source_index));
    }
    if (state.has_display_offset) {
      dataset.setAttribute(u"display_offset_ns"_s, QString::number(state.display_offset_ns));
    }
    if (state.timeline_order >= 0) {
      dataset.setAttribute(u"timeline_order"_s, QString::number(state.timeline_order));
    }
    if (dataset.hasAttributes()) {
      stream_info.appendChild(dataset);
    }
    wrapper.appendChild(stream_info);
  }
  root.appendChild(wrapper);
  root.setAttribute(u"pj4_version"_s, QString::number(kStreamingLayoutSchemaVersion));
  return true;
}

bool isSourceBoundLayout(const QDomDocument& doc) {
  const QDomElement root = doc.documentElement();
  return root.attribute(u"binding"_s, u"source"_s) != "generic"_L1 &&
         (!root.firstChildElement(kStreamingSourcesTag).isNull() || !extractDataSource(doc, QDir()).isEmpty());
}

QString ensureLayoutExtension(const QString& path) {
  if (path.isEmpty() || !QFileInfo(path).suffix().isEmpty()) {
    return path;
  }
  return path + QLatin1String(kLayoutExtension);
}

bool containsEphemeralBrowserPath(const QByteArray& serialized) {
  return serialized.contains(kBrowserUploadScheme) || serialized.contains("/pj_uploads");
}

std::optional<LogicalSideFiles> logicalBrowserSideFiles(
    std::string_view config, const QString& data_path, const QString& data_logical_path,
    const std::function<std::optional<StagedFileInfo>(const QString&)>& staged_file, QString* error) {
  const auto fail = [error](const QString& reason) -> std::optional<LogicalSideFiles> {
    if (error != nullptr) {
      *error = reason;
    }
    return std::nullopt;
  };
  const QString data_dir = data_path.isEmpty() ? QString() : QFileInfo(data_path).path();
  const QStringList values = detail::configStringValues(config);
  QSet<QString> taken(values.cbegin(), values.cend());
  taken.insert(data_logical_path);
  QHash<QString, QString> replacements;
  // One file picked twice is staged twice: its copies share a name and fingerprint, and one logical name.
  QHash<QString, QString> logical_by_content;
  LogicalSideFiles result;
  for (const QString& value : values) {
    if (value.isEmpty() || value == data_path) {
      continue;
    }
    if (value == data_dir) {
      replacements.insert(value, QString());
      continue;
    }
    const std::optional<StagedFileInfo> staged = staged_file(value);
    if (!staged.has_value()) {
      continue;
    }
    if (staged->name.isEmpty() || !isCanonicalSha256(staged->content_sha256)) {
      return fail(
          QCoreApplication::translate(
              "LayoutXml",
              "'%1', which the source's plugin reads, has no content fingerprint: the file changed since it "
              "was picked. Open the source again.")
              .arg(staged->name));
    }
    const QString content_key = staged->name + u'\n' + staged->content_sha256;
    if (const auto known = logical_by_content.constFind(content_key); known != logical_by_content.cend()) {
      replacements.insert(value, *known);
      continue;
    }
    QString logical = staged->name;
    for (qsizetype n = result.side_files.size(); taken.contains(logical); ++n) {
      logical = u"pj4-side/%1/%2"_s.arg(n).arg(staged->name);
    }
    taken.insert(logical);
    logical_by_content.insert(content_key, logical);
    replacements.insert(value, logical);
    result.side_files.push_back({.filename = logical, .content_sha256 = staged->content_sha256});
  }
  result.config = replacements.isEmpty()
                      ? std::string(config)
                      : detail::replaceConfigStringValues(config, replacements).value_or(std::string(config));
  return result;
}

QList<DataSourceRef> extractDataSource(const QDomDocument& doc, const QDir& layout_dir) {
  QList<DataSourceRef> sources;
  const QDomElement root = doc.documentElement();
  // A missing or non-numeric pj4_version reads as 0, so an unversioned document takes the pre-v4 offset basis.
  const int schema_version = root.attribute(u"pj4_version"_s, u"0"_s).toInt();
  const QDomElement wrapper = root.firstChildElement(u"previouslyLoaded_Datafiles"_s);
  if (wrapper.isNull()) {
    return sources;
  }
  for (QDomElement file_info = wrapper.firstChildElement(u"fileInfo"_s); !file_info.isNull();
       file_info = file_info.nextSiblingElement(u"fileInfo"_s)) {
    const QString filename = file_info.attribute(u"filename"_s);
    if (filename.isEmpty()) {
      continue;
    }
    DataSourceRef info;
    info.serialized_path = filename;
    // Anchoring an opaque identity would turn pj-upload://... into <layout-dir>/pj-upload:/...
    if (isOpaqueSourceIdentity(filename)) {
      info.resolved_path = filename;
    } else {
      const QFileInfo qfi(filename);
      info.resolved_path = qfi.isAbsolute() ? qfi.absoluteFilePath() : layout_dir.absoluteFilePath(filename);
    }
    info.prefix = file_info.attribute(u"prefix"_s);
    readContentSha256(file_info, info.content_sha256, info.content_sha256_malformed);

    // Single-track state on <fileInfo>, consulted only when there are no <dataset> children.
    if (file_info.hasAttribute(u"display_offset_ns"_s)) {
      bool ok = false;
      info.display_offset_ns = file_info.attribute(u"display_offset_ns"_s).toLongLong(&ok);
      info.has_display_offset = ok;
      info.display_offset_includes_global_reference = ok && schema_version < 4;
    }
    bool order_ok = false;
    info.timeline_order = file_info.attribute(u"timeline_order"_s, u"-1"_s).toInt(&order_ok);
    if (!order_ok) {
      info.timeline_order = -1;
    }

    for (QDomElement dataset = file_info.firstChildElement(u"dataset"_s); !dataset.isNull();
         dataset = dataset.nextSiblingElement(u"dataset"_s)) {
      info.datasets.push_back(readDatasetRef(dataset, schema_version));
    }

    for (QDomElement side = file_info.firstChildElement(u"sideFile"_s); !side.isNull();
         side = side.nextSiblingElement(u"sideFile"_s)) {
      SideFileRef side_file{.filename = side.attribute(u"filename"_s)};
      if (side_file.filename.isEmpty()) {
        continue;
      }
      readContentSha256(side, side_file.content_sha256, side_file.content_sha256_malformed);
      info.side_files.push_back(std::move(side_file));
    }

    const QDomElement plugin = file_info.firstChildElement(u"plugin"_s);
    if (!plugin.isNull()) {
      info.plugin_id = plugin.attribute(u"ID"_s);
      info.plugin_manifest_id = plugin.attribute(u"manifest_id"_s);
      // text() joins the CDATA sections appendJsonAsCdata splits the JSON into.
      info.plugin_config_json = plugin.text();
    }

    // directCdataText, not text(): only the element's own CDATA, so a child element cannot leak into the
    // descriptor.
    const QDomElement materialize = file_info.firstChildElement(u"materialize"_s);
    if (!materialize.isNull()) {
      info.materialize_provider = materialize.attribute(u"provider"_s);
      info.materialize_identity = materialize.attribute(u"identity"_s);
      info.materialize_descriptor_json = directCdataText(materialize);
    }
    sources.push_back(std::move(info));
  }

  return sources;
}

QString SeriesPath::display() const {
  const QString path = topic.isEmpty() ? field : topic + QLatin1Char('/') + field;
  return dataset_source.isEmpty() ? path : dataset_source + QLatin1Char(':') + path;
}

namespace {

std::optional<SeriesPath> readPath(
    const QDomElement& curve, const QString& topic_attr, const QString& field_attr, const QString& dataset_id_attr,
    const QString& dataset_source_attr, const QString& dataset_path_attr) {
  if (!curve.hasAttribute(topic_attr)) {
    return std::nullopt;
  }
  bool id_ok = false;
  const auto raw_id = curve.attribute(dataset_id_attr).toULongLong(&id_ok);
  const std::uint32_t dataset_id =
      id_ok && raw_id <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(raw_id) : 0;
  return SeriesPath{
      curve.attribute(topic_attr), curve.attribute(field_attr), dataset_id, curve.attribute(dataset_source_attr),
      curve.attribute(dataset_path_attr)};
}

// Includes the qualifiers: two same-topic datasets must not collapse into one entry.
QString seriesPathDedupKey(const SeriesPath& path) {
  return QString::number(path.dataset_id) + QLatin1Char('\x1f') + path.dataset_source + QLatin1Char('\x1f') +
         path.dataset_path + QLatin1Char('\x1f') + path.topic + QLatin1Char('\x1f') + path.field;
}

template <typename Fn>
void forEachPlotChild(const QDomDocument& doc, const QString& tag, Fn&& fn) {
  const QDomNodeList plot_nodes = doc.elementsByTagName(u"plot"_s);
  for (int i = 0; i < plot_nodes.size(); ++i) {
    const QDomElement plot = plot_nodes.at(i).toElement();
    if (plot.isNull()) {
      continue;
    }
    for (QDomElement child = plot.firstChildElement(tag); !child.isNull(); child = child.nextSiblingElement(tag)) {
      fn(child);
    }
  }
}

template <typename Fn>
void forEachPlotCurve(const QDomDocument& doc, Fn&& fn) {
  forEachPlotChild(doc, u"curve"_s, std::forward<Fn>(fn));
}

template <typename Fn>
void forEachMarkerScope(const QDomDocument& doc, Fn&& fn) {
  forEachPlotChild(doc, u"marker_scope"_s, std::forward<Fn>(fn));
}

}  // namespace

std::optional<SeriesPath> readTimeSeriesPath(const QDomElement& curve) {
  return readPath(curve, u"topic"_s, u"field"_s, u"dataset_id"_s, u"dataset_source"_s, u"dataset_path"_s);
}

std::optional<SeriesPath> readXyXPath(const QDomElement& curve) {
  return readPath(curve, u"x_topic"_s, u"x_field"_s, u"x_dataset_id"_s, u"x_dataset_source"_s, u"x_dataset_path"_s);
}

std::optional<SeriesPath> readXyYPath(const QDomElement& curve) {
  return readPath(curve, u"y_topic"_s, u"y_field"_s, u"y_dataset_id"_s, u"y_dataset_source"_s, u"y_dataset_path"_s);
}

QList<SeriesPath> extractSeriesPaths(const QDomDocument& doc) {
  QList<SeriesPath> paths;
  QSet<QString> seen;
  const auto push = [&](const std::optional<SeriesPath>& p) {
    if (!p.has_value()) {
      return;
    }
    const QString dedup_key = seriesPathDedupKey(*p);
    if (!seen.contains(dedup_key)) {
      seen.insert(dedup_key);
      paths.push_back(*p);
    }
  };
  forEachPlotCurve(doc, [&](const QDomElement& curve) {
    push(readTimeSeriesPath(curve));
    push(readXyXPath(curve));
    push(readXyYPath(curve));
  });
  return paths;
}

std::vector<std::uint32_t> matchFanoutDatasets(
    const QList<DataSourceDatasetRef>& saved, const std::vector<std::uint32_t>& candidates,
    const std::function<QString(std::uint32_t)>& source_name_of) {
  std::vector<std::uint32_t> matches(static_cast<std::size_t>(saved.size()), 0);
  const auto named_saved_count = [&saved](const QString& name) {
    return static_cast<int>(std::count_if(
        saved.begin(), saved.end(), [&name](const DataSourceDatasetRef& child) { return child.source_name == name; }));
  };
  const auto named_candidate_count = [&candidates, &source_name_of](const QString& name) {
    return static_cast<int>(std::count_if(
        candidates.begin(), candidates.end(), [&](std::uint32_t id) { return source_name_of(id) == name; }));
  };

  QSet<std::uint32_t> used;
  // An empty saved name matches any live name.
  const auto match_by_index = [&](const DataSourceDatasetRef& child) -> std::optional<std::uint32_t> {
    if (child.source_index < 0 || child.source_index >= static_cast<int>(candidates.size())) {
      return std::nullopt;
    }
    const std::uint32_t indexed = candidates[static_cast<std::size_t>(child.source_index)];
    if (used.contains(indexed)) {
      return std::nullopt;
    }
    if (child.source_name.isEmpty() || source_name_of(indexed) == child.source_name) {
      return indexed;
    }
    return std::nullopt;
  };

  for (qsizetype child_index = 0; child_index < saved.size(); ++child_index) {
    const DataSourceDatasetRef& child = saved[child_index];
    const bool name_is_duplicated = !child.source_name.isEmpty() && named_saved_count(child.source_name) > 1;

    std::uint32_t matched = 0;
    if (name_is_duplicated) {
      if (named_candidate_count(child.source_name) == named_saved_count(child.source_name)) {
        matched = match_by_index(child).value_or(0);
      }
    } else {
      std::vector<std::uint32_t> source_matches;
      for (const std::uint32_t candidate : candidates) {
        if (used.contains(candidate)) {
          continue;
        }
        if (child.source_name.isEmpty() || source_name_of(candidate) == child.source_name) {
          source_matches.push_back(candidate);
        }
      }
      if (source_matches.size() == 1) {
        matched = source_matches.front();
      } else {
        matched = match_by_index(child).value_or(0);
      }
    }
    if (matched != 0) {
      used.insert(matched);
      matches[static_cast<std::size_t>(child_index)] = matched;
    }
  }
  return matches;
}

QList<SeriesPath> rebindCurveKeys(QDomDocument& doc, const SeriesKeyResolver& resolve) {
  QList<SeriesPath> unresolved;
  QSet<QString> unresolved_seen;
  const auto record_unresolved = [&](const SeriesPath& p) {
    const QString dedup_key = seriesPathDedupKey(p);
    if (!unresolved_seen.contains(dedup_key)) {
      unresolved_seen.insert(dedup_key);
      unresolved.push_back(p);
    }
  };

  std::vector<QDomElement> curves;
  forEachPlotCurve(doc, [&](const QDomElement& curve) { curves.push_back(curve); });

  for (QDomElement& curve : curves) {
    const bool persistent_intent = plot_xml::isPendingIntent(curve);
    const std::optional<SeriesPath> xy_x = readXyXPath(curve);
    if (xy_x.has_value()) {
      // XY curve: both axes must resolve, else the curve is undrawable.
      const std::optional<SeriesPath> xy_y = readXyYPath(curve);
      const std::optional<QString> x_key = resolve(*xy_x);
      const std::optional<QString> y_key = xy_y.has_value() ? resolve(*xy_y) : std::nullopt;
      if (x_key.has_value() && y_key.has_value()) {
        curve.setAttribute(u"curve_x"_s, *x_key);
        curve.setAttribute(u"curve_y"_s, *y_key);
      } else {
        curve.removeAttribute(u"curve_x"_s);
        curve.removeAttribute(u"curve_y"_s);
        if (!persistent_intent && !x_key.has_value()) {
          record_unresolved(*xy_x);
        }
        if (!persistent_intent && xy_y.has_value() && !y_key.has_value()) {
          record_unresolved(*xy_y);
        }
      }
      continue;
    }

    const std::optional<SeriesPath> ts = readTimeSeriesPath(curve);
    if (!ts.has_value()) {
      continue;  // No stable identity to rebind; leave as-is.
    }
    if (const std::optional<QString> key = resolve(*ts); key.has_value()) {
      curve.setAttribute(u"name"_s, *key);
    } else {
      curve.removeAttribute(u"name"_s);
      if (!persistent_intent) {
        record_unresolved(*ts);
      }
    }
  }
  return unresolved;
}

namespace {

// An (id, source, path) attribute triple that carries a dataset qualifier, and the attributes naming the topic it
// qualifies (`topic_name` is the scene spelling).
struct DatasetIdentityAttributes {
  QString id;
  QString source;
  QString path;
  QString topic;
  QString topic_name;
};

// Persisted attribute prefixes. kPlain: a time-series curve, a <transform> / <on_demand> / <CustomWidget> <input>,
// a dataset-bound <generator> (an all_datasets one has no dataset_id), a marker scope, a scene <layer> /
// <config_topic>. kX / kY: the XY curve axes. kInput: a <processor>'s input. kSource: a scene <robot_model> /
// <trail>.
enum class Qualifier : std::uint8_t { kPlain, kX, kY, kInput, kSource };

const std::array<DatasetIdentityAttributes, 5>& qualifierShapes() {
  static const std::array<DatasetIdentityAttributes, 5> shapes{{
      {u"dataset_id"_s, u"dataset_source"_s, u"dataset_path"_s, u"topic"_s, u"topic_name"_s},
      {u"x_dataset_id"_s, u"x_dataset_source"_s, u"x_dataset_path"_s, u"x_topic"_s, u"x_topic_name"_s},
      {u"y_dataset_id"_s, u"y_dataset_source"_s, u"y_dataset_path"_s, u"y_topic"_s, u"y_topic_name"_s},
      {u"input_dataset_id"_s, u"input_dataset_source"_s, u"input_dataset_path"_s, u"input_topic"_s,
       u"input_topic_name"_s},
      {u"source_dataset_id"_s, u"source_dataset_source"_s, u"source_dataset_path"_s, u"source_topic"_s,
       u"source_topic_name"_s},
  }};
  return shapes;
}

// The elements that read a dataset through the plain qualifier without naming a topic: a dataset-bound marker rule
// (an all_datasets one reads every dataset), a marker scope, and a transform's or on-demand recipe's <input> (it may
// name another node's output alone). `parent` empty: any.
struct NamelessReader {
  QLatin1StringView tag;
  QLatin1StringView parent;
};
constexpr std::array<NamelessReader, 4> kNamelessReaders{{
    {"generator"_L1, {}},
    {"marker_scope"_L1, {}},
    {"input"_L1, "transform"_L1},
    {"input"_L1, "on_demand"_L1},
}};

const DatasetIdentityAttributes& qualifier(Qualifier shape) {
  return qualifierShapes()[static_cast<std::size_t>(shape)];
}

// The three <plot><curve> shapes: time-series and both XY axes.
std::span<const DatasetIdentityAttributes, 3> curveQualifiers() {
  return std::span(qualifierShapes()).first<3>();
}

// QDom elements are shared handles: a const document still yields writable elements.
template <typename Fn>
void forEachTagged(const QDomDocument& doc, const QString& tag, Fn&& fn) {
  const QDomNodeList nodes = doc.elementsByTagName(tag);
  for (int index = 0; index < nodes.size(); ++index) {
    QDomElement element = nodes.at(index).toElement();
    if (!element.isNull()) {
      fn(element);
    }
  }
}

template <typename Fn>
void forEachProcessor(const QDomDocument& doc, Fn&& fn) {
  forEachTagged(doc, u"processor"_s, std::forward<Fn>(fn));
}

template <typename Fn>
void forEachGenerator(const QDomDocument& doc, Fn&& fn) {
  forEachTagged(doc, u"generator"_s, std::forward<Fn>(fn));
}

template <typename Fn>
void forEachTransformInput(const QDomDocument& doc, Fn&& fn) {
  for (const QString& tag : {u"transform"_s, u"on_demand"_s}) {
    const QDomNodeList processors = doc.elementsByTagName(tag);
    for (int index = 0; index < processors.size(); ++index) {
      const QDomElement processor = processors.at(index).toElement();
      for (QDomElement input = processor.firstChildElement(u"input"_s); !input.isNull();
           input = input.nextSiblingElement(u"input"_s)) {
        fn(input);
      }
    }
  }
}

// Selected through the parent: a marker rule's <input> elements are not wires and carry no qualifiers.
template <typename Fn>
void forEachCustomWidgetInput(const QDomDocument& doc, Fn&& fn) {
  forEachTagged(doc, u"CustomWidget"_s, [&fn](QDomElement& widget) {
    for (QDomElement input = widget.firstChildElement(u"input"_s); !input.isNull();
         input = input.nextSiblingElement(u"input"_s)) {
      fn(input);
    }
  });
}

// Document order (matchDatasetsToFiles pairs by first appearance); iterative, so a deep layout cannot overflow the
// stack. Visitors see every widget family's elements: select by exact tagName or touch only attributes you own.
template <typename Fn>
void forEachElementUnder(const QDomElement& root, Fn&& visit) {
  std::vector<QDomElement> pending;
  pending.push_back(root);
  while (!pending.empty()) {
    QDomElement element = pending.back();
    pending.pop_back();
    visit(element);
    for (QDomElement child = element.lastChildElement(); !child.isNull(); child = child.previousSiblingElement()) {
      pending.push_back(child);
    }
  }
}

template <typename Fn>
void forEachElement(const QDomDocument& doc, Fn&& visit) {
  forEachElementUnder(doc.documentElement(), std::forward<Fn>(visit));
}

// Every distinct (path, source) pair the layout's qualifiers carry, in document order.
QList<DatasetIdentity> qualifierPairs(const QDomDocument& doc) {
  QList<DatasetIdentity> pairs;
  forEachElement(doc, [&pairs](const QDomElement& element) {
    for (const DatasetIdentityAttributes& shape : qualifierShapes()) {
      DatasetIdentity pair{.path = element.attribute(shape.path), .source = element.attribute(shape.source)};
      if ((!pair.path.isEmpty() || !pair.source.isEmpty()) && !pairs.contains(pair)) {
        pairs.push_back(std::move(pair));
      }
    }
  });
  return pairs;
}

}  // namespace

bool DatasetIdentity::isOpaque() const {
  return isOpaqueSourceIdentity(path.isEmpty() ? source : path);
}

namespace {

// The one entry of `files` whose file name is `name`, or empty when none or several are.
QString uniqueFileNamed(const QStringList& files, const QString& name) {
  QString found;
  for (const QString& file : files) {
    if (QFileInfo(file).fileName() == name) {
      if (!found.isEmpty()) {
        return {};
      }
      found = file;
    }
  }
  return found;
}

// A qualifier's identity: its saved path, else its source label; empty when it has neither or an opaque path.
QString qualifierIdentity(const QDomElement& element, const DatasetIdentityAttributes& shape) {
  const QString path = element.attribute(shape.path);
  if (path.isEmpty()) {
    return element.attribute(shape.source);
  }
  return isOpaqueSourceIdentity(path) ? QString() : path;
}

// The label the loader gives `kept`'s dataset, when `saved_source` is the one it gave `saved_file`: the file name, or
// "<complete base name>/<entry>" for a fan-out. Any other label (a plugin's display name) is kept as saved.
QString keptSourceLabel(const QString& saved_source, const QString& saved_file, const QString& kept) {
  const QFileInfo saved(saved_file);
  const QFileInfo now(kept);
  if (saved_source == saved.fileName()) {
    return now.fileName();
  }
  if (const QString prefix = saved.completeBaseName() + u'/'; saved_source.startsWith(prefix)) {
    return now.completeBaseName() + u'/' + saved_source.mid(prefix.size());
  }
  return saved_source;
}

}  // namespace

QList<DatasetFileMatch> matchDatasetsToFiles(
    const QDomDocument& doc, const QStringList& files, const QStringList& source_order) {
  // Distinct identities (the saved path, else the label) in first-appearance order, each with the first label seen
  // for it.
  QList<DatasetFileMatch> saved;
  for (const DatasetIdentity& pair : qualifierPairs(doc)) {
    if (isOpaqueSourceIdentity(pair.path)) {
      continue;
    }
    const QString identity = pair.path.isEmpty() ? pair.source : pair.path;
    const auto it = std::find_if(saved.begin(), saved.end(), [&](const auto& s) { return s.identity == identity; });
    if (it == saved.end()) {
      saved.push_back({.identity = identity, .label = pair.source, .file = {}, .rule = DatasetFileMatch::Rule::kPath});
    } else if (it->label.isEmpty()) {
      it->label = pair.source;
    }
  }
  const auto order = [&source_order](const DatasetFileMatch& entry) {
    const auto it = std::find_if(source_order.begin(), source_order.end(), [&entry](const QString& path) {
      return QDir::cleanPath(path) == entry.identity;
    });
    return it == source_order.end() ? source_order.size() : std::distance(source_order.begin(), it);
  };
  std::stable_sort(saved.begin(), saved.end(), [&order](const auto& a, const auto& b) { return order(a) < order(b); });

  const auto has_path = [](const DatasetFileMatch& entry) { return QFileInfo(entry.identity).isAbsolute(); };
  // A path-less label that some saved path also carries names that path's dataset: it follows that path's match.
  QList<DatasetFileMatch> aliases;
  QList<DatasetFileMatch> datasets;
  for (const DatasetFileMatch& entry : saved) {
    const bool alias = !has_path(entry) && std::any_of(saved.begin(), saved.end(), [&](const DatasetFileMatch& other) {
      return has_path(other) && other.label == entry.identity;
    });
    (alias ? aliases : datasets).push_back(entry);
  }

  QStringList unclaimed = files;
  QList<DatasetFileMatch> matched;
  const auto take = [&](DatasetFileMatch entry, const QString& file, DatasetFileMatch::Rule rule) {
    entry.file = file;
    entry.rule = rule;
    if (entry.label.isEmpty()) {
      entry.label = QFileInfo(entry.identity).fileName();
    }
    unclaimed.removeAll(file);
    matched.push_back(std::move(entry));
  };
  QList<DatasetFileMatch> left;
  for (const DatasetFileMatch& entry : datasets) {
    const auto same =
        std::find_if(files.begin(), files.end(), [&entry](const QString& f) { return isSamePath(f, entry.identity); });
    if (same != files.end()) {
      take(entry, *same, DatasetFileMatch::Rule::kPath);
    } else {
      left.push_back(entry);
    }
  }
  QList<DatasetFileMatch> unmatched;
  for (const DatasetFileMatch& entry : left) {
    const QString name = has_path(entry) ? QFileInfo(entry.identity).fileName() : entry.identity;
    const QString file = uniqueFileNamed(unclaimed, name);
    if (!file.isEmpty()) {
      take(entry, file, DatasetFileMatch::Rule::kName);
    } else if (has_path(entry)) {
      unmatched.push_back(entry);  // a label alone (a toolbox's source) has no format to pair by
    }
  }
  // Pairs only within one format: a recording and a CSV trading places would leave every curve unresolved.
  const auto format = [](const QString& path) { return QFileInfo(path).suffix().toLower(); };
  QSet<QString> formats;
  for (const DatasetFileMatch& entry : unmatched) {
    formats.insert(format(entry.identity));
  }
  for (const QString& suffix : formats) {
    QList<DatasetFileMatch> datasets_of;
    std::copy_if(unmatched.begin(), unmatched.end(), std::back_inserter(datasets_of), [&](const auto& entry) {
      return format(entry.identity) == suffix;
    });
    QStringList files_of;
    std::copy_if(unclaimed.begin(), unclaimed.end(), std::back_inserter(files_of), [&](const QString& file) {
      return format(file) == suffix;
    });
    if (datasets_of.size() == files_of.size()) {
      for (qsizetype i = 0; i < datasets_of.size(); ++i) {
        take(datasets_of[i], files_of[i], DatasetFileMatch::Rule::kPosition);
      }
    }
  }
  for (const DatasetFileMatch& alias : aliases) {
    const auto target = std::find_if(matched.begin(), matched.end(), [&alias](const DatasetFileMatch& m) {
      return QFileInfo(m.identity).isAbsolute() && m.label == alias.identity;
    });
    if (target != matched.end()) {
      const QString file = target->file;
      take(alias, file, DatasetFileMatch::Rule::kAlias);
    }
  }
  return matched;
}

void removeDatasetQualifiersForGenericLayout(QDomDocument& doc, const QList<DatasetFileMatch>& keep) {
  // Keeping a scene's or Custom Widget's upload path would source-bind the layout and leak a page-lifetime browser
  // identity.
  forEachElement(doc, [&keep](QDomElement& element) {
    for (const DatasetIdentityAttributes& shape : qualifierShapes()) {
      element.removeAttribute(shape.id);
      const QString identity = qualifierIdentity(element, shape);
      const auto match = std::find_if(keep.begin(), keep.end(), [&](const auto& m) { return m.identity == identity; });
      if (identity.isEmpty() || match == keep.end()) {
        element.removeAttribute(shape.source);
        element.removeAttribute(shape.path);
        continue;
      }
      if (const QString source = element.attribute(shape.source); !source.isEmpty()) {
        element.setAttribute(shape.source, keptSourceLabel(source, identity, match->file));
      }
      element.setAttribute(shape.path, match->file);
    }
  });
}

namespace {

// What `element` reads through `shape`, so a pinned layout gives it that qualifier: the topic it names under the shape
// (curves, wires, recipe inputs, filters, scene layers and payloads), empty for a nameless reader, nullopt when it
// reads nothing through it. A local scene layer names no topic and stays local.
std::optional<QString> readsThrough(const QDomElement& element, const DatasetIdentityAttributes& shape) {
  for (const QString& attribute : {shape.topic, shape.topic_name}) {
    if (QString topic = element.attribute(attribute); !topic.isEmpty()) {
      return topic;
    }
  }
  if (shape.id != qualifier(Qualifier::kPlain).id || element.attribute(u"all_datasets"_s) == "1"_L1) {
    return std::nullopt;
  }
  const QString tag = element.tagName();
  const QString parent = element.parentNode().toElement().tagName();
  const bool nameless = std::any_of(kNamelessReaders.begin(), kNamelessReaders.end(), [&](const NamelessReader& r) {
    return tag == r.tag && (r.parent.isEmpty() || parent == r.parent);
  });
  return nameless ? std::optional<QString>(QString()) : std::nullopt;
}

// Whether a marker rule reads one of `topics`: its <input name> keys are "<topic>/<field>".
bool ruleReadsAny(const QDomElement& rule, const QSet<QString>& topics) {
  for (QDomElement input = rule.firstChildElement(u"input"_s); !input.isNull();
       input = input.nextSiblingElement(u"input"_s)) {
    const QString key = input.attribute(u"name"_s);
    if (std::any_of(topics.begin(), topics.end(), [&key](const QString& topic) {
          return key == topic || key.startsWith(topic + u'/');
        })) {
      return true;
    }
  }
  return false;
}

// Removes every qualifier under `root`, then pins to `pin` the readers of `topics`, the marker rules reading one of
// them and the other nameless readers; the others bind by what they read.
void pinQualifiers(const QDomElement& root, const DatasetPin& pin, const QSet<QString>& topics) {
  forEachElementUnder(root, [&pin, &topics](QDomElement& element) {
    for (const DatasetIdentityAttributes& shape : qualifierShapes()) {
      element.removeAttribute(shape.id);
      element.removeAttribute(shape.source);
      element.removeAttribute(shape.path);
      const std::optional<QString> topic = readsThrough(element, shape);
      if (!topic.has_value()) {
        continue;
      }
      const bool pinned = topic->isEmpty() ? element.tagName() != "generator"_L1 || ruleReadsAny(element, topics)
                                           : topics.contains(*topic);
      if (!pinned) {
        continue;
      }
      if (pin.id != 0) {
        element.setAttribute(shape.id, QString::number(pin.id));
      }
      element.setAttribute(shape.source, pin.source);
      if (!pin.path.isEmpty()) {
        element.setAttribute(shape.path, pin.path);
      }
    }
  });
}

template <typename Fn>
void forEachRecipe(const QDomDocument& doc, Fn&& fn) {
  for (const QString& tag : {u"processor"_s, u"transform"_s, u"on_demand"_s, u"generator"_s}) {
    forEachTagged(doc, tag, fn);
  }
}

// Whether `recipe` (filter, transform, on-demand recipe or marker rule) reads one of `topics`.
bool recipeReadsAny(const QDomElement& recipe, const QSet<QString>& topics) {
  if (recipe.tagName() == "processor"_L1) {
    return topics.contains(recipe.attribute(u"input_topic"_s));
  }
  if (recipe.tagName() == "generator"_L1) {
    return ruleReadsAny(recipe, topics);
  }
  for (QDomElement input = recipe.firstChildElement(u"input"_s); !input.isNull();
       input = input.nextSiblingElement(u"input"_s)) {
    if (topics.contains(input.attribute(u"topic"_s))) {
      return true;
    }
  }
  return false;
}

// The topics `recipe` writes: a filter's output, and the topic part of each <output> name of a transform, on-demand
// recipe or marker rule.
QStringList recipeOutputs(const QDomElement& recipe) {
  if (recipe.tagName() == "processor"_L1) {
    return {recipe.attribute(u"output_name"_s)};
  }
  QStringList outputs;
  for (QDomElement output = recipe.firstChildElement(u"output"_s); !output.isNull();
       output = output.nextSiblingElement(u"output"_s)) {
    outputs.push_back(output.attribute(u"name"_s).section(u':', 0, 0));
  }
  return outputs;
}

QSet<QString> producedTopics(const QDomDocument& doc) {
  QSet<QString> produced;
  forEachRecipe(doc, [&produced](const QDomElement& recipe) {
    for (const QString& topic : recipeOutputs(recipe)) {
      produced.insert(topic);
    }
  });
  return produced;
}

// `topics` and, through chains of recipes, the outputs of every recipe reading one of them: a recipe pinned to a
// dataset writes there, so what reads its output is pinned with it.
QSet<QString> withOutputsOfReaders(const QDomDocument& doc, QSet<QString> topics) {
  for (qsizetype known = -1; known != topics.size();) {
    known = topics.size();
    forEachRecipe(doc, [&topics](const QDomElement& recipe) {
      if (recipeReadsAny(recipe, topics)) {
        for (const QString& topic : recipeOutputs(recipe)) {
          topics.insert(topic);
        }
      }
    });
  }
  return topics;
}

// A snapshot: elementsByTagName lists are live, so inserting while walking one revisits the inserted elements.
std::vector<QDomElement> elementsTagged(const QDomDocument& doc, const QString& tag) {
  std::vector<QDomElement> elements;
  forEachTagged(doc, tag, [&elements](const QDomElement& element) { elements.push_back(element); });
  return elements;
}

}  // namespace

QList<DatasetIdentity> datasetIdentities(const QDomDocument& doc) {
  const QList<DatasetIdentity> pairs = qualifierPairs(doc);
  QList<DatasetIdentity> identities;
  for (const DatasetIdentity& pair : pairs) {
    const bool named_by_another = std::any_of(pairs.begin(), pairs.end(), [&pair](const DatasetIdentity& other) {
      return other != pair && ((pair.source.isEmpty() && other.path == pair.path) ||
                               (pair.path.isEmpty() && !other.path.isEmpty() && other.source == pair.source));
    });
    if (!named_by_another) {
      identities.push_back(pair);
    }
  }
  return identities;
}

QSet<QString> referencedTopics(const QDomDocument& doc) {
  QSet<QString> topics;
  forEachElement(doc, [&topics](const QDomElement& element) {
    for (const DatasetIdentityAttributes& shape : qualifierShapes()) {
      for (const QString& attribute : {shape.topic, shape.topic_name}) {
        if (const QString topic = element.attribute(attribute); !topic.isEmpty()) {
          topics.insert(topic);
        }
      }
    }
  });
  return topics - producedTopics(doc);
}

DatasetChoice planDatasetChoice(
    const QDomDocument& doc, const std::optional<QList<DatasetIdentity>>& saved, const LoadedDatasets& loaded) {
  const QList<DatasetIdentity> named = datasetIdentities(doc);
  const QList<DatasetIdentity>& as_saved = saved.has_value() ? *saved : named;
  const auto opaque = [](const DatasetIdentity& identity) { return identity.isOpaque(); };
  if (as_saved.size() > 1 || named.size() > 1 || std::any_of(as_saved.begin(), as_saved.end(), opaque) ||
      std::any_of(named.begin(), named.end(), opaque) || (named.size() == 1 && loaded.names_one(named.front()))) {
    return {};
  }
  const QSet<QString> topics = referencedTopics(doc);
  QHash<std::uint32_t, QSet<QString>> carried;
  loaded.for_each_topic([&topics, &carried](std::uint32_t dataset, const QString& topic) {
    if (topics.contains(topic)) {
      carried[dataset].insert(topic);
    }
  });
  // Only a topic two or more datasets carry is ambiguous; any other binds where it is unique, whatever is chosen.
  QSet<QString> seen;
  QSet<QString> shared;
  for (const QSet<QString>& names : std::as_const(carried)) {
    shared |= seen & names;
    seen |= names;
  }
  if (shared.isEmpty()) {
    return {};
  }
  // The shared topics a dataset carries, then all the layout's topics it carries to break a tie.
  const auto fit = [&shared](const QSet<QString>& names) { return std::pair{(names & shared).size(), names.size()}; };
  std::pair<qsizetype, qsizetype> most{};
  for (const QSet<QString>& names : std::as_const(carried)) {
    most = std::max(most, fit(names));
  }
  DatasetChoice choice;
  for (const DatasetPin& pin : loaded.datasets) {
    if (fit(carried.value(pin.id)) == most) {
      choice.datasets.push_back(pin);
    }
  }
  if (choice.datasets.size() == 1) {
    choice.kind = DatasetChoice::Kind::kPin;
    choice.topics = carried.value(choice.datasets.front().id) & shared;
  } else if (choice.datasets.size() > 1) {
    choice.kind = DatasetChoice::Kind::kAsk;
    choice.topics = shared;
    choice.blockers = duplicationBlockers(doc);
  }
  return choice;
}

QStringList duplicationBlockers(const QDomDocument& doc) {
  QStringList blockers;
  for (const QString& tag : {u"transform"_s, u"on_demand"_s}) {
    forEachTagged(doc, tag, [&blockers](const QDomElement& recipe) {
      blockers.push_back(recipe.attribute(u"owner_plugin"_s) + u'/' + recipe.attribute(u"id"_s));
    });
  }
  return blockers;
}

void pinLayoutToDataset(QDomDocument& doc, const DatasetPin& pin, const QSet<QString>& topics) {
  pinQualifiers(doc.documentElement(), pin, withOutputsOfReaders(doc, topics));
}

void pinLayoutToDatasets(
    QDomDocument& doc, const QList<DatasetPin>& pins, const QSet<QString>& topics,
    const RememberedCurveColor& remembered) {
  if (pins.isEmpty()) {
    return;
  }
  // The pinned references name the first dataset; each further copy is cloned from it and pinned again.
  const QSet<QString> pinned = withOutputsOfReaders(doc, topics);
  pinQualifiers(doc.documentElement(), pins.front(), pinned);
  if (pins.size() == 1) {
    return;
  }
  QDomElement processors = doc.documentElement().firstChildElement(u"data_processors"_s);
  std::vector<QDomElement> recipes;
  for (QDomElement recipe = processors.firstChildElement(); !recipe.isNull(); recipe = recipe.nextSiblingElement()) {
    recipes.push_back(recipe);
  }
  const auto clone_for_pins = [&pins, &pinned](QDomElement& parent, const QDomElement& original, const auto& adapt) {
    QDomNode after = original;
    for (int copy = 1; copy < pins.size(); ++copy) {
      QDomElement clone = original.cloneNode(true).toElement();
      pinQualifiers(clone, pins[copy], pinned);
      adapt(clone, copy);
      after = parent.insertAfter(clone, after);
    }
  };
  for (const QDomElement& recipe : recipes) {
    const bool filter = recipe.tagName() == "processor"_L1;
    const bool rule = recipe.tagName() == "generator"_L1 && recipe.attribute(u"all_datasets"_s) != "1"_L1;
    if ((filter || rule) && recipeReadsAny(recipe, pinned)) {
      clone_for_pins(processors, recipe, [](QDomElement&, int) {});
    }
  }

  QSet<QString> ids;  // every tab, area and plot id: a copy takes one no element has
  for (const QString& tag : {u"Tab"_s, u"DockArea"_s, u"plot"_s}) {
    forEachTagged(doc, tag, [&ids](const QDomElement& element) { ids.insert(element.attribute(u"id"_s)); });
  }
  const auto new_id = [&ids](const QString& id) {
    for (int n = 2;; ++n) {
      if (QString candidate = u"%1~%2"_s.arg(id).arg(n); !ids.contains(candidate)) {
        ids.insert(candidate);
        return candidate;
      }
    }
  };
  for (QDomElement& area : elementsTagged(doc, u"tabbed_widget"_s)) {
    std::vector<QDomElement> tabs;
    for (QDomElement tab = area.firstChildElement(u"Tab"_s); !tab.isNull(); tab = tab.nextSiblingElement(u"Tab"_s)) {
      tabs.push_back(tab);
    }
    for (std::size_t index = 0; index < tabs.size(); ++index) {
      QDomElement& tab = tabs[index];
      forEachElementUnder(tab, [&remembered](QDomElement& element) {
        if (element.tagName() == "plot"_L1) {
          plot_xml::fillMissingCurveColors(element, remembered);
        }
      });
      const QString name = tab.attribute(u"tab_name"_s, u"tab%1"_s.arg(index + 1));
      clone_for_pins(area, tab, [&](QDomElement& clone, int copy) {
        plot_xml::prepareTabCopy(clone, new_id);
        clone.setAttribute(u"tab_name"_s, u"%1 [%2]"_s.arg(name, pins[copy].label));
      });
      tab.setAttribute(u"tab_name"_s, u"%1 [%2]"_s.arg(name, pins.front().label));
    }
    QDomElement current = area.firstChildElement(u"currentTabIndex"_s);
    if (!current.isNull()) {
      current.setAttribute(u"index"_s, current.attribute(u"index"_s, u"0"_s).toInt() * static_cast<int>(pins.size()));
    }
  }
}

namespace {

void stampIdentityPair(QDomElement& element, const DatasetIdentityAttributes& pair, const DatasetPathLookup& lookup) {
  if (!element.hasAttribute(pair.id)) {
    return;
  }
  bool ok = false;
  const qulonglong raw_id = element.attribute(pair.id).toULongLong(&ok);
  if (!ok || raw_id == 0 || raw_id > std::numeric_limits<std::uint32_t>::max()) {
    return;
  }
  const QString path = lookup(static_cast<std::uint32_t>(raw_id));
  if (!path.isEmpty()) {
    element.setAttribute(pair.path, path);
  }
}

void stripUnvalidatedIdentityPair(QDomElement& element, const DatasetIdentityAttributes& pair) {
  if (!element.hasAttribute(pair.id) || !element.attribute(pair.path).isEmpty()) {
    return;
  }
  bool ok = false;
  const qulonglong id = element.attribute(pair.id).toULongLong(&ok);
  if (ok && id != 0) {  // 0 is a local-scene sentinel, not a remintable id.
    element.removeAttribute(pair.id);
  }
}

}  // namespace

void stampDatasetSourcePaths(QDomDocument& doc, const DatasetPathLookup& lookup) {
  if (!lookup) {
    return;
  }
  forEachPlotCurve(doc, [&lookup](QDomElement curve) {
    for (const DatasetIdentityAttributes& pair : curveQualifiers()) {
      stampIdentityPair(curve, pair, lookup);
    }
  });
  forEachProcessor(
      doc, [&lookup](QDomElement& processor) { stampIdentityPair(processor, qualifier(Qualifier::kInput), lookup); });
  forEachTransformInput(
      doc, [&lookup](QDomElement& input) { stampIdentityPair(input, qualifier(Qualifier::kPlain), lookup); });
  forEachGenerator(doc, [&lookup](QDomElement& gen) { stampIdentityPair(gen, qualifier(Qualifier::kPlain), lookup); });
  forEachMarkerScope(
      doc, [&lookup](QDomElement scope) { stampIdentityPair(scope, qualifier(Qualifier::kPlain), lookup); });
  forEachCustomWidgetInput(
      doc, [&lookup](QDomElement& input) { stampIdentityPair(input, qualifier(Qualifier::kPlain), lookup); });
}

void removeUnvalidatedDatasetIds(QDomDocument& doc) {
  forEachPlotCurve(doc, [](QDomElement curve) {
    for (const DatasetIdentityAttributes& pair : curveQualifiers()) {
      stripUnvalidatedIdentityPair(curve, pair);
    }
  });
  forEachProcessor(
      doc, [](QDomElement& processor) { stripUnvalidatedIdentityPair(processor, qualifier(Qualifier::kInput)); });
  forEachTransformInput(
      doc, [](QDomElement& input) { stripUnvalidatedIdentityPair(input, qualifier(Qualifier::kPlain)); });
  forEachGenerator(doc, [](QDomElement& gen) { stripUnvalidatedIdentityPair(gen, qualifier(Qualifier::kPlain)); });
  forEachMarkerScope(doc, [](QDomElement scope) { stripUnvalidatedIdentityPair(scope, qualifier(Qualifier::kPlain)); });
  forEachCustomWidgetInput(
      doc, [](QDomElement& input) { stripUnvalidatedIdentityPair(input, qualifier(Qualifier::kPlain)); });
}

void resolveDatasetSourcePaths(QDomDocument& doc, const QDir& layout_dir) {
  remapDatasetSourcePaths(doc, [&layout_dir](const QString& saved_path) {
    if (isOpaqueSourceIdentity(saved_path)) {
      return saved_path;
    }
    return QDir::cleanPath(QFileInfo(saved_path).isAbsolute() ? saved_path : layout_dir.absoluteFilePath(saved_path));
  });
}

void remapDatasetSourcePaths(QDomDocument& doc, const DatasetPathRemapper& remap) {
  if (!remap) {
    return;
  }
  forEachElement(doc, [&remap](QDomElement& element) {
    for (const DatasetIdentityAttributes& shape : qualifierShapes()) {
      if (!element.hasAttribute(shape.path)) {
        continue;
      }
      const QString saved_path = element.attribute(shape.path);
      if (saved_path.isEmpty()) {
        continue;
      }
      element.setAttribute(shape.path, remap(saved_path));
    }
  });
}

void stampDatasetIdentities(QDomDocument& doc, const QHash<std::uint32_t, QString>& identity_for_dataset) {
  if (identity_for_dataset.isEmpty()) {
    return;
  }
  forEachElement(doc, [&identity_for_dataset](QDomElement& element) {
    for (const DatasetIdentityAttributes& shape : qualifierShapes()) {
      bool ok = false;
      const std::uint32_t dataset_id = element.attribute(shape.id).toUInt(&ok);
      if (const auto identity = identity_for_dataset.constFind(dataset_id);
          ok && identity != identity_for_dataset.cend()) {
        element.setAttribute(shape.path, identity.value());
      }
    }
  });
}

void bindStreamQualifiers(QDomDocument& doc, const QHash<QString, std::uint32_t>& dataset_for_identity) {
  if (dataset_for_identity.isEmpty()) {
    return;
  }
  forEachElement(doc, [&dataset_for_identity](QDomElement& element) {
    std::optional<std::uint32_t> bound;
    for (const DatasetIdentityAttributes& shape : qualifierShapes()) {
      const auto dataset = dataset_for_identity.constFind(element.attribute(shape.path));
      if (dataset == dataset_for_identity.cend()) {
        continue;
      }
      element.setAttribute(shape.id, QString::number(dataset.value()));
      element.removeAttribute(shape.source);
      element.removeAttribute(shape.path);
      bound = dataset.value();
    }
    if (!bound.has_value()) {
      return;
    }
    const QString tag = element.tagName();
    if (tag == "curve"_L1 || tag == "layer"_L1 || tag == "config_topic"_L1) {
      plot_xml::markPendingIntent(element);
      return;
    }
    // A robot or trail payload names its stream for its <layer>, which is what waits for the
    // topic; a layer with no identity of its own demands the topic from the payload's stream.
    QDomElement layer = element.parentNode().toElement();
    if (layer.tagName() != "layer"_L1) {
      return;
    }
    plot_xml::markPendingIntent(layer);
    const DatasetIdentityAttributes& own = qualifier(Qualifier::kPlain);
    if (!layer.hasAttribute(own.id) && !layer.hasAttribute(own.source) && !layer.hasAttribute(own.path)) {
      layer.setAttribute(own.id, QString::number(*bound));
    }
  });
}

void relativizeDatasetSourcePaths(QDomDocument& doc, const DatasetPathRemapper& relocate) {
  if (!relocate) {
    return;
  }
  remapDatasetSourcePaths(doc, [&relocate](const QString& saved_path) -> QString {
    if (isOpaqueSourceIdentity(saved_path) || !QFileInfo(saved_path).isAbsolute()) {
      return saved_path;
    }
    return relocate(saved_path);
  });
}

void remapFileInfoFilenames(QDomDocument& doc, const DatasetPathRemapper& remap) {
  if (!remap) {
    return;
  }
  const QDomElement wrapper = doc.documentElement().firstChildElement(u"previouslyLoaded_Datafiles"_s);
  for (QDomElement file_info = wrapper.firstChildElement(u"fileInfo"_s); !file_info.isNull();
       file_info = file_info.nextSiblingElement(u"fileInfo"_s)) {
    // extractDataSource's selection rule.
    const QString filename = file_info.attribute(u"filename"_s);
    if (filename.isEmpty()) {
      continue;
    }
    const QString remapped = remap(filename);
    if (remapped != filename) {
      file_info.setAttribute(u"filename"_s, remapped);
    }
  }
}

void stripUnresolvedCurves(QDomDocument& doc) {
  std::vector<QDomNode> victims;
  forEachPlotCurve(doc, [&](const QDomElement& curve) {
    if (plot_xml::isPendingIntent(curve)) {
      return;
    }
    const bool has_ts_key = !curve.attribute(u"name"_s).isEmpty();
    const bool has_xy_keys = !curve.attribute(u"curve_x"_s).isEmpty() && !curve.attribute(u"curve_y"_s).isEmpty();
    if (!has_ts_key && !has_xy_keys) {
      victims.push_back(curve);
    }
  });
  for (QDomNode& v : victims) {
    v.parentNode().removeChild(v);
  }
}

void normalizePlotRangeBasis(QDomDocument& doc) {
  const QDomNodeList plots = doc.elementsByTagName(u"plot"_s);
  for (int index = 0; index < plots.size(); ++index) {
    const QDomElement plot = plots.at(index).toElement();
    if (plot.isNull()) {
      continue;
    }
    QDomElement range = plot.firstChildElement(u"range"_s);
    if (range.isNull() || range.hasAttribute(plot_xml::kXBasisAttribute)) {
      continue;
    }
    const bool is_xy = plot.attribute(u"mode"_s) == u"XYPlot"_s;
    range.setAttribute(plot_xml::kXBasisAttribute, is_xy ? plot_xml::kXBasisValue : plot_xml::kXBasisAbsolute);
  }
}

QDomElement writeSourceTimelineViewState(QDomDocument& doc, const SourceTimelineViewState& state) {
  QDomElement element = doc.createElement(u"source_timeline"_s);
  if (state.zoom) {
    // 17 significant digits: the ~1e-7 zoom must round-trip exactly.
    element.setAttribute(u"zoom"_s, QString::number(*state.zoom, 'g', 17));
  }
  if (state.scroll_left_ns) {
    element.setAttribute(u"scroll_left_ns"_s, QString::number(*state.scroll_left_ns));
  }
  if (state.scroll_top_px) {
    element.setAttribute(u"scroll_top_px"_s, QString::number(*state.scroll_top_px));
  }
  if (state.name_column_width) {
    element.setAttribute(u"name_column_width"_s, QString::number(*state.name_column_width));
  }
  if (state.snap) {
    element.setAttribute(u"snap"_s, *state.snap ? u"true"_s : u"false"_s);
  }
  return element;
}

SourceTimelineViewState readSourceTimelineViewState(const QDomElement& element) {
  SourceTimelineViewState state;
  if (element.isNull()) {
    return state;
  }
  bool ok = false;
  if (element.hasAttribute(u"zoom"_s)) {
    const double zoom = element.attribute(u"zoom"_s).toDouble(&ok);
    if (ok && zoom > 0.0) {
      state.zoom = zoom;
    }
  }
  if (element.hasAttribute(u"scroll_left_ns"_s)) {
    const qint64 left_ns = element.attribute(u"scroll_left_ns"_s).toLongLong(&ok);
    if (ok) {
      state.scroll_left_ns = left_ns;
    }
  }
  if (element.hasAttribute(u"scroll_top_px"_s)) {
    const int top_px = element.attribute(u"scroll_top_px"_s).toInt(&ok);
    if (ok && top_px >= 0) {
      state.scroll_top_px = top_px;
    }
  }
  if (element.hasAttribute(u"name_column_width"_s)) {
    const int width = element.attribute(u"name_column_width"_s).toInt(&ok);
    if (ok && width > 0) {
      state.name_column_width = width;
    }
  }
  if (element.hasAttribute(u"snap"_s)) {
    state.snap = element.attribute(u"snap"_s) == "true"_L1;
  }
  return state;
}

bool isSamePath(const QString& a, const QString& b) {
  if (a.isEmpty() || b.isEmpty()) {
    return false;
  }
  const bool a_is_opaque = isOpaqueSourceIdentity(a);
  const bool b_is_opaque = isOpaqueSourceIdentity(b);
  if (a_is_opaque || b_is_opaque) {
    return a_is_opaque && b_is_opaque && a == b;
  }
  const QString canon_a = QFileInfo(a).canonicalFilePath();
  const QString canon_b = QFileInfo(b).canonicalFilePath();
  // canonicalFilePath() is empty for a missing file: two missing files are not the same.
  if (canon_a.isEmpty() || canon_b.isEmpty()) {
    return false;
  }
  return canon_a == canon_b;
}

}  // namespace PJ::layout_xml
