// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// GUI-thread CPU of the Log panel (LogDockWidget over a real SessionManager / ObjectStore):
//
//   QT_QPA_PLATFORM=offscreen log_panel_benchmark [--only playback|streaming|oneoff] [--stream-seconds 10]
//
// playback: 1e4 and 1e5 kLog entries over a 10-minute log, in 1 or 3 merged topics, future rows greyed or hidden,
// with and without a message filter. A precise 33 ms timer (PlaybackEngine's tick) moves the tracker at 1x while the
// view follows it (setPlaying), through 10 windows spread over the log; each sample is the GUI-thread CPU from one
// tick to the next, so it holds the tracker update, the auto-scroll and the paint.
//
// streaming: a producer thread pushes entries into the ObjectStore at a fixed rate, and a 50 ms GUI timer announces
// them with notifyIngest(live) after setting the retention budget, as StreamingSourceManager's ingest tick does (5 s
// window, its default, and 64 MiB); the tracker follows the newest stamp at 30 Hz. "late" stamps each entry up to
// 50 ms in the past, so a topic is rewritten and the panel reloads it. A refresh sample is the CPU of the GUI event
// in which the model took rows (the 250 ms trailing edge, or the notifyIngest of a leading one).
//
// oneoff: dropping a loaded 1e5-entry topic on a shown panel, and one filter change over 1e5 rows; each includes
// 30 ms of event processing for the paint.
//
// One LOG_BENCH line per run, then a summary table. Built with PJ_BUILD_DEMOS; no ctest.

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "pj_base/builtin/log_codec.hpp"
#include "pj_base/dataset.hpp"
#include "pj_bench_probes.h"
#include "pj_datastore/engine.hpp"
#include "pj_log/LogDockWidget.h"
#include "pj_log/LogView.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr Timestamp kNs = 1'000'000'000;
constexpr Timestamp kEpochNs = 1'763'600'000 * kNs;
constexpr auto kLogMetadata = R"({"builtin_object_type":"kLog"})";
constexpr int kTickMs = 33;                  // PlaybackEngine's kTickIntervalMs
constexpr int kIngestTickMs = 50;            // StreamingSourceManager's kPollPeriodMs
constexpr Timestamp kRetentionNs = 5 * kNs;  // StreamingSourceManager's default buffer
constexpr size_t kObjectBudgetBytes = 64U * 1024U * 1024U;
constexpr double kPlaybackSpanS = 600.0;
constexpr const char* kFilterText = "timeout";

/// Times the top-level events of the GUI thread (CPU of the thread, nested events included).
class TimedApplication final : public QApplication {
 public:
  using QApplication::QApplication;
  std::function<void(double cpu_ms)> on_top_event;

  bool notify(QObject* receiver, QEvent* event) override {
    if (depth_ > 0 || !on_top_event) {
      return QApplication::notify(receiver, event);
    }
    ++depth_;
    const double start = pj_bench::threadCpuMs();
    const bool result = QApplication::notify(receiver, event);
    const double cpu = pj_bench::threadCpuMs() - start;
    --depth_;
    on_top_event(cpu);
    return result;
  }

 private:
  int depth_ = 0;
};

/// Realistic entries: mixed levels, six sources, a few categories, messages of 40-200 chars; one phrase in 20 holds
/// kFilterText.
class LogGenerator {
 public:
  explicit LogGenerator(unsigned seed) : rng_(seed) {}

