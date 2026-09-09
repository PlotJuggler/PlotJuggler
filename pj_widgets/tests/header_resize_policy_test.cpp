// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QFontMetrics>
#include <QHeaderView>
#include <QTableWidget>
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

  /// Simulates a user drag of the divider on @p section's right edge. Qt applies
  /// the new size first and the policy reacts to the signal, exactly as during a
  /// real drag.
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

TEST(HeaderResizePolicy, SeedingWidthsDoesNotStarveLaterColumns) {
  // The regression this guards: seeding through QHeaderView::resizeSection is
  // read back as a user drag, so each seed steals from the next column and the
  // rightmost ones end up under their labels.
  Fixture f({"Channel name", "Schema", "Encoding", "Msg Count"});
  // Seed above every label floor: the floor depends on the platform plugin's
  // font (offscreen on Windows renders these labels wider than 96 px), and this
  // test is about seeds not stealing from neighbours, not about the floor rule.
  int seed = kSeedWidth;
  for (int i = 1; i < f.header->count(); ++i) {
    const QString label = f.table.horizontalHeaderItem(i)->text();
    seed = std::max(seed, expectedFloor(f.header, label));
  }
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
  f.policy->setSectionWidth(2, kTableWidth / 2);
  const QList<int> before = f.widths();

  f.dragDivider(0, 40);

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

}  // namespace
}  // namespace PJ
