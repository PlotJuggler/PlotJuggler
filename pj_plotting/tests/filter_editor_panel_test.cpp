// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QPair>
#include <QPen>
#include <QPushButton>
#include <QSplitter>
#include <QString>
#include <QToolButton>
#include <QtGlobal>
#include <chrono>
#include <cmath>
#include <string_view>
#include <vector>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/FilterEditorPanel.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace {

PJ::TopicId addScalarTopic(
    PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name, int count = 2) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle_or.has_value()) << handle_or.error();
  if (!handle_or.has_value()) {
    return 0;
  }
  for (int i = 0; i < count; ++i) {
    writer.appendScalar(*handle_or, static_cast<PJ::Timestamp>(i + 1) * 1'000'000, static_cast<double>(i) - 3.0);
  }
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  return handle_or->topic_id;
}

// Add a scalar topic and return its CurveDescriptor (matched by topic id).
PJ::CurveDescriptor addSource(
    PJ::SessionManager& session, PJ::CatalogModel& catalog, PJ::DatasetId dataset_id, std::string_view name,
    int count = 2) {
  const PJ::TopicId topic_id = addScalarTopic(session, dataset_id, name, count);
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return *descriptor;
    }
  }
  ADD_FAILURE() << "descriptor not found for " << name;
  return {};
}

// Select the transform row whose UserRole id matches `id`.
void selectTransform(QListWidget* transform_list, const QString& id) {
  for (int row = 0; row < transform_list->count(); ++row) {
    if (transform_list->item(row)->data(Qt::UserRole).toString() == id) {
      transform_list->setCurrentRow(row);
      return;
    }
  }
  FAIL() << "transform id not found: " << id.toStdString();
}

QString currentTransformId(QListWidget* transform_list) {
  const QListWidgetItem* item = transform_list->currentItem();
  return item ? item->data(Qt::UserRole).toString() : QString();
}

// Builds a panel over a single source descriptor and returns the label its "Source
// curve" list shows for that source (item 0) — the tests that pin label formatting.
QString firstSourceLabel(PJ::SessionManager& session, PJ::CatalogModel& catalog, const PJ::CurveDescriptor& source) {
  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  EXPECT_NE(series_list, nullptr);
  if (series_list == nullptr || series_list->count() != 1) {
    EXPECT_EQ(series_list ? series_list->count() : 0, 1);
    return {};
  }
  return series_list->item(0)->text();
}

}  // namespace

// Apply path: choosing a filter and clicking Apply materializes a real output
// topic in the datastore AND reports a (source key -> output key) replacement for
// the host to swap in place. NOTHING extra is materialized before Apply.
TEST(FilterEditorPanelTest, ApplyMaterializesOutputAndReportsReplacement) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto source = addSource(session, catalog, *dataset, "/imu/accel");

  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* apply_btn = panel.findChild<QPushButton*>("save_btn");
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(apply_btn, nullptr);

  // Constructing the panel + showing the preview must not materialize anything.
  EXPECT_EQ(catalog.curves().size(), 1U);

  selectTransform(transform_list, u"absolute"_s);
  ASSERT_GT(series_list->count(), 0);
  series_list->item(0)->setSelected(true);

  QList<QPair<QString, QString>> reported;
  int applied_count = 0;
  QObject::connect(&panel, &PJ::FilterEditorPanel::applied, &panel, [&](QList<QPair<QString, QString>> replacements) {
    reported = replacements;
    ++applied_count;
  });

  apply_btn->click();

  ASSERT_EQ(applied_count, 1);
  ASSERT_EQ(reported.size(), 1);
  EXPECT_EQ(reported[0].first, source.name);  // source key replaced

  const auto curves_after = catalog.curves();
  EXPECT_EQ(curves_after.size(), 2U);
  EXPECT_TRUE(catalog.curveDescriptor(reported[0].second).has_value());
}

// Bug: configuring several sources one at a time (per-source memory) and clicking
// Apply must materialize EVERY configured source, not just the currently-selected
// one. The untouched source stays "No Transform" and is left alone.
TEST(FilterEditorPanelTest, AppliesEverySourceWithItsOwnConfiguredTransform) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto s0 = addSource(session, catalog, *dataset, "/angular_velocity/x");
  const auto s1 = addSource(session, catalog, *dataset, "/angular_velocity/y");
  const auto s2 = addSource(session, catalog, *dataset, "/angular_velocity/z");

  PJ::FilterEditorPanel panel(&session, &catalog, {s0, s1, s2}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* apply_btn = panel.findChild<QPushButton*>("save_btn");
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(apply_btn, nullptr);

  // Configure s0 and s1 individually; leave s2 on "No Transform".
  series_list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
  selectTransform(transform_list, u"absolute"_s);
  series_list->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
  selectTransform(transform_list, u"scale"_s);
  series_list->setCurrentRow(2, QItemSelectionModel::ClearAndSelect);

  QList<QPair<QString, QString>> reported;
  int applied_count = 0;
  QObject::connect(&panel, &PJ::FilterEditorPanel::applied, &panel, [&](QList<QPair<QString, QString>> r) {
    reported = r;
    ++applied_count;
  });

  apply_btn->click();

  ASSERT_EQ(applied_count, 1);
  ASSERT_EQ(reported.size(), 2);
  const QStringList replaced{reported[0].first, reported[1].first};
  EXPECT_TRUE(replaced.contains(s0.name));
  EXPECT_TRUE(replaced.contains(s1.name));
  EXPECT_FALSE(replaced.contains(s2.name));
  EXPECT_EQ(catalog.curves().size(), 5U);  // 3 sources + 2 outputs
}

