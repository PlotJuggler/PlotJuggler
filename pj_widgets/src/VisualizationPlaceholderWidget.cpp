// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/VisualizationPlaceholderWidget.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMovie>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QSize>
#include <QTimer>
#include <QToolButton>
#include <QtEndian>
#include <algorithm>
#include <vector>

#include "pj_widgets/AppSettings.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/OwnedMenu.h"
#include "pj_widgets/SvgUtil.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

/// Side of a family button at its natural size and at the smallest the grid may shrink to.
constexpr int kButtonSide = 66;
constexpr int kMinButtonSide = 44;
/// Icon side inside a kButtonSide button; smaller buttons scale it by the same ratio.
constexpr int kIconSide = 48;
/// The family buttons fill a grid this many columns wide, row by row.
constexpr int kColumns = 3;

/// Length of `count` buttons of `side` in a line, with the layout's spacing between them.
int lineLength(int count, int side, int spacing) {
  return count * side + std::max(count - 1, 0) * spacing;
}

QToolButton* makeIconButton(const QString& icon_path, const QString& tooltip, QWidget* parent) {
  auto* button = new QToolButton(parent);
  // Icon is theme-tinted by the caller via LoadSvg+setIcon after the
  // button is created; the placeholder also re-tints on theme changes.
  Q_UNUSED(icon_path);
  button->setIconSize(QSize(kIconSide, kIconSide));
  button->setFixedSize(kButtonSide, kButtonSide);
  button->setAutoRaise(true);
  button->setFocusPolicy(Qt::NoFocus);
  button->setToolTip(tooltip);
  // The pointing-hand cursor signals that every family icon is clickable.
  button->setCursor(Qt::PointingHandCursor);
  return button;
}

bool acceptsCatalogItems(const QMimeData* mime_data) {
  return mime_data != nullptr && mime_data->hasFormat(CurveTreeView::catalogItemsMimeType());
}

bool acceptCatalogDrag(QDropEvent* event) {
  if (event != nullptr && acceptsCatalogItems(event->mimeData())) {
    event->acceptProposedAction();
    return true;
  }
  return false;
}

bool dropCatalogItems(QDropEvent* event, VisualizationPlaceholderWidget* target) {
  const QMimeData* mime_data = event != nullptr ? event->mimeData() : nullptr;
  const QStringList keys = CurveTreeView::decodeCatalogKeys(mime_data);
  if (keys.empty()) {
    return false;
  }
  // A right-drag of exactly two curves carries the new_XY_axis mime, the "create XY plot" gesture: routed so the host
  // builds an XY plot instead of two ordinary curves; everything else is a normal catalog drop.
  if (keys.size() == 2 && mime_data != nullptr && mime_data->hasFormat(CurveTreeView::newXyAxisMimeType())) {
    emit target->catalogItemsXyRequested(keys);
  } else {
    emit target->catalogItemsDropped(keys);
  }
  event->acceptProposedAction();
  return true;
}

// Release builds compile in the key from the PJ_EASTER_EGG_KEY GitHub secret (pj_widgets/CMakeLists.txt); every
// other build has none, and the easter egg is off.
#ifndef PJ_EASTER_EGG_KEY
#define PJ_EASTER_EGG_KEY ""
#endif
constexpr char kEasterEggKey[] = PJ_EASTER_EGG_KEY;

// The easter egg pool: every encrypted 128x128 looping GIF in this resource folder (memes.qrc, so desktop only),
// listed at run time; scripts/pack_easter_eggs.py makes them.
constexpr auto kEasterEggDir = ":/resources/gif";
constexpr int kEasterEggSide = 128;
// Persisted (ISO date string): the first day the easter egg may show (again). Missing means first run, which sets it.
constexpr auto kEasterEggNextKey = "EasterEgg/next_date";
// Persisted: the user's character (easterEggCharacter), fixed by their first easter egg; later ones are its variants.
constexpr auto kEasterEggCharacterKey = "EasterEgg/character";
constexpr int kMinCooldownDays = 2;
constexpr int kMaxCooldownDays = 5;
constexpr int kMinFirstRunGraceDays = 4;
constexpr int kMaxFirstRunGraceDays = 8;
// A fresh random idle stretch after each layout activity.
constexpr int kMinIdleSeconds = 120;
constexpr int kMaxIdleSeconds = 300;

