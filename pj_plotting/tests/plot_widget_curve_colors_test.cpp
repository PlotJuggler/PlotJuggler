// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>

#include <QColor>
#include <QDomDocument>
#include <QPen>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

// These tests pin the intended colour rule, a deliberate design and not an accident of the implementation: a new curve
// takes a palette colour that no curve of its plot uses yet (a layout-restored or user-picked curve counts);
// once every palette colour is in use it takes the least-used one, lowest index first, so colours repeat evenly instead
// of all landing on one.

namespace {

constexpr auto kGlobalColorKey = "Preferences::curve_color_global";

QColor palette(int index) {
  return PJ::PlotWidgetBase::paletteColor(index);
}

// "#rrggbb", so a failing comparison prints readable colours.
std::string hex(const QColor& color) {
  return color.name().toStdString();
}

std::string colorOf(const PJ::PlotWidget::CurveInfo* info) {
  return info != nullptr && info->curve != nullptr ? hex(info->curve->pen().color()) : std::string();
}

// The palette is a code constant: measure its period instead of repeating it here.
int paletteSize() {
  int size = 1;
  while (palette(size) != palette(0)) {
    ++size;
  }
  return size;
}

std::vector<std::string> colorsOf(const PJ::PlotWidget& plot) {
  std::vector<std::string> colors;
  for (const auto& info : plot.curveList()) {
    colors.push_back(colorOf(&info));
  }
  return colors;
}

// Pins the rotation once the palette is exhausted, for a plot holding two laps of curves: the curves after the first
// lap do not repeat one another, each lap hands the colours out in palette order, and no colour is used more often
// than the plot has laps.
void expectEvenRotation(const std::vector<std::string>& colors) {
  const auto size = static_cast<std::size_t>(paletteSize());
  ASSERT_EQ(colors.size(), 2 * size);
  EXPECT_NE(colors[size + 1], colors[size]);  // the first two curves past the palette
  for (std::size_t i = 0; i < size; ++i) {
    EXPECT_EQ(colors[i], hex(palette(static_cast<int>(i)))) << "first lap, curve " << i;
    EXPECT_EQ(colors[size + i], hex(palette(static_cast<int>(i)))) << "second lap, curve " << i;
  }
  std::map<std::string, int> uses;
  for (const std::string& color : colors) {
    ++uses[color];
  }
  for (const auto& [color, count] : uses) {
    EXPECT_LE(count, 2) << color;
  }
}

// Pins "Preferences::curve_color_global" for one test and puts the stored value back afterwards.
class ColorModeGuard {
 public:
  explicit ColorModeGuard(bool global) {
    QSettings settings;
    had_value_ = settings.contains(QLatin1String(kGlobalColorKey));
    previous_ = settings.value(QLatin1String(kGlobalColorKey));
    settings.setValue(QLatin1String(kGlobalColorKey), global);
  }
  ~ColorModeGuard() {
    QSettings settings;
    if (had_value_) {
      settings.setValue(QLatin1String(kGlobalColorKey), previous_);
    } else {
      settings.remove(QLatin1String(kGlobalColorKey));
    }
  }
  ColorModeGuard(const ColorModeGuard&) = delete;
  ColorModeGuard& operator=(const ColorModeGuard&) = delete;

 private:
  bool had_value_ = false;
  QVariant previous_;
};

// One dataset with `count` scalar topics; keys()[i] is the catalog key of topic "/s<i>".
class ColorFixture {
 public:
  explicit ColorFixture(int count) : catalog_(&session_) {
    auto dataset = session_.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
    EXPECT_TRUE(dataset.has_value());
    for (int i = 0; i < count; ++i) {
      keys_.push_back(addTopic(*dataset, "/s" + std::to_string(i)));
    }
  }

  PJ::SessionManager& session() {
    return session_;
  }
  PJ::CatalogModel& catalog() {
    return catalog_;
  }
  [[nodiscard]] const QStringList& keys() const {
    return keys_;
  }

 private:
  QString addTopic(PJ::DatasetId dataset, std::string_view name) {
    PJ::DataWriter writer = session_.dataEngine().createWriter();
    auto handle = writer.registerScalarSeries(dataset, name, PJ::NumericType::kFloat64);
    EXPECT_TRUE(handle.has_value());
    if (!handle.has_value()) {
      return {};
    }
    writer.appendScalar(*handle, 100, 1.0);
    writer.appendScalar(*handle, 200, 2.0);
    EXPECT_FALSE(session_.commitChunks(writer.flushAll()).empty());
    for (const auto& curve : catalog_.curves()) {
      if (const auto descriptor = catalog_.curveDescriptor(curve.name);
          descriptor && descriptor->topic_id == handle->topic_id) {
        return curve.name;
      }
    }
    return {};
  }

