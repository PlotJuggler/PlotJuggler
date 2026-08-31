// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/download_manager.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <cstring>

#include "pj_marketplace/archive_limits.hpp"
using namespace Qt::StringLiterals;

namespace {

// Spins the event loop until spy receives at least one signal or timeout expires.
bool waitForSignal(QSignalSpy& spy, int timeout_ms = 5000) {
  QDeadlineTimer deadline(timeout_ms);
  while (spy.isEmpty() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return !spy.isEmpty();
}

// ---------------------------------------------------------------------------
// Minimal HTTP/1.1 server that serves a fixed body to all incoming requests.
// Binds to a random loopback port; no external network required.
// ---------------------------------------------------------------------------

class LocalHttpServer {
 public:
  LocalHttpServer() {
    server_.listen(QHostAddress::LocalHost, 0);
    QObject::connect(&server_, &QTcpServer::newConnection, [this]() {
      QTcpSocket* socket = server_.nextPendingConnection();
      socket->setParent(&server_);
      QObject::connect(socket, &QTcpSocket::readyRead, [this, socket]() {
        socket->readAll();  // consume the HTTP request
        const QByteArray header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: " +
            QByteArray::number(body_.size()) +
            "\r\n"
            "Connection: close\r\n\r\n";
        socket->write(header + body_);
        socket->flush();
        socket->disconnectFromHost();
      });
    });
  }

  QUrl url() const {
    return QUrl(u"http://127.0.0.1:%1/"_s.arg(server_.serverPort()));
  }

  void setBody(const QByteArray& body) {
    body_ = body;
  }

 private:
  QTcpServer server_;
  QByteArray body_;
};

// ---------------------------------------------------------------------------
// libarchive reports failures through return codes. Unchecked, a builder that
// silently produced a truncated or empty archive would surface downstream as
// "Could not open ZIP" and be mistaken for the policy under test rejecting it.
// ---------------------------------------------------------------------------

void requireArchiveOk(int status, struct archive* a, const char* what) {
  ASSERT_EQ(status, ARCHIVE_OK) << what << ": " << (archive_error_string(a) != nullptr ? archive_error_string(a) : "");
}

void requireBytesWritten(la_ssize_t written, qsizetype expected, struct archive* a) {
  ASSERT_EQ(written, static_cast<la_ssize_t>(expected))
      << "archive_write_data wrote " << written << " of " << expected
      << " bytes: " << (archive_error_string(a) != nullptr ? archive_error_string(a) : "");
}

// ---------------------------------------------------------------------------
// Helper: builds an in-memory ZIP from a map of {filename -> content}
// ---------------------------------------------------------------------------

QByteArray buildZip(const QMap<QString, QByteArray>& files) {
  std::vector<char> buffer(4 * 1024 * 1024);
  size_t used = 0;

  auto write_deleter = [](struct archive* a) { archive_write_free(a); };
  std::unique_ptr<struct archive, decltype(write_deleter)> a(archive_write_new(), write_deleter);

  requireArchiveOk(archive_write_set_format_zip(a.get()), a.get(), "set_format_zip");
  requireArchiveOk(archive_write_add_filter_none(a.get()), a.get(), "add_filter_none");
  requireArchiveOk(archive_write_open_memory(a.get(), buffer.data(), buffer.size(), &used), a.get(), "open_memory");

  auto entry_deleter = [](struct archive_entry* e) { archive_entry_free(e); };
  std::unique_ptr<struct archive_entry, decltype(entry_deleter)> entry(archive_entry_new(), entry_deleter);

  for (auto it = files.cbegin(); it != files.cend(); ++it) {
    archive_entry_clear(entry.get());
    archive_entry_set_pathname(entry.get(), it.key().toUtf8().constData());
    archive_entry_set_size(entry.get(), it.value().size());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    requireArchiveOk(archive_write_header(a.get(), entry.get()), a.get(), "write_header");
    requireBytesWritten(
        archive_write_data(a.get(), it.value().constData(), static_cast<size_t>(it.value().size())), it.value().size(),
        a.get());
  }

  requireArchiveOk(archive_write_close(a.get()), a.get(), "write_close");
  return QByteArray(buffer.data(), static_cast<int>(used));
}

static QString sha256Hex(const QByteArray& data) {
  return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

// ---------------------------------------------------------------------------
// Helper: builds a ZIP holding one entry of a caller-chosen type. buildZip()
// only emits regular files, and the entry-kind policy is precisely about what
// happens to everything else.
// ---------------------------------------------------------------------------

// filetype is __LA_MODE_T, libarchive's own alias: mode_t only where POSIX
// provides it, unsigned short on Windows. The AE_IF* constants carry that type.
QByteArray buildZipWithEntry(const QString& name, __LA_MODE_T filetype, const QByteArray& link_target) {
  std::vector<char> buffer(64 * 1024);
  size_t used = 0;

  auto write_deleter = [](struct archive* a) { archive_write_free(a); };
  std::unique_ptr<struct archive, decltype(write_deleter)> a(archive_write_new(), write_deleter);
  requireArchiveOk(archive_write_set_format_zip(a.get()), a.get(), "set_format_zip");
  requireArchiveOk(archive_write_add_filter_none(a.get()), a.get(), "add_filter_none");
  requireArchiveOk(archive_write_open_memory(a.get(), buffer.data(), buffer.size(), &used), a.get(), "open_memory");

  auto entry_deleter = [](struct archive_entry* e) { archive_entry_free(e); };
  std::unique_ptr<struct archive_entry, decltype(entry_deleter)> entry(archive_entry_new(), entry_deleter);
  archive_entry_set_pathname(entry.get(), name.toUtf8().constData());
  archive_entry_set_filetype(entry.get(), filetype);
  archive_entry_set_perm(entry.get(), 0644);
  archive_entry_set_size(entry.get(), 0);
  if (!link_target.isEmpty()) {
    archive_entry_set_symlink(entry.get(), link_target.constData());
  }
  requireArchiveOk(archive_write_header(a.get(), entry.get()), a.get(), "write_header");
  requireArchiveOk(archive_write_close(a.get()), a.get(), "write_close");
  return QByteArray(buffer.data(), static_cast<int>(used));
}

// ---------------------------------------------------------------------------
// Helper: builds a ZIP whose entries do NOT declare their size.
//
// Omitting archive_entry_set_size() makes libarchive defer the sizes to a
// descriptor after each payload, and padding the archive comment pushes the
// end-of-central-directory record out of the window the seekable reader scans —
// so the reader falls back to the streaming path and sees only the local
// headers, where the sizes are zero. Both conditions are required: either one
// alone still yields a declared size.
// ---------------------------------------------------------------------------

QByteArray buildZipWithoutDeclaredSizes(const QMap<QString, QByteArray>& files) {
  constexpr int kCommentBytes = 40000;  // must exceed the reader's trailing scan window
  std::vector<char> buffer(4 * 1024 * 1024 + kCommentBytes);
  size_t used = 0;

  auto write_deleter = [](struct archive* a) { archive_write_free(a); };
  std::unique_ptr<struct archive, decltype(write_deleter)> a(archive_write_new(), write_deleter);
  requireArchiveOk(archive_write_set_format_zip(a.get()), a.get(), "set_format_zip");
  requireArchiveOk(archive_write_add_filter_none(a.get()), a.get(), "add_filter_none");
  requireArchiveOk(
      archive_write_open_memory(a.get(), buffer.data(), buffer.size() - kCommentBytes, &used), a.get(), "open_memory");

  auto entry_deleter = [](struct archive_entry* e) { archive_entry_free(e); };
  std::unique_ptr<struct archive_entry, decltype(entry_deleter)> entry(archive_entry_new(), entry_deleter);
  for (auto it = files.cbegin(); it != files.cend(); ++it) {
    archive_entry_clear(entry.get());
    archive_entry_set_pathname(entry.get(), it.key().toUtf8().constData());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    // No archive_entry_set_size() on purpose.
    requireArchiveOk(archive_write_header(a.get(), entry.get()), a.get(), "write_header");
    requireBytesWritten(
        archive_write_data(a.get(), it.value().constData(), static_cast<size_t>(it.value().size())), it.value().size(),
        a.get());
  }
  requireArchiveOk(archive_write_close(a.get()), a.get(), "write_close");

  // Declare the comment in the EOCD, then append it.
  qsizetype eocd = -1;
  for (qsizetype i = static_cast<qsizetype>(used) - 22; i >= 0; --i) {
    if (std::memcmp(buffer.data() + i, "PK\x05\x06", 4) == 0) {
      eocd = i;
      break;
    }
  }
  EXPECT_GE(eocd, 0) << "end-of-central-directory record not found";
  if (eocd < 0) {
    return {};
  }
  buffer[static_cast<size_t>(eocd) + 20] = static_cast<char>(kCommentBytes & 0xff);
  buffer[static_cast<size_t>(eocd) + 21] = static_cast<char>((kCommentBytes >> 8) & 0xff);
  std::memset(buffer.data() + used, 'C', kCommentBytes);
  used += kCommentBytes;

  return QByteArray(buffer.data(), static_cast<int>(used));
}

// ---------------------------------------------------------------------------
// Helper: builds a ZIP whose entry understates its own size.
//
// libarchive's writer records whatever size it was told and then writes what it
// was given, so the inconsistency is patched in afterwards: the uncompressed
// size lives at +22 of the local file header and at +24 of the central
// directory, and a seekable reader consults the latter, so both must be lowered
// for the reader to believe the smaller number.
// ---------------------------------------------------------------------------

QByteArray buildZipUnderstatingSize(const QString& name, const QByteArray& content, uint32_t declared) {
  QByteArray zip = buildZip({{name, content}});

  const qsizetype local = zip.indexOf(QByteArrayLiteral("PK\x03\x04"));
  const qsizetype central = zip.indexOf(QByteArrayLiteral("PK\x01\x02"));
  EXPECT_GE(local, 0);
  EXPECT_GE(central, 0);
  if (local < 0 || central < 0) {
    return {};
  }
  for (int i = 0; i < 4; ++i) {
    const char byte = static_cast<char>((declared >> (8 * i)) & 0xff);
    zip[local + 22 + i] = byte;
    zip[central + 24 + i] = byte;
  }
  return zip;
}

// Drives one archive through the full pipeline under `limits` and reports whether
// it was refused *for the expected reason*.
//
// Matching the reason is the point: an unreadable archive, a network error or a
// different quota all produce a refusal, so asserting only that something failed
// would report the policy under test as working when it never ran.
testing::AssertionResult installIsRefused(
    const QByteArray& zip_data, const PJ::ArchiveLimits& limits, const QString& expected_reason) {
  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  dm.setArchiveLimits(limits);
  QTemporaryDir tmp;
  if (!tmp.isValid()) {
    return testing::AssertionFailure() << "could not create a temporary destination";
  }

  QSignalSpy failed_spy(&dm, &PJ::DownloadManager::failed);
  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);
  dm.fetch(server.url(), {}, tmp.path());

  if (!waitForSignal(failed_spy)) {
    return testing::AssertionFailure() << "the install was not refused";
  }
  if (!finished_spy.isEmpty()) {
    return testing::AssertionFailure() << "the install both failed and finished";
  }
  const QString reason = failed_spy.first().at(1).toString();
  if (!reason.contains(expected_reason)) {
    return testing::AssertionFailure() << "refused for the wrong reason\n  expected to contain: "
                                       << expected_reason.toStdString()
                                       << "\n  actual:              " << reason.toStdString();
  }
  return testing::AssertionSuccess();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(DownloadManagerTest, InvalidUrlEmitsFailed) {
  PJ::DownloadManager dm;
  QSignalSpy failed_spy(&dm, &PJ::DownloadManager::failed);
  QSignalSpy started_spy(&dm, &PJ::DownloadManager::started);

  const int id = dm.fetch(QUrl("http://255.255.255.255/nonexistent"), {}, {});

  EXPECT_TRUE(waitForSignal(started_spy));
  EXPECT_EQ(started_spy.first().at(0).toInt(), id);

  EXPECT_TRUE(waitForSignal(failed_spy));
  EXPECT_EQ(failed_spy.first().at(0).toInt(), id);
  EXPECT_FALSE(failed_spy.first().at(1).toString().isEmpty());
}

TEST(DownloadManagerTest, SuccessfulDownloadExtractsFiles) {
  const QByteArray zip_data = buildZip({{"hello.txt", "world"}});
  const QString checksum = u"sha256:"_s + sha256Hex(zip_data);

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);
  QSignalSpy failed_spy(&dm, &PJ::DownloadManager::failed);

  dm.fetch(server.url(), checksum, tmp.path());

  EXPECT_TRUE(waitForSignal(finished_spy));
  EXPECT_TRUE(failed_spy.isEmpty());
  EXPECT_TRUE(QFile::exists(tmp.path() + "/hello.txt"));
}

TEST(DownloadManagerTest, UppercaseChecksumIsAccepted) {
  const QByteArray zip_data = buildZip({{"hello.txt", "world"}});
  // Hex digests are case-insensitive; a registry may list it in uppercase.
  const QString checksum = QStringLiteral("sha256:") + sha256Hex(zip_data).toUpper();

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);
  QSignalSpy failed_spy(&dm, &PJ::DownloadManager::failed);

  dm.fetch(server.url(), checksum, tmp.path());

  EXPECT_TRUE(waitForSignal(finished_spy));
  EXPECT_TRUE(failed_spy.isEmpty());
  EXPECT_TRUE(QFile::exists(tmp.path() + "/hello.txt"));
}

TEST(DownloadManagerTest, UppercaseSha256PrefixIsAccepted) {
  const QByteArray zip_data = buildZip({{"hello.txt", "world"}});
  // The "sha256:" prefix itself is also matched case-insensitively; a hand-
  // authored registry entry may spell it "SHA256:".
  const QString checksum = QStringLiteral("SHA256:") + sha256Hex(zip_data).toUpper();

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);
  QSignalSpy failed_spy(&dm, &PJ::DownloadManager::failed);

