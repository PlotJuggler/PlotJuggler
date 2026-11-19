// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A layout that records streaming sources starts them from their saved
// configuration, with no dialog, and its views bind each stream's topics as they
// arrive. StreamingPresetTest covers a preset (a layout opened by URL: the
// browser's byte-level replay, all-or-nothing with its file sources);
// DesktopStreamLayoutTest a layout file the desktop opens and saves. The test
// stream (mock_layout_stream_plugin.cpp) records the configuration it was given.

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QDialog>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLabel>
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
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "FileLoader.h"
#include "LayoutXml.h"
#include "MainWindow.h"
#include "SourceTimelineController.h"
#include "StreamingSourceManager.h"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_widgets/IntScrubber.h"
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

uint64_t datasetRowCount(PJ::DataEngine& engine, PJ::DatasetId dataset) {
  PJ::DataReader reader(engine);
  uint64_t rows = 0;
  for (const PJ::TopicId topic : engine.listTopics(dataset)) {
    const auto meta = reader.getMetadata(topic);
    rows += meta.has_value() ? meta->total_row_count : 0;
  }
  return rows;
}

class StreamingPresetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(probe_dir_.isValid());
    ASSERT_TRUE(settings_dir_.isValid());
    // Settings in a directory of this case's own.
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
  // The user paused on the stream they follow, at its first sample: a failed preset must leave that as it is. The
  // first sample, because the range only grows past it, so no later range update can clamp the cursor and move
  // the baseline.
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

// A stream that ends while streaming is paused leaves its paused rows waiting for the resume, and no
// session. Removing its dataset takes those rows too: the resume merges nothing at all, for any
// stream, while the paused buffer holds rows of a dataset the session no longer has.
TEST_F(StreamingPresetTest, RemovingADatasetDropsThePausedRowsOfItsEndedStream) {
  const QString end_file = probe_dir_.filePath(u"end"_s);
  ASSERT_EQ(
      loadPreset(
          u"live"_s, streamLayout(
                         {{u"plot1"_s, streamCurve(u"robot/data"_s, u"robot"_s)}},
                         {{u"robot"_s, uR"({"topic":"robot/data","end_file":"%1"})"_s.arg(end_file)}})),
      u"ok"_s);
  PJ::StreamingSourceManager& streams = PresetTestPeer::streams(*window_);
  const PJ::DatasetId dataset = streamSessions().value(u"robot"_s);
  ASSERT_NE(dataset, 0U);

  streams.onPauseToggled(true);
  PJ::DataEngine& paused = streams.secondaryEngineForTests();
  const auto paused_rows = [&] { return datasetRowCount(paused, dataset); };
  ASSERT_TRUE(pumpUntil([&] { return paused_rows() > 0; }));
  QSignalSpy stopped(&streams, &PJ::StreamingSourceManager::streamStopped);
  {
    QFile end(end_file);
    ASSERT_TRUE(end.open(QIODevice::WriteOnly));
  }
  ASSERT_TRUE(pumpUntil([&] { return !stopped.isEmpty(); }));
  ASSERT_GT(paused_rows(), 0U) << "the ended stream's paused rows should wait for the resume";

  window_->removeDatasets({dataset});
  const auto merged = paused.flushTo(sessionManager().dataEngine());
  EXPECT_TRUE(merged.has_value()) << merged.error();
}

namespace {

// Starts the test stream `id` from a layout record, on `topic` and with `extra` added to its configuration.
PJ::DatasetId startStream(PJ::StreamingSourceManager& streams, const QString& id, const QString& extra = {}) {
  PJ::layout_xml::StreamingSourceRef source;
  source.id = id;
  source.plugin_id = QString::fromLatin1(kPluginId);
  source.plugin_config_json = uR"({"topic":"%1/data"%2})"_s.arg(id, extra);
  QString error;
  const PJ::DatasetId dataset = streams.startFromLayout(source, source.plugin_config_json, error);
  EXPECT_NE(dataset, 0U) << error.toStdString();
  return dataset;
}

}  // namespace

