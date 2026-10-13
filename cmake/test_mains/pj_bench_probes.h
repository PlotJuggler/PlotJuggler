// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Header-only process probes of every frame benchmark (target pj_bench_probes:
// scene3d_frame_benchmark, pj_app's mixed_layout_benchmark, pj_plotting's
// plot_canvas_benchmark): Linux RSS, GPU memory, the QRhi of a QRhiWidget and a
// frame counter for any GPU widget. Every probe that the platform cannot answer
// returns a negative value, printed as "n/a". The Qt-free GUI-thread CPU clock and
// the sample statistics live in pj_bench_stats.h, included here.

#include <rhi/qrhi.h>

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
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "pj_bench_stats.h"

#ifndef Q_OS_WIN
#include <sys/resource.h>
#endif

namespace pj_bench {

/// std::chrono::steady_clock now, ns: the clock of the views' test-hook stage stamps.
inline std::int64_t steadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/// Minor page faults of the whole process so far (every thread: decode workers
/// and the driver's too); negative off Linux/Unix.
inline long long minorFaults() {
#ifdef Q_OS_WIN
  return -1;
#else
  rusage usage{};
  return getrusage(RUSAGE_SELF, &usage) == 0 ? static_cast<long long>(usage.ru_minflt) : -1;
#endif
}

/// Mean current frequency of the CPUs cpufreq reports, MHz; negative when none
/// does (off Linux, some VMs).
inline double cpuMhz() {
  double sum_khz = 0.0;
  int count = 0;
#ifdef Q_OS_LINUX
  for (int cpu = 0;; ++cpu) {
    QFile freq(QStringLiteral("/sys/devices/system/cpu/cpu%1/cpufreq/scaling_cur_freq").arg(cpu));
    if (!freq.open(QIODevice::ReadOnly)) {
      break;
    }
    sum_khz += freq.readAll().trimmed().toDouble();
    ++count;
  }
#endif
  return count > 0 ? sum_khz / count / 1000.0 : -1.0;
}

/// The GPU's actual clock (i915 `gt_act_freq_mhz` of the first DRM card that
/// has one), MHz; negative where none is readable (other drivers, platforms).
inline double gpuMhz() {
#ifdef Q_OS_LINUX
  static const QString path = [] {
    for (int card = 0; card < 8; ++card) {
      const QString candidate = QStringLiteral("/sys/class/drm/card%1/gt_act_freq_mhz").arg(card);
      if (QFile::exists(candidate)) {
        return candidate;
      }
    }
    return QString();
  }();
  QFile freq(path);
  if (!path.isEmpty() && freq.open(QIODevice::ReadOnly)) {
    return freq.readAll().trimmed().toDouble();
  }
#endif
  return -1.0;
}

/// Warm-up of a frame loop: call done(frame_ms) after each warm-up frame. True
/// once frames ran for at least `min_ms`, the last 5 frames' wall times are
/// within 10% of each other and the GPU clock (gpuMhz(), where readable) moved
/// less than 5% over the last `settle_ms`; always true after `max_frames` frames
/// or `max_ms`. The rule waits on time and the clock, not a frame count, because
/// a GPU idle before the loop sits at its lowest clock and ramps up over several
/// frames (i915: 0 -> 600 -> 1500 MHz). i915 may also hold a light load at an
/// intermediate clock for a while and raise it later, inside the measured
/// frames: no warm-up rules that out, so report the clock over the measured
/// frames too. coldMs() is the first frame's wall time, the cold-after-idle
/// latency a user sees on a first interaction (reported apart as cold_ms).
class WarmUp {
 public:
  explicit WarmUp(int min_ms = 250, int settle_ms = 100, int max_frames = 120, int max_ms = 2000)
      : min_ms_(min_ms), settle_ms_(settle_ms), max_frames_(max_frames), max_ms_(max_ms) {
    timer_.start();
    settled_.start();
    mhz_ = gpuMhz();
  }
  [[nodiscard]] bool done(double frame_ms) {
    if (frames_ == 0) {
      cold_ms_ = frame_ms;
    }
    last_ms_[static_cast<std::size_t>(frames_ % kSteadyFrames)] = frame_ms;
    ++frames_;
    const double mhz = gpuMhz();
    last_mhz_ = mhz;
    if (mhz >= 0.0 && std::abs(mhz - mhz_) > 0.05 * std::max(mhz_, 100.0)) {
      mhz_ = mhz;
      settled_.restart();
    }
    const qint64 elapsed = timer_.elapsed();
    if (elapsed >= max_ms_ || frames_ >= max_frames_) {
      return true;
    }
    const auto [fastest, slowest] = std::minmax_element(last_ms_.begin(), last_ms_.end());
    const bool steady = frames_ >= kSteadyFrames && *slowest <= 1.1 * *fastest;
    return elapsed >= min_ms_ && steady && (mhz < 0.0 || settled_.elapsed() >= settle_ms_);
  }
  [[nodiscard]] int frames() const {
    return frames_;
  }
  /// The first frame's wall time; negative before it.
  [[nodiscard]] double coldMs() const {
    return cold_ms_;
  }
  [[nodiscard]] double elapsedMs() const {
    return static_cast<double>(timer_.nsecsElapsed()) / 1.0e6;
  }
  /// The GPU clock when the warm-up ended (negative where unreadable).
  [[nodiscard]] double gpuMhzAtEnd() const {
    return last_mhz_;
  }

 private:
  static constexpr int kSteadyFrames = 5;
  int min_ms_;
  int settle_ms_;
  int max_frames_;
  int max_ms_;
  int frames_ = 0;
  std::array<double, kSteadyFrames> last_ms_{};
  double mhz_ = -1.0;  // the clock the settle window started at
  double last_mhz_ = -1.0;
  double cold_ms_ = -1.0;
  QElapsedTimer timer_;
  QElapsedTimer settled_;
};

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

/// "12.3" or "n/a" for a negative probe.
inline QByteArray fmt(double value, int decimals = 1) {
  return value < 0.0 ? QByteArray("n/a") : QByteArray::number(value, 'f', decimals);
}

/// Counts the frames a GPU widget finished (QRhiWidget::frameSubmitted) and
/// stamps the last one (steadyNs()). Anything else (a raster widget) never counts; see valid().
class FrameCounter {
 public:
  explicit FrameCounter(QWidget* widget) {
    if (auto* rhi_widget = qobject_cast<QRhiWidget*>(widget); rhi_widget != nullptr) {
      connection_ = QObject::connect(rhi_widget, &QRhiWidget::frameSubmitted, [this] { finish(); });
    }
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
  [[nodiscard]] std::int64_t lastFinishedNs() const {
    return last_ns_;
  }

 private:
  void finish() {
    ++count_;
    last_ns_ = steadyNs();
  }

  std::uint64_t count_ = 0;
  std::int64_t last_ns_ = 0;
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

}  // namespace pj_bench
