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
#include <QStyle>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <QWheelEvent>
#include <memory>
#include <string>
#include <vector>

#include "pj_log/LogTableModel.h"
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
  [[nodiscard]] int timeWidth() const {
    return list()->header()->sectionSize(PJ::log_view::LogTableModel::kTime);
  }
  /// The Time column shows @p time whole.
  [[nodiscard]] bool timeFits(double time, PJ::TimeLabelFormat format = PJ::TimeLabelFormat::kSeconds) const {
    const QString text = PJ::formatTimeLabel(time, format, 3, true);
    const int margin = list()->style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, list()) + 1;
    return timeWidth() >= list()->fontMetrics().horizontalAdvance(text) + 2 * margin;
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
  EXPECT_TRUE(view_.topicHidden(0));
  EXPECT_FALSE(view_.topicHidden(1));
  // Renamed, a hidden topic stays hidden: hiding belongs to the topic, not to its label.
  view_.setTopicLabel(0, "/renamed");
  EXPECT_EQ(model()->rowCount(), 2);
  EXPECT_FALSE(topics->menu()->actions()[0]->isChecked());

  view_.removeTopic(1);
  EXPECT_EQ(model()->rowCount(), 0);  // /rosout is still hidden
  view_.removeTopic(0);
  EXPECT_EQ(pages->currentWidget()->objectName(), "emptyPage");
  // A removed topic is forgotten: a topic taking its id shows.
  fill(0, "/rosout", 3);
  EXPECT_FALSE(view_.topicHidden(0));
  EXPECT_EQ(model()->rowCount(), 3);
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
  EXPECT_EQ(future->toolTip(), "Show the entries after the playhead, greyed") << "the tooltip names what a click does";
  EXPECT_EQ(model()->rowCount(), 50);
  QSignalSpy edited(&view_, &LogView::stateEdited);
  future->click();
  EXPECT_EQ(edited.count(), 1);
  EXPECT_TRUE(view_.state().show_future);
  EXPECT_EQ(future->toolTip(), "Hide the entries after the playhead");
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

