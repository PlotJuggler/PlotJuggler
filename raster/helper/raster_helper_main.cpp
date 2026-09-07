// SPDX-License-Identifier: GPL-2.0-only
// External render helper: drives doomgeneric and bridges it to PlotJuggler over
// shared memory (frames) + a local socket (frame-ready + input). Links
// doomgeneric (GPLv2) and Qt6::Core/Network (LGPL). PlotJuggler links none of
// this.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QQueue>
#include <QSharedMemory>
#include <QTimer>
#include <cstring>

extern "C" {
#include "doomgeneric.h"
}

#include "RasterIpcProtocol.h"

namespace {
QSharedMemory* g_shm = nullptr;
QLocalSocket* g_sock = nullptr;
QElapsedTimer g_clock;
// The engine renders a fixed-size DG_ScreenBuffer; the shared-memory copies are
// sized to match it, so these are constants rather than configurable.
uint32_t g_w = DOOMGENERIC_RESX;
uint32_t g_h = DOOMGENERIC_RESY;
uint32_t g_seq = 0;

struct KeyEvent {
  int pressed;
  unsigned char key;
};
QQueue<KeyEvent> g_keys;
}  // namespace

extern "C" {

void DG_Init() {}

void DG_DrawFrame() {
  if (g_shm == nullptr || !g_shm->lock()) {
    return;
  }
  auto* base = static_cast<unsigned char*>(g_shm->data());
  auto* hdr = reinterpret_cast<pj_raster::ShmHeader*>(base);
  hdr->magic = pj_raster::kShmMagic;
  hdr->version = pj_raster::kShmVersion;
  hdr->width = g_w;
  hdr->height = g_h;
  hdr->bytes_per_pixel = pj_raster::kBytesPerPixel;
  hdr->buffer_count = pj_raster::kBufferCount;
  // Write to the inactive buffer, then publish it.
  const uint32_t active_index =
      hdr->active_index < pj_raster::kBufferCount ? hdr->active_index : pj_raster::kBufferCount - 1;
  const uint32_t write_index = (active_index + 1) % pj_raster::kBufferCount;
  unsigned char* dst = base + pj_raster::bufferOffset(write_index, g_w, g_h, pj_raster::kBytesPerPixel);
  std::memcpy(dst, DG_ScreenBuffer, pj_raster::bufferBytes(g_w, g_h, pj_raster::kBytesPerPixel));
  hdr->active_index = write_index;
  hdr->frame_seq = ++g_seq;
  g_shm->unlock();

  if (g_sock != nullptr) {
    uint8_t msg[pj_raster::kMessageBytes];
    pj_raster::encodeMessage({pj_raster::MsgType::kFrameReady, g_seq}, msg);
    g_sock->write(reinterpret_cast<char*>(msg), pj_raster::kMessageBytes);
    g_sock->flush();
  }
}

void DG_SleepMs(uint32_t ms) {
  // Pump the socket so inbound key messages arrive between ticks.
  QCoreApplication::processEvents(QEventLoop::AllEvents, static_cast<int>(ms));
}

uint32_t DG_GetTicksMs() {
  return static_cast<uint32_t>(g_clock.elapsed());
}

int DG_GetKey(int* pressed, unsigned char* key) {
  QCoreApplication::processEvents(QEventLoop::AllEvents, 0);
  if (g_keys.isEmpty()) {
    return 0;
  }
  const KeyEvent e = g_keys.dequeue();
  *pressed = e.pressed;
  *key = e.key;
  return 1;
}

void DG_SetWindowTitle(const char* /*title*/) {}

}  // extern "C"

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCommandLineParser p;
  QCommandLineOption shm("shm", "shm key", "key");
  QCommandLineOption sock("sock", "socket name", "name");
  QCommandLineOption wad("wad", "iwad path", "path");
  p.addOptions({shm, sock, wad});
  p.process(app);

  g_shm = new QSharedMemory(p.value(shm));
  if (!g_shm->attach()) {
    return 2;
  }
  g_sock = new QLocalSocket();
  g_sock->connectToServer(p.value(sock));
  if (!g_sock->waitForConnected(2000)) {
    return 3;
  }
  QObject::connect(g_sock, &QLocalSocket::readyRead, [&] {
    while (g_sock->bytesAvailable() >= pj_raster::kMessageBytes) {
      const QByteArray b = g_sock->read(pj_raster::kMessageBytes);
      const pj_raster::Message m = pj_raster::decodeMessage(reinterpret_cast<const uint8_t*>(b.constData()));
      if (m.type == pj_raster::MsgType::kKeyDown) {
        g_keys.enqueue({1, static_cast<unsigned char>(m.arg)});
      } else if (m.type == pj_raster::MsgType::kKeyUp) {
        g_keys.enqueue({0, static_cast<unsigned char>(m.arg)});
      } else if (m.type == pj_raster::MsgType::kBye) {
        QCoreApplication::quit();
      }
    }
  });
  QObject::connect(g_sock, &QLocalSocket::disconnected, &app, &QCoreApplication::quit);

  g_clock.start();

  // doomgeneric_Create wants argv-style args incl. -iwad <path>.
  const QByteArray prog = QByteArrayLiteral("pj-raster-helper");
  const QByteArray iwad = QByteArrayLiteral("-iwad");
  const QByteArray wadpath = p.value(wad).toUtf8();
  char* dg_argv[] = {
      const_cast<char*>(prog.constData()), const_cast<char*>(iwad.constData()), const_cast<char*>(wadpath.constData())};
  doomgeneric_Create(3, dg_argv);

  // Drive the engine; doomgeneric_Tick renders one frame + calls our callbacks.
  QTimer tick;
  QObject::connect(&tick, &QTimer::timeout, [&] { doomgeneric_Tick(); });
  tick.start(0);
  return app.exec();
}
