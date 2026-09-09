// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "LayoutXml.h"
#include "MainWindow.h"
#include "SourceTimelineController.h"
#include "dataset_test_helpers.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {

// Reaches MainWindow's private data-source save/apply seam plus the Source
// Timeline controller, so the round-trip runs against the real widget tree.
class MainWindowSourceLayoutTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }

  [[nodiscard]] static QDomElement appendSources(MainWindow& window, QDomDocument& doc, const QDir& layout_dir) {
    return window.appendDataSourceElement(doc, layout_dir);
  }

  static void applyTimeline(MainWindow& window, const QList<layout_xml::DataSourceRef>& sources) {
    window.applyTimelineStateFromLayout(sources);
  }

  static void setTrackOrder(MainWindow& window, std::vector<DatasetId> order) {
    ASSERT_NE(window.source_timeline_controller_, nullptr);
    window.source_timeline_controller_->setDisplayOrder(std::move(order));
  }

  [[nodiscard]] static std::vector<DatasetId> trackOrder(const MainWindow& window) {
    EXPECT_NE(window.source_timeline_controller_, nullptr);
    return window.source_timeline_controller_ != nullptr ? window.source_timeline_controller_->currentTrackOrder()
                                                         : std::vector<DatasetId>{};
  }
};

}  // namespace PJ

