// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Header-only process probes of every frame benchmark (target pj_bench_probes:
// scene3d_frame_benchmark, pj_app's mixed_layout_benchmark, pj_plotting's
// plot_canvas_benchmark): GUI-thread CPU time, Linux RSS, GPU memory, the QRhi of
// a QRhiWidget and a frame counter for any GPU widget. Every probe that the
// platform cannot answer returns a negative value, printed as "n/a".

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QRhiWidget>
#include <QSet>
#include <QString>
#include <QWidget>
#include <QWindow>
#ifdef PJ_RHI_WIDGETS
#include <rhi/qrhi.h>
#else
#include <QOpenGLWidget>  // the GL build's GPU widgets; the QRhi build's need not link QtOpenGLWidgets
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <ctime>
#endif

namespace pj_bench {

/// CPU time of the calling thread (the GUI thread in every benchmark), ms.
inline double threadCpuMs() {
#ifdef Q_OS_WIN
  FILETIME creation{};
  FILETIME exit{};
  FILETIME kernel{};
  FILETIME user{};
  GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user);
  const auto ticks = [](const FILETIME& t) { return (std::uint64_t{t.dwHighDateTime} << 32U) | t.dwLowDateTime; };
  return static_cast<double>(ticks(kernel) + ticks(user)) / 1.0e4;  // 100 ns ticks
#else
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<double>(now.tv_sec) * 1.0e3 + static_cast<double>(now.tv_nsec) / 1.0e6;
#endif
}

/// A "Vm*:" field of /proc/self/status in MiB; negative off Linux.
inline double procStatusMib([[maybe_unused]] const char* field) {
#ifdef Q_OS_LINUX
  QFile status(QStringLiteral("/proc/self/status"));
  if (!status.open(QIODevice::ReadOnly)) {
    return -1.0;
  }
  for (const QByteArray& line : status.readAll().split('\n')) {
    if (line.startsWith(field)) {
      return line.mid(static_cast<int>(qstrlen(field))).trimmed().split(' ').value(0).toDouble() / 1024.0;
    }
  }
#endif
  return -1.0;
}
inline double rssMib() {
  return procStatusMib("VmRSS:");
}
/// Peak RSS since the last resetPeakRss().
inline double peakRssMib() {
  return procStatusMib("VmHWM:");
}
/// Resets the peak RSS (clear_refs 5).
inline void resetPeakRss() {
#ifdef Q_OS_LINUX
  QFile clear(QStringLiteral("/proc/self/clear_refs"));
  if (clear.open(QIODevice::WriteOnly)) {
    clear.write("5");
  }
#endif
}

/// GPU memory the kernel's DRM driver charges to this process, summed over its
/// DRM clients' "drm-total-*" regions (Linux fdinfo: i915, xe, amdgpu, ...),
/// MiB. The same number for every graphics API; negative when no client reports
/// one (software rasterizers, the NVIDIA driver, other platforms).
inline double drmTotalMib() {
#ifdef Q_OS_LINUX
  const QDir dir(QStringLiteral("/proc/self/fdinfo"));
  QSet<QByteArray> clients;
  double total_kib = 0.0;
  bool reported = false;
  for (const QString& fd : dir.entryList(QDir::Files)) {
    QFile info(dir.filePath(fd));
    if (!info.open(QIODevice::ReadOnly)) {
      continue;
    }
    const QList<QByteArray> lines = info.readAll().split('\n');
    QByteArray client;
    double kib = 0.0;
    bool has_total = false;
    for (const QByteArray& line : lines) {
      const qsizetype colon = line.indexOf(':');
      if (colon < 0) {
        continue;
      }
      const QByteArray key = line.left(colon);
      const QList<QByteArray> value = line.mid(colon + 1).simplified().split(' ');
      if (key == "drm-client-id") {
        client = value.value(0);
      } else if (key.startsWith("drm-total-")) {
        const QByteArray unit = value.value(1);
        const double scale = unit == "KiB"   ? 1.0
                             : unit == "MiB" ? 1024.0
                             : unit == "GiB" ? 1024.0 * 1024.0
                                             : 1.0 / 1024.0;
        kib += value.value(0).toDouble() * scale;
        has_total = true;
      }
    }
    if (!client.isEmpty() && has_total && !clients.contains(client)) {
      clients.insert(client);
      total_kib += kib;
      reported = true;
    }
  }
  return reported ? total_kib / 1024.0 : -1.0;
#else
  return -1.0;
#endif
}

