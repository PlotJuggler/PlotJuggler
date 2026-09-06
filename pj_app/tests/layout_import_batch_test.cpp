// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Tests for LayoutImportBatch — the §6.2 rewrite-then-classify restore
// coordinator. The provider is the shared descriptor-scripted fake toolbox
// (tests/support/fake_import_provider.h, registered via
// registerStaticToolbox), so each test declares its scenario in the layout
// document itself. Stock cache-hit loads run through the real FileLoader +
// the SDK's mock_file_source_plugin. Shell effects (workspace checkpoint/
// rollback, dataset removal, the consolidated trust prompt, diagnostics)
// are injected hook recorders — the batch is MainWindow-free by design.

#include <gtest/gtest.h>

#include <QDomDocument>
#include <QDomElement>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "FileLoader.h"
#include "LayoutImportBatch.h"
#include "LayoutXml.h"
#include "ToolboxHostWiring.h"
#include "dataset_test_helpers.h"
#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/sdk/data_source_patterns.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/McapRecordingWriter.h"
#include "pj_runtime/PluginRuntimeCatalog.h"
#include "pj_runtime/RecordingFormat.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/SourceCacheStore.h"
#include "pj_runtime/SourceCaptureService.h"
#include "support/fake_import_provider.h"
#include "support/loader_test_support.h"
using namespace Qt::StringLiterals;

#ifndef PJ_MOCK_FILE_SOURCE_PLUGIN_PATH
#error "PJ_MOCK_FILE_SOURCE_PLUGIN_PATH must be defined"
#endif
#ifndef PJ_RUNTIME_HOST_OBJECT_PARSER_PATH
#error "PJ_RUNTIME_HOST_OBJECT_PARSER_PATH must be defined"
#endif

namespace {

using namespace pj_fake_import;
using pj_app_test::flushQueuedEvents;
using pj_app_test::pumpUntil;

constexpr const char* kMockPluginName = "Mock File Source";
constexpr const char* kMockManifestId = "mock-file-source";

// A statically registered file source claiming ".mcap", so a host-cache
// artifact can be loaded through the real FileLoader in-process (the real
// MCAP loader is an external plugin). Content-agnostic: writes one record.
constexpr const char* kStaticMcapPluginName = "Static MCAP Source";
constexpr const char* kStaticMcapManifestId = "static-mcap-source";

// Test knob: a true makes every StaticMcapSource import fail, modelling a
// cached artifact the stock loader cannot ingest (the quarantine trigger).
std::atomic<bool> g_static_mcap_fail{false};

class StaticMcapSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
  }
  std::string saveConfig() const override {
    return config_;
  }
  PJ::Status loadConfig(std::string_view config_json) override {
    config_ = std::string(config_json);
    return PJ::okStatus();
  }
  PJ::Status importData() override {
    if (g_static_mcap_fail.load()) {
      return PJ::unexpected("static mcap source instructed to fail");
    }
    auto topic = writeHost().ensureTopic("cache/data");
    if (!topic) {
      return PJ::unexpected(topic.error());
    }
    return writeHost().appendRecord(*topic, PJ::Timestamp{100}, {{.name = "value", .value = 1.0}});
  }

 private:
  std::string config_ = "{}";
};

[[nodiscard]] const PJ_data_source_vtable_t* staticMcapVtable() {
  static const PJ_data_source_vtable_t* vt = PJ::DataSourcePluginBase::vtableWithCreate(
      []() noexcept -> void* {
        try {
          return new StaticMcapSource();
        } catch (...) {
          return nullptr;
        }
      },
      R"({"id":"static-mcap-source","name":"Static MCAP Source","version":"1.0.0",)"
      R"("file_extensions":[".mcap"]})");
  return vt;
}

// ---------------------------------------------------------------------------
// Layout-document builder
// ---------------------------------------------------------------------------

struct SourceSpec {
  QString filename;    // <fileInfo filename>
  QString descriptor;  // empty = plain source (no <materialize>)
  QString provider = QString::fromUtf8(kProviderId);
  QString identity = u"id-1"_s;  // <materialize identity>
  // The <plugin> the stock loader replays with (host-cache-hit tests point it
  // at the static ".mcap" source below).
  QString plugin_name = QString::fromUtf8(kMockPluginName);
  QString plugin_manifest = QString::fromUtf8(kMockManifestId);
};

[[nodiscard]] QDomDocument makeLayoutDoc(const QList<SourceSpec>& specs) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  root.setAttribute(u"pj4_version"_s, u"4"_s);
  root.setAttribute(u"binding"_s, u"source"_s);
  doc.appendChild(root);
  QDomElement wrapper = doc.createElement(u"previouslyLoaded_Datafiles"_s);
  root.appendChild(wrapper);
  for (const SourceSpec& spec : specs) {
    QDomElement file_info = doc.createElement(u"fileInfo"_s);
    file_info.setAttribute(u"filename"_s, spec.filename);
    QDomElement plugin = doc.createElement(u"plugin"_s);
    plugin.setAttribute(u"ID"_s, spec.plugin_name);
    plugin.setAttribute(u"manifest_id"_s, spec.plugin_manifest);
    PJ::layout_xml::appendJsonAsCdata(doc, plugin, u"{}"_s);
    file_info.appendChild(plugin);
    if (!spec.descriptor.isEmpty()) {
      QDomElement materialize = doc.createElement(u"materialize"_s);
      materialize.setAttribute(u"provider"_s, spec.provider);
      materialize.setAttribute(u"identity"_s, spec.identity);
      PJ::layout_xml::appendJsonAsCdata(doc, materialize, spec.descriptor);
      file_info.appendChild(materialize);
    }
    wrapper.appendChild(file_info);
    // One curve qualifier per source so the remap pass is observable.
    QDomElement curve = doc.createElement(u"curve"_s);
    curve.setAttribute(u"dataset_path"_s, spec.filename);
    root.appendChild(curve);
  }
  return doc;
}

[[nodiscard]] QStringList fileInfoFilenames(const QDomDocument& doc) {
  QStringList names;
  const QDomElement wrapper = doc.documentElement().firstChildElement(u"previouslyLoaded_Datafiles"_s);
  for (QDomElement file_info = wrapper.firstChildElement(u"fileInfo"_s); !file_info.isNull();
       file_info = file_info.nextSiblingElement(u"fileInfo"_s)) {
    names.push_back(file_info.attribute(u"filename"_s));
  }
  return names;
}