// After a random stretch with no layout activity (noteLayoutActivity), one visible placeholder chosen at random shows
// the easter egg; at most once per process, then not again until the persisted date. Parented to qApp; deletes itself
// once shown.
class IdleEasterEgg : public QObject {
 public:
  static void arm() {
    static bool armed = false;
    if (armed || qApp == nullptr) {
      return;
    }
    armed = true;
    // Offscreen is the unit tests and the QA agent: a GIF appearing there is noise.
    if (sizeof(kEasterEggKey) <= 1 || easterEggPool().isEmpty() || QGuiApplication::platformName() == u"offscreen"_s) {
      return;
    }
    const QDate today = QDate::currentDate();
    const QDate next =
        QDate::fromString(appSettings().value(QString::fromLatin1(kEasterEggNextKey)).toString(), Qt::ISODate);
    if (!next.isValid()) {
      // First run: the first easter egg waits a random first-run grace after install.
      appSettings().setValue(
          QString::fromLatin1(kEasterEggNextKey),
          today.addDays(QRandomGenerator::global()->bounded(kMinFirstRunGraceDays, kMaxFirstRunGraceDays + 1))
              .toString(Qt::ISODate));
      return;
    }
    if (next > today) {
      return;
    }
    instance_ = new IdleEasterEgg(qApp);
  }

  static void restart() {
    if (instance_ != nullptr) {
      instance_->idle_.start(1000 * QRandomGenerator::global()->bounded(kMinIdleSeconds, kMaxIdleSeconds + 1));
    }
  }

 private:
  explicit IdleEasterEgg(QObject* parent) : QObject(parent) {
    idle_.setSingleShot(true);
    connect(&idle_, &QTimer::timeout, this, &IdleEasterEgg::onIdle);
  }

  // No placeholder with room: wait for the next layout activity. A GIF that does not decrypt (a wrong key) ends it.
  void onIdle() {
    const QString character = appSettings().value(QString::fromLatin1(kEasterEggCharacterKey)).toString();
    const QString path = pickEasterEggGif(easterEggPool(), character);
    const QByteArray gif = loadEasterEgg(path);
    if (gif.isEmpty()) {
      deleteLater();
      return;
    }
    std::vector<VisualizationPlaceholderWidget*> candidates;
    for (QWidget* widget : QApplication::allWidgets()) {
      if (auto* placeholder = qobject_cast<VisualizationPlaceholderWidget*>(widget)) {
        candidates.push_back(placeholder);
      }
    }
    while (!candidates.empty()) {
      const auto pick = QRandomGenerator::global()->bounded(static_cast<qsizetype>(candidates.size()));
      if (candidates[pick]->showEasterEgg(gif)) {
        appSettings().setValue(QString::fromLatin1(kEasterEggCharacterKey), easterEggCharacter(path));
        const int days = QRandomGenerator::global()->bounded(kMinCooldownDays, kMaxCooldownDays + 1);
        appSettings().setValue(
            QString::fromLatin1(kEasterEggNextKey), QDate::currentDate().addDays(days).toString(Qt::ISODate));
        deleteLater();
        return;
      }
      candidates.erase(candidates.begin() + pick);
    }
  }

  static inline QPointer<IdleEasterEgg> instance_;
  QTimer idle_;
};

}  // namespace