// The newest stream is the one followed live. When it stops while an older one still runs,
// the older one is followed: playback keeps playing and the cursor jumps to its tip.
TEST_F(StreamingPresetTest, StoppingTheFollowedStreamFollowsAnotherOneStillRunning) {
  PJ::StreamingSourceManager& streams = PresetTestPeer::streams(*window_);
  const auto tip = [&](PJ::DatasetId dataset) {
    const auto range = sessionManager().datasetDisplayRange(dataset);
    return range.has_value() ? range->max.value : 0.0;
  };
  const PJ::DatasetId older = startStream(streams, u"older"_s);
  ASSERT_TRUE(pumpUntil([&] { return sessionManager().datasetDisplayRange(older).has_value(); }));
  // The newer stream publishes nothing, so while it is followed the transport stays where the older one left it.
  const PJ::DatasetId newer =
      startStream(streams, u"newer"_s, uR"(,"gate_file":"%1")"_s.arg(probe_dir_.filePath(u"never"_s)));
  ASSERT_NE(older, 0U);
  ASSERT_NE(newer, 0U);
  ASSERT_EQ(PresetTestPeer::followedStream(*window_), newer);
  PJ::PlaybackEngine& playback = PresetTestPeer::session(*window_).playbackEngine();
  ASSERT_TRUE(playback.isPlaying());
  const double tip_when_newer_started = tip(older);
  ASSERT_TRUE(pumpUntil([&] { return tip(older) > tip_when_newer_started; }));
  const double tip_before_stop = tip(older);

  ASSERT_TRUE(streams.stopDatasetAndWait(newer, u"test"_s));
  EXPECT_EQ(PresetTestPeer::followedStream(*window_), older);
  EXPECT_EQ(PresetTestPeer::session(*window_).activeStreamingDataset(), older);
  EXPECT_GE(playback.currentTime().value, tip_before_stop) << "the cursor should land on the older stream's tip";
  EXPECT_TRUE(pumpUntil([&] { return playback.currentTime().value > tip_before_stop; }));
  EXPECT_TRUE(playback.isPlaying());
}

// Live-follow goes back to the stream that launched last, not to the highest dataset id: an
// interactive start owns its dataset from the moment its dialog opens.
TEST_F(StreamingPresetTest, StoppingTheFollowedStreamFollowsTheLastOneLaunched) {
  PJ::StreamingSourceManager& streams = PresetTestPeer::streams(*window_);
  qputenv("PJ_LAYOUT_STREAM_DIALOG_CONFIG", R"({"topic":"x/data"})");
  streams.onStartRequested(QString::fromLatin1(kPluginId));
  ASSERT_TRUE(streams.hasPendingStart());
  const PJ::DatasetId y = startStream(streams, u"y"_s);
  ASSERT_NE(y, 0U);
  QDialog* dialog = nullptr;
  ASSERT_TRUE(pumpUntil([&] {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
      if (auto* found = qobject_cast<QDialog*>(widget); found != nullptr && found->isModal() && found->isVisible()) {
        dialog = found;
      }
    }
    return dialog != nullptr;
  }));
  dialog->accept();
  ASSERT_TRUE(pumpUntil([&] { return !streams.hasPendingStart(); }));
  qunsetenv("PJ_LAYOUT_STREAM_DIALOG_CONFIG");
  const PJ::DatasetId x = PresetTestPeer::followedStream(*window_);
  ASSERT_NE(x, y);
  ASSERT_LT(x, y) << "the dialog start owns the older dataset";
  const PJ::DatasetId z = startStream(streams, u"z"_s);
  ASSERT_EQ(PresetTestPeer::followedStream(*window_), z);
  ASSERT_TRUE(pumpUntil([&] {
    return sessionManager().datasetDisplayRange(x).has_value() && sessionManager().datasetDisplayRange(y).has_value();
  }));

  ASSERT_TRUE(streams.stopDatasetAndWait(z, u"test"_s));
  EXPECT_EQ(PresetTestPeer::followedStream(*window_), x);
}