[[nodiscard]] QStringList curveDatasetPaths(const QDomDocument& doc) {
  QStringList paths;
  for (QDomElement curve = doc.documentElement().firstChildElement(u"curve"_s); !curve.isNull();
       curve = curve.nextSiblingElement(u"curve"_s)) {
    paths.push_back(curve.attribute(u"dataset_path"_s));
  }
  return paths;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class LayoutImportBatchTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(data_dir_.isValid());
    const QString plugin_src = QString::fromUtf8(PJ_MOCK_FILE_SOURCE_PLUGIN_PATH);
    const QString plugin_dst = extensions_dir_.filePath(QFileInfo(plugin_src).fileName());
    ASSERT_TRUE(QFile::copy(plugin_src, plugin_dst)) << "could not stage " << plugin_src.toStdString();
    // The parser the capture-mode fake provider pushes through, so a wired
    // capture records real bytes and publishes a real cache artifact.
    const QString parser_src = QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH);
    const QString parser_dst = extensions_dir_.filePath(QFileInfo(parser_src).fileName());
    ASSERT_TRUE(QFile::copy(parser_src, parser_dst)) << "could not stage " << parser_src.toStdString();

    app_session_ = std::make_unique<PJ::AppSession>(extensions_dir_.path());
    ASSERT_FALSE(app_session_->extensionCatalog().findSourcesForExtension(u".mock"_s).empty());
    ASSERT_NE(app_session_->extensionCatalog().findParserByEncoding(u"runtime_host_object"_s), nullptr);
    ASSERT_TRUE(app_session_->extensionCatalog().pluginCatalog().registerStaticToolbox(&kFakeVtable));
    PJ::StaticPluginSet static_plugins;
    static_plugins.data_sources.emplace_back(staticMcapVtable());
    ASSERT_TRUE(app_session_->extensionCatalog().pluginCatalog().registerStaticPlugins(static_plugins));
    ASSERT_FALSE(app_session_->extensionCatalog().findSourcesForExtension(u".mcap"_s).empty());

    // The M3 host cache every batch of this fixture consults (empty unless a
    // test publishes into it, so the pre-existing scenarios are unaffected).
    ASSERT_TRUE(cache_dir_.isValid());
    cache_store_ = std::make_unique<PJ::SourceCacheStore>(std::filesystem::path(cache_dir_.path().toStdU16String()));
    capture_service_ = std::make_unique<PJ::SourceCaptureService>(*cache_store_);

    loader_ = std::make_unique<PJ::FileLoader>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), app_session_->catalogModel());
    // The shell (MainWindow::onFileLoaded) is what records loaded sources;
    // mirror that wiring so the batch's already-loaded classification sees
    // the same registry it does in production.
    QObject::connect(
        loader_.get(), &PJ::FileLoader::fileLoaded, loader_.get(),
        [this](
            const QString& path, const QString& prefix, const QString& plugin_id, const QString& config,
            const QString& manifest_id) {
          app_session_->sessionManager().recordLoadedSource(path, prefix, plugin_id, config, manifest_id);
        });

    g_log.clear();
    g_instance = nullptr;
    g_next_dataset_id.store(101);
    g_static_mcap_fail.store(false);
  }

  void TearDown() override {
    batch_.reset();
    loader_->joinForShutdown();
  }

  [[nodiscard]] QString makeMockFile(const QString& name) {
    return pj_app_test::makeMockFile(data_dir_, name);
  }

  // Loads `path` through the real pipeline so loadedSources()/catalog have it.
  void preloadMockFile(const QString& path) {
    ASSERT_TRUE(pj_app_test::loadAndWait(*loader_, path, pj_app_test::mockLoadHints()));
    ASSERT_FALSE(app_session_->catalogModel().isEmpty());
  }

  PJ::LayoutImportBatch& makeBatch(bool interactive, std::uint64_t max_transfer_bytes = 0) {
    PJ::LayoutImportBatch::Hooks hooks;
    hooks.begin_workspace_checkpoint = [this]() {
      ++capture_calls_;
      return [this]() {
        ++restore_calls_;
        return restore_result_;
      };
    };
    hooks.remove_dataset = [this](PJ::DatasetId id) { removed_.push_back(id); };
    hooks.confirm_import = [this](const QStringList& lines) {
      if (fail_if_confirm_) {
        ADD_FAILURE() << "confirm_import must never be called on this batch";
      }
      ++confirm_calls_;
      confirm_lines_ = lines;
      return confirm_choice_;
    };
    hooks.diagnostics = [this](const PJ::Diagnostic& diagnostic) { diagnostics_.push_back(diagnostic); };
    batch_ = std::make_unique<PJ::LayoutImportBatch>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), *loader_, app_session_->catalogModel(),
        capture_service_.get(), interactive, max_transfer_bytes, std::move(hooks));
    finished_count_ = 0;
    QObject::connect(batch_.get(), &PJ::LayoutImportBatch::finished, batch_.get(), [this]() { ++finished_count_; });
    return *batch_;
  }

  // extract + makeBatch + prepare — the common per-test preamble.
  PJ::LayoutImportBatch& prepareBatch(QDomDocument& doc, bool interactive, std::uint64_t max_transfer_bytes = 0) {
    const QList<PJ::layout_xml::DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir(data_dir_.path()));
    PJ::LayoutImportBatch& batch = makeBatch(interactive, max_transfer_bytes);
    batch.prepare(doc, refs);
    return batch;
  }

  // start() (must report kRunning) + pump to the finished state.
  [[nodiscard]] bool runToFinish(PJ::LayoutImportBatch& batch) {
    if (batch.start() != PJ::LayoutImportBatch::StartResult::kRunning) {
      ADD_FAILURE() << "start() did not report kRunning";
      return false;
    }
    return pumpUntil([&batch]() { return batch.isFinished(); });
  }

  [[nodiscard]] int diagnosticCount(const std::string& id) const {
    return static_cast<int>(
        std::count_if(diagnostics_.begin(), diagnostics_.end(), [&id](const PJ::Diagnostic& d) { return d.id == id; }));
  }

  // The live dataset loaded from `path` (0 = none) — for attaching records.
  [[nodiscard]] PJ::DatasetId datasetForPath(const QString& path) const {
    for (const auto& [dataset_id, name] : app_session_->catalogModel().datasets()) {
      static_cast<void>(name);
      if (PJ::layout_xml::isSamePath(loader_->sourcePathForDataset(dataset_id), path)) {
        return dataset_id;
      }
    }
    return 0;
  }

  [[nodiscard]] bool sourceLoaded(const QString& path) const {
    const auto& loaded = app_session_->sessionManager().loadedSources();
    return std::any_of(
        loaded.begin(), loaded.end(), [&path](const auto& src) { return PJ::layout_xml::isSamePath(src.path, path); });
  }

  // Publishes a manifest-valid single-message capture artifact for
  // (provider, descriptor) into the host cache, releasing the publish pin.
  void publishArtifact(const QString& provider, const QString& descriptor) {
    const std::string identity = PJ::sourceCacheIdentity(provider.toStdString(), descriptor.toStdString());
    auto txn = cache_store_->beginPublish(identity);
    ASSERT_TRUE(txn.has_value());
    PJ::McapRecordingWriter writer;
    ASSERT_TRUE(writer.open(txn->partialPath(), PJ::RecordingInfo{}).has_value());
    auto channel = writer.addChannel(
        PJ::RecordedBinding{.topic = "/imu", .encoding = "raw", .type_name = "t", .schema_bytes = {}});
    ASSERT_TRUE(channel.has_value());
    const uint8_t payload[] = {1, 2, 3};
    ASSERT_TRUE(writer.write(*channel, 100, {payload, 3}).has_value());
    PJ::RecordingSummary summary;
    QJsonObject topics;
    topics.insert(u"/imu"_s, 1);
    const QJsonObject body{
        {u"version"_s, static_cast<int>(PJ::kCaptureManifestVersion)},
        {u"provider_id"_s, provider},
        {u"identity"_s, QString::fromStdString(identity)},
        {u"attests_empty_topics"_s, false},
        {u"requested_topic_messages"_s, topics},
        {u"total_messages"_s, 1},
    };
    summary.extra_metadata.emplace_back(
        std::string(PJ::kCaptureMetadataName), QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
    ASSERT_TRUE(writer.close(summary).has_value());
    auto pinned = cache_store_->publish(identity, std::move(*txn));
    ASSERT_TRUE(pinned.has_value());
    published_artifact_path_ = QString::fromStdU16String(pinned->path.u16string());
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir data_dir_;
  QTemporaryDir cache_dir_;
  std::unique_ptr<PJ::AppSession> app_session_;
  std::unique_ptr<PJ::SourceCacheStore> cache_store_;
  std::unique_ptr<PJ::SourceCaptureService> capture_service_;
  QString published_artifact_path_;
  std::unique_ptr<PJ::FileLoader> loader_;
  std::unique_ptr<PJ::LayoutImportBatch> batch_;

  int capture_calls_ = 0;
  int restore_calls_ = 0;
  bool restore_result_ = true;
  std::vector<PJ::DatasetId> removed_;
  int confirm_calls_ = 0;
  QStringList confirm_lines_;
  PJ::LayoutImportBatch::TrustChoice confirm_choice_ = PJ::LayoutImportBatch::TrustChoice::kTrustAndImport;
  bool fail_if_confirm_ = false;
  std::vector<PJ::Diagnostic> diagnostics_;
  int finished_count_ = 0;
};

using Outcome = PJ::LayoutImportBatch::SourceOutcome;
using StartResult = PJ::LayoutImportBatch::StartResult;
using TrustChoice = PJ::LayoutImportBatch::TrustChoice;

// ---------------------------------------------------------------------------
// The classify matrix: already-loaded / cache hit / miss, with a plain
// (no-materialize) sibling that the batch must never touch. Also pins the
// §6.2 step 4+5 document rewrite (fileInfo filename + qualifier remap).
// ---------------------------------------------------------------------------
TEST_F(LayoutImportBatchTest, ClassifyMatrixRewritesDocumentAndResolvesEachClass) {
  const QString loaded_path = makeMockFile(u"loaded.mock"_s);
  preloadMockFile(loaded_path);
  const QString hit_path = makeMockFile(u"hit.mock"_s);
  const QString miss_path = data_dir_.filePath(u"missing.mock"_s);

  // Saved paths deliberately differ from the provider's effective paths so
  // the rewrite is observable.
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/loaded.mock"_s, makeDescriptor(u"s1"_s, u"trusted"_s, loaded_path)},
      SourceSpec{u"/saved/hit.mock"_s, makeDescriptor(u"s2"_s, u"trusted"_s, hit_path)},
      SourceSpec{u"/saved/miss.mock"_s, makeDescriptor(u"s3"_s, u"trusted"_s, miss_path)},
      SourceSpec{u"/saved/plain.mock"_s, QString{}},
  });
  const QList<PJ::layout_xml::DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir(data_dir_.path()));
  ASSERT_EQ(refs.size(), 4);

  PJ::LayoutImportBatch& batch = makeBatch(/*interactive=*/false);
  batch.prepare(doc, refs);

  // Step 4: the materialize-bearing fileInfo filenames now carry the
  // provider's effective paths; the plain sibling is untouched.
  EXPECT_EQ(fileInfoFilenames(doc), (QStringList{loaded_path, hit_path, miss_path, u"/saved/plain.mock"_s}));
  // Step 5: qualifier remap saved -> effective, plain sibling untouched.
  EXPECT_EQ(curveDatasetPaths(doc), (QStringList{loaded_path, hit_path, miss_path, u"/saved/plain.mock"_s}));

  // The already-loaded source needs no work; hit + miss do.
  EXPECT_TRUE(batch.hasPendingWork());
  EXPECT_EQ(batch.pendingSourceLines(), (QStringList{hit_path, miss_path}));

  ASSERT_TRUE(runToFinish(batch));

  const PJ::LayoutImportBatch::BatchResult& result = batch.result();
  ASSERT_EQ(result.sources.size(), 3) << "plain sources must not produce batch results";
  EXPECT_FALSE(result.cancelled);
  EXPECT_EQ(result.sources[0].outcome, Outcome::kResolvedAlreadyLoaded);
  EXPECT_EQ(result.sources[0].effective_path, loaded_path);
  EXPECT_EQ(result.sources[1].outcome, Outcome::kResolvedCacheHit);
  EXPECT_TRUE(sourceLoaded(hit_path)) << "the cache hit must have gone through the stock loader";
  EXPECT_EQ(result.sources[2].outcome, Outcome::kResolvedImported);
  EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"s3"})) << "only the miss may start an import job";
  EXPECT_EQ(finished_count_, 1);
  EXPECT_EQ(confirm_calls_, 0) << "all-trusted sources must not prompt";
}