// Bug: "Apply to all" stamps the visible transform onto every source's memory, so
// the subsequent Apply must materialize all of them (not just the selected one).
TEST(FilterEditorPanelTest, ApplyAfterApplyToAllMaterializesEverySource) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto s0 = addSource(session, catalog, *dataset, "/angular_velocity/x");
  const auto s1 = addSource(session, catalog, *dataset, "/angular_velocity/y");
  const auto s2 = addSource(session, catalog, *dataset, "/angular_velocity/z");

  PJ::FilterEditorPanel panel(&session, &catalog, {s0, s1, s2}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* apply_all = panel.findChild<QToolButton*>("filter_apply_all_btn");
  auto* apply_btn = panel.findChild<QPushButton*>("save_btn");
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(apply_all, nullptr);
  ASSERT_NE(apply_btn, nullptr);

  selectTransform(transform_list, u"scale"_s);
  apply_all->click();

  QList<QPair<QString, QString>> reported;
  QObject::connect(
      &panel, &PJ::FilterEditorPanel::applied, &panel, [&](QList<QPair<QString, QString>> r) { reported = r; });
  apply_btn->click();

  EXPECT_EQ(reported.size(), 3);
  EXPECT_EQ(catalog.curves().size(), 6U);  // 3 sources + 3 outputs
}

// Bug: a materialized filter output is a single-column topic whose column is the
// generic "value" (derived_engine.cpp). The source list must label it by its topic
// name (the alias), not the meaningless "value" column.
TEST(FilterEditorPanelTest, SourceListLabelsValueColumnByTopicName) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::CurveDescriptor output{};
  output.name = u"dataset:1/topic:7/column:0"_s;
  output.topic_name = u"/angular_velocity/y[Absolute]"_s;
  output.field_name = u"value"_s;
  output.field_path = u"value"_s;

  EXPECT_EQ(firstSourceLabel(session, catalog, output), u"/angular_velocity/y[Absolute]"_s);
}

// A real message field is labelled by its FULL "topic/field" path, not just the
// field: "/twist/twist/linear/x" alone is ambiguous across topics (odom vs cmd_vel).
TEST(FilterEditorPanelTest, SourceListPrependsTopicToFieldName) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::CurveDescriptor source{};
  source.name = u"dataset:1/topic:3/column:5"_s;
  source.topic_name = u"odom"_s;
  source.field_name = u"/twist/twist/linear/x"_s;
  source.field_path = u"/twist/twist/linear/x"_s;

  EXPECT_EQ(firstSourceLabel(session, catalog, source), u"odom/twist/twist/linear/x"_s);
}

// Reopening the editor on an already-filtered curve: (A) the source list labels it
// by its ORIGINAL input series (not the output alias), and (B) selecting "No
// Transform" keeps Apply enabled and, on Apply, removes the filter — reporting the
// (output -> input) swap so the host reverts the plot curve.
TEST(FilterEditorPanelTest, ReopenOnFilterOutputLabelsInputAndRemovesOnNoTransform) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto src = addSource(session, catalog, *dataset, "/angular_velocity/x");

  // Materialize a filter output (records a recipe the editor can re-open on).
  const auto handle = session.dataProcessorService().applyFilter(
      src.topic_id, src.dataset_id, "absolute", "/angular_velocity/x[Absolute]", src.column_index);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  catalog.rebuildFromDatastore();
  ASSERT_EQ(catalog.curves().size(), 2U);  // input + output

  PJ::CurveDescriptor output{};
  for (const auto& curve : catalog.curves()) {
    if (const auto d = catalog.curveDescriptor(curve.name); d && d->topic_id == handle->output_topic_id) {
      output = *d;
    }
  }
  ASSERT_NE(output.topic_id, 0U);

  // The label the editor shows for the raw input (cross-check target).
  PJ::FilterEditorPanel input_panel(&session, &catalog, {src}, {});
  const QString input_label = input_panel.findChild<QListWidget*>("series_list")->item(0)->text();

  PJ::FilterEditorPanel panel(&session, &catalog, {output}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* apply_btn = panel.findChild<QPushButton*>("save_btn");
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(apply_btn, nullptr);

  // (A) The output source is labelled by its original input, not "x[Absolute]".
  ASSERT_EQ(series_list->count(), 1);
  EXPECT_EQ(series_list->item(0)->text(), input_label);
  EXPECT_FALSE(series_list->item(0)->text().contains("[Absolute]"_L1));

  // Select it -> EDIT mode: the recipe's transform shows and Apply is enabled.
  series_list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
  EXPECT_EQ(currentTransformId(transform_list), u"absolute"_s);
  EXPECT_TRUE(apply_btn->isEnabled());

  // (B) Switch to "No Transform" -> Apply stays enabled (was the bug).
  selectTransform(transform_list, u"none"_s);
  EXPECT_TRUE(apply_btn->isEnabled());

  QList<QPair<QString, QString>> reported;
  int applied_count = 0;
  QObject::connect(&panel, &PJ::FilterEditorPanel::applied, &panel, [&](QList<QPair<QString, QString>> r) {
    reported = r;
    ++applied_count;
  });
  apply_btn->click();

  // Apply removed the filter and reported (output -> input) for the host to revert.
  EXPECT_EQ(applied_count, 1);
  ASSERT_EQ(reported.size(), 1);
  EXPECT_TRUE(reported[0].second.contains(QString::number(src.topic_id)));  // reverts to the input topic
  catalog.rebuildFromDatastore();
  EXPECT_EQ(catalog.curves().size(), 1U);  // output topic gone, only the input remains
}

