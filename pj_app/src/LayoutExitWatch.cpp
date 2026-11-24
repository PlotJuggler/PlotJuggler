// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "LayoutExitWatch.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "MainWindow.h"

namespace PJ {

LayoutExitWatch::LayoutExitWatch(
    MainWindow& window, std::chrono::milliseconds timeout, std::function<void(int)> report_exit_code, QObject* parent)
    : QObject(parent), report_exit_code_(std::move(report_exit_code)) {
  timeout_timer_.setSingleShot(true);
  connect(&timeout_timer_, &QTimer::timeout, this, [this]() { decide(kExitCodeTimeout); });
  connect(
      &window, &MainWindow::layoutRestoreSettled, this,
      [this, load = window.nextLayoutLoad()](bool success, quint64 settled) {
        if (settled == load) {
          decide(success ? kExitCodeSuccess : kExitCodeLoadFailed);
        }
      });
  const auto clamped_ms = std::min<std::chrono::milliseconds::rep>(timeout.count(), std::numeric_limits<int>::max());
  timeout_timer_.start(static_cast<int>(clamped_ms));
}

void LayoutExitWatch::decide(int code) {
  if (decided_) {
    return;
  }
  decided_ = true;
  timeout_timer_.stop();
  report_exit_code_(code);
}

}  // namespace PJ