  PJ::SessionManager session_;
  PJ::CatalogModel catalog_;
  QStringList keys_;
};

}  // namespace

// A layout restore brings its curves back with explicit colours. A curve added afterwards must not repeat one: the
// session counter knows nothing of colours a restore (or the user) put on the plot.
TEST(PlotWidgetCurveColors, NewCurveSkipsColoursRestoredFromALayout) {
  ColorFixture fixture(3);
  const ColorModeGuard mode(/*global=*/true);

  PJ::PlotWidget saved(&fixture.session(), &fixture.catalog());
  ASSERT_NE(saved.addCurve(fixture.keys()[0], palette(0)), nullptr);
  ASSERT_NE(saved.addCurve(fixture.keys()[1], palette(1)), nullptr);
  QDomDocument doc;
  QDomElement plot_element = saved.xmlSaveState(doc);
  saved.stampLiveCurveKeys(plot_element);

  PJ::PlotWidget restored(&fixture.session(), &fixture.catalog());
  ASSERT_TRUE(restored.xmlLoadState(plot_element));
  ASSERT_EQ(restored.curveList().size(), 2U);

  const auto* added = restored.addCurve(fixture.keys()[2]);
  ASSERT_NE(added, nullptr);
  EXPECT_NE(colorOf(added), hex(palette(0)));
  EXPECT_NE(colorOf(added), hex(palette(1)));
  EXPECT_EQ(colorOf(added), hex(palette(2)));
}

// A colour the user picked is as taken as an auto-assigned one.
TEST(PlotWidgetCurveColors, NewCurveSkipsAColourTheUserPicked) {
  ColorFixture fixture(3);
  const ColorModeGuard mode(/*global=*/true);

  PJ::PlotWidget plot(&fixture.session(), &fixture.catalog());
  ASSERT_NE(plot.addCurve(fixture.keys()[0]), nullptr);  // palette(0), the counter now points at 1
  plot.onChangeCurveColor(fixture.keys()[0], palette(1));

  const auto* added = plot.addCurve(fixture.keys()[1]);
  ASSERT_NE(added, nullptr);
  EXPECT_NE(colorOf(added), hex(palette(1)));
  EXPECT_EQ(colorOf(added), hex(palette(2)));
}

// "Per plot" colours restart at the first colour of every plot, but skip those the plot already uses: after a restore
// with palette(2) and palette(3), the plot's positional index would land on palette(2).
TEST(PlotWidgetCurveColors, PerPlotModeSkipsRestoredColours) {
  ColorFixture fixture(3);
  const ColorModeGuard mode(/*global=*/false);

  PJ::PlotWidget plot(&fixture.session(), &fixture.catalog());
  ASSERT_NE(plot.addCurve(fixture.keys()[0], palette(2)), nullptr);
  ASSERT_NE(plot.addCurve(fixture.keys()[1], palette(3)), nullptr);

  const auto* added = plot.addCurve(fixture.keys()[2]);
  ASSERT_NE(added, nullptr);
  EXPECT_NE(colorOf(added), hex(palette(2)));
  EXPECT_NE(colorOf(added), hex(palette(3)));
}

// Removing a curve from the middle must not make the next one collide with the curve that is left holding its slot.
TEST(PlotWidgetCurveColors, PerPlotModeAfterARemovalKeepsColoursDistinct) {
  ColorFixture fixture(4);
  const ColorModeGuard mode(/*global=*/false);

  PJ::PlotWidget plot(&fixture.session(), &fixture.catalog());
  for (int i = 0; i < 3; ++i) {
    ASSERT_NE(plot.addCurve(fixture.keys()[i]), nullptr);
  }
  plot.removeCurve(fixture.keys()[0]);

  const auto* added = plot.addCurve(fixture.keys()[3]);
  ASSERT_NE(added, nullptr);
  std::set<std::string> seen;
  for (const auto& info : plot.curveList()) {
    EXPECT_TRUE(seen.insert(colorOf(&info)).second) << info.source_name.toStdString();
  }
}