VisualizationPlaceholderWidget::VisualizationPlaceholderWidget(QWidget* parent) : QWidget(parent) {
  setAcceptDrops(true);
  setObjectName(u"VisualizationPlaceholderWidget"_s);

  action_split_horizontal_ = new QAction(tr("Split &Horizontally"), this);
  connect(action_split_horizontal_, &QAction::triggered, this, [this]() { emit splitHorizontalRequested(); });

  action_split_vertical_ = new QAction(tr("Split &Vertically"), this);
  connect(action_split_vertical_, &QAction::triggered, this, [this]() { emit splitVerticalRequested(); });

  action_paste_ = new QAction(tr("&Paste"), this);
  action_paste_->setEnabled(false);
  connect(action_paste_, &QAction::triggered, this, [this]() { emit pasteRequested(); });

  auto* layout = new QGridLayout(this);
  layout->setContentsMargins(
      theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
      theme::space(theme::Space::None));
  layout->setSpacing(theme::space(theme::Space::Comfortable));
  // The buttons' current fixed size must not become the widget's minimum: minimumSizeHint reports the smallest grid,
  // and fitButtons shrinks the buttons to it.
  layout->setSizeConstraint(QLayout::SetNoConstraint);
  layout->setAlignment(Qt::AlignCenter);
  const struct {
    const char* path;
    const char* tooltip;
    const char* object_name;
    VisualizationKind kind;
  } specs[] = {
      {":/resources/svg/line_axis.svg", QT_TR_NOOP("Plot"), "buttonVizPlot", VisualizationKind::kPlot},
      {":/resources/svg/image.svg", QT_TR_NOOP("2D"), "buttonVizScene2D", VisualizationKind::kScene2D},
      {":/resources/svg/cube.svg", QT_TR_NOOP("3D"), "buttonVizScene3D", VisualizationKind::kScene3D},
      {":/resources/svg/view_list.svg", QT_TR_NOOP("Log"), "buttonVizLog", VisualizationKind::kLog},
      {":/resources/svg/state_transitions.svg", QT_TR_NOOP("State Transitions"), "buttonVizStateTransitions",
       VisualizationKind::kStateTransitions},
      // Always shown; the shell enables it (setCustomWidgetsAvailable) according to the installed widget catalog.
      {":/resources/svg/custom_widget.svg", QT_TR_NOOP("Custom Widget"), "buttonVizCustomWidget",
       VisualizationKind::kCustomWidget},
  };
  icon_buttons_.reserve(std::size(specs));
  for (const auto& spec : specs) {
    auto* button = makeIconButton(QString::fromLatin1(spec.path), tr(spec.tooltip), this);
    button->setObjectName(QString::fromLatin1(spec.object_name));
    button->setAcceptDrops(true);
    button->installEventFilter(this);
    const VisualizationKind kind = spec.kind;
    connect(button, &QToolButton::clicked, this, [this, kind]() { emit visualizationRequested(kind); });
    const auto index = static_cast<int>(icon_buttons_.size());
    layout->addWidget(button, index / kColumns, index % kColumns);
    icon_buttons_.push_back({button, QString::fromLatin1(spec.path)});
  }
  // Initial paint at whatever theme is currently active. Subsequent
  // changes flow in through onStylesheetChanged.
  onStylesheetChanged(currentTheme());
  IdleEasterEgg::arm();
  noteLayoutActivity();
}

VisualizationPlaceholderWidget::~VisualizationPlaceholderWidget() {
  noteLayoutActivity();
}

void VisualizationPlaceholderWidget::setPasteActionEnabled(bool enabled) {
  if (action_paste_ != nullptr) {
    action_paste_->setEnabled(enabled);
  }
}

void VisualizationPlaceholderWidget::setCustomWidgetsAvailable(bool available) {
  QToolButton* button = customWidgetButton();
  if (button == nullptr) {
    return;
  }
  button->setEnabled(available);
  // A disabled button must not swallow drops: they then reach this widget.
  button->setAcceptDrops(available);
  button->setCursor(available ? Qt::PointingHandCursor : Qt::ArrowCursor);
  if (available) {
    button->setToolTip(tr("Custom Widget"));
  } else {
#ifdef PJ_TARGET_WASM
    button->setToolTip(tr("No custom widgets installed."));
#else
    button->setToolTip(tr("No custom widgets installed. Get them from the Marketplace."));
#endif
  }
}

QToolButton* VisualizationPlaceholderWidget::customWidgetButton() const {
  return findChild<QToolButton*>(QStringLiteral("buttonVizCustomWidget"));
}

bool VisualizationPlaceholderWidget::showEasterEgg(const QByteArray& gif) {
  if (easter_egg_ != nullptr) {
    return easter_egg_->isVisible();
  }
  if (gif.isEmpty() || !isVisible() || !easterEggFits()) {
    return false;
  }
  easter_egg_ = new QLabel(this);
  easter_egg_->setObjectName(u"placeholderEasterEgg"_s);
  // Clicks, drops and the context menu still reach the placeholder through it.
  easter_egg_->setAttribute(Qt::WA_TransparentForMouseEvents);
  auto* buffer = new QBuffer();
  buffer->setData(gif);
  buffer->open(QIODevice::ReadOnly);
  auto* movie = new QMovie(buffer, {}, easter_egg_);
  // Owned by the movie, so it outlives the movie's reader.
  buffer->setParent(movie);
  easter_egg_->setMovie(movie);
  movie->start();
  placeEasterEgg();
  return true;
}

