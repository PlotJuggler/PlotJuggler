// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The app's own MCP tools: each a thin call into an existing service or MainWindow action.

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSettings>
#include <QTemporaryFile>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>

#include "FileLoader.h"
#include "LoadInput.h"
#include "MainWindow.h"
#include "PluginPlotTabsController.h"
#include "StreamingSourceManager.h"
#include "mcp/McpServer.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/FeatureCatalogHost.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/PluginRuntimeCatalog.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/agent_tools/derived_recipes.hpp"
#include "pj_scripting/filter_catalogue.h"
#ifdef PJ_TARGET_WASM
#include "mcp/McpBrowserLink.h"
#include "pj_runtime/BrowserFetch.h"
#include "pj_runtime/PresetCatalog.h"
#endif

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

/// export_series refuses more rows than this instead of writing a file nobody can load.
constexpr std::size_t kMaxExportRows = 10'000'000;

json ok(const json& payload) {
  return mcp::textResult(payload.dump(-1, ' ', false, json::error_handler_t::replace));
}

json fail(const std::string& message) {
  return mcp::textResult(message, true);
}

QString qs(const json& args, const char* key) {
  const auto it = args.find(key);
  return it != args.end() && it->is_string() ? QString::fromStdString(it->get<std::string>()) : QString();
}

json parseOrNull(const QString& config) {
  return config.isEmpty() ? json() : json::parse(config.toStdString(), nullptr, false);
}

/// A string array argument; nullopt when absent or not a non-empty array of strings.
std::optional<std::vector<std::string>> strings(const json& args, const char* key) {
  const auto it = args.find(key);
  if (it == args.end() || !it->is_array() || it->empty() ||
      !std::all_of(it->begin(), it->end(), [](const json& v) { return v.is_string(); })) {
    return std::nullopt;
  }
  return it->get<std::vector<std::string>>();
}

/// The transform id (and script class id) a series name maps to: letters and digits kept, every
/// other byte as `_xx` hex, so two names never share an id ("a/b" and "a_b" stay apart).
std::string transformId(const std::string& name) {
  std::string id;
  for (const char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 128 && std::isalnum(u) != 0) {
      id += c;
    } else {
      std::array<char, 4> hex{};
      std::snprintf(hex.data(), hex.size(), "_%02x", u);
      id += hex.data();
    }
  }
  return id;
}

/// The title prefix of a dialog an agent opens for the user (ask_user).
QString agentDialogTitlePrefix() {
  return QCoreApplication::translate("PJ::McpServer", "An AI agent asked you to choose: ");
}

/// The value column's rows [begin, end) as doubles; empty for a column type that is not numeric.
std::vector<double> valuesAsDouble(const sdk::MaterializedSeriesView& series, std::size_t begin, std::size_t end) {
  std::vector<double> out(end - begin);
  const auto copy = [&](const auto* values) {
    if (values == nullptr) {
      return false;
    }
    std::transform(values + begin, values + end, out.begin(), [](auto v) { return static_cast<double>(v); });
    return true;
  };
  if (copy(series.valuesAsFloat64()) || copy(series.valuesAsFloat32()) || copy(series.valuesAsInt64()) ||
      copy(series.valuesAsInt32()) || copy(series.valuesAsInt16()) || copy(series.valuesAsInt8()) ||
      copy(series.valuesAsUint64()) || copy(series.valuesAsUint32()) || copy(series.valuesAsUint16()) ||
      copy(series.valuesAsUint8())) {
    return out;
  }
  return {};
}

/// One resolved series (or its windowed slice).
struct Column {
  std::string path;       ///< the canonical path, dataset-qualified when several datasets are loaded
  DatasetId dataset = 0;  ///< 0 when the catalog names no source for it
  std::vector<int64_t> stamps;
  std::vector<double> values;
};

/// The raw-ns span [first, last] of a dataset's samples to read.
using RawWindow = std::function<std::pair<int64_t, int64_t>(DatasetId)>;

