// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Test-only implementation included by MainWindow.cpp only when the
// browser acceptance configuration enables PJ_WASM_ENABLE_INGRESS_PROBE.
// Keeping it out of the shell source makes the production ownership boundary
// explicit while preserving same-translation-unit access to probe helpers.

#include <functional>

#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"

namespace {

QMetaObject::Connection wasm_frame_timing_connection;
std::uint64_t wasm_frame_timing_sequence = 0;

void stopWasmFrameTimingProbe() {
  QObject::disconnect(wasm_frame_timing_connection);
  wasm_frame_timing_connection = {};
}

}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_open_load() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* main_window = qobject_cast<PJ::MainWindow*>(widget)) {
      QMetaObject::invokeMethod(main_window, "onLoadDataRequested", Qt::QueuedConnection);
      return;
    }
  }
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_open_robot_picker() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    if (main_window == nullptr) {
      continue;
    }
    for (QToolButton* button : main_window->findChildren<QToolButton*>(u"scene3dAddRobotModel"_s)) {
      if (button->isVisibleTo(main_window)) {
        QMetaObject::invokeMethod(button, &QToolButton::click, Qt::QueuedConnection);
        return;
      }
    }
  }
  qWarning("PJ_WASM_ROBOT_PICKER_FAILED no visible add button");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_open_layout() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    if (main_window == nullptr) {
      continue;
    }
    auto* action = main_window->findChild<QAction*>(u"actionLoadLayout"_s);
    qInfo(
        "PJ_WASM_LAYOUT_ACTION present=%d enabled=%d visible=%d", action != nullptr ? 1 : 0,
        action != nullptr && action->isEnabled() ? 1 : 0, action != nullptr && action->isVisible() ? 1 : 0);
    if (action != nullptr && action->isEnabled()) {
      action->trigger();
    }
    return;
  }
  qWarning("PJ_WASM_LAYOUT_ACTION_FAILED no main window");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_file_menu() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    if (main_window == nullptr) {
      continue;
    }
    for (QMenuBar* menu_bar : main_window->findChildren<QMenuBar*>()) {
      for (QAction* action : menu_bar->actions()) {
        QString label = action->text();
        label.remove(QLatin1Char('&'));
        if (label != QObject::tr("File")) {
          continue;
        }
        const QPoint center = menu_bar->mapToGlobal(menu_bar->actionGeometry(action).center());
        qInfo("PJ_WASM_FILE_MENU center=%d,%d", center.x(), center.y());
        return;
      }
    }
  }
  qWarning("PJ_WASM_FILE_MENU_FAILED not found");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_load_button() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    auto* button = main_window != nullptr ? main_window->findChild<QPushButton*>(u"buttonLoadDatafile"_s) : nullptr;
    if (button == nullptr) {
      continue;
    }
    const QPoint center = main_window->mapFromGlobal(button->mapToGlobal(button->rect().center()));
    qInfo(
        "PJ_WASM_LOAD_BUTTON center=%d,%d enabled=%d visible=%d", center.x(), center.y(), button->isEnabled() ? 1 : 0,
        button->isVisibleTo(main_window) ? 1 : 0);
    return;
  }
  qWarning("PJ_WASM_LOAD_BUTTON_FAILED not found");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_timeline_play() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    auto* button = main_window != nullptr ? main_window->findChild<QPushButton*>(u"buttonPlay"_s) : nullptr;
    if (button == nullptr || !button->isVisibleTo(main_window)) {
      continue;
    }
    const QPoint center = main_window->mapFromGlobal(button->mapToGlobal(button->rect().center()));
    qInfo(
        "PJ_WASM_TIMELINE_PLAY center=%d,%d enabled=%d checked=%d", center.x(), center.y(), button->isEnabled() ? 1 : 0,
        button->isChecked() ? 1 : 0);
    return;
  }
  qWarning("PJ_WASM_TIMELINE_PLAY_FAILED not found");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_begin_frame_timing() {
  stopWasmFrameTimingProbe();
  wasm_frame_timing_sequence = 0;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    if (main_window == nullptr) {
      continue;
    }
    for (auto* canvas : main_window->findChildren<PJ::PlotRhiCanvas*>()) {
      if (!canvas->isVisibleTo(main_window)) {
        continue;
      }
      wasm_frame_timing_connection = QObject::connect(canvas, &PJ::PlotRhiCanvas::frameSubmitted, canvas, []() {
        ++wasm_frame_timing_sequence;
        qInfo(
            "PJ_WASM_PERF_FRAME sequence=%llu now_ms=%.3f", static_cast<unsigned long long>(wasm_frame_timing_sequence),
            emscripten_get_now());
      });
      qInfo("PJ_WASM_PERF_FRAME_BEGIN now_ms=%.3f", emscripten_get_now());
      return;
    }
  }
  qWarning("PJ_WASM_PERF_FRAME_FAILED no visible plot canvas");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_stop_frame_timing() {
  const auto frame_count = wasm_frame_timing_sequence;
  stopWasmFrameTimingProbe();
  qInfo(
      "PJ_WASM_PERF_FRAME_END frames=%llu now_ms=%.3f", static_cast<unsigned long long>(frame_count),
      emscripten_get_now());
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_recent_button() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    auto* button = main_window != nullptr ? main_window->findChild<QToolButton*>(u"buttonRecentFiles"_s) : nullptr;
    if (button == nullptr) {
      continue;
    }
    const QPoint center = button->mapToGlobal(button->rect().center());
    qInfo(
        "PJ_WASM_RECENT_BUTTON center=%d,%d enabled=%d visible=%d", center.x(), center.y(), button->isEnabled() ? 1 : 0,
        button->isVisible() ? 1 : 0);
    return;
  }
  qWarning("PJ_WASM_RECENT_BUTTON_FAILED not found");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_recent_menu() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* menu = qobject_cast<QMenu*>(widget);
    if (menu == nullptr || !menu->isVisible() || menu->objectName() != "PJMenu"_L1) {
      continue;
    }
    for (QAction* action : menu->actions()) {
      if (!action->isEnabled() || action->isSeparator()) {
        continue;
      }
      const QPoint center = menu->mapToGlobal(menu->actionGeometry(action).center());
      qInfo("PJ_WASM_RECENT_ACTION label=%s center=%d,%d", qPrintable(action->text()), center.x(), center.y());
    }
    return;
  }
  qWarning("PJ_WASM_RECENT_MENU_FAILED not visible");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_layout_dialog() {
  auto* dialog = qobject_cast<PJ::MessageBox*>(QApplication::activeModalWidget());
  auto* title = dialog != nullptr ? dialog->findChild<QLabel*>(u"pjMessageBoxTitle"_s) : nullptr;
  auto* body = dialog != nullptr ? dialog->findChild<QLabel*>(u"pjMessageBoxBody"_s) : nullptr;
  qInfo(
      "PJ_WASM_LAYOUT_DIALOG present=%d title=%s body=%s", dialog != nullptr ? 1 : 0,
      title != nullptr ? qPrintable(title->text()) : "", body != nullptr ? qPrintable(body->text()) : "");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_dismiss_layout_dialog() {
  auto* dialog = qobject_cast<PJ::MessageBox*>(QApplication::activeModalWidget());
  auto* button = dialog != nullptr ? dialog->findChild<QPushButton*>(u"pjMessageBoxButton"_s) : nullptr;
  if (button != nullptr) {
    button->click();
    return;
  }
  qWarning("PJ_WASM_LAYOUT_DIALOG_DISMISS_FAILED no active layout dialog");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_layout_persistence() {
  const QSettings settings;
  const QStringList layouts = settings.value(u"Layout/recent"_s).toStringList();
  const QStringList files = settings.value(u"File/recent"_s).toStringList();
  qInfo("PJ_WASM_LAYOUT_PERSISTENCE recent=%d files=%d", layouts.size(), files.size());
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_source_layout_decision() {
  auto* dialog = qobject_cast<PJ::MessageBox*>(QApplication::activeModalWidget());
  if (dialog != nullptr) {
    for (QPushButton* button : dialog->findChildren<QPushButton*>()) {
      const QPoint center = button->mapToGlobal(button->rect().center());
      if (button->text() == QObject::tr("Reselect source") ||
          button->text().startsWith(QObject::tr("Select source "))) {
        qInfo("PJ_WASM_SOURCE_LAYOUT_RESELECT center=%d,%d", center.x(), center.y());
      } else if (button->text() == QObject::tr("Cancel")) {
        qInfo("PJ_WASM_SOURCE_LAYOUT_CANCEL center=%d,%d", center.x(), center.y());
      }
    }
    return;
  }
  qWarning("PJ_WASM_SOURCE_LAYOUT_RESELECT_FAILED no active decision");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_supersede_source_layout_picker() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* main_window = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window->wasmProbeSupersedeBrowserReplayPicker();
      return;
    }
  }
  qWarning("PJ_WASM_SOURCE_LAYOUT_SUPERSEDE_FAILED no main window");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_dialog_buttons() {
  PJ::MainWindow* main_window = nullptr;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* candidate = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window = candidate;
      break;
    }
  }
  QWidget* dialog = QApplication::activeModalWidget();
  if (dialog == nullptr) {
    dialog = qobject_cast<QDialog*>(QApplication::activeWindow());
  }
  if (dialog == nullptr) {
    // Browser-facing Dialog::open() is deliberately non-blocking and some Qt
    // WASM builds do not expose it through activeModalWidget(). Fall back to
    // the visible top-level dialog so the physical-button probe follows the
    // same asynchronous dialog contract.
    for (QWidget* widget : QApplication::topLevelWidgets()) {
      if (qobject_cast<QDialog*>(widget) != nullptr && widget->isVisible()) {
        dialog = widget;
        break;
      }
    }
  }
  QDialogButtonBox* button_box = nullptr;
  int lowest_button_y = std::numeric_limits<int>::min();
  if (dialog != nullptr) {
    for (auto* candidate : dialog->findChildren<QDialogButtonBox*>(u"buttonBox"_s)) {
      auto* candidate_ok = candidate->button(QDialogButtonBox::Ok);
      if (!candidate->isVisibleTo(dialog) || candidate_ok == nullptr || !candidate_ok->isVisibleTo(dialog)) {
        continue;
      }
      const int candidate_y = candidate_ok->mapToGlobal(candidate_ok->rect().center()).y();
      if (candidate_y > lowest_button_y) {
        lowest_button_y = candidate_y;
        button_box = candidate;
      }
    }
  }
  auto* ok = button_box != nullptr ? button_box->button(QDialogButtonBox::Ok) : nullptr;
  auto* cancel = button_box != nullptr ? button_box->button(QDialogButtonBox::Cancel) : nullptr;
  if (main_window == nullptr || dialog == nullptr || button_box == nullptr || (ok == nullptr && cancel == nullptr)) {
    qWarning("PJ_WASM_DIALOG_BUTTONS_FAILED incomplete active dialog");
    return;
  }
  const auto center = [main_window](QWidget* button) {
    return button != nullptr ? main_window->mapFromGlobal(button->mapToGlobal(button->rect().center()))
                             : QPoint(-1, -1);
  };
  const QPoint ok_center = center(ok);
  const QPoint cancel_center = center(cancel);
  qInfo(
      "PJ_WASM_DIALOG_BUTTONS ok=%d,%d cancel=%d,%d ok_enabled=%d cancel_enabled=%d", ok_center.x(), ok_center.y(),
      cancel_center.x(), cancel_center.y(), ok != nullptr && ok->isEnabled() ? 1 : 0,
      cancel != nullptr && cancel->isEnabled() ? 1 : 0);
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_csv_dialog_controls() {
  PJ::MainWindow* main_window = nullptr;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* candidate = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window = candidate;
      break;
    }
  }
  QWidget* dialog = QApplication::activeModalWidget();
  auto* delimiter = dialog != nullptr ? dialog->findChild<QComboBox*>(u"comboBox"_s) : nullptr;
  auto* select_column = dialog != nullptr ? dialog->findChild<QRadioButton*>(u"radioButtonSelect"_s) : nullptr;
  auto* columns = dialog != nullptr ? dialog->findChild<QListWidget*>(u"listWidgetSeries"_s) : nullptr;
  QListWidgetItem* time_item = nullptr;
  if (columns != nullptr) {
    const auto matches = columns->findItems(u"time"_s, Qt::MatchExactly);
    time_item = matches.empty() ? nullptr : matches.front();
  }
  if (main_window == nullptr || delimiter == nullptr || select_column == nullptr || columns == nullptr) {
    qWarning("PJ_WASM_CSV_DIALOG_CONTROLS_FAILED incomplete active dialog");
    return;
  }
  const auto center = [main_window](QWidget* widget) {
    return main_window->mapFromGlobal(widget->mapToGlobal(widget->rect().center()));
  };
  const QPoint delimiter_center = center(delimiter);
  const QPoint select_center = center(select_column);
  QPoint semicolon_center(-1, -1);
  if (QAbstractItemView* popup = delimiter->view();
      popup != nullptr && popup->isVisible() && delimiter->model()->rowCount() > 1) {
    const QModelIndex semicolon = delimiter->model()->index(1, delimiter->modelColumn());
    semicolon_center =
        main_window->mapFromGlobal(popup->viewport()->mapToGlobal(popup->visualRect(semicolon).center()));
  }
  QPoint time_center(-1, -1);
  if (time_item != nullptr) {
    time_center =
        main_window->mapFromGlobal(columns->viewport()->mapToGlobal(columns->visualItemRect(time_item).center()));
  }
  qInfo(
      "PJ_WASM_CSV_DIALOG_CONTROLS delimiter=%d,%d semicolon=%d,%d select=%d,%d time=%d,%d", delimiter_center.x(),
      delimiter_center.y(), semicolon_center.x(), semicolon_center.y(), select_center.x(), select_center.y(),
      time_center.x(), time_center.y());
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_ingest_controls() {
  PJ::MainWindow* main_window = nullptr;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* candidate = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window = candidate;
      break;
    }
  }
  QPoint stop_center(-1, -1);
  QPoint remove_center(-1, -1);
  QPoint keep_center(-1, -1);
  QPoint cancel_center(-1, -1);
  if (main_window != nullptr) {
    const auto center = [main_window](QWidget* widget) {
      return main_window->mapFromGlobal(widget->mapToGlobal(widget->rect().center()));
    };
    // The per-dataset row affordances are where a stop is expressed: ✕ keeps
    // what arrived, the bin discards it, and the CHOICE is the click — there is
    // no follow-up dialog. Reported through the view's own hit-test, so this
    // can only ever name a point a click would honour.
    for (auto* tree : main_window->findChildren<PJ::CurveTreeView*>()) {
      if (!tree->isVisibleTo(main_window)) {
        continue;
      }
      const PJ::CurveTreeView::StopButtonPoints points = tree->activeStopButtonPoints();
      const auto to_window = [main_window, tree](const QPoint& viewport_point) {
        return main_window->mapFromGlobal(tree->viewport()->mapToGlobal(viewport_point));
      };
      if (points.keep.x() >= 0) {
        keep_center = to_window(points.keep);
        stop_center = keep_center;  // the row IS the stop affordance
      }
      if (points.discard.x() >= 0) {
        remove_center = to_window(points.discard);
        if (stop_center.x() < 0) {
          stop_center = remove_center;
        }
      }
      if (stop_center.x() >= 0) {
        break;
      }
    }
    // The title-bar strip is retained but suppressed (kShowTitleBarIngestStrip);
    // still reported when something shows it, so this probe keeps describing
    // whatever stop affordance is actually on screen.
    auto* stop = main_window->findChild<QToolButton*>(u"ingestPrimaryButton"_s);
    if (stop_center.x() < 0 && stop != nullptr && stop->isVisibleTo(main_window)) {
      stop_center = center(stop);
    }
    auto* decision = qobject_cast<PJ::MessageBox*>(QApplication::activeModalWidget());
    auto* title = decision != nullptr ? decision->findChild<QLabel*>(u"pjMessageBoxTitle"_s) : nullptr;
    if (decision != nullptr && title != nullptr && title->text() == QObject::tr("Stop loading?")) {
      for (auto* button : decision->findChildren<QPushButton*>(u"pjMessageBoxButton"_s)) {
        if (button->text() == QObject::tr("Remove All")) {
          remove_center = center(button);
        } else if (button->text() == QObject::tr("Stop and Keep")) {
          keep_center = center(button);
        } else if (button->text() == QObject::tr("Cancel")) {
          cancel_center = center(button);
        }
      }
    }
  }
  qInfo(
      "PJ_WASM_INGEST_CONTROLS stop=%d,%d remove=%d,%d keep=%d,%d cancel=%d,%d", stop_center.x(), stop_center.y(),
      remove_center.x(), remove_center.y(), keep_center.x(), keep_center.y(), cancel_center.x(), cancel_center.y());
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_layout_snapshot() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* main_window = qobject_cast<PJ::MainWindow*>(widget)) {
      const QByteArray encoded = main_window->wasmProbeLayoutBytes().toBase64();
      qInfo("PJ_WASM_LAYOUT_SNAPSHOT base64=%s", encoded.constData());
      return;
    }
  }
  qWarning("PJ_WASM_LAYOUT_SNAPSHOT_FAILED no main window");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_dataset_count() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* main_window = qobject_cast<PJ::MainWindow*>(widget)) {
      qInfo("PJ_WASM_DATASET_COUNT count=%lld", static_cast<long long>(main_window->wasmProbeDatasetCount()));
      return;
    }
  }
  qWarning("PJ_WASM_DATASET_COUNT_FAILED no main window");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_streaming_state() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* main_window = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window->wasmProbeReportStreamingState();
      return;
    }
  }
  qWarning("PJ_WASM_STREAM_STATE_FAILED no main window");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_cycle_time_tracker() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    if (main_window == nullptr) {
      continue;
    }
    auto* button = main_window->findChild<QToolButton*>(u"buttonTimeTracker"_s);
    if (button != nullptr) {
      button->click();
      return;
    }
  }
  qWarning("PJ_WASM_TRACKER_CYCLE_FAILED button unavailable");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_show_legend() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    if (main_window == nullptr) {
      continue;
    }
    auto* button = main_window->findChild<QToolButton*>(u"buttonLegendPosition"_s);
    if (button != nullptr) {
      if (!button->isChecked()) {
        button->click();
      }
      return;
    }
  }
  qWarning("PJ_WASM_LEGEND_SHOW_FAILED button unavailable");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_toggle_show_point() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    if (main_window == nullptr) {
      continue;
    }
    auto* button = main_window->findChild<QToolButton*>(u"buttonShowpoint"_s);
    if (button != nullptr) {
      button->click();
      return;
    }
  }
  qWarning("PJ_WASM_SHOW_POINT_TOGGLE_FAILED button unavailable");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_force_text_failure(int enabled) {
  int canvases = 0;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    for (auto* canvas : widget->findChildren<PJ::PlotRhiCanvas*>()) {
      canvas->setProperty("pjWasmForceTextAtlasFailure", enabled != 0);
      if (enabled != 0) {
        canvas->setProperty("pjWasmTextFailureAttempts", 0);
      }
      canvas->replot();
      ++canvases;
    }
  }
  qInfo("PJ_WASM_TEXT_FAILURE_MODE enabled=%d canvases=%d", enabled != 0 ? 1 : 0, canvases);
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_force_pipeline_failure(int enabled) {
  int canvases = 0;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    for (auto* canvas : widget->findChildren<PJ::PlotRhiCanvas*>()) {
      canvas->setProperty("pjWasmForcePipelineFailure", enabled != 0);
      canvas->replot();
      ++canvases;
    }
  }
  qInfo("PJ_WASM_PIPELINE_FAILURE_MODE enabled=%d canvases=%d", enabled != 0 ? 1 : 0, canvases);
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_set_first_curve_dotted() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    for (auto* plot : widget->findChildren<PJ::PlotWidget*>()) {
      if (!plot->isVisible()) {
        continue;
      }
      for (const PJ::PlotWidgetBase::CurveInfo& info : plot->curveList()) {
        if (info.curve == nullptr) {
          continue;
        }
        QPen pen = info.curve->pen();
        pen.setStyle(Qt::DotLine);
        info.curve->setPen(pen);
        plot->replot();
        qInfo("PJ_WASM_CURVE_DOTTED title=%s", qPrintable(info.curve->title().text()));
        return;
      }
    }
  }
  qWarning("PJ_WASM_CURVE_DOTTED_FAILED no visible curve");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_set_extreme_offscreen_view() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    for (auto* plot : widget->findChildren<PJ::PlotWidget*>()) {
      const auto curve = std::find_if(
          plot->curveList().cbegin(), plot->curveList().cend(),
          [](const PJ::PlotWidgetBase::CurveInfo& info) { return info.curve != nullptr; });
      if (!plot->isVisible() || curve == plot->curveList().cend()) {
        continue;
      }
      // The browser fixture has y={1e100, 0, 0.5}. Keep its final two samples
      // visible while the first maps far offscreen. The non-control width makes
      // the huge source phase land close to a pattern boundary. This catches
      // both an enormous absolute geometry cursor and a phase anchor whose ULP
      // is larger than the entire clipped canvas span.
      QPen pen = curve->curve->pen();
      pen.setStyle(Qt::DotLine);
      pen.setWidthF(1.4);
      curve->curve->setPen(pen);
      plot->setZoomRectangle(QRectF(QPointF(0.0, -1.0), QPointF(2.0, 1.0)), false);
      plot->replot();
      qInfo("PJ_WASM_EXTREME_OFFSCREEN_VIEW title=%s", qPrintable(curve->curve->title().text()));
      return;
    }
  }
  qWarning("PJ_WASM_EXTREME_OFFSCREEN_VIEW_FAILED no visible curve");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_plot_state() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    if (main_window == nullptr) {
      continue;
    }
    for (QShortcut* shortcut : main_window->findChildren<QShortcut*>()) {
      if (shortcut->key() == QKeySequence(Qt::Key_F8)) {
        QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection);
        return;
      }
    }
  }
  qWarning("PJ_WASM_PLOT_STATE_FAILED state probe unavailable");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_filter_editor() {
  PJ::MainWindow* main_window = nullptr;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* candidate = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window = candidate;
      break;
    }
  }
  auto* panel = main_window != nullptr ? main_window->findChild<PJ::FilterEditorPanel*>() : nullptr;
  auto* transforms = panel != nullptr ? panel->findChild<QListWidget*>(u"transform_list"_s) : nullptr;
  auto* apply = panel != nullptr ? panel->findChild<QPushButton*>(u"save_btn"_s) : nullptr;
  if (main_window == nullptr || panel == nullptr || !panel->isVisibleTo(main_window) || transforms == nullptr ||
      apply == nullptr) {
    qWarning("PJ_WASM_FILTER_EDITOR_FAILED editor unavailable");
    return;
  }

  QListWidgetItem* absolute_item = nullptr;
  for (int row = 0; row < transforms->count(); ++row) {
    QListWidgetItem* item = transforms->item(row);
    if (item != nullptr && item->data(Qt::UserRole).toString() == u"absolute"_s) {
      absolute_item = item;
      break;
    }
  }
  const auto center = [main_window](QWidget* widget) {
    return main_window->mapFromGlobal(widget->mapToGlobal(widget->rect().center()));
  };
  QPoint absolute_center(-1, -1);
  if (absolute_item != nullptr) {
    absolute_center = main_window->mapFromGlobal(
        transforms->viewport()->mapToGlobal(transforms->visualItemRect(absolute_item).center()));
  }
  const QPoint apply_center = center(apply);
  const QListWidgetItem* selected_item = transforms->currentItem();
  const QString selected = selected_item != nullptr ? selected_item->data(Qt::UserRole).toString() : QString();
  auto* preview = panel->findChild<PJ::PlotWidget*>();
  const int preview_curves = preview != nullptr ? static_cast<int>(preview->curveList().size()) : 0;
  qInfo(
      "PJ_WASM_FILTER_EDITOR absolute=%d,%d apply=%d,%d selected=%s apply_enabled=%d preview_curves=%d",
      absolute_center.x(), absolute_center.y(), apply_center.x(), apply_center.y(), qPrintable(selected),
      apply->isEnabled() ? 1 : 0, preview_curves);

  if (preview == nullptr) {
    return;
  }
  for (const PJ::PlotWidgetBase::CurveInfo& info : preview->curveList()) {
    if (info.curve == nullptr || info.curve->data() == nullptr) {
      continue;
    }
    const auto* data = info.curve->data();
    QStringList y_values;
    const std::size_t count = data->size();
    const std::size_t reported = std::min<std::size_t>(count, 32);
    y_values.reserve(static_cast<qsizetype>(reported));
    for (std::size_t index = 0; index < reported; ++index) {
      y_values.push_back(QString::number(data->sample(index).y(), 'g', 17));
    }
    const QByteArray title = info.curve->title().text().toUtf8().toBase64();
    qInfo(
        "PJ_WASM_FILTER_PREVIEW title_b64=%s visible=%d samples=%llu y=%s truncated=%d", title.constData(),
        info.curve->isVisible() ? 1 : 0, static_cast<unsigned long long>(count), qPrintable(y_values.join(u","_s)),
        reported < count ? 1 : 0);
  }
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_filter_result() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* main_window = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window->wasmProbeReportFilterResult();
      return;
    }
  }
  qWarning("PJ_WASM_FILTER_RESULT_FAILED no main window");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_transform_editor() {
  PJ::MainWindow* main_window = nullptr;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* candidate = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window = candidate;
      break;
    }
  }
  if (main_window == nullptr) {
    qWarning("PJ_WASM_TRANSFORM_EDITOR_FAILED no main window");
    return;
  }

  const auto center = [main_window](QWidget* widget) {
    return widget != nullptr ? main_window->mapFromGlobal(widget->mapToGlobal(widget->rect().center()))
                             : QPoint(-1, -1);
  };
  auto* add = main_window->findChild<QToolButton*>(u"buttonAddCustom"_s);
  auto* tree = main_window->findChild<QTreeWidget*>(u"treeView"_s);
  QPoint source_center(-1, -1);
  if (tree != nullptr) {
    for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
      QTreeWidgetItem* item = *it;
      if (item->text(0) != u"temp"_s || item->isHidden()) {
        continue;
      }
      const QRect rect = tree->visualItemRect(item);
      if (!rect.isEmpty()) {
        source_center = main_window->mapFromGlobal(tree->viewport()->mapToGlobal(rect.center()));
        break;
      }
    }
  }
  auto* panel = main_window->findChild<QWidget*>(u"TransformEditorDialog"_s);
  const bool open = panel != nullptr && panel->isVisibleTo(main_window);
  auto* sources = open ? panel->findChild<QTableWidget*>(u"tableSources"_s) : nullptr;
  auto* name = open ? panel->findChild<QLineEdit*>(u"nameLineEdit"_s) : nullptr;
  auto* function = open ? panel->findChild<QPlainTextEdit*>(u"functionText"_s) : nullptr;
  auto* terminal = open ? panel->findChild<QPlainTextEdit*>(u"terminalPlainText"_s) : nullptr;
  auto* create = open ? panel->findChild<QPushButton*>(u"pushButtonCreate"_s) : nullptr;
  const QPoint add_center = center(add);
  const QPoint drop_center = sources != nullptr ? center(sources->viewport()) : QPoint(-1, -1);
  const QPoint name_center = center(name);
  const QPoint function_center = center(function);
  const QPoint create_center = center(create);
  auto* preview_frame = open ? panel->findChild<QFrame*>(u"framePlotPreview"_s) : nullptr;
  auto* preview = preview_frame != nullptr ? preview_frame->findChild<PJ::PlotWidget*>() : nullptr;
  const int preview_curves = preview != nullptr ? static_cast<int>(preview->curveList().size()) : 0;
  qInfo(
      "PJ_WASM_TRANSFORM_EDITOR open=%d add=%d,%d source=%d,%d drop=%d,%d name=%d,%d function=%d,%d "
      "create=%d,%d create_enabled=%d source_rows=%d preview_curves=%d",
      open ? 1 : 0, add_center.x(), add_center.y(), source_center.x(), source_center.y(), drop_center.x(),
      drop_center.y(), name_center.x(), name_center.y(), function_center.x(), function_center.y(), create_center.x(),
      create_center.y(), create != nullptr && create->isEnabled() ? 1 : 0, sources != nullptr ? sources->rowCount() : 0,
      preview_curves);
  const QByteArray name_b64 = name != nullptr ? name->text().toUtf8().toBase64() : QByteArray();
  const QByteArray function_b64 = function != nullptr ? function->toPlainText().toUtf8().toBase64() : QByteArray();
  const QByteArray terminal_b64 = terminal != nullptr ? terminal->toPlainText().toUtf8().toBase64() : QByteArray();
  auto* panel_engine = main_window->findChild<PJ::PanelEngine*>();
  const PJ::PanelEngine::Stats stats = panel_engine != nullptr ? panel_engine->stats() : PJ::PanelEngine::Stats{};
  auto* panel_timer = panel_engine != nullptr ? panel_engine->findChild<QTimer*>() : nullptr;
  QStringList visible_dialogs;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* dialog = qobject_cast<QDialog*>(widget);
    if (dialog != nullptr && dialog->isVisible()) {
      visible_dialogs.push_back(dialog->objectName() + u":"_s + dialog->windowTitle());
    }
  }
  const QByteArray dialogs_b64 = visible_dialogs.join(u"|"_s).toUtf8().toBase64();
  qInfo(
      "PJ_WASM_TRANSFORM_EDITOR_DETAIL name_b64=%s function_b64=%s terminal_visible=%d terminal_b64=%s "
      "ticks=%d events=%d applies=%d timer_active=%d dialogs_b64=%s",
      name_b64.constData(), function_b64.constData(), terminal != nullptr && terminal->isVisibleTo(panel) ? 1 : 0,
      terminal_b64.constData(), stats.tick_count, stats.event_count, stats.diff_apply_count,
      panel_timer != nullptr && panel_timer->isActive() ? 1 : 0, dialogs_b64.constData());

  if (preview == nullptr) {
    return;
  }
  for (const PJ::PlotWidgetBase::CurveInfo& info : preview->curveList()) {
    if (info.curve == nullptr || info.curve->data() == nullptr) {
      continue;
    }
    const auto* data = info.curve->data();
    const std::size_t count = data->size();
    const double first = count > 0 ? data->sample(0).y() : 0.0;
    const double last = count > 0 ? data->sample(count - 1).y() : 0.0;
    const QByteArray title = info.curve->title().text().toUtf8().toBase64();
    qInfo(
        "PJ_WASM_TRANSFORM_PREVIEW title_b64=%s samples=%llu first=%.17g last=%.17g dashed=%d", title.constData(),
        static_cast<unsigned long long>(count), first, last, info.curve->pen().style() == Qt::DashLine ? 1 : 0);
  }
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_toolbox_transform_result() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* main_window = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window->wasmProbeReportToolboxTransformResult();
      return;
    }
  }
  qWarning("PJ_WASM_TOOLBOX_TRANSFORM_RESULT_FAILED no main window");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_curve_style_controls() {
  PJ::MainWindow* main_window = nullptr;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* candidate = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window = candidate;
      break;
    }
  }
  if (main_window == nullptr) {
    qWarning("PJ_WASM_CURVE_STYLE_CONTROLS_FAILED no main window");
    return;
  }
  QPushButton* panel = nullptr;
  for (QPushButton* candidate : main_window->findChildren<QPushButton*>(u"plotTabsPanelButton"_s)) {
    if (candidate->toolTip() == QObject::tr("Toggle right panel")) {
      panel = candidate;
      break;
    }
  }
  const std::array<const char*, 10> names = {
      "globalStyleLines",         "globalStyleDots", "globalStyleLinesAndDots", "globalStyleSticks", "globalStyleSteps",
      "globalStyleStepsInverted", "globalWidth1_0",  "globalWidth1_5",          "globalWidth2_0",    "globalWidth3_0",
  };
  std::array<QToolButton*, names.size()> controls{};
  for (std::size_t index = 0; index < names.size(); ++index) {
    controls[index] = main_window->findChild<QToolButton*>(QString::fromLatin1(names[index]));
    if (controls[index] == nullptr) {
      qWarning("PJ_WASM_CURVE_STYLE_CONTROLS_FAILED missing %s", names[index]);
      return;
    }
  }
  if (panel == nullptr) {
    qWarning("PJ_WASM_CURVE_STYLE_CONTROLS_FAILED missing panel toggle");
    return;
  }
  auto* grid = main_window->findChild<QToolButton*>(u"buttonActivateGrid"_s);
  if (grid == nullptr) {
    qWarning("PJ_WASM_CURVE_STYLE_CONTROLS_FAILED missing grid toggle");
    return;
  }
  const auto center = [main_window](QWidget* control) {
    return main_window->mapFromGlobal(control->mapToGlobal(control->rect().center()));
  };
  const QPoint panel_center = center(panel);
  const QPoint grid_center = center(grid);
  std::array<QPoint, controls.size()> centers{};
  std::transform(controls.cbegin(), controls.cend(), centers.begin(), center);
  qInfo(
      "PJ_WASM_CURVE_STYLE_CONTROLS open=%d panel=%d,%d grid=%d,%d lines=%d,%d dots=%d,%d lines_dots=%d,%d "
      "sticks=%d,%d steps=%d,%d steps_inverted=%d,%d width10=%d,%d width15=%d,%d width20=%d,%d width30=%d,%d",
      controls.front()->isVisible() ? 1 : 0, panel_center.x(), panel_center.y(), grid_center.x(), grid_center.y(),
      centers[0].x(), centers[0].y(), centers[1].x(), centers[1].y(), centers[2].x(), centers[2].y(), centers[3].x(),
      centers[3].y(), centers[4].x(), centers[4].y(), centers[5].x(), centers[5].y(), centers[6].x(), centers[6].y(),
      centers[7].x(), centers[7].y(), centers[8].x(), centers[8].y(), centers[9].x(), centers[9].y());
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_xy_dialog() {
  PJ::MainWindow* main_window = nullptr;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* candidate = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window = candidate;
      break;
    }
  }
  for (QWidget* widget : QApplication::allWidgets()) {
    auto* dialog = qobject_cast<PJ::XYCurveDialog*>(widget);
    if (dialog == nullptr || !dialog->isVisible()) {
      continue;
    }
    auto* x_edit = dialog->findChild<QLineEdit*>(u"lineEditX"_s);
    auto* y_edit = dialog->findChild<QLineEdit*>(u"lineEditY"_s);
    auto* alias_edit = dialog->findChild<QLineEdit*>(u"lineEditName"_s);
    auto* swap = dialog->findChild<QPushButton*>(u"pushButtonSwap"_s);
    auto* box = dialog->findChild<QDialogButtonBox*>(u"buttonBox"_s);
    auto* ok = box != nullptr ? box->button(QDialogButtonBox::Ok) : nullptr;
    if (main_window == nullptr || x_edit == nullptr || y_edit == nullptr || alias_edit == nullptr || swap == nullptr ||
        ok == nullptr) {
      qWarning("PJ_WASM_XY_DIALOG_FAILED incomplete dialog");
      return;
    }
    // The dialog is a separate top-level Qt window. Mapping directly between
    // unrelated QWidget trees is unreliable on the WASM platform plugin; cross
    // through Qt global coordinates, then report main-window-relative points.
    const QPoint ok_center = main_window->mapFromGlobal(ok->mapToGlobal(ok->rect().center()));
    const QPoint alias_center = main_window->mapFromGlobal(alias_edit->mapToGlobal(alias_edit->rect().center()));
    const QPoint swap_center = main_window->mapFromGlobal(swap->mapToGlobal(swap->rect().center()));
    const QPoint dialog_position = main_window->mapFromGlobal(dialog->mapToGlobal(QPoint(0, 0)));
    const QByteArray encoded_x = QUrl::toPercentEncoding(x_edit->text());
    const QByteArray encoded_y = QUrl::toPercentEncoding(y_edit->text());
    const QByteArray encoded_alias = QUrl::toPercentEncoding(alias_edit->text());
    qInfo(
        "PJ_WASM_XY_DIALOG x=%s y=%s alias=%s ok=%d,%d alias_edit=%d,%d swap=%d,%d rect=%d,%d,%dx%d",
        encoded_x.constData(), encoded_y.constData(), encoded_alias.constData(), ok_center.x(), ok_center.y(),
        alias_center.x(), alias_center.y(), swap_center.x(), swap_center.y(), dialog_position.x(), dialog_position.y(),
        dialog->width(), dialog->height());
    return;
  }
  qWarning("PJ_WASM_XY_DIALOG_FAILED no visible dialog");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_xy_duplicate_warning() {
  PJ::MainWindow* main_window = nullptr;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* candidate = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window = candidate;
      break;
    }
  }
  for (QWidget* widget : QApplication::allWidgets()) {
    auto* warning = qobject_cast<PJ::MessageBox*>(widget);
    if (warning == nullptr || !warning->isVisible()) {
      continue;
    }
    auto* title = warning->findChild<QLabel*>(u"pjMessageBoxTitle"_s);
    auto* body = warning->findChild<QLabel*>(u"pjMessageBoxBody"_s);
    auto* ok = warning->findChild<QPushButton*>(u"pjMessageBoxButton"_s);
    if (main_window == nullptr || title == nullptr || body == nullptr || ok == nullptr) {
      qWarning("PJ_WASM_XY_WARNING_FAILED incomplete warning");
      return;
    }
    const QPoint ok_center = main_window->mapFromGlobal(ok->mapToGlobal(ok->rect().center()));
    const QByteArray encoded_title = QUrl::toPercentEncoding(title->text());
    const QByteArray encoded_body = QUrl::toPercentEncoding(body->text());
    qInfo(
        "PJ_WASM_XY_WARNING title=%s body=%s ok=%d,%d", encoded_title.constData(), encoded_body.constData(),
        ok_center.x(), ok_center.y());
    return;
  }
  qWarning("PJ_WASM_XY_WARNING_FAILED no visible warning");
}

