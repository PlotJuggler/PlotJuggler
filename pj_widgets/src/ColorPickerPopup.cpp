// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ColorPickerPopup.h"

#include <QLineEdit>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QVBoxLayout>
using namespace Qt::StringLiterals;

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

namespace {

constexpr int kHueSliderHeight = 18;
constexpr int kSquareSide = 220;
constexpr int kCursorRadius = 6;
constexpr auto kPopupMargin = theme::Space::Comfortable;

}  // namespace

HueSlider::HueSlider(QWidget* parent) : QWidget(parent) {
  setFixedHeight(kHueSliderHeight);
  setCursor(Qt::PointingHandCursor);
}

QSize HueSlider::sizeHint() const {
  return {kSquareSide, kHueSliderHeight};
}

void HueSlider::setHue(int hue) {
  hue = qBound(0, hue, 359);
  if (hue_ == hue) {
    return;
  }
  hue_ = hue;
  update();
}

void HueSlider::paintEvent(QPaintEvent* /*event*/) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  const int w = width();
  const int h = height();

  QLinearGradient grad(0, 0, w, 0);
  for (int i = 0; i <= 6; ++i) {
    grad.setColorAt(i / 6.0, QColor::fromHsv(i * 60 % 360, 255, 255));
  }
  p.fillRect(rect(), grad);

  // White core + black flanks so the handle stays visible against any rainbow
  // segment (red/cyan would otherwise camouflage a single-color line).
  const int x = qBound(0, static_cast<int>(qreal(hue_) / 359.0 * (w - 1)), w - 1);
  p.setPen(QPen(theme::contrastInk(theme::Contrast::Dark), 1));
  p.drawLine(x - 2, 0, x - 2, h - 1);
  p.drawLine(x + 2, 0, x + 2, h - 1);
  p.setPen(QPen(theme::contrastInk(theme::Contrast::Light), 2));
  p.drawLine(x, 0, x, h - 1);
}

void HueSlider::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    updateFromMouseX(static_cast<int>(event->position().x()));
  }
}

void HueSlider::mouseMoveEvent(QMouseEvent* event) {
  if ((event->buttons() & Qt::LeftButton) != 0) {
    updateFromMouseX(static_cast<int>(event->position().x()));
  }
}

void HueSlider::updateFromMouseX(int x) {
  const int w = width();
  if (w <= 1) {
    return;
  }
  const int hue = static_cast<int>(qreal(qBound(0, x, w - 1)) / qreal(w - 1) * 359.0);
  if (hue == hue_) {
    return;
  }
  hue_ = hue;
  update();
  emit hueChanged(hue_);
}

SVSquare::SVSquare(QWidget* parent) : QWidget(parent) {
  setMinimumSize(kSquareSide, kSquareSide);
  setCursor(Qt::CrossCursor);
}

QSize SVSquare::sizeHint() const {
  return {kSquareSide, kSquareSide};
}

void SVSquare::setHue(int hue) {
  hue = qBound(0, hue, 359);
  if (hue_ == hue) {
    return;
  }
  hue_ = hue;
  update();
}

void SVSquare::setSV(qreal s, qreal v) {
  s_ = qBound(0.0, s, 1.0);
  v_ = qBound(0.0, v, 1.0);
  update();
}

QColor SVSquare::color() const {
  return QColor::fromHsvF(qreal(hue_) / 360.0, s_, v_);
}

void SVSquare::paintEvent(QPaintEvent* /*event*/) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  const int w = width();
  const int h = height();

  // Standard SV-square composition: pure hue background, a white-to-transparent saturation gradient (left to right) and
  // a transparent-to-black value gradient (top to bottom).
  p.fillRect(rect(), QColor::fromHsv(hue_, 255, 255));

  QLinearGradient sat(0, 0, w, 0);
  sat.setColorAt(0.0, Qt::white);
  sat.setColorAt(1.0, QColor(255, 255, 255, 0));
  p.fillRect(rect(), sat);

  QLinearGradient val(0, 0, 0, h);
  val.setColorAt(0.0, QColor(0, 0, 0, 0));
  val.setColorAt(1.0, Qt::black);
  p.fillRect(rect(), val);

  // Hollow white circle with black outline for contrast on any background.
  const qreal cx = s_ * (w - 1);
  const qreal cy = (1.0 - v_) * (h - 1);
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(theme::contrastInk(theme::Contrast::Dark), 1));
  p.drawEllipse(QPointF(cx, cy), kCursorRadius + 1, kCursorRadius + 1);
  p.setPen(QPen(theme::contrastInk(theme::Contrast::Light), 2));
  p.drawEllipse(QPointF(cx, cy), kCursorRadius, kCursorRadius);
}

