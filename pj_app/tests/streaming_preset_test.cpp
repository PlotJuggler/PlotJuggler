// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A preset (a layout opened by URL) that records streaming sources starts them
// from their saved configuration, with no dialog, inside the same all-or-nothing
// transaction as its file sources; its views bind each stream's topics as they
// arrive. Runs on the desktop: the replay is the browser's byte-level path, and
// the test stream (mock_layout_stream_plugin.cpp) records the configuration it
// was given.

#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include "FileLoader.h"
#include "LayoutXml.h"
#include "MainWindow.h"
#include "SourceTimelineController.h"
#include "StreamingSourceManager.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "support/loader_test_support.h"
#include "support/preset_test_support.h"
#include "support/route_server.h"

#ifndef PJ_MOCK_FILE_SOURCE_PLUGIN_PATH
#error "PJ_MOCK_FILE_SOURCE_PLUGIN_PATH must be defined"
#endif
#ifndef PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH
#error "PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH must be defined"
#endif

using namespace Qt::StringLiterals;

namespace {

using PJ::PresetTestPeer;
using pj_app_test::closeDialogs;
using pj_app_test::pumpUntil;
using pj_app_test::RouteServer;
using pj_app_test::visibleDialogs;

constexpr auto kPluginId = "Mock Layout Stream";
constexpr auto kManifestId = "mock-layout-stream";

// One <streamInfo>; `dataset` is its optional <dataset> child.
struct Stream {
  QString id;
  QString config;
  QString dataset = {};
  QString plugin_id = QString::fromLatin1(kPluginId);
  QString manifest_id = QString::fromLatin1(kManifestId);
};

// A plot with its curves (raw <curve> elements).
struct Plot {
  QString id;
  QString curves;
};

QString streamCurve(const QString& topic, const QString& stream_id) {
  return uR"(<curve topic="%1" field="value" dataset_path="pj-stream://%2"/>)"_s.arg(topic, stream_id);
}

QString fileCurve(const QString& filename, const QString& field = u"value"_s) {
  return uR"(<curve topic="mock/file_data" field="%1" dataset_path="%2"/>)"_s.arg(field, filename);
}

// A source-bound layout: each plot in its own dock area, the files the mock
// file source replays, then the streams.
QByteArray streamLayout(const QList<Plot>& plots, const QList<Stream>& streams, const QStringList& files = {}) {
  QString areas;
  for (const Plot& plot : plots) {
    areas += uR"(<DockArea id="%1-area" name="%1"><plot id="%1" mode="TimeSeries">%2</plot></DockArea>)"_s.arg(
        plot.id, plot.curves);
  }
  QString file_infos;
  for (const QString& file : files) {
    file_infos +=
        uR"(<fileInfo filename="%1" prefix=""><plugin ID="Mock File Source" manifest_id="mock-file-source">)"
        uR"(<![CDATA[{}]]></plugin></fileInfo>)"_s.arg(file);
  }
  QString stream_infos;
  for (const Stream& stream : streams) {
    stream_infos +=
        uR"(<streamInfo id="%1"><plugin ID="%2" manifest_id="%3"><![CDATA[%4]]></plugin>%5</streamInfo>)"_s.arg(
            stream.id, stream.plugin_id, stream.manifest_id, stream.config, stream.dataset);
  }
  return uR"(<root pj4_version="5" binding="source"><tabbed_widget parent="main_window"><Tab id="t1" containers="1">)"
         uR"(<Container><DockSplitter count="%1" orientation="-">%2</DockSplitter></Container></Tab></tabbed_widget>)"
         uR"(<previouslyLoaded_Datafiles>%3</previouslyLoaded_Datafiles>)"
         uR"(<previouslyLoaded_StreamingSources>%4</previouslyLoaded_StreamingSources></root>)"_s
             .arg(QString::number(plots.size()), areas, file_infos, stream_infos)
             .toUtf8();
}

class StreamingPresetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(probe_dir_.isValid());
    ASSERT_TRUE(settings_dir_.isValid());
    // Settings of this process only: every pj_app_gui_tests process on the machine
    // (parallel ctest, other checkouts) clears the shared test file when it starts.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    for (const char* plugin : {PJ_MOCK_FILE_SOURCE_PLUGIN_PATH, PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH}) {
      const QString path = QString::fromUtf8(plugin);
      ASSERT_TRUE(QFile::copy(path, extensions_dir_.filePath(QFileInfo(path).fileName())));
    }
    qputenv("PJ_LAYOUT_STREAM_PROBE_FILE", probeFile().toLocal8Bit());
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_.path());
    QObject::connect(
        window_.get(), &PJ::MainWindow::layoutUrlLoadFinished, [this](const QUrl&, bool ok, const QString& reason) {
          EXPECT_EQ(ok, reason.isEmpty()) << reason.toStdString();
          outcomes_.push_back(ok ? u"ok"_s : reason);
        });
  }

  void TearDown() override {
    window_.reset();
    qunsetenv("PJ_LAYOUT_STREAM_PROBE_FILE");
  }

  [[nodiscard]] QString probeFile() const {
    return probe_dir_.filePath(u"stream.probe"_s);
  }

  // Every line the test stream recorded, in order.
  [[nodiscard]] QStringList probeLines() const {
    QFile file(probeFile());
    if (!file.open(QIODevice::ReadOnly)) {
      return {};
    }
    return QString::fromUtf8(file.readAll()).split(u'\n', Qt::SkipEmptyParts);
  }

  // Serves `layout` at /<name>/layout.pj4.xml and each file beside it, opens
  // it, waits for its one terminal and returns "ok" or the failure reason.
  QString loadPreset(const QString& name, const QByteArray& layout, const QStringList& files = {}) {
    const QString layout_path = u"/%1/layout.pj4.xml"_s.arg(name);
    server_.route(layout_path, layout);
    for (const QString& file : files) {
      server_.route(u"/%1/%2"_s.arg(name, file), file.toUtf8());
    }
    const qsizetype before = outcomes_.size();
    window_->loadLayoutFromUrl(server_.url(layout_path));
    EXPECT_TRUE(pumpUntil([&] { return outcomes_.size() > before; })) << name.toStdString();
    EXPECT_FALSE(PresetTestPeer::loadPending(*window_));
    pj_app_test::flushQueuedEvents();
    return outcomes_.size() > before ? outcomes_[before] : u"<unsettled>"_s;
  }

  [[nodiscard]] PJ::SessionManager& sessionManager() const {
    return PresetTestPeer::session(*window_).sessionManager();
  }

  // Live stream sessions by the record id a source-bound save would give them.
  [[nodiscard]] QHash<QString, PJ::DatasetId> streamSessions() const {
    QHash<QString, PJ::DatasetId> sessions;
    PJ::StreamingSourceManager& streams = PresetTestPeer::streams(*window_);
    for (const PJ::DatasetId dataset_id : sessionManager().dataEngine().listDatasets()) {
      if (const QString id = streams.layoutSourceId(dataset_id); !id.isEmpty()) {
        sessions.insert(id, dataset_id);
      }
    }
    return sessions;
  }

  // The dataset each curve of plot `state_id` is drawn from.
  [[nodiscard]] std::vector<PJ::DatasetId> plotDatasets(const QString& state_id) const {
    std::vector<PJ::DatasetId> datasets;
    PJ::PlotWidget* plot = PresetTestPeer::plot(*window_, state_id);
    if (plot == nullptr) {
      return datasets;
    }
    const PJ::CatalogModel& catalog = PresetTestPeer::session(*window_).catalogModel();
    for (const auto& curve : plot->curveList()) {
      const auto descriptor = catalog.curveDescriptor(curve.source_name);
      datasets.push_back(descriptor.has_value() ? descriptor->dataset_id : 0);
    }
    return datasets;
  }

  // Every file dataset's source identity, in catalog order.
  [[nodiscard]] QStringList fileSources() const {
    QStringList sources;
    for (const auto& [id, name] : PresetTestPeer::session(*window_).catalogModel().datasets()) {
      (void)name;
      if (const QString source = PresetTestPeer::fileLoader(*window_).sourcePathForDataset(id); !source.isEmpty()) {
        sources.push_back(source);
      }
    }
    return sources;
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir probe_dir_;
  QTemporaryDir settings_dir_;
  RouteServer server_;
  std::unique_ptr<PJ::MainWindow> window_;
  QStringList outcomes_;
};

