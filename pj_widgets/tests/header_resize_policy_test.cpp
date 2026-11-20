// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QFontMetrics>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QTableView>
#include <QTableWidget>
#include <QTreeView>
#include <algorithm>

#include "pj_widgets/HeaderResizePolicy.h"

namespace PJ {
namespace {

constexpr int kTableWidth = 600;
constexpr int kTableHeight = 200;
constexpr int kSeedWidth = 96;
constexpr int kAbsoluteFloor = 20;

// Long enough that its label floor exceeds the seeded width, which is what makes
// a neighbour unable to give any width away.
constexpr const char* kWideLabel = "A Very Wide Column Label";

struct Fixture {
  QTableWidget table;
  QHeaderView* header;
  HeaderResizePolicy* policy;

  explicit Fixture(const QStringList& labels)
      : table(0, static_cast<int>(labels.size())), header(table.horizontalHeader()) {
    table.setHorizontalHeaderLabels(labels);
    table.resize(kTableWidth, kTableHeight);
    table.show();
    policy = HeaderResizePolicy::install(header, 0, kAbsoluteFloor);
  }

  [[nodiscard]] QList<int> widths() const {
    QList<int> out;
    for (int i = 0; i < header->count(); ++i) {
      out.push_back(header->sectionSize(i));
    }
    return out;
  }

  [[nodiscard]] int total() const {
    int sum = 0;
    for (int w : widths()) {
      sum += w;
    }
    return sum;
  }