  dm.fetch(server.url(), checksum, tmp.path());

  EXPECT_TRUE(waitForSignal(finished_spy));
  EXPECT_TRUE(failed_spy.isEmpty());
  EXPECT_TRUE(QFile::exists(tmp.path() + "/hello.txt"));
}

TEST(DownloadManagerTest, MixedCaseChecksumIsAccepted) {
  const QByteArray zip_data = buildZip({{"hello.txt", "world"}});
  // Every second hex digit uppercased so the check exercises a truly mixed
  // input rather than an all-upper or all-lower one — guards against a
  // well-meaning "normalise both sides with toLower()" refactor that would
  // still pass the all-upper test but subtly break other well-formed inputs.
  QString hex = sha256Hex(zip_data);
  for (int i = 0; i < hex.size(); i += 2) {
    hex[i] = hex[i].toUpper();
  }
  const QString checksum = QStringLiteral("sha256:") + hex;

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);
  QSignalSpy failed_spy(&dm, &PJ::DownloadManager::failed);

  dm.fetch(server.url(), checksum, tmp.path());

  EXPECT_TRUE(waitForSignal(finished_spy));
  EXPECT_TRUE(failed_spy.isEmpty());
  EXPECT_TRUE(QFile::exists(tmp.path() + "/hello.txt"));
}