// A running stream that has published nothing has no live edge to follow: live-follow ends,
// so playback runs to the end of what it shows and pauses, instead of holding a frozen cursor.
TEST_F(StreamingPresetTest, StoppingTheFollowedStreamSkipsAStreamWithNoData) {
  PJ::StreamingSourceManager& streams = PresetTestPeer::streams(*window_);
  const PJ::DatasetId silent =
      startStream(streams, u"silent"_s, uR"(,"gate_file":"%1")"_s.arg(probe_dir_.filePath(u"never"_s)));
  const PJ::DatasetId live = startStream(streams, u"live"_s);
  ASSERT_NE(silent, 0U);
  ASSERT_NE(live, 0U);
  ASSERT_TRUE(pumpUntil([&] { return sessionManager().datasetDisplayRange(live).has_value(); }));

  ASSERT_TRUE(streams.stopDatasetAndWait(live, u"test"_s));
  EXPECT_EQ(PresetTestPeer::followedStream(*window_), 0U);
  EXPECT_EQ(PresetTestPeer::session(*window_).activeStreamingDataset(), 0U);
  EXPECT_TRUE(pumpUntil([&] { return !PresetTestPeer::session(*window_).playbackEngine().isPlaying(); }));
}

// A sample stamped before the newest live one (a late arrival) can reach a paused stream.
// The resume still merges every paused row into the live engine, and so does the next one.
TEST_F(StreamingPresetTest, ResumeMergesPausedRowsThatArriveLate) {
  const QString late_file = probe_dir_.filePath(u"late"_s);
  ASSERT_EQ(
      loadPreset(
          u"late"_s, streamLayout(
                         {{u"plot1"_s, streamCurve(u"robot/data"_s, u"robot"_s)}},
                         {{u"robot"_s, uR"({"topic":"robot/data","late_file":"%1"})"_s.arg(late_file)}})),
      u"ok"_s);
  PJ::StreamingSourceManager& streams = PresetTestPeer::streams(*window_);
  const PJ::DatasetId dataset = streamSessions().value(u"robot"_s);
  ASSERT_NE(dataset, 0U);
  PJ::DataEngine& live = sessionManager().dataEngine();
  PJ::DataEngine& paused = streams.secondaryEngineForTests();
  const auto rows = [dataset](PJ::DataEngine& engine) { return datasetRowCount(engine, dataset); };
  const auto has_late_row = [dataset](PJ::DataEngine& engine) {
    constexpr PJ::Timestamp kLateStamp = 1'000'000'000;  // the mock stream's kStampBase
    PJ::DataReader reader(engine);
    for (const PJ::TopicId topic : engine.listTopics(dataset)) {
      const auto row = reader.latestAt(PJ::QueryPoint{.topic_id = topic, .t = kLateStamp});
      if (row.has_value() && row->has_value()) {
        return true;
      }
    }
    return false;
  };
  ASSERT_TRUE(pumpUntil([&] { return rows(live) > 0; }));

  streams.onPauseToggled(true);
  {
    QFile late(late_file);
    ASSERT_TRUE(late.open(QIODevice::WriteOnly));
  }
  ASSERT_TRUE(pumpUntil([&] { return has_late_row(paused) && rows(paused) > 1; }));
  streams.onPauseToggled(false);
  EXPECT_EQ(rows(paused), 0U) << "the paused rows did not reach the live engine";
  EXPECT_TRUE(has_late_row(live));

  streams.onPauseToggled(true);
  const uint64_t left_behind = rows(paused);
  ASSERT_TRUE(pumpUntil([&] { return rows(paused) > left_behind; }));
  streams.onPauseToggled(false);
  EXPECT_EQ(rows(paused), 0U) << "the next resume did not reach the live engine either";
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

// A layout file the desktop opens (File → Load Layout, or --layout) starts the streams it
// records, with no plugin dialog. Asked first when the user opens it; started as-is from
// the command line.
class DesktopStreamLayoutTest : public StreamingPresetTest {
 protected:
  // One plot drawing "robot/data" from the stream record "robot", started with `config`.
  static QByteArray robotLayout(const QString& config = uR"({"topic":"robot/data"})"_s) {
    return streamLayout({{u"plot1"_s, streamCurve(u"robot/data"_s, u"robot"_s)}}, {{u"robot"_s, config}});
  }

  // Writes `layout` to a file and opens it as --layout does (no prompt), or as File →
  // Load Layout does when `interactive`; returns whether it applied.
  bool openLayoutFile(const QByteArray& layout, bool interactive = false) {
    const QString path = probe_dir_.filePath(u"layout.pj4.xml"_s);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(layout) != layout.size()) {
      return false;
    }
    file.close();
    return openLayoutPath(path, interactive);
  }

  // `layout` with a <left_panel_state> naming a `seconds` streaming buffer.
  static QByteArray withStreamingBuffer(QByteArray layout, int seconds) {
    return layout.replace(
        "</root>", uR"(<left_panel_state sources_tab="stream" streaming_buffer="%1"/></root>)"_s.arg(seconds).toUtf8());
  }

  [[nodiscard]] PJ::IntScrubber* bufferSpinBox() const {
    return window_->findChild<PJ::IntScrubber*>(u"streamingSpinBox"_s);
  }

  // As the user does: the value counts once editing finishes.
  void setUserBuffer(int seconds) {
    PJ::IntScrubber* spin = bufferSpinBox();
    ASSERT_NE(spin, nullptr);
    spin->setValue(seconds);
    emit spin->editingFinished();
  }

  bool openLayoutPath(const QString& path, bool interactive = false) {
    std::optional<bool> settled;
    const auto connection =
        QObject::connect(window_.get(), &PJ::MainWindow::layoutRestoreSettled, [&settled](bool ok) { settled = ok; });
    PresetTestPeer::loadLayoutFile(*window_, path, interactive);
    EXPECT_TRUE(pumpUntil([&settled] { return settled.has_value(); }));
    QObject::disconnect(connection);
    pj_app_test::flushQueuedEvents();
    return settled.value_or(false);
  }

  // Answers the next modal message box that has a button labelled `label` by clicking
  // it, once `ready` (when given) holds, and records the box's text in `asked`.
  void answerNextQuestion(const QString& label, QString* asked = nullptr, std::function<bool()> ready = {}) {
    auto* timer = new QTimer(window_.get());
    QObject::connect(timer, &QTimer::timeout, timer, [timer, label, asked, ready = std::move(ready)] {
      QWidget* modal = QApplication::activeModalWidget();
      if (modal == nullptr || (ready && !ready())) {
        return;
      }
      for (QAbstractButton* button : modal->findChildren<QAbstractButton*>(u"pjMessageBoxButton"_s)) {
        if (button->text() == label) {
          if (const auto* body = modal->findChild<QLabel*>(u"pjMessageBoxBody"_s);
              asked != nullptr && body != nullptr) {
            *asked = body->text();
          }
          timer->deleteLater();
          button->click();
          return;
        }
      }
    });
    timer->start(10);
  }
};

