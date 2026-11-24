#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>
#include <vector>

#include "pj_widgets/VisualizationKind.h"

class QAction;
class QContextMenuEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QLabel;
class QObject;
class QPaintEvent;
class QPoint;
class QResizeEvent;
class QToolButton;

namespace PJ {

// Neutral dock content shown before the user chooses a visualization family. The Plot, State Transitions, 2D, 3D and
// Custom Widget icons are clickable: a click emits visualizationRequested so the host converts the dock into an empty
// widget of that family (for Custom Widget the host first asks which widget). A catalog drop still creates and
// populates the matching widget directly.
class VisualizationPlaceholderWidget : public QWidget {
  Q_OBJECT
 public:
  explicit VisualizationPlaceholderWidget(QWidget* parent = nullptr);
  // Creation and destruction are layout activity (noteLayoutActivity): a new dock or tab, a dock cleared, or the
  // placeholder replaced by a widget.
  ~VisualizationPlaceholderWidget() override;
  // The icon grid (three columns, filled row by row) at full size, and at its smallest: the dock's scroll area reads
  // the minimum, so a small dock shrinks the icons instead of scrolling them.
  [[nodiscard]] QSize sizeHint() const override;
  [[nodiscard]] QSize minimumSizeHint() const override;
  // Enables Paste when the host sees compatible widget XML in the clipboard.
  void setPasteActionEnabled(bool enabled);
  // Enables the Custom Widget icon when at least one Widget type is installed. When disabled its tooltip says so (and
  // points at the Marketplace unless this build has none). Enabled by default.
  void setCustomWidgetsAvailable(bool available);
  // The Custom Widget icon (null only if construction failed).
  [[nodiscard]] QToolButton* customWidgetButton() const;
  // Shows `gif` (the file's bytes) looping in the bottom-left corner as the idle easter egg, unless it is empty, the
  // placeholder is hidden or the GIF would cover a family icon. Returns whether it is now shown. The app-wide idle
  // watcher (armed by the first placeholder) decides when and which GIF (pickEasterEggGif), at most once per process.
  bool showEasterEgg(const QByteArray& gif);

 public slots:
  // Re-tints the family icons through renderSvgPixmap to the active theme's ink (light #3D3D3D, dark #E0E0E0), as every
  // other chrome icon. DockWidget routes MainWindow's stylesheetChanged signal here.
  void onStylesheetChanged(const QString& theme);

 signals:
  void catalogItemsDropped(QStringList keys);
  // Emitted instead of catalogItemsDropped when the drop is the "create XY plot" gesture (a right-drag of exactly two
  // curves, the new_XY_axis mime): the host makes an XY plot, not two time-series curves.
  void catalogItemsXyRequested(QStringList keys);
  void splitHorizontalRequested();
  void splitVerticalRequested();
  // Emitted when the placeholder Paste menu action is triggered.
  void pasteRequested();
  // Emitted just before the context menu is built, so Paste state can refresh.
  void contextMenuAboutToShow();
  // Emitted when the user clicks a family icon; the host converts the placeholder into an empty widget of that family
  // (no data bound yet).
  void visualizationRequested(VisualizationKind kind);

 protected:
  // Fills the widget with the Data Backdrop surface for the active theme. Self-painted, not autoFillBackground or QSS:
  // the app stylesheet's QStyleSheetStyle can clobber a widget palette, so painting reads the framework colour cached
  // by onStylesheetChanged.
  void paintEvent(QPaintEvent* event) override;
  void contextMenuEvent(QContextMenuEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dragMoveEvent(QDragMoveEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  // Renders every family icon for theme_ at its button's current icon size.
  void renderIcons();
  // Sizes every button so the grid fits within width() and height(); no-op when unchanged.
  void fitButtons();
  // The grid's size for buttons of `side` px.
  [[nodiscard]] QSize gridSize(int side) const;
  [[nodiscard]] int columnCount() const;
  [[nodiscard]] int rowCount() const;
  void showSplitContextMenu(const QPoint& global_pos);
  void updateSplitActionIcons(const QString& theme);
  // Where the easter egg sits: bottom-left, inset by the comfortable spacing.
  [[nodiscard]] QRect easterEggRect() const;
  // Whether easterEggRect lies inside the placeholder, clear of the icon grid.
  [[nodiscard]] bool easterEggFits() const;
  // Moves the easter egg with the corner, hidden while the icons shrink into its corner.
  void placeEasterEgg();

  struct IconButton {
    QToolButton* button;
    QString icon_path;
  };
  QAction* action_split_horizontal_ = nullptr;
  QAction* action_split_vertical_ = nullptr;
  QAction* action_paste_ = nullptr;
  std::vector<IconButton> icon_buttons_;
  // Data Backdrop fill, refreshed per theme by onStylesheetChanged.
  QColor backdrop_color_;
  // The theme of the last onStylesheetChanged, to re-render the icons when the buttons resize.
  QString theme_;
  QLabel* easter_egg_ = nullptr;
};

// Restarts the idle easter egg's countdown. Only layout activity counts, a tab or a widget created or removed:
// placeholders report themselves, and a dock host reports each dock it destroys. No-op until the first placeholder
// arms the watcher, and once it has shown.
void noteLayoutActivity();

// An easter egg GIF's character: its base name (up to the first '.') before the last '_' ("llama-coat" for
// "llama-coat_A.gif.enc"), or the whole base name when it has no '_'.
[[nodiscard]] QString easterEggCharacter(const QString& gif_path);
// A random GIF of `pool` among the variants of `character`; any GIF of `pool` when none matches (the user's first
// easter egg, or a character no longer shipped). Empty only when `pool` is.
// The encrypted easter egg files: resource paths on the desktop; in the browser, file names for the fetch hook
// (setEasterEggFetch), and none until it is set.
[[nodiscard]] QStringList easterEggPool();
// A pool entry decrypted: from the resources (loadEasterEgg), or its fetched bytes. Empty when the file is missing or
// this build has no key or the wrong one.
[[nodiscard]] QByteArray loadEasterEgg(const QString& path);
[[nodiscard]] QByteArray decryptEasterEgg(const QByteArray& encrypted, const QString& path);
#ifdef PJ_TARGET_WASM
// The browser build carries no easter egg files: the host fetches pool entry `name` and calls `done` with its bytes
// (empty on failure) on `context`'s thread, or never once `context` is destroyed.
using EasterEggFetch =
    std::function<void(const QString& name, QObject* context, std::function<void(const QByteArray&)> done)>;
void setEasterEggFetch(EasterEggFetch fetch);
#endif
// Encrypts or decrypts (the same XOR) an easter egg file: a SHA-256 counter keystream over `key` and the file `name`.
// scripts/pack_easter_eggs.py implements the same cipher; the two must stay byte-identical.
[[nodiscard]] QByteArray easterEggCipher(QByteArray data, const QByteArray& key, const QString& name);
[[nodiscard]] QString pickEasterEggGif(const QStringList& pool, const QString& character);

}  // namespace PJ