  std::vector<uint8_t> next(Timestamp stamp) {
    static constexpr std::array<const char*, 6> kSources = {"/planner",      "/controller",   "/camera_driver",
                                                            "/lidar_driver", "/localization", "/battery_monitor"};
    static constexpr std::array<const char*, 4> kCategories = {"", "nav", "perception", "hw"};
    static constexpr std::array<const char*, 20> kPhrases = {
        "Replanning around a new obstacle in the costmap",
        "Tracking error within bounds",
        "Frame dropped: the publisher queue is full",
        "Scan received with fewer points than expected",
        "Localization covariance is growing",
        "Battery voltage reading",
        "Waiting for transform map -> base_link: timeout",
        "Goal accepted by the action server",
        "Velocity command clamped to the configured limit",
        "Parameter update applied",
        "Lifecycle transition: configuring -> inactive",
        "Received an empty point cloud",
        "Recovery behavior started: clearing the local costmap",
        "Controller frequency below target",
        "Map server loaded the occupancy grid",
        "IMU message out of order, discarded",
        "Path found with cost",
        "Motor driver reports a high temperature",
        "Diagnostics aggregator: all subsystems OK",
        "Camera exposure adjusted",
    };
    std::uniform_int_distribution<size_t> phrase(0, kPhrases.size() - 1);
    std::uniform_int_distribution<size_t> source(0, kSources.size() - 1);
    std::uniform_int_distribution<int> length(40, 200);
    std::uniform_int_distribution<int> number(0, 99999);
    std::discrete_distribution<int> level({0, 30, 50, 12, 7, 1});
    std::discrete_distribution<size_t> category({70, 10, 10, 10});

    sdk::Log log;
    log.timestamp_ns = stamp;
    log.level = static_cast<sdk::Log::Level>(level(rng_));
    log.source = kSources[source(rng_)];
    log.category = kCategories[category(rng_)];
    log.message = kPhrases[phrase(rng_)];
    const auto target = static_cast<size_t>(length(rng_));
    static constexpr std::array<const char*, 5> kKeys = {" id=", " x=", " y=", " seq=", " dt_ms="};
    for (size_t k = 0; log.message.size() < target; ++k) {
      log.message += kKeys[k % kKeys.size()];
      log.message += std::to_string(number(rng_));
    }
    log.message.resize(target);
    return serializeLog(log);
  }

 private:
  std::mt19937 rng_;
};

struct Fixture {
  SessionManager session;
  PlaybackEngine playback;
  DatasetId dataset = 0;
  std::vector<ObjectTopicId> topics;

  explicit Fixture(int topic_count) {
    const auto domain = session.dataEngine().createTimeDomain("robot.mcap");
    dataset =
        *session.dataEngine().createDataset(DatasetDescriptor{.source_name = "robot.mcap", .time_domain_id = *domain});
    for (int i = 0; i < topic_count; ++i) {
      topics.push_back(*session.objectStore().registerTopic(
          ObjectTopicDescriptor{
              .dataset_id = dataset, .topic_name = "/rosout_" + std::to_string(i), .metadata_json = kLogMetadata}));
    }
  }

  /// `count` entries spread evenly over kPlaybackSpanS, dealt round-robin to the topics.
  void fill(size_t count) {
    LogGenerator gen(42);
    const auto spacing = static_cast<Timestamp>(kPlaybackSpanS * static_cast<double>(kNs) / static_cast<double>(count));
    for (size_t i = 0; i < count; ++i) {
      const Timestamp stamp = kEpochNs + static_cast<Timestamp>(i) * spacing;
      (void)session.objectStore().pushOwned(topics[i % topics.size()], stamp, gen.next(stamp));
    }
  }
};

std::unique_ptr<LogDockWidget> makeShownDock(Fixture& fixture) {
  auto dock = std::make_unique<LogDockWidget>(&fixture.session, nullptr, &fixture.playback);
  dock->resize(1280, 720);
  dock->show();
  (void)pj_bench::waitUntil([&] { return dock->windowHandle() != nullptr && dock->windowHandle()->isExposed(); });
  return dock;
}

void setView(LogView& view, bool show_future, bool filter) {
  LogViewState state = view.state();
  state.show_future = show_future;
  state.text = filter ? QString::fromLatin1(kFilterText) : QString();
  view.applyState(state);
}