// EDIT-mode Update changes the output topic's DATA under existing curve adapters
// (same key). Without a data-changed notification the plot keeps its cached samples
// and only refreshes on a zoom; Apply must fire samplesIngested for the output topic
// so plots drop their cache and re-read.
TEST(FilterEditorPanelTest, EditModeUpdateNotifiesOutputTopicForRepaint) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto src = addSource(session, catalog, *dataset, "/angular_velocity/x");
  const auto handle = session.dataProcessorService().applyFilter(
      src.topic_id, src.dataset_id, "absolute", "/angular_velocity/x[Absolute]", src.column_index);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  catalog.rebuildFromDatastore();

  PJ::CurveDescriptor output{};
  for (const auto& curve : catalog.curves()) {
    if (const auto d = catalog.curveDescriptor(curve.name); d && d->topic_id == handle->output_topic_id) {
      output = *d;
    }
  }
  ASSERT_NE(output.topic_id, 0U);

  PJ::FilterEditorPanel panel(&session, &catalog, {output}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* apply_btn = panel.findChild<QPushButton*>("save_btn");
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(apply_btn, nullptr);

  // EDIT mode is auto-entered on open; switch to a different transform and Update.
  selectTransform(transform_list, u"scale"_s);
  ASSERT_TRUE(apply_btn->isEnabled());

  int notify_count = 0;
  QVector<PJ::TopicId> notified_ids;
  QObject::connect(
      &session, &PJ::SessionManager::samplesIngested, &session, [&](const QVector<PJ::TopicId>& ids, bool) {
        ++notify_count;
        notified_ids += ids;
      });
  apply_btn->click();

  ASSERT_GE(notify_count, 1);  // the in-place update notified consumers
  EXPECT_TRUE(notified_ids.contains(handle->output_topic_id));
}

// The preview adopts the display settings the host pushes — mirroring the plot the
// editor was opened on (grid + curve style + line width), applied plot-level so it
// matches the real plot faithfully.
TEST(FilterEditorPanelTest, PreviewHonorsPushedDisplaySettings) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto source = addSource(session, catalog, *dataset, "/imu/accel");

  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* preview = panel.findChild<PJ::PlotWidget*>();
  ASSERT_NE(preview, nullptr);
  EXPECT_FALSE(preview->gridVisible());  // default: no grid until the host pushes settings

  panel.setPreviewDisplay(/*grid=*/true, PJ::PlotWidgetBase::kDots, PJ::LineWidth::kPoints20);
  EXPECT_TRUE(preview->gridVisible());
  EXPECT_EQ(preview->defaultCurveStyle(), PJ::PlotWidgetBase::kDots);
  EXPECT_EQ(preview->lineWidth(), PJ::LineWidth::kPoints20);

  panel.setPreviewDisplay(/*grid=*/false, PJ::PlotWidgetBase::kLines, PJ::LineWidth::kPoints10);
  EXPECT_FALSE(preview->gridVisible());
  EXPECT_EQ(preview->defaultCurveStyle(), PJ::PlotWidgetBase::kLines);
  EXPECT_EQ(preview->lineWidth(), PJ::LineWidth::kPoints10);
}