TEST(DownloadManagerTest, EmptyChecksumSkipsVerification) {
  const QByteArray zip_data = buildZip({{"readme.txt", "content"}});

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);

  dm.fetch(server.url(), {}, tmp.path());

  EXPECT_TRUE(waitForSignal(finished_spy));
  EXPECT_TRUE(QFile::exists(tmp.path() + "/readme.txt"));
}

// A bare "sha256:" prefix with no digest is a malformed registry field. It must
// fail (a garbage checksum should not silently pass), but with a message that
// names the registry, not the generic "Checksum mismatch" that implies a corrupt
// or tampered artifact.
TEST(DownloadManagerTest, BareSha256PrefixFailsAsMalformedNotMismatch) {
  const QByteArray zip_data = buildZip({{"readme.txt", "content"}});

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy failed_spy(&dm, &PJ::DownloadManager::failed);
  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);

  dm.fetch(server.url(), u"sha256:"_s, tmp.path());

  EXPECT_TRUE(waitForSignal(failed_spy));
  EXPECT_TRUE(finished_spy.isEmpty());
  const QString reason = failed_spy.first().at(1).toString();
  EXPECT_TRUE(reason.contains("Malformed", Qt::CaseInsensitive))
      << "a prefix-only checksum must report a malformed registry field, got: " << reason.toStdString();
  EXPECT_FALSE(reason.contains("mismatch", Qt::CaseInsensitive))
      << "must not blame the artifact with a generic mismatch";
}

