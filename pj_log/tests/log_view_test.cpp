// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <QWheelEvent>
#include <string>
#include <vector>

#include "pj_log/LogView.h"
#include "pj_widgets/Search.h"

namespace {

using PJ::LogEntry;
using PJ::LogLevel;
using PJ::LogView;
using PJ::LogViewState;

LogEntry entry(
    double time, const std::string& message, LogLevel level = LogLevel::kInfo, const std::string& source = "/node",
    const std::string& category = {}) {
  LogEntry out;
  out.time = time;
  out.level = level;
  out.source = source;
  out.category = category;
  out.message = message;
  return out;
}

/// Entries "<label> #i" at times 0..count-1.
std::vector<LogEntry> numbered(const std::string& label, int count) {
  std::vector<LogEntry> out;
  for (int i = 0; i < count; ++i) {
    out.push_back(entry(i, label + " #" + std::to_string(i)));
  }
  return out;
}

class LogViewTest : public ::testing::Test {
 protected:
  void SetUp() override {
    view_.resize(700, 500);
    view_.show();
    QApplication::processEvents();
  }

  void fill(uint32_t topic, const QString& label, int count) {
    view_.setTopicLabel(topic, label);
    view_.replaceEntries(topic, numbered(label.toStdString(), count));
    QApplication::processEvents();
  }

  void track(double time) {
    view_.setTrackerTime(time);
    QApplication::processEvents();
  }

  [[nodiscard]] QTreeView* list() const {
    return view_.findChild<QTreeView*>("logView");
  }
  [[nodiscard]] QAbstractItemModel* model() const {
    return list()->model();
  }
  [[nodiscard]] bool rowVisible(int row) const {
    const QRect rect = list()->visualRect(model()->index(row, 0));
    return rect.isValid() && list()->viewport()->rect().intersects(rect);
  }
  [[nodiscard]] QString message(int row) const {
    return model()->index(row, 5).data().toString();
  }
  /// The visible sections end to end, Message filling the rest of the viewport: a hidden column leaves no gap.
  void expectNoGap() const {
    QApplication::processEvents();
    const QHeaderView* header = list()->header();
    int previous = -1;
    for (int section = 0; section < 5; ++section) {
      if (!header->isSectionHidden(section)) {
        previous = section;
      }
    }
    ASSERT_GE(previous, 0);
    EXPECT_EQ(
        header->sectionViewportPosition(5), header->sectionViewportPosition(previous) + header->sectionSize(previous));
    EXPECT_EQ(header->length(), list()->viewport()->width());
  }
  [[nodiscard]] QString detail() const {
    return view_.findChild<QPlainTextEdit*>("detailText")->toPlainText();
  }

