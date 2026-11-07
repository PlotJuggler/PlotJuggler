// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A layout opened by URL (a browser preset) replays its <fileInfo> sources with
// no picker: each is downloaded from beside the layout, staged, checked against
// its fingerprint, then imported all or nothing. Runs on the desktop because the
// replay, BrowserFileStore and FileLoader are the same code in both builds; the
// SDK's mock file source ingests the staged `.mock` files.

#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <algorithm>
#include <memory>
#include <utility>

#include "FileLoader.h"
#include "LayoutXml.h"
#include "MainWindow.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PresetCatalog.h"
#include "support/loader_test_support.h"
#include "support/preset_test_support.h"
#include "support/route_server.h"

#ifndef PJ_MOCK_FILE_SOURCE_PLUGIN_PATH
#error "PJ_MOCK_FILE_SOURCE_PLUGIN_PATH must be defined"
#endif

using namespace Qt::StringLiterals;

namespace {

using PJ::PresetTestPeer;
using Workspace = PresetTestPeer::Workspace;
using pj_app_test::closeDialogs;
using pj_app_test::pumpUntil;
using pj_app_test::RouteServer;
using pj_app_test::visibleDialogs;

QString sha256(const QByteArray& bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// One <fileInfo> replayed by the mock source; `config` reaches the plugin.
struct Source {
  QString filename;
  QString content_sha256;
  QString config = u"{}"_s;
};

// A source-bound layout: one plot with a curve per source (bound by its logical
// path, so each lands on its own dataset), plus optional extra plot curves and
// root children.
QByteArray presetLayout(
    const QList<Source>& sources, const QString& extra_curves = {}, const QString& extra_root = {}) {
  QString curves = extra_curves;
  QString files;
  for (const Source& source : sources) {
    curves += uR"(<curve topic="mock/file_data" field="value" dataset_path="%1"/>)"_s.arg(source.filename);
    files +=
        uR"(<fileInfo filename="%1" prefix=""%2><plugin ID="Mock File Source" manifest_id="mock-file-source">)"
        uR"(<![CDATA[%3]]></plugin></fileInfo>)"_s.arg(
            source.filename,
            source.content_sha256.isEmpty() ? QString() : u" content_sha256=\"%1\""_s.arg(source.content_sha256),
            source.config);
  }
  return uR"(<root pj4_version="4" binding="source"><tabbed_widget parent="main_window"><Tab id="t1" containers="1">)"
         uR"(<Container><DockArea id="a1" name="View"><plot id="plot1" mode="TimeSeries">%1</plot></DockArea>)"
         uR"(</Container></Tab></tabbed_widget><previouslyLoaded_Datafiles>%2</previouslyLoaded_Datafiles>%3</root>)"_s
             .arg(curves, files, extra_root)
             .toUtf8();
}

// Every dataset's source identity, in catalog order.
QStringList datasetSources(PJ::MainWindow& window) {
  QStringList sources;
  PJ::AppSession& app = PresetTestPeer::session(window);
  for (const auto& [id, name] : app.catalogModel().datasets()) {
    (void)name;
    sources.push_back(PresetTestPeer::fileLoader(window).sourcePathForDataset(id));
  }
  return sources;
}

class RemotePresetReplayTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(data_dir_.isValid());
    const QString plugin = QString::fromUtf8(PJ_MOCK_FILE_SOURCE_PLUGIN_PATH);
    ASSERT_TRUE(QFile::copy(plugin, extensions_dir_.filePath(QFileInfo(plugin).fileName())));
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_.path());
    QObject::connect(
        window_.get(), &PJ::MainWindow::layoutUrlLoadFinished, [this](const QUrl& url, bool ok, const QString& reason) {
          EXPECT_EQ(ok, reason.isEmpty()) << reason.toStdString();
          outcomes_.push_back({url, ok ? u"ok"_s : reason});
        });
  }

  // Opens `layout_path` from the server, waits until the load settles and
  // returns how it ended: "ok" or the failure reason. Exactly one terminal.
  QString loadPreset(const QString& layout_path) {
    const qsizetype before = outcomes_.size();
    window_->loadLayoutFromUrl(server_.url(layout_path));
    EXPECT_TRUE(pumpUntil([&] { return outcomes_.size() > before; })) << layout_path.toStdString();
    EXPECT_FALSE(PresetTestPeer::loadPending(*window_));
    pj_app_test::flushQueuedEvents();
    EXPECT_EQ(outcomes_.size(), before + 1) << layout_path.toStdString();
    if (outcomes_.size() <= before) {
      return u"<unsettled>"_s;
    }
    EXPECT_EQ(outcomes_[before].url, server_.url(layout_path));
    return outcomes_[before].reason;
  }

  [[nodiscard]] QString lastOutcome() const {
    return outcomes_.isEmpty() ? u"<none>"_s : outcomes_.back().reason;
  }

  // A local dataset plotted through a desktop layout: the workspace a failed
  // replay must give back.
  void openLocalWorkspace() {
    const QString path = pj_app_test::makeMockFile(data_dir_, u"base.mock"_s);
    ASSERT_TRUE(pj_app_test::loadAndWait(PresetTestPeer::fileLoader(*window_), path, pj_app_test::mockLoadHints()));
    QByteArray layout = presetLayout({}, uR"(<curve topic="mock/file_data" field="value"/>)"_s);
    layout.replace("binding=\"source\"", "binding=\"generic\"");
    const QString layout_path = data_dir_.filePath(u"base.pj4.xml"_s);
    QFile file(layout_path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(layout);
    file.close();
    bool settled = false;
    const auto connection =
        QObject::connect(window_.get(), &PJ::MainWindow::layoutRestoreSettled, [&settled](bool success) {
          EXPECT_TRUE(success);
          settled = true;
        });
    PresetTestPeer::loadLayoutFile(*window_, layout_path);
    ASSERT_TRUE(pumpUntil([&settled] { return settled; }));
    QObject::disconnect(connection);
    ASSERT_EQ(PresetTestPeer::curveCount(*window_), 1);
    ASSERT_EQ(datasetSources(*window_).size(), 1);
  }

  // The failed or cancelled replay left the workspace and the data exactly as they were.
  void expectUnchanged(const Workspace& before, const QStringList& sources_before) {
    EXPECT_EQ(datasetSources(*window_), sources_before);
    EXPECT_TRUE(PresetTestPeer::capture(*window_) == before);
    closeDialogs();
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir data_dir_;
  RouteServer server_;
  std::unique_ptr<PJ::MainWindow> window_;
  // Every layoutUrlLoadFinished, in order: the URL and "ok" or the reason.
  struct Outcome {
    QUrl url;
    QString reason;
  };
  QList<Outcome> outcomes_;
};

// Both sources of a preset load from beside the layout (a sibling directory
// included) and the layout applies, without a picker or any other prompt.
TEST_F(RemotePresetReplayTest, RelativePairNeedsNoPicker) {
  const QByteArray left = "left bytes";
  const QByteArray right = "right bytes";
  server_.route(
      u"/presets/pair/layout.pj4.xml"_s,
      presetLayout({{u"data/left.mock"_s, sha256(left)}, {u"../shared/right.mock"_s, {}}}));
  server_.route(u"/presets/pair/data/left.mock"_s, left);
  server_.route(u"/presets/shared/right.mock"_s, right);
  int dialogs_seen = 0;
  QTimer dialog_watch;
  QObject::connect(&dialog_watch, &QTimer::timeout, [&dialogs_seen] { dialogs_seen += visibleDialogs(); });
  dialog_watch.start(10);

  window_->loadLayoutFromUrl(server_.url(u"/presets/pair/layout.pj4.xml"_s));
  // A second preset while one is pending is refused at once, fetching nothing,
  // and the pending one carries on.
  window_->loadLayoutFromUrl(server_.url(u"/presets/other/layout.pj4.xml"_s));
  ASSERT_EQ(outcomes_.size(), 1);
  EXPECT_EQ(outcomes_[0].url, server_.url(u"/presets/other/layout.pj4.xml"_s));
  EXPECT_EQ(outcomes_[0].reason, u"busy"_s);
  ASSERT_TRUE(pumpUntil([this] { return outcomes_.size() > 1; }));
  pj_app_test::flushQueuedEvents();
  ASSERT_EQ(outcomes_.size(), 2);
  EXPECT_EQ(outcomes_[1].url, server_.url(u"/presets/pair/layout.pj4.xml"_s));
  EXPECT_EQ(outcomes_[1].reason, u"ok"_s);

  EXPECT_EQ(dialogs_seen, 0);
  EXPECT_EQ(
      server_.requests,
      (QStringList{
          u"/presets/pair/layout.pj4.xml"_s, u"/presets/pair/data/left.mock"_s, u"/presets/shared/right.mock"_s}));
  const QStringList sources = datasetSources(*window_);
  ASSERT_EQ(sources.size(), 2);
  for (const QString& source : sources) {
    EXPECT_TRUE(PJ::isBrowserUploadIdentity(source)) << source.toStdString();
  }
  const QStringList keys = PresetTestPeer::curveKeys(*window_);
  ASSERT_EQ(keys.size(), 2);
  EXPECT_NE(keys[0], keys[1]) << "each curve binds to its own source's dataset";
}

// A preset's recording may be larger than its small files (index, layout,
// thumbnail): one above their cap still loads.
TEST_F(RemotePresetReplayTest, RecordingAboveTheSmallFileCapLoads) {
  const QByteArray recording(static_cast<qsizetype>(PJ::kMaxPresetDownloadBytes + 1), 'x');
  server_.route(u"/large/layout.pj4.xml"_s, presetLayout({{u"recording.mock"_s, sha256(recording)}}));
  server_.route(u"/large/recording.mock"_s, recording);
  // The download's progress is reported for the layout's URL, ending with the whole recording.
  QList<QUrl> progress_urls;
  std::pair<qint64, qint64> last_progress{-1, -1};
  QObject::connect(
      window_.get(), &PJ::MainWindow::layoutUrlLoadProgress, [&](const QUrl& url, qint64 received, qint64 total) {
        progress_urls.push_back(url);
        last_progress = {received, total};
      });

  EXPECT_EQ(loadPreset(u"/large/layout.pj4.xml"_s), u"ok"_s);
  EXPECT_EQ(datasetSources(*window_).size(), 1);
  ASSERT_FALSE(progress_urls.isEmpty());
  for (const QUrl& url : progress_urls) {
    EXPECT_EQ(url, server_.url(u"/large/layout.pj4.xml"_s));
  }
  EXPECT_EQ(last_progress, (std::pair<qint64, qint64>{recording.size(), recording.size()}));
}

// A preset that loads replaces the data the preset before it brought, and only
// that: data the user opened stays, and a preset that fails keeps the last one.
TEST_F(RemotePresetReplayTest, NextPresetReplacesThePreviousPresetsData) {
  openLocalWorkspace();
  const QString user_source = datasetSources(*window_).front();
  server_.route(u"/first/layout.pj4.xml"_s, presetLayout({{u"first.mock"_s, {}}}));
  server_.route(u"/first/first.mock"_s, "first bytes");
  server_.route(u"/broken/layout.pj4.xml"_s, presetLayout({{u"absent.mock"_s, {}}}));
  server_.route(u"/second/layout.pj4.xml"_s, presetLayout({{u"left.mock"_s, {}}, {u"right.mock"_s, {}}}));
  server_.route(u"/second/left.mock"_s, "left bytes");
  server_.route(u"/second/right.mock"_s, "right bytes");

  ASSERT_EQ(loadPreset(u"/first/layout.pj4.xml"_s), u"ok"_s);
  const QStringList first_sources = datasetSources(*window_);
  ASSERT_EQ(first_sources.size(), 2);
  EXPECT_EQ(first_sources.front(), user_source);

  EXPECT_EQ(loadPreset(u"/broken/layout.pj4.xml"_s), u"download"_s);
  closeDialogs();
  EXPECT_EQ(datasetSources(*window_), first_sources);

  ASSERT_EQ(loadPreset(u"/second/layout.pj4.xml"_s), u"ok"_s);
  const QStringList second_sources = datasetSources(*window_);
  ASSERT_EQ(second_sources.size(), 3);
  EXPECT_EQ(second_sources.front(), user_source);
  EXPECT_FALSE(second_sources.contains(first_sources.back()));
  EXPECT_EQ(PresetTestPeer::curveCount(*window_), 2);
}

// A preset dataset that a merge folded the user's data into holds the user's data too, even
// when it is the merge's anchor and keeps the preset's source identity: the next preset leaves it.
TEST_F(RemotePresetReplayTest, NextPresetKeepsAMergedPresetDataset) {
  openLocalWorkspace();
  server_.route(u"/first/layout.pj4.xml"_s, presetLayout({{u"first.mock"_s, {}}}));
  server_.route(u"/first/first.mock"_s, "first bytes");
  server_.route(u"/second/layout.pj4.xml"_s, presetLayout({{u"second.mock"_s, {}}}));
  server_.route(u"/second/second.mock"_s, "second bytes");
  ASSERT_EQ(loadPreset(u"/first/layout.pj4.xml"_s), u"ok"_s);

  PJ::AppSession& app = PresetTestPeer::session(*window_);
  PJ::DatasetId user_dataset = 0;
  PJ::DatasetId preset_dataset = 0;
  for (const auto& [id, name] : app.catalogModel().datasets()) {
    (void)name;
    const bool from_preset = PJ::isBrowserUploadIdentity(PresetTestPeer::fileLoader(*window_).sourcePathForDataset(id));
    (from_preset ? preset_dataset : user_dataset) = id;
  }
  ASSERT_NE(user_dataset, 0U);
  ASSERT_NE(preset_dataset, 0U);
  ASSERT_EQ(app.mergeDatasets({preset_dataset, user_dataset}), preset_dataset)
      << "the case needs the preset's dataset as the merge anchor";
  ASSERT_EQ(app.catalogModel().datasets().size(), 1U);

  ASSERT_EQ(loadPreset(u"/second/layout.pj4.xml"_s), u"ok"_s);
  QList<PJ::DatasetId> remaining;
  for (const auto& [id, name] : app.catalogModel().datasets()) {
    (void)name;
    remaining.push_back(id);
  }
  EXPECT_TRUE(remaining.contains(preset_dataset)) << "the merged dataset, with the user's data, was removed";
  EXPECT_EQ(remaining.size(), 2);
}

// A source whose bytes do not match the layout's fingerprint, or whose
// fingerprint is malformed, fails the preset before anything is imported.
TEST_F(RemotePresetReplayTest, FingerprintMismatchFails) {
  openLocalWorkspace();
  const Workspace before = PresetTestPeer::capture(*window_);
  const QStringList sources_before = datasetSources(*window_);
  int loads = 0;
  QObject::connect(&PresetTestPeer::fileLoader(*window_), &PJ::FileLoader::requestStarted, [&loads] { ++loads; });

  server_.route(
      u"/mismatch/layout.pj4.xml"_s, presetLayout({{u"left.mock"_s, sha256("other bytes")}, {u"right.mock"_s, {}}}));
  server_.route(u"/mismatch/left.mock"_s, "left bytes");
  server_.route(u"/mismatch/right.mock"_s, "right bytes");
  EXPECT_EQ(loadPreset(u"/mismatch/layout.pj4.xml"_s), u"fingerprint"_s);
  expectUnchanged(before, sources_before);
  EXPECT_FALSE(server_.requests.contains(u"/mismatch/right.mock"_s)) << "the failure stops further downloads";

  QByteArray malformed = presetLayout({{u"left.mock"_s, {}}});
  malformed.replace("prefix=\"\"", "prefix=\"\" content_sha256=\"ABCDEF\"");
  server_.route(u"/malformed/layout.pj4.xml"_s, malformed);
  server_.route(u"/malformed/left.mock"_s, "left bytes");
  EXPECT_EQ(loadPreset(u"/malformed/layout.pj4.xml"_s), u"fingerprint"_s);
  expectUnchanged(before, sources_before);
  EXPECT_FALSE(server_.requests.contains(u"/malformed/left.mock"_s)) << "a malformed record is refused up front";
  EXPECT_EQ(loads, 0);
}

// Every way a preset can fail after it started — a missing file, an import
// error, a cancel while downloading or importing, a local resource, a layout
// that does not apply — removes what it imported and gives the previous
// workspace back, leaving unrelated loads alone.
TEST_F(RemotePresetReplayTest, FailedOrCancelledReplayRestoresWorkspace) {
  openLocalWorkspace();
  const Workspace before = PresetTestPeer::capture(*window_);
  const QStringList sources_before = datasetSources(*window_);
  const QList<Source> pair{{u"left.mock"_s, {}}, {u"right.mock"_s, {}}};
  server_.route(u"/ok/left.mock"_s, "left bytes");
  server_.route(u"/ok/right.mock"_s, "right bytes");

  // The second file is missing.
  server_.route(u"/ok/missing.pj4.xml"_s, presetLayout({{u"left.mock"_s, {}}, {u"absent.mock"_s, {}}}));
  EXPECT_EQ(loadPreset(u"/ok/missing.pj4.xml"_s), u"download"_s);
  expectUnchanged(before, sources_before);

  // The second file fails to import after the first one succeeded.
  server_.route(
      u"/ok/import.pj4.xml"_s,
      presetLayout({{u"left.mock"_s, {}}, {u"right.mock"_s, {}, uR"({"fail_start":true})"_s}}));
  EXPECT_EQ(loadPreset(u"/ok/import.pj4.xml"_s), u"import"_s);
  expectUnchanged(before, sources_before);

  // Each recording fits the data cap, but together they exceed the preset's.
  const QByteArray big(static_cast<qsizetype>(PJ::kMaxPresetDataBytes / 2 + 1), 'x');
  server_.route(u"/big/layout.pj4.xml"_s, presetLayout(pair));
  server_.route(u"/big/left.mock"_s, big);
  server_.route(u"/big/right.mock"_s, big);
  EXPECT_EQ(loadPreset(u"/big/layout.pj4.xml"_s), u"size"_s);
  expectUnchanged(before, sources_before);
  EXPECT_TRUE(server_.requests.contains(u"/big/right.mock"_s));

  // Both sources import but a curve names a field neither of them has.
  server_.route(
      u"/ok/apply.pj4.xml"_s,
      presetLayout(pair, uR"(<curve topic="mock/file_data" field="absent" dataset_path="left.mock"/>)"_s));
  EXPECT_EQ(loadPreset(u"/ok/apply.pj4.xml"_s), u"apply-after-source-load"_s);
  expectUnchanged(before, sources_before);

#ifdef PJ_WITH_SCENE3D
  // A web-hosted layout may not point a robot model at local files.
  server_.route(
      u"/ok/local-root.pj4.xml"_s,
      presetLayout(
          pair, {}, uR"(<robot_model source_type="topic" package_url_roots='{"pkg":"file:///opt/pkg/"}'/>)"_s));
  const qsizetype requests_before = server_.requests.size();
  EXPECT_EQ(loadPreset(u"/ok/local-root.pj4.xml"_s), u"resources"_s);
  expectUnchanged(before, sources_before);
  EXPECT_EQ(server_.requests.mid(requests_before), QStringList{u"/ok/local-root.pj4.xml"_s})
      << "the layout is refused before any of its sources is fetched";
#endif

  // Cancelled while the layout itself downloads; its late answer changes nothing.
  server_.route(u"/slow/layout.pj4.xml"_s, presetLayout(pair));
  server_.hold(u"/slow/layout.pj4.xml"_s);
  window_->loadLayoutFromUrl(server_.url(u"/slow/layout.pj4.xml"_s));
  ASSERT_TRUE(pumpUntil([this] { return server_.requests.contains(u"/slow/layout.pj4.xml"_s); }));
  window_->cancelLayoutFromUrl();
  EXPECT_FALSE(PresetTestPeer::loadPending(*window_));
  EXPECT_EQ(lastOutcome(), u"cancelled"_s);
  server_.release();
  pj_app_test::flushQueuedEvents(10);
  expectUnchanged(before, sources_before);
  EXPECT_FALSE(server_.requests.contains(u"/slow/left.mock"_s));

  // Cancelled while the second file downloads; its late answer changes nothing.
  server_.route(u"/held/layout.pj4.xml"_s, presetLayout(pair));
  server_.route(u"/held/left.mock"_s, "left bytes");
  server_.route(u"/held/right.mock"_s, "right bytes");
  server_.hold(u"/held/right.mock"_s);
  window_->loadLayoutFromUrl(server_.url(u"/held/layout.pj4.xml"_s));
  ASSERT_TRUE(pumpUntil([this] { return server_.requests.contains(u"/held/right.mock"_s); }));
  window_->cancelLayoutFromUrl();
  ASSERT_TRUE(pumpUntil([this] { return !PresetTestPeer::loadPending(*window_); }));
  EXPECT_EQ(lastOutcome(), u"cancelled"_s);
  server_.release();
  pj_app_test::flushQueuedEvents(10);
  expectUnchanged(before, sources_before);

  // Cancelled while its imports run: only this preset's loads stop, a load the
  // user queued meanwhile still lands.
  const QString unrelated = pj_app_test::makeMockFile(data_dir_, u"unrelated.mock"_s);
  PJ::FileLoader& loader = PresetTestPeer::fileLoader(*window_);
  bool cancelled = false;
  const auto started = QObject::connect(&loader, &PJ::FileLoader::requestStarted, [&] {
    if (cancelled) {
      return;
    }
    cancelled = true;
    QTimer::singleShot(0, window_.get(), [&] {
      loader.loadFile(unrelated, nullptr, pj_app_test::mockLoadHints());
      window_->cancelLayoutFromUrl();
    });
  });
  server_.route(u"/ok/cancel.pj4.xml"_s, presetLayout(pair));
  EXPECT_EQ(loadPreset(u"/ok/cancel.pj4.xml"_s), u"cancelled"_s);
  QObject::disconnect(started);
  ASSERT_TRUE(cancelled);
  ASSERT_TRUE(pumpUntil([&] { return datasetSources(*window_).size() == 2; }));
  pj_app_test::flushQueuedEvents(10);
  EXPECT_EQ(datasetSources(*window_), (QStringList{sources_before.front(), unrelated}));
  closeDialogs();
}

// Text a downloaded layout supplies reaches its warning box escaped: markup in
// a source name is shown, never rendered.
TEST_F(RemotePresetReplayTest, LayoutTextInAWarningIsPlainText) {
  // The XML spelling of the filename `/<b>bold</b><a href=x>link</a>.mock`, a local path a web layout may not name.
  server_.route(
      u"/markup/layout.pj4.xml"_s,
      presetLayout({{u"/&lt;b&gt;bold&lt;/b&gt;&lt;a href=x&gt;link&lt;/a&gt;.mock"_s, {}}}));
  EXPECT_EQ(loadPreset(u"/markup/layout.pj4.xml"_s), u"source-url"_s);
  QString shown;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (widget->objectName() == u"pjMessageBox"_s && widget->isVisible()) {
      if (const auto* body = widget->findChild<QLabel*>(u"pjMessageBoxBody"_s)) {
        shown = body->text();
      }
    }
  }
  EXPECT_FALSE(shown.contains(u"<b>"_s)) << shown.toStdString();
  EXPECT_TRUE(shown.contains(u"&lt;b&gt;bold&lt;/b&gt;&lt;a href=x&gt;link&lt;/a&gt;"_s)) << shown.toStdString();
  closeDialogs();
}

}  // namespace