/// Resolves and reads `paths` the way read_series does (suffixes, "dataset:topic/field", the
/// near-miss candidates in the error); a path resolving to a series already read is skipped. With
/// `window`, only the samples inside it are kept, plus the one before it (its value holds at
/// the window's start), and a context with `read_window` decodes only those.
Expected<std::vector<Column>> readColumns(
    const agent_tools::ToolContext& context, const std::vector<std::string>& paths, const RawWindow& window = {}) {
  const sdk::ToolboxHostView& host = context.host;
  auto catalog = host.catalogSnapshot();
  if (!catalog) {
    return unexpected("catalog unavailable: " + catalog.error());
  }
  const agent_tools::TopicDatasetMap topic_dataset = agent_tools::datasetByTopicIndex(*catalog);
  std::vector<Column> columns;
  for (const std::string& want : paths) {
    const agent_tools::SeriesLookup lookup = agent_tools::resolveSeriesPath(*catalog, want, topic_dataset);
    if (!lookup.resolved) {
      return unexpected(agent_tools::seriesLookupError(want, lookup));
    }
    if (std::any_of(columns.begin(), columns.end(), [&](const Column& c) { return c.path == lookup.resolved->path; })) {
      continue;
    }
    Column column{.path = lookup.resolved->path, .dataset = 0, .stamps = {}, .values = {}};
    if (const auto source = agent_tools::dataSourceHandleFor(*catalog, lookup.resolved->dataset)) {
      column.dataset = source->id;
    }
    const std::optional<std::pair<int64_t, int64_t>> bounds =
        window ? std::optional(window(column.dataset)) : std::nullopt;
    auto view = bounds && context.read_window
                    ? context.read_window(lookup.resolved->handle, bounds->first, bounds->second)
                    : host.readSeries(lookup.resolved->handle);
    if (!view) {
      return unexpected("read failed for '" + want + "': " + view.error());
    }
    const auto stamps = view->timestamps();
    std::size_t begin = 0;
    std::size_t end = stamps.size();
    if (bounds) {
      const auto [first, last] = *bounds;
      begin = static_cast<std::size_t>(std::lower_bound(stamps.begin(), stamps.end(), first) - stamps.begin());
      begin = begin > 0 ? begin - 1 : 0;
      end = static_cast<std::size_t>(std::upper_bound(stamps.begin(), stamps.end(), last) - stamps.begin());
      end = std::max(begin, end);
    }
    if (end - begin > kMaxExportRows) {
      return unexpected(
          "series '" + want + "' has more than " + std::to_string(kMaxExportRows) +
          " samples in the window: narrow t0/t1");
    }
    column.values = valuesAsDouble(*view, begin, end);
    if (column.values.size() != end - begin) {
      return unexpected("series '" + want + "' is not numeric");
    }
    column.stamps.assign(
        stamps.begin() + static_cast<std::ptrdiff_t>(begin), stamps.begin() + static_cast<std::ptrdiff_t>(end));
    columns.push_back(std::move(column));
  }
  return columns;
}

/// The loader config the file at `path` was loaded with.
std::optional<SessionManager::LoadedSource> loadedConfig(const SessionManager& manager, const QString& path) {
  for (const SessionManager::LoadedSource& source : manager.loadedSources()) {
    if (!path.isEmpty() && FileLoader::sameSourceIdentity(source.path, path)) {
      return source;
    }
  }
  return std::nullopt;
}

/// A CSV header field: quoted, embedded quotes doubled.
std::string csvField(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    out += c == '"' ? std::string("\"\"") : std::string(1, c);
  }
  return out + '"';
}

/// The header of a .npy file (format 3.0: UTF-8 field names) holding `rows` records of float64 fields: "time", then
/// one per series, named by its path. numpy.load reads it as a structured array.
std::string npyHeader(const json& names, std::size_t rows) {
  const auto quoted = [](const std::string& text) {
    std::string out = "'";
    for (const char c : text) {
      if (c == '\\' || c == '\'') {
        out += '\\';
      }
      out += c;
    }
    return out + "'";
  };
  std::string dict = "{'descr': [('time', '<f8')";
  for (const json& name : names) {
    dict += ", (" + quoted(name.get<std::string>()) + ", '<f8')";
  }
  dict += "], 'fortran_order': False, 'shape': (" + std::to_string(rows) + ",), }";
  // Magic, version and length come first, and the data must start on a 64-byte boundary.
  constexpr std::size_t kPrefix = 6 + 2 + 4;
  dict.append((64 - (kPrefix + dict.size() + 1) % 64) % 64, ' ');
  dict += '\n';
  std::string out = "\x93NUMPY";
  out += {'\x03', '\x00'};
  for (int shift = 0; shift < 32; shift += 8) {
    out += static_cast<char>((dict.size() >> shift) & 0xFF);
  }
  return out + dict;
}

}  // namespace