TEST_F(DesktopStreamLayoutTest, CommandLineLayoutStartsItsStreamsWithoutAsking) {
  const QString config = uR"({"topic":"robot/data"})"_s;
  int dialogs_seen = 0;
  QTimer dialog_watch;
  QObject::connect(&dialog_watch, &QTimer::timeout, [&dialogs_seen] { dialogs_seen += visibleDialogs(); });
  dialog_watch.start(10);
  ASSERT_TRUE(openLayoutFile(robotLayout(config)));
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 1; }));
  dialog_watch.stop();
  EXPECT_EQ(dialogs_seen, 0);

  const QStringList probe = probeLines();
  ASSERT_GE(probe.size(), 2);
  EXPECT_EQ(probe.mid(0, 2), (QStringList{u"load "_s + config, u"start"_s}));
  const QHash<QString, PJ::DatasetId> sessions = streamSessions();
  ASSERT_EQ(sessions.keys(), QStringList{u"robot"_s});
  EXPECT_EQ(plotDatasets(u"plot1"_s), std::vector<PJ::DatasetId>{sessions.value(u"robot"_s)});
}

// Saving with "Bind to this data source" records the live stream and what is drawn from
// it; opening that file while the stream still runs binds to it instead of starting a
// second one, and opening it after the stream is gone starts it again.
TEST_F(DesktopStreamLayoutTest, SavedStreamLayoutRoundTrips) {
  const QString config = uR"({"topic":"robot/data","value":3})"_s;
  ASSERT_TRUE(openLayoutFile(robotLayout(config)));
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 1; }));
  const PJ::DatasetId first = streamSessions().value(u"robot"_s);
  ASSERT_NE(first, 0U);

  const QString saved_path = probe_dir_.filePath(u"saved.pj4.xml"_s);
  PresetTestPeer::saveLayoutFile(*window_, saved_path, /*include_data_source=*/true);
  QFile saved_file(saved_path);
  ASSERT_TRUE(saved_file.open(QIODevice::ReadOnly));
  QDomDocument saved;
  ASSERT_TRUE(saved.setContent(saved_file.readAll()));
  EXPECT_EQ(saved.documentElement().attribute(u"pj4_version"_s), u"5"_s);
  QString error;
  const QList<PJ::layout_xml::StreamingSourceRef> records = PJ::layout_xml::extractStreamingSources(saved, &error);
  ASSERT_EQ(records.size(), 1) << error.toStdString();
  EXPECT_EQ(records[0].id, u"robot"_s);
  EXPECT_EQ(records[0].plugin_manifest_id, QString::fromLatin1(kManifestId));
  EXPECT_EQ(records[0].plugin_config_json, config);
  ASSERT_GT(saved.elementsByTagName(u"curve"_s).size(), 0);
  EXPECT_EQ(saved.elementsByTagName(u"curve"_s).at(0).toElement().attribute(u"dataset_path"_s), u"pj-stream://robot"_s);
  EXPECT_TRUE(saved.documentElement().firstChildElement(u"left_panel_state"_s).hasAttribute(u"streaming_buffer"_s))
      << "a stream-bound layout records the buffer its streams restart with";

  // Still streaming: the layout binds to the running session.
  ASSERT_TRUE(openLayoutPath(saved_path));
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 1; }));
  EXPECT_EQ(probeLines().count(u"start"_s), 1);
  EXPECT_EQ(plotDatasets(u"plot1"_s), std::vector<PJ::DatasetId>{first});

  // Stopped: opening it starts the stream again from the saved configuration.
  PresetTestPeer::streams(*window_).stopAllAndWait(u"test"_s);
  pj_app_test::flushQueuedEvents();
  ASSERT_TRUE(openLayoutPath(saved_path));
  ASSERT_TRUE(pumpUntil([this] { return probeLines().count(u"start"_s) == 2; }));
  EXPECT_EQ(probeLines().filter(u"load "_s).last(), u"load "_s + config);
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 1; }));
  const PJ::DatasetId second = streamSessions().value(u"robot"_s);
  EXPECT_NE(second, 0U);
  EXPECT_EQ(plotDatasets(u"plot1"_s), std::vector<PJ::DatasetId>{second});
}

