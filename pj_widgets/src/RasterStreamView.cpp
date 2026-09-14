// SPDX-License-Identifier: MPL-2.0
#include "pj_widgets/RasterStreamView.h"

#include <QByteArray>
#include <QKeyEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPainter>
#include <QProcess>
#include <QSharedMemory>
#include <QUuid>
#include <cstdint>
#include <utility>

#include "RasterIpcProtocol.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/RasterFrame.h"
using namespace Qt::StringLiterals;

namespace PJ {

RasterStreamView::RasterStreamView(QWidget* parent) : QWidget(parent), key_translator_([](int key) { return key; }) {
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_OpaquePaintEvent);
}

RasterStreamView::~RasterStreamView() {
  // ~QWidget deletes the socket, server and process children after this body.
  // Their signals must not reach our slots then: this object is no longer a
  // RasterStreamView by that point, so endSession() would be undefined behavior.
  if (socket_ != nullptr) {
    socket_->disconnect(this);
  }
  if (server_ != nullptr) {
    server_->disconnect(this);
  }
  if (process_ != nullptr) {
    process_->disconnect(this);
  }
  teardown();
}

void RasterStreamView::start(const QString& helper_path, const QString& wad_path) {
  if (helper_path.isEmpty() || wad_path.isEmpty()) {
    endSession();
    return;
  }
  const QString unique = QUuid::createUuid().toString(QUuid::Id128);
  const QString shm_key = u"pjr-shm-"_s + unique;
  const QString sock_name = u"pjr-sock-"_s + unique;

  shm_ = new QSharedMemory(shm_key, this);
  const int total = static_cast<int>(pj_raster::shmTotalSize(kWidth, kHeight, pj_raster::kBytesPerPixel));
  if (!shm_->create(total)) {
    endSession();
    return;
  }

  server_ = new QLocalServer(this);
  connect(server_, &QLocalServer::newConnection, this, &RasterStreamView::onHelperConnected);
  QLocalServer::removeServer(sock_name);
  if (!server_->listen(sock_name)) {
    endSession();
    return;
  }

  process_ = new QProcess(this);
  connect(process_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) { endSession(); });
  connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { endSession(); });
  process_->start(helper_path, {u"--shm"_s, shm_key, u"--sock"_s, sock_name, u"--wad"_s, wad_path});
}

void RasterStreamView::setKeyTranslator(std::function<int(int)> translator) {
  key_translator_ = translator ? std::move(translator) : std::function<int(int)>([](int key) { return key; });
}

void RasterStreamView::onHelperConnected() {
  if (socket_ != nullptr) {
    return;
  }
  socket_ = server_->nextPendingConnection();
  socket_->setParent(this);
  connect(socket_, &QLocalSocket::readyRead, this, &RasterStreamView::onSocketReadyRead);
  connect(socket_, &QLocalSocket::disconnected, this, [this]() { endSession(); });
  setFocus();
}

void RasterStreamView::onSocketReadyRead() {
  while (socket_->bytesAvailable() >= pj_raster::kMessageBytes) {
    const QByteArray bytes = socket_->read(pj_raster::kMessageBytes);
    const pj_raster::Message msg = pj_raster::decodeMessage(reinterpret_cast<const uint8_t*>(bytes.constData()));
    if (msg.type == pj_raster::MsgType::kFrameReady && shm_ != nullptr) {
      if (!shm_->lock()) {
        continue;
      }
      const QImage view = imageForActiveFrame(static_cast<const unsigned char*>(shm_->constData()), shm_->size());
      frame_ = view.copy();
      shm_->unlock();
      if (!frame_.isNull()) {
        scaled_ = QImage();  // invalidate the cached scale; recomputed on next paint
        update();
      }
    } else if (msg.type == pj_raster::MsgType::kBye) {
      endSession();
    }
  }
}

void RasterStreamView::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  const auto fw_theme = theme::appTheme();
  painter.fillRect(rect(), theme::surface(theme::Surface::DataBackdrop, fw_theme));
  if (frame_.isNull()) {
    return;
  }
  // Re-scaling is the expensive part, so cache it and recompute only when the
  // frame or the widget size actually changes (not on every unrelated repaint).
  if (scaled_.isNull() || scaled_for_ != size()) {
    scaled_ = frame_.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled_for_ = size();
  }
  const int x = (width() - scaled_.width()) / 2;
  const int y = (height() - scaled_.height()) / 2;
  painter.drawImage(x, y, scaled_);
}

void RasterStreamView::sendMessage(const pj_raster::Message& msg) {
  if (socket_ == nullptr) {
    return;
  }
  uint8_t bytes[pj_raster::kMessageBytes];
  pj_raster::encodeMessage(msg, bytes);
  socket_->write(reinterpret_cast<char*>(bytes), pj_raster::kMessageBytes);
  socket_->flush();
}

void RasterStreamView::sendKey(int qt_key, bool down) {
  const int code = key_translator_(qt_key);
  if (code == 0) {
    return;
  }
  sendMessage({down ? pj_raster::MsgType::kKeyDown : pj_raster::MsgType::kKeyUp, static_cast<uint32_t>(code)});
}

void RasterStreamView::keyPressEvent(QKeyEvent* event) {
  if (!event->isAutoRepeat()) {
    sendKey(event->key(), true);
  }
  event->accept();
}

void RasterStreamView::keyReleaseEvent(QKeyEvent* event) {
  if (!event->isAutoRepeat()) {
    sendKey(event->key(), false);
  }
  event->accept();
}

void RasterStreamView::endSession() {
  if (ended_) {
    return;
  }
  ended_ = true;
  emit sessionEnded();
}

void RasterStreamView::teardown() {
  sendMessage({pj_raster::MsgType::kBye, 0});
  if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
    process_->disconnect(this);
    process_->terminate();
    if (!process_->waitForFinished(800)) {
      process_->kill();
      process_->waitForFinished(800);
    }
  }
}

}  // namespace PJ
