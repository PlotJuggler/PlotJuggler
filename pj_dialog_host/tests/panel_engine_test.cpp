// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <pj_plotting/PlotWidget.h>
#include <pj_runtime/AppSession.h>
#include <pj_widgets/Dialog.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QEventLoop>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QWidget>
#include <memory>
#include <pj_plugins/host/dialog_handle.hpp>
#include <pj_plugins/host_qt/chart_preview_widget.hpp>
#include <pj_plugins/host_qt/panel_engine.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string_view>

#include "mock_panel_plugin.hpp"
using namespace Qt::StringLiterals;

// Defined in mock_panel_plugin.cpp via PJ_DIALOG_PLUGIN(MockPanelPlugin).
extern "C" const PJ_dialog_vtable_t* PJ_get_dialog_vtable() noexcept;

namespace {

// QApplication must exist before any QWidget is built. Created once per
// test executable and torn down at exit.
QApplication* qapp() {
  static int argc = 0;
  static QApplication app(argc, nullptr);
  // QSettings needs a non-empty organization/application identity to read and
  // write reliably across platforms: with an empty identity Windows returns
  // QSettings::AccessError, so writes are dropped and reads fall back to the
  // default. ThemeChangeReappliesWidgetData toggles the theme through QSettings,
  // so without this its toggle is a silent no-op on Windows (passes on Linux,
  // which is file-backed and lenient). Give the test process a stable identity.
  static const bool kIdentitySet = [] {
    QCoreApplication::setOrganizationName(u"PlotJugglerTest"_s);
    QCoreApplication::setApplicationName(u"panel_engine_test"_s);
    return true;
  }();
  (void)kIdentitySet;
  return &app;
}

// Pump the event loop for `ms` milliseconds to let timers and queued
// connections fire.
void pumpEventLoop(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

PJ::DialogHandle makeMockHandle() {
  return PJ::DialogHandle(PJ_get_dialog_vtable());
}

constexpr int kPreferredSubPanelWidth = 1200;
constexpr int kPreferredSubPanelHeight = 900;

constexpr char kSizedSubPanelUi[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>SizedSubPanel</class>
 <widget class="QWidget" name="sizedSubPanel">
  <property name="geometry">
   <rect><x>0</x><y>0</y><width>1200</width><height>900</height></rect>
  </property>
  <property name="minimumSize">
   <size><width>500</width><height>400</height></size>
  </property>
  <property name="windowTitle"><string>Sized sub-panel</string></property>
  <layout class="QVBoxLayout">
   <item>
    <widget class="QLabel" name="preferredSizeContent">
     <property name="minimumSize">
      <size><width>640</width><height>440</height></size>
     </property>
     <property name="text"><string>Large content</string></property>
    </widget>
   </item>
  </layout>
 </widget>
 <resources/>
 <connections/>
</ui>
)";

class SizedSubPanelPlugin final : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return R"({"id":"sized-sub-panel","name":"Sized Sub-panel","version":"0.0.1"})";
  }

  std::string ui_content() const override {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>SizedSubPanelHost</class>
 <widget class="QWidget" name="sizedSubPanelHost">
  <layout class="QVBoxLayout">
   <item>
    <widget class="QPushButton" name="openSubPanel">
     <property name="text"><string>Open</string></property>
    </widget>
   </item>
  </layout>
 </widget>
 <resources/>
 <connections/>
</ui>
)";
  }

  std::string widget_data() override {
    PJ::WidgetData data;
    if (open_sub_panel_) {
      data.requestSubPanel(kSizedSubPanelUi);
      open_sub_panel_ = false;
    }
    return data.toJson();
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name != "openSubPanel") {
      return false;
    }
    open_sub_panel_ = true;
    return true;
  }

 private:
  bool open_sub_panel_ = false;
};

PJ::DialogHandle makeSizedSubPanelHandle() {
  const auto* vtable = PJ::DialogPluginBase::vtableWithCreate([]() noexcept -> void* {
    try {
      return static_cast<PJ::DialogPluginBase*>(new SizedSubPanelPlugin());
    } catch (...) {
      return nullptr;
    }
  });
  return PJ::DialogHandle(vtable);
}

}  // namespace

class PanelEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    qapp();
    resetMockPanelState();
  }
};

