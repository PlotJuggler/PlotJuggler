// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#include <QByteArray>
#include <QDir>
#include <QFileInfo>

#include "Durability.hpp"

namespace PJ {
namespace {

#if defined(_WIN32)
QString windowsFailure(const QString& action, DWORD error) {
  return QStringLiteral("%1 (Win32 error %2)").arg(action).arg(error);
}
#else
QString posixFailure(const QString& action, int error) {
  return QStringLiteral("%1: %2").arg(action, QString::fromLocal8Bit(std::strerror(error)));
}
#endif

}  // namespace

Expected<void, QString> syncFileToDisk(QFileDevice& file) {
  if (!file.flush()) {
    return unexpected(QStringLiteral("cannot flush file: %1").arg(file.errorString()));
  }
  const int descriptor = file.handle();
  if (descriptor < 0) {
    return unexpected(QStringLiteral("cannot obtain native file descriptor"));
  }

#if defined(_WIN32)
  const intptr_t native_handle = _get_osfhandle(descriptor);
  if (native_handle == -1) {
    return unexpected(QStringLiteral("cannot obtain native file handle"));
  }
  if (FlushFileBuffers(reinterpret_cast<HANDLE>(native_handle)) == 0) {
    return unexpected(windowsFailure(QStringLiteral("cannot force file contents to storage"), GetLastError()));
  }
#else
  int result = 0;
  do {
    result = ::fsync(descriptor);
  } while (result != 0 && errno == EINTR);
  if (result != 0) {
    return unexpected(posixFailure(QStringLiteral("cannot force file contents to storage"), errno));
  }
#endif
  return {};
}

Expected<void, QString> syncDirectoryToDisk(const QString& directory) {
#if defined(_WIN32)
  const QString native_directory = QDir::toNativeSeparators(QFileInfo(directory).absoluteFilePath());
  const HANDLE handle = CreateFileW(
      reinterpret_cast<LPCWSTR>(native_directory.utf16()), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return unexpected(windowsFailure(QStringLiteral("cannot open directory for durable sync"), GetLastError()));
  }
  if (FlushFileBuffers(handle) == 0) {
    const DWORD error = GetLastError();
    CloseHandle(handle);
    return unexpected(windowsFailure(QStringLiteral("cannot force directory changes to storage"), error));
  }
  if (CloseHandle(handle) == 0) {
    return unexpected(windowsFailure(QStringLiteral("cannot close durable directory handle"), GetLastError()));
  }
#else
  const QByteArray native_directory = QFile::encodeName(QFileInfo(directory).absoluteFilePath());
  int flags = O_RDONLY;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
  int descriptor = -1;
  do {
    descriptor = ::open(native_directory.constData(), flags);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    return unexpected(posixFailure(QStringLiteral("cannot open directory for durable sync"), errno));
  }

  int sync_result = 0;
  do {
    sync_result = ::fsync(descriptor);
  } while (sync_result != 0 && errno == EINTR);
  const int sync_error = errno;
  if (::close(descriptor) != 0 && sync_result == 0) {
    return unexpected(posixFailure(QStringLiteral("cannot close durable directory descriptor"), errno));
  }
  if (sync_result != 0) {
    return unexpected(posixFailure(QStringLiteral("cannot force directory changes to storage"), sync_error));
  }
#endif
  return {};
}

}  // namespace PJ