// A generic save ("Bind to this data source" off) records no stream.
TEST_F(DesktopStreamLayoutTest, GenericSaveRecordsNoStream) {
  ASSERT_TRUE(openLayoutFile(robotLayout()));
  const QString saved_path = probe_dir_.filePath(u"generic.pj4.xml"_s);
  PresetTestPeer::saveLayoutFile(*window_, saved_path, /*include_data_source=*/false);
  QFile saved_file(saved_path);
  ASSERT_TRUE(saved_file.open(QIODevice::ReadOnly));
  const QByteArray bytes = saved_file.readAll();
  EXPECT_FALSE(bytes.contains("streamInfo")) << bytes.toStdString();
  EXPECT_FALSE(bytes.contains("pj-stream://")) << bytes.toStdString();
  EXPECT_FALSE(bytes.contains("streaming_buffer")) << "only a stream-bound layout saves the buffer";
}

// A stream-bound layout's buffer never shrinks a running stream: the stream keeps its window and the spin box shows
// it. A wider buffer applies, even to the stream the layout itself just started.
TEST_F(DesktopStreamLayoutTest, StreamLayoutBufferNeverShrinksARunningStream) {
  PJ::StreamingSourceManager& streams = PresetTestPeer::streams(*window_);
  ASSERT_TRUE(openLayoutFile(withStreamingBuffer(robotLayout(), 20)));
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 1; }));
  const PJ::DatasetId robot = streamSessions().value(u"robot"_s);
  ASSERT_NE(robot, 0U);
  EXPECT_EQ(streams.retentionSeconds(), 20);

  setUserBuffer(60);
  ASSERT_EQ(streams.retentionSeconds(), 60);
  ASSERT_TRUE(openLayoutFile(withStreamingBuffer(robotLayout(), 5)));
  EXPECT_EQ(streamSessions().value(u"robot"_s), robot) << "the layout binds to the running stream";
  streams.ingestTickForTests(robot);
  EXPECT_EQ(streams.retentionSeconds(), 60);
  EXPECT_EQ(bufferSpinBox()->value(), 60);
}

