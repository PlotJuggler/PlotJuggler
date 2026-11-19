// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Start starts the source the combo shows, whoever set it (a user, a repopulation, a layout), and only a user's pick
// becomes the remembered default. A restored streaming buffer reaches the streams through streamingBufferChanged.
// Which buffer a layout brings is MainWindow's decision, covered in streaming_preset_test.cpp.

#include <gtest/gtest.h>

#include <QComboBox>
#include <QDomDocument>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <optional>

#include "pj_widgets/IntScrubber.h"
#include "ui/LeftPanel.h"

using namespace Qt::StringLiterals;

namespace {

QDomElement leftPanelState(QDomDocument& doc, const QString& source) {
  QDomElement element = doc.createElement(u"left_panel_state"_s);
  element.setAttribute(u"sources_tab"_s, u"stream"_s);
  element.setAttribute(u"streaming_source"_s, source);
  return element;
}

QString startedSource(PJ::LeftPanel& panel) {
  QSignalSpy start_spy(&panel, &PJ::LeftPanel::streamingStartRequested);
  auto* start = panel.findChild<QPushButton*>(u"buttonStreamingOptions"_s);
  if (start == nullptr) {
    return u"<no Start button>"_s;
  }
  start->click();
  return start_spy.count() == 1 ? start_spy.first().at(0).toString() : u"<no start>"_s;
}

int spinBoxValue(const PJ::LeftPanel& panel) {
  const auto* spin = panel.findChild<PJ::IntScrubber*>(u"streamingSpinBox"_s);
  return spin != nullptr ? spin->value() : -1;
}

}  // namespace

TEST(LeftPanelStreamingTest, StartStartsTheRestoredSource) {
  QSettings().clear();
  PJ::LeftPanel panel;
  panel.setStreamingSources({u"Dummy Streamer"_s, u"UDP Server"_s});
  ASSERT_EQ(startedSource(panel), u"Dummy Streamer"_s);

  QDomDocument doc;
  panel.restoreSourcesState(leftPanelState(doc, u"UDP Server"_s), std::nullopt);
  EXPECT_EQ(startedSource(panel), u"UDP Server"_s);
  EXPECT_FALSE(QSettings().contains(u"MainWindow.streamingSource"_s)) << "a layout must not change the defaults";

  panel.restoreSourcesState(leftPanelState(doc, u"Not Installed"_s), std::nullopt);
  EXPECT_EQ(startedSource(panel), u"UDP Server"_s);
}

TEST(LeftPanelStreamingTest, OnlyAUserPickBecomesTheDefault) {
  QSettings().clear();
  PJ::LeftPanel panel;
  panel.setStreamingSources({u"Dummy Streamer"_s, u"UDP Server"_s});
  EXPECT_FALSE(QSettings().contains(u"MainWindow.streamingSource"_s));

  auto* combo = panel.findChild<QComboBox*>(u"comboStreaming"_s);
  ASSERT_NE(combo, nullptr);
  combo->setCurrentIndex(1);
  emit combo->textActivated(combo->currentText());  // what QComboBox emits for a user's pick
  EXPECT_EQ(QSettings().value(u"MainWindow.streamingSource"_s).toString(), u"UDP Server"_s);
}

TEST(LeftPanelStreamingTest, RestoredBufferReachesTheStreams) {
  QSettings().clear();
  PJ::LeftPanel panel;
  const int before = spinBoxValue(panel);

  QSignalSpy buffer_spy(&panel, &PJ::LeftPanel::streamingBufferChanged);
  QDomDocument doc;
  panel.restoreSourcesState(leftPanelState(doc, u"Dummy Streamer"_s), std::nullopt);
  EXPECT_EQ(buffer_spy.count(), 0);
  EXPECT_EQ(spinBoxValue(panel), before);

  panel.restoreSourcesState(leftPanelState(doc, u"Dummy Streamer"_s), 20);
  EXPECT_EQ(spinBoxValue(panel), 20);
  ASSERT_EQ(buffer_spy.count(), 1);
  EXPECT_EQ(buffer_spy.takeFirst().at(0).toInt(), 20);

  EXPECT_FALSE(panel.saveSourcesState(doc, /*stream_bound=*/false).hasAttribute(u"streaming_buffer"_s));
  EXPECT_EQ(panel.saveSourcesState(doc, /*stream_bound=*/true).attribute(u"streaming_buffer"_s), u"20"_s);
}