const std::unordered_map<std::string, McpServer::AppTool>& McpServer::appTools() {
  static const std::unordered_map<std::string, AppTool> tools = {
      {"get_status", &McpServer::getStatus},
#ifndef PJ_TARGET_WASM  // no feature catalog in the browser
      {"about", &McpServer::about},
#endif
      {"open_file", &McpServer::openFile},
      {"reload_file", &McpServer::reloadFile},
      {"unload_file", &McpServer::unloadFile},
      {"start_stream", &McpServer::startStream},
      {"stop_stream", &McpServer::stopStream},
#ifndef PJ_TARGET_WASM  // the file would land in the page's memory, out of the agent's reach
      {"export_series", &McpServer::exportSeries},
#endif
      {"add_script_series", &McpServer::addScriptSeries},
      {"add_filter", &McpServer::addFilter},
      {"remove_script_series", &McpServer::removeScriptSeries},
      {"screenshot", &McpServer::screenshot},
  };
  return tools;
}

void McpServer::awaitLoad(quint64 ticket, Reply reply, std::function<json(quint32)> on_loaded) {
  auto* receiver = new QObject(this);
  connect(
      &window_.fileLoader(), &FileLoader::loadFinished, receiver,
      [receiver, ticket, reply, on_loaded](
          quint64 settled, LoadOutcome outcome, const QString& reason, DatasetId id, const QVector<DatasetId>&) {
        if (settled != ticket) {
          return;
        }
        receiver->deleteLater();
        if (outcome == LoadOutcome::kCancelled) {
          reply(fail("the load was cancelled (the user closed the dialog or stopped the import)"));
        } else if (outcome != LoadOutcome::kLoaded) {
          reply(fail("load failed: " + reason.toStdString()));
        } else {
          reply(on_loaded(id));
        }
      });
}

void McpServer::getStatus(const json&, const Caller& caller, Reply reply) {
  AppSession& session = window_.appSession();
  SessionManager& manager = session.sessionManager();
  StreamingSourceManager& streams = window_.streamingSources();
  json datasets = json::array();
  for (const auto& [id, dataset_name] : session.catalogModel().datasets()) {
    const QString path = manager.datasetSourcePath(id);
    json entry = {{"id", id}, {"name", dataset_name.toStdString()}, {"path", path.toStdString()}};
    if (const auto range = manager.datasetDisplayRange(id)) {
      entry["time_range"] = {range->min.value, range->max.value};  // datasets may not share one clock
    }
    if (const auto source = loadedConfig(manager, path)) {
      entry["loader"] = source->plugin_id.toStdString();
      entry["config"] = parseOrNull(source->plugin_config_json);
    }
    if (const QString stream_id = streams.layoutSourceId(id); !stream_id.isEmpty()) {
      for (const auto& live : streams.layoutSources()) {
        if (live.id == stream_id) {
          entry["stream"] = live.plugin_id.toStdString();
          entry["config"] = parseOrNull(live.plugin_config_json);
        }
      }
    }
    datasets.push_back(std::move(entry));
  }
  json stream_sources = json::array();
  for (const LoadedDataSource* source : session.extensionCatalog().streamSources()) {
    stream_sources.push_back(source->name);
  }
  json tab_ids = json::array();
  if (auto ids = window_.pluginPlotTabs().listTabIds(caller.owner); ids.has_value()) {
    tab_ids = *ids;
  }
  const QWidget* modal = QApplication::activeModalWidget();
  PlaybackEngine& playback = session.playbackEngine();
  reply(
      ok({{"datasets", datasets},
          {"stream_sources", stream_sources},
          {"time_range", {playback.rangeMin().value, playback.rangeMax().value}},
          {"current_time", playback.currentTime().value},
          {"playing", playback.isPlaying()},
          {"agent", caller.owner.toStdString()},
          {"agent_tabs", tab_ids},
          {"focused", QApplication::activeWindow() != nullptr},
          {"user_dialog", modal != nullptr ? json(shownTitle(*modal)) : json()}}));
}

void McpServer::about(const json&, const Caller&, Reply reply) {
#ifdef PJ_TARGET_WASM
  reply(fail("no feature catalog"));
#else
  FeatureCatalogHost* features = window_.featureCatalog();
  reply(features != nullptr ? ok(json::parse(features->document(), nullptr, false)) : fail("no feature catalog"));
#endif
}

