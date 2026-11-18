// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A layout's streaming source and buffer restore into the combo and the spin box. StreamingSourceManager learns
// them only from LeftPanel's signals, so the restore must emit them too, or Start launches a source the combo does
// not show and the session keeps another window than the spin box shows. It must still leave QSettings alone.

#include <gtest/gtest.h>

#include <QComboBox>
#include <QDomDocument>
#include <QSettings>
#include <QSignalSpy>

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

}  // namespace

TEST(LeftPanelStreamingTest, RestoredSourceAndBufferReachTheStreamingManager) {
  QSettings().clear();
  PJ::LeftPanel panel;
  panel.setStreamingSources({u"Dummy Streamer"_s, u"UDP Server"_s});
  auto* combo = panel.findChild<QComboBox*>(u"comboStreaming"_s);
  ASSERT_NE(combo, nullptr);
  ASSERT_EQ(combo->currentText(), u"Dummy Streamer"_s);

  QSignalSpy source_spy(&panel, &PJ::LeftPanel::streamingSourceChanged);
  QSignalSpy buffer_spy(&panel, &PJ::LeftPanel::streamingBufferChanged);
  QDomDocument doc;
  panel.restoreSourcesState(leftPanelState(doc, u"UDP Server"_s, u"20"_s));

  EXPECT_EQ(combo->currentText(), u"UDP Server"_s);
  ASSERT_EQ(source_spy.count(), 1) << "the manager's selected source would stay on the earlier pick";
  EXPECT_EQ(source_spy.takeFirst().at(0).toString(), u"UDP Server"_s);
  ASSERT_EQ(buffer_spy.count(), 1) << "the manager's retention would stay on the QSettings value";
  EXPECT_EQ(buffer_spy.takeFirst().at(0).toInt(), 20);

  EXPECT_FALSE(QSettings().contains(u"MainWindow.streamingSource"_s)) << "a layout must not change the defaults";
  EXPECT_FALSE(QSettings().contains(u"MainWindow.streamingBufferValue"_s)) << "a layout must not change the defaults";
}

TEST(LeftPanelStreamingTest, UnknownSourceOrBadBufferEmitsNothing) {
  QSettings().clear();
  PJ::LeftPanel panel;
  panel.setStreamingSources({u"Dummy Streamer"_s});

  QSignalSpy source_spy(&panel, &PJ::LeftPanel::streamingSourceChanged);
  QSignalSpy buffer_spy(&panel, &PJ::LeftPanel::streamingBufferChanged);
  QDomDocument doc;
  panel.restoreSourcesState(leftPanelState(doc, u"Not Installed"_s, u"abc"_s));

  EXPECT_EQ(source_spy.count(), 0);
  EXPECT_EQ(buffer_spy.count(), 0);
}