TEST_F(PanelEngineTest, OpenPanelReturnsConstructedWidget) {
  PJ::PanelEngine engine(makeMockHandle());
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  // The mock UI has labelHello, textBox, and buttonClose.
  EXPECT_NE(panel->findChild<QLabel*>("labelHello"), nullptr);
  EXPECT_NE(panel->findChild<QLineEdit*>("textBox"), nullptr);
  EXPECT_NE(panel->findChild<QPushButton*>("buttonClose"), nullptr);

  delete panel;
}

TEST_F(PanelEngineTest, InitialWidgetDataIsApplied) {
  mockPanelState().label = "Hello from PanelEngine";
  mockPanelState().text = "preset";

  PJ::PanelEngine engine(makeMockHandle());
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  auto* label = panel->findChild<QLabel*>("labelHello");
  auto* text = panel->findChild<QLineEdit*>("textBox");
  ASSERT_NE(label, nullptr);
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(label->text().toStdString(), "Hello from PanelEngine");
  EXPECT_EQ(text->text().toStdString(), "preset");

  delete panel;
}

TEST_F(PanelEngineTest, ThemeChangeReappliesWidgetData) {
  // Host-themed icons (setButtonIconNamed → loadSvg) are applied once and then
  // diffed away, so a live theme switch must re-apply the panel's last
  // widget-data to re-tint them. The icon color can't be observed here (no
  // resources linked), so we prove the re-apply fires by watching a plain widget
  // value get restored from the plugin's last data on a StyleChange.
  const QString saved_theme = QSettings().value(u"StyleSheet::theme"_s).toString();
  QSettings().setValue(u"StyleSheet::theme"_s, u"light"_s);

  mockPanelState().label = "FromPlugin";
  PJ::PanelEngine engine(makeMockHandle());
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);
  auto* label = panel->findChild<QLabel*>("labelHello");
  ASSERT_NE(label, nullptr);
  ASSERT_EQ(label->text(), u"FromPlugin"_s);

  // Externally clobber the label. A normal tick won't restore it — the plugin's
  // data is unchanged, so the diff is empty — only the theme-change re-apply will.
  label->setText(u"CLOBBERED"_s);

  // Theme changes, then the app re-polishes the panel (StyleChange): the engine
  // re-applies the plugin's last data, restoring the label.
  QSettings().setValue(u"StyleSheet::theme"_s, u"dark"_s);
  QEvent style_change(QEvent::StyleChange);
  QApplication::sendEvent(panel, &style_change);
  EXPECT_EQ(label->text(), u"FromPlugin"_s);

  // A second StyleChange with no further theme change must NOT re-apply (the
  // applied-theme gate prevents redundant re-applies on unrelated polish events).
  label->setText(u"CLOBBERED2"_s);
  QEvent style_change2(QEvent::StyleChange);
  QApplication::sendEvent(panel, &style_change2);
  EXPECT_EQ(label->text(), u"CLOBBERED2"_s);

  if (saved_theme.isEmpty()) {
    QSettings().remove(u"StyleSheet::theme"_s);
  } else {
    QSettings().setValue(u"StyleSheet::theme"_s, saved_theme);
  }
  delete panel;
}

TEST_F(PanelEngineTest, ThemeChangeKeepsSessionBackedChart) {
  const QString saved_theme = QSettings().value(u"StyleSheet::theme"_s).toString();
  QSettings().setValue(u"StyleSheet::theme"_s, u"light"_s);

  PJ::AppSession session;
  mockPanelState().chart_enabled = true;
  PJ::PanelEngineConfig config;
  config.session = &session;
  config.catalog = &session.catalogModel();
  PJ::PanelEngine engine(makeMockHandle(), config);
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  auto* frame = panel->findChild<QFrame*>(u"chartPreview"_s);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(frame->findChildren<PJ::PlotWidget*>().size(), 1);
  ASSERT_TRUE(frame->findChildren<PJ::ChartPreviewWidget*>().isEmpty());

  QSettings().setValue(u"StyleSheet::theme"_s, u"dark"_s);
  QEvent style_change(QEvent::StyleChange);
  QApplication::sendEvent(panel, &style_change);

  EXPECT_EQ(frame->findChildren<PJ::PlotWidget*>().size(), 1);
  EXPECT_TRUE(frame->findChildren<PJ::ChartPreviewWidget*>().isEmpty());

  if (saved_theme.isEmpty()) {
    QSettings().remove(u"StyleSheet::theme"_s);
  } else {
    QSettings().setValue(u"StyleSheet::theme"_s, saved_theme);
  }
  delete panel;
}