// A batch whose only materialize source is already loaded has nothing to do.
TEST_F(LayoutImportBatchTest, AlreadyLoadedOnlyReportsNoAsyncWork) {
  const QString loaded_path = makeMockFile(u"loaded.mock"_s);
  preloadMockFile(loaded_path);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{loaded_path, makeDescriptor(u"s1"_s, u"trusted"_s, loaded_path)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  EXPECT_FALSE(batch.hasPendingWork());
  EXPECT_EQ(batch.start(), StartResult::kNoAsyncWork);
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedAlreadyLoaded);
  EXPECT_EQ(capture_calls_, 0) << "no work -> no workspace checkpoint";
}

// ---------------------------------------------------------------------------
// Trust gate
// ---------------------------------------------------------------------------

TEST_F(LayoutImportBatchTest, NonInteractiveNeedsConfirmationFailsWithDiagnosticAndNoDialog) {
  fail_if_confirm_ = true;
  const QString miss_path = data_dir_.filePath(u"missing.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/a.mock"_s, makeDescriptor(u"s1"_s, u"confirm"_s, miss_path)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  EXPECT_EQ(batch.start(), StartResult::kNoAsyncWork);
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kFailed);
  EXPECT_GE(diagnosticCount("layout-import-untrusted"), 1);
  EXPECT_TRUE(g_log.snapshot().empty()) << "an untrusted non-interactive source must never start a job";
}

TEST_F(LayoutImportBatchTest, InteractiveConfirmationTrustAndImportRunsTheJob) {
  const QString miss_path = data_dir_.filePath(u"missing.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/a.mock"_s, makeDescriptor(u"s1"_s, u"confirm"_s, miss_path)},
  });

  confirm_choice_ = TrustChoice::kTrustAndImport;
  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/true);
  ASSERT_TRUE(runToFinish(batch));
  EXPECT_EQ(confirm_calls_, 1);
  ASSERT_EQ(confirm_lines_.size(), 1);
  EXPECT_TRUE(confirm_lines_[0].contains(miss_path)) << confirm_lines_[0].toStdString();
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedImported);
}

TEST_F(LayoutImportBatchTest, InteractiveConfirmationSkipSkipsTheSources) {
  const QString miss_path = data_dir_.filePath(u"missing.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/a.mock"_s, makeDescriptor(u"s1"_s, u"confirm"_s, miss_path)},
  });

  confirm_choice_ = TrustChoice::kSkip;
  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/true);
  EXPECT_EQ(batch.start(), StartResult::kNoAsyncWork);
  EXPECT_EQ(confirm_calls_, 1);
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kSkippedUntrusted);
  EXPECT_TRUE(g_log.snapshot().empty());
}

TEST_F(LayoutImportBatchTest, InteractiveConfirmationCancelAbortsBeforeAnyWork) {
  const QString hit_path = makeMockFile(u"hit.mock"_s);
  const QString miss_path = data_dir_.filePath(u"missing.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/hit.mock"_s, makeDescriptor(u"s1"_s, u"trusted"_s, hit_path)},
      SourceSpec{u"/saved/a.mock"_s, makeDescriptor(u"s2"_s, u"confirm"_s, miss_path)},
  });

  confirm_choice_ = TrustChoice::kCancelLayout;
  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/true);
  EXPECT_EQ(batch.start(), StartResult::kCancelledByUser);
  EXPECT_EQ(capture_calls_, 0) << "cancel at the trust gate must precede any mutation";
  EXPECT_FALSE(loader_->isBusy()) << "not even the trusted cache hit may have been enqueued";
  EXPECT_TRUE(g_log.snapshot().empty());
}

TEST_F(LayoutImportBatchTest, RefusedDescriptorAlwaysFails) {
  const QString hit_path = makeMockFile(u"hit.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/hit.mock"_s, makeDescriptor(u"s1"_s, u"refused"_s, hit_path)},
  });

  fail_if_confirm_ = true;
  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/true);
  EXPECT_EQ(batch.start(), StartResult::kNoAsyncWork);
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kFailed);
  EXPECT_GE(diagnosticCount("layout-import-refused"), 1);
  EXPECT_FALSE(sourceLoaded(hit_path)) << "a refused source must not be loaded, even from a local cache file";
}

TEST_F(LayoutImportBatchTest, NeedsConfirmationCacheHitLoadsWithoutPrompt) {
  // Trust gates the NETWORK touch; a cache hit downloads nothing (§10:
  // cache file present -> no network), so no confirmation is required.
  fail_if_confirm_ = true;
  const QString hit_path = makeMockFile(u"hit.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/hit.mock"_s, makeDescriptor(u"s1"_s, u"confirm"_s, hit_path)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/true);
  ASSERT_TRUE(runToFinish(batch));
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedCacheHit);
  EXPECT_TRUE(sourceLoaded(hit_path));
}

