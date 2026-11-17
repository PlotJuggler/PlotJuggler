#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The --exit-after-layout exit code: reports exactly one code, from one MainWindow's layoutRestoreSettled or the
// deadline, whichever comes first. A success settles only after the import batch finished, so quitting on it never
// tears down an active batch. main.cpp passes QCoreApplication::exit.

#include <QObject>
#include <QTimer>
#include <chrono>
#include <functional>

namespace PJ {

class MainWindow;

class LayoutExitWatch : public QObject {
  Q_OBJECT

 public:
  // Process exit codes: a CLI contract.
  static constexpr int kExitCodeSuccess = 0;
  static constexpr int kExitCodeLoadFailed = 1;  // open/parse/apply failure, cancel, cancelled batch
  static constexpr int kExitCodeTimeout = 2;

  // Arms the deadline at once: construct before the layout load is scheduled, or a settlement can be missed.
  // `timeout` is clamped to QTimer's int milliseconds; a non-positive one times out on the next event-loop turn.
  LayoutExitWatch(
      MainWindow& window, std::chrono::milliseconds timeout, std::function<void(int)> report_exit_code,
      QObject* parent = nullptr);

 private:
  void decide(int code);

  std::function<void(int)> report_exit_code_;
  QTimer timeout_timer_;
  bool decided_ = false;
};

}  // namespace PJ
