// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QWidget>
#include <functional>

#include "RasterIpcProtocol.h"

class QLocalServer;
class QLocalSocket;
class QProcess;
class QSharedMemory;

namespace PJ {

// Hosts an external renderer process and shows its framebuffer in a panel. Owns a QSharedMemory frame segment and a
// QLocalServer the helper connects back to, launches the helper, paints each published frame (letterbox-scaled) and
// forwards key events to it. Emits sessionEnded() when the helper exits or disconnects. Pure Qt and cross-platform (no
// window embedding, no X11).
//
// The view is engine-agnostic: by default it forwards raw Qt key codes; a consumer driving a process with another key
// encoding installs a translator with setKeyTranslator().
class RasterStreamView : public QWidget {
  Q_OBJECT
 public:
  explicit RasterStreamView(QWidget* parent = nullptr);
  ~RasterStreamView() override;

  void start(const QString& helper_path, const QString& wad_path);

  // Installs a Qt-key to wire-code mapper (return 0 to drop a key); without it raw Qt key codes are forwarded.
  void setKeyTranslator(std::function<int(int)> translator);

 signals:
  void sessionEnded();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;

 private:
  void onHelperConnected();
  void onSocketReadyRead();
  void sendKey(int qt_key, bool down);
  void sendMessage(const pj_raster::Message& msg);
  void endSession();
  void teardown();

  QSharedMemory* shm_ = nullptr;
  QLocalServer* server_ = nullptr;
  QLocalSocket* socket_ = nullptr;
  QProcess* process_ = nullptr;
  QImage frame_;
  QImage scaled_;     // frame_ letterbox-scaled to the current size(), cached
  QSize scaled_for_;  // the size() scaled_ was computed for
  std::function<int(int)> key_translator_;
  bool ended_ = false;
  // Frame geometry: must match the render helper's fixed engine resolution; the shared segment is sized from it.
  static constexpr int kWidth = 640;
  static constexpr int kHeight = 400;
};

}  // namespace PJ
