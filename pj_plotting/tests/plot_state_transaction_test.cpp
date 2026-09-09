// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QEventLoop>
#include <QRectF>
#include <QSplitter>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>
#include <cmath>
#include <functional>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotMagnifier.h"
#include "pj_plotting/PlotPanner.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/IDataWidget.h"
#include "pj_runtime/SessionManager.h"

using namespace Qt::StringLiterals;

namespace {

// Only these suites disable GL; other runner cases must retain their canvas behavior.
class PlotStateTransactionEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    const auto* unit_test = ::testing::UnitTest::GetInstance();
    for (int i = 0; i < unit_test->total_test_suite_count(); ++i) {
      const auto* suite = unit_test->GetTestSuite(i);
      const std::string_view name = suite->name();
      if (suite->should_run() &&
          (name == "PlotGestureSnapshot" || name == "PlotDockerXmlValidation" || name == "PlotDockerUndo")) {
        PJ::PlotWidgetBase::setOpenGlDisabledOverride(true);
        return;
      }
    }
  }
};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new PlotStateTransactionEnvironment);

constexpr PJ::Timestamp kSecondNs = 1'000'000'000;

PJ::TopicId addScalarTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle.has_value()) << handle.error();
  if (!handle.has_value()) {
    return 0;
  }
  writer.appendScalar(*handle, 0, 1.0);
  writer.appendScalar(*handle, 10 * kSecondNs, 2.0);
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  return handle->topic_id;
}

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const PJ::CurveDescriptor& descriptor : catalog.curves()) {
    if (descriptor.topic_id == topic_id) {
      return descriptor.name;
    }
  }
  return {};
}

class GesturePlotWidget : public PJ::PlotWidget {
 public:
  using PJ::PlotWidget::PlotWidget;

  void panBy(int delta_x, int delta_y) {
    panner1()->moveCanvas(delta_x, delta_y);
  }

  void magnifyX(double factor) {
    magnifier()->rescale(factor, PJ::PlotMagnifier::kXAxis);
  }
};

struct GestureFixture {
  PJ::SessionManager session;
  PJ::CatalogModel catalog{&session};
  GesturePlotWidget plot{&session, &catalog};

  GestureFixture() {
    auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
    EXPECT_TRUE(dataset.has_value()) << dataset.error();
    if (!dataset.has_value()) {
      return;
    }
    const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/value"));
    EXPECT_FALSE(key.isEmpty());
    EXPECT_NE(plot.addCurve(key), nullptr);

    plot.resize(800, 500);
    plot.show();
    QApplication::processEvents();
    plot.setZoomRectangle(QRectF(2.0, -5.0, 4.0, 10.0), /*emit_signal=*/false);
    plot.replot();
    QApplication::processEvents();
  }
};

struct SynchronousSnapshot {
  int undo_count = 0;
  QRectF event_rect;
  QRectF canvas_rect;
  double saved_left = 0.0;
  double saved_right = 0.0;
};

void watchSynchronousSnapshot(GesturePlotWidget& plot, SynchronousSnapshot& snapshot) {
  QObject::connect(
      &plot, &PJ::PlotWidget::rectChanged, &plot,
      [&](PJ::PlotWidget*, const QRectF& rect) { snapshot.event_rect = rect; }, Qt::DirectConnection);
  QObject::connect(
      &plot, &PJ::PlotWidget::undoableChange, &plot,
      [&]() {
        ++snapshot.undo_count;
        snapshot.canvas_rect = plot.currentBoundingRect();
        QDomDocument doc;
        const QDomElement range = plot.xmlSaveState(doc).firstChildElement(u"range"_s);
        ASSERT_FALSE(range.isNull());
        snapshot.saved_left = range.attribute(u"left"_s).toDouble();
        snapshot.saved_right = range.attribute(u"right"_s).toDouble();
      },
      Qt::DirectConnection);
}

void expectSnapshotMatchesCommittedGesture(const QRectF& before, const SynchronousSnapshot& snapshot) {
  ASSERT_EQ(snapshot.undo_count, 1);
  EXPECT_GT(std::abs(snapshot.event_rect.left() - before.left()), 1e-6);
  EXPECT_NEAR(snapshot.canvas_rect.left(), snapshot.event_rect.left(), 1e-6);
  EXPECT_NEAR(snapshot.canvas_rect.right(), snapshot.event_rect.right(), 1e-6);
  EXPECT_NEAR(snapshot.saved_left, snapshot.event_rect.left(), 1e-6);
  EXPECT_NEAR(snapshot.saved_right, snapshot.event_rect.right(), 1e-6);
}

QString serializedState(PJ::PlotDocker& docker) {
  QDomDocument doc;
  doc.appendChild(docker.xmlSaveState(doc));
  return doc.toString(-1);
}