TEST(DownloadManagerTest, ChecksumMismatchEmitsFailed) {
  const QByteArray zip_data = buildZip({{"file.txt", "content"}});

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy failed_spy(&dm, &PJ::DownloadManager::failed);
  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);

  dm.fetch(server.url(), u"sha256:0000000000000000000000000000000000000000000000000000000000000000"_s, tmp.path());

  EXPECT_TRUE(waitForSignal(failed_spy));
  EXPECT_TRUE(finished_spy.isEmpty());
  EXPECT_TRUE(failed_spy.first().at(1).toString().contains("Checksum"));
}

TEST(DownloadManagerTest, InvalidZipEmitsFailed) {
  LocalHttpServer server;
  server.setBody(QByteArray("this is not a zip"));

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy failed_spy(&dm, &PJ::DownloadManager::failed);
  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);

  dm.fetch(server.url(), {}, tmp.path());

  EXPECT_TRUE(waitForSignal(failed_spy));
  EXPECT_TRUE(finished_spy.isEmpty());
}

TEST(DownloadManagerTest, PathTraversalInZipEmitsFailed) {
  const QByteArray zip_data = buildZip({{"../../evil.txt", "malicious"}});

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy failed_spy(&dm, &PJ::DownloadManager::failed);
  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);

  dm.fetch(server.url(), {}, tmp.path());

  EXPECT_TRUE(waitForSignal(failed_spy));
  EXPECT_TRUE(finished_spy.isEmpty());
}