void SVSquare::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    updateFromMouseXY(static_cast<int>(event->position().x()), static_cast<int>(event->position().y()));
  }
}

void SVSquare::mouseMoveEvent(QMouseEvent* event) {
  if ((event->buttons() & Qt::LeftButton) != 0) {
    updateFromMouseXY(static_cast<int>(event->position().x()), static_cast<int>(event->position().y()));
  }
}

void SVSquare::updateFromMouseXY(int x, int y) {
  const int w = width();
  const int h = height();
  if (w <= 1 || h <= 1) {
    return;
  }
  const qreal s = qBound(0.0, qreal(x) / qreal(w - 1), 1.0);
  const qreal v = qBound(0.0, 1.0 - qreal(y) / qreal(h - 1), 1.0);
  if (qFuzzyCompare(s, s_) && qFuzzyCompare(v, v_)) {
    return;
  }
  s_ = s;
  v_ = v;
  update();
  emit colorChanged(color());
}

ColorPickerPopup::ColorPickerPopup(QWidget* parent) : QDialog(parent) {
  setWindowFlags(Qt::Popup);

  hue_slider_ = new HueSlider(this);
  sv_square_ = new SVSquare(this);
  hex_edit_ = new QLineEdit(this);
  hex_edit_->setPlaceholderText(u"#rrggbb"_s);
  hex_edit_->setMaxLength(7);
  hex_edit_->setValidator(new QRegularExpressionValidator(QRegularExpression(u"#?[0-9a-fA-F]{0,6}"_s), hex_edit_));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(
      theme::space(kPopupMargin), theme::space(kPopupMargin), theme::space(kPopupMargin), theme::space(kPopupMargin));
  layout->setSpacing(theme::space(kPopupMargin));
  layout->addWidget(hue_slider_);
  layout->addWidget(sv_square_, 1);
  layout->addWidget(hex_edit_);

  connect(hue_slider_, &HueSlider::hueChanged, this, &ColorPickerPopup::onHueChanged);
  connect(sv_square_, &SVSquare::colorChanged, this, &ColorPickerPopup::onSquareChanged);
  connect(hex_edit_, &QLineEdit::editingFinished, this, &ColorPickerPopup::onHexCommitted);
}

QColor ColorPickerPopup::color() const {
  return QColor::fromHsvF(qreal(hue_) / 360.0, s_, v_);
}

void ColorPickerPopup::setColor(const QColor& color) {
  if (!color.isValid()) {
    return;
  }
  const qreal h = color.hsvHueF();
  // Achromatic colors (black, white, grays) report hue == -1: the previous hue is kept, so the picker does not snap to
  // red on grayscale input.
  if (h >= 0) {
    hue_ = qBound(0, static_cast<int>(h * 360.0), 359);
  }
  s_ = color.hsvSaturationF();
  v_ = color.valueF();

  QSignalBlocker block_hue(hue_slider_);
  QSignalBlocker block_sv(sv_square_);
  QSignalBlocker block_hex(hex_edit_);
  hue_slider_->setHue(hue_);
  sv_square_->setHue(hue_);
  sv_square_->setSV(s_, v_);
  pushToHexField(color);
}

void ColorPickerPopup::onHueChanged(int hue) {
  hue_ = hue;
  QSignalBlocker block_sv(sv_square_);
  sv_square_->setHue(hue);
  recomputeAndEmit();
}

void ColorPickerPopup::onSquareChanged(QColor c) {
  s_ = c.hsvSaturationF();
  v_ = c.valueF();
  recomputeAndEmit();
}

void ColorPickerPopup::onHexCommitted() {
  QString text = hex_edit_->text().trimmed();
  if (!text.startsWith('#')) {
    text.prepend('#');
  }
  if (text.size() != 7) {
    pushToHexField(color());
    return;
  }
  const QColor c(text);
  if (!c.isValid()) {
    pushToHexField(color());
    return;
  }
  setColor(c);
  emit colorChanged(c);
}

void ColorPickerPopup::recomputeAndEmit() {
  const QColor c = color();
  pushToHexField(c);
  emit colorChanged(c);
}

void ColorPickerPopup::pushToHexField(const QColor& c) {
  QSignalBlocker block_hex(hex_edit_);
  hex_edit_->setText(c.name());
}

}  // namespace PJ