// ---------------------------------------------------------------------------
// Guard 3 (§7): estimated_bytes vs the batch's max_transfer_bytes ceiling.
// ---------------------------------------------------------------------------

TEST_F(LayoutImportBatchTest, Guard3NonInteractiveOverLimitRefusesAtLimitProceeds) {
  fail_if_confirm_ = true;
  const QString over_path = data_dir_.filePath(u"over.mock"_s);
  const QString at_path = data_dir_.filePath(u"at.mock"_s);
  const QString unknown_path = data_dir_.filePath(u"unknown.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/over.mock"_s, makeDescriptor(u"over"_s, u"trusted"_s, over_path, 1001)},
      SourceSpec{u"/saved/at.mock"_s, makeDescriptor(u"at"_s, u"trusted"_s, at_path, 1000)},
      SourceSpec{u"/saved/unknown.mock"_s, makeDescriptor(u"unknown"_s, u"trusted"_s, unknown_path, 0)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false, /*max_transfer_bytes=*/1000);
  ASSERT_TRUE(runToFinish(batch));

  ASSERT_EQ(batch.result().sources.size(), 3);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kFailed) << "estimated > ceiling must refuse";
  EXPECT_GE(diagnosticCount("layout-import-size-limit"), 1);
  EXPECT_EQ(batch.result().sources[1].outcome, Outcome::kResolvedImported) << "estimated == ceiling proceeds";
  EXPECT_EQ(batch.result().sources[2].outcome, Outcome::kResolvedImported) << "unknown estimate (0) proceeds";
  EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"at", "unknown"}));
  // The ceiling is the guard-3 enforcement channel: every started job must
  // carry it.
  EXPECT_EQ(g_log.maxBytes(), (std::vector<std::uint64_t>{1000, 1000}));
}

TEST_F(LayoutImportBatchTest, Guard3InteractiveOverLimitJoinsTheConfirmation) {
  const QString over_path = data_dir_.filePath(u"over.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/over.mock"_s, makeDescriptor(u"over"_s, u"trusted"_s, over_path, 2000)},
  });

  confirm_choice_ = TrustChoice::kTrustAndImport;
  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/true, /*max_transfer_bytes=*/1000);
  ASSERT_TRUE(runToFinish(batch));
  EXPECT_EQ(confirm_calls_, 1) << "a trusted-but-over-ceiling source still needs explicit confirmation";
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedImported);
}

// ---------------------------------------------------------------------------
// §6.4 provider-absent / invalid-descriptor stock fallback
// ---------------------------------------------------------------------------

TEST_F(LayoutImportBatchTest, ProviderAbsentFallsBackToStockLoadPerSavedPath) {
  const QString exists_path = makeMockFile(u"cached.mock"_s);
  const QString missing_path = data_dir_.filePath(u"gone.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{exists_path, makeDescriptor(u"s1"_s, u"trusted"_s, u"/other/effective.mock"_s), u"no-such-provider"_s},
      SourceSpec{
          missing_path, makeDescriptor(u"s2"_s, u"trusted"_s, u"/other/effective2.mock"_s), u"no-such-provider"_s},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);

  // No provider -> no rewrite: the document keeps its saved paths.
  EXPECT_EQ(fileInfoFilenames(doc), (QStringList{exists_path, missing_path}));
  EXPECT_EQ(diagnosticCount("layout-import-provider-unavailable"), 2) << "one distinct diagnostic per source";

  ASSERT_TRUE(runToFinish(batch));
  ASSERT_EQ(batch.result().sources.size(), 2);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedCacheHit)
      << "the strongest degraded mode: hinted file exists -> stock load proceeds";
  EXPECT_TRUE(sourceLoaded(exists_path));
  EXPECT_EQ(batch.result().sources[1].outcome, Outcome::kFailed);
  EXPECT_GE(diagnosticCount("layout-import-source-missing"), 1);
  EXPECT_TRUE(g_log.snapshot().empty()) << "no provider -> no import jobs";
}

TEST_F(LayoutImportBatchTest, InvalidDescriptorFallsBackToStockLoad) {
  const QString exists_path = makeMockFile(u"cached.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{exists_path, u"this is not json"_s},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  EXPECT_GE(diagnosticCount("layout-import-descriptor-invalid"), 1);
  ASSERT_TRUE(runToFinish(batch));
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedCacheHit);
}

// ---------------------------------------------------------------------------
// Sequential jobs, failure isolation, cancel/rollback, finished-once
// ---------------------------------------------------------------------------

TEST_F(LayoutImportBatchTest, MissJobsRunSequentiallyThroughTerminalChaining) {
  const QString first_path = data_dir_.filePath(u"first.mock"_s);
  const QString second_path = data_dir_.filePath(u"second.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/first.mock"_s, makeDescriptor(u"first"_s, u"trusted"_s, first_path, 0, u"block"_s)},
      SourceSpec{u"/saved/second.mock"_s, makeDescriptor(u"second"_s, u"trusted"_s, second_path)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  ASSERT_EQ(batch.start(), StartResult::kRunning);
  flushQueuedEvents();
  EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"first"}))
      << "the second job must not start while the first is still running";

  ASSERT_NE(g_instance, nullptr);
  g_instance->releaseStart();
  ASSERT_TRUE(pumpUntil([&batch]() { return batch.isFinished(); }));
  EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"first", "second"}));
  ASSERT_EQ(batch.result().sources.size(), 2);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedImported);
  EXPECT_EQ(batch.result().sources[1].outcome, Outcome::kResolvedImported);
  EXPECT_EQ(finished_count_, 1);
}

TEST_F(LayoutImportBatchTest, PerJobFailureIsolationContinuesRemainingJobs) {
  const QString first_path = data_dir_.filePath(u"first.mock"_s);
  const QString second_path = data_dir_.filePath(u"second.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/first.mock"_s, makeDescriptor(u"first"_s, u"trusted"_s, first_path, 0, u"fail"_s)},
      SourceSpec{u"/saved/second.mock"_s, makeDescriptor(u"second"_s, u"trusted"_s, second_path)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  ASSERT_TRUE(runToFinish(batch));

  ASSERT_EQ(batch.result().sources.size(), 2);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kFailed);
  EXPECT_FALSE(batch.result().sources[0].message.isEmpty());
  EXPECT_EQ(batch.result().sources[1].outcome, Outcome::kResolvedImported)
      << "a failed job must not abort the remaining jobs";
  EXPECT_FALSE(batch.result().cancelled);
  EXPECT_EQ(restore_calls_, 0) << "per-job failure is not a rollback";
  EXPECT_TRUE(removed_.empty());
}

TEST_F(LayoutImportBatchTest, EagerOnlyOutcomeResolvesWithDiagnostic) {
  const QString miss_path = data_dir_.filePath(u"missing.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/a.mock"_s, makeDescriptor(u"s1"_s, u"trusted"_s, miss_path, 0, u"eager"_s)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  ASSERT_TRUE(runToFinish(batch));
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedImported);
  EXPECT_GE(diagnosticCount("layout-import-eager-only"), 1)
      << "an EAGER_ONLY dataset is usable but carries no import record — diagnose it";
}

