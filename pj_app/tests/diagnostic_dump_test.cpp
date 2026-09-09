// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The --dump-diagnostics collector (stage-5 E2): accumulates EVERY diagnostic
// of the run from a QtDiagnosticBridge — including ones sunk BEFORE the
// collector attached (the bridge re-emits queued, so nothing is delivered
// until the event loop first spins) — and serializes them as a versioned
// envelope ({"version": 1, "records": [...]}) of stable five-field records.
// Deliberately NOT a DiagnosticHistory view: that ring caps at 200 and a
// long headless run must not silently truncate.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "DiagnosticDump.h"
#include "pj_base/diagnostic_sink.hpp"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_runtime/DiagnosticHistory.h"

namespace {

// The bridge delivers queued; spin the loop until `done` (or ~5s).
[[nodiscard]] bool pumpUntil(const std::function<bool()>& done) {
  const QDeadlineTimer deadline(5000);
  while (!done() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return done();
}

// The dump's top level is a versioned envelope — {"version": 1, "records":
// [...]} — so run-level metadata can be added compatibly later.
[[nodiscard]] QJsonObject readDumpEnvelope(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return QJsonDocument::fromJson(file.readAll()).object();
}

[[nodiscard]] QJsonArray readDumpRecords(const QString& path) {
  return readDumpEnvelope(path).value(QStringLiteral("records")).toArray();
}

TEST(DiagnosticDumpTest, CapturesPreAttachDiagnosticsWithAllFiveFields) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString dump_path = dir.filePath(QStringLiteral("diag.json"));

  PJ::QtDiagnosticBridge bridge;
  const PJ::DiagnosticSink sink = bridge.sink();
  PJ::DiagnosticDump dump(dump_path);

  // Sunk BEFORE attach: still queued (no loop ran yet), so the collector must
  // see it once the loop spins — the "from app start" guarantee.
  sink(PJ::Diagnostic{PJ::DiagnosticLevel::kWarning, "Layout", "layout-open-failed", "cannot open"});
  dump.attachTo(&bridge);
  sink(PJ::Diagnostic{PJ::DiagnosticLevel::kError, "Loader", "load-failed", "boom"});
  sink(PJ::Diagnostic{PJ::DiagnosticLevel::kInfo, "Layout", "loaded", "Loaded layout: x"});

  ASSERT_TRUE(pumpUntil([&dump]() { return dump.size() == 3; }))
      << "all three diagnostics (incl. the pre-attach one) must be collected, got " << dump.size();
  ASSERT_TRUE(dump.write());

  const QJsonObject envelope = readDumpEnvelope(dump_path);
  EXPECT_EQ(envelope.value(QStringLiteral("version")).toInt(), 1) << "the envelope carries the schema version";
  const QJsonArray records = envelope.value(QStringLiteral("records")).toArray();
  ASSERT_EQ(records.size(), 3);

  const QJsonObject first = records.at(0).toObject();
  EXPECT_EQ(first.value(QStringLiteral("level")).toString(), QStringLiteral("warning"));
  EXPECT_EQ(first.value(QStringLiteral("source")).toString(), QStringLiteral("Layout"));
  EXPECT_EQ(first.value(QStringLiteral("id")).toString(), QStringLiteral("layout-open-failed"));
  EXPECT_EQ(first.value(QStringLiteral("message")).toString(), QStringLiteral("cannot open"));
  const QString timestamp = first.value(QStringLiteral("timestamp")).toString();
  EXPECT_TRUE(QDateTime::fromString(timestamp, Qt::ISODateWithMs).isValid())
      << "timestamp must be ISO 8601 with milliseconds, got: " << timestamp.toStdString();

  EXPECT_EQ(records.at(1).toObject().value(QStringLiteral("level")).toString(), QStringLiteral("error"));
  EXPECT_EQ(records.at(1).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("load-failed"));
  EXPECT_EQ(records.at(2).toObject().value(QStringLiteral("level")).toString(), QStringLiteral("info"));
  EXPECT_EQ(records.at(2).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("loaded"));
}

TEST(DiagnosticDumpTest, DoesNotTruncateBeyondTheHistoryRingCap) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString dump_path = dir.filePath(QStringLiteral("many.json"));

  PJ::QtDiagnosticBridge bridge;
  const PJ::DiagnosticSink sink = bridge.sink();
  PJ::DiagnosticDump dump(dump_path);
  dump.attachTo(&bridge);

  // Strictly more than the 200-record DiagnosticHistory ring: the collector
  // must keep the WHOLE run.
  constexpr int count = PJ::DiagnosticHistory::kDefaultMaxRecords + 50;
  for (int i = 0; i < count; ++i) {
    sink(
        PJ::Diagnostic{PJ::DiagnosticLevel::kInfo, "Test", "bulk-" + std::to_string(i), "record " + std::to_string(i)});
  }
  ASSERT_TRUE(pumpUntil([&dump]() { return dump.size() == count; }));
  ASSERT_TRUE(dump.write());

