// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The Custom Series panel lists derived series, but only DataProcessorService knows
// which catalog topics are transform outputs. These tests pin that the panel is
// DERIVED from the service rather than accumulated from announcements, on the two
// paths that announce nothing: a layout restore, which rebuilds the whole processor
// graph with no plugin involved, and a withdrawal through pj.data_processors.v1,
// which an announce-only pass has nothing to say about.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QTemporaryDir>
#include <string>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/SessionManager.h"
#include "ui/CurveListPanel.h"

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowCustomSeriesTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }

  [[nodiscard]] static bool restoreDataProcessors(MainWindow& window, const QDomElement& root) {
    return window.restoreDataProcessors(root);
  }

  // The production hook every recipe-set change goes through.
  static void syncCustomSeriesPanel(MainWindow& window) {
    window.syncCustomSeriesPanel();
  }
};

}  // namespace PJ

namespace {

constexpr const char* kTopic = "sensor";
constexpr const char* kInputSeries = "sensor/value";
constexpr const char* kOutputTopic = "sensor/negated";
constexpr const char* kTransformKey = "pluginA/negate";

// A self-contained Luau transform class; `id` must match the transform's user id.
constexpr const char* kNegate = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";

// A layout carrying one plugin transform, shaped exactly as
// MainWindow::saveDataProcessors writes it.
QDomDocument layoutWithTransform(PJ::DatasetId dataset_id, const QString& source_name) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  QDomElement processors = doc.createElement(u"data_processors"_s);
  root.appendChild(processors);

  QDomElement transform = doc.createElement(u"transform"_s);
  transform.setAttribute(u"owner_plugin"_s, u"pluginA"_s);
  transform.setAttribute(u"id"_s, u"negate"_s);
  transform.setAttribute(u"backend"_s, u"luau"_s);
  transform.setAttribute(u"api_version"_s, u"1"_s);
  processors.appendChild(transform);

  QDomElement input = doc.createElement(u"input"_s);
  input.setAttribute(u"name"_s, QString::fromLatin1(kInputSeries));
  input.setAttribute(u"dataset_id"_s, QString::number(dataset_id));
  input.setAttribute(u"dataset_source"_s, source_name);
  input.setAttribute(u"topic"_s, QString::fromLatin1(kTopic));
  input.setAttribute(u"field"_s, u"value"_s);
  input.setAttribute(u"column"_s, u"0"_s);
  transform.appendChild(input);

  QDomElement output = doc.createElement(u"output"_s);
  output.setAttribute(u"name"_s, QString::fromLatin1(kOutputTopic));
  transform.appendChild(output);

  QDomElement params = doc.createElement(u"params"_s);
  params.appendChild(doc.createCDATASection(u"{}"_s));
  transform.appendChild(params);
  QDomElement script = doc.createElement(u"script"_s);
  script.appendChild(doc.createCDATASection(QString::fromLatin1(kNegate)));
  transform.appendChild(script);
  return doc;
}

// One MainWindow with a dataset, a scalar topic, and the layout above restored.
struct RestoredSession {
  QTemporaryDir extensions_dir;
  PJ::MainWindow window{extensions_dir.path()};
  PJ::CurveListPanel* panel = window.findChild<PJ::CurveListPanel*>(u"curveListPanel"_s);

  [[nodiscard]] PJ::AppSession& app() {
    return PJ::MainWindowCustomSeriesTestPeer::session(window);
  }

  [[nodiscard]] PJ::DataProcessorService& processors() {
    return app().sessionManager().dataProcessorService();
  }

  [[nodiscard]] bool setUp() {
    if (!extensions_dir.isValid() || panel == nullptr) {
      return false;
    }
    const PJ::DatasetId dataset = pj_test::createDataset(app(), "run.csv");
    if (dataset == 0U || pj_test::addScalarTopic(app(), dataset, kTopic) == 0U) {
      return false;
    }
    const QDomDocument doc = layoutWithTransform(dataset, u"run.csv"_s);
    return PJ::MainWindowCustomSeriesTestPeer::restoreDataProcessors(window, doc.documentElement());
  }
};

// The restore replays the transform and materializes its output, but no plugin is
// loaded to announce it. Nothing else would ever put the row on screen, so the
// user's derived series were simply missing after reopening their own layout.
TEST(MainWindowCustomSeriesTest, RestoredTransformIsListed) {
  RestoredSession fx;
  ASSERT_TRUE(fx.setUp());

  EXPECT_EQ(fx.panel->customSeriesNames(), QStringList{QString::fromLatin1(kOutputTopic)});
}

// A withdrawal through the ABI: the transform is gone from the service, so the row
// must go with it. It used to survive, and the panel's own Delete could not clear
// it either — that action removes a row only after removing the transform behind
// it, which by then no longer exists.
TEST(MainWindowCustomSeriesTest, WithdrawnTransformLeavesNoRow) {
  RestoredSession fx;
  ASSERT_TRUE(fx.setUp());
  ASSERT_EQ(fx.panel->customSeriesNames().size(), 1);

  ASSERT_TRUE(fx.processors().removeTransform(kTransformKey).has_value());
  PJ::MainWindowCustomSeriesTestPeer::syncCustomSeriesPanel(fx.window);

  EXPECT_TRUE(fx.panel->customSeriesNames().isEmpty());
}

// The same withdrawal seen only through the catalog: retiring the output makes the
// rebuild emit itemsRemoved, and the panel prunes on that alone. This is the net
// under the sync above — it holds even when nobody re-derives the set.
TEST(MainWindowCustomSeriesTest, CatalogRemovalAlonePrunesTheRow) {
  RestoredSession fx;
  ASSERT_TRUE(fx.setUp());
  ASSERT_EQ(fx.panel->customSeriesNames().size(), 1);

  ASSERT_TRUE(fx.processors().removeTransform(kTransformKey).has_value());
  fx.app().catalogModel().rebuildFromDatastore();

  EXPECT_TRUE(fx.panel->customSeriesNames().isEmpty());
}

// Clear All empties the catalog, which reports `cleared()` instead of a key list.
TEST(MainWindowCustomSeriesTest, ClearedCatalogEmptiesCustomSeries) {
  RestoredSession fx;
  ASSERT_TRUE(fx.setUp());
  ASSERT_EQ(fx.panel->customSeriesNames().size(), 1);

  fx.app().catalogModel().clearAll();

  EXPECT_TRUE(fx.panel->customSeriesNames().isEmpty());
}

}  // namespace