TEST_F(LayoutImportBatchTest, CancelRollsBackProducedDatasetsAndWorkspace) {
  const QString first_path = data_dir_.filePath(u"first.mock"_s);
  const QString second_path = data_dir_.filePath(u"second.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/first.mock"_s, makeDescriptor(u"first"_s, u"trusted"_s, first_path)},
      SourceSpec{u"/saved/second.mock"_s, makeDescriptor(u"second"_s, u"trusted"_s, second_path, 0, u"block"_s)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  ASSERT_EQ(batch.start(), StartResult::kRunning);
  EXPECT_EQ(capture_calls_, 1) << "the prior workspace must be checkpointed before the first mutation";

  // Wait until the FIRST job resolved (its dataset 101 is produced) while
  // the second sits blocked on its gate.
  ASSERT_TRUE(pumpUntil([&batch]() { return batch.result().sources[0].outcome == Outcome::kResolvedImported; }));
  EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"first", "second"}));

  batch.cancel();
  ASSERT_TRUE(pumpUntil([&batch]() { return batch.isFinished(); }));

  EXPECT_TRUE(batch.result().cancelled);
  EXPECT_EQ(batch.result().sources[1].outcome, Outcome::kCancelled);
  EXPECT_EQ(removed_, (std::vector<PJ::DatasetId>{101})) << "the produced dataset must be removed on rollback";
  EXPECT_EQ(restore_calls_, 1) << "the prior workspace must be restored exactly once";
  EXPECT_EQ(finished_count_, 1);

  // Repeated cancel after the terminal state must be inert.
  batch.cancel();
  flushQueuedEvents();
  EXPECT_EQ(finished_count_, 1);
  EXPECT_EQ(restore_calls_, 1);
}

TEST_F(LayoutImportBatchTest, FinishedIsEmittedExactlyOnceAndOnlyViaTheEventLoop) {
  const QString hit_path = makeMockFile(u"hit.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/hit.mock"_s, makeDescriptor(u"s1"_s, u"trusted"_s, hit_path)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  ASSERT_EQ(batch.start(), StartResult::kRunning);
  EXPECT_EQ(finished_count_, 0) << "finished must never fire synchronously from start()";
  ASSERT_TRUE(pumpUntil([&batch]() { return batch.isFinished(); }));
  flushQueuedEvents();
  EXPECT_EQ(finished_count_, 1);
}

// ---------------------------------------------------------------------------
// T7 correlation + per-job cancel: activeImportDataset() mirrors the ACTIVE
// job's zero-or-one dataset announcement, and cancelActiveImportJob() is the
// keep-partial per-job stop (D3) — never the batch cancel/rollback path.
// ---------------------------------------------------------------------------

TEST_F(LayoutImportBatchTest, ActiveImportDatasetTracksTheRunningJobsAnnouncedId) {
  const QString first_path = data_dir_.filePath(u"first.mock"_s);
  const QString second_path = data_dir_.filePath(u"second.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{
          u"/saved/first.mock"_s, makeDescriptor(u"first"_s, u"trusted"_s, first_path, 0, u"announce-then-block"_s)},
      SourceSpec{u"/saved/second.mock"_s, makeDescriptor(u"second"_s, u"trusted"_s, second_path, 0, u"block"_s)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  EXPECT_FALSE(batch.activeImportDataset().has_value()) << "no job started yet";
  ASSERT_EQ(batch.start(), StartResult::kRunning);

  // The first job announces its dataset and then HOLDS on the resume gate:
  // the correlation must expose exactly that id while the job still runs.
  ASSERT_TRUE(pumpUntil([&batch]() { return batch.activeImportDataset().has_value(); }));
  EXPECT_EQ(*batch.activeImportDataset(), 101u);
  EXPECT_FALSE(batch.isFinished());
  EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"first"})) << "the second job must not have started";

  // The terminal clears the correlation BEFORE the startNextJob() chain:
  // once the (blocked, announces-nothing-yet) second job is up, the surface
  // must be empty again — never the first job's stale id.
  ASSERT_NE(g_instance, nullptr);
  g_instance->releaseResume();
  ASSERT_TRUE(pumpUntil([]() { return g_log.snapshot().size() == 2u; }));
  EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"first", "second"}));
  EXPECT_FALSE(batch.activeImportDataset().has_value())
      << "the first job's id must be cleared at its terminal, before the next job starts";
  EXPECT_FALSE(batch.isFinished());

  g_instance->releaseStart();  // let the second job run to its terminal
  ASSERT_TRUE(pumpUntil([&batch]() { return batch.isFinished(); }));
  EXPECT_FALSE(batch.activeImportDataset().has_value());
  ASSERT_EQ(batch.result().sources.size(), 2);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedImported);
  EXPECT_EQ(batch.result().sources[1].outcome, Outcome::kResolvedImported);
}

TEST_F(LayoutImportBatchTest, FailModeJobLeavesActiveImportDatasetEmptyEndToEnd) {
  // D5: a job that never announces a dataset (race-loss/failure) leaves the
  // correlation empty for its WHOLE life — no event will ever match it.
  const QString miss_path = data_dir_.filePath(u"missing.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/a.mock"_s, makeDescriptor(u"s1"_s, u"trusted"_s, miss_path, 0, u"fail"_s)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  EXPECT_FALSE(batch.activeImportDataset().has_value());
  ASSERT_EQ(batch.start(), StartResult::kRunning);
  ASSERT_TRUE(pumpUntil([&batch]() {
    EXPECT_FALSE(batch.activeImportDataset().has_value()) << "a zero-on_dataset job must never populate the surface";
    return batch.isFinished();
  }));
  flushQueuedEvents();

  EXPECT_FALSE(batch.activeImportDataset().has_value());
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kFailed);
  EXPECT_EQ(finished_count_, 1) << "the degraded job must still conclude the batch normally";
}

TEST_F(LayoutImportBatchTest, CancelActiveImportJobKeepsPartialAndContinues) {
  const QString first_path = data_dir_.filePath(u"first.mock"_s);
  const QString second_path = data_dir_.filePath(u"second.mock"_s);
  const QString third_path = data_dir_.filePath(u"third.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/first.mock"_s, makeDescriptor(u"first"_s, u"trusted"_s, first_path)},
      SourceSpec{u"/saved/second.mock"_s, makeDescriptor(u"second"_s, u"trusted"_s, second_path, 0, u"block"_s)},
      SourceSpec{u"/saved/third.mock"_s, makeDescriptor(u"third"_s, u"trusted"_s, third_path)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  ASSERT_EQ(batch.start(), StartResult::kRunning);
  // The first job resolves (its dataset 101 enters the ledger); the second
  // starts and sits blocked on its start gate.
  ASSERT_TRUE(pumpUntil([&batch]() { return batch.result().sources[0].outcome == Outcome::kResolvedImported; }));
  ASSERT_EQ(g_log.snapshot(), (std::vector<std::string>{"first", "second"}));
  ASSERT_FALSE(batch.isFinished());

  // D3 keep-partial: ONLY the active job dies; the batch continues to the
  // third job, and nothing the earlier jobs produced is rolled back.
  batch.cancelActiveImportJob();
  ASSERT_TRUE(pumpUntil([&batch]() { return batch.isFinished(); }));
  flushQueuedEvents();

  EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"first", "second", "third"}))
      << "the batch must continue past the cancelled job";
  ASSERT_EQ(batch.result().sources.size(), 3);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedImported);
  EXPECT_EQ(batch.result().sources[1].outcome, Outcome::kCancelled);
  EXPECT_EQ(batch.result().sources[2].outcome, Outcome::kResolvedImported);
  EXPECT_FALSE(batch.result().cancelled) << "a per-job cancel is not a batch cancel";
  EXPECT_TRUE(removed_.empty()) << "keep-partial: earlier produced datasets must survive";
  EXPECT_EQ(restore_calls_, 0) << "keep-partial: no workspace rollback";
  EXPECT_GE(diagnosticCount("layout-import-cancelled"), 1);
  EXPECT_EQ(finished_count_, 1);
}

TEST_F(LayoutImportBatchTest, CancelActiveImportJobWithNoActiveJobIsANoOp) {
  const QString miss_path = data_dir_.filePath(u"missing.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/a.mock"_s, makeDescriptor(u"s1"_s, u"trusted"_s, miss_path)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  // Before start(): nothing to cancel, nothing may change.
  batch.cancelActiveImportJob();
  EXPECT_FALSE(batch.isFinished());
  EXPECT_TRUE(batch.hasPendingWork());

  ASSERT_TRUE(runToFinish(batch));
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedImported);

  // After the batch finished: still a no-op (no new terminal, no signal).
  batch.cancelActiveImportJob();
  flushQueuedEvents();
  EXPECT_EQ(finished_count_, 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedImported);
  EXPECT_EQ(diagnosticCount("layout-import-cancelled"), 0);
}