// Gesture-driven undo publication is debounced (one snapshot after the gesture
// goes quiet); spin the event loop past that window so it fires.
void pumpPastGestureDebounce() {
  QEventLoop loop;
  QTimer::singleShot(350, &loop, &QEventLoop::quit);
  loop.exec();
}

class SplitterRestoreProbe : public QWidget, public PJ::IDataWidget {
 public:
  explicit SplitterRestoreProbe(std::function<void()> probe, QWidget* parent = nullptr)
      : QWidget(parent), probe_(std::move(probe)) {}

  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double /*time*/) override {}
  bool xmlLoadState(const QDomElement& /*element*/) override {
    probe_();
    return true;
  }

 private:
  std::function<void()> probe_;
};

void expectRejectedWithoutMutation(const QString& xml) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"original"_s, &session, &catalog);
  const QString original_id = docker.stateId();
  const QString original_xml = serializedState(docker);
  const int original_count = docker.plotCount();

  QDomDocument doc;
  ASSERT_TRUE(doc.setContent(xml));
  EXPECT_FALSE(docker.xmlLoadState(doc.documentElement())) << qPrintable(xml);
  EXPECT_EQ(docker.stateId(), original_id);
  EXPECT_EQ(docker.name(), u"original"_s);
  EXPECT_EQ(docker.plotCount(), original_count);
  EXPECT_EQ(serializedState(docker), original_xml);
}

}  // namespace

TEST(PlotGestureSnapshot, PanPublishesAfterCanvasMapsAreUpdated) {
  GestureFixture fixture;
  const QRectF before = fixture.plot.currentBoundingRect();
  SynchronousSnapshot snapshot;
  watchSynchronousSnapshot(fixture.plot, snapshot);

  fixture.plot.panBy(60, 0);
  pumpPastGestureDebounce();

  expectSnapshotMatchesCommittedGesture(before, snapshot);
}

TEST(PlotGestureSnapshot, MagnificationPublishesAfterCanvasMapsAreUpdated) {
  GestureFixture fixture;
  const QRectF before = fixture.plot.currentBoundingRect();
  SynchronousSnapshot snapshot;
  watchSynchronousSnapshot(fixture.plot, snapshot);

  fixture.plot.magnifyX(2.0);
  pumpPastGestureDebounce();

  expectSnapshotMatchesCommittedGesture(before, snapshot);
}

TEST(PlotGestureSnapshot, PlainWidgetResizeDoesNotCreateUndoEntry) {
  GestureFixture fixture;
  int undo_count = 0;
  QObject::connect(
      &fixture.plot, &PJ::PlotWidget::undoableChange, &fixture.plot, [&]() { ++undo_count; }, Qt::DirectConnection);

  fixture.plot.resize(950, 620);
  pumpPastGestureDebounce();

  EXPECT_EQ(undo_count, 0);
}

TEST(PlotDockerXmlValidation, RejectsMalformedTreesBeforeMutatingLiveState) {
  const QStringList malformed_layouts = {
      // A malformed splitter branch must reject the whole structural unit.
      uR"(<Tab id="changed" tab_name="changed" containers="1"><Container>
          <DockSplitter orientation="|" count="2" sizes="0.5;0.5">
          <DockArea id="a1" name="A"><placeholder/></DockArea><UnknownBranch/>
          </DockSplitter></Container></Tab>)"_s,
      uR"(<Tab containers="1"><Container><DockSplitter orientation="?" count="1" sizes="1">
          <DockArea><placeholder/></DockArea></DockSplitter></Container></Tab>)"_s,
      uR"(<Tab containers="1"><Container><DockSplitter orientation="|" count="two" sizes="1">
          <DockArea><placeholder/></DockArea></DockSplitter></Container></Tab>)"_s,
      uR"(<Tab containers="1"><Container><DockSplitter orientation="|" count="2" sizes="1">
          <DockArea><placeholder/></DockArea></DockSplitter></Container></Tab>)"_s,
      uR"(<Tab containers="1"><Container><DockSplitter orientation="|" count="1" sizes="nan">
          <DockArea><placeholder/></DockArea></DockSplitter></Container></Tab>)"_s,
      uR"(<Tab containers="1"><Container><DockSplitter orientation="|" count="2" sizes="0.5;">
          <DockArea><placeholder/></DockArea><DockArea><placeholder/></DockArea>
          </DockSplitter></Container></Tab>)"_s,
      uR"(<Tab containers="1"><Container><DockSplitter orientation="|" count="2" sizes="0;0">
          <DockArea><placeholder/></DockArea><DockArea><placeholder/></DockArea>
          </DockSplitter></Container></Tab>)"_s,
      // One dock area has exactly one payload; choosing a first sibling is lossy.
      uR"(<Tab containers="1"><Container><DockArea><placeholder/><plot mode="TimeSeries"/></DockArea>
          </Container></Tab>)"_s,
      uR"(<Tab containers="1"><Container><DockArea><placeholder><UnknownElement/></placeholder></DockArea>
          </Container></Tab>)"_s,
      // Floating containers are unsupported; accepting two restores only one.
      uR"(<Tab containers="2"><Container><DockArea><placeholder/></DockArea></Container>
          <Container><DockArea><placeholder/></DockArea></Container></Tab>)"_s,
      uR"(<Tab containers="many"><Container><DockArea><placeholder/></DockArea></Container></Tab>)"_s,
  };

  for (const QString& xml : malformed_layouts) {
    expectRejectedWithoutMutation(xml);
  }
}

