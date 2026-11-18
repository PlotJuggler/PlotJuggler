// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A layout's streaming source and buffer restore into the combo and the spin box, and the restore emits what it
// applied so StreamingSourceManager follows the controls. The buffer belongs only to a stream-bound layout, and a
// load never shrinks the window of a stream that is running. MainWindow-level coverage (live sessions, an aborted
// progressive restore, saving) is in streaming_preset_test.cpp.

#include <gtest/gtest.h>

#include <QComboBox>
#include <QDomDocument>
#include <QSettings>
#include <QSignalSpy>

#include "pj_widgets/IntScrubber.h"
#include "ui/LeftPanel.h"

using namespace Qt::StringLiterals;

namespace {

QDomElement leftPanelState(QDomDocument& doc, const QString& source, const QString& buffer) {
  QDomElement element = doc.createElement(u"left_panel_state"_s);
  element.setAttribute(u"sources_tab"_s, u"stream"_s);
  element.setAttribute(u"streaming_source"_s, source);
  element.setAttribute(u"streaming_buffer"_s, buffer);
  return element;
}

PJ::LeftPanel::StreamingRestore streamBound(std::optional<int> live_retention_s = std::nullopt) {
  return {.stream_bound = true, .live_retention_s = live_retention_s};
}

int spinBoxValue(const PJ::LeftPanel& panel) {
  const auto* spin = panel.findChild<PJ::IntScrubber*>(u"streamingSpinBox"_s);
  return spin != nullptr ? spin->value() : -1;
}

}  // namespace

TEST(LeftPanelStreamingTest, RestoreEmitsTheRestoredSourceAndBuffer) {
  QSettings().clear();
  PJ::LeftPanel panel;
  panel.setStreamingSources({u"Dummy Streamer"_s, u"UDP Server"_s});
  auto* combo = panel.findChild<QComboBox*>(u"comboStreaming"_s);
  ASSERT_NE(combo, nullptr);
  ASSERT_EQ(combo->currentText(), u"Dummy Streamer"_s);

  QSignalSpy source_spy(&panel, &PJ::LeftPanel::streamingSourceChanged);
  QSignalSpy buffer_spy(&panel, &PJ::LeftPanel::streamingBufferChanged);
  QDomDocument doc;
  panel.restoreSourcesState(leftPanelState(doc, u"UDP Server"_s, u"20"_s), streamBound());

  EXPECT_EQ(combo->currentText(), u"UDP Server"_s);
  ASSERT_EQ(source_spy.count(), 1) << "the manager's selected source would stay on the earlier pick";
  EXPECT_EQ(source_spy.takeFirst().at(0).toString(), u"UDP Server"_s);
  ASSERT_EQ(buffer_spy.count(), 1) << "the manager's retention would stay on the QSettings value";
  EXPECT_EQ(buffer_spy.takeFirst().at(0).toInt(), 20);
  EXPECT_EQ(spinBoxValue(panel), 20);
  EXPECT_FALSE(QSettings().contains(u"MainWindow.streamingSource"_s)) << "a layout must not change the defaults";
}

TEST(LeftPanelStreamingTest, UnknownSourceOrBadBufferEmitsNothing) {
  QSettings().clear();
  PJ::LeftPanel panel;
  panel.setStreamingSources({u"Dummy Streamer"_s});

  QSignalSpy source_spy(&panel, &PJ::LeftPanel::streamingSourceChanged);
  QSignalSpy buffer_spy(&panel, &PJ::LeftPanel::streamingBufferChanged);
  QDomDocument doc;
  panel.restoreSourcesState(leftPanelState(doc, u"Not Installed"_s, u"abc"_s), streamBound());

  EXPECT_EQ(source_spy.count(), 0);
  EXPECT_EQ(buffer_spy.count(), 0);
}

// A file-only layout, old ones that still carry the attribute included, neither touches the buffer on load nor
// saves it.
TEST(LeftPanelStreamingTest, FileOnlyLayoutLeavesTheBufferAlone) {
  QSettings().clear();
  PJ::LeftPanel panel;
  panel.setStreamingSources({u"Dummy Streamer"_s});
  const int before = spinBoxValue(panel);

  QSignalSpy buffer_spy(&panel, &PJ::LeftPanel::streamingBufferChanged);
  QDomDocument doc;
  panel.restoreSourcesState(
      leftPanelState(doc, u"Dummy Streamer"_s, u"42"_s), {.stream_bound = false, .live_retention_s = std::nullopt});
  EXPECT_EQ(buffer_spy.count(), 0);
  EXPECT_EQ(spinBoxValue(panel), before);

  EXPECT_FALSE(panel.saveSourcesState(doc, /*stream_bound=*/false).hasAttribute(u"streaming_buffer"_s));
  EXPECT_EQ(
      panel.saveSourcesState(doc, /*stream_bound=*/true).attribute(u"streaming_buffer"_s), QString::number(before));
}

// A running stream keeps its wider window, and the spin box shows that window; a wider buffer still applies.
TEST(LeftPanelStreamingTest, BufferNeverShrinksARunningStream) {
  QSettings().clear();
  PJ::LeftPanel panel;
  panel.setStreamingSources({u"Dummy Streamer"_s});

  QSignalSpy buffer_spy(&panel, &PJ::LeftPanel::streamingBufferChanged);
  QDomDocument doc;
  panel.restoreSourcesState(leftPanelState(doc, u"Dummy Streamer"_s, u"5"_s), streamBound(60));
  EXPECT_EQ(spinBoxValue(panel), 60);
  for (const QList<QVariant>& emitted : buffer_spy) {
    EXPECT_EQ(emitted.at(0).toInt(), 60) << "the running window must not shrink";
  }

  buffer_spy.clear();
  panel.restoreSourcesState(leftPanelState(doc, u"Dummy Streamer"_s, u"90"_s), streamBound(60));
  EXPECT_EQ(spinBoxValue(panel), 90);
  ASSERT_EQ(buffer_spy.count(), 1);
  EXPECT_EQ(buffer_spy.takeFirst().at(0).toInt(), 90);
}