#ifdef PJ_RHI_WIDGETS
/// The QRhi `widget` renders with (its top-level's). QRhiWidget::rhi() is
/// protected; a member pointer named through a subclass that re-publishes it
/// reads it without touching the widget's own type.
struct RhiWidgetAccess : QRhiWidget {
  using QRhiWidget::rhi;
};
inline QRhi* rhiOf(const QRhiWidget& widget) {
  return (widget.*(&RhiWidgetAccess::rhi))();
}

/// Device memory `rhi`'s allocator has handed out (Vulkan's VMA; D3D12 reports
/// its adapter total), MiB: every texture and buffer of every QRhiWidget in that
/// top-level, not its swapchain nor the allocator's unused block space. Negative
/// on backends without one (OpenGL, D3D11, Metal).
inline double rhiAllocatorMib(QRhi* rhi) {
  if (rhi == nullptr) {
    return -1.0;
  }
  const QRhiStats stats = rhi->statistics();
  if (stats.totalUsageBytes > 0) {
    return static_cast<double>(stats.totalUsageBytes) / (1024.0 * 1024.0);
  }
  return stats.blockCount == 0 ? -1.0 : static_cast<double>(stats.usedBytes) / (1024.0 * 1024.0);
}
#endif

/// "12.3" or "n/a" for a negative probe.
inline QByteArray fmt(double value, int decimals = 1) {
  return value < 0.0 ? QByteArray("n/a") : QByteArray::number(value, 'f', decimals);
}

/// Counts the frames a GPU widget finished: QRhiWidget::frameSubmitted, or in
/// the GL build QOpenGLWidget::frameSwapped. Anything else (a raster widget)
/// never counts; see valid().
class FrameCounter {
 public:
  explicit FrameCounter(QWidget* widget) {
    if (auto* rhi_widget = qobject_cast<QRhiWidget*>(widget); rhi_widget != nullptr) {
      connection_ = QObject::connect(rhi_widget, &QRhiWidget::frameSubmitted, [this] { ++count_; });
    }
#ifndef PJ_RHI_WIDGETS
    if (auto* gl_widget = qobject_cast<QOpenGLWidget*>(widget); gl_widget != nullptr) {
      connection_ = QObject::connect(gl_widget, &QOpenGLWidget::frameSwapped, [this] { ++count_; });
    }
#endif
  }
  ~FrameCounter() {
    QObject::disconnect(connection_);
  }
  FrameCounter(const FrameCounter&) = delete;
  FrameCounter& operator=(const FrameCounter&) = delete;

  [[nodiscard]] std::uint64_t count() const {
    return count_;
  }
  [[nodiscard]] bool valid() const {
    return static_cast<bool>(connection_);
  }

 private:
  std::uint64_t count_ = 0;
  QMetaObject::Connection connection_;
};

/// Runs the event loop until `done()` or `timeout_ms`; false on timeout.
inline bool waitUntil(const std::function<bool()>& done, int timeout_ms = 10000) {
  QElapsedTimer timer;
  timer.start();
  while (!done()) {
    if (timer.elapsed() > timeout_ms) {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
  }
  return true;
}

/// Runs the event loop until `frames()` stops moving for `quiet_ms` (at most
/// `timeout_ms`), then returns how many frames it adds over `window_ms` more:
/// the idle-frame count of a settled view.
inline std::uint64_t idleFrames(
    const std::function<std::uint64_t()>& frames, int quiet_ms = 1000, int timeout_ms = 15000, int window_ms = 1000) {
  QElapsedTimer total;
  total.start();
  QElapsedTimer quiet;
  quiet.start();
  std::uint64_t last = frames();
  while (quiet.elapsed() < quiet_ms && total.elapsed() < timeout_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    if (frames() != last) {
      last = frames();
      quiet.restart();
    }
  }
  const std::uint64_t start = frames();
  QElapsedTimer window;
  window.start();
  while (window.elapsed() < window_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  }
  return frames() - start;
}

/// Median, min and max of `values` (0 when empty).
struct Stats {
  double median = 0.0;
  double min = 0.0;
  double max = 0.0;
};
inline Stats stats(std::vector<double> values) {
  if (values.empty()) {
    return {};
  }
  std::sort(values.begin(), values.end());
  return {values[values.size() / 2], values.front(), values.back()};
}

}  // namespace pj_bench
