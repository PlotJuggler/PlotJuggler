// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PreferencesDialog.h"

#include <QAbstractItemModel>
#include <QBrush>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QIcon>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkInformation>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStackedWidget>
#include <QStyle>
#include <QSvgRenderer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "DebugMode.h"
#include "MainWindow.h"
#include "PreferencesNavRow.h"
#include "Splashscreen.h"
#include "Theme.h"
#include "pj_runtime/HttpGet.h"
#include "pj_runtime/RecordingService.h"
#include "pj_widgets/DualOptionsWidget.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/SvgButton.h"
#include "pj_widgets/ToggleSwitch.h"
#include "ui_PreferencesDialog.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// First-launch defaults for the chrome-metric scrubbers. Mirrored from
// MainWindow.cpp's k*Default constants — kept in sync by hand because
// the reset button has to match the codepath that runs when QSettings
// has no saved value yet.
constexpr int kDefaultIconSize = 24;
constexpr int kDefaultIconPadding = 4;
constexpr int kDefaultLayoutPadding = 2;
constexpr int kDefaultLayoutSpacing = 2;

QIcon loadIconInkIcon(const QString& resource_path, theme::Theme token_theme) {
  QFile file(resource_path);
  if (!file.open(QFile::ReadOnly | QFile::Text)) {
    return {};
  }
  QString svg = QString::fromUtf8(file.readAll());
  const QString ink = theme::iconInk(token_theme).name(QColor::HexRgb);
  svg.replace(QRegularExpression(QStringLiteral(R"(fill="#[0-9A-Fa-f]{6}")")), QStringLiteral("fill=\"%1\"").arg(ink));
  QSvgRenderer renderer(svg.toUtf8());
  QImage image(64, 64, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(&painter);
  painter.end();
  return QIcon(QPixmap::fromImage(image));
}

}  // namespace

PreferencesDialog::PreferencesDialog(Theme& theme, QWidget* parent)
    : Dialog(parent),
      ui_(new Ui::PreferencesContent),
      theme_(theme),
      original_theme_(theme.currentTheme()),
      original_metrics_(
          qobject_cast<MainWindow*>(parent) != nullptr ? qobject_cast<MainWindow*>(parent)->chromeMetrics()
                                                       : ChromeMetrics{}) {
  setDialogTitle(tr("Preferences"));
  // The Dialog content area already has its own (zero-margin) layout, so
  // we instantiate the .ui onto a child body widget rather than setupUi(this).
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);
  // Free the dialog to resize down. The base Dialog's top-level layout uses Qt's
  // default SetDefaultConstraint, which on every activation forces the window's
  // minimum size up to the layout's computed minimum — and with the Appearance
  // page's Fixed-height scrubber rows that minimum is tall enough to block
  // shrinking (and to override a smaller restored/explicit size). SetNoConstraint
  // stops the layout from imposing that minimum (children still fill via their
  // own layouts); the explicit minimum below is then the only floor.
  if (auto* root = layout()) {
    root->setSizeConstraint(QLayout::SetNoConstraint);
  }
  setMinimumSize(420, 300);
  // Restore the user's last dialog size if they resized it before; otherwise a
  // sensible default (room for the 160 px nav column + a comfortable page body).
  // The dialog is resizable (edge-drag handled by the base class); geometry is
  // saved on close (see the destructor).
  {
    QSettings settings;
    const QByteArray geometry = settings.value(u"Preferences::dialog_geometry"_s).toByteArray();
    // A stale/corrupt blob (Qt upgrade, truncated .ini) makes restoreGeometry
    // return false and apply nothing — fall back to the default size rather than
    // opening off-screen or at 0x0.
    if (geometry.isEmpty() || !restoreGeometry(geometry)) {
      resize(560, 400);
    }
  }

  // Populate the left-hand nav column. Each row is a click-target that
  // switches pagesStack to the matching index. The trailing stretch in
  // navLayout (added in .ui) pushes the rows to the top.
  auto* nav_layout = qobject_cast<QVBoxLayout*>(ui_->navContainer->layout());
  std::vector<QString> nav_names{
      tr("Appearance"), tr("Plotting"), tr("Scene 2D"), tr("Scene 3D"), tr("Plugins"),
  };
  if (RecordingService::isSupported()) {
    nav_names.push_back(tr("Recording"));
  } else {
    // No sink to record into, so no nav row; hiding the page also keeps it out
    // of the stack's size hint.
    ui_->pageRecording->hide();
  }
  nav_rows_.reserve(nav_names.size());
  for (const QString& label : nav_names) {
    const int index = static_cast<int>(nav_rows_.size());  // rows follow pagesStack order
    auto* row = new PreferencesNavRow(label, ui_->navContainer);
    nav_layout->insertWidget(index, row);
    nav_rows_.push_back(row);
    connect(row, &PreferencesNavRow::clicked, this, [this, row, index]() {
      for (auto* other : nav_rows_) {
        other->setSelected(other == row);
      }
      ui_->pagesStack->setCurrentIndex(index);
    });
  }
  if (!nav_rows_.empty()) {
    nav_rows_.front()->setSelected(true);
    ui_->pagesStack->setCurrentIndex(kAppearancePage);
  }

  // Developer-only chrome-metric scrubbers (icon/layout sizing) ship hidden;
  // --debug-mode reveals them. They sit in a spanning row of the Appearance
  // form, so toggle the whole row (setRowVisible) to reclaim its vertical space
  // when hidden — hiding only the widget would leave the row's gap.
  if (auto* appearance_form = qobject_cast<QFormLayout*>(ui_->pageAppearance->layout())) {
    int row = -1;
    QFormLayout::ItemRole role{};
    appearance_form->getWidgetPosition(ui_->debugChromeWidget, &row, &role);
    if (row >= 0) {
      appearance_form->setRowVisible(row, isDebugMode());
    }
  }

  // Scene 3D page is a placeholder for now — the asset-folder controls are
  // shown but disabled until the feature lands.
  ui_->pageScene3D->setEnabled(false);

  // Icon-size + icon-padding scrubbers. Ranges match the spec and the
  // clamps inside MainWindow::setIconSize / setIconPadding. Initial
  // values come from MainWindow so the dialog reflects the running
  // app's current state. Live preview: each valueChanged tick pushes
  // straight through MainWindow's setter (which clamps, persists, and
  // emits iconMetricsChanged) so the running app resizes in real time
  // while the user scrubs. On reject (Esc or the title-bar close) we restore the snapshot.
  auto* main_window = qobject_cast<MainWindow*>(parent);
  ui_->iconSizeScrubber->setRange(12, 48);
  ui_->iconSizeScrubber->setSingleStep(1);
  ui_->iconSizeScrubber->setSuffix(u" px"_s);
  ui_->iconSizeScrubber->setValue(original_metrics_.icon_size);

  ui_->iconPaddingScrubber->setRange(0, 32);
  ui_->iconPaddingScrubber->setSingleStep(1);
  ui_->iconPaddingScrubber->setSuffix(u" px"_s);
  ui_->iconPaddingScrubber->setValue(original_metrics_.icon_padding);

  ui_->layoutPaddingScrubber->setRange(0, 16);
  ui_->layoutPaddingScrubber->setSingleStep(1);
  ui_->layoutPaddingScrubber->setSuffix(u" px"_s);
  ui_->layoutPaddingScrubber->setValue(original_metrics_.layout_padding);

  ui_->layoutSpacingScrubber->setRange(0, 16);
  ui_->layoutSpacingScrubber->setSingleStep(1);
  ui_->layoutSpacingScrubber->setSuffix(u" px"_s);
  ui_->layoutSpacingScrubber->setValue(original_metrics_.layout_spacing);

  // Value preferences seeded together (one QSettings read pass) and committed on
  // OK below. The scoped block keeps `settings` from shadowing the same-named
  // local in the accept/reject handlers (which run later as slots):
  //  - Float precision (Appearance): 1-6 decimals; its readers (plot tooltips,
  //    the curve tracker, the curve-list value column) re-read the key on their
  //    next redraw.
  //  - Curve-colour sequence (Plotting): global = one continuous colour sequence
  //    across all plots; per plot = the sequence restarts within each plot.
  //  - OpenGL (Appearance): default on; PlotWidgetBase reads the key when a plot
  //    is constructed (applies to newly created plots). --disable-opengl can
  //    force it off for a session without touching this saved value.
  ui_->scrubberFloatPrecision->setRange(1, 6);
  ui_->scrubberFloatPrecision->setSingleStep(1);
  ui_->curveColorMode->setOptions(tr("global"), tr("per plot"));
  ui_->splashMode->setOptions(tr("memes"), tr("serious"));
  {
    QSettings settings;
    ui_->scrubberFloatPrecision->setValue(settings.value(u"Preferences::precision"_s, 3).toInt());
    ui_->curveColorMode->setSelectedIndex(settings.value(u"Preferences::curve_color_global"_s, true).toBool() ? 0 : 1);
    ui_->openglToggle->setChecked(
        settings.value(u"Preferences::use_opengl"_s, true).toBool(),
        /*animate=*/false);
    ui_->checkUpdatesToggle->setChecked(
        settings.value(u"Preferences::check_updates_on_startup"_s, true).toBool(),
        /*animate=*/false);
    ui_->telemetryToggle->setChecked(
        settings.value(u"Preferences::send_anonymous_stats"_s, true).toBool(),
        /*animate=*/false);
    ui_->splashMode->setSelectedIndex(
        settings.value(kSplashModeKey, kSplashModeMemes).toString() == kSplashModeSerious ? 1 : 0);
  }

  if (main_window != nullptr) {
    connect(ui_->iconSizeScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setIconSize);
    connect(ui_->iconPaddingScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setIconPadding);
    connect(ui_->layoutPaddingScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setLayoutPadding);
    connect(ui_->layoutSpacingScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setLayoutSpacing);
  }

  // Plugins page: the user-managed custom folder list (drag-reorderable, with
  // add/remove) plus the read-only built-in folders. Folders that do not exist
  // on disk render in red. The custom list persists on OK and applies on next
  // launch (no hot reload of extensions).
  if (main_window != nullptr) {
    auto paint_missing = [this](QListWidget* list) {
      const auto token_theme = theme::themeFor(theme_.currentTheme() == QLatin1String("light"));
      const QBrush missing_brush(theme::interaction(theme::Variant::Highlight, theme::State::Nominal, token_theme));
      for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        const bool missing = !QDir(item->text()).exists();
        item->setForeground(missing ? missing_brush : QBrush());
        item->setToolTip(missing ? tr("This folder does not exist.") : QString());
      }
    };

    ui_->listCustomPluginFolders->setDragDropMode(QAbstractItemView::InternalMove);
    ui_->listCustomPluginFolders->addItems(main_window->customPluginFolders());
    paint_missing(ui_->listCustomPluginFolders);
    // A drag-reorder re-serializes the items and drops their foreground brush, so
    // repaint the missing-folder tint after a move.
    connect(ui_->listCustomPluginFolders->model(), &QAbstractItemModel::rowsMoved, this, [this, paint_missing]() {
      paint_missing(ui_->listCustomPluginFolders);
    });

    ui_->listDefaultPluginFolders->addItems(main_window->builtinPluginFolders());
    ui_->listDefaultPluginFolders->setSelectionMode(QAbstractItemView::NoSelection);
    ui_->listDefaultPluginFolders->setFocusPolicy(Qt::NoFocus);
    paint_missing(ui_->listDefaultPluginFolders);
    connect(&theme_, &Theme::themeChanged, this, [this, paint_missing](const QString&) {
      paint_missing(ui_->listCustomPluginFolders);
      paint_missing(ui_->listDefaultPluginFolders);
    });

    // Marketplace registry URL editor. An explicit default/custom mode switch:
    // "default" shows the built-in URL read-only, "custom" enables the field.
    // Validation runs when editing finishes and paints the text red while the
    // URL is invalid or unreachable — advisory, never blocking (see
    // onRegistryUrlEditingFinished).
    ui_->registryUrlMode->setOptions(tr("default"), tr("custom"));
    const QString stored_override = main_window->registryUrlSetting();
    ui_->registryUrlMode->setSelectedIndex(stored_override.isEmpty() ? 0 : 1);
    ui_->lineEditRegistryUrl->setText(stored_override.isEmpty() ? MainWindow::defaultRegistryUrl() : stored_override);
    ui_->lineEditRegistryUrl->setEnabled(!stored_override.isEmpty());
    connect(ui_->registryUrlMode, &DualOptionsWidget::selectionChanged, this, [this](int index) {
      const bool custom = index == 1;
      ui_->lineEditRegistryUrl->setEnabled(custom);
      if (custom) {
        ui_->lineEditRegistryUrl->setFocus();
        ui_->lineEditRegistryUrl->selectAll();
      } else {
        ui_->lineEditRegistryUrl->setText(MainWindow::defaultRegistryUrl());
        last_checked_registry_url_.clear();
        setRegistryUrlError(false);
      }
    });
    connect(
        ui_->lineEditRegistryUrl, &QLineEdit::editingFinished, this, &PreferencesDialog::onRegistryUrlEditingFinished);
    // A stored custom URL gets checked (and possibly painted red) right away,
    // so a stale override is visible without touching the field.
    if (!stored_override.isEmpty()) {
      onRegistryUrlEditingFinished();
    }

    // SvgButton re-tints itself on a theme change — no manual retint wiring.
    ui_->buttonAddPluginFolder->setIconPath(u":/resources/svg/add.svg"_s);
    ui_->buttonAddPluginFolder->setExtent(26, 24);
    ui_->buttonRemovePluginFolder->setIconPath(u":/resources/svg/trash.svg"_s);
    ui_->buttonRemovePluginFolder->setExtent(26, 24);
    ui_->buttonAddPluginFolder->setToolTip(tr("Add a plugin folder…"));
    ui_->buttonRemovePluginFolder->setToolTip(tr("Remove the selected folder"));
    connect(ui_->buttonAddPluginFolder, &QToolButton::clicked, this, [this, paint_missing]() {
      const QString dir = PJ::FileDialog::getExistingDirectory(this, tr("Add plugin folder"));
      if (!dir.isEmpty()) {
        ui_->listCustomPluginFolders->addItem(dir);
        paint_missing(ui_->listCustomPluginFolders);
      }
    });
    connect(ui_->buttonRemovePluginFolder, &QToolButton::clicked, this, [this, paint_missing]() {
      qDeleteAll(ui_->listCustomPluginFolders->selectedItems());
      paint_missing(ui_->listCustomPluginFolders);
    });
  }

  // Recording page. Wired only where the page is reachable, so an unreachable
  // page's .ui defaults can never be committed over the stored settings.
  if (RecordingService::isSupported()) {
    const RecordingService::Settings stored = RecordingService::loadSettings();
    ui_->lineEditRecordingDir->setText(stored.directory);
    // Left empty, recordings land in the default folder: name it so the user can
    // find the files without having to pick a folder first.
    ui_->lineEditRecordingDir->setToolTip(tr("Leave empty to use the default folder: %1")
                                              .arg(QDir::toNativeSeparators(RecordingService::defaultDirectory())));
    ui_->scrubberRecordingQueueMiB->setValue(stored.queue_budget_mib);

    // SvgButton re-tints itself on a theme change — no manual retint wiring.
    ui_->buttonRecordingDirBrowse->setIconPath(u":/resources/svg/folder_open.svg"_s);
    ui_->buttonRecordingDirBrowse->setExtent(26, 24);
    ui_->buttonRecordingDirBrowse->setToolTip(tr("Choose the recordings folder…"));
    connect(ui_->buttonRecordingDirBrowse, &QToolButton::clicked, this, [this]() {
      // An empty field means the default folder, so start the picker there
      // rather than in the process working directory.
      const QString field = ui_->lineEditRecordingDir->text().trimmed();
      const QString dir = PJ::FileDialog::getExistingDirectory(
          this, tr("Choose the recordings folder"), field.isEmpty() ? RecordingService::defaultDirectory() : field);
      if (!dir.isEmpty()) {
        ui_->lineEditRecordingDir->setText(dir);
      }
    });

    // No rejected handler: neither of these is applied live, so Cancel never
    // reaches the QSettings write below.
    connect(this, &QDialog::accepted, this, [this]() {
      RecordingService::Settings settings;
      // Trimmed, so a stray space cannot turn the empty "default folder" value
      // into a relative path.
      settings.directory = ui_->lineEditRecordingDir->text().trimmed();
      settings.queue_budget_mib = ui_->scrubberRecordingQueueMiB->value();
      RecordingService::saveSettings(settings);
      emit recordingSettingsChanged();
    });
  }

  // Reset-to-defaults button. Snaps each scrubber back to the
  // first-launch defaults; the scrubbers' valueChanged signals
  // already feed MainWindow's setters, so the running app live-
  // previews the reset, and rejecting the dialog (Esc / title-bar close)
  // still reverts to the dialog's open-time snapshot.
  // Same glyph as the timeline align-rail "reset all" button (restart_alt).
  // SvgButton re-tints itself on a theme change.
  ui_->buttonResetDefaults->setIconPath(u":/resources/svg/restart_alt.svg"_s);
  ui_->buttonResetDefaults->setSize(SvgButton::Size::kDefault);
  connect(ui_->buttonResetDefaults, &QToolButton::clicked, this, [this]() {
    ui_->iconSizeScrubber->setValue(kDefaultIconSize);
    ui_->iconPaddingScrubber->setValue(kDefaultIconPadding);
    ui_->layoutPaddingScrubber->setValue(kDefaultLayoutPadding);
    ui_->layoutSpacingScrubber->setValue(kDefaultLayoutSpacing);
  });

  // Toggle: thumb-left = light, thumb-right = dark. The icon visible
  // in the un-covered slot represents the *destination* state — moon
  // when in light mode (click to go dark), sun when in dark mode.
  // Mapping: thumb-right (checked = true) → light theme, sun
  // visible in the left slot. thumb-left (checked = false) → dark
  // theme, moon visible in the right slot. "ON" reads as the
  // bright/active state.
  //
  // Larger than the compact 34x18 default so the sun/moon icons read clearly.
  ui_->themeToggle->setFixedSize(44, 24);
  auto apply_toggle_icons = [this]() {
    const auto token_theme = theme::themeFor(theme_.currentTheme() == QLatin1String("light"));
    ui_->themeToggle->setLeftIcon(loadIconInkIcon(QStringLiteral(":/resources/svg/light_mode_light.svg"), token_theme));
    ui_->themeToggle->setRightIcon(loadIconInkIcon(QStringLiteral(":/resources/svg/dark_mode_light.svg"), token_theme));
  };
  apply_toggle_icons();
  // Snap the toggle to the active theme without animating — the
  // dialog opens with the thumb already at its correct endpoint,
  // not mid-slide from 0 to 1 across the first 180ms after open.
  ui_->themeToggle->setChecked(original_theme_ == QLatin1String("light"), /*animate=*/false);

  // One-shot lock around the whole click → animation → setTheme →
  // propagation cycle. Three pieces:
  //
  //   1. `clicked` fires from mouseReleaseEvent / keyPressEvent
  //      right after the animation starts, BEFORE any subsequent
  //      input can be delivered. Disabling here blocks every
  //      further click for the entire 180ms slide, so the toggle
  //      can't be "trilled" mid-animation.
  //
  //   2. `toggled` fires only on natural animation completion
  //      (ToggleSwitch ties it to QPropertyAnimation::finished),
  //      so setTheme runs exactly once per locked cycle at the
  //      settled state.
  //
  //   3. `stylesheetChanged` re-enables — but via Qt::QueuedConnection.
  //      The signal is emitted synchronously inside setTheme (deep
  //      in MainWindow::onThemeChanged, after qApp->setStyleSheet
  //      + applyIcons + every dependent widget's onStylesheetChanged
  //      runs). A direct re-enable here would land mid-cascade,
  //      while the rest of setTheme (qssChanged → apply_theme_chrome →
  //      another qApp->setStyleSheet) is still running. Queued
  //      posts to the event loop and only fires after the entire
  //      synchronous chain has unwound and the event loop has
  //      drained any queued repaint events. By that point the
  //      toggle's visual state and theme_.currentTheme() are
  //      guaranteed to match.
  if (main_window != nullptr) {
    connect(
        main_window, &MainWindow::stylesheetChanged, this,
        [this, apply_toggle_icons](const QString&) {
          apply_toggle_icons();
          ui_->themeToggle->setEnabled(true);
        },
        Qt::QueuedConnection);
  }
  connect(ui_->themeToggle, &ToggleSwitch::clicked, this, [this]() { ui_->themeToggle->setEnabled(false); });
  connect(ui_->themeToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
    theme_.setTheme(checked ? u"light"_s : u"dark"_s);
  });

  // Plotting page: auto-zoom plots. When on, adding/removing a curve rescales
  // that plot's Y axis to fit (see PlotWidget::autoZoomPlotVertically). Seed
  // from the persisted preference (no animation — open at the settled
  // position); like the other plotting prefs this commits only on OK.
  {
    QSettings settings;
    ui_->autoZoomToggle->setChecked(
        settings.value(u"Preferences::auto_zoom_plots"_s, true).toBool(),
        /*animate=*/false);
  }
  connect(this, &QDialog::rejected, this, [this, main_window]() {
    theme_.setTheme(original_theme_);
    if (main_window != nullptr) {
      main_window->setIconSize(original_metrics_.icon_size);
      main_window->setIconPadding(original_metrics_.icon_padding);
      main_window->setLayoutPadding(original_metrics_.layout_padding);
      main_window->setLayoutSpacing(original_metrics_.layout_spacing);
    }
  });
  // The chrome setters above only apply live — commit to QSettings on OK. Rejecting
  // restores the snapshot and never persisted, so the .ini keeps the originals.
  connect(this, &QDialog::accepted, this, [this, main_window]() {
    if (main_window != nullptr) {
      main_window->persistChromeMetrics();
      QStringList plugin_folders;
      for (int row = 0; row < ui_->listCustomPluginFolders->count(); ++row) {
        plugin_folders << ui_->listCustomPluginFolders->item(row)->text();
      }
      main_window->setCustomPluginFolders(plugin_folders);

      // Registry URL: default mode (or a custom value equal to the default)
      // stores "no override", so the user keeps following future defaults. A
      // syntactically valid custom URL persists; reachability is advisory (red
      // text), never a blocker — the URL may legitimately be offline right now.
      const bool custom_registry = ui_->registryUrlMode->selectedIndex() == 1;
      const QString registry_url = ui_->lineEditRegistryUrl->text().trimmed();
      if (!custom_registry || registry_url == MainWindow::defaultRegistryUrl()) {
        main_window->setRegistryUrlSetting({});
      } else if (MainWindow::isValidRegistryUrl(registry_url)) {
        main_window->setRegistryUrlSetting(registry_url);
      }
    }
    QSettings settings;
    settings.setValue(u"Preferences::precision"_s, ui_->scrubberFloatPrecision->value());
    settings.setValue(u"Preferences::use_opengl"_s, ui_->openglToggle->isChecked());
    settings.setValue(u"Preferences::check_updates_on_startup"_s, ui_->checkUpdatesToggle->isChecked());
    settings.setValue(u"Preferences::send_anonymous_stats"_s, ui_->telemetryToggle->isChecked());
    settings.setValue(u"Preferences::curve_color_global"_s, ui_->curveColorMode->selectedIndex() == 0);
    settings.setValue(kSplashModeKey, ui_->splashMode->selectedIndex() == 1 ? kSplashModeSerious : kSplashModeMemes);
    settings.setValue(u"Preferences::auto_zoom_plots"_s, ui_->autoZoomToggle->isChecked());
  });

  connect(ui_->buttonOk, &QPushButton::clicked, this, &QDialog::accept);
}