// The saved configuration reaches the plugin as it was saved, filled in for this
// page: no dialog, no remembered settings in its place. Saving writes the
// template back; undo and redo never start a stream.
TEST_F(StreamingPresetTest, SavedConfigSkipsDialog) {
  const QString remembered = uR"({"topic":"settings/topic"})"_s;
  QSettings().setValue(u"PluginConfig/Mock Layout Stream"_s, remembered);
  window_->setDeploymentPageUrl(QUrl(u"https://robot.local:8443/app/index.html?preset=live"_s));
  const QString config_template =
      uR"({"_parser_config":"{\"embedded\":true}","topic":"robot/data","url":"${deployment.ws_origin}/bridge"})"_s;
  int dialogs_seen = 0;
  QTimer dialog_watch;
  QObject::connect(&dialog_watch, &QTimer::timeout, [&dialogs_seen] { dialogs_seen += visibleDialogs(); });
  dialog_watch.start(10);

  ASSERT_EQ(
      loadPreset(
          u"live"_s,
          streamLayout({{u"plot1"_s, streamCurve(u"robot/data"_s, u"robot"_s)}}, {{u"robot"_s, config_template}})),
      u"ok"_s);
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 1; }));
  dialog_watch.stop();
  EXPECT_EQ(dialogs_seen, 0);

  QString expanded = config_template;
  expanded.replace(u"${deployment.ws_origin}"_s, u"wss://robot.local:8443"_s);
  const QStringList probe = probeLines();
  ASSERT_GE(probe.size(), 2);
  EXPECT_EQ(probe.mid(0, 2), (QStringList{u"load "_s + expanded, u"start"_s}));
  EXPECT_EQ(QSettings().value(u"PluginConfig/Mock Layout Stream"_s).toString(), remembered);

  const QHash<QString, PJ::DatasetId> sessions = streamSessions();
  ASSERT_EQ(sessions.keys(), QStringList{u"robot"_s});
  EXPECT_EQ(plotDatasets(u"plot1"_s), std::vector<PJ::DatasetId>{sessions.value(u"robot"_s)});
  const QList<PJ::layout_xml::StreamingSourceRef> records = PresetTestPeer::streams(*window_).layoutSources();
  ASSERT_EQ(records.size(), 1);
  EXPECT_EQ(records[0].id, u"robot"_s);
  EXPECT_EQ(records[0].plugin_id, QString::fromLatin1(kPluginId));
  EXPECT_EQ(records[0].plugin_manifest_id, QString::fromLatin1(kManifestId));
  EXPECT_EQ(records[0].plugin_config_json, config_template);

  // Save Layout with Sources: the template, not what this page made of it, and
  // the curve still names its stream.
  QString error;
  const std::optional<QDomDocument> saved = PresetTestPeer::sourceLayout(*window_, error);
  ASSERT_TRUE(saved.has_value()) << error.toStdString();
  const QByteArray saved_bytes = saved->toByteArray(2);
  EXPECT_FALSE(saved_bytes.contains("wss://")) << saved_bytes.toStdString();
  EXPECT_EQ(saved->documentElement().attribute(u"pj4_version"_s), u"5"_s);
  const QList<PJ::layout_xml::StreamingSourceRef> saved_records =
      PJ::layout_xml::extractStreamingSources(*saved, &error);
  ASSERT_EQ(saved_records.size(), 1) << error.toStdString();
  EXPECT_EQ(saved_records[0].id, u"robot"_s);
  EXPECT_EQ(saved_records[0].plugin_config_json, config_template);
  const QDomElement curve = saved->elementsByTagName(u"curve"_s).at(0).toElement();
  EXPECT_EQ(curve.attribute(u"dataset_path"_s), u"pj-stream://robot"_s);

  // History snapshots carry no stream records, and moving through them starts nothing.
  const QByteArray live = PresetTestPeer::liveState(*window_);
  EXPECT_FALSE(live.contains("previouslyLoaded_StreamingSources"));
  EXPECT_FALSE(live.contains("streamInfo"));
  PresetTestPeer::plot(*window_, u"plot1"_s)->removeAllCurves();
  PresetTestPeer::recordDiscreteState(*window_);
  PresetTestPeer::undo(*window_);
  EXPECT_EQ(PresetTestPeer::curveKeys(*window_).size(), 1);
  PresetTestPeer::redo(*window_);
  EXPECT_TRUE(PresetTestPeer::curveKeys(*window_).isEmpty());
  pj_app_test::flushQueuedEvents();
  EXPECT_EQ(probeLines().count(u"start"_s), 1);
  EXPECT_EQ(streamSessions(), sessions);
}