TEST(DownloadManagerTest, CancelEmitsCancelled) {
  // Server that accepts connections but never sends a response — download hangs indefinitely.
  QTcpServer hanging_server;
  hanging_server.listen(QHostAddress::LocalHost, 0);

  PJ::DownloadManager dm;
  QSignalSpy cancelled_spy(&dm, &PJ::DownloadManager::cancelled);
  QSignalSpy failed_spy(&dm, &PJ::DownloadManager::failed);
  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);

  const int id = dm.fetch(QUrl(u"http://127.0.0.1:%1/"_s.arg(hanging_server.serverPort())), {}, {});

  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  dm.cancel(id);

  EXPECT_TRUE(waitForSignal(cancelled_spy, 2000));
  EXPECT_EQ(cancelled_spy.first().at(0).toInt(), id);
  EXPECT_TRUE(failed_spy.isEmpty());
  EXPECT_TRUE(finished_spy.isEmpty());
}

TEST(DownloadManagerTest, PhaseChangedFiresVerifyingThenExtractingBeforeFinished) {
  // Small artifact with a correct checksum: both phases should fire, in order,
  // before finished is emitted.
  const QByteArray zip_data = buildZip({{"hello.txt", "world"}});
  const QString checksum = sha256Hex(zip_data);

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy phase_spy(&dm, &PJ::DownloadManager::phaseChanged);
  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);

  dm.fetch(server.url(), checksum, tmp.path());

  EXPECT_TRUE(waitForSignal(finished_spy));

  ASSERT_EQ(phase_spy.count(), 2);
  const auto phase0 = phase_spy.at(0).at(1).value<PJ::DownloadManager::WorkPhase>();
  const auto phase1 = phase_spy.at(1).at(1).value<PJ::DownloadManager::WorkPhase>();
  EXPECT_EQ(phase0, PJ::DownloadManager::WorkPhase::Verifying);
  EXPECT_EQ(phase1, PJ::DownloadManager::WorkPhase::Extracting);
}

