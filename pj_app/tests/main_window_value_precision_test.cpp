// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A float precision chosen in Preferences reaches the plots' tracker value box as soon as OK closes the dialog,
// without waiting for the cursor to move.

#include <gtest/gtest.h>

#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <memory>

#include "MainWindow.h"
#include "PreferencesDialog.h"
#include "dataset_test_helpers.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/AppSettings.h"
#include "pj_widgets/IntScrubber.h"

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowValuePrecisionTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }
};

}  // namespace PJ

namespace {

TEST(MainWindowValuePrecisionTest, OkInPreferencesRedrawsTheTrackerWithTheNewPrecision) {
  QSettings().setValue(QString::fromLatin1(PJ::kPrecisionSettingsKey), 3);
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = PJ::MainWindowValuePrecisionTestPeer::session(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app, "precision");
  PJ::DataWriter writer = app.sessionManager().dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset, "/sensor", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  writer.appendScalar(*handle, 0, 1.23456789);
  writer.appendScalar(*handle, 1'000'000'000, 1.23456789);
  ASSERT_FALSE(app.sessionManager().commitChunks(writer.flushAll()).empty());
  app.catalogModel().rebuildFromDatastore();

  PJ::DockWidget* dock = window.findChild<PJ::TabbedPlotWidget*>()->dockerAt(0)->plotAt(0);
  ASSERT_NE(dock, nullptr);
  auto* plot = new PJ::PlotWidget(&app.sessionManager(), &app.catalogModel(), dock);
  dock->setPlotWidget(plot);
  ASSERT_EQ(app.catalogModel().curves().size(), 1U);
  ASSERT_NE(plot->addCurve(app.catalogModel().curves().front().name), nullptr);
  plot->zoomOut(false);
  plot->setTrackerPosition(plot->maxZoomRect().center().x());
  ASSERT_TRUE(plot->trackerValueBoxText().contains(u">1.235</font>"_s)) << plot->trackerValueBoxText().toStdString();

  QTimer::singleShot(0, &window, [&window] {
    auto* dialog = window.findChild<PJ::PreferencesDialog*>();
    ASSERT_NE(dialog, nullptr);
    dialog->findChild<PJ::IntScrubber*>(u"scrubberFloatPrecision"_s)->setValue(5);
    dialog->accept();
  });
  QMetaObject::invokeMethod(&window, "onShowPreferencesDialog");

  EXPECT_TRUE(QTest::qWaitFor([plot] { return plot->trackerValueBoxText().contains(u">1.23457</font>"_s); }, 2000))
      << plot->trackerValueBoxText().toStdString();
  QSettings().remove(QString::fromLatin1(PJ::kPrecisionSettingsKey));
}

}  // namespace