// Two streams of one plugin are two sources: each keeps its own dataset, curves
// and timeline track, beside the preset's file.
TEST_F(StreamingPresetTest, SamePluginSourcesRemainDistinct) {
  const QByteArray layout = streamLayout(
      {{u"plot_file"_s, fileCurve(u"data.mock"_s)},
       {u"plot_left"_s, streamCurve(u"shared/data"_s, u"left"_s)},
       {u"plot_right"_s, streamCurve(u"shared/data"_s, u"right"_s)}},
      {{u"left"_s, uR"({"topic":"shared/data","value":1})"_s,
        uR"(<dataset display_offset_ns="1000" timeline_order="1"/>)"_s},
       {u"right"_s, uR"({"topic":"shared/data","value":2})"_s,
        uR"(<dataset display_offset_ns="2000" timeline_order="0"/>)"_s}},
      {u"data.mock"_s});
  ASSERT_EQ(loadPreset(u"pair"_s, layout, {u"data.mock"_s}), u"ok"_s);
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 3; }));

  const QHash<QString, PJ::DatasetId> sessions = streamSessions();
  ASSERT_EQ(sessions.size(), 2);
  const PJ::DatasetId left = sessions.value(u"left"_s);
  const PJ::DatasetId right = sessions.value(u"right"_s);
  ASSERT_NE(left, 0U);
  ASSERT_NE(right, 0U);
  EXPECT_NE(left, right);
  EXPECT_EQ(plotDatasets(u"plot_left"_s), std::vector<PJ::DatasetId>{left});
  EXPECT_EQ(plotDatasets(u"plot_right"_s), std::vector<PJ::DatasetId>{right});
  const QStringList files = fileSources();
  ASSERT_EQ(files.size(), 1);
  EXPECT_TRUE(PJ::isBrowserUploadIdentity(files.front())) << files.front().toStdString();

  EXPECT_EQ(sessionManager().sourceDisplayOffset(left).value.count(), 1000);
  EXPECT_EQ(sessionManager().sourceDisplayOffset(right).value.count(), 2000);
  // The saved slots first (right, then left), then the file, which saved none.
  const std::vector<PJ::DatasetId> order = PresetTestPeer::timelineOrder(*window_);
  ASSERT_EQ(order.size(), 3U);
  EXPECT_EQ(order[0], right);
  EXPECT_EQ(order[1], left);

  // Saved again, each stream keeps its record, its track and its curves.
  QString error;
  const std::optional<QDomDocument> saved = PresetTestPeer::sourceLayout(*window_, error);
  ASSERT_TRUE(saved.has_value()) << error.toStdString();
  const QList<PJ::layout_xml::StreamingSourceRef> records = PJ::layout_xml::extractStreamingSources(*saved, &error);
  ASSERT_EQ(records.size(), 2) << error.toStdString();
  QHash<QString, PJ::layout_xml::StreamingSourceRef> by_id;
  for (const PJ::layout_xml::StreamingSourceRef& record : records) {
    by_id.insert(record.id, record);
  }
  EXPECT_EQ(by_id.value(u"left"_s).plugin_config_json, uR"({"topic":"shared/data","value":1})"_s);
  EXPECT_EQ(by_id.value(u"left"_s).dataset.display_offset_ns, 1000);
  EXPECT_EQ(by_id.value(u"left"_s).dataset.timeline_order, 1);
  EXPECT_EQ(by_id.value(u"right"_s).dataset.display_offset_ns, 2000);
  EXPECT_EQ(by_id.value(u"right"_s).dataset.timeline_order, 0);
  EXPECT_EQ(PJ::layout_xml::extractDataSource(*saved, QDir()).size(), 1);
  QSet<QString> curve_sources;
  const QDomNodeList curves = saved->elementsByTagName(u"curve"_s);
  for (int index = 0; index < curves.size(); ++index) {
    curve_sources.insert(curves.at(index).toElement().attribute(u"dataset_path"_s));
  }
  EXPECT_EQ(curve_sources, (QSet<QString>{u"data.mock"_s, u"pj-stream://left"_s, u"pj-stream://right"_s}));

  // That save opens as a preset of its own, onto fresh sessions laid out the same.
  ASSERT_EQ(loadPreset(u"again"_s, saved->toByteArray(2), {u"data.mock"_s}), u"ok"_s);
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 3; }));
  const QHash<QString, PJ::DatasetId> reopened = streamSessions();
  ASSERT_EQ(reopened.size(), 2);
  EXPECT_FALSE(reopened.values().contains(left));
  EXPECT_FALSE(reopened.values().contains(right));
  EXPECT_EQ(plotDatasets(u"plot_left"_s), std::vector<PJ::DatasetId>{reopened.value(u"left"_s)});
  EXPECT_EQ(plotDatasets(u"plot_right"_s), std::vector<PJ::DatasetId>{reopened.value(u"right"_s)});
  EXPECT_EQ(sessionManager().sourceDisplayOffset(reopened.value(u"left"_s)).value.count(), 1000);
  const std::vector<PJ::DatasetId> reopened_order = PresetTestPeer::timelineOrder(*window_);
  ASSERT_EQ(reopened_order.size(), 3U);
  EXPECT_EQ(reopened_order[0], reopened.value(u"right"_s));
  EXPECT_EQ(reopened_order[1], reopened.value(u"left"_s));
}