// With nothing streaming, a stream-bound layout's buffer applies, smaller or not. The file is loaded first, so the
// layout applies when its stream is declined.
TEST_F(DesktopStreamLayoutTest, StreamLayoutBufferAppliesWhenNothingRuns) {
  PJ::StreamingSourceManager& streams = PresetTestPeer::streams(*window_);
  QFile data(probe_dir_.filePath(u"data.mock"_s));
  ASSERT_TRUE(data.open(QIODevice::WriteOnly) && data.write("data.mock") > 0);
  data.close();
  const QList<Plot> plots = {{u"plot_file"_s, fileCurve(u"data.mock"_s)}};
  ASSERT_TRUE(openLayoutFile(streamLayout(plots, {}, {u"data.mock"_s})));
  setUserBuffer(60);

  answerNextQuestion(u"Load Layout only"_s);
  ASSERT_TRUE(openLayoutFile(
      withStreamingBuffer(streamLayout(plots, {{u"robot"_s, uR"({"topic":"robot/data"})"_s}}, {u"data.mock"_s}), 20),
      /*interactive=*/true));
  EXPECT_TRUE(streamSessions().isEmpty());
  EXPECT_EQ(streams.retentionSeconds(), 20);
  EXPECT_EQ(bufferSpinBox()->value(), 20);
}

// The stream a layout starts itself is not a running stream to protect: with nothing streaming before the load, a
// buffer smaller than the user's still applies.
TEST_F(DesktopStreamLayoutTest, StreamLayoutBufferBelowTheUserDefaultApplies) {
  PJ::StreamingSourceManager& streams = PresetTestPeer::streams(*window_);
  setUserBuffer(60);
  ASSERT_TRUE(openLayoutFile(withStreamingBuffer(robotLayout(), 20)));
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 1; }));
  ASSERT_EQ(streamSessions().keys(), QStringList{u"robot"_s});
  EXPECT_EQ(streams.retentionSeconds(), 20);
  EXPECT_EQ(bufferSpinBox()->value(), 20);
}

// A layout without stream records leaves the buffer alone, even when it carries the attribute.
TEST_F(DesktopStreamLayoutTest, FileOnlyLayoutLeavesTheBufferAlone) {
  PJ::StreamingSourceManager& streams = PresetTestPeer::streams(*window_);
  setUserBuffer(30);
  QFile data(probe_dir_.filePath(u"data.mock"_s));
  ASSERT_TRUE(data.open(QIODevice::WriteOnly) && data.write("data.mock") > 0);
  data.close();
  ASSERT_TRUE(openLayoutFile(
      withStreamingBuffer(streamLayout({{u"plot_file"_s, fileCurve(u"data.mock"_s)}}, {}, {u"data.mock"_s}), 20)));
  EXPECT_EQ(streams.retentionSeconds(), 30);
  EXPECT_EQ(bufferSpinBox()->value(), 30);
}