void McpServer::openFile(const json& args, const Caller& caller, Reply reply) {
  LoadHints hints;
  hints.still_allowed = stillAuthorized(caller);  // the load may wait in the queue, then for the user
  const bool ask_user = args.value("ask_user", false);
  hints.dialog_policy = ask_user ? DialogPolicy::kInteractive : DialogPolicy::kNever;
  if (ask_user) {
    hints.dialog_title_prefix = agentDialogTitlePrefix();
  } else if (const auto config = args.find("config"); config != args.end() && config->is_object()) {
    hints.preset_config_json = QString::fromStdString(config->dump());
  }
#ifdef PJ_TARGET_WASM
  // The page cannot read the agent's disk: the relay serves the file the agent named and passes its
  // url (and the file's name) instead of the path. The download streams into a staged MEMFS file.
  const QUrl url(qs(args, "url"));
  if (!McpBrowserLink::acceptableRelay(url)) {
    reply(fail("in the browser, open_file needs the PlotJuggler relay (pj_mcp_relay.py) to serve the file"));
    return;
  }
  const QString name = args.contains("name") ? qs(args, "name") : url.fileName();
  BrowserFileStore::StageResult staged = agent_files_.reserve(name);
  if (!staged.input.has_value()) {
    reply(fail("cannot stage " + name.toStdString() + ": " + staged.error.toStdString()));
    return;
  }
  const QString staged_path = staged.input->backing_path;
  browserFetchToFile(
      url, staged_path, kMaxPresetDataBytes, std::chrono::seconds(60),
      [self = QPointer<McpServer>(this), caller, hints = std::move(hints), reply,
       input = std::move(*staged.input)](BrowserFetchResult file) mutable {
        // Dropping `input` (every early return) releases its lease, which removes the partial file.
        if (self == nullptr || !self->authorized(caller)) {
          return;
        }
        if (!file.error.isEmpty()) {
          reply(fail("cannot download the file from the relay: " + file.error.toStdString()));
          return;
        }
        input.content_sha256 = std::move(file.sha256);
        self->startLoad(std::move(input), std::move(hints), std::move(reply));
      });
#else
  const QFileInfo info(qs(args, "path"));
  if (!info.isAbsolute() || !info.isFile()) {
    reply(fail("path must be an absolute path to an existing file"));
    return;
  }
  startLoad(LoadInput::fromNativePath(info.absoluteFilePath()), std::move(hints), std::move(reply));
#endif
}

void McpServer::startLoad(LoadInput input, LoadHints hints, Reply reply) {
  const LoadRequestId ticket = window_.fileLoader().loadFileTicketed(std::move(input), nullptr, hints);
  if (ticket == 0) {
    reply(fail("load rejected (no plugin for this file type?)"));
    return;
  }
  // What was actually loaded (the user's dialog choices included), so the agent need not guess.
  awaitLoad(ticket, reply, [this](DatasetId id) {
    const SessionManager& manager = window_.appSession().sessionManager();
    json result = {{"loaded", true}, {"dataset_id", id}};
    if (const auto source = loadedConfig(manager, manager.datasetSourcePath(id))) {
      result["config"] = parseOrNull(source->plugin_config_json);
    }
    return ok(result);
  });
}

void McpServer::reloadFile(const json& args, const Caller& caller, Reply reply) {
  const DatasetId id = resolveDataset(args.value("dataset", json()));
  const LoadRequestId ticket = id == 0 ? 0 : window_.reloadDataset(id, DialogPolicy::kNever, stillAuthorized(caller));
  if (ticket == 0) {
    reply(fail("no file-backed dataset matches (see get_status)"));
    return;
  }
  awaitLoad(ticket, reply, [](DatasetId reloaded) { return ok({{"reloaded", true}, {"dataset_id", reloaded}}); });
}

void McpServer::unloadFile(const json& args, const Caller& caller, Reply reply) {
  const DatasetId id = resolveDataset(args.value("dataset", json()));
  if (id == 0) {
    reply(fail("no such dataset (see get_status)"));
    return;
  }
  // The reply waits for the user when derived series depend on the dataset: one outcome either way.
  window_.confirmAndRemoveDependentTransforms(
      window_.appSession().sessionManager().dataEngine().listTopics(id),
      [this, id, reply] {
        window_.removeDatasets({id});
        reply(ok({{"unloaded", id}}));
      },
      stillAuthorized(caller),
      [reply] { reply(fail("not unloaded: the user kept the derived series that depend on this dataset")); });
}