// A preset whose streams cannot all start fails as a whole, with its reason:
// its files and the streams it did start are gone, and the preset before it is
// left running. The next preset that loads replaces that one, and only that one.
TEST_F(StreamingPresetTest, FailedSetupKeepsPreviousPreset) {
  ASSERT_EQ(
      loadPreset(
          u"first"_s,
          streamLayout(
              {{u"plot1"_s, fileCurve(u"first.mock"_s) + streamCurve(u"first/data"_s, u"first"_s)}},
              {{u"first"_s, uR"({"topic":"first/data"})"_s}}, {u"first.mock"_s}),
          {u"first.mock"_s}),
      u"ok"_s);
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 2; }));
  // A stream no preset started survives every preset load.
  QString error;
  PJ::layout_xml::StreamingSourceRef unowned;
  unowned.id = u"mine"_s;
  unowned.plugin_id = QString::fromLatin1(kPluginId);
  unowned.plugin_config_json = uR"({"topic":"mine/data"})"_s;
  const PJ::DatasetId mine =
      PresetTestPeer::streams(*window_).startFromLayout(unowned, unowned.plugin_config_json, error);
  ASSERT_NE(mine, 0U) << error.toStdString();
  // The user paused on the stream they follow, at its first sample: a failed preset
  // must leave that as it is. The first sample, because the range only grows past it
  // (live ingest sets the stream's range, file ingest a union holding it), so no later
  // range update can clamp the cursor and move the baseline.
  PJ::PlaybackEngine& playback = PresetTestPeer::session(*window_).playbackEngine();
  ASSERT_EQ(PresetTestPeer::followedStream(*window_), mine);
  ASSERT_TRUE(pumpUntil([&] { return sessionManager().datasetDisplayRange(mine).has_value(); }));
  playback.pause();
  const PJ::DisplayRange mine_range = *sessionManager().datasetDisplayRange(mine);
  playback.setRangeAndCurrentTime(mine_range, mine_range.min);
  const double paused_at = playback.currentTime().value;
  ASSERT_EQ(paused_at, mine_range.min.value);

  const QStringList files_before = fileSources();
  const QStringList keys_before = PresetTestPeer::curveKeys(*window_);
  const QHash<QString, PJ::DatasetId> sessions_before = streamSessions();
  ASSERT_EQ(sessions_before.size(), 2);
  ASSERT_EQ(sessions_before.value(u"mine"_s), mine);
  const QString good = uR"({"topic":"next/data"})"_s;
  struct Case {
    QString name;
    QList<Stream> streams;
    QString reason;
    QString extra_curves = {};
  };
  for (const Case& failing : QList<Case>{
           // The plugin refuses the saved configuration: nothing is substituted.
           {u"rejected"_s, {{u"s"_s, uR"({"reject":true})"_s}}, u"stream"_s},
           // The second stream fails to start after the first one started.
           {u"start"_s, {{u"s1"_s, good}, {u"s2"_s, uR"({"fail_start":true})"_s}}, u"stream"_s},
           // This build has no such plugin.
           {u"missing"_s, {{u"s"_s, good, {}, u"Missing Stream"_s, u"missing-stream"_s}}, u"stream"_s},
           // The page (the desktop has none) cannot fill in the deployment origin.
           {u"context"_s, {{u"s"_s, uR"({"url":"${deployment.ws_origin}/bridge"})"_s}}, u"stream-config"_s},
           // Two records share an id.
           {u"records"_s, {{u"s"_s, good}, {u"s"_s, good}}, u"stream-records"_s},
           // Everything starts, then the layout does not apply.
           {u"apply"_s, {{u"s"_s, good}}, u"apply-after-source-load"_s, fileCurve(u"next.mock"_s, u"absent"_s)},
       }) {
    const qsizetype requests_before = server_.requests.size();
    const QByteArray layout = streamLayout(
        {{u"plot1"_s, fileCurve(u"next.mock"_s) + streamCurve(u"next/data"_s, u"s"_s) + failing.extra_curves}},
        failing.streams, {u"next.mock"_s});
    EXPECT_EQ(loadPreset(failing.name, layout, {u"next.mock"_s}), failing.reason) << failing.name.toStdString();
    closeDialogs();
    pj_app_test::flushQueuedEvents();
    EXPECT_EQ(fileSources(), files_before) << failing.name.toStdString();
    EXPECT_EQ(PresetTestPeer::curveKeys(*window_), keys_before) << failing.name.toStdString();
    EXPECT_EQ(streamSessions(), sessions_before) << failing.name.toStdString();
    EXPECT_EQ(PresetTestPeer::followedStream(*window_), mine) << failing.name.toStdString();
    EXPECT_FALSE(playback.isPlaying()) << failing.name.toStdString();
    EXPECT_EQ(playback.currentTime().value, paused_at) << failing.name.toStdString();
    if (failing.reason != u"stream"_s && failing.reason != u"apply-after-source-load"_s) {
      EXPECT_EQ(server_.requests.size(), requests_before + 1)
          << failing.name.toStdString() << ": refused before any source was fetched";
    }
  }

  ASSERT_EQ(
      loadPreset(
          u"next"_s,
          streamLayout(
              {{u"plot1"_s, fileCurve(u"next.mock"_s) + streamCurve(u"next/data"_s, u"next"_s)}}, {{u"next"_s, good}},
              {u"next.mock"_s}),
          {u"next.mock"_s}),
      u"ok"_s);
  const QHash<QString, PJ::DatasetId> sessions = streamSessions();
  EXPECT_EQ(QSet<QString>(sessions.keyBegin(), sessions.keyEnd()), (QSet<QString>{u"mine"_s, u"next"_s}));
  EXPECT_EQ(sessions.value(u"mine"_s), mine);
  const QStringList files = fileSources();
  ASSERT_EQ(files.size(), 1);
  EXPECT_FALSE(files_before.contains(files.front()));
}