namespace {
struct WasmPixelBounds {
  qsizetype pixels = 0;
  int min_x = 0;
  int min_y = 0;
  int max_x = -1;
  int max_y = -1;
  std::uint64_t coordinate_hash = 0xcbf29ce484222325ULL;
};

WasmPixelBounds findWasmExactColor(const QImage& image, const QColor& color) {
  WasmPixelBounds result{.min_x = image.width(), .min_y = image.height()};
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      if (std::abs(pixel.red() - color.red()) > 2 || std::abs(pixel.green() - color.green()) > 2 ||
          std::abs(pixel.blue() - color.blue()) > 2) {
        continue;
      }
      ++result.pixels;
      result.min_x = std::min(result.min_x, x);
      result.min_y = std::min(result.min_y, y);
      result.max_x = std::max(result.max_x, x);
      result.max_y = std::max(result.max_y, y);
      result.coordinate_hash ^=
          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) << 32U) | static_cast<std::uint32_t>(x);
      result.coordinate_hash *= 0x100000001b3ULL;
    }
  }
  return result;
}

struct WasmLineCadence {
  qsizetype pixels = 0;
  int column = -1;
  int period = -1;
  int period_matches = 0;
  int period_comparisons = 0;
  int phase_mask = 0;
  int on_phases = 0;
};