void McpServer::startStream(const json& args, const Caller& caller, Reply reply) {
  const QString source = qs(args, "source");
  const auto& sources = window_.appSession().extensionCatalog().streamSources();
  if (std::none_of(sources.begin(), sources.end(), [&source](const LoadedDataSource* s) {
        return QString::fromStdString(s->name) == source;
      })) {
    reply(fail("unknown stream source '" + source.toStdString() + "' (get_status lists stream_sources)"));
    return;
  }
  if (args.value("ask_user", false)) {
    // The source's dialog is modal: open it once this request has returned, and only if no other
    // dialog (another agent's request queued alongside, or the user's own) came up meanwhile and
    // the caller may still act.
    QTimer::singleShot(0, this, [this, source, reply, allowed = stillAuthorized(caller)] {
      if (!allowed()) {
        return;
      }
      if (const QWidget* modal = QApplication::activeModalWidget()) {
        reply(userDialogOpen(*modal));
        return;
      }
      StreamingSourceManager& streams = window_.streamingSources();
      DatasetId started = 0;
      QString failure;
      QObject guard;
      connect(&streams, &StreamingSourceManager::streamStarted, &guard, [&started](DatasetId id) { started = id; });
      connect(&streams, &StreamingSourceManager::setupError, &guard, [&failure](const QString& m) { failure = m; });
      connect(&streams, &StreamingSourceManager::streamError, &guard, [&failure](DatasetId, const QString& m) {
        failure = m;
      });
      streams.startSession(source, agentDialogTitlePrefix(), allowed);
      if (started != 0) {
        reply(ok({{"dataset_id", started}, {"note", "topics appear as the source advertises them: list_topics"}}));
      } else {
        reply(fail(failure.isEmpty() ? "the user cancelled the stream dialog" : failure.toStdString()));
      }
    });
    return;
  }
  // No dialog: the agent's config, else what the user last confirmed in this source's dialog.
  const auto given = args.find("config");
  const QString config = given != args.end() && given->is_object()
                             ? QString::fromStdString(given->dump())
                             : QSettings().value(u"PluginConfig/"_s + source).toString();
  layout_xml::StreamingSourceRef ref;
  ref.id = u"stream"_s;
  ref.plugin_id = source;
  ref.plugin_config_json = config;
  QString error;
  const DatasetId id = window_.streamingSources().startFromLayout(ref, config, error);
  if (id == 0) {
    reply(fail(error.toStdString()));
    return;
  }
  reply(
      ok({{"dataset_id", id},
          {"config", parseOrNull(config)},
          {"note", "topics appear as the source advertises them: list_topics"}}));
}

void McpServer::stopStream(const json& args, const Caller&, Reply reply) {
  const DatasetId id = resolveDataset(args.value("dataset", json()));
  if (id == 0 || !window_.streamingSources().stopDatasetAndWait(id, u"stopped by the agent"_s)) {
    reply(fail("no live stream matches (get_status marks live ones with stream)"));
    return;
  }
  reply(ok({{"stopped", id}, {"note", "its data stays loaded; unload_file removes it"}}));
}