TEST(DownloadManagerTest, PhaseChangedSkipsVerifyingWhenChecksumEmpty) {
  // No checksum requested → the verify phase must be skipped and only
  // Extracting is announced.
  const QByteArray zip_data = buildZip({{"hello.txt", "world"}});

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy phase_spy(&dm, &PJ::DownloadManager::phaseChanged);
  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);

  dm.fetch(server.url(), {}, tmp.path());

  EXPECT_TRUE(waitForSignal(finished_spy));

  ASSERT_EQ(phase_spy.count(), 1);
  const auto phase = phase_spy.at(0).at(1).value<PJ::DownloadManager::WorkPhase>();
  EXPECT_EQ(phase, PJ::DownloadManager::WorkPhase::Extracting);
}

TEST(DownloadManagerTest, CancelDuringExtractEmitsCancelled) {
  // Many small entries so the extract loop has plenty of cancel checkpoints
  // and the flag is observed before the loop completes. Each iteration in
  // extractFromMemory checks the cancel flag before consuming the next entry.
  QMap<QString, QByteArray> files;
  const QByteArray blob(4 * 1024, 'x');
  for (int i = 0; i < 400; ++i) {
    files.insert(u"file_%1.bin"_s.arg(i, 3, 10, QLatin1Char('0')), blob);
  }
  const QByteArray zip_data = buildZip(files);

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy cancelled_spy(&dm, &PJ::DownloadManager::cancelled);
  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);
  QSignalSpy failed_spy(&dm, &PJ::DownloadManager::failed);

  const int id = dm.fetch(server.url(), {}, tmp.path());

  // Trigger cancel the moment the worker announces the Extracting phase.
  // Receiver is &dm so the lambda runs on the manager's thread — cancel()
  // touches internal maps which the manager only reads from its own thread.
  QObject::connect(
      &dm, &PJ::DownloadManager::phaseChanged, &dm, [&dm, id](int the_id, PJ::DownloadManager::WorkPhase phase) {
        if (the_id == id && phase == PJ::DownloadManager::WorkPhase::Extracting) {
          dm.cancel(the_id);
        }
      });

  EXPECT_TRUE(waitForSignal(cancelled_spy, 10000));
  EXPECT_EQ(cancelled_spy.first().at(0).toInt(), id);
  EXPECT_TRUE(finished_spy.isEmpty());
  EXPECT_TRUE(failed_spy.isEmpty());
}

TEST(DownloadManagerTest, MultipleOperationsHaveUniqueIds) {
  PJ::DownloadManager dm;

  const int id1 = dm.fetch(QUrl("http://255.255.255.255/1"), {}, {});
  const int id2 = dm.fetch(QUrl("http://255.255.255.255/2"), {}, {});
  const int id3 = dm.fetch(QUrl("http://255.255.255.255/3"), {}, {});

  EXPECT_NE(id1, id2);
  EXPECT_NE(id2, id3);
  EXPECT_NE(id1, id3);

  dm.cancel(id1);
  dm.cancel(id2);
  dm.cancel(id3);
}

// ---------------------------------------------------------------------------
// Archive limits
//
// The budgets are injected so a breach costs a few bytes instead of the 256 MiB
// a default-sized fixture would need.
// ---------------------------------------------------------------------------

TEST(DownloadManagerTest, ArchiveWithinLimitsStillInstalls) {
  const QByteArray zip_data = buildZip({{"lib/plugin.so", "0123456789"}});

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  PJ::ArchiveLimits limits;
  limits.maximum_entry_count = 4;
  limits.maximum_entry_expanded_bytes = 64;
  limits.maximum_total_expanded_bytes = 64;
  dm.setArchiveLimits(limits);

  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);
  dm.fetch(server.url(), {}, tmp.path());

  EXPECT_TRUE(waitForSignal(finished_spy));
  EXPECT_TRUE(QFile::exists(tmp.filePath(u"lib/plugin.so"_s)));
}