WasmLineCadence findWasmVerticalCadence(
    const QImage& image, const QColor& color, int expected_period, int expected_on_phases) {
  WasmLineCadence result;
  std::vector<int> column_counts(static_cast<std::size_t>(image.width()), 0);
  std::vector<int> row_counts(static_cast<std::size_t>(image.height()), 0);
  const auto matches = [&color](const QColor& pixel) {
    return std::abs(pixel.red() - color.red()) <= 2 && std::abs(pixel.green() - color.green()) <= 2 &&
           std::abs(pixel.blue() - color.blue()) <= 2;
  };
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (matches(image.pixelColor(x, y))) {
        ++result.pixels;
        ++column_counts[static_cast<std::size_t>(x)];
        ++row_counts[static_cast<std::size_t>(y)];
      }
    }
  }
  if (result.pixels == 0 || column_counts.empty()) {
    return result;
  }

  // A horizontal grid dash can temporarily turn every intersecting column the
  // target color. Exclude those rows before choosing a vertical line and
  // measuring its period, otherwise an intersection can inflate a run length.
  const int horizontal_threshold = std::max(16, image.width() / 8);
  std::vector<bool> horizontal_rows(static_cast<std::size_t>(image.height()), false);
  std::fill(column_counts.begin(), column_counts.end(), 0);
  for (int y = 0; y < image.height(); ++y) {
    const bool horizontal = row_counts[static_cast<std::size_t>(y)] >= horizontal_threshold;
    horizontal_rows[static_cast<std::size_t>(y)] = horizontal;
    if (horizontal) {
      continue;
    }
    for (int x = 0; x < image.width(); ++x) {
      if (matches(image.pixelColor(x, y))) {
        ++column_counts[static_cast<std::size_t>(x)];
      }
    }
  }
  // Major and minor grid pens now share the framework's Gridline token, so
  // color alone cannot distinguish them. Search every target-color column for
  // the requested cosmetic Qt cadence: 6 rows / 4 on for DashLine, 3 / 1 for
  // DotLine. This still observes the rendered framebuffer rather than the CPU
  // pen state, and it remains independent of DPR.
  WasmLineCadence best;
  best.period = expected_period;
  bool best_has_expected_duty = false;
  for (int x = 0; x < image.width(); ++x) {
    if (column_counts[static_cast<std::size_t>(x)] == 0) {
      continue;
    }
    int period_matches = 0;
    int period_comparisons = 0;
    for (int y = 0; y + expected_period < image.height(); ++y) {
      if (horizontal_rows[static_cast<std::size_t>(y)] ||
          horizontal_rows[static_cast<std::size_t>(y + expected_period)]) {
        continue;
      }
      const bool first = matches(image.pixelColor(x, y));
      const bool second = matches(image.pixelColor(x, y + expected_period));
      period_matches += first == second ? 1 : 0;
      ++period_comparisons;
    }
    if (period_comparisons == 0) {
      continue;
    }
    std::vector<int> phase_matches(static_cast<std::size_t>(expected_period), 0);
    std::vector<int> phase_comparisons(static_cast<std::size_t>(expected_period), 0);
    for (int y = 0; y < image.height(); ++y) {
      if (horizontal_rows[static_cast<std::size_t>(y)]) {
        continue;
      }
      const std::size_t phase = static_cast<std::size_t>(y % expected_period);
      phase_matches[phase] += matches(image.pixelColor(x, y)) ? 1 : 0;
      ++phase_comparisons[phase];
    }
    int mask = 0;
    int on_phases = 0;
    for (int phase = 0; phase < expected_period; ++phase) {
      if (phase_matches[static_cast<std::size_t>(phase)] * 2 >= phase_comparisons[static_cast<std::size_t>(phase)]) {
        mask |= 1 << phase;
        ++on_phases;
      }
    }
    // Canonicalize the cyclic phase because scale-map rounding may move the
    // first dash between framebuffer rows without changing its duty cycle.
    const int period_mask = (1 << expected_period) - 1;
    int canonical_mask = period_mask;
    for (int rotation = 0; rotation < expected_period; ++rotation) {
      const int rotated = ((mask >> rotation) | (mask << (expected_period - rotation))) & period_mask;
      canonical_mask = std::min(canonical_mask, rotated);
    }
    const bool has_expected_duty = on_phases == expected_on_phases;
    const bool better_ratio =
        best.column < 0 || period_matches * best.period_comparisons > best.period_matches * period_comparisons;
    const bool equal_ratio =
        best.column >= 0 && period_matches * best.period_comparisons == best.period_matches * period_comparisons;
    if ((!best_has_expected_duty && has_expected_duty) ||
        (best_has_expected_duty == has_expected_duty &&
         (better_ratio || (equal_ratio && column_counts[static_cast<std::size_t>(x)] >
                                              column_counts[static_cast<std::size_t>(best.column)])))) {
      best.column = x;
      best.period_matches = period_matches;
      best.period_comparisons = period_comparisons;
      best.phase_mask = canonical_mask;
      best.on_phases = on_phases;
      best_has_expected_duty = has_expected_duty;
    }
  }
  result.column = best.column;
  result.period = best.period;
  result.period_matches = best.period_matches;
  result.period_comparisons = best.period_comparisons;
  result.phase_mask = best.phase_mask;
  result.on_phases = best.on_phases;
  return result;
}