// The XY path picks its colour with PlotWidgetBase::nextColor(), independent of the session counter.
TEST(PlotWidgetCurveColors, NewXYCurveSkipsColoursAlreadyOnThePlot) {
  ColorFixture fixture(4);

  PJ::PlotWidget plot(&fixture.session(), &fixture.catalog());
  plot.setModeXY(true);
  ASSERT_NE(plot.addCurveXY(fixture.keys()[0], fixture.keys()[1], u"first"_s, palette(0)), nullptr);

  const auto* added = plot.addCurveXY(fixture.keys()[2], fixture.keys()[3], u"second"_s);
  ASSERT_NE(added, nullptr);
  EXPECT_NE(colorOf(added), hex(palette(0)));
  EXPECT_EQ(colorOf(added), hex(palette(1)));
}

// What already worked: a curve removed and added again keeps its remembered colour, whatever the plot holds meanwhile.
TEST(PlotWidgetCurveColors, ReAddedCurveKeepsItsRememberedColour) {
  ColorFixture fixture(2);
  const ColorModeGuard mode(/*global=*/true);

  PJ::PlotWidget plot(&fixture.session(), &fixture.catalog());
  ASSERT_NE(plot.addCurve(fixture.keys()[0]), nullptr);
  const std::string first = colorOf(&plot.curveList().front());
  plot.removeCurve(fixture.keys()[0]);
  ASSERT_NE(plot.addCurve(fixture.keys()[1]), nullptr);

  const auto* again = plot.addCurve(fixture.keys()[0]);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(colorOf(again), first);
}

// Every palette colour in use: curves keep rotating through the palette in order, in the session-wide sequence.
TEST(PlotWidgetCurveColors, ExhaustedPaletteRotatesEvenlyInGlobalMode) {
  ColorFixture fixture(2 * paletteSize());
  const ColorModeGuard mode(/*global=*/true);

  PJ::PlotWidget plot(&fixture.session(), &fixture.catalog());
  for (const QString& key : fixture.keys()) {
    ASSERT_NE(plot.addCurve(key), nullptr);
  }
  expectEvenRotation(colorsOf(plot));
}

TEST(PlotWidgetCurveColors, ExhaustedPaletteRotatesEvenlyInPerPlotMode) {
  ColorFixture fixture(2 * paletteSize());
  const ColorModeGuard mode(/*global=*/false);

  PJ::PlotWidget plot(&fixture.session(), &fixture.catalog());
  for (const QString& key : fixture.keys()) {
    ASSERT_NE(plot.addCurve(key), nullptr);
  }
  expectEvenRotation(colorsOf(plot));
}

TEST(PlotWidgetCurveColors, ExhaustedPaletteRotatesEvenlyInAnXYPlot) {
  ColorFixture fixture(2);

  PJ::PlotWidget plot(&fixture.session(), &fixture.catalog());
  plot.setModeXY(true);
  for (int i = 0; i < 2 * paletteSize(); ++i) {
    ASSERT_NE(plot.addCurveXY(fixture.keys()[0], fixture.keys()[1], u"xy%1"_s.arg(i)), nullptr) << i;
  }
  expectEvenRotation(colorsOf(plot));
}

// With palette(5) and palette(6) on the plot twice and every other palette colour once, the added curve takes the
// lowest of the least-used colours, not the one its position in the plot would give.
TEST(PlotWidgetCurveColors, ExhaustedPaletteTakesTheLowestOfTheLeastUsedColours) {
  ASSERT_GE(paletteSize(), 7);
  ColorFixture fixture(paletteSize() + 3);
  const ColorModeGuard mode(/*global=*/false);

  PJ::PlotWidget plot(&fixture.session(), &fixture.catalog());
  int next_key = 0;
  for (int slot = 0; slot < paletteSize(); ++slot) {
    ASSERT_NE(plot.addCurve(fixture.keys()[next_key++], palette(slot)), nullptr);
  }
  ASSERT_NE(plot.addCurve(fixture.keys()[next_key++], palette(5)), nullptr);
  ASSERT_NE(plot.addCurve(fixture.keys()[next_key++], palette(6)), nullptr);

  const auto* added = plot.addCurve(fixture.keys()[next_key]);
  ASSERT_NE(added, nullptr);
  EXPECT_EQ(colorOf(added), hex(palette(0)));
}