TEST(DownloadManagerTest, TooManyEntriesIsRefused) {
  const QByteArray zip_data = buildZip({{"a.txt", "a"}, {"b.txt", "b"}, {"c.txt", "c"}});

  PJ::ArchiveLimits limits;
  limits.maximum_entry_count = 2;

  EXPECT_TRUE(installIsRefused(zip_data, limits, u"archive holds more than 2 entries"_s));
}

TEST(DownloadManagerTest, OversizedSingleEntryIsRefused) {
  const QByteArray zip_data = buildZip({{"big.bin", QByteArray(64, 'x')}});

  PJ::ArchiveLimits limits;
  limits.maximum_entry_expanded_bytes = 16;

  EXPECT_TRUE(installIsRefused(zip_data, limits, u"archive entry expands to more than 16 bytes"_s));
}

TEST(DownloadManagerTest, OversizedTotalExpansionIsRefused) {
  // Each entry fits on its own; together they do not.
  const QByteArray zip_data = buildZip({{"a.bin", QByteArray(16, 'a')}, {"b.bin", QByteArray(16, 'b')}});

  PJ::ArchiveLimits limits;
  limits.maximum_entry_expanded_bytes = 32;
  limits.maximum_total_expanded_bytes = 24;

  EXPECT_TRUE(installIsRefused(zip_data, limits, u"archive expands to more than 24 bytes in total"_s));
}

TEST(DownloadManagerTest, OversizedCompressedArchiveIsRefused) {
  const QByteArray zip_data = buildZip({{"a.txt", QByteArray(256, 'a')}});

  PJ::ArchiveLimits limits;
  limits.maximum_compressed_bytes = 32;

  EXPECT_TRUE(installIsRefused(zip_data, limits, u"Archive is larger than the 32-byte limit"_s));
}

TEST(DownloadManagerTest, DeeplyNestedPathIsRefused) {
  const QByteArray zip_data = buildZip({{"a/b/c/d/e.txt", "deep"}});

  PJ::ArchiveLimits limits;
  limits.maximum_path_segments = 3;

  EXPECT_TRUE(installIsRefused(zip_data, limits, u"path is deeper than 3 segments"_s));
}

TEST(DownloadManagerTest, SymlinkEntryIsRefused) {
  const QByteArray zip_data = buildZipWithEntry(u"link"_s, AE_IFLNK, "/etc/passwd");

  EXPECT_TRUE(installIsRefused(zip_data, PJ::ArchiveLimits{}, u"neither a regular file nor a directory"_s));
}

TEST(DownloadManagerTest, WindowsDeviceNameIsRefused) {
  // Harmless on Linux, unusable on Windows: refused on both so a package cannot
  // install on one platform and break on the other.
  const QByteArray zip_data = buildZip({{"aux.so", "payload"}});

  EXPECT_TRUE(installIsRefused(zip_data, PJ::ArchiveLimits{}, u"uses a reserved device name"_s));
}

TEST(DownloadManagerTest, BackslashSeparatorIsRefused) {
#ifdef Q_OS_WIN
  // libarchive treats '\' as a directory separator on Windows and normalizes it to
  // '/' before archive_entry_pathname() returns it, so the entry never reaches
  // admitEntryPath() carrying a backslash and the check has nothing to catch. The
  // refusal is Linux-shaped by nature; on Windows the separator is native.
  GTEST_SKIP() << "libarchive normalizes '\\' to '/' on Windows; the backslash never reaches admitEntryPath()";
#endif
  // A backslash is a separator on Windows and an ordinary filename character on
  // Linux, so the same archive lays out differently on each. Refusing it keeps a
  // package from installing two different shapes.
  const QByteArray zip_data = buildZip({{"quota-probe/sub\\plugin.so", "payload"}});

  EXPECT_TRUE(installIsRefused(zip_data, PJ::ArchiveLimits{}, u"uses a backslash separator"_s));
}