QRect VisualizationPlaceholderWidget::easterEggRect() const {
  const int inset = theme::space(theme::Space::Comfortable);
  return {inset, height() - inset - kEasterEggSide, kEasterEggSide, kEasterEggSide};
}

bool VisualizationPlaceholderWidget::easterEggFits() const {
  // The layout centres the grid; computed rather than read from the buttons, whose geometry lags a resize.
  QRect grid({}, gridSize(icon_buttons_.front().button->width()));
  grid.moveCenter(rect().center());
  const QRect egg = easterEggRect();
  return egg.top() >= 0 && egg.right() < width() && !egg.intersects(grid);
}

void VisualizationPlaceholderWidget::placeEasterEgg() {
  if (easter_egg_ == nullptr) {
    return;
  }
  easter_egg_->setGeometry(easterEggRect());
  easter_egg_->setVisible(easterEggFits());
}

void VisualizationPlaceholderWidget::onStylesheetChanged(const QString& theme) {
  updateSplitActionIcons(theme);

  // Cache the Data Backdrop fill for the active theme and repaint. Qualified
  // PJ::theme:: to see past the `theme` parameter.
  const bool light = theme.contains(QStringLiteral("light"));
  backdrop_color_ = PJ::theme::surface(PJ::theme::Surface::DataBackdrop, PJ::theme::themeFor(light));
  update();

  theme_ = theme;
  renderIcons();
}

void VisualizationPlaceholderWidget::renderIcons() {
  // renderSvgPixmap, not loadSvg: the central icons rasterize at exactly their display size (DPR baked in) and stay
  // crisp. The shared loadSvg cache always renders at 64x64, downsampled to 48x48 here, which blurs the larger
  // placeholder buttons.
  for (const auto& entry : icon_buttons_) {
    const QSize icon_size = entry.button->iconSize();
    const QPixmap pixmap = renderSvgPixmap(entry.icon_path, theme_, icon_size, devicePixelRatioF());
    entry.button->setIcon(QIcon(pixmap));
  }
}

int VisualizationPlaceholderWidget::columnCount() const {
  return std::min(static_cast<int>(icon_buttons_.size()), kColumns);
}

int VisualizationPlaceholderWidget::rowCount() const {
  return (static_cast<int>(icon_buttons_.size()) + kColumns - 1) / kColumns;
}

void VisualizationPlaceholderWidget::fitButtons() {
  const QMargins margins = layout()->contentsMargins();
  const int spacing = layout()->spacing();
  const int columns = columnCount();
  const int rows = rowCount();
  const int by_width = (width() - margins.left() - margins.right() - (columns - 1) * spacing) / columns;
  const int by_height = (height() - margins.top() - margins.bottom() - (rows - 1) * spacing) / rows;
  const int side = std::clamp(std::min(by_width, by_height), kMinButtonSide, kButtonSide);
  if (icon_buttons_.front().button->width() == side) {
    return;
  }
  const int icon = qRound(side * double(kIconSide) / double(kButtonSide));
  for (const auto& entry : icon_buttons_) {
    entry.button->setFixedSize(side, side);
    entry.button->setIconSize(QSize(icon, icon));
  }
  renderIcons();
}

QSize VisualizationPlaceholderWidget::gridSize(int side) const {
  const QMargins margins = layout()->contentsMargins();
  const int spacing = layout()->spacing();
  return {
      lineLength(columnCount(), side, spacing) + margins.left() + margins.right(),
      lineLength(rowCount(), side, spacing) + margins.top() + margins.bottom()};
}

QSize VisualizationPlaceholderWidget::sizeHint() const {
  return gridSize(kButtonSide);
}

QSize VisualizationPlaceholderWidget::minimumSizeHint() const {
  return gridSize(kMinButtonSide);
}

void VisualizationPlaceholderWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  fitButtons();
  placeEasterEgg();
}

void VisualizationPlaceholderWidget::paintEvent(QPaintEvent* event) {
  if (backdrop_color_.isValid()) {
    QPainter painter(this);
    painter.fillRect(event->rect(), backdrop_color_);
  }
  QWidget::paintEvent(event);
}

void VisualizationPlaceholderWidget::contextMenuEvent(QContextMenuEvent* event) {
  if (event == nullptr) {
    return;
  }
  showSplitContextMenu(event->globalPos());
  event->accept();
}