void McpServer::exportSeries(const json& args, const Caller& caller, Reply reply) {
  const auto paths = strings(args, "paths");
  if (!paths) {
    reply(fail("paths: a list of series paths"));
    return;
  }
  const std::string format = args.value("format", "csv");
  if (format != "csv" && format != "npy") {
    reply(fail("format: csv or npy"));
    return;
  }
  const bool npy = format == "npy";
  const QString requested_path = qs(args, "path");
  if (!requested_path.isEmpty() && !QFileInfo(requested_path).isAbsolute()) {
    reply(fail("path must be absolute"));
    return;
  }
  constexpr int64_t kMinNs = std::numeric_limits<int64_t>::min();
  constexpr int64_t kMaxNs = std::numeric_limits<int64_t>::max();
  const double t0 = args.value("t0", -std::numeric_limits<double>::infinity());
  const double t1 = args.value("t1", std::numeric_limits<double>::infinity());
  const double rate = args.value("rate_hz", 0.0);
  std::optional<agent_tools::ToolContext> context = agentContext(caller.owner);
  if (!context) {
    reply(fail(mcp::kNoServicesError));
    return;
  }
  const SessionManager& manager = window_.appSession().sessionManager();
  // Display seconds, as playback and read_series windows take them: one offset per dataset.
  const auto offsetOf = [&manager](DatasetId dataset) {
    return manager.displayTimeForSource(dataset, 0).has_value() ? manager.displayOffset(dataset) : DisplayOffset{};
  };
  // Saturates instead of overflowing: a far-off bound plus an epoch offset can exceed int64 ns.
  const auto toRaw = [](double seconds, DisplayOffset offset, int64_t unbounded) {
    if (!std::isfinite(seconds)) {
      return unbounded;
    }
    const double raw_ns = seconds * 1e9 + static_cast<double>(offset.value.count());
    if (raw_ns >= 9.2e18) {
      return kMaxNs;
    }
    if (raw_ns <= -9.2e18) {
      return kMinNs;
    }
    return displaySecondsToRaw(DisplaySeconds(seconds), offset);
  };
  auto columns = readColumns(*context, *paths, [&](DatasetId dataset) {
    const DisplayOffset offset = offsetOf(dataset);
    return std::pair{toRaw(t0, offset, kMinNs), toRaw(t1, offset, kMaxNs)};
  });
  if (!columns) {
    reply(fail(columns.error()));
    return;
  }
  // Times stay integer display nanoseconds (each column's stamps, shifted in place), so rows a few ns apart stay apart
  // and print exactly: as double seconds, an epoch-sized display time resolves only ~0.2 us.
  for (Column& column : *columns) {
    const int64_t offset_ns = offsetOf(column.dataset).value.count();
    for (int64_t& stamp : column.stamps) {
      stamp = checkedTimelineDifference(stamp, offset_ns).value_or(stamp < 0 ? kMinNs : kMaxNs);
    }
  }
  const int64_t t0_ns = toRaw(t0, DisplayOffset{}, kMinNs);
  const int64_t t1_ns = toRaw(t1, DisplayOffset{}, kMaxNs);
  // Rows: a uniform grid at rate_hz, else every sample time of any series, inside [t0, t1].
  std::vector<int64_t> stamps_grid;
  int64_t lo = kMaxNs;
  std::size_t rows = 0;
  if (rate > 0) {
    int64_t hi = kMinNs;
    for (const Column& column : *columns) {
      if (!column.stamps.empty()) {
        lo = std::min(lo, column.stamps.front());
        hi = std::max(hi, column.stamps.back());
      }
    }
    lo = std::max(lo, t0_ns);
    hi = std::min(hi, t1_ns);
    const double span = (static_cast<double>(hi) - static_cast<double>(lo)) * 1e-9 * rate;
    rows = hi >= lo
               ? (span < static_cast<double>(kMaxExportRows) ? static_cast<std::size_t>(span) + 1 : kMaxExportRows + 1)
               : 0;
  } else {
    // Each series is in time order, so a merge builds the grid; a sort of every stamp costs ~20x as much.
    std::vector<int64_t> merged;
    for (const Column& column : *columns) {
      const auto first = std::lower_bound(column.stamps.begin(), column.stamps.end(), t0_ns);
      const auto last = std::upper_bound(first, column.stamps.end(), t1_ns);
      merged.clear();
      merged.reserve(stamps_grid.size() + static_cast<std::size_t>(last - first));
      std::set_union(stamps_grid.begin(), stamps_grid.end(), first, last, std::back_inserter(merged));
      merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
      stamps_grid.swap(merged);
      if (stamps_grid.size() > kMaxExportRows) {
        break;  // the grid only grows: refused below, without merging the rest
      }
    }
    rows = stamps_grid.size();
  }
  if (rows > kMaxExportRows) {
    reply(fail(
        "the export would have more than " + std::to_string(kMaxExportRows) +
        " rows: narrow t0/t1, lower rate_hz, or export fewer series"));
    return;
  }
  // The default file is created new, owner-only and randomly named, so no one can plant it first.
  std::unique_ptr<QFile> file;
  if (requested_path.isEmpty()) {
    auto temp = std::make_unique<QTemporaryFile>(
        QDir::temp().filePath(npy ? u"plotjuggler_export_XXXXXX.npy"_s : u"plotjuggler_export_XXXXXX.csv"_s));
    temp->setAutoRemove(false);
    if (temp->open()) {
      file = std::move(temp);
    }
  } else {
    // An agent often names a folder that does not exist yet.
    QDir().mkpath(QFileInfo(requested_path).absolutePath());
    auto named = std::make_unique<QFile>(requested_path);
    if (named->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      file = std::move(named);
    }
  }
  if (file == nullptr) {
    reply(fail("cannot create " + (requested_path.isEmpty() ? QDir::tempPath() : requested_path).toStdString()));
    return;
  }
  const QString path = file->fileName();
  bool written = true;
  json names = json::array();
  for (const Column& column : *columns) {
    names.push_back(column.path);
  }
  std::string out = npy ? npyHeader(names, rows) : "time";
  for (const Column& column : *columns) {
    out += npy ? std::string() : ',' + csvField(column.path);
  }
  out += npy ? "" : "\n";
  const auto flush = [&] {
    written = written && file->write(out.data(), static_cast<qint64>(out.size())) == static_cast<qint64>(out.size());
    out.clear();
  };
  // Each value as the shortest text that reads back to the same double, in the C locale whatever the process's (a
  // decimal comma would split a column). std::to_chars is ~2.5x faster than QByteArray::number, but Apple's libc++ has
  // its floating-point overloads only from macOS 13.3, above Qt's 13.0 floor, and does not advertise them.
  std::array<char, 32> digits{};
  const auto append_value = [&](double value) {
    if (std::isnan(value)) {
      out += "nan";  // one spelling: to_chars writes a computed NaN as "-nan", or "-nan(ind)" with MSVC
      return;
    }
#if defined(__cpp_lib_to_chars)
    out.append(digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr);
#else
    const QByteArray text = QByteArray::number(value, 'g', QLocale::FloatingPointShortest);
    out.append(text.constData(), static_cast<std::size_t>(text.size()));
#endif
  };
  // The time column: exact display seconds, to the nanosecond.
  const auto append_time = [&out, &digits](int64_t ns) {
    const uint64_t magnitude = ns < 0 ? 0 - static_cast<uint64_t>(ns) : static_cast<uint64_t>(ns);
    if (ns < 0) {
      out += '-';
    }
    out.append(
        digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), magnitude / 1'000'000'000).ptr);
    std::array<char, 10> fraction{'.', '0', '0', '0', '0', '0', '0', '0', '0', '0'};
    for (uint64_t rest = magnitude % 1'000'000'000, i = 9; rest > 0; rest /= 10, --i) {
      fraction[i] = static_cast<char>('0' + rest % 10);
    }
    out.append(fraction.data(), fraction.size());
  };
  static_assert(std::endian::native == std::endian::little, "npy records are written as little-endian <f8");
  const auto append_raw = [&out](double value) { out.append(reinterpret_cast<const char*>(&value), sizeof value); };
  // Sample-and-hold: each row holds every series' last value at or before its time (empty, or NaN in a .npy, before
  // the first).
  std::vector<std::size_t> cursor(columns->size(), 0);
  for (std::size_t row = 0; row < rows && written; ++row) {
    const int64_t t = rate > 0 ? lo + std::llround(static_cast<double>(row) * 1e9 / rate) : stamps_grid[row];
    if (npy) {
      // Whole seconds and the rest apart, so the sum rounds once, to the double nearest the exact time.
      append_raw(static_cast<double>(t / 1'000'000'000) + static_cast<double>(t % 1'000'000'000) * 1e-9);
    } else {
      append_time(t);
    }
    for (std::size_t c = 0; c < columns->size(); ++c) {
      const Column& column = (*columns)[c];
      while (cursor[c] < column.stamps.size() && column.stamps[cursor[c]] <= t) {
        ++cursor[c];
      }
      const bool held = cursor[c] > 0;
      if (npy) {
        append_raw(held ? column.values[cursor[c] - 1] : std::numeric_limits<double>::quiet_NaN());
        continue;
      }
      out += ',';
      if (held) {
        append_value(column.values[cursor[c] - 1]);
      }
    }
    if (!npy) {
      out += '\n';
    }
    if (out.size() > (1U << 22)) {
      flush();
    }
  }
  flush();
  written = written && file->flush();
  file->close();
  written = written && file->error() == QFileDevice::NoError;
  if (!written) {
    const std::string why = file->errorString().toStdString();
    file->remove();
    reply(fail("writing " + path.toStdString() + " failed: " + why));
    return;
  }
  reply(ok(
      {{"path", path.toStdString()},
       {"rows", rows},
       {"columns", names},
       {"format", format},
       {"note", npy ? "numpy.load(path) is a structured array: d['time'] in display seconds (as playback), d[<column>] "
                      "for each series, sample-and-hold, NaN before a series starts; pandas.DataFrame(d) makes a table"
                    : "time in display seconds (as playback); values sample-and-hold, empty before a series starts"}}));
}