// A stream-only preset (the browser's live samples): presets whose stream cannot start leave
// the running preset's streams alone; the next one that loads stops and removes both.
TEST_F(StreamingPresetTest, StreamOnlyPresetReplacesThePreviousStreams) {
  ASSERT_EQ(
      loadPreset(
          u"live"_s,
          streamLayout(
              {{u"plot_a"_s, streamCurve(u"a/data"_s, u"a"_s)}, {u"plot_b"_s, streamCurve(u"b/data"_s, u"b"_s)}},
              {{u"a"_s, uR"({"topic":"a/data"})"_s}, {u"b"_s, uR"({"topic":"b/data"})"_s}})),
      u"ok"_s);
  const QHash<QString, PJ::DatasetId> live = streamSessions();
  ASSERT_EQ(live.size(), 2);

  const QString next = uR"({"topic":"next/data"})"_s;
  const QList<Plot> next_plot{{u"plot_c"_s, streamCurve(u"next/data"_s, u"c"_s)}};
  EXPECT_EQ(
      loadPreset(u"missing"_s, streamLayout(next_plot, {{u"c"_s, next, {}, u"Missing Stream"_s, u"missing-stream"_s}})),
      u"stream"_s);
  closeDialogs();
  EXPECT_EQ(
      loadPreset(u"typo"_s, streamLayout(next_plot, {{u"c"_s, uR"({"url":"${deployment.wsorigin}"})"_s}})),
      u"stream-config"_s);
  closeDialogs();
  pj_app_test::flushQueuedEvents();
  EXPECT_EQ(streamSessions(), live);

  QSignalSpy stopped(&PresetTestPeer::streams(*window_), &PJ::StreamingSourceManager::streamStopped);
  ASSERT_EQ(loadPreset(u"next"_s, streamLayout(next_plot, {{u"c"_s, next}})), u"ok"_s);
  QSet<PJ::DatasetId> stopped_ids;
  for (const QList<QVariant>& arguments : stopped) {
    stopped_ids.insert(arguments.at(0).value<PJ::DatasetId>());
  }
  EXPECT_EQ(stopped_ids, (QSet<PJ::DatasetId>{live.value(u"a"_s), live.value(u"b"_s)}));
  const QHash<QString, PJ::DatasetId> sessions = streamSessions();
  EXPECT_EQ(QSet<QString>(sessions.keyBegin(), sessions.keyEnd()), QSet<QString>{u"c"_s});
  for (const auto& [dataset_id, unused_name] : PresetTestPeer::session(*window_).catalogModel().datasets()) {
    (void)unused_name;
    EXPECT_FALSE(dataset_id == live.value(u"a"_s) || dataset_id == live.value(u"b"_s)) << "a replaced stream's dataset";
  }
}