TEST(DownloadManagerTest, EntriesWithoutADeclaredSizeStillInstall) {
  // Regression: treating an undeclared size as a declared zero refused the first
  // data block of a perfectly valid archive.
  const QByteArray zip_data = buildZipWithoutDeclaredSizes(
      {{"quota-probe/manifest.json", QByteArray(516, 'm')}, {"quota-probe/libplugin.so", QByteArray(9000, 'p')}});

  LocalHttpServer server;
  server.setBody(zip_data);

  PJ::DownloadManager dm;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  QSignalSpy finished_spy(&dm, &PJ::DownloadManager::finished);
  dm.fetch(server.url(), {}, tmp.path());

  EXPECT_TRUE(waitForSignal(finished_spy));
  EXPECT_EQ(QFileInfo(tmp.filePath(u"quota-probe/libplugin.so"_s)).size(), 9000);
}

TEST(DownloadManagerTest, UndeclaredSizeStillHonoursTheBudget) {
  // The caps still apply to an undeclared entry; they are just enforced as the
  // bytes arrive instead of up front.
  const QByteArray zip_data = buildZipWithoutDeclaredSizes({{"quota-probe/big.bin", QByteArray(9000, 'x')}});

  PJ::ArchiveLimits limits;
  limits.maximum_entry_expanded_bytes = 4096;

  EXPECT_TRUE(installIsRefused(zip_data, limits, u"does not declare its size and expands past the remaining budget"_s));
}

TEST(DownloadManagerTest, UndeclaredEntriesRespectTheAggregateTotal) {
  // Two entries that each fit the per-entry cap but together outgrow the total.
  // The second can only be stopped if the first was charged for what it wrote,
  // which for an undeclared entry happens once the entry closes.
  const QByteArray zip_data = buildZipWithoutDeclaredSizes(
      {{"quota-probe/a.bin", QByteArray(4000, 'a')}, {"quota-probe/b.bin", QByteArray(4000, 'b')}});

  PJ::ArchiveLimits limits;
  limits.maximum_entry_expanded_bytes = 8192;  // generous: neither entry trips this
  limits.maximum_total_expanded_bytes = 6000;  // but 4000 + 4000 does

  EXPECT_TRUE(installIsRefused(zip_data, limits, u"does not declare its size"_s));
}

TEST(DownloadManagerTest, DeclaredSizeThatUnderstatesIsRefused) {
#ifdef Q_OS_WIN
  // The fixture is a STORED entry whose declared uncompressed size is hand-patched
  // below its real length. Whether the reader hands the excess over as data blocks
  // (so the per-block cap trips with this message) or resolves the malformed stored
  // entry some other way depends on the libarchive build, and the Windows build
  // takes the other path. The cap itself is exercised cross-platform by the
  // undeclared-size tests above; only this bomb-shaped fixture is build-specific.
  GTEST_SKIP() << "malformed-STORED-entry fixture is libarchive-build-specific on Windows; the cap is covered by the "
                  "undeclared-size tests";
#endif
  // The decompression-bomb shape: the header promises little and the payload
  // delivers more, so the budget is charged for a fraction of what would land.
  //
  // The payload has to outgrow the reader's own output buffer. Below that it
  // decompresses in one go and the reader reports the mismatch itself before
  // handing over a single block; past it, blocks arrive and the cap is what
  // stops them — which is the case that matters, since that is when the excess
  // would otherwise reach the disk.
  const QByteArray zip_data = buildZipUnderstatingSize(u"quota-probe/liar.bin"_s, QByteArray(1024 * 1024, 'x'), 64);

  EXPECT_TRUE(installIsRefused(zip_data, PJ::ArchiveLimits{}, u"holds more bytes than it declares"_s));
}

}  // namespace

// ---------------------------------------------------------------------------
// main: required to initialise QCoreApplication before GTest runs
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