  LogView view_;
};

TEST_F(LogViewTest, EmptyStateAndTopicColumn) {
  auto* pages = view_.findChild<QStackedWidget*>("pages");
  auto* topics = view_.findChild<QToolButton*>("topicsButton");
  ASSERT_NE(pages, nullptr);
  ASSERT_NE(topics, nullptr);
  EXPECT_EQ(pages->currentWidget()->objectName(), "emptyPage");

  fill(0, "/rosout", 3);
  EXPECT_EQ(pages->currentWidget()->objectName(), "contentPage");
  EXPECT_TRUE(list()->isColumnHidden(4));
  EXPECT_FALSE(topics->isVisibleTo(&view_));

  fill(1, "/other/rosout", 2);
  EXPECT_FALSE(list()->isColumnHidden(4));
  EXPECT_TRUE(topics->isVisibleTo(&view_));
  EXPECT_EQ(model()->rowCount(), 5);

  // Hiding a topic from the menu hides its rows.
  ASSERT_EQ(topics->menu()->actions().size(), 2);
  topics->menu()->actions()[0]->setChecked(false);  // listed by topic id: /rosout first
  EXPECT_EQ(model()->rowCount(), 2);
  EXPECT_EQ(view_.state().hidden_topics, QStringList{"/rosout"});

  view_.removeTopic(1);
  EXPECT_EQ(model()->rowCount(), 0);  // /rosout is still hidden
  view_.removeTopic(0);
  EXPECT_EQ(pages->currentWidget()->objectName(), "emptyPage");
}

TEST_F(LogViewTest, NothingInThePanelTakesDrops) {
  EXPECT_FALSE(view_.acceptDrops());
  EXPECT_FALSE(list()->acceptDrops());
  EXPECT_FALSE(list()->viewport()->acceptDrops());
  EXPECT_FALSE(view_.findChild<PJ::Search*>("filterEdit")->lineEdit()->acceptDrops());
  auto* text = view_.findChild<QPlainTextEdit*>("detailText");
  EXPECT_FALSE(text->acceptDrops());
  EXPECT_FALSE(text->viewport()->acceptDrops());
}

TEST_F(LogViewTest, PlayingFollowsTheTrackerAndBlocksScrolling) {
  fill(0, "/rosout", 500);
  view_.setPlaying(true);
  track(250.0);
  ASSERT_EQ(model()->rowCount(), 251);  // later rows hidden
  EXPECT_TRUE(rowVisible(250));
  EXPECT_FALSE(rowVisible(0));
  QScrollBar* bar = list()->verticalScrollBar();
  EXPECT_FALSE(bar->isEnabled());
  const int before = bar->value();

  const QPoint centre = list()->viewport()->rect().center();
  QWheelEvent wheel(
      centre, list()->viewport()->mapToGlobal(centre), QPoint(), QPoint(0, -360), Qt::NoButton, Qt::NoModifier,
      Qt::NoScrollPhase, false);
  QApplication::sendEvent(list()->viewport(), &wheel);
  QTest::keyClick(list(), Qt::Key_PageDown);
  QApplication::processEvents();
  EXPECT_EQ(bar->value(), before);

  track(400.0);
  EXPECT_TRUE(rowVisible(400));
  EXPECT_GT(bar->value(), before);

  view_.setPlaying(false);
  EXPECT_TRUE(bar->isEnabled());
  const int paused = bar->value();
  // Up: the list ends at the tracker's row, so it can only scroll back.
  QWheelEvent wheel_up(
      centre, list()->viewport()->mapToGlobal(centre), QPoint(), QPoint(0, 360), Qt::NoButton, Qt::NoModifier,
      Qt::NoScrollPhase, false);
  QApplication::sendEvent(list()->viewport(), &wheel_up);
  QApplication::processEvents();
  EXPECT_NE(bar->value(), paused);
}

TEST_F(LogViewTest, PausedDoubleClickSeeksToTheRowAndKeepsTheView) {
  // Greyed future rows: a seek back keeps every row, so the scroll position can stay.
  LogViewState grey = view_.state();
  grey.show_future = true;
  view_.applyState(grey);
  fill(0, "/rosout", 500);
  track(300.0);
  QSignalSpy seeks(&view_, &LogView::seekRequested);
  list()->scrollTo(model()->index(100, 0), QAbstractItemView::PositionAtTop);
  QApplication::processEvents();
  const int scrolled = list()->verticalScrollBar()->value();
  ASSERT_TRUE(rowVisible(105));

  const QRect rect = list()->visualRect(model()->index(105, 5));
  // A single click selects and shows the entry, and leaves the tracker alone.
  QTest::mouseClick(list()->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
  EXPECT_EQ(seeks.count(), 0);
  EXPECT_EQ(list()->currentIndex().row(), 105);
  EXPECT_TRUE(detail().contains("/rosout #105"));
  QTest::mouseDClick(list()->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
  ASSERT_EQ(seeks.count(), 1);
  EXPECT_DOUBLE_EQ(seeks.front().front().toDouble(), 105.0);

  // The host answers the seek by moving the tracker there: the view stays on the clicked row.
  track(105.0);
  EXPECT_EQ(list()->verticalScrollBar()->value(), scrolled);
  EXPECT_EQ(list()->currentIndex().row(), 105);

  // A tracker move from elsewhere brings the last revealed row into view.
  track(450.0);
  EXPECT_TRUE(rowVisible(450));

  // Playing, a double click does not seek.
  view_.setPlaying(true);
  QTest::mouseDClick(list()->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
  EXPECT_EQ(seeks.count(), 1);
}

TEST_F(LogViewTest, FutureRowsGreyOrHidden) {
  fill(0, "/rosout", 100);
  track(49.5);
  // Hidden by default; the eye shows them greyed.
  auto* future = view_.findChild<QToolButton*>("futureButton");
  EXPECT_FALSE(view_.state().show_future);
  EXPECT_FALSE(future->isChecked());
  EXPECT_EQ(model()->rowCount(), 50);
  QSignalSpy edited(&view_, &LogView::stateEdited);
  future->click();
  EXPECT_EQ(edited.count(), 1);
  EXPECT_TRUE(view_.state().show_future);
  EXPECT_EQ(model()->rowCount(), 100);
  const QColor grey = list()->palette().color(QPalette::PlaceholderText);
  EXPECT_EQ(model()->index(60, 5).data(Qt::ForegroundRole).value<QColor>(), grey);
  EXPECT_FALSE(model()->index(40, 5).data(Qt::ForegroundRole).isValid());
  // Only the row the tracker is at (the last one at or before it) is tinted, and it follows the tracker.
  EXPECT_TRUE(model()->index(49, 0).data(Qt::BackgroundRole).isValid());
  EXPECT_FALSE(model()->index(50, 0).data(Qt::BackgroundRole).isValid());
  track(52.5);
  EXPECT_TRUE(model()->index(52, 0).data(Qt::BackgroundRole).isValid());
  EXPECT_FALSE(model()->index(49, 0).data(Qt::BackgroundRole).isValid());

  future->click();
  EXPECT_EQ(edited.count(), 2);
  EXPECT_FALSE(view_.state().show_future);
  EXPECT_EQ(model()->rowCount(), 53);
  track(80.0);
  EXPECT_EQ(model()->rowCount(), 81);
  // New entries after the tracker stay hidden; at or before it they appear.
  view_.appendEntries(0, {entry(100.0, "late")});
  EXPECT_EQ(model()->rowCount(), 81);
  track(200.0);
  EXPECT_EQ(model()->rowCount(), 101);
}

TEST_F(LogViewTest, LevelColoursAndFirstLine) {
  view_.setTopicLabel(0, "/rosout");
  view_.replaceEntries(
      0, {entry(1.0, "fine"), entry(2.0, "careful\nsecond line", LogLevel::kWarning),
          entry(3.0, "broken", LogLevel::kError)});
  track(5.0);
  EXPECT_FALSE(model()->index(0, 1).data(Qt::ForegroundRole).isValid());
  const QColor warn = model()->index(1, 1).data(Qt::ForegroundRole).value<QColor>();
  const QColor error = model()->index(2, 1).data(Qt::ForegroundRole).value<QColor>();
  EXPECT_TRUE(warn.isValid());
  EXPECT_TRUE(error.isValid());
  EXPECT_NE(warn, error);
  EXPECT_EQ(message(1), "careful");
  EXPECT_EQ(model()->index(1, 1).data().toString(), "Warn");
}

TEST_F(LogViewTest, FiltersFromTheToolbar) {
  view_.setTopicLabel(0, "/rosout");
  view_.replaceEntries(
      0, {entry(1.0, "motor spin", LogLevel::kDebug, "/a"), entry(2.0, "Motor ready", LogLevel::kInfo, "/b"),
          entry(3.0, "battery", LogLevel::kError, "/a"), entry(4.0, "plain", LogLevel::kUnknown, "/b")});
  track(10.0);
  ASSERT_EQ(model()->rowCount(), 4);

  QSignalSpy edited(&view_, &LogView::stateEdited);
  view_.findChild<QToolButton*>("levelDebug")->click();
  EXPECT_EQ(model()->rowCount(), 3);
  EXPECT_EQ(edited.count(), 1);

  // The text filters wait for typing to pause.
  view_.findChild<PJ::Search*>("sourceEdit")->setText("/B");
  EXPECT_EQ(model()->rowCount(), 3);
  QTest::qWait(400);
  EXPECT_EQ(model()->rowCount(), 2);

  QTest::keyClicks(view_.findChild<PJ::Search*>("filterEdit")->lineEdit(), "MOTOR");
  EXPECT_GT(edited.count(), 1);  // typing is a gesture
  QTest::qWait(400);
  EXPECT_EQ(model()->rowCount(), 1);
  EXPECT_EQ(message(0), "Motor ready");
}

TEST_F(LogViewTest, AppliedStateRoundTripsAndIsNotAGesture) {
  fill(0, "/rosout", 3);
  QSignalSpy edited(&view_, &LogView::stateEdited);
  LogViewState state;
  state.levels = {false, true, true, true, false};
  state.source = "/x";
  state.category = "cat";
  state.text = "boom";
  state.hidden_topics = {"/rosout"};
  state.show_future = false;
  view_.applyState(state);
  EXPECT_EQ(view_.state(), state);
  EXPECT_EQ(edited.count(), 0);
  EXPECT_EQ(model()->rowCount(), 0);
  // A restored category filter keeps its field although no entry has a category.
  EXPECT_TRUE(view_.findChild<PJ::Search*>("categoryEdit")->isVisibleTo(&view_));
}

TEST_F(LogViewTest, AppendsAndReloadsKeepTheSelection) {
  fill(0, "/rosout", 20);
  track(100.0);
  list()->setCurrentIndex(model()->index(5, 0));
  std::vector<LogEntry> more;
  for (int i = 20; i < 30; ++i) {
    more.push_back(entry(i, "/rosout #" + std::to_string(i)));
  }
  view_.appendEntries(0, std::move(more));
  EXPECT_EQ(model()->rowCount(), 30);
  EXPECT_EQ(list()->currentIndex().row(), 5);
  EXPECT_EQ(message(29), "/rosout #29");

  // A reload with an earlier entry renumbers the rows: the selection follows its entry.
  std::vector<LogEntry> reloaded = numbered("/rosout", 30);
  reloaded.insert(reloaded.begin(), entry(-1.0, "early"));
  view_.replaceEntries(0, std::move(reloaded));
  EXPECT_EQ(list()->currentIndex().row(), 6);
  EXPECT_EQ(message(6), "/rosout #5");
}

TEST_F(LogViewTest, CategoryColumnAndFilterAppearWithTheFirstCategory) {
  auto* field = view_.findChild<PJ::Search*>("categoryEdit");
  ASSERT_NE(field, nullptr);
  fill(0, "/rosout", 3);
  track(10.0);
  EXPECT_TRUE(list()->isColumnHidden(3));
  EXPECT_FALSE(field->isVisibleTo(&view_));

  view_.appendEntries(
      0, {entry(3.0, "joint limit", LogLevel::kInfo, "/arm", "motion"),
          entry(4.0, "link up", LogLevel::kInfo, "/net", "net")});
  EXPECT_FALSE(list()->isColumnHidden(3));
  EXPECT_TRUE(field->isVisibleTo(&view_));
  EXPECT_EQ(model()->index(3, 3).data().toString(), "motion");

  field->setText("NET");
  QTest::qWait(400);
  ASSERT_EQ(model()->rowCount(), 1);
  list()->setCurrentIndex(model()->index(0, 0));
  EXPECT_TRUE(detail().contains("Category: net"));
}

TEST_F(LogViewTest, EmptySourceAndCategoryColumnsHideWithoutAGap) {
  constexpr int kSource = 2;
  constexpr int kCategory = 3;
  view_.setTopicLabel(0, "/rosout");
  const std::vector<LogEntry> bare = {entry(1.0, "a", LogLevel::kInfo, ""), entry(2.0, "b", LogLevel::kInfo, "")};
  view_.replaceEntries(0, bare);
  track(10.0);
  EXPECT_TRUE(list()->isColumnHidden(kSource));
  EXPECT_TRUE(list()->isColumnHidden(kCategory));
  expectNoGap();

  view_.appendEntries(0, {entry(3.0, "c", LogLevel::kInfo, "", "motion")});
  EXPECT_TRUE(list()->isColumnHidden(kSource));
  EXPECT_FALSE(list()->isColumnHidden(kCategory));
  expectNoGap();
  view_.replaceEntries(0, bare);  // the only categorized entry is gone
  EXPECT_TRUE(list()->isColumnHidden(kCategory));
  expectNoGap();

  view_.setTopicLabel(1, "/other");
  view_.replaceEntries(1, {entry(4.0, "d", LogLevel::kInfo, "/node")});
  EXPECT_FALSE(list()->isColumnHidden(kSource));
  expectNoGap();
  view_.removeTopic(1);
  EXPECT_TRUE(list()->isColumnHidden(kSource));
  expectNoGap();
}

TEST_F(LogViewTest, TimeColumnAndDetailFollowTheHostFormat) {
  view_.setTopicLabel(0, "/rosout");
  view_.replaceEntries(0, {entry(1779215727.239, "instant")});
  track(2e9);
  EXPECT_EQ(model()->index(0, 0).data().toString(), "1779215727.239");
  view_.setTimeLabelFormat(PJ::TimeLabelFormat::kUtcClock);
  const QString clock = PJ::formatTimeLabel(1779215727.239, PJ::TimeLabelFormat::kUtcClock, 3, true);
  EXPECT_EQ(model()->index(0, 0).data().toString(), clock);
  list()->setCurrentIndex(model()->index(0, 0));
  EXPECT_TRUE(detail().contains("Time: " + clock));
}

}  // namespace