void McpServer::addScriptSeries(const json& args, const Caller& caller, Reply reply) {
  // A plugin-style transform owned by the agent: listed under Custom Topics, saved in layouts with
  // its script, and outside undo/redo like the agent's tabs.
  const std::string series = args.value("name", "");
  const std::string given_script = args.value("script", "");
  const auto inputs = strings(args, "inputs");
  if (series.empty() || given_script.empty() || !inputs) {
    reply(fail("name, script and inputs (a list of series paths) are required"));
    return;
  }
  std::optional<agent_tools::ToolContext> context = agentContext(caller.owner);
  if (!context) {
    reply(fail(mcp::kNoServicesError));
    return;
  }
  auto columns = readColumns(*context, *inputs);
  if (!columns) {
    reply(fail(columns.error()));
    return;
  }
  // Inputs are joined on equal timestamps: refuse a join that would compute nothing.
  std::vector<int64_t> shared = columns->front().stamps;
  for (const Column& column : *columns) {
    std::vector<int64_t> next;
    std::set_intersection(
        shared.begin(), shared.end(), column.stamps.begin(), column.stamps.end(), std::back_inserter(next));
    shared = std::move(next);
  }
  if (shared.empty()) {
    std::string why = "these inputs share no timestamps, so the series would have 0 points:";
    for (const Column& column : *columns) {
      why += " " + column.path + " (" + std::to_string(column.stamps.size()) + " samples)";
    }
    reply(fail(why + ". Use inputs from one message, or resample one of them first."));
    return;
  }
  std::vector<std::string> input_names;
  for (const Column& column : *columns) {
    input_names.push_back(column.path);
  }
  // Several outputs live under the series name, so two scripts' `x` never collide.
  std::vector<std::string> outputs{series};
  if (const auto named = strings(args, "outputs")) {
    outputs.clear();
    for (const std::string& output : *named) {
      outputs.push_back(series + "/" + output);
    }
  }
  // The transform id must equal the script's class id: stamp it onto whatever class the script
  // defines (Luau: returns one; Python: class T).
  const std::string transform_id = transformId(series);
  std::string script = given_script;
  if (script.starts_with("# pj-script: python")) {
    script += "\nT.id = \"" + transform_id + "\"\n";
  } else {
    if (script.starts_with("-- pj-script:")) {
      script = script.substr(script.find('\n') + 1);
    }
    script = "-- pj-script: luau\nlocal agent_class = (function()\n" + script + "\nend)()\nagent_class.id = \"" +
             transform_id + "\"\nreturn agent_class\n";
  }
  SessionManager& manager = window_.appSession().sessionManager();
  auto created = manager.dataProcessorService().upsertTransform(
      caller.owner.toStdString(), transform_id, input_names, outputs, script,
      args.value("params", json::object()).dump(),
      /*ephemeral=*/false, /*input_column_index=*/0, /*history_exempt=*/true);
  if (!created.has_value()) {
    reply(fail(created.error()));
    return;
  }
  CatalogModel& catalog = window_.appSession().catalogModel();
  catalog.rebuildFromDatastore();
  window_.syncCustomTopicsPanel();
  json topics = json::object();  // each output topic with its plottable fields
  for (const std::string& output : created->outputs) {
    topics[output] = json::array();
  }
  for (const CatalogItem& item : catalog.items()) {
    const auto* scalar = std::get_if<ScalarFieldPayload>(&item.payload);
    if (scalar != nullptr && topics.contains(item.topic_name.toStdString())) {
      topics[item.topic_name.toStdString()].push_back(scalar->field_path.toStdString());
    }
  }
  reply(ok({{"topics", topics}}));
}