// Regression (Codex I3): a reactive setPreviewDisplay() — the host pushing a new
// style/width while the panel is open — must NOT wipe the ghost's dashed "before"
// pen. applyPreviewDisplay() re-pens every curve plot-level, so applyGhostPens() must
// re-dash the ghost afterwards.
TEST(FilterEditorPanelTest, ReactiveDisplayChangeKeepsGhostDashed) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto source = addSource(session, catalog, *dataset, "/imu/accel");

  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* preview = panel.findChild<PJ::PlotWidget*>();
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(preview, nullptr);

  selectTransform(transform_list, u"scale"_s);  // active transform -> dashed ghost
  ASSERT_GT(series_list->count(), 0);
  series_list->item(0)->setSelected(true);

  // Drive the debounced (150 ms single-shot) preview timer so the ghost is built + dashed.
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < 350) {
    QApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  ASSERT_EQ(preview->curveList().size(), 2U);  // ghost + filtered

  const QString kFilteredPrefix = u"__filter_preview__"_s;
  const auto ghostPenStyle = [&]() -> Qt::PenStyle {
    for (const auto& info : preview->curveList()) {
      if (info.curve != nullptr && !info.source_name.startsWith(kFilteredPrefix)) {
        return info.curve->pen().style();
      }
    }
    return Qt::NoPen;
  };
  ASSERT_EQ(ghostPenStyle(), Qt::DashLine);  // active transform -> dashed ghost

  // Reactive host push (e.g. a global curve-width click while the panel is open).
  panel.setPreviewDisplay(/*grid=*/false, PJ::PlotWidgetBase::kLines, PJ::LineWidth::kPoints30);
  EXPECT_EQ(ghostPenStyle(), Qt::DashLine);  // dash survived the plot-level restyle
}

// Bug: a multi-select edit must PREVIEW every selected series, not just the first
// one — Apply already updates all of them, so a preview that shows only the primary
// is misleading. With two sources selected and an active transform, the preview
// holds two input ghosts + two filtered curves (four total), not a single pair.
TEST(FilterEditorPanelTest, PreviewShowsEverySelectedSource) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto sx = addSource(session, catalog, *dataset, "/angular_velocity/x");
  const auto sy = addSource(session, catalog, *dataset, "/angular_velocity/y");

  PJ::FilterEditorPanel panel(&session, &catalog, {sx, sy}, {});
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* preview = panel.findChild<PJ::PlotWidget*>();
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(preview, nullptr);

  selectTransform(transform_list, u"scale"_s);  // active transform
  series_list->selectAll();                     // BOTH sources selected

  // Drive the debounced (150 ms single-shot) preview timer by spinning the event
  // loop across enough real time for it to fire.
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < 350) {
    QApplication::processEvents(QEventLoop::AllEvents, 50);
  }

  EXPECT_EQ(preview->curveList().size(), 4U);
}

// Regression: the filtered preview curve must share the GHOST's display offset — the
// "Use time offset" (t0) per-dataset shift — and must re-align when the toggle flips the
// frame. Two bugs this guards: (1) previewFilteredPoints used offsetOf(time_domain),
// which omits the per-dataset shift the ghost's adapter applies via displayOffset(); and
// (2) nothing recomputed the filtered curve on displayOffsetChanged, so a toggle stranded
// it in the old frame. Asserted as: filtered x == ghost x in BOTH t0 states.
TEST(FilterEditorPanelTest, PreviewFilteredCurveSharesGhostOffsetAcrossT0Toggle) {
  PJ::SessionManager session;  // use_time_offset_ defaults to false
  PJ::CatalogModel catalog(&session);
  // Own TimeDomain (mirrors FileLoader): "Use time offset" writes the align-starts
  // shift to the domain, so the dataset must not be on the default (id 0) domain.
  auto domain = session.dataEngine().createTimeDomain("drive");
  ASSERT_TRUE(domain.has_value()) << domain.error();
  auto dataset =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto source = addSource(session, catalog, *dataset, "/x");  // first sample at t=1e6 ns

  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* preview = panel.findChild<PJ::PlotWidget*>();
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(preview, nullptr);

  selectTransform(transform_list, u"absolute"_s);
  ASSERT_GT(series_list->count(), 0);
  series_list->item(0)->setSelected(true);

  // Drive the debounced (150 ms single-shot) preview timer.
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < 350) {
    QApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  ASSERT_EQ(preview->curveList().size(), 2U);  // ghost + filtered

  const QString kFilteredPrefix = u"__filter_preview__"_s;
  const auto firstX = [&](bool filtered) -> double {
    for (const auto& info : preview->curveList()) {
      if (info.source_name.startsWith(kFilteredPrefix) == filtered) {
        return info.curve->data()->sample(0).x();
      }
    }
    ADD_FAILURE() << (filtered ? "filtered" : "ghost") << " preview curve missing";
    return 0.0;
  };

  // t0 OFF (offset 0): ghost and filtered share the same frame.
  const double ghost_off = firstX(false);
  const double filtered_off = firstX(true);
  EXPECT_NEAR(filtered_off, ghost_off, 1e-9);

  // Flip the frame. setUseTimeOffset emits displayOffsetChanged synchronously: the ghost
  // re-reads (PlotWidget's own handler) and the panel recomputes the filtered curve (our
  // new connect).
  session.setUseTimeOffset(true);
  QApplication::processEvents(QEventLoop::AllEvents, 50);

  const double ghost_on = firstX(false);
  const double filtered_on = firstX(true);
  EXPECT_NEAR(filtered_on, ghost_on, 1e-9);                // still aligned with the ghost after the toggle
  EXPECT_GT(std::abs(filtered_off - filtered_on), 1e-12);  // filtered actually tracked the per-dataset shift
}