/// Runs the event loop for `ms` (paints, deferred layouts).
void spin(int ms) {
  QEventLoop loop;  // sleeps when idle, so the thread CPU it adds is only the events it ran
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

std::vector<std::string> g_summary;

void report(const QByteArray& line, const QByteArray& summary) {
  std::printf("LOG_BENCH %s\n", line.constData());
  std::fflush(stdout);
  g_summary.push_back(summary.toStdString());
}

double mean(const std::vector<double>& values) {
  return values.empty() ? 0.0 : std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

void runPlayback(size_t entries, int topic_count, bool show_future, bool filter) {
  Fixture fixture(topic_count);
  fixture.fill(entries);
  auto dock = makeShownDock(fixture);
  setView(*dock->view(), show_future, filter);
  for (const ObjectTopicId topic : fixture.topics) {
    (void)dock->tryAcceptObjectTopic(topic, sdk::BuiltinObjectType::kLog, {});
  }
  dock->view()->setPlaying(true);
  const auto& store = dock->view()->store();
  const double t_min = store.row(0).time;
  const double t_max = store.row(store.size() - 1).time;
  spin(200);

  constexpr int kWindows = 10;
  constexpr int kTicksPerWindow = 30;
  std::vector<double> samples;
  int window = 0;
  int tick = 0;
  double last_cpu = 0.0;
  QEventLoop loop;
  QTimer timer;
  timer.setTimerType(Qt::PreciseTimer);
  timer.setInterval(kTickMs);
  QObject::connect(&timer, &QTimer::timeout, [&] {
    const double now = pj_bench::threadCpuMs();
    if (tick > 1) {  // tick 1 follows a seek: not a playback step
      samples.push_back(now - last_cpu);
    }
    if (tick == kTicksPerWindow) {
      tick = 0;
      if (++window == kWindows) {
        loop.quit();
        return;
      }
    }
    const double start = t_min + (t_max - t_min) * (window + 0.05) / kWindows;
    ++tick;
    last_cpu = pj_bench::threadCpuMs();
    dock->onTrackerTime(start + tick * kTickMs / 1000.0);
  });
  timer.start();
  loop.exec();

  const pj_bench::Stats s = pj_bench::stats(samples);
  const double pct = mean(samples) / kTickMs * 100.0;
  const char* mode = show_future ? "grey" : "hide";
  report(
      QByteArray("scenario=playback entries=") + QByteArray::number(qulonglong(entries)) +
          " topics=" + QByteArray::number(topic_count) + " future=" + mode +
          " filter=" + (filter ? kFilterText : "none") + " ticks=" + QByteArray::number(samples.size()) + " rows=" +
          QByteArray::number(dock->view()->model()->rowCount()) + " tick_cpu_ms_median=" + pj_bench::fmt(s.median, 3) +
          " tick_cpu_ms_p95=" + pj_bench::fmt(s.p95, 3) + " tick_cpu_ms_max=" + pj_bench::fmt(s.max, 3) +
          " cpu_pct_at_30hz=" + pj_bench::fmt(pct, 1) + " rss_mib=" + pj_bench::fmt(pj_bench::rssMib()),
      QByteArray("playback  ") + QByteArray::number(qulonglong(entries)).leftJustified(7) +
          " topics=" + QByteArray::number(topic_count) + " " + mode + " filter=" + (filter ? "on " : "off") +
          " | tick ms med " + pj_bench::fmt(s.median, 2) + " p95 " + pj_bench::fmt(s.p95, 2) + " max " +
          pj_bench::fmt(s.max, 2) + " | " + pj_bench::fmt(pct, 1) + " % core");
}

void runStreaming(TimedApplication& app, int rate, bool late, int seconds) {
  Fixture fixture(1);
  const ObjectTopicId topic = fixture.topics.front();
  auto dock = makeShownDock(fixture);
  (void)dock->tryAcceptObjectTopic(topic, sdk::BuiltinObjectType::kLog, {});
  dock->view()->setPlaying(true);
  spin(200);

  bool touched = false;
  bool removed = false;
  auto* model = dock->view()->model();
  const QObject guard;  // drops the connections before the locals they capture go
  QObject::connect(model, &QAbstractItemModel::rowsInserted, &guard, [&] { touched = true; });
  QObject::connect(model, &QAbstractItemModel::rowsRemoved, &guard, [&] { touched = removed = true; });
  QObject::connect(model, &QAbstractItemModel::modelReset, &guard, [&] { touched = removed = true; });
  std::vector<double> refreshes;
  int reloads = 0;
  app.on_top_event = [&](double cpu_ms) {
    if (touched) {
      refreshes.push_back(cpu_ms);
      reloads += removed ? 1 : 0;
      touched = removed = false;
    }
  };

  const long total = static_cast<long>(rate) * seconds;
  std::atomic<Timestamp> newest{0};
  std::atomic<long> pushed{0};
  const auto start = std::chrono::steady_clock::now();
  std::thread producer([&] {
    LogGenerator gen(7);
    std::mt19937 rng(11);
    std::uniform_int_distribution<Timestamp> lateness(0, late ? 50'000'000 : 0);
    while (pushed.load() < total) {
      const Timestamp elapsed =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
      const long due = std::min(total, static_cast<long>(static_cast<double>(elapsed) * rate / kNs));
      for (long n = pushed.load(); n < due; ++n) {
        const Timestamp stamp = kEpochNs + elapsed - lateness(rng);
        (void)fixture.session.objectStore().pushOwned(topic, stamp, gen.next(stamp));
        newest.store(std::max(newest.load(), stamp));
        pushed.store(n + 1);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  QTimer ingest;
  ingest.setInterval(kIngestTickMs);
  QObject::connect(&ingest, &QTimer::timeout, [&] {
    fixture.session.objectStore().setRetentionBudget(
        topic, RetentionBudget{.time_window_ns = kRetentionNs, .max_memory_bytes = kObjectBudgetBytes});
    fixture.session.notifyIngest({}, /*live=*/true, fixture.dataset);
  });
  QTimer tracker;
  tracker.setTimerType(Qt::PreciseTimer);
  tracker.setInterval(kTickMs);
  QObject::connect(&tracker, &QTimer::timeout, [&] {
    if (const Timestamp stamp = newest.load(); stamp != 0) {
      dock->onTrackerTime(
          toAxisDouble(rawToDisplaySecondsAtOrAfter(stamp, fixture.session.displayOffset(fixture.dataset))));
    }
  });

  QElapsedTimer wall;
  wall.start();
  const double cpu_start = pj_bench::threadCpuMs();
  ingest.start();
  tracker.start();
  QEventLoop loop;
  QTimer::singleShot(seconds * 1000 + 600, &loop, &QEventLoop::quit);  // the last trailing refresh lands too
  loop.exec();
  const double cpu = pj_bench::threadCpuMs() - cpu_start;
  const double wall_ms = static_cast<double>(wall.elapsed());
  app.on_top_event = nullptr;
  producer.join();

  size_t retained = 0;
  fixture.session.objectStore().forEachTopicRetained(
      [&](ObjectTopicId id, const ObjectTopicDescriptor&, const ObjectStore::RetainedRange& range) {
        if (id == topic) {
          retained = range.entry_count;
        }
      });
  const pj_bench::Stats s = pj_bench::stats(refreshes);
  const double pct = cpu / wall_ms * 100.0;
  const char* order = late ? "late50ms" : "in_order";
  report(
      QByteArray("scenario=streaming rate=") + QByteArray::number(rate) + " order=" + order +
          " seconds=" + QByteArray::number(seconds) + " pushed=" + QByteArray::number(qlonglong(pushed.load())) +
          " gui_cpu_pct=" + pj_bench::fmt(pct, 1) + " refreshes=" + QByteArray::number(refreshes.size()) +
          " reloads=" + QByteArray::number(reloads) + " refresh_cpu_ms_median=" + pj_bench::fmt(s.median, 3) +
          " refresh_cpu_ms_p95=" + pj_bench::fmt(s.p95, 3) + " refresh_cpu_ms_max=" + pj_bench::fmt(s.max, 3) +
          " shown=" + QByteArray::number(qulonglong(dock->view()->store().size())) + " store_retained=" +
          QByteArray::number(qulonglong(retained)) + " rss_mib=" + pj_bench::fmt(pj_bench::rssMib()),
      QByteArray("streaming ") + QByteArray::number(rate).leftJustified(6) + " msg/s " +
          QByteArray(order).leftJustified(8) + " | refresh ms med " + pj_bench::fmt(s.median, 2) + " p95 " +
          pj_bench::fmt(s.p95, 2) + " max " + pj_bench::fmt(s.max, 2) + " (" + QByteArray::number(refreshes.size()) +
          ", " + QByteArray::number(reloads) + " reloads) | " + pj_bench::fmt(pct, 1) + " % core | shown " +
          QByteArray::number(qulonglong(dock->view()->store().size())));
}

void runOneOff() {
  constexpr size_t kEntries = 100'000;
  constexpr int kRepeats = 5;
  std::vector<double> drop_cpu;
  std::vector<double> drop_wall;
  {
    Fixture fixture(1);
    fixture.fill(kEntries);
    for (int i = 0; i < kRepeats; ++i) {
      auto dock = makeShownDock(fixture);
      spin(100);
      QElapsedTimer wall;
      wall.start();
      const double cpu = pj_bench::threadCpuMs();
      (void)dock->tryAcceptObjectTopic(fixture.topics.front(), sdk::BuiltinObjectType::kLog, {});
      drop_wall.push_back(static_cast<double>(wall.nsecsElapsed()) / 1e6);
      spin(30);
      drop_cpu.push_back(pj_bench::threadCpuMs() - cpu);
    }
  }
  const pj_bench::Stats drop = pj_bench::stats(drop_cpu);
  const pj_bench::Stats drop_call = pj_bench::stats(drop_wall);
  report(
      QByteArray("scenario=drop entries=") + QByteArray::number(qulonglong(kEntries)) +
          " repeats=" + QByteArray::number(kRepeats) + " call_wall_ms_median=" + pj_bench::fmt(drop_call.median, 1) +
          " cpu_ms_with_paint_median=" + pj_bench::fmt(drop.median, 1) + " cpu_ms_max=" + pj_bench::fmt(drop.max, 1) +
          " rss_mib=" + pj_bench::fmt(pj_bench::rssMib()),
      QByteArray("drop      100000 entries, 1 topic          | call ") + pj_bench::fmt(drop_call.median, 1) +
          " ms wall, " + pj_bench::fmt(drop.median, 1) + " ms CPU with paint");

  Fixture fixture(1);
  fixture.fill(kEntries);
  auto dock = makeShownDock(fixture);
  (void)dock->tryAcceptObjectTopic(fixture.topics.front(), sdk::BuiltinObjectType::kLog, {});
  dock->onTrackerTime(dock->view()->store().row(kEntries / 2).time);
  spin(100);
  std::vector<double> set_cpu;
  std::vector<double> clear_cpu;
  for (int i = 0; i < kRepeats; ++i) {
    for (const bool filter : {true, false}) {
      const double cpu = pj_bench::threadCpuMs();
      setView(*dock->view(), true, filter);  // what the debounce timer's applyFilters runs
      spin(30);
      (filter ? set_cpu : clear_cpu).push_back(pj_bench::threadCpuMs() - cpu);
    }
  }
  const pj_bench::Stats set = pj_bench::stats(set_cpu);
  const pj_bench::Stats clear = pj_bench::stats(clear_cpu);
  report(
      QByteArray("scenario=filter_change rows=") + QByteArray::number(qulonglong(kEntries)) +
          " repeats=" + QByteArray::number(kRepeats) + " set_cpu_ms_median=" + pj_bench::fmt(set.median, 1) +
          " set_cpu_ms_max=" + pj_bench::fmt(set.max, 1) + " clear_cpu_ms_median=" + pj_bench::fmt(clear.median, 1) +
          " clear_cpu_ms_max=" + pj_bench::fmt(clear.max, 1),
      QByteArray("filter    100000 rows                      | set ") + pj_bench::fmt(set.median, 1) + " ms, clear " +
          pj_bench::fmt(clear.median, 1) + " ms CPU with paint");
}

}  // namespace
}  // namespace PJ

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  PJ::TimedApplication app(argc, argv);
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOption({u"only"_s, u"playback, streaming or oneoff"_s, u"group"_s});
  parser.addOption({u"stream-seconds"_s, u"seconds per streaming run"_s, u"seconds"_s, u"10"_s});
  parser.process(app);
  const QString only = parser.value(u"only"_s);
  const int stream_seconds = std::max(1, parser.value(u"stream-seconds"_s).toInt());
  std::printf(
      "platform=%s cpu_mhz=%s\n", qPrintable(QGuiApplication::platformName()),
      pj_bench::fmt(pj_bench::cpuMhz(), 0).data());

  if (only.isEmpty() || only == u"playback"_s) {
    for (const size_t entries : {size_t{10'000}, size_t{100'000}}) {
      for (const int topics : {1, 3}) {
        for (const bool show_future : {true, false}) {
          for (const bool filter : {false, true}) {
            PJ::runPlayback(entries, topics, show_future, filter);
          }
        }
      }
    }
  }
  if (only.isEmpty() || only == u"streaming"_s) {
    for (const int rate : {100, 1000, 10000}) {
      PJ::runStreaming(app, rate, false, stream_seconds);
    }
    for (const int rate : {1000, 10000}) {
      PJ::runStreaming(app, rate, true, stream_seconds);
    }
  }
  if (only.isEmpty() || only == u"oneoff"_s) {
    PJ::runOneOff();
  }
  std::printf("\nSummary (GUI-thread CPU)\n");
  for (const std::string& line : PJ::g_summary) {
    std::printf("  %s\n", line.c_str());
  }
  return 0;
}