TEST_F(PanelEngineTest, TickPropagatesPluginStateChanges) {
  PJ::PanelEngine engine(makeMockHandle(), {.tick_interval_ms = 10, .enable_diff = true, .enable_file_picker = false});
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);
  panel->show();  // hidden panels tick at 1/10 rate; exercise the visible fast path

  // After mutating plugin state, the next tick should refresh the label.
  mockPanelState().label = "Updated";
  pumpEventLoop(50);  // ample for several ticks

  auto* label = panel->findChild<QLabel*>("labelHello");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->text().toStdString(), "Updated");
  EXPECT_GT(engine.stats().tick_count, 0);

  delete panel;
}

TEST_F(PanelEngineTest, RequestCloseFiresCallback) {
  PJ::PanelEngine engine(makeMockHandle(), {.tick_interval_ms = 10, .enable_diff = true, .enable_file_picker = false});
  std::string captured_reason;
  bool fired = false;
  engine.onCloseRequested([&](std::string reason) {
    captured_reason = std::move(reason);
    fired = true;
    return true;
  });

  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);
  panel->show();  // hidden panels tick at 1/10 rate; exercise the visible fast path

  // Ask the plugin to request close on next tick.
  mockPanelState().close_on_next_tick = true;
  mockPanelState().close_reason = "import_complete";

  pumpEventLoop(50);

  EXPECT_TRUE(fired);
  EXPECT_EQ(captured_reason, "import_complete");

  delete panel;
}

// A host that keeps the panel alive past its own batch completion (a pinned tab
// ignoring "import_complete") declines the close, and the session MUST keep
// running. Regression: the engine used to tear itself down regardless of the
// callback, so the kept-open tab stayed on screen as an inert shell — no ticks,
// no widget-data exchange, every click silently dropped, unrecoverable without
// reopening the plugin.
TEST_F(PanelEngineTest, DeclinedCloseKeepsTheSessionRunning) {
  PJ::PanelEngine engine(
      makeMockHandle(), {/*tick_interval_ms=*/10, /*enable_diff=*/true, /*catalog_key_resolver=*/{}});
  int close_requests = 0;
  engine.onCloseRequested([&](std::string /*reason*/) {
    ++close_requests;
    return false;  // the owner keeps this panel open
  });

  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);
  panel->show();

  mockPanelState().close_on_next_tick = true;
  mockPanelState().close_reason = "import_complete";
  pumpEventLoop(50);
  ASSERT_GT(close_requests, 0) << "the plugin never asked to close; test is not exercising the path";

  // The engine must still be ticking AND still applying plugin widget data.
  const int ticks_after_decline = engine.stats().tick_count;
  mockPanelState().close_on_next_tick = false;
  mockPanelState().label = "StillAlive";
  pumpEventLoop(100);

  EXPECT_GT(engine.stats().tick_count, ticks_after_decline) << "engine stopped ticking after a declined close";
  auto* label = panel->findChild<QLabel*>("labelHello");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->text().toStdString(), "StillAlive") << "widget data stopped flowing after a declined close";

  delete panel;
}

TEST_F(PanelEngineTest, HiddenPanelStillTicksAtReducedRate) {
  PJ::PanelEngine engine(makeMockHandle(), {/*tick_interval_ms=*/5, /*enable_diff=*/true, /*catalog_key_resolver=*/{}});
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);
  // Never shown: the plugin's periodic logic must stay alive regardless —
  // a pinned toolbox in a non-current tab keeps fetching/advancing.
  mockPanelState().label = "Updated";
  pumpEventLoop(200);  // 40 timer fires -> at least a few 1/10-rate ticks

  EXPECT_GT(engine.stats().tick_count, 0);
  auto* label = panel->findChild<QLabel*>("labelHello");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->text().toStdString(), "Updated");

  delete panel;
}

TEST_F(PanelEngineTest, ButtonBoxRejectClosesPanel) {
  // The panel's standard QDialogButtonBox (Close) is wired to the close path:
  // connectWidgetSignals skips button-box buttons and the non-modal panel has
  // no QDialog::reject, so without that wiring Close would be inert.
  PJ::PanelEngine engine(makeMockHandle());
  std::string captured_reason;
  bool fired = false;
  engine.onCloseRequested([&](std::string reason) {
    captured_reason = std::move(reason);
    fired = true;
    return true;
  });

  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  auto* button_box = panel->findChild<QDialogButtonBox*>("buttonBox");
  ASSERT_NE(button_box, nullptr);
  auto* close_btn = button_box->button(QDialogButtonBox::Close);
  ASSERT_NE(close_btn, nullptr);
  close_btn->click();

  pumpEventLoop(10);

  EXPECT_TRUE(fired);
  EXPECT_EQ(captured_reason, "closed by user");

  delete panel;
}