TEST_F(LogViewTest, EntriesAppendedAfterASourceReplaceItsRows) {
  view_.setTopicLabel(0, "/rosout");
  auto source = std::make_shared<PJ::VectorLogRowSource>();
  source->merge({entry(1.0, "one"), entry(2.0, "two"), entry(3.0, "three")});
  view_.setTopicRows(0, source);
  ASSERT_EQ(view_.store().size(), 3u);
  view_.appendEntries(0, {entry(4.0, "appended")});  // a new source with its own keys
  ASSERT_EQ(view_.store().size(), 1u);
  EXPECT_EQ(view_.store().row(0).message, "appended");
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

// The entry the user picked stays theirs: a seek back that hides it selects nothing in its place, and the seek forward
// that shows it again selects it again.
TEST_F(LogViewTest, ASelectionHiddenByASeekBackComesBackWithIt) {
  fill(0, "/rosout", 20);
  track(10.0);
  list()->setCurrentIndex(model()->index(5, 0));
  ASSERT_TRUE(detail().contains("/rosout #5"));

  track(2.0);  // later rows hidden: /rosout #5 with them
  EXPECT_FALSE(list()->selectionModel()->hasSelection());
  EXPECT_EQ(detail(), QString());

  track(10.0);
  EXPECT_EQ(list()->currentIndex().row(), 5);
  EXPECT_TRUE(list()->selectionModel()->isRowSelected(5, QModelIndex()));
  EXPECT_TRUE(detail().contains("/rosout #5"));
}

// A topic indexed anew (a merge folds another log into it) rebuilds the rows under the viewport: the list finds the
// tracker again instead of keeping a scroll offset that now points at other rows.
TEST_F(LogViewTest, ATopicIndexedAnewKeepsTheTrackerInView) {
  fill(0, "/rosout", 200);
  track(150.0);
  ASSERT_TRUE(rowVisible(150));

  auto merged = std::make_shared<PJ::VectorLogRowSource>();
  std::vector<LogEntry> entries;
  for (int i = 0; i < 200; ++i) {
    entries.push_back(entry(i, "/rosout #" + std::to_string(i)));
    entries.push_back(entry(i + 0.5, "/other #" + std::to_string(i)));
  }
  merged->merge(std::move(entries));
  view_.setTopicRows(0, merged);
  QApplication::processEvents();

  ASSERT_EQ(model()->rowCount(), 301);  // later rows hidden: up to "/rosout #150"
  EXPECT_TRUE(rowVisible(300)) << "the tracker's row";
}

// A file reload refills the topic in place: the host shows it empty, then indexes the rows again over several updates.
// The picked entry is selected again and the list stays on the tracker's row through all of them, not the top.
TEST_F(LogViewTest, AProgressiveRefillEndsOnTheTracker) {
  fill(0, "/rosout", 200);
  track(150.0);
  list()->setCurrentIndex(model()->index(145, 0));
  view_.setTopicRows(0, std::make_shared<PJ::VectorLogRowSource>());
  QApplication::processEvents();

  const std::vector<LogEntry> all = numbered("/rosout", 200);
  auto refilled = std::make_shared<PJ::VectorLogRowSource>();
  refilled->merge(std::vector<LogEntry>(all.begin(), all.begin() + 40));
  view_.updateTopicRows(0, refilled);
  QApplication::processEvents();
  refilled->merge(std::vector<LogEntry>(all.begin() + 40, all.end()));
  view_.updateTopicRows(0, refilled);
  QApplication::processEvents();

  EXPECT_TRUE(rowVisible(150)) << "the tracker's row";
  EXPECT_EQ(list()->currentIndex().row(), 145);
  EXPECT_TRUE(detail().contains("/rosout #145"));
}

// Rows landing before the tracker's row (another topic refilled) push it down: the list keeps it in view.
TEST_F(LogViewTest, RowsLandingBeforeTheTrackerKeepItInView) {
  fill(0, "/rosout", 200);
  fill(1, "/other", 200);
  track(150.0);
  view_.setTopicRows(0, std::make_shared<PJ::VectorLogRowSource>());
  QApplication::processEvents();
  ASSERT_TRUE(rowVisible(model()->rowCount() - 1));

  auto refilled = std::make_shared<PJ::VectorLogRowSource>();
  refilled->merge(numbered("/rosout", 200));
  view_.updateTopicRows(0, refilled);
  QApplication::processEvents();

  EXPECT_TRUE(rowVisible(model()->rowCount() - 1)) << "the tracker's row";
}

// A drag across rows selects the row it ends on before Qt makes it current: that row is the user's choice too.
TEST_F(LogViewTest, ADraggedSelectionStaysThroughATrackerMove) {
  fill(0, "/rosout", 20);
  track(19.0);
  const auto center = [this](int row) { return list()->visualRect(model()->index(row, 0)).center(); };
  QTest::mousePress(list()->viewport(), Qt::LeftButton, {}, center(3));
  QTest::mouseMove(list()->viewport(), center(5));
  QTest::mouseRelease(list()->viewport(), Qt::LeftButton, {}, center(5));
  ASSERT_TRUE(list()->selectionModel()->isRowSelected(5, QModelIndex()));

  track(18.0);
  EXPECT_TRUE(list()->selectionModel()->isRowSelected(5, QModelIndex()));
  EXPECT_TRUE(detail().contains("/rosout #5"));
}

// A hidden topic keeps the Topics menu that unhides it, even when it is the only topic left (the other one deleted,
// or folded into it by a merge) or the only one restored.
TEST_F(LogViewTest, AHiddenTopicKeepsTheTopicsMenu) {
  auto* topics = view_.findChild<QToolButton*>("topicsButton");
  ASSERT_NE(topics, nullptr);
  fill(0, "/rosout", 3);
  fill(1, "/other", 3);
  topics->menu()->actions()[0]->setChecked(false);
  view_.removeTopic(1);
  ASSERT_EQ(model()->rowCount(), 0);
  EXPECT_TRUE(topics->isVisibleTo(&view_));
  topics->menu()->actions()[0]->setChecked(true);
  EXPECT_EQ(model()->rowCount(), 3);
  EXPECT_FALSE(topics->isVisibleTo(&view_)) << "one topic, shown: nothing to choose";

  view_.setTopicHidden(0, true);
  EXPECT_TRUE(topics->isVisibleTo(&view_));
}

// A topic removed takes the user's choice along: another topic that takes its id later is not selected by it.
TEST_F(LogViewTest, ARemovedTopicTakesItsSelectionAlong) {
  fill(0, "/rosout", 20);
  list()->setCurrentIndex(model()->index(5, 0));
  view_.removeTopic(0);
  fill(0, "/other", 20);  // the same times
  EXPECT_FALSE(list()->selectionModel()->hasSelection());
}

// Hiding a topic over a large list filters in slices; rows that land meanwhile follow the new filter at once.
TEST_F(LogViewTest, ATopicHiddenBeforeItsRowsLandNeverShowsThem) {
  constexpr int kRows = static_cast<int>(PJ::log_view::LogTableModel::kFilterSliceRows) + 10;
  fill(0, "/rosout", kRows);
  view_.setTopicLabel(1, "/other");
  view_.setTopicHidden(1, true);
  view_.replaceEntries(1, numbered("/other", 10));
  EXPECT_EQ(model()->rowCount(), kRows);
  QApplication::processEvents();
  QApplication::processEvents();
  EXPECT_EQ(model()->rowCount(), kRows);
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

TEST_F(LogViewTest, VisibleFilterFieldsShareTheRowEvenly) {
  view_.setTopicLabel(0, "/rosout");
  view_.replaceEntries(0, {entry(1.0, "a", LogLevel::kInfo, "/node", "motion")});
  QApplication::processEvents();
  const auto* source = view_.findChild<PJ::Search*>("sourceEdit");
  const auto* category = view_.findChild<PJ::Search*>("categoryEdit");
  const auto* message = view_.findChild<PJ::Search*>("filterEdit");
  ASSERT_TRUE(source->isVisible() && category->isVisible());
  EXPECT_NEAR(source->width(), message->width(), 1);
  EXPECT_NEAR(category->width(), message->width(), 1);

  view_.replaceEntries(0, {entry(1.0, "a", LogLevel::kInfo, "/node")});  // no category left: its field hides
  QApplication::processEvents();
  ASSERT_FALSE(category->isVisible());
  EXPECT_NEAR(source->width(), message->width(), 1);
}

// The Time column fits every time the topics hold, not only the rows shown: an entry after the playhead, hidden now,
// shows whole when the playhead reaches it. It follows the times both ways; a width the user dragged stays.
TEST_F(LogViewTest, TheTimeColumnFitsEveryTimeTheTopicsHold) {
  track(0.0);
  view_.setTopicLabel(0, "/rosout");
  view_.replaceEntries(0, {entry(0.0, "first"), entry(1180.25, "after the playhead")});
  QApplication::processEvents();
  ASSERT_EQ(model()->rowCount(), 1);
  EXPECT_TRUE(timeFits(1180.25));

  view_.appendEntries(0, {entry(123456.5, "later")});
  QApplication::processEvents();
  EXPECT_TRUE(timeFits(123456.5));
  const int wide = timeWidth();
  view_.replaceEntries(0, {entry(0.0, "first"), entry(1.5, "short")});
  QApplication::processEvents();
  EXPECT_LT(timeWidth(), wide) << "narrower times narrow it";

  list()->header()->resizeSection(0, timeWidth() + 10);
  const int dragged = timeWidth();
  view_.appendEntries(0, {entry(98765432.5, "much later")});
  QApplication::processEvents();
  EXPECT_EQ(timeWidth(), dragged);
}

TEST_F(LogViewTest, TheTimeColumnFitsTheEarliestTimeToo) {
  track(0.0);
  view_.setTopicLabel(0, "/rosout");
  view_.replaceEntries(0, {entry(-123456.5, "before the start"), entry(0.0, "start")});
  QApplication::processEvents();
  EXPECT_TRUE(timeFits(-123456.5));
}

// The host repeats its time format on every Preferences OK: only a different format fits the column again.
TEST_F(LogViewTest, TheSameTimeFormatKeepsADraggedTimeWidth) {
  track(0.0);
  view_.setTopicLabel(0, "/rosout");
  view_.replaceEntries(0, {entry(0.0, "first"), entry(1180.25, "later")});
  QApplication::processEvents();
  list()->header()->resizeSection(0, timeWidth() + 10);
  const int dragged = timeWidth();
  view_.setTimeLabelFormat(PJ::TimeLabelFormat::kSeconds);
  EXPECT_EQ(timeWidth(), dragged);
}

TEST_F(LogViewTest, AFormatChangeWithNoRowsFitsTheNextOnes) {
  track(2e9);
  view_.setTopicLabel(0, "/rosout");
  view_.replaceEntries(0, {entry(1.0, "first")});
  QApplication::processEvents();
  list()->header()->resizeSection(0, timeWidth() + 10);
  view_.replaceEntries(0, {});
  view_.setTimeLabelFormat(PJ::TimeLabelFormat::kUtcClock);
  view_.replaceEntries(0, {entry(1779215727.239, "instant")});
  QApplication::processEvents();
  EXPECT_TRUE(timeFits(1779215727.239, PJ::TimeLabelFormat::kUtcClock));
}

// QTreeView fits a double-clicked divider to the rows it shows; the Time divider fits every time instead.
TEST_F(LogViewTest, DoubleClickingTheTimeDividerFitsEveryTime) {
  track(0.0);
  view_.setTopicLabel(0, "/rosout");
  view_.replaceEntries(0, {entry(0.0, "first"), entry(1180.25, "after the playhead")});
  QApplication::processEvents();
  emit list() -> header()->sectionHandleDoubleClicked(0);
  QApplication::processEvents();
  EXPECT_TRUE(timeFits(1180.25));
  view_.appendEntries(0, {entry(123456.5, "later")});
  QApplication::processEvents();
  EXPECT_TRUE(timeFits(123456.5)) << "the fitted width is not mistaken for a drag";
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