PreferencesDialog::~PreferencesDialog() {
  // Remember the dialog size across launches regardless of OK/Cancel — window
  // size is a UI preference, not a settings change.
  QSettings settings;
  settings.setValue(u"Preferences::dialog_geometry"_s, saveGeometry());
  delete ui_;
}

void PreferencesDialog::showPage(int index) {
  for (std::size_t i = 0; i < nav_rows_.size(); ++i) {
    nav_rows_[i]->setSelected(static_cast<int>(i) == index);
  }
  ui_->pagesStack->setCurrentIndex(index);
}

void PreferencesDialog::onRegistryUrlEditingFinished() {
  const QString url_text = ui_->lineEditRegistryUrl->text().trimmed();
  if (url_text == last_checked_registry_url_) {
    return;  // already checked (or being checked) — no duplicate probe
  }
  last_checked_registry_url_ = url_text;

  if (url_text.isEmpty() || !MainWindow::isValidRegistryUrl(url_text)) {
    setRegistryUrlError(true);
    return;
  }

  const QUrl url(url_text);
  if (url.isLocalFile()) {
    setRegistryUrlError(!QFile::exists(url.toLocalFile()));
    return;
  }

  // Reachability probe. When the system is not reported Online (offline, or no
  // usable backend to tell), skip it and show the value as fine — a network
  // outage must not paint a URL red that will work once connectivity returns.
  [[maybe_unused]] static const bool backend_loaded = QNetworkInformation::loadDefaultBackend();
  QNetworkInformation* network_info = QNetworkInformation::instance();
  if (network_info == nullptr || network_info->reachability() != QNetworkInformation::Reachability::Online) {
    setRegistryUrlError(false);
    return;
  }

  if (network_ == nullptr) {
    network_ = new QNetworkAccessManager(this);
  }
  httpGetWithTimeout(
      *network_, QNetworkRequest(url), std::chrono::seconds(5), this, [this, url_text](QNetworkReply& reply) {
        // The user may have edited again while this probe was in flight; a
        // stale result must not repaint the newer text.
        if (ui_->lineEditRegistryUrl->text().trimmed() != url_text) {
          return;
        }
        setRegistryUrlError(reply.error() != QNetworkReply::NoError);
      });
}

void PreferencesDialog::setRegistryUrlError(bool error) {
  QLineEdit* edit = ui_->lineEditRegistryUrl;
  if (edit->property("urlError").toBool() == error) {
    return;
  }
  edit->setProperty("urlError", error);
  edit->style()->unpolish(edit);
  edit->style()->polish(edit);
}

}  // namespace PJ