// Topics that arrive after the layout applied bind to the stream their curve
// names, never to another stream that publishes the same topic first.
TEST_F(StreamingPresetTest, LateTopicsBindToCorrectSource) {
  // Each stream publishes only once the test opens its gate, so the order is the test's, not the clock's.
  const QString left_gate = probe_dir_.filePath(u"left.gate"_s);
  const QString right_gate = probe_dir_.filePath(u"right.gate"_s);
  const auto open_gate = [](const QString& path) {
    QFile gate(path);
    ASSERT_TRUE(gate.open(QIODevice::WriteOnly));
  };
  const QByteArray layout = streamLayout(
      {{u"plot_left"_s, streamCurve(u"late/data"_s, u"left"_s)},
       {u"plot_right"_s, streamCurve(u"late/data"_s, u"right"_s)}},
      {{u"left"_s, uR"({"topic":"late/data","gate_file":"%1"})"_s.arg(left_gate)},
       {u"right"_s, uR"({"topic":"late/data","gate_file":"%1"})"_s.arg(right_gate)}});
  ASSERT_EQ(loadPreset(u"late"_s, layout), u"ok"_s);
  PJ::PlotWidget* left_plot = PresetTestPeer::plot(*window_, u"plot_left"_s);
  PJ::PlotWidget* right_plot = PresetTestPeer::plot(*window_, u"plot_right"_s);
  ASSERT_NE(left_plot, nullptr);
  ASSERT_NE(right_plot, nullptr);
  // Nothing has arrived yet: both curves wait for their topics.
  EXPECT_TRUE(PresetTestPeer::curveKeys(*window_).isEmpty());
  EXPECT_EQ(left_plot->pendingCurveIntentCount(), 1U);
  EXPECT_EQ(right_plot->pendingCurveIntentCount(), 1U);

  const QHash<QString, PJ::DatasetId> sessions = streamSessions();
  ASSERT_EQ(sessions.size(), 2);
  open_gate(left_gate);
  ASSERT_TRUE(pumpUntil([&] { return !left_plot->curveList().empty(); }));
  EXPECT_EQ(plotDatasets(u"plot_left"_s), std::vector<PJ::DatasetId>{sessions.value(u"left"_s)});
  // More of the left stream's ticks: each one is a chance to bind the right curve to it.
  static_cast<void>(pumpUntil([] { return false; }, 300));
  EXPECT_TRUE(right_plot->curveList().empty()) << "the right curve must wait for its own stream";
  open_gate(right_gate);
  ASSERT_TRUE(pumpUntil([&] { return !right_plot->curveList().empty(); }));
  EXPECT_EQ(plotDatasets(u"plot_right"_s), std::vector<PJ::DatasetId>{sessions.value(u"right"_s)});
  EXPECT_EQ(plotDatasets(u"plot_left"_s), std::vector<PJ::DatasetId>{sessions.value(u"left"_s)});
}

}  // namespace