void McpServer::addFilter(const json& args, const Caller& caller, Reply reply) {
  // A built-in filter becomes an agent transform too: its catalogued class, defaults filled in.
  const std::string filter = args.value("filter", "");
  const std::string input = args.value("input", "");
  if (input.empty() || filter.empty()) {
    reply(fail("input and a built-in filter name are required"));
    return;
  }
  const DataProcessorService& processors = window_.appSession().sessionManager().dataProcessorService();
  const scripting::FilterCatalogue* catalogue = processors.filterCatalogue();
  const std::optional<std::string> script =
      catalogue != nullptr ? catalogue->standaloneClassScript(filter) : std::nullopt;
  if (!script) {
    std::string known;
    for (const auto& [id, name] : processors.availableFilters()) {
      known += (known.empty() ? "" : ", ") + id;
    }
    reply(fail("unknown built-in filter '" + filter + "'; available: " + known));
    return;
  }
  addScriptSeries(
      {{"name", args.value("name", input + "[" + filter + "]")},
       {"inputs", json::array({input})},
       {"script", *script},
       {"params", args.value("params", json::object())}},
      caller, std::move(reply));
}

void McpServer::removeScriptSeries(const json& args, const Caller& caller, Reply reply) {
  const Status removed = window_.appSession().sessionManager().dataProcessorService().removeOwnTransform(
      DataProcessorService::makeTransformKey(caller.owner.toStdString(), transformId(args.value("name", ""))));
  if (removed.has_value()) {
    window_.appSession().catalogModel().rebuildFromDatastore();
    window_.syncCustomTopicsPanel();
  }
  reply(removed.has_value() ? ok("ok") : fail(removed.error()));
}

void McpServer::screenshot(const json& args, const Caller& caller, Reply reply) {
  if (args.contains("tab")) {
    if (const Status focused = window_.pluginPlotTabs().focusTab(caller.owner, qs(args, "tab")); !focused) {
      reply(fail(focused.error()));
      return;
    }
  }
  grabScreenshot(args.value("max_width", 1600), [reply](const QByteArray& png) {
    reply(
        {{"content",
          json::array({{{"type", "image"}, {"mimeType", "image/png"}, {"data", png.toBase64().toStdString()}}})}});
  });
}

}  // namespace PJ