TEST_F(PanelEngineTest, WidgetEventReachesPlugin) {
  PJ::PanelEngine engine(makeMockHandle(), {.tick_interval_ms = 10, .enable_diff = true, .enable_file_picker = false});
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  auto* text = panel->findChild<QLineEdit*>("textBox");
  ASSERT_NE(text, nullptr);

  text->setText("typed by test");
  // QLineEdit::textChanged is synchronous; let one tick run to apply diffed
  // state from the plugin back to the widget.
  pumpEventLoop(30);

  EXPECT_EQ(mockPanelState().text, "typed by test");
  EXPECT_GT(engine.stats().event_count, 0);

  delete panel;
}

TEST_F(PanelEngineTest, SubPanelUsesLoadedRootPreferredSize) {
  PJ::PanelEngine engine(makeSizedSubPanelHandle());
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  auto* open_button = panel->findChild<QPushButton*>("openSubPanel");
  ASSERT_NE(open_button, nullptr);
  open_button->click();

  auto* dialog = panel->findChild<PJ::Dialog*>();
  ASSERT_NE(dialog, nullptr);
  auto* loaded_root = dialog->findChild<QWidget*>("sizedSubPanel");
  ASSERT_NE(loaded_root, nullptr);
  EXPECT_GE(loaded_root->minimumWidth(), 500);
  EXPECT_GE(loaded_root->minimumHeight(), 400);
  EXPECT_GE(loaded_root->sizeHint().width(), 640);
  EXPECT_GE(loaded_root->sizeHint().height(), 440);

  ASSERT_NE(dialog->screen(), nullptr);
  const QSize available = dialog->screen()->availableSize();
  const QSize expected = QSize(kPreferredSubPanelWidth, kPreferredSubPanelHeight).boundedTo(available);
  EXPECT_GE(dialog->width(), expected.width());
  EXPECT_GE(dialog->height(), expected.height());

  delete panel;
}

TEST_F(PanelEngineTest, WidgetEventCostsOneWidgetDataBuild) {
  // A forwarded widget event needs exactly ONE widget_data() poll (the
  // applyAndDiff after sendEvent). The per-event file-picker check must reuse
  // that already-fetched document — a second full build + parse per event
  // doubled the cost of every slider-drag tick.
  PJ::PanelEngineConfig config;
  config.tick_interval_ms = 100000;  // keep the tick timer out of the count
  PJ::PanelEngine engine(makeMockHandle(), config);
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  auto* text = panel->findChild<QLineEdit*>("textBox");
  ASSERT_NE(text, nullptr);

  const int baseline = mockPanelState().widget_data_calls;
  text->setText("one event");  // textChanged is synchronous
  EXPECT_EQ(mockPanelState().text, "one event");
  EXPECT_EQ(mockPanelState().widget_data_calls, baseline + 1);

  delete panel;
}

TEST_F(PanelEngineTest, ConfiguredWidgetEventRestartsTickDeadline) {
  PJ::PanelEngineConfig config;
  config.tick_interval_ms = 1000;
  config.restart_tick_timer_on_event = true;
  PJ::PanelEngine engine(makeMockHandle(), config);
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);
  auto* timer = engine.findChild<QTimer*>();
  auto* text = panel->findChild<QLineEdit*>("textBox");
  ASSERT_NE(timer, nullptr);
  ASSERT_NE(text, nullptr);

  // A coarse 1 s QTimer may round every restart down by as much as 5%, so its
  // reported remaining time can stay near 950 ms before and after a real
  // restart. Make only this timing observation precise; production keeps the
  // default timer type and the behavior under test is still PanelEngine's
  // event-driven start().
  timer->stop();
  timer->setTimerType(Qt::PreciseTimer);
  timer->start();
  pumpEventLoop(100);
  const int before_event = timer->remainingTime();
  text->setText("restart deadline");
  const int after_event = timer->remainingTime();

  EXPECT_GT(before_event, 0);
  EXPECT_GT(after_event, before_event + 20);
  EXPECT_GT(engine.stats().event_count, 0);

  delete panel;
}