// A progressive restore that aborts (here on a data processor whose input never arrives) applies none of the
// layout's left panel state: the running stream keeps its window.
TEST_F(DesktopStreamLayoutTest, AnAbortedRestoreLeavesTheBufferAlone) {
  PJ::StreamingSourceManager& streams = PresetTestPeer::streams(*window_);
  const int before = streams.retentionSeconds();
  QFile data(probe_dir_.filePath(u"data.mock"_s));
  ASSERT_TRUE(data.open(QIODevice::WriteOnly) && data.write("data.mock") > 0);
  data.close();
  QByteArray layout = withStreamingBuffer(
      streamLayout(
          {{u"plot_file"_s, fileCurve(u"data.mock"_s)}}, {{u"robot"_s, uR"({"topic":"robot/data"})"_s}},
          {u"data.mock"_s}),
      before + 40);
  layout.replace(
      "</root>",
      R"(<data_processors><processor input_topic="/missing_processor_input" input_field="value" )"
      R"(input_dataset_source="missing-processor-source" processor_id="absolute" output_name="missing[Absolute]">)"
      R"(<![CDATA[{}]]></processor></data_processors></root>)");
  QSignalSpy buffer_spy(&streams, &PJ::StreamingSourceManager::retentionWindowChanged);
  EXPECT_FALSE(openLayoutFile(layout));
  EXPECT_EQ(streams.retentionSeconds(), before);
  EXPECT_EQ(bufferSpinBox()->value(), before);
  for (const QList<QVariant>& emitted : buffer_spy) {
    EXPECT_EQ(emitted.at(1).toLongLong(), static_cast<qint64>(before) * 1'000'000'000LL);
  }
}

// A stream started without a configuration cannot be restarted from a file: the save
// leaves it out and still writes the rest.
TEST_F(DesktopStreamLayoutTest, SaveSkipsAStreamWithoutConfiguration) {
  PJ::layout_xml::StreamingSourceRef bare;
  bare.id = u"bare"_s;
  bare.plugin_id = QString::fromLatin1(kPluginId);
  bare.plugin_manifest_id = QString::fromLatin1(kManifestId);
  QString error;
  ASSERT_NE(PresetTestPeer::streams(*window_).startFromLayout(bare, QString{}, error), 0U) << error.toStdString();
  answerNextQuestion(u"OK"_s);  // a refused save would report in a box; never block on it

  const QString saved_path = probe_dir_.filePath(u"bare.pj4.xml"_s);
  PresetTestPeer::saveLayoutFile(*window_, saved_path, /*include_data_source=*/true);
  QFile saved_file(saved_path);
  ASSERT_TRUE(saved_file.open(QIODevice::ReadOnly));
  const QByteArray bytes = saved_file.readAll();
  EXPECT_FALSE(bytes.contains("streamInfo")) << bytes.toStdString();
}

// Two records with one configuration run as two sessions; reopening the layout while both
// run binds each record to its own session.
TEST_F(DesktopStreamLayoutTest, IdenticalStreamsKeepTheirOwnSessionsOnReopen) {
  const QString config = uR"({"topic":"robot/data"})"_s;
  const QByteArray layout = streamLayout(
      {{u"plot_a"_s, streamCurve(u"robot/data"_s, u"a"_s)}, {u"plot_b"_s, streamCurve(u"robot/data"_s, u"b"_s)}},
      {{u"a"_s, config}, {u"b"_s, config}});
  ASSERT_TRUE(openLayoutFile(layout));
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 2; }));
  const QHash<QString, PJ::DatasetId> sessions = streamSessions();
  ASSERT_EQ(sessions.size(), 2);

  ASSERT_TRUE(openLayoutFile(layout));
  EXPECT_EQ(probeLines().count(u"start"_s), 2);
  EXPECT_EQ(plotDatasets(u"plot_a"_s), std::vector<PJ::DatasetId>{sessions.value(u"a"_s)});
  EXPECT_EQ(plotDatasets(u"plot_b"_s), std::vector<PJ::DatasetId>{sessions.value(u"b"_s)});
}

// Opened from the app, the layout names its stream and asks before starting it.
TEST_F(DesktopStreamLayoutTest, AppLoadAsksBeforeStarting) {
  const QByteArray layout = robotLayout();
  QString asked;
  answerNextQuestion(u"Start streaming"_s, &asked);
  ASSERT_TRUE(openLayoutFile(layout, /*interactive=*/true));
  EXPECT_TRUE(asked.contains(QString::fromLatin1(kPluginId))) << asked.toStdString();
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 1; }));
  EXPECT_EQ(probeLines().count(u"start"_s), 1);
}