// ---------------------------------------------------------------------------
// Codex r1 F1: provider provenance + the is_materialized verdict are
// authoritative for classification.
// ---------------------------------------------------------------------------

// Adds/overrides one field of a scripted descriptor (e.g. "materialized").
[[nodiscard]] QString withField(const QString& descriptor, const QString& key, const QJsonValue& value) {
  QJsonObject obj = QJsonDocument::fromJson(descriptor.toUtf8()).object();
  obj.insert(key, value);
  return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

TEST_F(LayoutImportBatchTest, MaterializedFalseWithExistingFileIsAMissNotAHit) {
  // A stale/partial file at the cache path must NOT be promoted to a hit
  // when the provider's verdict says the artifact is not materialized.
  const QString stale_path = makeMockFile(u"stale.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{
          u"/saved/stale.mock"_s,
          withField(makeDescriptor(u"s1"_s, u"trusted"_s, stale_path), u"materialized"_s, false)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  ASSERT_TRUE(runToFinish(batch));
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedImported)
      << "is_materialized=false means MISS: the provider must re-materialize, not the stale file load";
  EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"s1"})) << "the import job must run";
  EXPECT_FALSE(sourceLoaded(stale_path)) << "the stale file must not go through the stock loader";
}

TEST_F(LayoutImportBatchTest, MaterializedTrueWithMissingFileFailsAsProviderBug) {
  const QString gone_path = data_dir_.filePath(u"gone.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{
          u"/saved/gone.mock"_s, withField(makeDescriptor(u"s1"_s, u"trusted"_s, gone_path), u"materialized"_s, true)},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  EXPECT_EQ(batch.start(), StartResult::kNoAsyncWork);
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kFailed)
      << "materialized-but-missing is a provider bug — never import silently over it";
  EXPECT_GE(diagnosticCount("layout-import-materialized-missing"), 1);
  EXPECT_TRUE(g_log.snapshot().empty()) << "no import job may start on an inconsistent cache verdict";
}

TEST_F(LayoutImportBatchTest, PathMatchWithMismatchedSourceRecordIsNotAlreadyLoaded) {
  // The cache slot was reused: a live dataset AT the effective path whose
  // SourceRecord names DIFFERENT provenance must not classify already-loaded.
  const QString loaded_path = makeMockFile(u"loaded.mock"_s);
  preloadMockFile(loaded_path);
  const PJ::DatasetId dataset = datasetForPath(loaded_path);
  ASSERT_NE(dataset, 0u);
  app_session_->sessionManager().attachSourceRecord(
      dataset, PJ::SourceRecord{
                   .provider_id = QString::fromUtf8(kProviderId),
                   .source_identity = u"fake:something-else"_s,
                   .descriptor_json = u"{\"other\":1}"_s,
               });

  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/loaded.mock"_s, makeDescriptor(u"s1"_s, u"trusted"_s, loaded_path)},
  });
  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  ASSERT_TRUE(runToFinish(batch));
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedCacheHit)
      << "provenance mismatch: the source must reload through the stock path, not skip as already-loaded";
}

TEST_F(LayoutImportBatchTest, MatchingSourceRecordIsAlreadyLoaded) {
  const QString loaded_path = makeMockFile(u"loaded.mock"_s);
  preloadMockFile(loaded_path);
  const PJ::DatasetId dataset = datasetForPath(loaded_path);
  ASSERT_NE(dataset, 0u);
  const QString descriptor = makeDescriptor(u"s1"_s, u"trusted"_s, loaded_path);
  app_session_->sessionManager().attachSourceRecord(
      dataset, PJ::SourceRecord{
                   .provider_id = QString::fromUtf8(kProviderId),
                   .source_identity = u"fake:"_s + loaded_path,
                   .descriptor_json = descriptor,
               });

  QDomDocument doc = makeLayoutDoc({SourceSpec{u"/saved/loaded.mock"_s, descriptor}});
  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  EXPECT_FALSE(batch.hasPendingWork());
  EXPECT_EQ(batch.start(), StartResult::kNoAsyncWork);
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedAlreadyLoaded)
      << "an exact {provider, identity, descriptor} record match is the strongest already-loaded proof";
  EXPECT_TRUE(g_log.snapshot().empty());
}

TEST_F(LayoutImportBatchTest, EmptySourceIdentityFailsTheSource) {
  const QString hit_path = makeMockFile(u"hit.mock"_s);
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{
          u"/saved/hit.mock"_s, withField(makeDescriptor(u"s1"_s, u"trusted"_s, hit_path), u"identity"_s, QString())},
  });

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  EXPECT_EQ(batch.start(), StartResult::kNoAsyncWork);
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kFailed)
      << "a query answer without a source identity is malformed (identity is ALWAYS returned, spec 6.3)";
  EXPECT_GE(diagnosticCount("layout-import-query-invalid"), 1);
  EXPECT_FALSE(sourceLoaded(hit_path));
}

// ---------------------------------------------------------------------------
// Codex r2: seams exposed by the provenance fixes.
// ---------------------------------------------------------------------------

// R2-1: when already-loaded matches VIA RECORD but the provider's cache path
// moved since the dataset was loaded, the document must be rewritten to the
// MATCHED DATASET'S LIVE path — the queried path names a file nothing in the
// session is loaded from, so path-tier curve/timeline binding would dangle.
TEST_F(LayoutImportBatchTest, RecordMatchedAlreadyLoadedRewritesToLiveDatasetPath) {
  const QString live_path = makeMockFile(u"live.mock"_s);
  preloadMockFile(live_path);
  const PJ::DatasetId dataset = datasetForPath(live_path);
  ASSERT_NE(dataset, 0u);
  const QString moved_path = data_dir_.filePath(u"moved-cache.mock"_s);  // the NEW cache location
  const QString descriptor =
      withField(makeDescriptor(u"s1"_s, u"trusted"_s, moved_path), u"identity"_s, u"fake:stable-identity"_s);
  app_session_->sessionManager().attachSourceRecord(
      dataset, PJ::SourceRecord{
                   .provider_id = QString::fromUtf8(kProviderId),
                   .source_identity = u"fake:stable-identity"_s,
                   .descriptor_json = descriptor,
               });

  QDomDocument doc = makeLayoutDoc({SourceSpec{u"/saved/live.mock"_s, descriptor}});
  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  EXPECT_FALSE(batch.hasPendingWork());
  EXPECT_EQ(batch.start(), StartResult::kNoAsyncWork);
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedAlreadyLoaded);
  EXPECT_EQ(batch.result().sources[0].effective_path, live_path);
  EXPECT_EQ(fileInfoFilenames(doc), (QStringList{live_path}))
      << "the document must describe the LIVE session, not the moved cache path";
  EXPECT_EQ(curveDatasetPaths(doc), (QStringList{live_path}))
      << "dataset-path qualifiers must bind against the live dataset's tracked path";
}

// R2-2: a provider-resolved cache hit must attach the same provenance a
// promoted miss gets, or the next layout save silently loses <materialize>.
TEST_F(LayoutImportBatchTest, CacheHitAttachesProviderSourceRecord) {
  const QString hit_path = makeMockFile(u"hit.mock"_s);
  const QString descriptor = makeDescriptor(u"s1"_s, u"trusted"_s, hit_path);
  QDomDocument doc = makeLayoutDoc({SourceSpec{u"/saved/hit.mock"_s, descriptor}});

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  ASSERT_TRUE(runToFinish(batch));
  ASSERT_EQ(batch.result().sources.size(), 1);
  ASSERT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedCacheHit);

  const PJ::DatasetId dataset = datasetForPath(hit_path);
  ASSERT_NE(dataset, 0u);
  const PJ::SourceRecord* record = app_session_->sessionManager().sourceRecord(dataset);
  ASSERT_NE(record, nullptr) << "a restored hit must stay re-saveable as an import source";
  EXPECT_EQ(record->provider_id, QString::fromUtf8(kProviderId));
  EXPECT_EQ(record->source_identity, u"fake:"_s + hit_path);
  EXPECT_EQ(record->descriptor_json, descriptor);
}