TEST_F(PanelEngineTest, CloseIsIdempotent) {
  PJ::PanelEngine engine(makeMockHandle());
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  engine.close();
  engine.close();  // Must not crash.

  delete panel;
}

// Claim: the owner's close callback may stop and deleteLater the engine without duplicate teardown or late ticks.
TEST_F(PanelEngineTest, OwnerCloseCallbackMayScheduleEngineDeletion) {
  auto* engine = new PJ::PanelEngine(makeMockHandle(), {.tick_interval_ms = 10});
  QPointer<PJ::PanelEngine> guard(engine);
  QWidget* panel = engine->openPanel();
  ASSERT_NE(panel, nullptr);
  int closes = 0;
  engine->onCloseRequested([&](const std::string&) {
    ++closes;
    engine->close();
    engine->deleteLater();
    return true;
  });
  mockPanelState().close_on_next_tick = true;
  panel->show();
  pumpEventLoop(50);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  EXPECT_TRUE(guard.isNull());
  EXPECT_EQ(closes, 1);
  delete panel;
}

namespace {

// A panel whose widget_data() is EMPTY whenever nothing changed (the documented
// "no update" answer, and what real plugins do to avoid serializing a document
// 20 times a second), and whose only non-empty payload is a sub-dialog request.
// Two consecutive requests are therefore byte-identical, and the host's
// skip-identical shortcut must not swallow the second one.
constexpr char kSilentSettingsUi[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>SilentSettings</class>
 <widget class="QWidget" name="silentSettings">
  <property name="windowTitle"><string>Silent settings</string></property>
  <layout class="QVBoxLayout">
   <item>
    <widget class="QLineEdit" name="model"/>
   </item>
   <item>
    <widget class="QDialogButtonBox" name="buttonBox">
     <property name="standardButtons"><set>QDialogButtonBox::Cancel|QDialogButtonBox::Ok</set></property>
    </widget>
   </item>
  </layout>
 </widget>
 <resources/>
 <connections/>
</ui>
)";

class SilentWhenIdlePlugin final : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return R"({"id":"silent-when-idle","name":"Silent when idle","version":"0.0.1"})";
  }

  std::string ui_content() const override {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>SilentWhenIdle</class>
 <widget class="QWidget" name="silentWhenIdle">
  <layout class="QVBoxLayout">
   <item>
    <widget class="QPushButton" name="openSettings">
     <property name="text"><string>Settings</string></property>
    </widget>
   </item>
  </layout>
 </widget>
 <resources/>
 <connections/>
</ui>
)";
  }

  std::string widget_data() override {
    if (!open_settings_) {
      return {};
    }
    open_settings_ = false;
    PJ::WidgetData data;
    data.requestSubDialog(kSilentSettingsUi);
    return data.toJson();
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name != "openSettings") {
      return false;
    }
    open_settings_ = true;
    return true;
  }

 private:
  bool open_settings_ = false;
};

PJ::DialogHandle makeSilentWhenIdleHandle() {
  const auto* vtable = PJ::DialogPluginBase::vtableWithCreate([]() noexcept -> void* {
    try {
      return static_cast<PJ::DialogPluginBase*>(new SilentWhenIdlePlugin());
    } catch (...) {
      return nullptr;
    }
  });
  return PJ::DialogHandle(vtable);
}

}  // namespace

TEST_F(PanelEngineTest, SubDialogReopensAfterCancelWhenThePluginIsSilentInBetween) {
  // Settings, Cancel, Settings: the second request carries the same bytes as
  // the first (nothing changed in between, and the plugin answered "" on every
  // poll since), so a host that dedups on raw equality never opens it again.
  PJ::PanelEngineConfig config;
  config.tick_interval_ms = 100000;  // drive everything through the click path
  config.enable_diff = true;
  PJ::PanelEngine engine(makeSilentWhenIdleHandle(), config);
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);
  auto* button = panel->findChild<QPushButton*>("openSettings");
  ASSERT_NE(button, nullptr);

  int opened = 0;
  auto click_then_cancel = [&] {
    // Fires inside the sub-dialog's nested exec() when one opened — and
    // harmlessly from the pump below when none did.
    QTimer::singleShot(0, panel, [&] {
      if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
        ++opened;
        modal->reject();
      }
    });
    button->click();
    pumpEventLoop(20);
  };

  click_then_cancel();
  ASSERT_EQ(opened, 1);
  click_then_cancel();
  EXPECT_EQ(opened, 2) << "an identical one-shot request after a silent idle stretch was deduplicated away";

  delete panel;
}