// [j] In a multi-source edit, each source's PREVIEW must use ITS OWN configured filter (matching
// what Apply materializes), not the currently-visible filter. Configure A->scale (default identity)
// and B->absolute, then multi-select both: A's filtered preview must show scale(A) (the negative
// preserved, -3), not absolute(A) (which would flip it to +3, the visible filter).
TEST(FilterEditorPanelTest, PreviewUsesPerSourceConfigInMultiSelect) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto a = addSource(session, catalog, *dataset, "/a");  // values {-3, -2}
  const auto b = addSource(session, catalog, *dataset, "/b");  // values {-3, -2}

  PJ::FilterEditorPanel panel(&session, &catalog, {a, b}, {});
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* preview = panel.findChild<PJ::PlotWidget*>();
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(preview, nullptr);
  ASSERT_EQ(series_list->count(), 2);

  // Configure A -> scale (default params = identity), then B -> absolute (B ends active/visible).
  series_list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
  selectTransform(transform_list, u"scale"_s);
  series_list->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
  selectTransform(transform_list, u"absolute"_s);

  // Multi-select BOTH so the preview shows every series Apply will touch.
  series_list->item(0)->setSelected(true);
  series_list->item(1)->setSelected(true);

  // Drive the debounced (150 ms single-shot) preview timer.
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < 350) {
    QApplication::processEvents(QEventLoop::AllEvents, 50);
  }

  // A's filtered preview curve is keyed "__filter_preview__" + A's input name.
  const QString a_filtered_key = u"__filter_preview__"_s + a.name;
  double a_first_y = 999.0;
  for (const auto& info : preview->curveList()) {
    if (info.source_name == a_filtered_key) {
      ASSERT_GT(info.curve->data()->size(), 0U);
      a_first_y = info.curve->data()->sample(0).y();
      break;
    }
  }
  EXPECT_NEAR(a_first_y, -3.0, 1e-9);  // scale(identity) preserves the negative; absolute would give +3
}

// Bug (Codex finding 1+2): when the editor is opened on already-filtered curves,
// Apply must RE-EDIT each existing filter IN PLACE (consistent with the preview,
// which filters the original input) instead of only updating the single selected
// recipe or chaining a new filter onto the output. Apply-to-all + Apply must update
// EVERY filter output, with no new chained topics.
TEST(FilterEditorPanelTest, ApplyToAllReEditsEveryFilterOutputInPlace) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto sx = addSource(session, catalog, *dataset, "/angular_velocity/x");
  const auto sy = addSource(session, catalog, *dataset, "/angular_velocity/y");

  // Materialize a "scale" filter on each source -> two filter-output curves.
  const auto hx =
      session.dataProcessorService().applyFilter(sx.topic_id, sx.dataset_id, "scale", "/x[Scale]", sx.column_index);
  const auto hy =
      session.dataProcessorService().applyFilter(sy.topic_id, sy.dataset_id, "scale", "/y[Scale]", sy.column_index);
  ASSERT_TRUE(hx.has_value()) << hx.error();
  ASSERT_TRUE(hy.has_value()) << hy.error();
  catalog.rebuildFromDatastore();

  PJ::CurveDescriptor ox{};
  PJ::CurveDescriptor oy{};
  for (const auto& curve : catalog.curves()) {
    if (const auto d = catalog.curveDescriptor(curve.name); d) {
      if (d->topic_id == hx->output_topic_id) {
        ox = *d;
      }
      if (d->topic_id == hy->output_topic_id) {
        oy = *d;
      }
    }
  }
  ASSERT_NE(ox.topic_id, 0U);
  ASSERT_NE(oy.topic_id, 0U);
  const std::size_t curves_before = catalog.curves().size();  // 2 inputs + 2 outputs

  PJ::FilterEditorPanel panel(&session, &catalog, {ox, oy}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* apply_all = panel.findChild<QToolButton*>("filter_apply_all_btn");
  auto* apply_btn = panel.findChild<QPushButton*>("save_btn");
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(apply_all, nullptr);
  ASSERT_NE(apply_btn, nullptr);

  selectTransform(transform_list, u"absolute"_s);  // change the edited filter
  apply_all->click();                              // stamp Absolute onto both
  apply_btn->click();

  catalog.rebuildFromDatastore();
  EXPECT_EQ(catalog.curves().size(), curves_before);  // re-edited in place, no new topics
  const auto* rx = session.dataProcessorService().filterConfig(hx->output_topic_id);
  const auto* ry = session.dataProcessorService().filterConfig(hy->output_topic_id);
  ASSERT_NE(rx, nullptr);
  ASSERT_NE(ry, nullptr);
  EXPECT_EQ(rx->processor_id, "absolute");  // both outputs updated, not just the selected one
  EXPECT_EQ(ry->processor_id, "absolute");
}