  const QJsonArray records = readDumpRecords(dump_path);
  ASSERT_EQ(records.size(), count);
  EXPECT_EQ(records.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("bulk-0"));
  EXPECT_EQ(
      records.at(count - 1).toObject().value(QStringLiteral("id")).toString(),
      QStringLiteral("bulk-%1").arg(count - 1));
}

TEST(DiagnosticDumpTest, WriteDrainsStillQueuedDeliveriesFirst) {
  // The aboutToQuit shape: a diagnostic emitted moments before a
  // QCoreApplication::exit() is still a posted event when write() runs — the
  // serializer must drain the queue first or the tail of the run is lost.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString dump_path = dir.filePath(QStringLiteral("tail.json"));

  PJ::QtDiagnosticBridge bridge;
  const PJ::DiagnosticSink sink = bridge.sink();
  PJ::DiagnosticDump dump(dump_path);
  dump.attachTo(&bridge);

  sink(PJ::Diagnostic{PJ::DiagnosticLevel::kWarning, "Layout", "layout-open-failed", "cannot open"});
  ASSERT_EQ(dump.size(), 0) << "delivery is queued — nothing arrives before the loop (or write) runs";
  ASSERT_TRUE(dump.write());

  EXPECT_EQ(dump.size(), 1);
  const QJsonArray records = readDumpRecords(dump_path);
  ASSERT_EQ(records.size(), 1);
  EXPECT_EQ(records.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("layout-open-failed"));
}

TEST(DiagnosticDumpTest, WriteDrainsOnlyTheBridgeNeverUnrelatedPostedEvents) {
  // The quit-time drain is deliberately NARROW: on the --exit-after-layout
  // timeout path, unrelated queued work (e.g. a late batch `finished`) must
  // NOT run inside the aboutToQuit write — only the bridge's own re-emits.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());

  PJ::QtDiagnosticBridge bridge;
  const PJ::DiagnosticSink sink = bridge.sink();
  PJ::DiagnosticDump dump(dir.filePath(QStringLiteral("narrow.json")));
  dump.attachTo(&bridge);

  QObject other;
  bool unrelated_ran = false;
  QMetaObject::invokeMethod(&other, [&unrelated_ran]() { unrelated_ran = true; }, Qt::QueuedConnection);
  sink(PJ::Diagnostic{PJ::DiagnosticLevel::kError, "Batch", "late-event", "boom"});

  ASSERT_TRUE(dump.write());
  EXPECT_EQ(dump.size(), 1) << "the bridge's own queued delivery is drained";
  EXPECT_FALSE(unrelated_ran) << "unrelated posted events must not run during a quit-time write";

  // Hygiene: deliver the unrelated event now, while its captures are alive.
  QCoreApplication::processEvents();
  EXPECT_TRUE(unrelated_ran);
}

TEST(DiagnosticDumpTest, PumpBeforeAttachLosesEarlyDiagnostics) {
  // Characterization of the hazard main.cpp's attach point guards against
  // (passes by construction — it pins Qt's delivery semantics, not new
  // code): once ANY pump runs, queued bridge re-emits are delivered to
  // whoever is subscribed at that moment. Delivered-to-nobody is
  // unrecoverable — which is why main.cpp attaches the collector
  // IMMEDIATELY after MainWindow construction, before the splash wait or
  // --test-data can processEvents().
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());

  PJ::QtDiagnosticBridge bridge;
  const PJ::DiagnosticSink sink = bridge.sink();
  PJ::DiagnosticDump dump(dir.filePath(QStringLiteral("late.json")));

  sink(PJ::Diagnostic{PJ::DiagnosticLevel::kError, "Plugins", "plugin-load-failed", "early"});
  QCoreApplication::processEvents();  // the splash-wait analog: pump BEFORE attach
  dump.attachTo(&bridge);

  ASSERT_TRUE(dump.write());
  EXPECT_EQ(dump.size(), 0) << "a pre-attach pump loses the record for good — attach must come first";
}

TEST(DiagnosticDumpTest, WriteReportsFailureOnAnUnwritablePath) {
  // A regular file cannot be a parent directory, so <temp>/blocker/diag.json is
  // guaranteed unwritable on every platform — no assumption about host paths.
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  const QString blocker = QDir(temp.path()).filePath(QStringLiteral("blocker"));
  {
    QFile f(blocker);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  }
  PJ::DiagnosticDump dump(QDir(blocker).filePath(QStringLiteral("diag.json")));
  dump.record(
      static_cast<int>(PJ::DiagnosticLevel::kInfo), QStringLiteral("Test"), QStringLiteral("id"),
      QStringLiteral("msg"));
  EXPECT_FALSE(dump.write());
}

}  // namespace