TEST(PlotDockerXmlValidation, ToleratesUnknownAttributesAtEveryLayoutLevel) {
  QDomDocument doc;
  ASSERT_TRUE(
      doc.setContent(uR"(
      <Tab id="future" containers="1" future_tab="yes"><Container future_container="yes">
      <DockSplitter orientation="|" count="2" sizes="0.5;0.5" future_splitter="yes">
      <DockArea id="a" name="A" future_area="yes"><placeholder future_placeholder="yes"/></DockArea>
      <DockArea id="b" name="B" future_area="yes"><placeholder future_placeholder="yes"/></DockArea>
      </DockSplitter></Container></Tab>)"_s));

  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"original"_s, &session, &catalog);
  EXPECT_TRUE(docker.xmlLoadState(doc.documentElement()));
  EXPECT_EQ(docker.plotCount(), 2);
}

TEST(PlotDockerUndo, SplitterMovesPublishOutsideRestoreAndStaySuppressedDuringRestore) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  PJ::DockWidget* first = docker.plotAt(0);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(first->splitHorizontal(), nullptr);

  QSplitter* splitter = nullptr;
  for (QSplitter* candidate : docker.findChildren<QSplitter*>()) {
    if (candidate->count() == 2) {
      splitter = candidate;
      break;
    }
  }
  ASSERT_NE(splitter, nullptr);

  int undo_count = 0;
  QObject::connect(&docker, &PJ::PlotDocker::undoableChange, &docker, [&]() { ++undo_count; });
  ASSERT_TRUE(
      QMetaObject::invokeMethod(splitter, "splitterMoved", Qt::DirectConnection, Q_ARG(int, 100), Q_ARG(int, 1)));
  pumpPastGestureDebounce();
  EXPECT_EQ(undo_count, 1);

  QDomDocument doc;
  QDomElement saved = docker.xmlSaveState(doc);
  QDomElement second_area = saved.elementsByTagName(u"DockArea"_s).at(1).toElement();
  ASSERT_FALSE(second_area.isNull());
  QDomElement second_payload = second_area.firstChildElement();
  ASSERT_FALSE(second_payload.isNull());
  second_payload.setTagName(u"splitter_probe"_s);

  undo_count = 0;
  bool restore_probe_ran = false;
  docker.setObjectWidgetFactory(
      [&](const QString& kind, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
        if (kind != "splitter_probe"_L1) {
          return nullptr;
        }
        return new SplitterRestoreProbe(
            [&]() {
              QSplitter* restored_splitter = nullptr;
              for (QSplitter* candidate : docker.findChildren<QSplitter*>()) {
                if (candidate->count() == 2) {
                  restored_splitter = candidate;
                  break;
                }
              }
              ASSERT_NE(restored_splitter, nullptr);
              const int before_probe = undo_count;
              ASSERT_TRUE(
                  QMetaObject::invokeMethod(
                      restored_splitter, "splitterMoved", Qt::DirectConnection, Q_ARG(int, 200), Q_ARG(int, 1)));
              EXPECT_EQ(undo_count, before_probe);
              restore_probe_ran = true;
            },
            parent);
      });
  ASSERT_TRUE(docker.xmlLoadState(saved));
  EXPECT_TRUE(restore_probe_ran);
  const int after_restore = undo_count;
  pumpPastGestureDebounce();
  EXPECT_EQ(undo_count, after_restore) << "a mid-restore splitter move must not publish late";

  QSplitter* restored_splitter = nullptr;
  for (QSplitter* candidate : docker.findChildren<QSplitter*>()) {
    if (candidate->count() == 2) {
      restored_splitter = candidate;
      break;
    }
  }
  ASSERT_NE(restored_splitter, nullptr);
  undo_count = 0;
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          restored_splitter, "splitterMoved", Qt::DirectConnection, Q_ARG(int, 300), Q_ARG(int, 1)));
  pumpPastGestureDebounce();
  EXPECT_EQ(undo_count, 1);
}