// The preview ghost is drawn in the source curve's actual plot colour (passed by the
// host keyed by the source's catalog key), not a palette fallback. Regression guard
// for the "preview is always blue" bug (the host had keyed colours by title).
TEST(FilterEditorPanelTest, PreviewGhostUsesProvidedSourceColor) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto source = addSource(session, catalog, *dataset, "/x");

  const QColor wanted(200, 30, 40);  // distinctive, not a palette colour
  QHash<QString, QColor> colors;
  colors.insert(source.name, wanted);  // keyed by the source's catalog key (== descriptor.name)
  PJ::FilterEditorPanel panel(&session, &catalog, {source}, colors);
  auto* preview = panel.findChild<PJ::PlotWidget*>();
  ASSERT_NE(preview, nullptr);

  QwtPlotCurve* ghost = nullptr;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline && ghost == nullptr) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    for (const auto& info : preview->curveList()) {
      if (info.source_name == source.name) {
        ghost = info.curve;
      }
    }
  }
  ASSERT_NE(ghost, nullptr);
  EXPECT_EQ(ghost->pen().color(), wanted);  // the provided colour, not palette blue
}

// Bug (3-reviewer consensus): removing a filter output set to "No Transform" must
// NOT early-return and drop the OTHER sources configured in the same Apply.
TEST(FilterEditorPanelTest, RemovalDoesNotDropOtherConfiguredSources) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto sx = addSource(session, catalog, *dataset, "/x");  // plain source
  const auto sy = addSource(session, catalog, *dataset, "/y");

  // Make /y into a filter output.
  const auto hy =
      session.dataProcessorService().applyFilter(sy.topic_id, sy.dataset_id, "scale", "/y[Scale]", sy.column_index);
  ASSERT_TRUE(hy.has_value()) << hy.error();
  catalog.rebuildFromDatastore();
  PJ::CurveDescriptor oy{};
  for (const auto& curve : catalog.curves()) {
    if (const auto d = catalog.curveDescriptor(curve.name); d && d->topic_id == hy->output_topic_id) {
      oy = *d;
    }
  }
  ASSERT_NE(oy.topic_id, 0U);

  PJ::FilterEditorPanel panel(&session, &catalog, {sx, oy}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* apply_btn = panel.findChild<QPushButton*>("save_btn");
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(apply_btn, nullptr);

  // Configure the plain source with Absolute, then switch to the filter output and
  // set it to "No Transform" (a removal).
  series_list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
  selectTransform(transform_list, u"absolute"_s);
  series_list->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
  selectTransform(transform_list, u"none"_s);

  QList<QPair<QString, QString>> reported;
  QObject::connect(
      &panel, &PJ::FilterEditorPanel::applied, &panel, [&](QList<QPair<QString, QString>> r) { reported = r; });
  apply_btn->click();

  // BOTH happened: /y[Scale] removed AND /x filtered.
  EXPECT_EQ(reported.size(), 2);
  EXPECT_EQ(session.dataProcessorService().filterConfig(hy->output_topic_id), nullptr);  // removed
  bool x_filtered = false;
  for (const auto& recipe : session.dataProcessorService().recipes()) {
    if (recipe.input_topic_id == sx.topic_id) {
      x_filtered = true;
    }
  }
  EXPECT_TRUE(x_filtered);  // the plain source's filter was NOT dropped by the removal
}

// A freshly-opened source defaults to "-- No Transform --", not the first filter.
TEST(FilterEditorPanelTest, DefaultsToNoTransform) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto source = addSource(session, catalog, *dataset, "/x");

  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  ASSERT_NE(transform_list, nullptr);
  EXPECT_EQ(currentTransformId(transform_list), u"none"_s);
}

// The alias field is editable (not the disabled field it used to be).
TEST(FilterEditorPanelTest, AliasFieldIsEditable) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto source = addSource(session, catalog, *dataset, "/x");

  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* alias = panel.findChild<QLineEdit*>("alias_edit");
  ASSERT_NE(alias, nullptr);
  EXPECT_TRUE(alias->isEnabled());
}

// Preview and controls live in a vertical, user-resizable splitter.
TEST(FilterEditorPanelTest, HasResizablePreviewSplitter) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto source = addSource(session, catalog, *dataset, "/x");

  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* splitter = panel.findChild<QSplitter*>("previewSplitter");
  ASSERT_NE(splitter, nullptr);
  EXPECT_EQ(splitter->orientation(), Qt::Vertical);
  EXPECT_EQ(splitter->count(), 2);  // preview pane + controls pane
}