// Windows CI regression, reproduced portably: whenever the saved path's
// SERIALIZED and RESOLVED forms differ (a relative path here; a drive-less
// "/saved/x" on Windows, which QFileInfo does not treat as absolute), the
// qualifier remap must match the document whichever form it holds — the
// serialized value (raw documents, these fixtures) or the resolved form
// MainWindow's resolveDatasetSourcePaths pre-pass produces.
TEST_F(LayoutImportBatchTest, QualifierRemapMatchesSerializedAndResolvedSavedPaths) {
  const QString hit_path = makeMockFile(u"hit.mock"_s);
  const QString serialized = u"saved-rel/hit.mock"_s;
  const QString resolved = QDir(data_dir_.path()).absoluteFilePath(serialized);
  QDomDocument doc = makeLayoutDoc({SourceSpec{serialized, makeDescriptor(u"s1"_s, u"trusted"_s, hit_path)}});
  // A second qualifier carrying the RESOLVED form (what the shell's
  // resolveDatasetSourcePaths pre-pass writes before prepare() runs).
  QDomElement resolved_curve = doc.createElement(u"curve"_s);
  resolved_curve.setAttribute(u"dataset_path"_s, resolved);
  doc.documentElement().appendChild(resolved_curve);

  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  EXPECT_EQ(fileInfoFilenames(doc), (QStringList{hit_path}));
  EXPECT_EQ(curveDatasetPaths(doc), (QStringList{hit_path, hit_path}))
      << "both the serialized and the pre-pass-resolved qualifier forms must remap";
  ASSERT_TRUE(runToFinish(batch));
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedCacheHit);
}

// ---------------------------------------------------------------------------
// M3 host-cache-first restore.
// ---------------------------------------------------------------------------

// A published capture artifact restores through the STOCK loader alone: no
// provider is consulted (the provider here does not even exist), the record
// is attached for the next layout save, and the produced dataset holds a
// read pin on the artifact until it is removed. The layout deliberately
// names the MOCK plugin — an artifact is always a standard MCAP, so the
// host-cache hit must build its own extension-driven hints instead of
// requiring the layout's original (mismatched) plugin identity.
TEST_F(LayoutImportBatchTest, HostCacheHitRestoresWithoutProviderAndPinsTheArtifact) {
  const QString provider = u"absent.cloud.provider"_s;
  const QString descriptor = uR"({"kind":"k","request":{},"v":1})"_s;
  publishArtifact(provider, descriptor);

  QDomDocument doc = makeLayoutDoc({SourceSpec{u"/saved/cached.mcap"_s, descriptor, provider, u"any-identity"_s}});
  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);

  // The document now describes the artifact the restore will load.
  EXPECT_EQ(fileInfoFilenames(doc), (QStringList{published_artifact_path_}));
  EXPECT_EQ(batch.pendingSourceLines(), (QStringList{published_artifact_path_}));
  // No provider was needed, so no provider-unavailable degrade fired.
  EXPECT_EQ(diagnosticCount("layout-import-provider-unavailable"), 0);

  ASSERT_TRUE(runToFinish(batch));
  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedCacheHit)
      << batch.result().sources[0].message.toStdString();
  // The mismatched plugin naming neither failed the load nor destroyed the
  // artifact (the F5 regression: hint mismatch must not trigger quarantine).
  EXPECT_TRUE(QFileInfo::exists(published_artifact_path_));

  const PJ::DatasetId dataset = datasetForPath(published_artifact_path_);
  ASSERT_NE(dataset, 0u);
  const PJ::SourceRecord* record = app_session_->sessionManager().sourceRecord(dataset);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->provider_id, provider);
  EXPECT_EQ(record->descriptor_json, descriptor);
  EXPECT_EQ(
      record->source_identity,
      QString::fromStdString(PJ::sourceCacheIdentityDigest(provider.toStdString(), descriptor.toStdString())));

  // The dataset's pin blocks quarantine/eviction; removing the dataset
  // releases it.
  const std::string identity = PJ::sourceCacheIdentity(provider.toStdString(), descriptor.toStdString());
  std::string reason;
  EXPECT_FALSE(cache_store_->quarantine(identity, &reason)) << "pinned artifact must not be evictable: " << reason;
  app_session_->sessionManager().removeDataset(dataset);
  EXPECT_TRUE(cache_store_->quarantine(identity, &reason)) << reason;
}

// A cached artifact whose stock load genuinely fails is quarantined and the
// source falls back to the live provider import, so a stale cache never
// blocks a restore — and the corrective document rewrite repoints the
// fileInfo filename + path qualifiers from the quarantined cache path at the
// provider's effective path, so path-tier binding works after the fallback.
TEST_F(LayoutImportBatchTest, FailedHostCacheLoadQuarantinesAndFallsBackToProvider) {
  const QString miss_target = data_dir_.filePath(u"missing.mock"_s);
  const QString descriptor = makeDescriptor(u"s1"_s, u"trusted"_s, miss_target);
  const QString provider = QString::fromUtf8(kProviderId);
  publishArtifact(provider, descriptor);
  g_static_mcap_fail.store(true);  // the artifact load fails inside the plugin

  QDomDocument doc = makeLayoutDoc({SourceSpec{u"/saved/cached.mcap"_s, descriptor, provider}});
  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  // prepare() pointed the document at the cache artifact.
  EXPECT_EQ(fileInfoFilenames(doc), (QStringList{published_artifact_path_}));
  ASSERT_TRUE(runToFinish(batch));

  ASSERT_EQ(batch.result().sources.size(), 1);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedImported)
      << batch.result().sources[0].message.toStdString();
  EXPECT_EQ(diagnosticCount("layout-import-cache-load-failed"), 1);
  // The artifact was quarantined: the next resolve is an absent miss.
  std::string miss;
  EXPECT_FALSE(capture_service_->resolve(provider.toStdString(), descriptor.toStdString(), &miss).has_value());
  EXPECT_FALSE(QFileInfo::exists(published_artifact_path_));
  // Corrective rewrite: the document no longer references the quarantined
  // path — filename and qualifiers now name the provider's effective path.
  EXPECT_EQ(fileInfoFilenames(doc), (QStringList{miss_target}));
  EXPECT_EQ(curveDatasetPaths(doc), (QStringList{miss_target}));
}

// The REAL wireSourceCapture handler, driven from a worker thread the way a
// toolbox release does: it must attach the SourceRecord AND register the
// published artifact as the dataset's saveable backing file (source path +
// loadedSources entry) — the inputs the layout-save walk serializes into a
// <fileInfo> + <materialize>, i.e. what makes a captured download survive a
// layout save (F1).
TEST_F(LayoutImportBatchTest, WireSourceCaptureRegistersPublishedDownloadForLayoutSave) {
  const PJ::DatasetId dataset = pj_test::createDataset(*app_session_, "cloud-download");
  ASSERT_NE(dataset, 0u);
  const QString artifact = makeMockFile(u"captured.mcap"_s);  // a real file: paths are normalized on store

  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  PJ::wireSourceCapture(deps, capture_service_.get(), "cloud.provider", app_session_->sessionManager());
  ASSERT_EQ(deps.capture_service, capture_service_.get());
  ASSERT_EQ(deps.capture_provider_id, "cloud.provider");
  ASSERT_TRUE(deps.on_capture_finalized);

  const std::string descriptor = R"({"kind":"k","request":{},"v":1})";
  std::thread release_thread([&]() {
    deps.on_capture_finalized(
        dataset, "cloud.provider", descriptor, "sha256/128:feed", /*published=*/true, /*refusal_reason=*/"",
        std::filesystem::path(artifact.toStdU16String()));
  });
  release_thread.join();
  ASSERT_TRUE(pumpUntil([&]() { return app_session_->sessionManager().sourceRecord(dataset) != nullptr; }));

  const PJ::SourceRecord* record = app_session_->sessionManager().sourceRecord(dataset);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->provider_id, u"cloud.provider"_s);
  EXPECT_EQ(record->source_identity, u"sha256/128:feed"_s);
  EXPECT_EQ(record->descriptor_json, QString::fromStdString(descriptor));
  EXPECT_TRUE(PJ::layout_xml::isSamePath(app_session_->sessionManager().datasetSourcePath(dataset), artifact));
  const auto& loaded = app_session_->sessionManager().loadedSources();
  EXPECT_TRUE(std::any_of(loaded.begin(), loaded.end(), [&artifact](const auto& src) {
    return PJ::layout_xml::isSamePath(src.path, artifact);
  }));
}