WasmPixelBounds findWasmBlendedColor(const QImage& image, const QColor& color, const QColor& background) {
  WasmPixelBounds result{.min_x = image.width(), .min_y = image.height()};
  const std::array<double, 3> delta = {
      static_cast<double>(color.red() - background.red()),
      static_cast<double>(color.green() - background.green()),
      static_cast<double>(color.blue() - background.blue()),
  };
  const double denominator = (delta[0] * delta[0]) + (delta[1] * delta[1]) + (delta[2] * delta[2]);
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      // A cosmetic one-pixel line can fall between MSAA samples, leaving no
      // exact source-color pixel. Match the RGB blend segment from the canvas
      // clear color to the tracker while rejecting unrelated curve/grid hues.
      const std::array<double, 3> offset = {
          static_cast<double>(pixel.red() - background.red()),
          static_cast<double>(pixel.green() - background.green()),
          static_cast<double>(pixel.blue() - background.blue()),
      };
      const double blend =
          denominator > 0.0 ? ((offset[0] * delta[0]) + (offset[1] * delta[1]) + (offset[2] * delta[2])) / denominator
                            : 0.0;
      if (blend < 0.15 || blend > 1.05 || std::abs(offset[0] - (blend * delta[0])) > 4.0 ||
          std::abs(offset[1] - (blend * delta[1])) > 4.0 || std::abs(offset[2] - (blend * delta[2])) > 4.0) {
        continue;
      }
      ++result.pixels;
      result.min_x = std::min(result.min_x, x);
      result.min_y = std::min(result.min_y, y);
      result.max_x = std::max(result.max_x, x);
      result.max_y = std::max(result.max_y, y);
      result.coordinate_hash ^=
          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) << 32U) | static_cast<std::uint32_t>(x);
      result.coordinate_hash *= 0x100000001b3ULL;
    }
  }
  return result;
}