// Each source remembers its own transform; switching sources does not bleed one
// source's transform onto another (it starts at "No Transform").
TEST(FilterEditorPanelTest, RemembersTransformPerSource) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto s0 = addSource(session, catalog, *dataset, "/angular_velocity/x");
  const auto s1 = addSource(session, catalog, *dataset, "/angular_velocity/y");
  const auto s2 = addSource(session, catalog, *dataset, "/angular_velocity/z");

  PJ::FilterEditorPanel panel(&session, &catalog, {s0, s1, s2}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(series_list, nullptr);
  ASSERT_EQ(series_list->count(), 3);

  // Source 0 starts at No Transform; configure it with Absolute.
  EXPECT_EQ(currentTransformId(transform_list), u"none"_s);
  selectTransform(transform_list, u"absolute"_s);

  // Source 1 must start fresh (No Transform), NOT inherit Absolute. Configure Scale.
  series_list->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
  EXPECT_EQ(currentTransformId(transform_list), u"none"_s);
  selectTransform(transform_list, u"scale"_s);

  // Source 2 also starts fresh.
  series_list->setCurrentRow(2, QItemSelectionModel::ClearAndSelect);
  EXPECT_EQ(currentTransformId(transform_list), u"none"_s);

  // Back to source 0: remembers Absolute. Back to source 1: remembers Scale.
  series_list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
  EXPECT_EQ(currentTransformId(transform_list), u"absolute"_s);
  series_list->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
  EXPECT_EQ(currentTransformId(transform_list), u"scale"_s);
}

// The preview filters the ENTIRE series, not just the first ~2000 samples.
TEST(FilterEditorPanelTest, PreviewCoversFullSeries) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  constexpr int kCount = 2500;  // exceeds the old 2000-sample preview cap
  const auto source = addSource(session, catalog, *dataset, "/x", kCount);

  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* preview = panel.findChild<PJ::PlotWidget*>();
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(preview, nullptr);

  selectTransform(transform_list, u"absolute"_s);
  series_list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);

  // Pump the debounced preview refresh until the filtered curve is populated.
  QwtPlotCurve* filtered = nullptr;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    filtered = nullptr;
    for (const auto& info : preview->curveList()) {
      if (info.source_name.startsWith("__filter_preview__"_L1)) {
        filtered = info.curve;
        break;
      }
    }
    if (filtered != nullptr && filtered->dataSize() >= static_cast<std::size_t>(kCount)) {
      break;
    }
  }
  ASSERT_NE(filtered, nullptr);
  EXPECT_EQ(filtered->dataSize(), static_cast<std::size_t>(kCount));  // whole series, not capped
}

// With No Transform selected, the source is drawn as its plain solid self (not a
// faded dashed "ghost"), and no filtered curve is shown.
TEST(FilterEditorPanelTest, NoTransformShowsPlainSourceNotGhost) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto source = addSource(session, catalog, *dataset, "/x");

  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* preview = panel.findChild<PJ::PlotWidget*>();
  ASSERT_NE(preview, nullptr);

  // Default is No Transform; pump the preview refresh until the source is drawn.
  QwtPlotCurve* ghost = nullptr;
  QwtPlotCurve* filtered = nullptr;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    ghost = nullptr;
    filtered = nullptr;
    for (const auto& info : preview->curveList()) {
      if (info.source_name == source.name) {
        ghost = info.curve;
      } else if (info.source_name.startsWith("__filter_preview__"_L1)) {
        filtered = info.curve;
      }
    }
    if (ghost != nullptr) {
      break;
    }
  }
  ASSERT_NE(ghost, nullptr);
  EXPECT_EQ(ghost->pen().style(), Qt::SolidLine);  // plain, not dashed
  EXPECT_EQ(ghost->pen().color().alpha(), 255);    // full opacity, not faded
  if (filtered != nullptr) {
    EXPECT_FALSE(filtered->isVisible());  // no filtered curve with no transform
  }
}

// The filtered curve's legend label is the alias (output series name), not the
// generic "filtered".
TEST(FilterEditorPanelTest, FilteredCurveLegendUsesAlias) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto source = addSource(session, catalog, *dataset, "/x");

  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* alias = panel.findChild<QLineEdit*>("alias_edit");
  auto* preview = panel.findChild<PJ::PlotWidget*>();
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(alias, nullptr);
  ASSERT_NE(preview, nullptr);

  selectTransform(transform_list, u"absolute"_s);
  series_list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);

  QwtPlotCurve* filtered = nullptr;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    filtered = nullptr;
    for (const auto& info : preview->curveList()) {
      if (info.source_name.startsWith("__filter_preview__"_L1)) {
        filtered = info.curve;
        break;
      }
    }
    if (filtered != nullptr && filtered->dataSize() > 0) {
      break;
    }
  }
  ASSERT_NE(filtered, nullptr);
  const QString alias_text = alias->text();
  ASSERT_FALSE(alias_text.isEmpty());               // auto-alias was filled in
  EXPECT_EQ(filtered->title().text(), alias_text);  // legend uses the alias
  EXPECT_NE(filtered->title().text(), u"filtered"_s);
}