// T7 (acceptance rows 7/9): re-download REPAIRS the cache, then restores
// offline. Run 1: the seeded artifact's stock load fails -> quarantine ->
// provider fallback, whose capture-mode import publishes a REAL replacement
// artifact through the wired capture pipeline (HeadlessDescriptorProviderSession
// runs the same wireSourceCapture as MainWindow). Run 2: the same layout is
// already loaded (record match) — no duplicate dataset. Run 3 (dataset
// removed): the repaired artifact restores through the stock loader alone,
// with no new provider job.
TEST_F(LayoutImportBatchTest, QuarantinedHitIsRepublishedByTheProviderThenRestoresOffline) {
  const QString provider = QString::fromUtf8(kProviderId);
  // The script rides inside "request" so the descriptor keeps the host's
  // attach envelope (kind/request/v only) and the capture can arm on it.
  const QString descriptor = u"{\"kind\":\"capture-test\",\"request\":%1,\"v\":1}"_s.arg(
      makeDescriptor(u"s1"_s, u"trusted"_s, data_dir_.filePath(u"unused.mock"_s), 0, u"capture"_s));
  publishArtifact(provider, descriptor);
  g_static_mcap_fail.store(true);  // run 1's hit load fails inside the plugin

  // --- Run 1: hit fails -> quarantine -> capture-mode provider fallback. ---
  {
    QDomDocument doc = makeLayoutDoc({SourceSpec{u"/saved/cached.mcap"_s, descriptor, provider}});
    PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
    ASSERT_TRUE(runToFinish(batch));
    ASSERT_EQ(batch.result().sources.size(), 1);
    EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedImported)
        << batch.result().sources[0].message.toStdString();
    EXPECT_EQ(diagnosticCount("layout-import-cache-load-failed"), 1);
    EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"s1"}));
  }
  batch_.reset();

  // The capture wiring republished a REAL artifact (same identity, so the
  // same slot the quarantined seed occupied) and, on the GUI thread, attached
  // the produced dataset's provenance + backing file. The manifest proves the
  // bytes are the REPUBLISHED capture, not the seed: the capture-mode fake
  // records "/s1", the seed declared "/imu".
  QString repaired_path;
  ASSERT_TRUE(pumpUntil([&]() {
    std::string miss;
    auto resolved = capture_service_->resolve(provider.toStdString(), descriptor.toStdString(), &miss);
    if (resolved.has_value()) {
      repaired_path = QString::fromStdU16String(resolved->artifact.path.u16string());
      EXPECT_TRUE(resolved->manifest.requested_topic_messages.contains("/s1"));
    }
    return resolved.has_value();
  })) << "the provider fallback must republish the request into the cache";
  ASSERT_EQ(app_session_->catalogModel().datasets().size(), 1u);
  const PJ::DatasetId imported = app_session_->catalogModel().datasets().front().first;
  ASSERT_TRUE(
      pj_app_test::pumpUntil([&]() { return app_session_->sessionManager().sourceRecord(imported) != nullptr; }));
  const PJ::SourceRecord* record = app_session_->sessionManager().sourceRecord(imported);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->provider_id, provider);
  EXPECT_EQ(record->descriptor_json, descriptor) << "provenance must be preserved byte-exact on the fallback dataset";
  EXPECT_TRUE(PJ::layout_xml::isSamePath(app_session_->sessionManager().datasetSourcePath(imported), repaired_path))
      << "the published artifact must become the dataset's saveable backing file";

  // --- Run 2: same layout, dataset live -> already loaded, no duplicate. ---
  {
    QDomDocument doc = makeLayoutDoc({SourceSpec{u"/saved/cached.mcap"_s, descriptor, provider}});
    PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
    EXPECT_EQ(batch.start(), StartResult::kNoAsyncWork);
    ASSERT_EQ(batch.result().sources.size(), 1);
    EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedAlreadyLoaded);
  }
  batch_.reset();
  EXPECT_EQ(app_session_->catalogModel().datasets().size(), 1u) << "a re-run must not duplicate the dataset";
  EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"s1"}));

  // --- Run 3: dataset removed -> the repaired artifact restores OFFLINE. ---
  app_session_->sessionManager().removeDataset(imported);
  app_session_->catalogModel().rebuildFromDatastore();
  g_static_mcap_fail.store(false);
  {
    QDomDocument doc = makeLayoutDoc({SourceSpec{u"/saved/cached.mcap"_s, descriptor, provider}});
    PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
    EXPECT_EQ(fileInfoFilenames(doc), (QStringList{repaired_path}));
    ASSERT_TRUE(runToFinish(batch));
    ASSERT_EQ(batch.result().sources.size(), 1);
    EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kResolvedCacheHit)
        << batch.result().sources[0].message.toStdString();
  }
  EXPECT_TRUE(sourceLoaded(repaired_path));
  EXPECT_EQ(app_session_->catalogModel().datasets().size(), 1u);
  EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"s1"})) << "the offline restore must not start an import job";
}

// T11 (P2): a CONTENDED cache miss (another instance mid-publish for the same
// identity) fails that ONE source with the user-actionable diagnostic and
// never contacts the provider — no retry loop, no duplicate download. An
// ordinary absent miss in the same batch keeps today's provider fallback.
TEST_F(LayoutImportBatchTest, ContendedCacheMissFailsWithoutContactingTheProvider) {
  const QString provider = QString::fromUtf8(kProviderId);
  const QString contended_descriptor = uR"({"kind":"contended","request":{},"v":1})"_s;
  publishArtifact(provider, contended_descriptor);
  // A second store handle holds the identity's exclusive lock and has a fresh
  // partial in flight, exactly as another process mid-publish would.
  PJ::SourceCacheStore other(std::filesystem::path(cache_dir_.path().toStdU16String()));
  auto contention =
      other.beginPublish(PJ::sourceCacheIdentity(provider.toStdString(), contended_descriptor.toStdString()));
  ASSERT_TRUE(contention.has_value());
  std::ofstream(contention->partialPath(), std::ios::binary) << "in-flight bytes";

  const QString absent_descriptor = makeDescriptor(u"s2"_s, u"trusted"_s, data_dir_.filePath(u"missing.mock"_s));
  QDomDocument doc = makeLayoutDoc({
      SourceSpec{u"/saved/contended.mcap"_s, contended_descriptor, provider},
      SourceSpec{u"/saved/absent.mock"_s, absent_descriptor, provider},
  });
  PJ::LayoutImportBatch& batch = prepareBatch(doc, /*interactive=*/false);
  ASSERT_TRUE(runToFinish(batch));
  contention->abort();

  ASSERT_EQ(batch.result().sources.size(), 2);
  EXPECT_EQ(batch.result().sources[0].outcome, Outcome::kFailed);
  EXPECT_TRUE(batch.result().sources[0].message.contains(u"another PlotJuggler instance"_s))
      << batch.result().sources[0].message.toStdString();
  EXPECT_GE(diagnosticCount("layout-import-cache-contended"), 1);
  EXPECT_EQ(batch.result().sources[1].outcome, Outcome::kResolvedImported)
      << "an absent miss must keep the provider fallback";
  EXPECT_EQ(g_log.snapshot(), (std::vector<std::string>{"s2"}))
      << "the contended source must never start an import job";
}

}  // namespace

PJ_APP_TEST_MAIN("layout_import_batch_test")