WasmPixelBounds findWasmDarkPixels(const QImage& image, QRect search_rect = {}) {
  WasmPixelBounds result{.min_x = image.width(), .min_y = image.height()};
  search_rect = search_rect.isValid() ? search_rect.intersected(image.rect()) : image.rect();
  for (int y = search_rect.top(); y <= search_rect.bottom(); ++y) {
    for (int x = search_rect.left(); x <= search_rect.right(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      // QRhi color geometry in the acceptance fixtures is saturated blue/red/
      // purple on a white canvas. Near-black framebuffer pixels therefore give
      // an independent, post-draw observation of legend/tracker glyphs and
      // borders rather than merely proving that a CPU atlas was generated.
      if (pixel.alpha() == 0 || pixel.red() > 80 || pixel.green() > 80 || pixel.blue() > 80) {
        continue;
      }
      ++result.pixels;
      result.min_x = std::min(result.min_x, x);
      result.min_y = std::min(result.min_y, y);
      result.max_x = std::max(result.max_x, x);
      result.max_y = std::max(result.max_y, y);
    }
  }
  return result;
}

struct WasmHoverState {
  bool visible = false;
  QPointF point;
  QString label;
};

WasmHoverState findWasmHoverState(PJ::PlotWidget* plot) {
  WasmHoverState result;
  if (plot == nullptr) {
    return result;
  }
  result.visible = plot->pointInspectorVisible();
  result.point = plot->pointInspectorPosition();
  result.label = plot->pointInspectorLabel();
  return result;
}

void reportWasmMenuAction(const QString& expected_label, const char* probe_name) {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* menu = qobject_cast<QMenu*>(widget);
    if (menu == nullptr || !menu->isVisible()) {
      continue;
    }
    for (QAction* action : menu->actions()) {
      QString label = action->text();
      label.remove(QLatin1Char('&'));
      if (label != expected_label) {
        continue;
      }
      const QPoint center = menu->mapToGlobal(menu->actionGeometry(action).center());
      qInfo("PJ_WASM_MENU_ACTION name=%s center=%d,%d", probe_name, center.x(), center.y());
      return;
    }
  }
  qWarning("PJ_WASM_MENU_ACTION_FAILED %s not visible", probe_name);
}
}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_scene2d_state() {
#ifdef PJ_WITH_SCENE2D
  struct ObservedScene2DViewer {
    quint64 identity = 0;
    QPointer<PJ::MediaViewerWidget> viewer;
  };
  static std::vector<ObservedScene2DViewer> observed_viewers;
  static quint64 next_viewer_identity = 1;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    if (main_window == nullptr) {
      continue;
    }
    QList<PJ::Scene2DDockWidget*> docks;
    for (auto* dock : main_window->findChildren<PJ::Scene2DDockWidget*>()) {
      if (dock != nullptr && dock->isVisibleTo(main_window)) {
        docks.push_back(dock);
      }
    }
    std::sort(docks.begin(), docks.end(), [main_window](const auto* lhs, const auto* rhs) {
      const QPoint lhs_position = lhs->mapTo(main_window, QPoint(0, 0));
      const QPoint rhs_position = rhs->mapTo(main_window, QPoint(0, 0));
      return lhs_position.x() != rhs_position.x() ? lhs_position.x() < rhs_position.x()
                                                  : lhs_position.y() < rhs_position.y();
    });

    static quint64 sequence = 0;
    const quint64 current_sequence = ++sequence;
    auto* slider = main_window->findChild<PJ::RealSlider*>(u"timeSlider"_s);
    const QPoint slider_position = slider != nullptr ? slider->mapTo(main_window, QPoint(0, 0)) : QPoint(-1, -1);
    qInfo(
        "PJ_WASM_SCENE2D_SUMMARY sequence=%llu docks=%lld slider=%d,%d,%dx%d value=%.17g range=%.17g,%.17g "
        "enabled=%d",
        static_cast<unsigned long long>(current_sequence), static_cast<long long>(docks.size()), slider_position.x(),
        slider_position.y(), slider != nullptr ? slider->width() : 0, slider != nullptr ? slider->height() : 0,
        slider != nullptr ? slider->getValue() : 0.0, slider != nullptr ? slider->getMinimum() : 0.0,
        slider != nullptr ? slider->getMaximum() : 0.0, slider != nullptr && slider->isEnabled() ? 1 : 0);

    for (qsizetype index = 0; index < docks.size(); ++index) {
      PJ::Scene2DDockWidget* dock = docks[index];
      auto* viewer = dock->findChild<PJ::MediaViewerWidget*>(u"scene2dMediaViewer"_s);
      quint64 viewer_identity = 0;
      if (viewer != nullptr) {
        const auto observed = std::find_if(
            observed_viewers.cbegin(), observed_viewers.cend(),
            [viewer](const ObservedScene2DViewer& entry) { return entry.viewer == viewer; });
        if (observed != observed_viewers.cend()) {
          viewer_identity = observed->identity;
        } else {
          viewer_identity = next_viewer_identity++;
          observed_viewers.push_back({.identity = viewer_identity, .viewer = viewer});
        }
      }
      const QPoint viewer_position = viewer != nullptr ? viewer->mapTo(main_window, QPoint(0, 0)) : QPoint(-1, -1);
      const PJ::MediaViewState view = viewer != nullptr ? viewer->viewState() : PJ::MediaViewState{};
      qInfo(
          "PJ_WASM_SCENE2D_DOCK sequence=%llu index=%lld identity=%llu layers=%llu viewer=%d,%d,%dx%d visible=%d "
          "view=%.9g,%.9g,%.9g",
          static_cast<unsigned long long>(current_sequence), static_cast<long long>(index),
          static_cast<unsigned long long>(viewer_identity),
          static_cast<unsigned long long>(dock->compositeLayerCountForTesting()), viewer_position.x(),
          viewer_position.y(), viewer != nullptr ? viewer->width() : 0, viewer != nullptr ? viewer->height() : 0,
          viewer != nullptr && viewer->isVisibleTo(main_window) ? 1 : 0, static_cast<double>(view.zoom),
          static_cast<double>(view.pan_x), static_cast<double>(view.pan_y));
      if (viewer == nullptr) {
        continue;
      }
      const QImage image = viewer->grabFramebuffer();
      const auto red = findWasmExactColor(image, QColor(255, 0, 0));
      const auto blue = findWasmExactColor(image, QColor(0, 0, 255));
      const auto green = findWasmExactColor(image, QColor(0, 255, 0));
      const auto magenta = findWasmExactColor(image, QColor(255, 0, 255));
      const auto yellow = findWasmExactColor(image, QColor(255, 255, 0));
      const auto cyan = findWasmExactColor(image, QColor(0, 255, 255));
      const auto white = findWasmExactColor(image, QColor(255, 255, 255));
      qInfo(
          "PJ_WASM_SCENE2D_READBACK sequence=%llu index=%lld red=%lld blue=%lld green=%lld magenta=%lld "
          "yellow=%lld cyan=%lld white=%lld framebuffer=%dx%d",
          static_cast<unsigned long long>(current_sequence), static_cast<long long>(index),
          static_cast<long long>(red.pixels), static_cast<long long>(blue.pixels), static_cast<long long>(green.pixels),
          static_cast<long long>(magenta.pixels), static_cast<long long>(yellow.pixels),
          static_cast<long long>(cyan.pixels), static_cast<long long>(white.pixels), image.width(), image.height());
    }
    return;
  }