// "Copy into all others" stamps the visible filter onto every source, so each
// series then shows (and would apply) that transform.
TEST(FilterEditorPanelTest, ApplyToAllStampsTransformOntoEverySource) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const auto s0 = addSource(session, catalog, *dataset, "/angular_velocity/x");
  const auto s1 = addSource(session, catalog, *dataset, "/angular_velocity/y");
  const auto s2 = addSource(session, catalog, *dataset, "/angular_velocity/z");

  PJ::FilterEditorPanel panel(&session, &catalog, {s0, s1, s2}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* apply_all = panel.findChild<QToolButton*>("filter_apply_all_btn");
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(apply_all, nullptr);

  // Configure source 0 with Scale, then copy it into all others.
  selectTransform(transform_list, u"scale"_s);
  apply_all->click();

  series_list->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
  EXPECT_EQ(currentTransformId(transform_list), u"scale"_s);
  series_list->setCurrentRow(2, QItemSelectionModel::ClearAndSelect);
  EXPECT_EQ(currentTransformId(transform_list), u"scale"_s);
}

// Changing the transform (not just the source) refits the Y range while AutoZoom
// is on: a Binary Filter on a small-amplitude source outputs values up to 1, and
// the preview must expand to show them instead of clipping at the source's range.
TEST(FilterEditorPanelTest, AutoZoomRefitsOnTransformChange) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  // Small-amplitude source (|y| = 0.1) with positive samples, so Greater(>0) -> 1.
  PJ::TopicId topic_id = 0;
  {
    PJ::DataWriter writer = session.dataEngine().createWriter();
    auto handle = writer.registerScalarSeries(*dataset, "/small", PJ::NumericType::kFloat64);
    ASSERT_TRUE(handle.has_value()) << handle.error();
    for (int i = 0; i < 40; ++i) {
      writer.appendScalar(*handle, static_cast<PJ::Timestamp>(i + 1) * 1'000'000, (i % 2 == 0) ? 0.1 : -0.1);
    }
    ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
    topic_id = handle->topic_id;
  }
  PJ::CurveDescriptor source;
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      source = *descriptor;
    }
  }
  ASSERT_FALSE(source.name.isEmpty());

  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* preview = panel.findChild<PJ::PlotWidget*>();
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(preview, nullptr);

  const auto pump = [&](double until_top, int budget_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      if (preview->maxZoomRect().top() >= until_top) {
        break;
      }
    }
  };

  series_list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
  pump(0.05, 1000);
  EXPECT_LT(preview->maxZoomRect().top(), 0.5);  // No Transform: range hugs the small source

  // Switch to Binary Filter (default Greater > 0): output reaches 1.
  selectTransform(transform_list, u"binary_filter"_s);
  pump(0.9, 2000);
  EXPECT_GT(preview->maxZoomRect().top(), 0.9);  // refit to include the filtered max (1)
}

// During streaming, the filtered "after" curve must TRACK newly ingested samples,
// not freeze at what it showed when the panel opened. The ghost is datastore-backed
// and refreshes on samplesIngested via PlotWidget's own handler; the filtered curve
// is a FilteredCurveAdapter that reports the SAME input topic, so the identical
// handler refreshes it too — no panel-side samplesIngested connection, no timer.
TEST(FilterEditorPanelTest, PreviewFilteredCurveTracksStreamingIngest) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "stream"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  // Keep the writer + handle so we can append more later — exactly how a streaming
  // source grows a topic (append -> flush -> append -> flush).
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(*dataset, "/random", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  for (int i = 0; i < 3; ++i) {
    writer.appendScalar(*handle, static_cast<PJ::Timestamp>(i + 1) * 1'000'000, static_cast<double>(i));
  }
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());

  PJ::CurveDescriptor source;
  for (const auto& curve : catalog.curves()) {
    if (const auto d = catalog.curveDescriptor(curve.name); d && d->topic_id == handle->topic_id) {
      source = *d;
    }
  }
  ASSERT_FALSE(source.name.isEmpty());

  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* transform_list = panel.findChild<QListWidget*>("transform_list");
  auto* series_list = panel.findChild<QListWidget*>("series_list");
  auto* preview = panel.findChild<PJ::PlotWidget*>();
  ASSERT_NE(transform_list, nullptr);
  ASSERT_NE(series_list, nullptr);
  ASSERT_NE(preview, nullptr);

  selectTransform(transform_list, u"absolute"_s);
  series_list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);

  const QString kFilteredPrefix = u"__filter_preview__"_s;
  const auto filtered_size = [&]() -> std::size_t {
    for (const auto& info : preview->curveList()) {
      if (info.source_name.startsWith(kFilteredPrefix)) {
        return info.curve->data()->size();
      }
    }
    return 0U;
  };
  const auto pump_until = [&](std::size_t target, int budget_ms) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < budget_ms && filtered_size() < target) {
      QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
  };

  // The initial debounced refresh mirrors the 3 starting samples.
  pump_until(3U, 500);
  ASSERT_EQ(filtered_size(), 3U);

  // Stream three more samples into the SAME topic and commit (emits samplesIngested).
  for (int i = 3; i < 6; ++i) {
    writer.appendScalar(*handle, static_cast<PJ::Timestamp>(i + 1) * 1'000'000, static_cast<double>(i));
  }
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());

  // No user interaction: PlotWidget's samplesIngested handler invalidates the
  // filtered adapter (it reports this input topic), so it recomputes to 6.
  pump_until(6U, 800);
  EXPECT_EQ(filtered_size(), 6U);
}