namespace {

// Creates a dataset on its own TimeDomain (required for a per-source offset) and
// writes a two-sample topic, mirroring FileLoader's one-domain-per-source.
PJ::DatasetId addDataset(PJ::AppSession& app, std::string_view source_name, std::string_view topic) {
  const PJ::DatasetId dataset = pj_test::createDataset(app, source_name, /*own_time_domain=*/true);
  if (dataset == 0 || pj_test::addScalarTopic(app, dataset, topic, /*first_ts=*/1'000, /*second_ts=*/2'000) == 0) {
    return 0;
  }
  return dataset;
}

// One file fans out into two datasets; each carries its own alignment offset and
// timeline slot. The v4 <dataset> children must serialize every track and the
// apply path must restore each on the matching fan-out sibling — never swapping.
TEST(MainWindowSourceLayoutTest, OneFanoutFileRoundTripsEveryOffsetAndTrackOrder) {
  QTemporaryDir extensions_dir;
  QTemporaryDir project_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(project_dir.isValid());
  const QString source_path = project_dir.filePath(QStringLiteral("data/run.mcap"));
  ASSERT_TRUE(QDir().mkpath(QFileInfo(source_path).absolutePath()));
  QFile source_file(source_path);
  ASSERT_TRUE(source_file.open(QIODevice::WriteOnly));
  source_file.close();

  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = PJ::MainWindowSourceLayoutTestPeer::session(window);
  PJ::SessionManager& session = app.sessionManager();
  const PJ::DatasetId left = addDataset(app, "run/left", "/left");
  const PJ::DatasetId right = addDataset(app, "run/right", "/right");
  ASSERT_NE(left, 0U);
  ASSERT_NE(right, 0U);
  app.catalogModel().rebuildFromDatastore();
  session.setDatasetSourcePath(left, source_path);
  session.setDatasetSourcePath(right, source_path);
  session.recordLoadedSource(source_path, QString{}, QStringLiteral("MCAP"), QStringLiteral(R"({"__pj_fanout":[]})"));

  session.setDisplayOffset(left, PJ::DisplayOffset{PJ::Duration{11'000}});
  session.setDisplayOffset(right, PJ::DisplayOffset{PJ::Duration{-22'000}});
  PJ::MainWindowSourceLayoutTestPeer::setTrackOrder(window, {right, left});

  QDomDocument doc;
  QDomElement root = doc.createElement(QStringLiteral("root"));
  root.setAttribute(QStringLiteral("pj4_version"), QStringLiteral("4"));
  doc.appendChild(root);
  const QDir layout_dir(project_dir.path());
  QDomElement wrapper = PJ::MainWindowSourceLayoutTestPeer::appendSources(window, doc, layout_dir);
  ASSERT_FALSE(wrapper.isNull());
  root.appendChild(wrapper);

  const QDomNodeList file_infos = wrapper.elementsByTagName(QStringLiteral("fileInfo"));
  ASSERT_EQ(file_infos.size(), 1) << "the source file must be replayed once, not once per fan-out dataset";
  const QDomElement file_info = file_infos.at(0).toElement();
  ASSERT_EQ(file_info.elementsByTagName(QStringLiteral("dataset")).size(), 2);

  const QList<PJ::layout_xml::DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, layout_dir);
  ASSERT_EQ(refs.size(), 1);
  ASSERT_EQ(refs.front().datasets.size(), 2);
  EXPECT_EQ(refs.front().datasets[0].source_name, QStringLiteral("run/left"));
  EXPECT_EQ(refs.front().datasets[0].source_index, 0);
  EXPECT_EQ(refs.front().datasets[0].display_offset_ns, 11'000);
  EXPECT_EQ(refs.front().datasets[0].timeline_order, 1);
  EXPECT_EQ(refs.front().datasets[1].source_name, QStringLiteral("run/right"));
  EXPECT_EQ(refs.front().datasets[1].source_index, 1);
  EXPECT_EQ(refs.front().datasets[1].display_offset_ns, -22'000);
  EXPECT_EQ(refs.front().datasets[1].timeline_order, 0);

  session.setDisplayOffset(left, PJ::DisplayOffset{PJ::Duration{0}});
  session.setDisplayOffset(right, PJ::DisplayOffset{PJ::Duration{0}});
  PJ::MainWindowSourceLayoutTestPeer::setTrackOrder(window, {left, right});
  PJ::MainWindowSourceLayoutTestPeer::applyTimeline(window, refs);

  EXPECT_EQ(session.sourceDisplayOffset(left).value.count(), 11'000);
  EXPECT_EQ(session.sourceDisplayOffset(right).value.count(), -22'000);
  EXPECT_EQ(PJ::MainWindowSourceLayoutTestPeer::trackOrder(window), (std::vector<PJ::DatasetId>{right, left}));
}

// A dataset carrying a SessionManager::SourceRecord (provider provenance) must
// save a <materialize> child under its <fileInfo> — a SIBLING of <plugin>, with
// provider/identity attributes and the descriptor JSON as VERBATIM CDATA bytes
// (a cross-repo identity contract, so it must survive the real XML pipeline
// byte-exact, "]]>" included). A recordless source must emit no such child.
TEST(MainWindowSourceLayoutTest, PrimaryDatasetSourceRecordEmitsMaterializeChild) {
  QTemporaryDir extensions_dir;
  QTemporaryDir project_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(project_dir.isValid());
  const auto make_source_file = [&project_dir](const QString& name) {
    const QString path = project_dir.filePath(name);
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
    return path;
  };
  const QString cloud_path = make_source_file(QStringLiteral("cache.mcap"));
  const QString plain_path = make_source_file(QStringLiteral("plain.mcap"));

  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = PJ::MainWindowSourceLayoutTestPeer::session(window);
  PJ::SessionManager& session = app.sessionManager();
  const PJ::DatasetId cloud_dataset = addDataset(app, "cache.mcap", "/cloud");
  const PJ::DatasetId plain_dataset = addDataset(app, "plain.mcap", "/plain");
  ASSERT_NE(cloud_dataset, 0U);
  ASSERT_NE(plain_dataset, 0U);
  app.catalogModel().rebuildFromDatastore();
  session.setDatasetSourcePath(cloud_dataset, cloud_path);
  session.setDatasetSourcePath(plain_dataset, plain_path);
  session.recordLoadedSource(
      cloud_path, QString{}, QStringLiteral("MCAP Loader"), QStringLiteral("{}"), QStringLiteral("mcap-loader"));
  session.recordLoadedSource(plain_path, QString{}, QStringLiteral("MCAP Loader"), QStringLiteral("{}"));

  const QString descriptor = QStringLiteral("{\n  \"key\": \"cloud/a ]]> b.mcap\",\n  \"v\": 1\n}");
  session.attachSourceRecord(
      cloud_dataset, PJ::SourceRecord{
                         .provider_id = QStringLiteral("mcap-cloud"),
                         .source_identity = QStringLiteral("mcap-cloud:v1:sha256/128:ab12"),
                         .descriptor_json = descriptor,
                     });

  QDomDocument doc;
  QDomElement root = doc.createElement(QStringLiteral("root"));
  root.setAttribute(QStringLiteral("pj4_version"), QStringLiteral("4"));
  doc.appendChild(root);
  const QDir layout_dir(project_dir.path());
  QDomElement wrapper = PJ::MainWindowSourceLayoutTestPeer::appendSources(window, doc, layout_dir);
  ASSERT_FALSE(wrapper.isNull());
  root.appendChild(wrapper);

  // Locate each file's <fileInfo>. The recorded source order is stable, but key
  // off the filename attribute so the assertion doesn't depend on it.
  QDomElement cloud_info;
  QDomElement plain_info;
  for (QDomElement file_info = wrapper.firstChildElement(QStringLiteral("fileInfo")); !file_info.isNull();
       file_info = file_info.nextSiblingElement(QStringLiteral("fileInfo"))) {
    const QString filename = file_info.attribute(QStringLiteral("filename"));
    if (filename.endsWith(QStringLiteral("cache.mcap"))) {
      cloud_info = file_info;
    } else if (filename.endsWith(QStringLiteral("plain.mcap"))) {
      plain_info = file_info;
    }
  }
  ASSERT_FALSE(cloud_info.isNull());
  ASSERT_FALSE(plain_info.isNull());

  // Old-reader pin: <materialize> is emitted as a DIRECT child of <fileInfo>,
  // a sibling of the <plugin> element (never nested inside it).
  const QDomElement materialize = cloud_info.firstChildElement(QStringLiteral("materialize"));
  ASSERT_FALSE(materialize.isNull());
  EXPECT_EQ(materialize.parentNode(), cloud_info);
  const QDomElement cloud_plugin = cloud_info.firstChildElement(QStringLiteral("plugin"));
  ASSERT_FALSE(cloud_plugin.isNull());
  EXPECT_EQ(cloud_plugin.parentNode(), cloud_info);
  // The plugin element writes BOTH identities: the display name (ID — what old
  // readers keep consuming) and the stable manifest id, when one was recorded.
  EXPECT_EQ(cloud_plugin.attribute(QStringLiteral("ID")), QStringLiteral("MCAP Loader"));
  EXPECT_EQ(cloud_plugin.attribute(QStringLiteral("manifest_id")), QStringLiteral("mcap-loader"));
  const QDomElement plain_plugin = plain_info.firstChildElement(QStringLiteral("plugin"));
  ASSERT_FALSE(plain_plugin.isNull());
  EXPECT_EQ(plain_plugin.attribute(QStringLiteral("ID")), QStringLiteral("MCAP Loader"));
  EXPECT_FALSE(plain_plugin.hasAttribute(QStringLiteral("manifest_id")))
      << "a source recorded without a manifest id must not grow the attribute";
  EXPECT_EQ(materialize.attribute(QStringLiteral("provider")), QStringLiteral("mcap-cloud"));
  EXPECT_EQ(materialize.attribute(QStringLiteral("identity")), QStringLiteral("mcap-cloud:v1:sha256/128:ab12"));
  EXPECT_TRUE(plain_info.firstChildElement(QStringLiteral("materialize")).isNull())
      << "a recordless source must not grow a materialize child";

  // Verbatim descriptor bytes through the REAL pipeline: serialize + reparse +
  // extractDataSource must return byte-identical descriptor JSON.
  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QList<PJ::layout_xml::DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, layout_dir);
  ASSERT_EQ(refs.size(), 2);
  const auto cloud_ref = std::find_if(refs.cbegin(), refs.cend(), [](const PJ::layout_xml::DataSourceRef& ref) {
    return ref.serialized_path.endsWith(QStringLiteral("cache.mcap"));
  });
  ASSERT_NE(cloud_ref, refs.cend());
  EXPECT_EQ(cloud_ref->materialize_provider, QStringLiteral("mcap-cloud"));
  EXPECT_EQ(cloud_ref->materialize_identity, QStringLiteral("mcap-cloud:v1:sha256/128:ab12"));
  EXPECT_EQ(cloud_ref->materialize_descriptor_json, descriptor);
  EXPECT_EQ(cloud_ref->plugin_id, QStringLiteral("MCAP Loader"));
  EXPECT_EQ(cloud_ref->plugin_manifest_id, QStringLiteral("mcap-loader"));
}

// T9 (M3, acceptance row 9): re-save after a cache hit. A host-cache restore
// leaves exactly this session state behind (LayoutImportBatch's loadCommitting
// + the shell's source registration): dataset source path = the cache
// artifact, a plugin-less loadedSources entry, and a SourceRecord whose
// identity is the host digest. The save walk must re-emit <fileInfo> +
// <materialize> with the descriptor bytes VERBATIM, so the next restore keys
// the same cache slot.
TEST(MainWindowSourceLayoutTest, CacheHitSessionStateResavesMaterializeWithByteExactDescriptor) {
  QTemporaryDir extensions_dir;
  QTemporaryDir project_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(project_dir.isValid());
  const QString artifact_path = project_dir.filePath(QStringLiteral("source_cache/deadbeef.mcap"));
  ASSERT_TRUE(QDir().mkpath(QFileInfo(artifact_path).absolutePath()));
  QFile artifact(artifact_path);
  ASSERT_TRUE(artifact.open(QIODevice::WriteOnly));
  artifact.close();

  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = PJ::MainWindowSourceLayoutTestPeer::session(window);
  PJ::SessionManager& session = app.sessionManager();
  const PJ::DatasetId dataset = addDataset(app, "cloud-download", "/imu");
  ASSERT_NE(dataset, 0U);
  app.catalogModel().rebuildFromDatastore();
  session.setDatasetSourcePath(dataset, artifact_path);
  session.recordLoadedSource(artifact_path, QString{});  // the wireSourceCapture form: no plugin identity
  // Odd spacing and unicode on purpose: the bytes are the cross-repo identity
  // contract and must survive the save walk untouched.
  const QString descriptor = QStringLiteral("{\"kind\":\"k\",  \"request\":{\"path\":\"caf\\u00e9\"},\n\"v\":1}");
  session.attachSourceRecord(
      dataset, PJ::SourceRecord{
                   .provider_id = QStringLiteral("mcap-cloud"),
                   .source_identity = QStringLiteral("sha256/128:0123456789abcdef0123456789abcdef"),
                   .descriptor_json = descriptor,
               });

  QDomDocument doc;
  QDomElement root = doc.createElement(QStringLiteral("root"));
  root.setAttribute(QStringLiteral("pj4_version"), QStringLiteral("4"));
  doc.appendChild(root);
  const QDir layout_dir(project_dir.path());
  QDomElement wrapper = PJ::MainWindowSourceLayoutTestPeer::appendSources(window, doc, layout_dir);
  ASSERT_FALSE(wrapper.isNull()) << "a plugin-less cache-hit source must still be saved";
  root.appendChild(wrapper);

  // Round-trip through the real XML pipeline: serialize, reparse, extract.
  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QList<PJ::layout_xml::DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, layout_dir);
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().resolved_path.endsWith(QStringLiteral("deadbeef.mcap")));
  EXPECT_EQ(refs.front().materialize_provider, QStringLiteral("mcap-cloud"));
  EXPECT_EQ(refs.front().materialize_identity, QStringLiteral("sha256/128:0123456789abcdef0123456789abcdef"));
  EXPECT_EQ(refs.front().materialize_descriptor_json, descriptor) << "descriptor bytes must round-trip byte-exact";
}

}  // namespace