#endif
  qWarning("PJ_WASM_SCENE2D_FAILED no main window or Scene2D support");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_multi_plot_state() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    if (main_window == nullptr) {
      continue;
    }
    QList<PJ::DockWidget*> docks;
    for (PJ::DockWidget* dock : main_window->findChildren<PJ::DockWidget*>()) {
      if (dock != nullptr && dock->isVisible() && dock->window() == main_window) {
        docks.push_back(dock);
      }
    }
    std::sort(docks.begin(), docks.end(), [main_window](const PJ::DockWidget* lhs, const PJ::DockWidget* rhs) {
      const QPoint lhs_position = lhs->mapTo(main_window, QPoint(0, 0));
      const QPoint rhs_position = rhs->mapTo(main_window, QPoint(0, 0));
      return lhs_position.x() != rhs_position.x() ? lhs_position.x() < rhs_position.x()
                                                  : lhs_position.y() < rhs_position.y();
    });

    static quint64 sequence = 0;
    const quint64 current_sequence = ++sequence;
    const qsizetype plot_count = std::count_if(docks.cbegin(), docks.cend(), [](PJ::DockWidget* dock) {
      return dock->plotWidget() != nullptr && dock->findChild<PJ::PlotRhiCanvas*>() != nullptr;
    });
    // Scan every participating curve color in every canvas readback. Besides
    // proving each plot renders its own curve, this lets acceptance assert that
    // a sibling canvas contains zero pixels from the other plot's curve.
    std::vector<std::pair<QString, QColor>> all_curve_colors;
    for (PJ::DockWidget* dock : docks) {
      PJ::PlotWidget* plot = dock->plotWidget();
      if (plot == nullptr) {
        continue;
      }
      for (const PJ::PlotWidgetBase::CurveInfo& info : plot->curveList()) {
        if (info.curve != nullptr) {
          all_curve_colors.emplace_back(info.curve->title().text(), info.curve->pen().color());
        }
      }
    }
    qInfo(
        "PJ_WASM_MULTI_SUMMARY sequence=%llu docks=%lld plots=%lld", static_cast<unsigned long long>(current_sequence),
        static_cast<long long>(docks.size()), static_cast<long long>(plot_count));

    for (qsizetype index = 0; index < docks.size(); ++index) {
      PJ::DockWidget* dock = docks[index];
      PJ::PlotWidget* plot = dock->plotWidget();
      const QString dock_id = dock->stateId();
      const QPoint dock_position = dock->mapTo(main_window, QPoint(0, 0));
      qInfo(
          "PJ_WASM_MULTI_DOCK sequence=%llu index=%lld id=%s kind=%s rect=%d,%d,%dx%d",
          static_cast<unsigned long long>(current_sequence), static_cast<long long>(index), qPrintable(dock_id),
          plot != nullptr ? "plot" : "placeholder", dock_position.x(), dock_position.y(), dock->width(),
          dock->height());
      if (plot == nullptr) {
        continue;
      }
      auto* canvas = plot->findChild<PJ::PlotRhiCanvas*>();
      if (canvas == nullptr) {
        continue;
      }
      QStringList titles;
      for (const PJ::PlotWidgetBase::CurveInfo& info : plot->curveList()) {
        if (info.curve == nullptr) {
          continue;
        }
        const QString title = info.curve->title().text();
        titles.push_back(title);
      }
      const QPoint canvas_position = canvas->mapTo(main_window, QPoint(0, 0));
      const QRectF view = plot->currentBoundingRect();
      const QString plot_id = plot->stateId();
      qInfo(
          "PJ_WASM_MULTI_PLOT sequence=%llu index=%lld dock_id=%s plot_id=%s titles=%s canvas=%d,%d,%dx%d "
          "view=%.17g,%.17g,%.17g,%.17g backend=%s vertices=%llu",
          static_cast<unsigned long long>(current_sequence), static_cast<long long>(index), qPrintable(dock_id),
          qPrintable(plot_id), qPrintable(titles.join(',')), canvas_position.x(), canvas_position.y(), canvas->width(),
          canvas->height(), view.left(), view.right(), view.bottom(), view.top(), qPrintable(canvas->rendererBackend()),
          static_cast<unsigned long long>(canvas->lastVertexCount()));

      const QColor tracker_color =
          PJ::theme::interaction(PJ::theme::Variant::Highlight, PJ::theme::State::Checked, PJ::theme::appTheme());
      const QColor canvas_color = canvas->palette().color(QPalette::Window);
      QTimer::singleShot(
          250, canvas, [canvas, current_sequence, index, plot_id, all_curve_colors, tracker_color, canvas_color]() {
            const QImage image = canvas->grabFramebuffer();
            QStringList counts;
            for (const auto& [title, color] : all_curve_colors) {
              counts.push_back(
                  u"%1:%2:%3"_s.arg(title, color.name(QColor::HexArgb)).arg(findWasmExactColor(image, color).pixels));
            }
            const WasmPixelBounds tracker = findWasmBlendedColor(image, tracker_color, canvas_color);
            const WasmPixelBounds text = findWasmDarkPixels(image);
            qInfo(
                "PJ_WASM_MULTI_READBACK sequence=%llu index=%lld plot_id=%s curves=%s tracker_pixels=%lld "
                "tracker_bounds=%d,%d..%d,%d text_pixels=%lld text_bounds=%d,%d..%d,%d framebuffer=%dx%d",
                static_cast<unsigned long long>(current_sequence), static_cast<long long>(index), qPrintable(plot_id),
                qPrintable(counts.join(',')), static_cast<long long>(tracker.pixels), tracker.min_x, tracker.min_y,
                tracker.max_x, tracker.max_y, static_cast<long long>(text.pixels), text.min_x, text.min_y, text.max_x,
                text.max_y, image.width(), image.height());
          });
    }
    return;
  }
  qWarning("PJ_WASM_MULTI_FAILED no main window");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_remove_all_action() {
  reportWasmMenuAction(u"Remove ALL curves"_s, "remove_all");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_zoom_out_action() {
  reportWasmMenuAction(u"Zoom Out"_s, "zoom_out");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_split_horizontal_action() {
  reportWasmMenuAction(u"Split Horizontally"_s, "split_horizontal");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_save_layout_action() {
  reportWasmMenuAction(u"Save Layout..."_s, "save_layout");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_save_source_layout_action() {
  reportWasmMenuAction(u"Save Layout with Sources..."_s, "save_source_layout");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_apply_filter_action() {
  reportWasmMenuAction(u"Apply Filter..."_s, "apply_filter");
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_quit_action() {
  reportWasmMenuAction(u"Quit"_s, "quit");
}


// Reveals every dataset-tree leaf (expandAll) and reports each row's center in
// MainWindow coordinates, one console line per leaf plus a terminator, so the
// browser specs drag real rows by NAME instead of hard-coding canvas pixels
// that break whenever the tree's row pitch or header height changes.
extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_curve_rows() {
  PJ::MainWindow* main_window = nullptr;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* candidate = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window = candidate;
      break;
    }
  }
  QTreeWidget* tree = main_window != nullptr ? main_window->findChild<QTreeWidget*>(u"treeView"_s) : nullptr;
  if (tree == nullptr) {
    qWarning("PJ_WASM_CURVE_ROWS_FAILED no dataset tree");
    return;
  }
  tree->expandAll();
  // Same-frame geometry: force the pending relayout so visualItemRect is final.
  QCoreApplication::processEvents();
  int count = 0;
  std::function<void(QTreeWidgetItem*, const QString&)> visit = [&](QTreeWidgetItem* item, const QString& prefix) {
    const QString path = prefix.isEmpty() ? item->text(0) : prefix + u"/"_s + item->text(0);
    const QRect rect = tree->visualItemRect(item);
    if (!rect.isEmpty()) {
      const QPoint center =
          main_window->mapFromGlobal(tree->viewport()->mapToGlobal(QPoint(rect.left() + 40, rect.center().y())));
      qInfo("PJ_WASM_CURVE_ROW path=%s x=%d y=%d children=%d", qPrintable(path), center.x(), center.y(),
            item->childCount());
      ++count;
    }
    for (int index = 0; index < item->childCount(); ++index) {
      visit(item->child(index), path);
    }
  };
  for (int index = 0; index < tree->topLevelItemCount(); ++index) {
    visit(tree->topLevelItem(index), QString{});
  }
  qInfo("PJ_WASM_CURVE_ROWS_DONE count=%d", count);
}

// Reports the center of each empty-dock placeholder family icon in MainWindow
// coordinates. The placeholder is looked up in the visible tab, so the specs
// click the icon they mean regardless of how many families the row holds.
extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_placeholder_icons() {
  PJ::MainWindow* main_window = nullptr;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* candidate = qobject_cast<PJ::MainWindow*>(widget)) {
      main_window = candidate;
      break;
    }
  }
  PJ::VisualizationPlaceholderWidget* placeholder = nullptr;
  if (main_window != nullptr) {
    for (auto* candidate : main_window->findChildren<PJ::VisualizationPlaceholderWidget*>()) {
      if (candidate->isVisible()) {
        placeholder = candidate;
        break;
      }
    }
  }
  if (placeholder == nullptr) {
    qWarning("PJ_WASM_PLACEHOLDER_ICONS_FAILED no visible placeholder");
    return;
  }
  const auto center = [main_window, placeholder](const char* name) {
    auto* button = placeholder->findChild<QToolButton*>(QString::fromLatin1(name));
    return button != nullptr ? main_window->mapFromGlobal(button->mapToGlobal(button->rect().center()))
                             : QPoint(-1, -1);
  };
  const QPoint plot = center("buttonVizPlot");
  const QPoint transitions = center("buttonVizStateTransitions");
  const QPoint scene2d = center("buttonVizScene2D");
  const QPoint scene3d = center("buttonVizScene3D");
  qInfo(
      "PJ_WASM_PLACEHOLDER_ICONS plot=%d,%d transitions=%d,%d scene2d=%d,%d scene3d=%d,%d", plot.x(), plot.y(),
      transitions.x(), transitions.y(), scene2d.x(), scene2d.y(), scene3d.x(), scene3d.y());
}

// clang-format off
EM_JS(void, pj_wasm_install_browser_test_probes, (), {
  globalThis.pjWasmOpenLoadProbe = () => Module._pj_wasm_test_open_load();
  globalThis.pjWasmOpenRobotPickerProbe = () => Module._pj_wasm_test_open_robot_picker();
  globalThis.pjWasmOpenLayoutProbe = () => Module._pj_wasm_test_open_layout();
  globalThis.pjWasmReportFileMenuProbe = () => Module._pj_wasm_test_report_file_menu();
  globalThis.pjWasmReportLoadButtonProbe = () => Module._pj_wasm_test_report_load_button();
  globalThis.pjWasmReportTimelinePlayProbe = () => Module._pj_wasm_test_report_timeline_play();
  globalThis.pjWasmBeginFrameTimingProbe = () => Module._pj_wasm_test_begin_frame_timing();
  globalThis.pjWasmStopFrameTimingProbe = () => Module._pj_wasm_test_stop_frame_timing();
  globalThis.pjWasmReportRecentButtonProbe = () => Module._pj_wasm_test_report_recent_button();
  globalThis.pjWasmReportRecentMenuProbe = () => Module._pj_wasm_test_report_recent_menu();
  globalThis.pjWasmReportLayoutDialogProbe = () => Module._pj_wasm_test_report_layout_dialog();
  globalThis.pjWasmDismissLayoutDialogProbe = () => Module._pj_wasm_test_dismiss_layout_dialog();
  globalThis.pjWasmReportLayoutPersistenceProbe = () => Module._pj_wasm_test_report_layout_persistence();
  globalThis.pjWasmReportSourceLayoutDecisionProbe = () => Module._pj_wasm_test_report_source_layout_decision();
  globalThis.pjWasmSupersedeSourceLayoutPickerProbe = () => Module._pj_wasm_test_supersede_source_layout_picker();
  globalThis.pjWasmReportDialogButtonsProbe = () => Module._pj_wasm_test_report_dialog_buttons();
  globalThis.pjWasmReportCsvDialogControlsProbe = () => Module._pj_wasm_test_report_csv_dialog_controls();
  globalThis.pjWasmReportIngestControlsProbe = () => Module._pj_wasm_test_report_ingest_controls();
  globalThis.pjWasmReportLayoutSnapshotProbe = () => Module._pj_wasm_test_report_layout_snapshot();
  globalThis.pjWasmReportDatasetCountProbe = () => Module._pj_wasm_test_report_dataset_count();
  globalThis.pjWasmReportCurveRowsProbe = () => Module._pj_wasm_test_report_curve_rows();
  globalThis.pjWasmReportPlaceholderIconsProbe = () => Module._pj_wasm_test_report_placeholder_icons();
  globalThis.pjWasmReportStreamingStateProbe = () => Module._pj_wasm_test_report_streaming_state();
  globalThis.pjWasmCycleTimeTrackerProbe = () => Module._pj_wasm_test_cycle_time_tracker();
  globalThis.pjWasmShowLegendProbe = () => Module._pj_wasm_test_show_legend();
  globalThis.pjWasmToggleShowPointProbe = () => Module._pj_wasm_test_toggle_show_point();
  globalThis.pjWasmForceTextFailureProbe = enabled => Module._pj_wasm_test_force_text_failure(enabled ? 1 : 0);
  globalThis.pjWasmForcePipelineFailureProbe = enabled => Module._pj_wasm_test_force_pipeline_failure(enabled ? 1 : 0);
  globalThis.pjWasmSetFirstCurveDottedProbe = () => Module._pj_wasm_test_set_first_curve_dotted();
  globalThis.pjWasmSetExtremeOffscreenViewProbe = () => Module._pj_wasm_test_set_extreme_offscreen_view();
  globalThis.pjWasmReportPlotStateProbe = () => Module._pj_wasm_test_report_plot_state();
  globalThis.pjWasmReportFilterEditorProbe = () => Module._pj_wasm_test_report_filter_editor();
  globalThis.pjWasmReportFilterResultProbe = () => Module._pj_wasm_test_report_filter_result();
  globalThis.pjWasmReportTransformEditorProbe = () => Module._pj_wasm_test_report_transform_editor();
  globalThis.pjWasmReportToolboxTransformResultProbe = () => Module._pj_wasm_test_report_toolbox_transform_result();
  globalThis.pjWasmReportCurveStyleControlsProbe = () => Module._pj_wasm_test_report_curve_style_controls();
  globalThis.pjWasmReportXyDialogProbe = () => Module._pj_wasm_test_report_xy_dialog();
  globalThis.pjWasmReportXyDuplicateWarningProbe = () => Module._pj_wasm_test_report_xy_duplicate_warning();
  globalThis.pjWasmReportScene2DStateProbe = () => Module._pj_wasm_test_report_scene2d_state();
  globalThis.pjWasmReportMultiPlotStateProbe = () => Module._pj_wasm_test_report_multi_plot_state();
  globalThis.pjWasmReportRemoveAllActionProbe = () => Module._pj_wasm_test_report_remove_all_action();
  globalThis.pjWasmReportZoomOutActionProbe = () => Module._pj_wasm_test_report_zoom_out_action();
  globalThis.pjWasmReportSplitHorizontalActionProbe = () => Module._pj_wasm_test_report_split_horizontal_action();
  globalThis.pjWasmReportSaveLayoutActionProbe = () => Module._pj_wasm_test_report_save_layout_action();
  globalThis.pjWasmReportSaveSourceLayoutActionProbe = () => Module._pj_wasm_test_report_save_source_layout_action();
  globalThis.pjWasmReportApplyFilterActionProbe = () => Module._pj_wasm_test_report_apply_filter_action();
  globalThis.pjWasmReportQuitActionProbe = () => Module._pj_wasm_test_report_quit_action();
});
// clang-format on