  /// Simulates a user drag of the divider on @p section's right edge: Qt applies the new size first and the policy
  /// reacts to the signal, as during a real drag.
  void dragDivider(int section, int by) {
    header->resizeSection(section, header->sectionSize(section) + by);
  }
};

// The floor the policy is expected to enforce, derived the same way it derives
// it. Kept here so the test states the rule rather than echoing an observation.
int expectedFloor(QHeaderView* header, const QString& label) {
  constexpr int kSideMargins = 12;
  return QFontMetrics(header->font()).horizontalAdvance(label) + kSideMargins;
}

// A seed width above every non-fill label floor. The floor depends on the platform plugin's font (offscreen on Windows
// renders these labels wider than 96 px) and a seed below it is clamped up, so tests about seeds or resizes seed with
// this.
int seedAboveFloors(const Fixture& f) {
  int seed = kSeedWidth;
  for (int i = 1; i < f.header->count(); ++i) {
    seed = std::max(seed, expectedFloor(f.header, f.table.horizontalHeaderItem(i)->text()));
  }
  return seed;
}

TEST(HeaderResizePolicy, SeedingWidthsDoesNotStarveLaterColumns) {
  // Seeding through QHeaderView::resizeSection is read back as a user drag, so each seed steals from the next column
  // and the rightmost end up under their labels.
  Fixture f({"Channel name", "Schema", "Encoding", "Msg Count"});
  const int seed = seedAboveFloors(f);
  for (int i = 1; i < f.header->count(); ++i) {
    f.policy->setSectionWidth(i, seed);
  }

  for (int i = 1; i < f.header->count(); ++i) {
    EXPECT_EQ(f.header->sectionSize(i), seed) << "column " << i << " did not keep the width it was seeded with";
  }
}

TEST(HeaderResizePolicy, ColumnsNeverOpenNarrowerThanTheirLabel) {
  Fixture f({"Name", kWideLabel, "B"});
  const int floor = expectedFloor(f.header, QString::fromLatin1(kWideLabel));
  // Seeding below the label width must be refused, not obeyed.
  f.policy->setSectionWidth(1, kAbsoluteFloor);
  EXPECT_GE(f.header->sectionSize(1), floor);
}

TEST(HeaderResizePolicy, DraggingCannotSquashANeighbourUnderItsLabel) {
  Fixture f({"Name", "Schema", "Encoding"});
  for (int i = 1; i < f.header->count(); ++i) {
    f.policy->setSectionWidth(i, kSeedWidth);
  }
  const int floor = expectedFloor(f.header, QStringLiteral("Schema"));

  f.dragDivider(0, kTableWidth);  // shove the first divider as far right as it goes

  EXPECT_GE(f.header->sectionSize(1), floor) << "the Schema label was squashed";
  EXPECT_GE(f.header->sectionSize(2), expectedFloor(f.header, QStringLiteral("Encoding")));
}

TEST(HeaderResizePolicy, WideningCascadesPastANeighbourAlreadyAtItsFloor) {
  // Column 1's label is wider than the seed, so it has nothing to give. The
  // width has to come from column 2 instead of the drag simply doing nothing.
  Fixture f({"Name", kWideLabel, "Encoding"});
  // Column 2 takes the room the other two leave, less a little for the fill column: fixed at half the table it
  // overflowed where the platform font renders the labels wider (offscreen on Windows), and an overflowing drag takes
  // no width from anyone by design.
  const int room =
      f.header->width() - expectedFloor(f.header, QStringLiteral("Name")) - expectedFloor(f.header, kWideLabel);
  constexpr int kDrag = 40;
  ASSERT_GT(room - 20 - expectedFloor(f.header, QStringLiteral("Encoding")), kDrag) << "the table is too narrow";
  f.policy->setSectionWidth(2, room - 20);
  const QList<int> before = f.widths();
  ASSERT_LE(f.total(), f.header->width()) << "the columns must fit before the drag";

  f.dragDivider(0, kDrag);

  EXPECT_GT(f.header->sectionSize(0), before[0]) << "the dragged column did not grow";
  EXPECT_EQ(f.header->sectionSize(1), before[1]) << "the floored column should not have moved";
  EXPECT_LT(f.header->sectionSize(2), before[2]) << "the slack should have come from column 2";
}

TEST(HeaderResizePolicy, TotalWidthIsPreservedAcrossADrag) {
  Fixture f({"Name", "Schema", "Encoding", "Msg Count"});
  for (int i = 1; i < f.header->count(); ++i) {
    f.policy->setSectionWidth(i, kSeedWidth);
  }
  const int before = f.total();

  f.dragDivider(1, 30);
  EXPECT_EQ(f.total(), before) << "a drag must not change how much width the header occupies";

  f.dragDivider(1, -20);
  EXPECT_EQ(f.total(), before);
}

TEST(HeaderResizePolicy, HiddenHeaderFillsTheViewportAfterAResize) {
  Fixture f({"Name", "Schema", "Encoding"});
  f.header->hide();
  const int seed = seedAboveFloors(f);
  for (int i = 1; i < f.header->count(); ++i) {
    f.policy->setSectionWidth(i, seed);
  }
  f.table.resize(kTableWidth + 200, kTableHeight);
  QApplication::processEvents();
  EXPECT_EQ(f.total(), f.table.viewport()->width());
  EXPECT_EQ(f.header->sectionSize(1), seed) << "only the fill column should absorb the change";

  f.table.resize(kTableWidth - 100, kTableHeight);
  QApplication::processEvents();
  EXPECT_EQ(f.total(), f.table.viewport()->width());
  EXPECT_EQ(f.header->sectionSize(2), seed) << "only the fill column should absorb the change";
}

TEST(HeaderResizePolicy, HiddenHeaderGrowingAColumnKeepsTheTotal) {
  Fixture f({"Name", "Schema", "Encoding"});
  f.header->hide();
  for (int i = 1; i < f.header->count(); ++i) {
    f.policy->setSectionWidth(i, kSeedWidth);
  }
  QApplication::processEvents();

  f.table.insertColumn(3);
  QApplication::processEvents();

  EXPECT_EQ(f.total(), f.table.viewport()->width());
  EXPECT_GE(f.header->sectionSize(3), kAbsoluteFloor);
  EXPECT_LE(f.header->sectionSize(3), f.header->defaultSectionSize());
  EXPECT_GE(f.header->sectionSize(0), kAbsoluteFloor);
}

// A data grid (a wide preview): more columns than the view can hold, every one at its label floor. Splitter semantics
// leave no neighbour with width to give, so without a fallback no divider moves at all.
QStringList overflowingLabels() {
  QStringList labels{QStringLiteral("Name")};
  for (int i = 0; i < 8; ++i) {
    labels.push_back(QString::fromLatin1(kWideLabel) + QString::number(i));
  }
  return labels;
}

TEST(HeaderResizePolicy, OverflowingHeaderResizesOnlyTheDraggedColumn) {
  Fixture f(overflowingLabels());
  ASSERT_GT(f.total(), f.header->width()) << "the fixture must overflow the view";
  const QList<int> before = f.widths();

  f.dragDivider(3, 40);

  QList<int> expected = before;
  expected[3] += 40;
  EXPECT_EQ(f.widths(), expected) << "the dragged column alone should grow; the scrollbar takes the rest";

  f.dragDivider(3, -40);
  EXPECT_EQ(f.widths(), before) << "narrowing back should restore the layout";
}

TEST(HeaderResizePolicy, OverflowingHeaderStillHonoursTheLabelFloor) {
  Fixture f(overflowingLabels());
  const QList<int> before = f.widths();

  f.header->resizeSection(3, kAbsoluteFloor);  // a real drag bottoms out at minimumSectionSize

  EXPECT_EQ(f.header->sectionSize(3), before[3]) << "a column at its floor must not be squashed under its label";
}

TEST(HeaderResizePolicy, OverflowingHeaderLetsTheLastColumnGrow) {
  Fixture f(overflowingLabels());
  const int last = f.header->count() - 1;
  const int before = f.header->sectionSize(last);

  f.dragDivider(last, 50);

  EXPECT_EQ(f.header->sectionSize(last), before + 50);
}

TEST(HeaderResizePolicy, OverflowingFillColumnKeepsItsWidthAcrossAResize) {
  // The fill column normally absorbs slack and is shrunk back on every resize; once the user sizes it in a grid that
  // cannot fit anyway, that width survives the next resize instead of snapping to the floor.
  Fixture f(overflowingLabels());
  const int widened = f.header->sectionSize(0) + 60;

  f.dragDivider(0, 60);
  EXPECT_EQ(f.header->sectionSize(0), widened);

  f.table.resize(kTableWidth + 20, kTableHeight);
  QApplication::processEvents();
  EXPECT_EQ(f.header->sectionSize(0), widened);
}

TEST(HeaderResizePolicy, FillColumnAbsorbsSlackAgainOnceTheGridFits) {
  Fixture f(overflowingLabels());
  f.dragDivider(0, 60);

  // Wide enough for every column: the fill column goes back to filling the view.
  f.table.resize(f.total() + 400, kTableHeight);
  QApplication::processEvents();
  EXPECT_EQ(f.total(), f.header->width());

  // ... and the user's width is released, so overflowing again shrinks it to its floor.
  f.table.resize(kTableWidth, kTableHeight);
  QApplication::processEvents();
  EXPECT_EQ(f.header->sectionSize(0), std::max(kAbsoluteFloor, expectedFloor(f.header, QStringLiteral("Name"))));
}

// The curve list hides its Values column by default. A hidden section keeps a stored size Qt still counts in length(),
// so the fill is sized from the visible sections only, or it stops short of the view.
TEST(HeaderResizePolicy, HiddenLastColumnLeavesTheFillSpanningTheView) {
  Fixture f({QStringLiteral("Name"), QStringLiteral("Values")});
  f.policy->setSectionWidth(1, 60);

  f.header->hideSection(1);
  f.policy->rebalance();

  EXPECT_EQ(f.header->sectionSize(0), f.header->width());
}

// A narrowing drag that turns an overflowing grid into one that fits stops at the exact fit: past it the splitter takes
// over, so the freed width goes to the next column and the divider stays under the cursor, instead of the fill column
// on the left absorbing it.
TEST(HeaderResizePolicy, NarrowingIntoAFittingGridHandsOverToTheSplitter) {
  Fixture f(overflowingLabels());
  f.dragDivider(3, 40);  // give column 3 room above its floor
  const int overflow = f.total() - f.header->width();
  f.table.resize(f.table.width() + overflow - 10, kTableHeight);  // now overflowing by 10 px
  QApplication::processEvents();
  ASSERT_EQ(f.total() - f.header->width(), 10);
  const int start = f.header->sectionSize(3);
  const int fill = f.header->sectionSize(0);

  f.header->resizeSection(3, start - 40);  // one mouse move, past the fit point
  EXPECT_EQ(f.total(), f.header->width());
  EXPECT_EQ(f.header->sectionSize(0), fill) << "the fill column must not absorb the drag";

  f.header->resizeSection(3, start - 40);  // the next move: now under splitter semantics
  EXPECT_EQ(f.header->sectionSize(3), start - 40) << "the divider must follow the cursor";
  EXPECT_EQ(f.header->sectionSize(0), fill);
  EXPECT_EQ(f.total(), f.header->width());
}

TEST(HeaderResizePolicy, UserFillWidthStaysWithTheColumnTheUserSized) {
  Fixture f(overflowingLabels());
  const int column1 = f.header->sectionSize(1);
  f.dragDivider(0, column1 + 80);  // wider than column 1, so leaking the pin would widen it

  f.policy->setFillSection(1);

  EXPECT_EQ(f.header->sectionSize(1), column1) << "a column the user never sized must not inherit that width";
}

// A log-style view: the fill column (Message) last, the hidden ones in the middle.
struct HidingFixture {
  static constexpr int kFill = 5;
  QStandardItemModel model{3, 6};
  int seeded = 90;  // raised above every seeded label's floor, which depends on the platform plugin's font
  QHeaderView* header = nullptr;
  QAbstractItemView* view = nullptr;
  HeaderResizePolicy* policy = nullptr;