bool VisualizationPlaceholderWidget::eventFilter(QObject* watched, QEvent* event) {
  Q_UNUSED(watched)
  if (event == nullptr) {
    return QWidget::eventFilter(watched, event);
  }

  switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove:
      return acceptCatalogDrag(static_cast<QDropEvent*>(event));
    case QEvent::Drop:
      return dropCatalogItems(static_cast<QDropEvent*>(event), this);
    case QEvent::ContextMenu:
      showSplitContextMenu(static_cast<QContextMenuEvent*>(event)->globalPos());
      event->accept();
      return true;
    default:
      break;
  }
  return QWidget::eventFilter(watched, event);
}

void VisualizationPlaceholderWidget::dragEnterEvent(QDragEnterEvent* event) {
  if (acceptCatalogDrag(event)) {
    return;
  }
  QWidget::dragEnterEvent(event);
}

void VisualizationPlaceholderWidget::dragMoveEvent(QDragMoveEvent* event) {
  if (acceptCatalogDrag(event)) {
    return;
  }
  QWidget::dragMoveEvent(event);
}

void VisualizationPlaceholderWidget::dropEvent(QDropEvent* event) {
  if (dropCatalogItems(event, this)) {
    return;
  }
  QWidget::dropEvent(event);
}

void VisualizationPlaceholderWidget::showSplitContextMenu(const QPoint& global_pos) {
  emit contextMenuAboutToShow();
  updateSplitActionIcons(currentTheme());

  auto* menu = new QMenu(this);
  menu->setObjectName(u"PJMenu"_s);
  menu->setProperty("categorySeparators", true);
  menu->addAction(action_paste_);
  menu->addSeparator();
  menu->addAction(action_split_horizontal_);
  menu->addAction(action_split_vertical_);
  showOwnedMenu(menu, global_pos);
}

void VisualizationPlaceholderWidget::updateSplitActionIcons(const QString& theme) {
  action_paste_->setIcon(QIcon(loadSvg(":/resources/svg/paste.svg", theme)));
  action_split_horizontal_->setIcon(QIcon(loadSvg(":/resources/svg/add_column.svg", theme)));
  action_split_vertical_->setIcon(QIcon(loadSvg(":/resources/svg/add_row.svg", theme)));
}

void noteLayoutActivity() {
  IdleEasterEgg::restart();
}

QString easterEggCharacter(const QString& gif_path) {
  const QString base = QFileInfo(gif_path).baseName();
  const auto underscore = base.lastIndexOf(u'_');
  return underscore > 0 ? base.left(underscore) : base;
}

QStringList easterEggPool() {
  const QDir dir(QString::fromLatin1(kEasterEggDir));
  QStringList paths;
  for (const QString& name : dir.entryList({u"*.gif.enc"_s}, QDir::Files)) {
    paths.push_back(dir.filePath(name));
  }
  return paths;
}

QByteArray loadEasterEgg(const QString& path) {
  QFile file(path);
  if (sizeof(kEasterEggKey) <= 1 || !file.open(QIODevice::ReadOnly)) {
    return {};
  }
  QByteArray gif = easterEggCipher(file.readAll(), QByteArray(kEasterEggKey), QFileInfo(path).fileName());
  return gif.startsWith("GIF8") ? gif : QByteArray();
}

QByteArray easterEggCipher(QByteArray data, const QByteArray& key, const QString& name) {
  const QByteArray salt = key + name.toUtf8();
  constexpr qsizetype kBlock = 32;  // one SHA-256 digest
  for (qsizetype offset = 0; offset < data.size(); offset += kBlock) {
    const auto counter = qToLittleEndian(static_cast<quint64>(offset / kBlock));
    const QByteArray pad = QCryptographicHash::hash(
        salt + QByteArray(reinterpret_cast<const char*>(&counter), sizeof(counter)), QCryptographicHash::Sha256);
    const qsizetype end = std::min(offset + kBlock, data.size());
    for (qsizetype i = offset; i < end; ++i) {
      data[i] = static_cast<char>(data[i] ^ pad[i - offset]);
    }
  }
  return data;
}

QString pickEasterEggGif(const QStringList& pool, const QString& character) {
  QStringList variants;
  for (const QString& gif : pool) {
    if (easterEggCharacter(gif) == character) {
      variants.push_back(gif);
    }
  }
  const QStringList& from = variants.isEmpty() ? pool : variants;
  return from.isEmpty() ? QString() : from[QRandomGenerator::global()->bounded(from.size())];
}

}  // namespace PJ