TEST_F(DesktopStreamLayoutTest, CancellingThePromptStartsNothing) {
  const QByteArray layout = robotLayout();
  answerNextQuestion(u"Cancel"_s);
  EXPECT_FALSE(openLayoutFile(layout, /*interactive=*/true));
  EXPECT_TRUE(probeLines().isEmpty());
  EXPECT_TRUE(streamSessions().isEmpty());
}

// A layout with a file and a stream restores progressively: a stream curve whose topic
// first arrives after the file finished loading still binds to its stream.
TEST_F(DesktopStreamLayoutTest, StreamCurveBindsWhenItsTopicArrivesAfterTheFiles) {
  QFile data(probe_dir_.filePath(u"data.mock"_s));
  ASSERT_TRUE(data.open(QIODevice::WriteOnly) && data.write("data.mock") > 0);
  data.close();
  const QString gate = probe_dir_.filePath(u"gate"_s);
  ASSERT_TRUE(openLayoutFile(streamLayout(
      {{u"plot_file"_s, fileCurve(u"data.mock"_s)}, {u"plot1"_s, streamCurve(u"robot/data"_s, u"robot"_s)}},
      {{u"robot"_s, uR"({"topic":"robot/data","gate_file":"%1"})"_s.arg(gate)}}, {u"data.mock"_s})));
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 1; }));

  QFile gate_file(gate);
  ASSERT_TRUE(gate_file.open(QIODevice::WriteOnly));
  gate_file.close();
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 2; }));
  EXPECT_EQ(plotDatasets(u"plot1"_s), std::vector<PJ::DatasetId>{streamSessions().value(u"robot"_s)});
}

// The report of a stream that could not start is a modal box in the app; the file the
// layout reloads may finish while it is open, and the restore must still settle.
TEST_F(DesktopStreamLayoutTest, RestoreSettlesWhenTheFilesFinishUnderTheStreamReport) {
  QFile data(probe_dir_.filePath(u"data.mock"_s));
  ASSERT_TRUE(data.open(QIODevice::WriteOnly) && data.write("data.mock") > 0);
  data.close();
  answerNextQuestion(u"Reload and start streaming"_s);
  answerNextQuestion(u"OK"_s, nullptr, [this] { return !PresetTestPeer::fileLoader(*window_).isBusy(); });
  EXPECT_TRUE(openLayoutFile(
      streamLayout(
          {{u"plot_file"_s, fileCurve(u"data.mock"_s)}}, {{u"bad"_s, uR"({"topic":"bad/data","fail_start":true})"_s}},
          {u"data.mock"_s}),
      /*interactive=*/true));
  EXPECT_EQ(PresetTestPeer::curveKeys(*window_).size(), 1);
}

// A stream that cannot start is named; the rest of the layout still applies.
TEST_F(DesktopStreamLayoutTest, AStreamThatFailsToStartIsReported) {
  // Unattended: reported as a diagnostic, never a dialog.
  QStringList warnings;
  QObject::connect(
      window_->diagnosticBridge(), &PJ::QtDiagnosticBridge::diagnosticReported, window_.get(),
      [&warnings](int, const QString&, const QString& id, const QString& message) {
        if (id == u"layout-streams-not-started"_s) {
          warnings.push_back(message);
        }
      });
  ASSERT_TRUE(openLayoutFile(streamLayout(
      {{u"plot1"_s, streamCurve(u"good/data"_s, u"good"_s)}},
      {{u"good"_s, uR"({"topic":"good/data"})"_s}, {u"bad"_s, uR"({"topic":"bad/data","fail_start":true})"_s}})));
  ASSERT_TRUE(pumpUntil([this] { return PresetTestPeer::curveKeys(*window_).size() == 1; }));
  EXPECT_EQ(streamSessions().keys(), QStringList{u"good"_s});
  EXPECT_EQ(visibleDialogs(), 0);
  ASSERT_EQ(warnings.size(), 1) << warnings.join(u'\n').toStdString();
  EXPECT_TRUE(warnings.front().contains(u"'bad'"_s)) << warnings.front().toStdString();
}

}  // namespace