  template <typename View>
  void setUp(View& v, QHeaderView* h) {
    model.setHorizontalHeaderLabels({"Time", "Level", "Category", "Topic", "Source", "Message"});
    v.setModel(&model);
    v.resize(kTableWidth, kTableHeight);
    v.show();
    view = &v;
    header = h;
    policy = HeaderResizePolicy::install(header, kFill, kAbsoluteFloor);
    for (int i = 0; i < kFill; ++i) {
      seeded = std::max(seeded, expectedFloor(header, model.horizontalHeaderItem(i)->text()));
    }
    for (int i = 0; i < kFill; ++i) {
      policy->setSectionWidth(i, seeded);
    }
    QApplication::processEvents();
    // expectNoGap() assumes the columns fit: past the viewport the policy overflows like a stock header.
    ASSERT_LE(
        (kFill * seeded) + expectedFloor(header, model.horizontalHeaderItem(kFill)->text()), v.viewport()->width());
  }

  /// Visible sections tile the viewport: each starts where the previous ended, and together they span it exactly.
  void expectNoGap() const {
    int expected_position = 0;
    for (int visual = 0; visual < header->count(); ++visual) {
      const int logical = header->logicalIndex(visual);
      if (header->isSectionHidden(logical)) {
        continue;
      }
      EXPECT_EQ(header->sectionViewportPosition(logical), expected_position) << "gap before column " << logical;
      expected_position += header->sectionSize(logical);
    }
    EXPECT_EQ(expected_position, view->viewport()->width());
    EXPECT_EQ(header->length(), view->viewport()->width()) << "the header length must not count hidden sections";
  }
};

template <typename View>
void checkHidingAndShowingLeavesNoGap(View& v, QHeaderView* h) {
  HidingFixture f;
  f.setUp(v, h);

  f.header->setSectionHidden(2, true);
  f.header->setSectionHidden(3, true);
  QApplication::processEvents();
  f.expectNoGap();

  f.header->setSectionHidden(2, false);
  QApplication::processEvents();
  EXPECT_EQ(f.header->sectionSize(2), f.seeded) << "a shown column should get back its width";
  EXPECT_EQ(f.header->sectionSize(4), f.seeded) << "only the fill column should give it room";
  f.expectNoGap();
}

TEST(HeaderResizePolicy, HiddenColumnsLeaveNoGapInATreeView) {
  QTreeView view;
  checkHidingAndShowingLeavesNoGap(view, view.header());
}

TEST(HeaderResizePolicy, HiddenColumnsLeaveNoGapInATableView) {
  QTableView view;
  checkHidingAndShowingLeavesNoGap(view, view.horizontalHeader());
}

TEST(HeaderResizePolicy, HidingTheFillColumnHandsTheSlackOver) {
  QTreeView view;
  HidingFixture f;
  f.setUp(view, view.header());

  f.header->setSectionHidden(HidingFixture::kFill, true);
  QApplication::processEvents();
  f.expectNoGap();

  f.header->setSectionHidden(HidingFixture::kFill, false);
  QApplication::processEvents();
  EXPECT_EQ(f.header->sectionSize(0), f.seeded) << "the stand-in fill should give the width back";
  f.expectNoGap();
}

TEST(HeaderResizePolicy, ShowingTheLastColumnGivesItItsWidth) {
  QTreeView view;
  HidingFixture f;
  f.setUp(view, view.header());
  f.policy->setFillSection(0);
  const int width = f.header->sectionSize(HidingFixture::kFill);
  f.header->setSectionHidden(HidingFixture::kFill, true);
  QApplication::processEvents();

  f.header->setSectionHidden(HidingFixture::kFill, false);
  QApplication::processEvents();
  EXPECT_EQ(f.header->sectionSize(HidingFixture::kFill), width);
  f.expectNoGap();
}

}  // namespace
}  // namespace PJ
